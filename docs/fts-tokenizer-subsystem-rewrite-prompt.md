# 提示词：TOKENIZE 分词子系统重写（目标：服务端再快 2~3 倍）

> 交给执行 agent 的**自包含**提示词。请完整读完再动手。
> 背景数据全部来自实测（2026-11-03，本机 + 官方平台）。

## 0. 目标

官方平台当前实测（唯一权威口径）：

| 指标 | 平台基线 | 当前 | 当前提升 |
|---|---|---|---|
| `tokenize_ik_avg_ms` | 0.0720 | 0.0607 | +15.7% |
| `tokenize_beng_avg_ms` | 0.0423 | 0.0386 | +8.8% |

平台综合 `fts_mean_improvement = 21.51%`、得分 43.01。分词类占 2/9 权重。

**目标**：`tokenize_ik` 降到 **0.045ms**、`tokenize_beng` 降到 **0.030ms**（约 +35% / +29%），
即服务端从 ~35µs 压到 ~20µs；若能到 ~11µs，则每项可到 +50%。

## 1. 成本结构（实测，别重复推导）

一次 `SELECT tokenize('<80 字中文>', 'ik')`（bench 用 `IK_TEXT`，约 80 个汉字）：

| 组成 | 成本 | 说明 |
|---|---|---|
| **客户端往返 + 语句开销** | **24~27 µs** | `SELECT 1` 实测；**不可优化**，是硬地板 |
| `parse_param`（parser 名 + 属性 JSON 解析） | **0.56 µs** | 上一轮已从 4.66µs 优化到 0.56µs（缓存） |
| `tokenize_fulltext` | **23.2 µs** | 见下拆分 |
| `pack_json_res`（JSON 树 → 二进制 + 写 datum） | **1.9 µs** | 已很小 |

`tokenize_fulltext` 内部（80 字中文）：

| 子项 | 成本 |
|---|---|
| IK CJK 处理器逐字 | **~10 µs**（实测 **120 ns/char** = 已存命中遍历 56 ns + 新匹配起点 64 ns） |
| 词表插入（`add_token` → `ObFTWordMap`，约 48 token） | ~2 µs |
| JSON 数组构建（`make_token_array_json`） | ~1 µs |
| helper init（建解析器、三本词典）、map create、其余三个处理器（letter/quantifier/surrogate） | ~10 µs |

注意：`tokenize` 的测量方式是 **3000 条 SQL 走 mysql CLI，总墙钟 / 3000**，
所以客户端往返（~25µs）**必然计入**。结论：即使服务端降到 0，该项的上限也只有约 +64%。
要 +50% 必须把服务端从 35µs 压到 ~11µs。

## 2. 硬约束（违反即失败）

1. **`TOKENIZE(...)` 的返回字符串必须逐字节不变，包括 token 顺序**。
   ⚠️ 当前输出顺序是 `ObFTWordMap` **哈希桶的遍历顺序**，既不是字典序也不是文本出现顺序。
   证据（`tools/deploy/mysql_test/test_suite/fts_index/r/mysql/tokenize_edge.result`）：
   `tokenize('数据库 the 索引 a', 'ik')` → `["a", "索引", "数据库", "the"]`。
   任何替换词表结构的方案都必须复现同一顺序，否则 `tokenize_edge` 会失败。
2. 分词语义不变：成词规则、停用词过滤、大小写折叠、中文数字、ngram、全角/日韩/emoji 处理。
3. 不得减少处理的字符/token 数、不得跳过停用词检查或 LOB 读取。
4. 不改变 `OS_FTS_TOKENIZE` 的参数校验、错误码、签名。
5. `build_*` 指标不得退化（构建路径调用同一个 `segment()`）。

## 3. 已完成的优化（不要重复做）

上一轮（见 `docs/fts-tokenizer-rewrite-report.md`）已经落地并验证：
- `parse_param` 结果缓存：-87.9%（4.66µs → 0.56µs）；
- 字符分类查表化 / 单次解码（`ob_ik_char_util.h`）；
- 停用词 fast path（`ob_fts_stop_word.cpp`）；
- IK 处理器与 arbitrator 的若干内联化；
- 服务端三相位合计从 33.4µs 降到 25.7µs（-23.1%）。

