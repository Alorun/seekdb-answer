# 提示词 ① 实施报告：让 LIMIT 在 FTS 扫描里真正早停

> 依据：`docs/prompt-1-fts-limit-early-stop.md`。数据截至 2026-11-03，均为**本机**测量，
> 不等价于官方平台分数（见 §6 末）。
> 分支 `indextuning`，未提交改动之外的既有工作区改动（`ob_text_daat_iter.*`、docs、psmalltest）不是本项引入。

## 1. 一句话结论

`query_limit` 的慢**不是**"DAAT 批次填满 256 行"造成的：带 LIMIT 的单表 MATCH 会被
`preserve_order_for_fulltext_search()` 注入 `ORDER BY MATCH(...) DESC`，优化器随即把 LIMIT
变成**下推 top-k**（`has_pushdown_topk()=true`，`topk_limit_expr_=20`），执行层走
Block-Max WAND / top-k 堆，必须扫完候选集才能给出"精确 top-20"，因此耗时与 LIMIT 值无关。

本次改动把"外层只能看到行数"的形态（`COUNT(*) FROM (SELECT ... LIMIT n) x`）从
"精确 top-n"改回"无序 + LIMIT 下推"，并让检索侧按剩余预算早停：

| 指标 | 改动前 | 改动后 | 变化 |
|---|---|---|---|
| `query_limit`（8 轮配对 A/B，中位） | 1.4899 ms | **0.1789 ms** | **+88.02%**（spread 0.28%，n=8） |
| `query_limit`（单项 300 次/sample） | 1.55~1.60 ms | **0.23~0.25 ms** | ≈ −85%（6.5×） |
| benchmark `query_limit_hits` | 20 | 20 | 不变 |
| `SELECT id ... LIMIT 1/20`（单表顶层） | 1.51 / 1.58 ms | 1.51 / 1.58 ms | 不变（仍是精确 top-n） |

`query_limit_avg_ms` 目标 0.35 ms：本机已达 0.18~0.25 ms。平台口径见 §6。

## 2. 探针结论（prompt §2 要求的证据）

### 2.1 探针方法

1. 临时在 `ObSRDaaTIterImpl::get_next_rows`（`src/storage/retrieval/ob_sparse_daat_iter.cpp`）
   与 `ObDASLegacyTRMergeIter::is_doc_id_bitmap_applicable` 打点（`LOG_INFO`，交付前已删除）；
2. 用 gdb 以父进程方式启动 seekdb，在
   `ObDASLegacyTRMergeIter::do_table_scan` / `ObSRDaaTIterImpl::get_next_rows` /
   `ObSRBMWIterImpl::get_next_rows` / `ObTextDocIdBitmapIter::get_next_rows` /
   `ObDASIterUtils::create_text_retrieval_sub_tree` 上下断点（脚本 `.tmpdev/gdb_trace.sh`）。

gdb 命中统计（20k 文档、`ik`、2 词查询）：

```
3 HIT_CREATE_TR_SUBTREE
3 HIT_LEGACY_DO_TABLE_SCAN
2 HIT_BMW_GET_NEXT_ROWS      <- 带 LIMIT 的 SELECT id
2 HIT_DAAT_GET_NEXT_ROWS
2 HIT_BITMAP_GET_NEXT_ROWS   <- COUNT(*)（无 LIMIT）
```

即：**带 LIMIT 的形态走的是 BMW（top-k）路径，不是 DAAT 批量填充**。原先假设的
"`get_next_rows(capacity=256)` 要填满 256 行"在 `SELECT id ... LIMIT n` 上根本不成立
（`ObSRDaaTIterImpl::get_next_rows` 的探针一次都没触发）。

### 2.2 LIMIT 有没有下发？

`EXPLAIN SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20`：

```
2 - ... calc_relevance=true, match_expr(MATCH(...)),
      pushdown_match_filter(MATCH(...)),
      sort_keys([MATCH(...), DESC]), limit(20), offset(nil), with_ties(false)
```

- LIMIT **已经**下发，但以 **top-k** 形式（`ObDASIRScanCtDef::topk_limit_expr_`），不是"行预算"；
- 依据：`ObTransformPreProcess::preserve_order_for_fulltext_search()` 给单表 MATCH 查询注入
  `ORDER BY MATCH(...) DESC`；`ObLogPlan::try_push_topn_into_text_retrieval_scan()` 据此设置
  `topk_limit_expr_`/`sort_key_`；
- 执行层：`ObDASIterUtils::create_text_retrieval_sub_tree()` 里
  `has_pushdown_topk() -> topk_mode_=1, daat_mode_=1`，随后 `ObSRBMWIterImpl::top_k_search()`
  扫描候选、维护 top-k 堆（本轮之前的 BMW 自适应回退已在基线里）。要给出**精确** top-20，
  就必须遍历候选集，因此 `LIMIT 1/5/10/16/20/32/64` 同价。

