# 提示词 B：分词侧两项优化（③ 深挖 TOKENIZE 内部 + ⑧ 语句层）

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
>
> 本文件含**两个子任务**，必须先做 A（服务端相位深挖）再做 B（语句层），
> 每步有独立验收门，不达标就回退并进入下一步。

## 0. 当前基线（实测，3 次中位，`select1`=0.0224~0.0254）

```
tokenize_ik_avg_ms    0.0572   （基线 0.0720，+20.6%）
tokenize_beng_avg_ms  0.0384   （基线 0.0423，+9.2%）
```

**目标**：`tokenize_ik` ≤ **0.050 ms**、`tokenize_beng` ≤ **0.034 ms**
（合计约 +11 个百分点到 tokenize 类别均值）。

成本结构（实测）：
- 客户端往返 + 语句开销 = `select1` ≈ **0.022~0.025 ms**（**不可优化**，bench 是 3000 条 SQL 除总墙钟）；
- 服务端 ≈ `0.0572 - 0.023` ≈ **34 µs**；
- 已测三相位只有 **25.7 µs**（`parse_param` 0.56 + `tokenize_fulltext` 23.2 + `pack_json_res` 1.9）；
- ⇒ **两处未归属**：`tokenize_fulltext` 内部约 9 µs（子任务 A）、语句层约 8 µs（子任务 B）。

已完成的优化（不要重复做）：
- `parse_param` 缓存（4.66 → 0.56 µs）；
- 字符分类查表化、停用词 fast path、IK `hits_` 定长数组（`ob_ik_hit_array.h`）；
- **JSON 二进制直写已落地**（`src/oblib/common/json_type/ob_json_bin.cpp` 新增约 54 行）
  与结果序列化路径调整（`src/query/protocol/ob_mysql_protocol_util.cpp`）
  —— 这两处就是最近一轮 `tokenize_ik` 0.0599 → 0.0572 的来源，**先确认它们是否还有余量**。

---

# 子任务 A：③ 深挖 `tokenize_fulltext` 内部的 ~9 µs

## A1. 强制第一步：四段相位计时

在 `tokenize_fulltext` 路径上分四段计时（`getenv("OB_FTS_PROF")` 门控、采样 1/64、用完删除）：

| 段 | 计时点 | 已知参考值 |
|---|---|---|
| `helper_init` | `ObFTParseHelper::init()` 前后（属性解析、解析器与三本词典获取） | ~1.1 µs |
| `map_create` | `ObFTWordMap::create()` 前后 | 未测 |
| `segment` | `helper.segment()` 前后（IK 逐字 + `ctx.add_token` 词表插入） | ~12 µs（CJK 120 ns/char × 80 ≈ 10 µs + 词表 ~2 µs） |
| `make_json` | `make_token_array_json()` 前后（已改为二进制直写，需重测） | 未测（XML 树版本约 1 µs） |

相关文件：
- `src/sql/engine/expr/ob_expr_tokenize.cpp`（`ObExprTokenize::parse_param` / `tokenize_fulltext` / `pack_json_res`）
- `src/storage/fts/ob_fts_parser_helper.cpp`（`ObFTParseHelper::init` / `segment` / `make_token_array_json`）
- `src/storage/fts/ik/*`（IK 逐字解析）
- `src/oblib/common/json_type/ob_json_bin.cpp`（二进制直写）

跑 4000 次取稳态。**四项之和与 23.2 µs 的差额，就是下一层要查的目标。**

## A2. 按数据选一条实施（只做收益最大的一项）

- **若 `helper_init` ≥3 µs**：解析器/属性缓存
  —— 优先"同一 `ObFTParseHelper` 实例内复用上次的 `(parser_name, properties 哈希)` 与已解析结构"；
  线程局部 LRU 只能用固定容量 8、值不得持有临时 allocator 指针；
- **若 `segment` ≥12 µs**：
  1. `ctx.add_token()` 的 word 拷贝 —— 同一次分词内多个 token 共享同一段源文本，
     可只存 `(offset, length)` 而不复制；
  2. 字母/数量词/代理对三个处理器的**空转调用合并** —— 现在每个字符都遍历处理器列表并各发一次虚调用，
     可按字符类型位掩码一次分派；
  3. 减少 `match_with_hit` 的调用次数（只在当前字符确实参与扩展时调用）；
- **若 `make_json` 仍 ≥2 µs**：检查二进制直写路径是否还有中间拷贝（`ObJsonBin` 的 buffer 是否可以
  直接交给 `pack_json_res`，省掉一次 memcpy）。

## A3. 硬约束（违反即失败）

