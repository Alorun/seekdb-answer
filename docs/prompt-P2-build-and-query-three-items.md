# 提示词 P2：构建侧 + 查询侧三项（D5/D6 build −6% + D7 query −6%）

> 自包含提示词。你可以与另一个 agent（分词侧 P1）**并行**工作，
> 因此**必须遵守 §0 的隔离协议**。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`，起点 HEAD `bbe2da610`）
>
> 本文件含**三个子任务**：D5（build 每行成本 −3%）、D6（build 再 −3%）、D7（query −6%）。
> D5/D6 都动 build，必须串行并各自独立验收；D7 与它们无文件交集，可在任意时机插入。

## 0. 并行隔离协议（先读）

1. 开工前把要改的文件 `cp` 到 `.tmpdev/p2/orig/`（`.git` 只读，恢复只能靠 `cp`）。
2. **A/B 做法**：构建 `bin-cand` → `cp` 恢复为开工前版本 → 构建 `bin-base` → `cp` 恢复改动。
3. **构建串行**（两人共用一个 `build_release`）：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
4. 实例用**自己的端口 2896**、目录 `.tmpdev/p2/inst`；不要用 2881~2895。
5. 不要 `pkill -f seekdb`；停实例用 `sdb.py stop`。磁盘需 >10GB。

## 1. 起点与目标

平台实测：

| 指标 | 基线 | 当前 |
|---|---|---|
| `build_ik_all_sec` | 6.52* | 4.791 |
| `build_ik_content_sec` | 5.13* | 4.082 |
| `build_beng_en_sec` | 2.80* | 2.075 |
| `query_cn / beng / mixed` | 1.3914 / 1.9911 / 1.5970 | 0.4331 / 0.5956 / 0.4735 |

\* 平台基线为反解值，逐次会话有 ±20% 漂移；**判断收益一律看绝对指标**。

**目标**：
- **D5**：build 三项各 **−3%**（→ 4.65 / 3.96 / 2.01）；
- **D6**：build 三项再 **−3%**（→ 4.51 / 3.84 / 1.95）；
- **D7**：`query_cn / beng / mixed` 各 **−6%**（→ 0.407 / 0.560 / 0.445；`query_limit` 已很高，可不动）。

本机对照（A2 落地后实测）：`2.659~2.707 / 2.179~2.217 / 1.203~1.218 s`。

### 必须理解的两条结论
1. **build 的对手是"每行成本"**：本机 `ik_content` 2.18 s = **固定地板 0.45 s** + **1.92 M 行 × 0.86 µs**；
2. **不要合并构建通路**：上一轮"两张分词表合并为一次扫描"实测**慢 7.1%**（牺牲两个扫描线程的并行度），
   代码已删除。也不要合并两张映射表（前提不成立：源表扫描只走主键不读 LOB；第 2/3 次 ALTER 复用映射表）。

---

## 2. D5：build 每行成本 −3%（先侧写）

### 2.1 第一步（强制）：拆开 0.86 µs/行
上一轮（C1）做过一次拆分，但候选改动只有 +2.5~4%、未过门被回退；**这次要带着已知结论往下拆**。
在以下位置加计时（`OB_FTS_PROF` 门控、1/64 采样、验完删除）：

| 段 | 位置 |
|---|---|
| 行 → store row 编码 | `src/storage/ddl/ob_tablet_slice_writer.cpp` 的 `append_row` → `ObMacroBlockWriter::append_row`（`src/storage/blocksstable/ob_macro_block_writer.cpp:684`）→ 微块编码 `ObMicroBlockWriter` |
| 微块追加 / 宏块 flush | 同上 |
| 排序比较 | DDL 内部 SQL 带 `ORDER BY`，排序在 PX 侧 sorter；在比较函数里累加计数与耗时 |
| 列校验和 | `report_ddl_column_checksum`（**不可跳过**，只统计占比） |

**C1 的已知线索**：排序比较约占每行成本的 40~50%（字符串键 `(word, docid)`，每行约 20 次比较）。
若本次复测确认，这是最大的一块。

### 2.2 按数据实施（选一项）
- **若排序比较 ≥40%**：把"比较排序"改成**分桶 + 桶内排序**——
  按 word 首字节（或首 2 字节）分桶，桶内再按 `(word, docid)` 排。
  ⚠️ 直载要求输出**仍然按目标表 PK 全局有序**，分桶必须保证桶间有序；
  ⚠️ 若 sorter 是通用算子（PX 的 `ObSortOp`），改它会影响所有 DDL/DML 排序 —— **优先在 FTS 的排序键上做文章**
  （例如把 word 预编码成定长前缀键），不要动通用 sorter。
- **若编码 ≥40%**：检查 `ObDatumRow` → store row 是否有可省的一次拷贝。
- **若校验和 ≥20%**：只能看能否批量化（确认现在是否已按批），**不得跳过**。

### 2.3 验收门
- build 三项各 **≥3%**（3 块交替取中位；`build_ik_all` 的 9~16 s 离群块作废重跑）；
- 索引内容等价：36 形态 dump 仅 `order_rel` 差异；**直载顺序正确**（建索引后 `MATCH` 结果集正确）；
- 5 个仓库用例 + DDL 14 项 + 命中数不变；单步 <3% 回退。

---

## 3. D6：build 再 −3%（收尾或第二次分桶）

### 3.1 两条候选（按 §2 的侧写数据选）
- **收尾合并**：`TAKE_EFFECT` 阶段（`enable_index` / `update_index_status_in_schema` / `release_ddl_kvs`）
  约 **20 ms/表 × 4 = 80 ms**；把 4 张表的 schema 状态更新合并为一次，
  或并行提交这 4 次收尾（注意错误传播）。**只动收尾，不动数据通路**（避开并行度陷阱）。
- **排序键再优化**：若 D5 已用分桶但仍有比较开销，评估对 `(word, docid)` 做**定长前缀键**
  （取 word 前 4 字节 + 长度 + docid 打包成 16 字节整数键），把字符串比较变成整数比较。

### 3.2 验收门
同 §2.3，且额外验证：建索引后**立即可查**、`DROP INDEX` 正常、online DDL 加列后索引仍可用。

---

## 4. D7：query 再 −6%

### 4.1 依据
上一轮 C3 把"每批 256 行"的消费侧开销改成了**批量消费接口**，拿到 `query_cn +16.5%`。
说明批次侧还有固定开销。当前 `query_cn` 0.4331 ms 的成本大致是：
12669 条 posting × 21 ns ≈ 266 µs（倒排扫描）+ 位图构建/emit 40~60 µs + 语句层约 37 µs + 其余。

### 4.2 做法（先量再改）
1. 在 `src/storage/retrieval/ob_text_retrieval_token_iter.cpp`（上轮的批量消费接口所在）继续量：
   每次批消费的固定开销、每次 `get_next_batch` 的调用次数与耗时；
2. 候选：
   - **减少批边界的记账**（若每批有可省的容器清理/重置）；
   - **位图路径的批产出**（`ObTextDocIdBitmapIter::emit_rows`）是否还有可外提的操作；
   - **倒排扫描的列裁剪**：确认只读 docid 列（若还读了别的列，裁掉）。
3. ⚠️ 批次 256 → 1024 已被 `eval_ctx->max_batch_size_` 体系**硬约束**，上一轮已确认放弃，不要重试。

### 4.3 验收门
- `query_cn / beng / mixed` 各 **≥2%**（8 轮配对；归一化到 `select1`），目标 6%；
- 命中数不变；5 个仓库用例 + 36 形态 dump（仅 `order_rel`）；
- 单步 <1.5% 回退（查询噪声地板）。

---

## 5. 正确性五关（每步都过）

1. 36 形态 dump（仅 `order_rel` 允许差异）；
2. 5 个仓库用例：`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`；
3. DDL 专项 14 项（`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`）；
4. 命中数 `8001 / 11000 / 7332 / 20`；
5. **`SELECT id` 的映射正确性**（D5/D6 若动了映射表相关路径必须单独验证）。

## 6. 测量纪律

1. build 用 **3 块交替取中位**（`TMPDIR=/tmp bash .tmpdev/ab_bins.sh <base> <cand> 3`）；
   `build_ik_all` 偶发 9~16 s 离群（clog 提交拥塞，与代码无关）⇒ 该块作废重跑；
2. query 用 **8 轮配对**；注意本机 `ab_pingpong` 近两轮已被证明**不可靠**
   （A2 记录：同一改动两次测得 −4.93% / −6.20%，与官方块模式 +10.18% 矛盾），
   建议**单实例交替 ≥15 样本/臂** + `select1` 归一化；
3. 平台是唯一裁判；本地只用于判断方向。

## 7. 交付要求

报告按 D5/D6/D7 分节：
1. 每行的 0.86 µs 拆成几段、各占多少（D5 必须先侧写）；
2. 选了什么、改了哪些文件、为什么；
3. 3 块交替 A/B 的 build 数据（中位 + 每块原始值 + 标注离群块）；
4. query 的 8 轮配对数据 + `select1` 归一化；
5. 正确性五关证据（尤其直载顺序、`SELECT id` 映射）；
6. 未完成部分与原因；本地数字与平台分数的区分（平台 build ≈ 本机 1.8 倍）。