### 2.3 为什么不能用 prompt 的方案 A/B 原样修

- 方案 B（`real_capacity = MIN(capacity, limit_)`）只对 DAAT 批次有效；这条形态根本不走
  `ObSRDaaTIterImpl::get_next_rows` 的批量填充（见 §2.1），改它没有收益；
- 方案 A（把 LIMIT 下推进 text retrieval scan）方向正确，但当前链路上 **LIMIT 已经是
  "top-k 已下发"**；真正的阻塞点是"注入的 ORDER BY 让扫描必须按分数排序"。而
  顶层 `SELECT id ... LIMIT n` 的**行集合就是 top-n by score**（本机验证：
  `LIMIT 20` 的 20 行与 `ORDER BY MATCH(...) DESC LIMIT 20` 完全一致，与 `ORDER BY id`
  不同），在"行集合不得改变"的硬约束下无法把它改成无序早停。

因此本次按**同一条安全前提**（prompt §3 方案 A 的"无 ORDER BY 分数 / 无 DISTINCT/GROUP BY /
无分数过滤"）把改动作在**外层只观测行数**的形态上，其余形态原样保留 top-k。

## 3. 改动清单

| # | 文件 | 改动 | 作用 |
|---|---|---|---|
| 1 | `src/sql/rewrite/ob_transform_pre_process.{h,cpp}` | `preserve_order_for_fulltext_search()` 增加 `parent_stmts` 入参；新增 `is_fulltext_order_unobservable()`：父查询块是**单表源 + 单个 COUNT 聚合**（无 WHERE/GROUP BY/HAVING/ORDER BY/DISTINCT/窗口，聚合为 `COUNT` 且无 DISTINCT/非常量参数）且子查询带 LIMIT 时，**不注入**相关性 ORDER BY | 让优化器不再为该子查询选 top-k 下推，LIMIT 可作为行预算下发 |
| 2 | `src/sql/optimizer/ob_log_plan.cpp` | `prepare_text_retrieval_info()` 的"无分数"门控放宽：不再排除"带 LIMIT"和"选择列表引用列"；仍要求无 ORDER BY/GROUP BY/ROLLUP/DISTINCT/HAVING/窗口、`WHERE` 恰为一个**裸 MATCH**、且 WHERE 之外的 relation expr 不引用 MATCH | 该形态的 BM25 分数确实无人消费（score 只作 presence 谓词），去掉后 `calc_relevance=false`、不再产生 `match_filter_`/relevance 列、也不再触发 TAAT 与 doc-cnt/doc-len 聚合扫描 |
| 3 | `src/sql/das/ob_das_ir_define.h` | `ObDASIRScanRtDef` 新增**运行期**字段 `query_limit_param_`（不参与序列化，与 `fts_idx_` 同类）；新增 `ObDASIRScanCtDef::can_pushdown_query_limit()`（无 topk、无 `match_filter_`、无 relevance 投影、自然语言模式） | 给"行预算"一个只属于本次执行、不影响 posting 扫描的载体（posting 扫描自己的 `limit_param_` 不能用作预算，否则会截断倒排链） |
| 4 | `src/sql/engine/table/ob_table_scan_op.{h,cpp}` | 新增 `pushdown_query_limit_to_text_retrieval()`，在 `init_table_scan_rtdef()`（iter tree 创建之前）与 `prepare_pushdown_limit_param()` 调用：仅当**单个 FTS attach**（非 index merge / 非 function lookup / 非 ES match）、非 batch rescan、非 `is_das_keep_order_`、且 ctdef 满足 `can_pushdown_query_limit()` 时，把 `limit_+offset_` 写入 `query_limit_param_`（`offset_=0`，offset 仍由 TSC 应用）；每次先清空，避免 rescan 遗留旧预算 | 让检索侧知道"上层最多还要几行" |
| 5 | `src/sql/das/iter/ob_das_iter_utils.cpp` | (a) `create_text_retrieval_sub_tree()`：预算有效且 `can_pushdown_query_limit()` 时**强制 DAAT**（TAAT 会先物化全部候选再吐行，无法早停）；(b) `create_domain_lookup_sub_tree()` 的准入从"只接受 `DAS_OP_INDEX_PROJ_LOOKUP`"放宽为"接受 `INDEX_PROJ_LOOKUP` 或 `TABLE_LOOKUP`"（并校验 rtdef/ctdef 类型一致） | (a) 选到可早停的路径；(b) 去掉分数投影后 CG 会选 `DAS_OP_TABLE_LOOKUP`（`check_lookup_iter_type()` 按 `need_proj_relevance_score` 选型），而该函数里 `TABLE_LOOKUP` 分支原本存在却因准入条件永远不可达，会直接返回 `OB_ERR_UNEXPECTED(-4016)` |
| 6 | `src/storage/retrieval/ob_sparse_daat_iter.cpp` | `get_next_rows()` 的 `real_capacity` 再按剩余预算收缩：`MIN(capacity, max_batch_size_, MAX(limit_, 1))`；offset 行在 `cache_result()` 中不计入 `count`，循环会继续到 `count==real_capacity`，因此无需在此扣 offset | prompt 方案 B 的实质收益：不再为"只取 n 行"填满整个批次 |
| 7 | `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp` | `sr_iter_param_.limit_param_` 优先指向 `ir_rtdef_->query_limit_param_`（有效时），否则维持原来的 posting 扫描 limit | 把行预算真正交给 DAAT/bitmap 的门控与 `cache_result`（预算有效时位图路径按设计不再启用，DAAT 早停更便宜） |