## 4. 建议的执行顺序（按"收益/风险"排序）

### 4.1 `hits_` 链表 → 定长数组（预计 -2~3 µs）
`src/storage/fts/ik/ob_ik_cjk_processor.h` 的 `ObList<ObDATrieHit, ObIAllocator> hits_`
与 `ob_ik_quantifier_processor.h` 的 `count_hits_`：每个字符都要 `push_back`/`erase`，
**每次 push 都从 allocator 分配一个链表节点**。

改法：定长数组（容量 32）+ 计数，保持插入顺序语义；`erase` 用"标记 + 压实"。
⚠️ 两个坑：
- `ObDATrieHit` **没有默认构造函数**（只有 `(dict, start_pos)`），数组化需要新增一个默认构造；
- `ObDATrieHit::operator=` **不拷贝 `dict_`**（`match_with_hit` 依赖它），数组搬运时必须显式拷贝 `dict_`。
- 容量溢出：若活跃命中数超过容量，必须回退到现有 `ObList` 路径（不要静默丢 token）。

### 4.2 词表桶内结构（预计 -1~2 µs）
`ObFTWordMap` 每 token 一次哈希插入 + `ObString` key 拷贝。可把桶内链表换成定长数组
（每桶 8，溢出回退链表），**桶数与哈希函数保持不变**，遍历顺序自然不变。

### 4.3 JSON 直写二进制（预计 -1~2 µs）
跳过 `ObJsonArray`/`ObJsonString` 语法树，直接按二进制 JSON 布局写 `["w1","w2",...]`，
必须与当前 `get_raw_binary` 的输出**逐字节一致**。若发现布局有版本/对齐差异，放弃本节。

### 4.4 其余处理器与 helper init（预计 -2~4 µs）
- `ObFTParseHelper::init` 每次调用重建解析器与三本词典（实测约 1.1µs）：可做"按
  `(parser_name, properties hash)` 的线程局部缓存"，容量固定 8，不持有临时 allocator 指针；
- `ObIKLetterProcessor` / `ObIKQuantifierProcessor` / `ObIKSurrogateProcessor` 的空转调用：
  当前每个字符都遍历 `ObList<ObIIKProcessor*>` 并各发一次虚调用，可改成按字符类型的位掩码一次分派。

### 4.5 只在前面都做完后再考虑
CJK 的字典遍历（56ns/char）本身：只有在有把握把 `match_with_hit` 的调用次数降下来时才动，
否则容易变成"改了但没收益"。

## 5. 验证方法（每一步都必须做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
# 构建（必须指定 TMPDIR，否则 Rust 的临时文件会被沙箱拒绝）
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12
# 磁盘：实例启动需要 >10GB 可用，低于阈值报 OB_SERVER_OUTOF_DISK_SPACE(-4184)
df -h .
```

**正确性（两种都要）**：
```bash
# 1) 仓库用例（14 组输出逐字节锁定，覆盖大小写/停用词/中文数字/全角/日韩/emoji/ngram）
MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh tokenize_edge
# 2) 36 形态 correctness dump 里所有 TOKENIZE 输出逐字节一致
TMPDIR=/tmp bash .tmpdev/correctness_dump.sh <实例目录> > /tmp/tok_new.txt && diff -u <旧 dump> /tmp/tok_new.txt
```

**性能（唯一可信口径）**：同机两二进制交替、8 轮配对取中位。
```bash
TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2
```
判定：`tokenize_ik` 的配对中位 >1.5% 才算有收益（本机噪声地板约 1.5~3%）；
单步 <3% 就回退，不要叠加复杂度。

## 6. 交付要求

报告里必须写清：
1. 每一步改了什么、为什么（对应 §4 哪一项）；
2. 每一步的 8 轮配对 A/B（中位 + stderr），被回退的尝试及原因；
3. **"输出顺序如何保持不变"的说明**（这是本任务最容易踩的坑）；
4. 正确性证据：`tokenize_edge` 结果、dump 差异行数；
5. `build_*` 是否退化（用同样方法 A/B）；
6. 未完成部分及原因；本地数字与平台分数的区分。
```
