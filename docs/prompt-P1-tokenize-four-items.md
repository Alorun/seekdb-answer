# 提示词 P1：分词侧四项（D1 beng 快路径 + D2/D3/D4 压 IK 服务端）

> 自包含提示词。你可以与另一个 agent（构建+查询侧，P2）**并行**工作，
> 因此**必须遵守 §0 的隔离协议**，否则你的测量会被污染。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`，起点 HEAD `bbe2da610` + A2 的工作区改动）
>
> 本文件含**四个子任务，按 D1 → D3 → D2 → D4 顺序串行执行**（D3 比 D2 便宜且风险低）。
> 每步独立验收，不达标就回退并进入下一步。

## 0. 并行隔离协议（先读，否则测量无效）

1. 开工前把要改的文件 `cp` 到 `.tmpdev/p1/orig/`（`.git` 只读，恢复只能靠 `cp`）。
2. **A/B 做法**：构建 `bin-cand` → `cp` 恢复为开工前版本 → 构建 `bin-base` → `cp` 恢复改动。
3. **构建串行**（两人共用一个 `build_release`）：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
   （`TMPDIR` 必须指定，否则 Rust 组件编译失败。）
4. 实例用**自己的端口 2895**、目录 `.tmpdev/p1/inst`；不要用 2881~2894。
5. 不要 `pkill -f seekdb`；停实例用 `sdb.py stop`。磁盘需 >10GB。

## 1. 起点与目标

平台实测（官方 3000×3 口径）：

| 指标 | 基线 | 当前（A2 前） | A2 后预计 |
|---|---|---|---|
| `tokenize_ik_avg_ms` | 0.0720 | 0.0473 | **0.0454~0.0426** |
| `tokenize_beng_avg_ms` | 0.0423 | 0.0332 | 0.0332（A2 只改 IK，beng 未变） |

**目标**：
- `tokenize_beng` ≤ **0.0297 ms**（+29.8%）
- `tokenize_ik` ≤ **0.0395 ms**（+45.1%~）
⇒ tokenize 类别从 +29.2% 提到 **+37~40%**，三类之和 +13.5 中的 8.4 由本文件贡献。

### 成本上限（务必理解）
`tokenize` 官方测量 = 3000 条独立 SQL 除总墙钟，**客户端往返 + 语句开销约 25 µs 不可优化**
（`select1` 实测 0.025）。因此：
- `tokenize_ik` 0.0454 → 服务端 **20.4 µs**，要压到 **14.5 µs**；
- `tokenize_beng` 0.0332 → 服务端 **8.2 µs**，要压到 **4.6 µs**。

### 上一轮（A2）已确认的成本结构（IK，16.6 µs/篇）
| 块 | µs | 占比 |
|---|---|---|
| `ObAddWord::process_word`（词表插入链） | **5.6** | 34% |
| ├ `groupby_word`（`ObFTWordMap` 插入，95 ns/token） | 3.9 | |
| ├ `casedown_word` | 1.25 | |
| └ `check_stopword` | 0.57 | |
| 每字符循环 charloop | 4.6 | 28% |
| ├ 其中字典跳转 `dict.child/root_child/is_word`（24 ns/字符） | 2.2 | |
| └ 其余（分类、letter/quantifier/surrogate、ctx 记账） | 2.4 | |
| `arbitrator_.process` | 3.4 | 20% |
| `output_result` + `make_token_array_json` | 1.4 | 8% |
| 其他 | 1.5 | 9% |

A2 已消除的：**解析器每 ~56 篇重建（13~14 µs 平摊）** —— 已改成文档级 arena，**不要重复做**。

---

## 2. D1（最大收益，先做）：beng 的 ASCII 快路径

### 2.1 依据
- `tokenize_beng` 服务端 **8.2 µs**，处理 **115 个纯 ASCII 字符 / 约 20 个 token** ⇒ **71 ns/char**；
- 对照：IK 的 CJK 逐字已压到 24 ns 字典跳转 + ~30 ns 其他；
- beng 现在走通用路径（UTF8 解码 + 字符分类 + 词缓冲 + 词表插入），对纯 ASCII 明显浪费；
- 已排除隐藏成本：`helper.init` 仅 0.07 µs、`parse_param` 0.56 µs（A2 实测）。
- **上一轮（C2）试过但失败**：只拿到 −0.65%（噪声内）。失败原因是**没有先做侧写**。本轮不许重犯。

### 2.2 第一步（强制）：先侧写 beng 的 8.2 µs
在 `src/storage/fts/ob_beng_ft_parser.cpp` 的分词路径上分段计时
（`getenv("OB_FTS_PROF")` 门控、采样 1/64、验完删除），至少拆出：

| 段 | 内容 |
|---|---|
| 字符循环 | 逐字符 UTF8 解码 + 分类（字母/数字/分隔符/无用） |
| 词缓冲 | 累积当前 word 的字节、长度、字符数 |
| 词表插入 | `ctx.add_token()` → `ObAddWord::process_word`（含 `groupby_word`/`casedown`/`check_stopword`） |
| 输出/JSON 之外的其他 | 记账、`TokenizeContext` 维护 |

跑 4000 次取稳态。**若三段之和明显小于 8.2 µs，说明还有一段没找到，先找出来再改。**

### 2.3 按数据实施
- **若字符循环 ≥4 µs**：加**纯 ASCII 快路径**——
  先做一次整段快速判定（逐字节检查最高位，或 16 字节 SIMD 批量判定），
  纯 ASCII 时用 **256 项查表**分类 + 直接切分，不做 UTF8 解码、不查词典；
  非纯 ASCII（含任何高位字节）**整段回落**原路径（不要分块混走，避免顺序风险）。
- **若词表插入 ≥4 µs**：见 D2（同一个靶点，先做 D2 再回来做 D1 的这条）。
- **若词缓冲 ≥2 µs**：减少每字符的长度/字符数累加，改为"只在分隔符处结算"。

### 2.4 硬约束
输出**逐字节不变、含 token 顺序**（顺序 = `ObFTWordMap` 桶下标 + 桶内头插链表 ⇒
**不得改桶数 / 哈希函数 / `add_token` 调用序列**）。必须提供灰度开关（如 `OB_FTS_BENG_ASCII_FAST=0` 回退）。

### 2.5 验收门
- `tokenize_beng` ≤ **0.0297 ms**（官方 3000×3），至少 ≥8% 才保留；
- `tokenize_edge` 里 beng 相关用例（大小写、全角、emoji、日韩、混合、超长重复字符）逐字节一致；
- 745 行 TOKENIZE 压测（`.tmpdev/tri/c2/stress_tok.sql`）diff=0；
- 单步 <3% 就回退。

---

## 3. D3（便宜、先做）：IK 的 `arbitrator` 3.4 µs + charloop 非字典部分 2.4 µs

### 3.1 依据
`arbitrator_.process` 占 20%（3.4 µs），charloop 里除字典跳转外的部分占 2.4 µs。两者合计 5.8 µs（35%）。

### 3.2 做法（先看再改）
1. 读 `src/storage/fts/ik/ob_ik_arbitrator.cpp`：`process` 每次调用做了什么？
   - 是否每个字符都遍历全部子分词器（CJK / letter / quantifier / surrogate）并各做一次虚调用？
     若是 → 按字符类型**一次分派**（用 `CharType` 直接选一个分支），省掉空转调用；
   - 是否有可省的临时容器构造/清空（`ObList`、`ObSEArray`）→ 改为复用成员；
2. charloop 非字典部分：`ob_ik_ft_parser.cpp` 里每字符的分类 + letter/quantifier/surrogate 处理 +
   `ctx` 记账。确认 `CharType` 分类是否已查表（A2 报表说已查表），
   剩余成本是否在**每字符写 `ctx` 的游标/长度**——可合并为段落式（遇到 token 边界才写）。

### 3.3 验收门
- `tokenize_ik` 提升 **≥2%**（官方口径）；
- 输出逐字节不变（含顺序）；`build_*` 不退化；单步 <2% 回退。

---

## 4. D2：IK 的 `process_word` 词表插入链 5.6 µs（最大单块）

### 4.1 依据
`ObAddWord::process_word`（`src/storage/fts/ob_fts_stop_word.cpp`）= 5.6 µs/篇：
- `groupby_word`（`ObFTWordMap` 插入）**3.9 µs = 95 ns/token × 41**；
- `casedown_word` 1.25 µs（30 ns/token）；
- `check_stopword` 0.57 µs（14 ns/token）。

### 4.2 做法（按收益排序）
1. **`ObFTWordMap` 插入 95 ns/token 太高**（正常 hash 插入约 20~30 ns）。先确认：
   - 表是否每文档重建、桶数是否按 `len/2` 预分配（不足会触发扩容 + rehash）；
   - 是否有节点分配（`ObList`/`ObHashMap` 的 node 从 allocator 拿）→ 改为**开放寻址 + 内联节点**；
   - key 的哈希是否对短中文字符串做了全字节哈希（Szudzik/FNV）→ 若有更快的（如首字节+长度+末字节）可换。
   ⚠️ **桶数与哈希函数是输出顺序的组成部分：任何改动都必须保持"同一 token 集合 ⇒ 同一遍历顺序"**。
   最安全的做法是：**保持桶数与哈希不变，只把桶内存储从链表改成内联数组**（顺序语义不变时需逐字节验证）。
2. **`casedown_word` 1.25 µs**：若词内无大写（纯 CJK/小写），跳过转换（已有 `needs_case_conversion` 快路径，
   确认它是否对 CJK 生效）。
3. **`check_stopword` 0.57 µs**：停用词表的查找是否可加"长度/首字节"预筛。

### 4.3 验收门
- `tokenize_ik` 提升 **≥3%**；
- 输出逐字节不变（**必须用 745 行压测 + 36 形态 dump + `tokenize_edge` 三重验证**）；
- `build_*` 不退化（构建也走 `process_word`）；单步 <3% 回退。

---

## 5. D4：IK 再深一步（吃 D2/D3 的剩余余量）

### 5.1 做法（按 D1~D3 之后的实测数据选，预期服务端 16.0 → 14.5 µs）
- **转移缓存**：`dict.child(node, ch)` 的二分查找加 direct-mapped 缓存（8K~16K 项，纯缓存语义透明）；
- **`is_word` 预取**：确认 `WORD_FLAG` 与 children offset 在同一字里（已合并），
  若还有二次加载则在 `child()` 返回时一并返回 word 位；
- **记账合并**：把 per-token 的 `doc_len`/`char_cnt` 累加改为批量结算。

### 5.2 验收门
- `tokenize_ik` 提升 **≥2%**；输出逐字节不变；`build_*` 不退化。

---

## 6. 正确性五关（每步都过）

1. **36 形态 dump**：唯一允许差异 `order_rel`；
2. **745 行 TOKENIZE 压测**：`.tmpdev/tri/c2/stress_tok.sql`（370 文本 × 2 输出模式 + 15 个 beng 用例），**diff 必须为 0**；
3. **`tokenize_edge`（14 组）**：`MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh tokenize_edge`；
4. **5 个仓库用例**：`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`；
5. **DDL 专项 14 项** + **命中数 8001/11000/7332/20**。

## 7. 测量纪律（本轮特别重要）

1. **`ab_pingpong` 在本机已不可用**：A2 记录同一改动两次测得 −4.93% 与 −6.20%（MAD 6.9~9.2%），
   与官方块模式 +10.18% 矛盾。**改用单实例交替 ≥15 样本/臂**，并用 `select1` 与未改指标做臂间偏差校正
   （A2 的干净做法：同一实例内交替跑 base/cand 各 15 次，取中位）。
2. 官方口径必须跑 `ROUNDS=3000 / SAMPLES=3`（`tools/benchmark/fts_large_bench.sh`），
   不要用 9000 轮的绝对值（客户端启动开销被稀释，数值系统性偏低）。
3. 每步只动一个方向；每步都给出 `select1` 与未改指标的对照值。

## 8. 交付要求

报告按 D1/D3/D2/D4 分节：
1. **每块实测 µs 与占比**（D1 必须是先侧写后实施）；
2. 选了什么、改了哪些文件、为什么；
3. **"输出顺序如何保持不变"的说明**；
4. 每步的官方口径数值 + 单实例 15 样本配对（中位 + MAD + `select1` 对照）；
5. 正确性五关证据；
6. `build_*` 是否退化；
7. 未完成部分与原因；本地数字与平台分数的区分。
