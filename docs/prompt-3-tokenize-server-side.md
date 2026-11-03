# 提示词 ③：TOKENIZE 服务端再降 30%

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）

## 0. 目标与价值

平台口径：`tokenize_ik_avg_ms` 基线 0.0720、当前 **0.0593**；`tokenize_beng` 0.0423 → **0.0333**。
tokenize 类占综合 1/3 权重（2 项均权）。

**目标**：`tokenize_ik` ≤ **0.049 ms**、`tokenize_beng` ≤ **0.033 ms**
⇒ tokenize 类均值从 +19.5% 提升到 +30% 以上（综合 +11 个百分点）。

## 1. 成本结构（实测，不要重复推导）

一次 `SELECT tokenize('<约 80 字中文>', 'ik')`：

| 组成 | 成本 | 可优化性 |
|---|---|---|
| **客户端往返 + 语句开销** | **24~27 µs** | **不可优化**（`SELECT 1` 实测；bench 是 3000 条 SQL 除总墙钟） |
| `parse_param`（parser 名 + 属性 JSON 解析） | 0.56 µs | 上一轮已从 4.66µs 优化到 0.56µs |
| `tokenize_fulltext` | **23.2 µs** | ← 主要目标 |
| `pack_json_res`（JSON 树 → 二进制 + 写 datum） | 1.9 µs | 已很小 |

`tokenize_fulltext` 内部已知：IK CJK 逐字 **≈10 µs**（实测 120 ns/char = 已存命中遍历 56 + 新匹配 64）、
词表插入 ≈2 µs、JSON 数组构建 ≈1 µs ⇒ **约 10 µs 未归属**。

## 2. 第 0 步（强制）：先把那 10 µs 拆开，再动代码

上一轮的教训：设计文档以为的"词表 7µs / JSON 3~5µs"实测只有 2µs 和 0.85µs，
而"重建解析器 + 三本词典"只有 1.1µs。**没有相位数据就不要改**。

在 `src/storage/fts/ob_fts_parser_helper.cpp` 的 `tokenize_fulltext` 路径上分四段计时
（`getenv("OB_FTS_PROF")` 门控，采样 1/64，用完删除）：

| 段 | 计时点 |
|---|---|
| `helper_init` | `ObFTParseHelper::init()` 前后（含属性解析、解析器与三本词典的获取） |
| `map_create` | `ObFTWordMap::create()` 前后 |
| `segment` | `helper.segment()` 前后（IK 逐字 + 词表插入） |
| `make_json` | `make_token_array_json()` 前后 |

跑 4000 次取稳态，产出四项均值。**如果四项之和仍比 23.2µs 少 8µs 以上**，
说明还有一段在别处（例如表达式求值框架、参数解析、内存分配），先去查那段，不要盲改。

## 3. 按第 0 步结论选一条实施（只做收益最大的 1~2 条）

### 3.1 若 `helper_init` ≥ 3µs：解析器/词典句柄复用
- 现状：每次 TOKENIZE 都 `ObFTParseHelper::init(&allocator, parser_name, properties)`，
  会按名字取解析器并解析属性。
- 做法（二选一，优先前者）：
  - **helper 级复用**：同一 `ObFTParseHelper` 实例内缓存上次的 `(parser_name, properties 内容哈希)`
    与已解析的属性结构；键不同才重解析。无全局状态、无跨租户风险。
  - **线程局部 LRU**：容量固定 8，键 `(parser_name, properties 哈希)`，值只含纯数据
    （**不得持有临时 allocator 的指针**）；容量满即整体清空，内存上界固定。
- 注意：属性可能因 `WITH PARSER ... PROPERTIES(...)` 不同而不同，键必须包含完整属性内容。

### 3.2 若 `segment` 仍 ≥ 12µs：继续压 CJK 热路径
- `hits_` 已改成定长数组（`src/storage/fts/ik/ob_ik_hit_array.h`，容量 32，溢出回退 `ObList`）。
- 剩余方向（按风险从低到高）：
  1. `ctx.add_token()` 里的 word 拷贝：同一次分词内多个 token 共享同一段源文本，
     可只存 `(offset, length)` 而不复制；
  2. 字母/数量词/代理对三个处理器的**空转调用合并**：现在每个字符都遍历处理器列表并各发一次虚调用，
     可按字符类型位掩码一次分派；
  3. `match_with_hit` 的调用次数：只在当前字符确实参与扩展时才调用。

### 3.3 若 `make_json` ≥ 3µs：直接写二进制 JSON
- 跳过 `ObJsonArray` / `ObJsonString` 中间树，按二进制 JSON 布局直接写 `["w1","w2",...]`；
- **必须**与现有 `pack_json_res`（`ObJsonWrapper::get_raw_binary`）的输出**逐字节一致**；
- 若发现布局有版本差异或对齐填充规则，放弃本节。

## 4. 硬约束（违反即失败）

1. **`TOKENIZE(...)` 返回字符串逐字节不变，含 token 顺序**。
   当前顺序是 `ObFTWordMap` **哈希桶的遍历顺序**，既非字典序也非文本序。
   证据：`tools/deploy/mysql_test/test_suite/fts_index/r/mysql/tokenize_edge.result` 里
   `tokenize('数据库 the 索引 a', 'ik')` → `["a", "索引", "数据库", "the"]`。
2. 分词语义不变：成词规则、停用词过滤、大小写折叠、中文数字、ngram、全角/日韩/emoji。
3. 不得减少处理的字符/token 数、不得跳过停用词检查或 LOB 读取。
4. 不改变 `OS_FTS_TOKENIZE` 的参数校验、错误码、签名。
5. **`build_*` 不得退化**（构建路径调用同一个 `segment()`）。

## 5. 验证（每一步都做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12   # 必须带 TMPDIR
df -h .                                                                    # 需 >10GB 可用
```

1. **`tokenize_edge`（14 组）必须 PASS**——它锁死了大小写/停用词/中文数字/全角/日韩/emoji/ngram
   的输出与顺序：`MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh tokenize_edge`
2. 36 形态 correctness dump 里**所有 TOKENIZE 输出逐字节一致**；
3. 4 个仓库用例全过（`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback`）；
4. 性能：同机两二进制交替、8 轮配对取中位
   `TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2`；
   判定门槛：`tokenize_ik` 配对中位 >1.5% 才算收益，**单步 <3% 就回退**，不要叠加；
5. `build_*` 用同样方法确认未退化。

## 6. 环境坑

1. `git checkout/stash` 失败（`.git` 只读），对比改动用 `cp` 备份再改回；
2. 不要 `pkill -f seekdb`（会杀沙箱包装进程）；停实例用 `sdb.py stop`；
3. 实例启动一律 `sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2881`；
4. 本机 `select1` 在 0.0204~0.0285 之间漂移（±15%），**禁止**用"单次跑 vs 固定基线"下结论；
5. `build_ik_all` 偶发 9~16s 离群（clog 提交拥塞，与代码无关），看 build 指标连跑 3 次取中位。

## 7. 交付要求

报告写清：
1. 第 0 步的四项相位均值（改动前），以及它们如何解释了 23.2µs；
2. 选了 §3 的哪一条、改了哪些文件、为什么；
3. **"输出顺序如何保持不变"的说明**（本任务最容易踩的坑）；
4. 每一步的 8 轮配对 A/B（中位 + stderr），被回退的尝试及原因；
5. 正确性证据：`tokenize_edge` 结果 + dump 差异行数；
6. `build_*` 是否退化；
7. 未完成部分与原因；本地数字与平台分数的区分。