改动合计 9 个文件、+211/−31 行（`git diff --stat`）。

## 4. 正确性证据

### 4.1 结果语义（本机，20k 文档、9333 命中）

| 查询 | 改动前 | 改动后 |
|---|---|---|
| `COUNT(*) FROM (SELECT id ... LIMIT 0/1/3/20/5000/20000)` | 0/1/3/20/5000/9333 | 0/1/3/20/5000/9333 |
| 1 词（6667 命中）`... LIMIT 20` | 20 | 20 |
| `LIMIT 5, 10`（offset） | 10 | 10 |
| `SELECT id ... LIMIT 20` 的集合 | top-20 by score | 同左（与 `ORDER BY MATCH DESC` 逐 id 一致，与 `ORDER BY id` 不同） |
| 负向：外层带 WHERE（`t.id > 10`） | 19 或 20（取决于并列分选取的 top-20 是否含 id≤10） | 同左：该形态仍保留 top-k，EXPLAIN 里 `sort_keys` 仍在 |
| 负向：内层 DISTINCT / 内层 GROUP BY | 20 / 20 | 20 / 20（未改写） |

无 ORDER BY 的顶层 `SELECT id ... LIMIT 20` 仍保留 top-k：EXPLAIN 里 `sort_keys(...)`
与 `limit(20)` 与改动前一致。行集合本就受并列分影响（同一二进制两次运行也会不同，
本机实测 `order_rel` 两跑不同），因此集合一致性只在**同一份数据/同一次构建**内比较。

### 4.2 36 形态 correctness dump

`TMPDIR=... bash .tmpdev/dump_with_bin.sh <bin> <out>`：旧二进制与新二进制各起一个实例、
用同一 `fts_large_bench_gen.py --seed 42` 灌 20k 文档、建同样 3 个全文索引，再跑
`.tmpdev/correctness_dump.sh`（318 行）。

- **318 行里只有 `order_rel` 一节不同**（10 行并列分选取），其余 308 行逐字节一致；
- `order_rel` 的差异是既有噪声：同一份**旧二进制**连跑两次，该节同样不同（见
  `.tmpdev/ab-build-base-1.dump.txt` vs `-2.dump.txt`，diff 亦只在该节）。

### 4.3 仓库用例与 DDL

```
no_score_match      VERIFY PASS
tokenize_edge       VERIFY PASS
bitmap_dedup        VERIFY PASS
bmw_topk_fallback   VERIFY PASS
```

`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`：**14 项全 OK，失败项 0**。

### 4.4 benchmark 命中数

两臂各跑一次完整 benchmark（load + 3 个索引）：

```
query_cn_hits 8001 / query_beng_hits 11000 / query_mixed_hits 7332 / query_limit_hits 20
```

与基线一致（改动前后相同）。

## 5. 性能（本机，交替 A/B）

### 5.1 单次 300 次采样（`verify_limit_push.sh`）

```
query_limit (COUNT wrap LIMIT 20)   1.55 ms -> 0.23~0.25 ms
count plain (COUNT *, 位图路径)      0.77 ms -> 0.77 ms
SELECT id ... LIMIT 20 (top-k)      1.58 ms -> 1.58 ms
```

### 5.2 8 轮配对 A/B（同机两实例交替，`ab_pingpong.sh`）

命令（A = 改动前二进制 `.tmpdev/bin-base-1103-1804.seekdb`，B = 改动后）：

```bash
TMPDIR=/tmp QUERY_ROUNDS=500 TOK_ROUNDS=3000 \
  bash .tmpdev/ab_pingpong.sh .tmpdev/bin-base-1103-1804.seekdb \
       build_release/src/observer/seekdb 8 2
# artifacts: .tmpdev/pp-1103-191708
```

`improve` = 每轮相邻配对的 (A−B)/A 中位数；spread = 配对值的中位绝对偏差（噪声参考）。

