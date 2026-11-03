# 提示词：FTS 查询子系统重写（目标：查询类指标再快 1.5~2.3 倍）

> 交给执行 agent 的**自包含**提示词。请完整读完再动手。
> 背景数据全部来自实测（2026-11-03，本机 + 官方平台），不需要你重新推导。

## 0. 你要达成的目标

官方平台当前实测（唯一权威口径）：

| 指标 | 平台基线 | 当前 | 当前提升 |
|---|---|---|---|
| `query_cn_avg_ms` | 1.3914 | 0.9957 | +28.4% |
| `query_beng_avg_ms` | 1.9911 | 1.5707 | +21.1% |
| `query_mixed_avg_ms` | 1.5970 | 1.1766 | +26.3% |
| **`query_limit_avg_ms`** | 1.7526 | **1.9936** | **−13.8%（唯一负项）** |

平台综合 `fts_mean_improvement = 21.51%`、得分 43.01。查询类占 4/9 权重。

**目标**：
1. **先把 `query_limit` 从 −13.8% 变成 ≥ +20%**（这一项单独值 +5.6 分）；
2. 再把 4 个查询指标整体提高到 +40% 以上（`query_cn` 需从 0.9957 降到 ~0.83ms，
   `query_limit` 需从 1.9936 降到 ~1.05ms）。

**不要动**：执行计划语义、MATCH 语义、命中集合、命中数（这是正确性硬约束）。

## 1. 已测到的关键事实（不要重复推导）

### 1.1 四个查询的形状（`tools/benchmark/fts_large_bench.sh`）
```sql
query_cn:    SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('<中文多词>');
query_beng:  SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('<英文多词>');   -- beng 解析器
query_mixed: 同上，混合文本
query_limit: SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('<中文多词>') LIMIT 20) t;
```
命中数必须恒为 `8001 / 11000 / 7332 / 20`。

### 1.2 当前执行路径
- 4 个查询都走 **DAAT**（`ObSRDaaTIterImpl`），不走 TAAT；
- `query_cn/beng/mixed` 因为"分数只作谓词"已被优化为**无分数路径**（`calc_relevance=false`，
  不生成 BM25 表达式与 relevance 投影）——这部分已经做完，是它们 +21~28% 的来源；
- `query_limit` 因为带 `LIMIT`，被 gate 显式排除（`ob_log_plan.cpp` 里的 `!select_stmt->has_limit()`），
  走的是**有分数路径**（`calc_relevance=true`），与基线代码路径一致。

### 1.3 `query_limit` 的异常（这是首要线索）
同一份代码、同一张 20k 文档表、同样 3 个 token 的查询，只改 LIMIT 值：

| 查询 | 耗时 |
|---|---|
| `SELECT id ... LIMIT 1` | 0.535 ms |
| `SELECT id ... LIMIT 5` | 0.511 ms |
| `SELECT id ... LIMIT 20` | **2.189 ms** |
| `SELECT id ... LIMIT 100` | 2.633 ms |
| `SELECT id ... LIMIT 1000` | 6.335 ms |
| `SELECT id ... LIMIT 5000` | 15.141 ms |
| `SELECT id ... LIMIT 20000`（≈全量 8001 行） | 19.297 ms |
| `SELECT COUNT(*) ...`（无 LIMIT） | 1.360 ms |

两个关键异常：
1. **`LIMIT 20` 比"全量 COUNT"还慢 60%**（2.19ms vs 1.36ms）；
2. `LIMIT 5` → `LIMIT 20` 之间从 0.51ms 跳到 2.19ms（15 行多花 1.68ms）。

`EXPLAIN` 显示 `LIMIT 5` 与 `LIMIT 20` 的**计划完全相同**（单个 `TEXT RETRIEVAL SCAN`，
`rowset=256`，`is_index_back=true`，`calc_relevance=true`），所以差异在运行时。
`is_index_back=true` 说明每回一行要做一次主表回查（`SELECT id` 需要 id 列），
实测约 2.4~2.9 µs/行（`LIMIT 5000` 15.1ms / 5000 行）。

**首要任务**：查清 0.51→2.19ms 这个台阶到底是什么（每次产出多少行、每批多少开销、
索引回查发生多少次）。可用手段：
- 在 `ObTableScanOp` / `ObDASScanIter` / `ObSRDaaTIterImpl` 里加临时计时（`OB_FTS_PROF` 环境变量门控，
  用完必须删除）；
- 对比 `LIMIT 6/8/10/12/16/20` 找出台阶精确位置（若是 16/32 之类的批次边界，就是 batch 效应）；
- 对比 `SELECT id ... LIMIT 20` 与 `SELECT COUNT(*) ... LIMIT 20`（去掉 id 投影 → 无回表）看回表占比。

### 1.4 DAAT 每 posting 的成本结构（本地实测）
`query_cn`：8001 命中行、3 个维度、12669 条 posting，总 1.02ms ⇒ **79 ns/posting**。
已测得：存储扫描 21 ns/posting、`save_docids` 4 ns/posting、`project_results` 5 ns/行。
剩下约 45~50 ns/posting 分布在：`fill_merge_heap`（每行 1~3 次堆 push + rebuild）、
`collect_dims_by_id`（堆 pop + `ObSRDaaTRelevanceCollector` 虚调用）、
`merge_cmp_.cmp`（**函数指针**的 datum 比较，每次约 5~10ns）、`cache_result`（`ObDocIdExt` 40 字节拷贝）。

