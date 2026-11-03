# FTS 查询子系统重写报告（query_limit 台阶根因 + 3 项查询优化）

> 执行依据：`docs/fts-query-subsystem-rewrite-prompt.md`。数据截至 2026-11-03。
> **口径说明**：本文件里除明确标注"平台"的数字外，全部是**本机** 8 轮配对 A/B 结果，
> 不等价于官方平台分数；平台换算见 §6。

## 1. 一句话结论

`query_limit` 的 0.51ms→2.19ms 台阶（本机表现为"LIMIT 1..64 恒定 ~1.9ms"）**不是批次效应、也不是回表**，
而是"带 LIMIT 的 FTS 查询会被强制走 Block-Max WAND（BMW）top-k 下推"：BMW 的每个 pivot 成本
是普通 DAA T 候选的 3~6 倍，而这批查询词太常见、块最大分无法剪枝（剪枝率 ~0.8%），于是全程白付
block-max 的固定开销。改成"BMW 剪枝无效即回退成 DAA T + top-k 堆"后，`query_limit` 由 −13.8% 转为
**+21.2%**；同时把"只要命中集合、不要分数"的形态改成 **doc id 位图去重**，三个 COUNT 类查询提升
**+48.7% / +57.3% / +51.5%**。

本机 8 轮配对 A/B（A = HEAD 检索代码，B = 本次改动；越小越好）：

| 指标 | A 中位 | B 中位 | 配对中位 | spread | n |
|---|---|---|---|---|---|
| `query_cn` | 1.0051 ms | **0.5126 ms** | **+48.67%** | 0.56% | 8 |
| `query_beng` | 1.6148 ms | **0.6847 ms** | **+57.34%** | 0.62% | 8 |
| `query_mixed` | 1.2014 ms | **0.5960 ms** | **+51.51%** | 1.55% | 8 |
| `query_limit` | 2.0514 ms | **1.6078 ms** | **+21.19%** | 1.26% | 8 |
| `tokenize_ik`（未改代码，噪声参照） | 0.0669 ms | 0.0658 ms | −0.83% | 2.83% | 8 |
| `tokenize_beng`（同上） | 0.0394 ms | 0.0410 ms | −6.17% | 8.72% | 8 |

> 查询类配对中位 **+50.09%**；命中数 A/B 完全一致（8001 / 11000 / 7332 / 20）。

## 2. `query_limit` 台阶的根因（一句话 + 证据）

**一句话**：单表 FTS 查询会被 `preserve_order_for_fulltext_search` 加上 `ORDER BY MATCH(...) DESC`；
一旦带 `LIMIT`，优化器就把 limit 下推进 FTS scan（`has_pushdown_topk()=true`），执行层因此选择
**BMW（Block-Max WAND）+ 每 token 一条 block-max 摘要扫描**，而不是普通 DAA T；BMW 的每次 pivot
要付 block-max 定位 + pivot 记账 + 归并轮次（实测 ~0.7~1.1 µs/候选，DAA T 仅 ~0.2 µs/候选），
而这些查询词的高频特性使块最大分始终高于 top-k 阈值、剪枝几乎不生效（21 次 skip / 2617 个 pivot），
所以 BMW 只剩开销。**回表只占 20 行 ≈ 50 µs，不是主因；批次大小也不是。**

证据（临时 `OB_FTS_PROF` 探针，已删除；20k 文档、3 token、`LIMIT 20`）：

```
merge_iter init: topk_mode_=1 daat_mode_=1 topk_limit_=20 tokens=3
                 need_block_max_scan=true need_calc_relevance=true has_pushdown_topk=true
merge_iter do_table_scan: children_cnt_=6 elapsed_us=182~289
BMW top_k_search: pivot_cnt=2617 eval_pivot_cnt=2596 skip_range_cnt=21
                  scored_candidates=2616 heap_count=20 total_us=1905~2926
```

- `pivot_cnt=2617` 说明 BMW 只给 2617/8001 个候选打了分（这一点是 BMW 唯一的价值），
  但 `skip_range_cnt=21` 说明**块级剪枝只成功了 21 次**，也就是"跳过候选"几乎全靠 pivot 机制本身；
- 把同一条查询的 top-k 换成"纯 DAA T 遍历 + 同一个 top-k 堆"（同样给 8001 个候选打分），
  `count_wrap_limit20` 从 2.24 ms 降到 1.72 ms —— **说明 BMW 在该分布下比朴素遍历更慢**；
- `LIMIT 1..64` 的耗时基本持平（1.6~1.9 ms），与"每次查询都要建成套 block-max 结构 + 全量 pivot"
  一致，这也是 prompt 里 0.51→2.19 ms 台阶在本机的同源表现（本机没有 0.5 ms 那一档，见 §6）。

## 3. 改动清单

