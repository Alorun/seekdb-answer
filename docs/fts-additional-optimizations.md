# 四项之外的补充优化方向

> 背景：四项方案（① LIMIT 早停、② COUNT 套 LIMIT 改写、③ TOKENIZE 服务端、④ 双 writer 合并）
> 合计约 +43 个百分点 → 三类之和约 144.7%（约 96 分），仍差满分线（150%）约 5 个点，
> 且平台波动需要余量。本文给出**四项之外的候选**，按预期收益排序，每条都有实测依据。

## 先排除的两个方向（不要再试）

| 方向 | 实测 | 结论 |
|---|---|---|
| 查询重复做计划（plan cache 未命中） | 字面量重复 **0.823 ms/次** vs 预处理语句 **0.760 ms/次**（差 8%），纯解析+报错开销 0.037 ms/次 | **排除**：计划已缓存，成本是真实执行 |
| 直载写入每行哈希查找 | 加 last-writer 缓存：2.553 → 2.551 s（**+0.08%**） | **排除**，已回退 |

## ⑤ `COUNT(*)` 的 count-only 快路径（不物化 id datum）— query 类 +8~10

### 依据
`SELECT COUNT(*) FROM docs WHERE MATCH(...)` 的 `EXPLAIN` 显示 TSC 的输出是
`output([T_FUN_COUNT(*)])` —— 计数由扫描算子自己完成，**上层的任何算子都不读 id 列**。
但 `ObTextDocIdBitmapIter::emit_rows` 仍在逐行做：
`set_string(buf, 16)` + `id_flags.set(count)` + `id_proj_expr->set_evaluated_projected()`。

实测 `query_cn` = 0.65~0.87 ms，其中 emit 段 ≈ **160 µs**（8001 行 × 约 20 ns）。

### 做法
1. 先确认前提：在 `ObDASLegacyTRMergeIter::is_doc_id_bitmap_applicable()`
   （`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`）里
   判断 **id 列是否属于本次扫描的输出**（查 `ir_ctdef_` 的 `result_output_` /
   `inv_scan_domain_id_col_` 是否被引用）。若不是 → 置 `count_only_`。
2. `ObTextDocIdBitmapIter::emit_rows` 在 `count_only_` 时只递增 `count` 与计数器，
   **不写 datum、不设 flag**（`count` 的语义就是"本批产出行数"，上层只数它）。
3. 保守起见：`count_only_` 只在"无 relevance 投影 + 无 filter + 无 limit 之外还有订单需求"时启用，
   即与现有位图 gate 同一组条件。

### 验收
- `query_cn/beng/mixed` 各再快 ≥10%（8 轮配对）；
- 命中数不变（8001/11000/7332/20）；4 个仓库用例全过；36 形态 dump 无新增差异。

### 风险
低。唯一要求是"确实没有消费者读 id 列"，必须用 `EXPLAIN` 的 `output([...])` 逐形态确认
（`SELECT id`、`SELECT COUNT(*)`、`SELECT COUNT(*), SUM(id)` 三种形态分别验证）。

---

## ⑥ 两张映射表合并构建（rowkey_doc + doc_rowkey 一次扫描）— build 类 +4~7

### 依据
4 张 aux 表里 **2 张是纯映射表**（`rowkey_doc`、`doc_rowkey`，各 20k 行），
各占一个 DDL 子任务，各自对源表做一次扫描 + 排序 + 直载写入。
平台 build 受总工作量限制（build 是本机 1.8 倍而 CPU 型指标同速）。
`prepare_aux_index_tables()`（`src/rootserver/ddl_task/ob_fts_index_build_task.cpp`）
一次性提交 3 个子任务，加上 `prepare_rowkey_doc_table()` 共 4 个。

### 做法
用与 ④ 相同的手法（一次扫描、两个 writer），把 `rowkey_doc` 与 `doc_rowkey` 合并：
两者是同一份 `(rowkey, docid)` 数据的正反两个方向，一次扫描即可同时产出，
省掉一次全表扫描 + 一个 DDL 子任务的固定开销（建 tablet / 等待 / 收尾）。