1. **`TOKENIZE(...)` 返回字符串逐字节不变，含 token 顺序**；
   当前顺序是 `ObFTWordMap` **哈希桶遍历顺序**（证据：
   `tools/deploy/mysql_test/test_suite/fts_index/r/mysql/tokenize_edge.result` 里
   `tokenize('数据库 the 索引 a','ik')` → `["a","索引","数据库","the"]`）；
2. 分词语义不变：成词规则、停用词过滤、大小写折叠、中文数字、ngram、全角/日韩/emoji；
3. 不得减少处理的字符/token 数、不得跳过停用词检查；
4. **`build_*` 不得退化**（构建路径调用同一个 `segment()`）。

## A4. 验收门
- `tokenize_ik` ≤ **0.053 ms**（8 轮配对中位）、`tokenize_beng` 不退化；
- `tokenize_edge`（14 组）PASS + 36 形态 dump 的 TOKENIZE 输出逐字节一致；
- `build_*` 用同样方法确认未退化；单步 <3% 就回退。

---

# 子任务 B：⑧ 语句层的 ~8 µs

## B1. 定位方法（先量再改）

同一实例、同一份 SQL 循环，四个对照相减：

| 对照 | 目的 | 已知值 |
|---|---|---|
| `SELECT 1` | 纯往返 + 最小语句开销 | 0.022~0.025 ms |
| `SELECT tokenize('', 'ik')` | 分词开销≈0，剩下语句层 + JSON 输出 | 未测 |
| `SELECT repeat('x', 300)` | 与 tokenize 输出体量相近的字符串 ⇒ 隔离结果序列化 | 未测 |
| `SELECT tokenize('<80 字>', 'ik')` | 完整路径 | 0.0572 ms |

把四者相减，定位 8 µs 落在：
（a）结果序列化 / 协议写回；（b）表达式求值框架；（c）`ObEvalCtx::TempAllocGuard` 首次分配；
（d）`TokenizeParam` 的重复构造或字符串参数解析。

## B2. 按定位结果实施
- **若在结果序列化/协议**：`src/query/protocol/ob_mysql_protocol_util.cpp` 已有一轮调整，
  继续看是否有一次多余拷贝（例如先写到临时 buffer 再拷进协议包）；
- **若在 `TempAllocGuard`/分配**：改为同一表达式实例内复用一块临时内存（注意生命周期与并发安全）；
- **若在框架**：检查每次调用是否重复构造了可复用的对象（`TokenizeParam`、属性字符串等）。

## B3. 硬约束
同 A3 的 1~4 条；另外**不得改变协议层对其它类型结果的序列化行为**
（`ob_mysql_protocol_util.cpp` 是通用代码，改动必须只影响 tokenize/字符串结果路径，或零影响）。

## B4. 验收门
- `tokenize_ik` 再降 **≥5%**（8 轮配对中位）；
- `tokenize_edge` + dump 输出逐字节一致；
- 通用协议路径无回归：跑一次完整的 `bench_run`，确认 query 全部指标不退化（8 轮配对）。

---

# 全局纪律（每步都遵守）

1. **构建**：`TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12`
   （不带 TMPDIR 时 Rust 组件会因沙箱只读临时目录而失败）；
2. **磁盘**：实例启动要求可用 >10GB，低于阈值报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`；
3. **不要 `pkill -f seekdb`**（会杀掉沙箱包装进程）；停实例用 `sdb.py stop --base-dir <dir>`；
4. **不要 `git checkout/stash`**（`.git` 只读），对比改动用 `cp` 备份；
5. **测量**：同机两二进制交替、8 轮配对取中位
   `TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2`；
   本机 `select1` 在 0.0204~0.0358 之间漂移，**禁止**用单次跑 vs 固定基线判定；
6. **正确性四关**：36 形态 dump（仅 `order_rel` 允许差异）、5 个仓库用例
   （`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`）、
   DDL 专项 14 项、命中数 8001/11000/7332/20；
7. **单步收益 <3% 就回退**，不要叠加复杂度。

# 交付要求

报告按子任务 A/B 分节：
1. 四段相位（以及四个对照）的实测均值，以及它们如何解释服务端 34 µs；
2. 选了哪一条、改了哪些文件、为什么；
3. **"输出顺序如何保持不变"的说明**（本任务最容易踩的坑）；
4. 每步的 8 轮配对数据（中位 + stderr）；
5. 正确性证据（`tokenize_edge`、dump 差异行数、5 个用例、命中数）；
6. `build_*` 与 `query_*` 是否退化；
7. 未完成部分与原因；本地数字与平台分数的区分。