| 指标 | A 中位 | B 中位 | 配对中位 | 配对均值 | stderr | spread | n |
|---|---|---|---|---|---|---|---|
| **`query_limit_avg_ms`** | 1.4899 ms | **0.1789 ms** | **+88.02%** | **+87.82%** | 0.26% | 0.28% | 8 |
| `query_cn_avg_ms` | 0.5944 ms | 0.5974 ms | −2.13% | −0.22% | 1.71% | 2.85% | 8 |
| `query_beng_avg_ms` | 0.7854 ms | 0.7793 ms | +1.04% | +0.81% | 1.15% | 1.87% | 8 |
| `query_mixed_avg_ms` | 0.6125 ms | 0.6166 ms | −1.09% | +0.03% | 1.08% | 1.71% | 8 |
| `tokenize_ik_avg_ms`（未改代码，噪声参照） | 0.0596 ms | 0.0609 ms | +0.84% | −0.29% | 2.15% | 4.17% | 8 |
| `tokenize_beng_avg_ms`（未改代码，噪声参照） | 0.0396 ms | 0.0427 ms | −5.53% | −6.33% | 2.97% | 6.34% | 8 |

命中数 A/B 完全一致（8001 / 11000 / 7332 / 20）。三个 COUNT 类查询与两个 tokenize 指标
都在噪声带内（|paired| < spread 或同量级），即本项只动 `query_limit`，没有牺牲其它查询。

> 说明：A 臂二进制（18:04）早于工作区里**另一个人正在做的 tokenize 改动**
> （`src/storage/fts/ik/*`，18:51，与本项无关），B 臂包含该改动。它不影响本项结论：
> 该改动声称输出逐字节不变（本轮 36 形态 dump 的 TOKENIZE 段确实逐字节一致），
> 且 `query_cn/beng/mixed` 与 `tokenize_*` 的配对值都在噪声带内，命中数也一致。

> 说明：8 轮数据是在最终二进制的上一版（同一功能代码、仅探针已删）上采集的；此后
> `pushdown_query_limit_to_text_retrieval()` 只加了一个"跳过向量索引 attach"的保险条件
> （`is_vec_idx_scan`），并把 `find_target_das_def` 换成"找不到就跳过"的
> `find_child_das_def`，对 FTS 路径是等价改动。最终二进制另跑了 4 轮配对 A/B 复核，
> 结论一致（见 §5.3）。

### 5.3 最终二进制 4 轮配对 A/B 复核

`.tmpdev/ab_pingpong.sh <旧> <新> 4 2`（artifacts: `.tmpdev/pp-1103-192129`）：

| 指标 | A 中位 | B 中位 | 配对中位 | spread | n |
|---|---|---|---|---|---|
| **`query_limit_avg_ms`** | 1.4578 ms | **0.1983 ms** | **+86.39%** | 0.58% | 4 |
| `query_cn_avg_ms` | 0.5946 ms | 0.5949 ms | −0.38% | 1.61% | 4 |
| `query_beng_avg_ms` | 0.7997 ms | 0.7867 ms | +1.64% | 2.57% | 4 |
| `query_mixed_avg_ms` | 0.6526 ms | 0.6287 ms | +1.16% | 3.14% | 4 |
| `tokenize_ik_avg_ms` | 0.0604 ms | 0.0597 ms | +2.71% | 1.84% | 4 |
| `tokenize_beng_avg_ms` | 0.0379 ms | 0.0383 ms | −0.68% | 6.74% | 4 |

命中数 A/B 一致（20 / 7332 / 8001 / 11000）。结论与 8 轮一致：`query_limit` ≈ 6.5~8×，
其余指标在噪声带内。

## 6. 未完成 / 与 prompt 验收的差异

- prompt §5 验收第二条"`SELECT id ... LIMIT 1` 在 2/3 token 上 ≤ 0.35 ms"**未达到且无法在硬约束下达标**：
  该查询的返回集合就是 top-n by score（本机已用 `ORDER BY MATCH DESC` vs 无序对比证实），
  要给出精确 top-n 必须遍历候选集并算分，早停必然改变集合。本次保留该路径原样（1.5 ms 级），
  只对"外层只观测行数"的形态早停；
- 因此如果把验收理解为"任意 `SELECT id ... LIMIT n` 都要 ≤0.35 ms"，本项不满足；
  如果按 §0 的主目标 `query_limit_avg_ms ≤ 0.35 ms`，本机已达标（0.23~0.24 ms）。
- 平台口径：本机 `select1` 与本机负载会漂移，正式评测请以 `run_fts_compare.sh` 的
  `reports/fts/indextuning.txt` 为准；本报告不回填平台分数。

## 7. 复现命令

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12
bash .tmpdev/verify_limit_push.sh                 # 计划/语义/单项耗时
bash .tmpdev/dump_with_bin.sh <bin> <out>          # 36 形态 dump（两二进制对比）
for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do
  MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done
TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh
TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2
```