| 编号 | 改动 | 文件 | 作用 |
|---|---|---|---|
| Q1 | **BMW 剪枝无效即回退 DAA T top-k** | `src/storage/retrieval/ob_sparse_bmw_iter.{h,cpp}` | `query_limit`：省掉无效的 pivot 记账；`finish_top_k_with_daat_scan()` 从当前（全部处于真实 posting 位置的）迭代器继续走 DAA T，喂同一个 top-k 堆 |
| Q2 | **无分数形态改用 doc id 位图去重** | `src/storage/retrieval/ob_text_daat_iter.{h,cpp}`（新增 `ObTextDocIdBitmapIter`）、`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.{h,cpp}`（`is_doc_id_bitmap_applicable()`） | `query_cn/beng/mixed`：把 k 路归并换成"按 doc id 置位 + 顺序扫描位图"，删除每 posting 的堆 push/pop、比较、relevance 收集 |
| Q3 | **归并比较快路径 + 无分数跳过取分** | `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}` | 定长二进制 doc id 用 `memcmp` 比较（替代通用 collation 比较函数指针）；整型 doc id 内联比较；`relevance_expr_ == nullptr` 时不再调用 `get_curr_score`（覆盖所有剩余的 DAA T 路径，包括有分数查询的 merge 部分） |

### 3.1 Q1：BMW 自适应回退（`query_limit`）

- 触发条件：每 `BMW_FALLBACK_PIVOT_CNT = 32` 个 pivot 检查一次，若"跳过区间数 / pivot 数 < 1/2"
  （`skip_range_cnt * 2 <= pivot_cnt`）就认为块最大分剪枝不划算，回退；
- 为什么可以中途切：回退点固定在 `EVALUATE_PIVOT` 成功之后，此刻 `evaluate_pivot()` 已经
  `unify_dim_iters_for_next_round()`，所有 dim iter 都在真实 posting 上、`next_round_cnt_ == 0`，
  与 DAA T 每轮开始的状态等价；已跳过的区间是被块最大分**证明**不可能进入 top-k 的区间，跳过仍然正确；
- tie 语义与原先一致（`process_collected_row` 的 `<=` 判定未变），只是并列组的选取可能不同
  （见 §5）。
- 有分数查询的分数值不受影响（BMW 与 DAA T 用的是同一个 BM25 表达式与同一个堆）。

### 3.2 Q2：doc id 位图去重（`query_cn/beng/mixed`）

无分数形态（`calc_relevance=false`、无 LIMIT/ORDER BY/DISTINCT/GROUP BY/窗口、选择列无列引用）
只需要"命中集合"，而 k 路归并的唯一作用是**按 doc id 去重**。于是：

1. 逐 dim 顺序读完 posting，把 doc id 的 `seq_id` 当作位下标置位（每 posting 1 次虚拟调用 + 1 次置位，
   没有堆、没有比较、没有 relevance 收集）；
2. 再顺序扫描位图输出行（`__builtin_ctzll` 逐 64-bit 字），行的 doc id 由"首个 posting 的
   tablet_id + 位下标"重建；
3. 兜底：doc id 超过位图上限（64M 文档/8MB）或同一流里 tablet_id 变化时，改存到旁路列表，
   结束时排序去重，结果仍然精确。

启用条件是"可证明顺序无关"的一组判据（`is_doc_id_bitmap_applicable()`）：
非 function-lookup、无 boolean 计算节点、`NATURAL_LANGUAGE_MODE`、`!need_calc_relevance()`、
`!need_proj_relevance_score()`、无 `match_filter_`、无 topk 下推、无 limit/offset、dim 数 ≥ 2、
doc id 是定长二进制串（`ObDocId`，16 字节）。

`EXPLAIN` 佐证该形态的消费者与顺序无关：

```
0 - output([T_FUN_COUNT(*)]), group(nil), agg_func([T_FUN_COUNT(*)])
1 - output(nil), access(nil)
    is_index_back=false, calc_relevance=false, match_expr(MATCH(...))
```

即：无回表、scan 不输出任何列、上层只有 COUNT。

### 3.3 Q3：比较快路径 / 跳过取分

- FTS 的 doc id 是 `ObDocId{tablet_id, seq_id}` 的 16 字节 varbinary，原先每次归并都比较
  `ObCharset::strcmpsp` 的通用 collation 路径；二进制 collation 下等价于等长 `memcmp`，改为直接 `memcmp`；
- 整型 domain id（无 rowkey-doc 映射的索引）改为内联 `uint64` 比较；
- 无分数时 `fill_merge_heap()` 不再调用 `get_curr_score()`（每 posting 省一次虚调用）。

## 4. 性能数据（本机）

### 4.1 8 轮配对 A/B（同机两实例交替，`ab_pingpong.sh`）

见 §1 表格。命令：

```bash
TMPDIR=/tmp QUERY_ROUNDS=500 TOK_ROUNDS=3000 \
  bash .tmpdev/ab_pingpong.sh <HEAD 二进制> <改动后二进制> 8 2
# artifacts: .tmpdev/pp-1103-155626/（A=HEAD 检索代码，B=改动后，同一 tokenizer 状态）
```

`tokenize_*` 两列代码未改，其 −0.8% / −6.2% 即本轮噪声地板（spread 2.8% / 8.7%），
说明查询类 +48~57% 远超噪声。