**已证伪的两条路**（别再走）：
- 把归并堆换成"手工 k 路线性归并"：已实现并验证结果等价，8 轮配对 A/B 为
  +1.47% / −4.04% / −2.86% / +2.66%，类别中位 **−0.69%** ⇒ **堆不是瓶颈**。
  原因推测：替代实现用了 `merge_cmp_.cmp` 函数指针做 O(k²) 次比较，比堆的 O(k log k) 还贵。
- 去掉 `filter_on_demand` 的每行 `BatchInfoScopeGuard`：8 轮 A/B 类别中位 **+0.14%** ⇒ 零收益。

### 1.5 无分数路径已经实现了什么（不要重复做）
- `need_calc_relevance=false` 时不建 BM25 表达式、不建 relevance 投影列；
- gate 条件：自然语言模式 + WHERE 只有一个 MATCH 条件 + 选择列表无列引用 +
  无 `ORDER BY` / `DISTINCT` / `GROUP BY` / `HAVING` / 窗口函数 / `LIMIT`；
- `get_curr_score()` 在无分数时只写 `0.0`（BM25 已不计算）。

## 2. 硬约束（违反即失败）

1. **4 个查询的命中数必须恒为 8001 / 11000 / 7332 / 20**；
2. MATCH/BM25 语义不变；有分数查询（如 `ORDER BY MATCH(...) DESC`、`MATCH(...) > 0.5`）的分数值不变；
3. 不得跳过 LOB 读取、不得减少扫描的 posting 行、不得改 benchmark 数据/SQL/计时/评分/基线；
4. 不得为了 LIMIT 快而返回不完整结果集（`COUNT(*) FROM (... LIMIT 20)` 必须等于 20）；
5. 保守优先：宁可少改，也不要让某个形态出现"快但结果不对"。

## 3. 建议的执行顺序

### 第 1 步：定位 `query_limit` 的台阶（只测量，不改逻辑）
1. 用 `LIMIT 6/8/10/12/16/20/24/32` 扫描出台阶；
2. 在 DAAT 里按批打印 `count`（本批产出行数）、每批的 `get_next_rows` 耗时、每批的 index-back 次数；
3. 在 `ObTableScanOp` 的 index-back 路径上计时；
4. 得出结论后写一句话总结：台阶是"批次效应"还是"回表次数"还是"每行固定开销"。

### 第 2 步：按结论做最小改动
可能的三个方向（按优先级）：
- **若台阶在批次边界**：让 DAAT 在 `LIMIT` 存在时按 `min(limit_remaining, batch)` 产出，
  而不是每次都填满 256 行；`cache_result` 已经支持 `limit_param_` 提前返回 `OB_ITER_END`，
  检查 `ObTableScanOp` 侧是否把 `limit_param_` 正确下发（`iter_param_->limit_param_`）。
- **若瓶颈是 index-back**：检查 `SELECT id` 是否真的需要回表——
  `determine_docid_type()` 对带显式主键的表返回 `TABLET_SEQUENCE`，因此需要 `rowkey_doc` 映射，
  回表不可避免；但可以**批量**回表（现在疑似逐行），把 256 行的回表合并成一次多行查询。
- **若瓶颈是每行固定开销**：只做**低风险**的专用化（例如 `docid` 为整型时用 `uint64_t` 数组 +
  内联比较替代 `merge_cmp_.cmp` 函数指针），并且必须用 8 轮配对 A/B 判定，收益 <3% 就回退。

### 第 3 步：整体提速（可选，若第 2 步已达标可跳过）
把 `query_cn/beng/mixed` 从 ~79 ns/posting 降到 ~55 ns/posting。优先级：
1. `merge_cmp_.cmp` 的整型快速路径（无分数 + 无 filter + docid 为整型时）；
2. `cache_result` 里 `ObDocIdExt`（40 字节）→ 整型数组（8 字节）的专用缓冲；
3. 只有在 1、2 都验证有收益后，才考虑重排归并结构。

## 4. 验证方法（每一步都必须做）

```bash
# 构建（必须指定 TMPDIR，否则沙箱会挡住 Rust 临时文件）
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12

# 磁盘要求：实例启动需要 >10GB 可用，低于阈值会报 OB_SERVER_OUTOF_DISK_SPACE(-4184)
df -h .
```

**正确性（三条都要）**：
1. 36 形态 correctness dump（`.tmpdev/correctness_dump.sh`）与改动前逐字节一致，
   唯一允许差异是 `order_rel`（`ORDER BY MATCH(...) DESC, id ASC LIMIT n` 的并列组选取，
   属既有行为）；
2. 仓库用例：`fts_index.no_score_match`、`fts_index.tokenize_edge` 必须 PASS
   （`MODE=verify bash .tmpdev/gen_result.sh <name>`）；
3. benchmark 命中数 8001/11000/7332/20。

**性能（唯一可信口径）**：同机两二进制交替、8 轮配对取中位。
```bash
TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2
```
判定：类别中位 >1.5% 才算有收益（本机查询噪声地板 ~1.5%）；<3% 的单项收益不值得引入复杂度。
**不要**用"单次跑 vs 固定基线"下结论（本机 `select1` 在 0.0204~0.0274 之间漂移，±15%）。

## 5. 交付要求

报告里必须写清：
1. `query_limit` 台阶的**根因**（一句话 + 证据）；
2. 改了哪些文件、每处改动的作用；
3. 每一项的 8 轮配对 A/B 数据（中位 + stderr），被回退的尝试及原因；
4. 正确性证据（dump 差异行数、用例结果、命中数）；
5. 未完成的部分及原因；
6. 明确说明：哪些数字是在本地测的、哪些来自官方平台，不要把本地数字当成平台分数。
```
