# 提示词 C2：分词侧算法级改造（T-1 IK 逐字 + T-2 beng ASCII 快路径）

> 自包含提示词。你可以与另外两个 agent **并行**工作（构建侧 / 查询侧），
> 因此**必须遵守 §0 的测量隔离协议**，否则你的 A/B 会被别人的改动污染（上一轮已发生）。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
>
> 本文件两个子任务是**同一子系统**（TOKENIZE 输出路径），必须**串行**做：
> 先 T-1 后 T-2，每步独立验收。

## 0. 并行工作协议（先读，否则测量无效）

1. **开工前**把你要改的每个文件复制到 `.tmpdev/c2/orig/`（保持相对路径）。
   `git checkout` 不可用（`.git` 只读），恢复只能靠 `cp`。
2. **A/B 正确做法**：构建 `bin-cand` → `cp` 恢复你的改动为开工前版本 → 构建 `bin-base` →
   `cp` 恢复你的改动。两个二进制只差你的改动，其他人并行的改动对两臂是常数，不影响归因。
3. **构建串行**：所有 `cmake --build` 必须加文件锁：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
   （`TMPDIR` 必须指定，否则 Rust 组件编译失败。）
4. **实例隔离**：用自己的端口 `--port 2892`、目录 `.tmpdev/c2/inst`，不要用 2881。
5. **不要 `pkill -f seekdb`**；停实例用 `sdb.py stop --base-dir <dir>`。
6. 磁盘需 >10GB 可用，否则实例报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`。

## 1. 当前基线（本机，8 次中位；平台与本地基本同速）

```
tokenize_ik_avg_ms    0.0597   （基线 0.0720，+17.0%；平台实测 0.0529）
tokenize_beng_avg_ms  0.0439   （基线 0.0423，−3.8%；平台实测 0.0356）
```

**目标**：`tokenize_ik` ≤ **0.050 ms**、`tokenize_beng` ≤ **0.035 ms**
⇒ tokenize 类别从 +21% 提到 **+34%** 以上。

### 关键约束（先理解，避免做无用功）
- `tokenize` 的测量是 **3000 条独立 SQL 除总墙钟**，因此**客户端往返 + 语句开销约 25 µs 不可优化**
  （`select1` 实测）；
- `tokenize_beng` 基线 42.3 µs 中 25 µs 是往返 ⇒ **它最多只能做到 +41%**（服务端归零）；
- 已测相位（勿重复做）：`parse_param` 0.56 µs、`tokenize_fulltext` **17.5 µs**
  （helper 0.2 + map 0.1 + **segment 16.5** + json 0.7）、`pack_json_res` 1.9 µs；
- 已完成的优化：`parse_param` 缓存、字符分类查表、停用词 fast path、`hits_` 定长数组
  （`ob_ik_hit_array.h`）、JSON 二进制直写（`ob_json_bin.cpp`）、协议层序列化调整。
- **上一轮已证否**：CJK trie 查找"复用"实测 −5.23%（更慢）→ 不要再走那条路。

---

## 2. T-1：IK CJK 逐字 120 ns/char → 50~60 ns/char（算法级替换）

### 2.1 依据（实测）
`segment` = 16.5 µs / 80 字符 ≈ 206 ns/char，其中 CJK 处理器实测：
- **已存命中遍历 56 ns/char**：`ob_ik_cjk_processor.cpp` 里对每个 pending hit 调
  `dict_main_.match_with_hit(...)`；
- **新匹配起点 64 ns/char**：从当前字符起 `dict_main_.match(...)`。

两者都在走**双数组 trie（DATrie）**：每次推进都要读 `base_[]`/`check_[]` 两个数组并做随机访存，
缓存不友好。这是 120 ns/char 的主因。

### 2.2 替换方案（推荐 1，风险从低到高）
1. **首字索引 + 最大匹配**（推荐）：
   - 预建 `first_char → 候选词表`（按词长降序；中文词长集中在 2~4 字）；
   - 对每个起点，按长度从大到小做 `memcmp` 试匹配，命中即产出 token 并推进；
   - 完全绕开 DATrie 的 base/check 跳转，比较成本变成"2~3 次 memcmp + 长度判断"。
   - 词典构建：在 `ObFTRangeDict`/`ObFTDATrieDict` 初始化时额外建一张 `first_char` 哈希索引
     （复用现有词典数据，不改词表内容）。
2. **DATrie 单数组化 + 位掩码预判**：把 `base/check` 合并为单数组，并先用一个 256 项位掩码
   判断"该字符是否可能是任何词的首字符"，不是则整段跳过。
3. **向量化字符分类**：一次处理 16 字节判断 ASCII/中文/USELESS，把分类成本并入扫描。

### 2.3 硬约束（违反即失败）
- **`TOKENIZE(...)` 输出逐字节不变，含 token 顺序**；
  当前顺序是 `ObFTWordMap` **哈希桶遍历顺序**（证据：
  `tools/deploy/mysql_test/test_suite/fts_index/r/mysql/tokenize_edge.result` 里
  `tokenize('数据库 the 索引 a','ik')` → `["a","索引","数据库","the"]`）；
- 分词语义不变：成词规则、最大/最细粒度、停用词、中文数字、全角、日韩、emoji；
- **必须提供灰度开关**（如 `_enable_ik_first_char_index`，默认关时走旧实现），
  便于出问题一键回退；
- `build_*` 不得退化（构建走同一个 `segment()`）。

### 2.4 验收门
- `tokenize_ik` ≤ **0.053 ms**（8 轮配对中位）、且 `build_*` 不退化；
- `tokenize_edge`（14 组）+ 36 形态 dump 的 TOKENIZE 输出**逐字节一致**；
- 开关开/关两种模式的输出一致；
- 单步 <3% 就回退。

---

## 3. T-2：beng 的 ASCII 快路径

### 3.1 依据
`tokenize_beng` 平台 0.0356 ms，服务端约 **10.6 µs** 处理 115 个 ASCII 字符（约 92 ns/char）。
beng 现在是通用路径（UTF8 解码 + 分类 + 词缓冲 + 词表），对纯 ASCII 有明显浪费。

### 3.2 做法
- 当整段文本是纯 ASCII（先做一次快速的 SIMD/逐字节判断）时走**快路径**：
  查表分类（字母/数字/分隔符，256 项表）+ 直接切分，不查词典、不做 UTF8 解码；
- 快路径产出的 `(offset, length)` 进入与通用路径**同一个** `ObFTWordMap` 累加逻辑，
  保证 hash 桶序与现状一致；
- 非纯 ASCII 或含任何高位字节 → 走原路径（不要试图分块混走，避免顺序风险）。

### 3.3 硬约束
同 T-1 的 1~4 条（输出逐字节不变、语义不变、灰度开关、build 不退化）。

### 3.4 验收门
- `tokenize_beng` ≤ **0.032 ms**（8 轮配对中位）；
- `tokenize_edge` 里 beng 相关用例（大小写、全角、emoji、混合）逐字节一致；
- 单步 <3% 就回退。

---

## 4. 正确性四关（每步都过）

1. 36 形态 correctness dump，唯一允许差异 `order_rel`；
2. 5 个仓库用例：`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`；
3. DDL 专项 14 项（`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`）；
4. benchmark 命中数 `8001 / 11000 / 7332 / 20`。

## 5. 测量纪律
- **8 轮配对取中位**（`TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <bin-base> <bin-cand> 8 2`）；
- 本机 `select1` 在 0.0204~0.0358 之间漂移，**禁止**用单次跑 vs 固定基线；
- `tokenize_beng` 的样本波动可达 ±20%（0.0307~0.0542），**至少 8 个样本**再下结论。

## 6. 交付要求
报告写清：
1. T-1 的算法替换细节（首字索引怎么建、内存代价、最大匹配如何保证与旧实现等价）；
2. **"输出顺序如何保持不变"的说明**（本任务最容易踩的坑）；
3. 每项的 8 轮配对数据（中位 + stderr/MAD）；
4. 灰度开关开/关两种模式的一致性证据；
5. 正确性证据（`tokenize_edge`、dump 差异行数、5 个用例、命中数）；
6. `build_*` 是否退化；
7. 未完成部分与原因；本地数字与平台分数的区分。