### 4.2 LIMIT 曲线（本机，单次 60 轮取均值）

| 形态 | 改动前 | 改动后 |
|---|---|---|
| `SELECT COUNT(*) ... MATCH(content)`（= query 类 COUNT） | 1.42~1.50 ms | **0.68 ms** |
| `SELECT COUNT(*) FROM (SELECT id ... LIMIT 20) t`（= query_limit） | 2.24 ms | **1.63~1.77 ms** |
| `SELECT id ... LIMIT 20` | 2.08 ms | 1.72~1.85 ms |
| `SELECT id ... LIMIT 1000` | 6.55 ms | 5.56 ms |
| `SELECT id ... LIMIT 20000`（全量 8001 行，回表主导） | 22.8 ms | 22.3 ms |

## 5. 正确性证据

1. **36 形态 correctness dump**（`q_equiv.sh`，A=HEAD、B=改动后，各 244 行）：
   **除 `order_rel` 外逐字节一致**（diff 仅 20 行，全部落在 `### order_rel` 段）。
   `order_rel` 的差异已核实是**并列组选取**：两组 id 的分数完全相同
   （`1.6259307170781316`），且该分数共有 **1001** 个文档并列，属 prompt 允许的既有差异。
2. **仓库用例**：`fts_index.no_score_match`（12 形态）**PASS**；`fts_index.tokenize_edge`（14 组）**PASS**。
3. **benchmark 命中数**：`8001 / 11000 / 7332 / 20`，与基线一致（A/B 两臂均校验）。
4. **集合级校验**：dump 里的 `cn_2terms_set` / `cn_common_set` / `en_set` / `mixed_set`
   （`COUNT, SUM(id), MIN(id), MAX(id)`）与 HEAD 完全一致 —— 即位图路径返回的文档集合逐个相同。

## 6. 本地数字与平台数字的关系（务必区分）

- §1/§4 是**本机**（24 核、NVMe、客户端往返 ~0.15 ms）8 轮配对结果，不是平台分数；
- 平台当前（`docs/fts-delivery-index.md` §1）：`query_cn 0.9957 / query_beng 1.5707 /
  query_mixed 1.1766 / query_limit 1.9936 ms`，查询类 +15.5%；
- 本机 HEAD 的绝对值和平台不同（例如本机 `query_cn` 1.0051 ms vs 平台 0.9957 ms，
  `query_beng` 1.6148 vs 1.5707，`query_mixed` 1.2014 vs 1.1766，`query_limit` 2.0514 vs 1.9936），
  两者量级接近、比例可参考；
- 若按本机配对比例线性外推：`query_cn ≈ 0.51 / 0.9957 → +49%`、`query_beng ≈ +57%`、
  `query_mixed ≈ +51%`、`query_limit ≈ +21%`；查询类从 +15.5% 提升到 **~+45%** 量级，
  按 `docs/fts-optimization-report.md` 的边际分数模型，`query_limit` 一项即由 −13.8% 转正
  （约 +5.6 分），其余三项再各加 20~30 个百分点。
- **以平台实测为准**，本地数字只用于判定"是否有收益、是否退化"。

## 7. 未完成 / 可继续的方向

1. **BMW 的 block-max 扫描仍然会被建起来**：回退发生在执行期，此时 3 条 block-max 扫描参数已就绪
   （`do_table_scan` 实测 182~289 µs）。若在优化器/ctdef 侧给出"剪枝大概率无效"的静态判据
   （例如 top-k 远小于估计命中数 + 词都是高频词），可以连这部分也省掉，`query_limit` 预计再快 5~10%。
2. **有分数路径仍给全部 8001 个候选打分**（回退之后）。要再进一步，需要改 BMW 的 per-pivot 成本
   （例如 pivot 之间复用 block-max 结果、或对"全维都命中"的稠密集合直接走 DAA T）。
3. 位图路径目前只覆盖"无分数"形态；`SELECT 1 FROM ... LIMIT n` 这类有分数形态不适用（需要 top-k 排序）。
4. 有分数查询的 `ObTextBMWIter` 仍保留原逻辑：本改动只加了"剪枝无效则回退"的判据，
   没有改 BM25/表达式求值，也没有改分数值。

## 8. 复现命令

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12   # 构建

# A/B（同机两实例交替，8 轮配对取中位）
TMPDIR=/tmp QUERY_ROUNDS=500 TOK_ROUNDS=3000 \
  bash .tmpdev/ab_pingpong.sh <A二进制> <B二进制> 8 2

# 正确性：36 形态 dump（A/B 两份数据各自灌库后逐字节 diff）
TMPDIR=$PWD/.tmpdev/tmp bash .tmpdev/q_equiv.sh <A二进制> <B二进制>

# 正确性：仓库用例
MODE=verify bash .tmpdev/q_gen_result.sh no_score_match   <二进制>
MODE=verify bash .tmpdev/q_gen_result.sh tokenize_edge    <二进制>

# 查询延迟曲线 + 命中数
TMPDIR=$PWD/.tmpdev/tmp bash .tmpdev/limit_curve.sh <二进制> 2890
```