### 验收
- build 三项各降 4~7%（3 块交替 A/B）；
- 4 个仓库用例 + DDL 专项 14 项全过；`SELECT id` 映射正确性（依赖这两张表）必须单独验证。

### 风险
中。与 ④ 共用同一套双 writer 基础设施，建议**在 ④ 之后做**，复用其代码。

---

## ⑦ 位图 emit 循环的微优化 — query 类 +1~3

### 依据
`emit_rows` 当前每行都做 `guard.set_batch_idx(count)`、`id_flags.set(count)`、
`id_proj_expr->set_evaluated_projected(*eval_ctx)`。其中后两者是整批语义，
本可在循环外设置一次（`set_evaluated_projected` 有 per-row 函数调用开销）。

### 做法
把 `set_evaluated_projected` 与 `get_evaluated_flags` 提到循环外；
`guard.set_batch_idx` 仅在 `count_only_ == false` 时需要。

### 验收
- 8 轮配对，`query_cn` 提升 ≥1.5% 才保留（本机噪声地板约 1.5%）；
- 无收益就回退（不要留无用改动）。

---

## ⑧ TOKENIZE 的"语句层"开销 — tokenize 类 +3~5

### 依据
`tokenize_ik` 端到端 0.0593 ms，减去客户端往返（`select1` ≈0.0254）得服务端 ≈34 µs；
而三个已测相位只有 `0.56 + 23.2 + 1.9 = 25.7 µs` ⇒ **约 8 µs 不在任何已测相位里**。

### 做法
1. 先量清这 8µs 在哪：候选是「结果集序列化与协议写回」「表达式求值框架开销」
   「`ObEvalCtx::TempAllocGuard` 的首次分配」；
2. 若在序列化：与 ③ 的 JSON 直写合并做（一次产出二进制 buffer，直接交给协议层）；
3. 若在框架：检查是否有可以省掉的 per-call 分配或对象构造。

### 验收
- `tokenize_ik` 再降 ≥5%（8 轮配对）；
- `tokenize_edge` 与 36 形态 dump 的 TOKENIZE 输出逐字节一致。

---

## 汇总：加上补充项后的账本

| 类别 | 四项后 | +⑤ | +⑥ | +⑦ | +⑧ | 合计 |
|---|---|---|---|---|---|---|
| build | 49.8（+13） | — | +5 | — | — | **54.8** |
| tokenize | 30.5（+11） | — | — | — | +4 | **34.5** |
| query | 64.4（+19） | +9 | — | +2 | — | **75.4** |
| **三类之和** | 144.7 | +9 | +5 | +2 | +4 | **164.7** |
| **平均** | 48.2 | | | | | **54.9** |
| **得分** | ~96 | | | | | **100（满分，且有 5 个点余量）** |

## 建议执行顺序

1. **先做 ①③④**（三项已在 `prompt-1/3/4` 里）——它们贡献 +43，是主力；
2. **再做 ⑤**（query 类 +9，改动小、风险低，是本文件里性价比最高的一项）；
3. **⑥ 必须在 ④ 之后**（复用其双 writer 基础设施）；
4. **⑦⑧ 最后做**，作为补齐余量的小项，单步 <1.5%/3% 就回退。

## 不变的正确性门槛（每一项都要过）

1. 36 形态 correctness dump 逐字节一致（唯一允许差异：`order_rel` 的并列组选取）；
2. 4 个仓库用例：`no_score_match`、`tokenize_edge`、`bitmap_dedup`、`bmw_topk_fallback`；
3. DDL 专项 14 项（建索引/分区表/双索引/online DDL/删索引/重建/`SELECT id` 映射）；
4. benchmark 命中数：`cn=8001 / beng=11000 / mixed=7332 / limit=20`；
5. 性能只用"同机两二进制交替 + 8 轮配对取中位"判定（本机 `select1` 漂移 ±15%）。
