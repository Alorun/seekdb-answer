# C3（查询侧）收尾报告：Q-1 倒排扫描批次 / Q-2 emit 批内提示

> 分支 `indextuning`，工作树 `/home/hahaha/ob-contest/seekdb-index-tuning`。
> 三 agent 并发期间完成；所有构建/实例/benchmark/dump 均在 `.tmpdev/tri/with_heavy_lock.sh` 内执行。
> 结论：**Q-1 的「批次 256→1024」被 `eval_ctx->max_batch_size_` 体系硬约束，按要求放弃**；
> 同一测量暴露出「批次消费侧」每 posting 的固定开销，改为**批量消费接口**，8+8 轮配对实测
> `query_cn +16.5%`（仅 select1 合格轮）且正确性四关全过，**保留**。
> **Q-2 实测无空间**（count-only emit 3.6 µs/query，已无 per-row 表达式操作），未改代码。

---

## 0. 结论速览

| 项 | 结果 | 决定 |
|---|---|---|
| Q-1 容量 256→1024 | 每批固定开销 0.53 µs、占扫描 12.7%，理论可省 ~22 µs/query（~4.4%）；但 `eval_ctx->max_batch_size_=256`（plan batch size）决定 DAS scan 输出 datum 数组大小，单独调大越界 | **放弃容量改动** |
| Q-1 替代：批次消费侧 | bitmap build 改为整批消费 buffered doc id，去掉每 posting 2 次虚调用 + 1 次 datum 拷贝 | **保留** |
| 配对 A/B（16 轮） | query_cn **+15.53% ± 1.37%**（全 16 轮）；**+16.47% ± 1.80%**（8 个 select1 合格轮） | 过 ≥3% 门槛 |
| Q-2 emit | count-only emit 实测 **3.6 µs/query（0.7%）**；该路径已无 per-row 表达式/guard；非 count-only 仅 LIMIT 20 行走 | **不改**（1.5% 门槛不可达） |
| 正确性 | 36 形态 dump 仅 `order_rel` 差异；5 仓库用例全 PASS；DDL 14/14 OK；命中 8001/11000/7332/20 全一致 | 通过 |

---

## 1. Q-1 每批扫描实测（capacity / count / 每批耗时 / 每 posting 固定开销）

方法：在 `ObTextRetrievalTokenIter::get_next_batch` 与
`ObTextDocIdBitmapIter::build_doc_id_set` / `emit_rows_count_only` 加 `OB_FTS_PROF` 门控计时
（只统计，不改行为；daemon 的 stderr 被重定向，故输出到 `/tmp/fts_prof_c3.log`），
构建 prof 二进制后单实例灌 20000 行 + 建索引，只跑 `query_cn` 600 次。
prof 源码在验证后**已全部回退**（最终二进制无任何计时/日志）。

样本：**608 次 bitmap build，32,320 次 get_next_batch，7,724,788 条 posting**（12,695 posting/query，53.2 批/query）。

| 指标 | 实测 |
|---|---|
| 每批 `capacity` | 恒为 **256**（`OB_MIN(max_batch_size_, capacity)`，两个入参都是 256） |
| 每批返回 `count` | 均值 **239.01**；满批 29,255 次（256 行）、部分批 1,843 次（均值 127.8 行）、空批 1,222 次（每 token 结束一次） |
| **每批耗时** | **4.165 µs**（其中 `das_scan_next_rows` 4.151 µs，占 **99.7%**；count-only 下 relevance 分支完全不走） |
| **每 posting** | **17.42 ns**（扫描侧；与上一轮 21 ns 估计同量级） |
| **每批固定开销** | **0.529 µs/批**（满批 4.434 µs vs 部分批 2.478 µs 解方程）；按 count 分桶加权最小二乘 **0.602 µs/批** |
| 固定开销占比 | **12.7%** 的扫描时间；折 **2.21 ns/posting ≈ 28 µs/query**（占 query_cn 0.50 ms 的 ~5.6%） |
| 满批 / 部分批 | 4.434 / 2.478 µs；worst batch 135.9 µs（个别切换/冷块） |
| bitmap build 总计 | **426.5 µs/query**（33.60 ns/posting），get_next_batch 占 **51.9%**，**消费侧（pull+bitmap）占 48.1%**（16.16 ns/posting ≈ 205 µs/query） |
| count-only emit | 19,448 次调用 / 608 query、486 万行，**2.21 ms 合计 = 3.6 µs/query** |

原始数据：`.tmpdev/tri/c3/logs/prof-lines.txt`，分析：`python3 .tmpdev/tri/c3/analyze_prof.py`。

**读法**：固定开销确实存在且有 12.7%，但**小于任务书假设的 1 µs/批**；
12669/256=50 批 ⇒ 实际只有 ~28 µs 可省，而不是 ~50 µs。
批次提到 1024 后 ≈13.3 批/query，理论省 `(53.2-13.3)×0.529 ≈ 21 µs/query ≈ 4.2%`。

## 2. 是否改动批次容量：否；约束在哪

**没有改容量**，也没有绕过。约束链（都是运行期值，不是编译期常量）：

1. `ObEvalCtx::max_batch_size_`（`src/query/api/query/engine/expr/ob_expr.h:277`）由 plan 的
   `batch_size_` 下发：`ObPhysicalPlan::alloc_op_spec`（`src/sql/engine/ob_physical_plan.cpp:823`）
   对每个 op spec 赋 `max_batch_size_ = batch_size_`，`batch_size_` 来自
   `ObCodeGenerator::detect_batch_size`（`src/sql/code_generator/ob_code_generator.cpp:86-176`，
   默认取 `_rowsets_max_rows`，本机为 **256**）。
2. DAS scan 的输出 expr datum 数组按**该 eval ctx** 的 `max_batch_size_` 分配：
   `ob_das_text_retrieval_iter.cpp:307` 的 `locate_datums_for_update(..., max_batch_size_)`；
   `ObDASScanOp::rescan` 也把 batch 夹到 `eval_ctx_->max_batch_size_`
   （`src/sql/das/ob_das_scan_op.cpp:424-425`）。
3. 检索侧自身也全按 256 分配：`ObTextRetrievalTokenIter::init` 的 `max_batch_size_` 与 `skip_`
   （`ob_text_retrieval_token_iter.cpp:71`、`ob_text_retrieval_token_iter.h:112`）、
   `ObTextRetrievalDaaTTokenIter::init` 的 `relevance_`/`doc_id_`（同文件 `init()` 内
   `prepare_allocate(max_batch_size_)`）、bitmap 的 `emit_buffer_`（`MAX(iter_param_->max_batch_size_,1)`）。

即：**token iter 单独调大 → das scan 会往 256 行大小的 datum 数组里写 1024 行**，直接越界；
要动就得把整个 plan 的 vector size（所有算子）一起调大（或改 `_rowsets_max_rows`/加 hint），
这属于全局执行器参数、会改变所有类别的内存与行为，**不属于查询侧低风险收尾，未采用**。
按任务书 §2.2-4「若约束无法打破，放弃本项，不强行改」执行。

## 3. Q-1 替代实现：批次消费侧（保留）

测量显示真正可动的是**消费侧**：bitmap build 通过
`get_next_row() → get_curr_id()` 逐条拉 256 行缓冲，每条 posting 付 2 次虚调用，
且每次 refill 还在 `save_docids()` 里把 256 个 datum 拷进 `doc_id_` 数组。
本改动让维度迭代器把**已缓冲的整批**直接交给 bitmap：

```
ObISRDaaTDimIter::get_next_id_batch(const ObDatum *&ids, const ObBitVector *&skip, int64_t &count)  // 默认 OB_NOT_IMPLEMENT
  ↳ ObTextRetrievalDaaTTokenIter 覆写：token_iter_->get_next_batch(256) 后返回 scan 输出 expr 的
    datum 数组（连续 256 个）+ skip，并标记整批已消费（不再经 save_docids 拷贝）
ObTextDocIdBitmapIter::build_doc_id_set：整批 for 循环 add_doc_id；接口未实现时回退原逐行路径
```

- 行集、skip 过滤、表 tablet 归属、datum 生命周期（到下次 refill）**完全不变**；
- `retrieval/` 之外零改动；不动分词、不动构建、不动 plan/容量；
- 改动文件 4 个（80 行）：`ob_i_sparse_retrieval_iter.h`、`ob_text_retrieval_token_iter.{h,cpp}`、
  `ob_text_daat_iter.cpp`。

### 3.1 8 轮配对数据（A=base=`bin/base.seekdb`，B=cand=`bin/cand.seekdb`）

`ab_pingpong.sh base cand 8 2`（同机两实例交替 A,B/B,A；每轮 2 个 sample，每 sample 500 次 SQL）。
共跑 **2 组 × 8 轮 = 16 个配对轮**（`pp-1104-122617`、`pp-1104-123038`）。

**全 16 轮（中位 ± stderr=std/√n）**：

| metric | A 中位 | B 中位 | 配对中位 | stderr | MAD |
|---|---|---|---|---|---|
| query_cn | 0.5608 ms | 0.4890 ms | **+15.53%** | **1.37%** | 4.12% |
| query_beng | 0.7320 ms | 0.6014 ms | **+18.24%** | 0.84% | 2.18% |
| query_mixed | 0.5892 ms | 0.5143 ms | **+12.97%** | 1.48% | 2.41% |
| query_limit（负对照，不走本改动） | 0.2067 ms | 0.2137 ms | +2.28% | 3.35% | 10.95% |
| tokenize_ik | 0.0519 ms | 0.0544 ms | −7.85% | 2.72% | 6.20% |
| tokenize_beng | 0.0388 ms | 0.0368 ms | +5.66% | 4.82% | 11.73% |
| select1（机器对照） | 0.0249 ms | 0.0243 ms | −2.10% | 4.30% | 14.86% |

**按协议 §5 剔除 select1 两臂差异 >15% 的轮次后（8/16 轮合格）**：

| metric | 配对中位 | stderr | min | max | n |
|---|---|---|---|---|---|
| query_cn | **+16.47%** | **1.80%** | +7.05% | +23.27% | 8 |
| query_beng | **+18.22%** | 1.07% | +13.70% | +21.31% | 8 |
| query_mixed | **+12.51%** | 2.56% | −5.88% | +17.33% | 8 |
| query_limit | +6.60% | 4.45% | −14.09% | +23.57% | 8 |

两组独立跑分别：run1 `cn +12.79% / beng +19.20% / mixed +13.94% / limit −6.22%`；
run2 `cn +18.21% / beng +16.48% / mixed +10.84% / limit +3.48%`。**结论可复现**。

**归因**：只有 `is_doc_id_bitmap_applicable()` 的无分数查询（cn/beng/mixed）变快；
`query_limit`（有 LIMIT，不走 bitmap）在 16 轮里是 +2.28%±3.35%、且两组符号相反 ⇒ 噪声，
正好作为阴性对照。08-16 轮逐轮 query_cn 均 >+5%（唯一 +5.34% 的 run2-r1 是 select1 不合格轮）。

**离群/作废标注**：
- select1 漂移 >15% 的 8 轮已列出（run1-r1/r3/r5/r6、run2-r1/r4/r6/r8），按要求作废；合格轮结论更强。
- run2 的 A 臂 `build_ik_all=6.874 s`（冷缓存离群，同轮 B=2.789 s），build 相关只用 run1/run2 的稳定值。
- 未做「交换臂位置」的第三次 A/B：脚本本身每轮交替 A/B 先后顺序，且两组独立跑一致，风险已缓解。

### 3.2 是否符合「已做/已证否」边界

未重复：无分数 gate、位图去重、count-only、LIMIT 下推、COUNT(*) 改写（均在改动前已存在）；
未触碰已证否的手工 k 路归并 / filter guard / 计划缓存。
本改动是新的「批次消费」层，属于 Q-1 的批次边界问题（减少批次内每条 posting 的行接口开销），
不是容量改动。

## 4. Q-2：emit 批内提示（实测无空间，未改）

- `emit_rows_count_only`（count-only，`query_cn` 走这条）：实测 **3.6 µs/query**（486 万行 / 608 query），
  占 `query_cn` 的 **0.7%**；当前实现已用整字 `popcount` 消费、无 `BatchInfoScopeGuard`、
  无 `set_evaluated_projected`、无 per-row 表达式调用，`emit_buffer_` 在 count-only 下也完全不分配。
- `emit_rows`（非 count-only）仅被 `query_limit`（LIMIT 20，`capacity≈20`）使用，
  per-row 的 `guard.set_batch_idx`/`set_evaluated_projected` 即使全省也只有 20 行 ×2 次调用，
  上界远低于 0.1%；`query_limit` 配对 +2.28%±3.35%（阴性对照）也证明这里没有可测空间。
- 因此 **Q-2 未改代码**：任务书门槛 1.5%，而该路径总耗时只有 0.7%×（其中还有相当部分是不可省的
  bitmap 游标推进）。不引入改动，也没有可归因的 A/B（改与不改的差异 < 噪声地板）。

## 5. 正确性证据

所有检查都在 `with_heavy_lock.sh` 内、用 `.tmpdev/tri/c3/bin/cand.seekdb`（A/B 的同一二进制）完成。

1. **36 形态 dump**（`.tmpdev/correctness_dump.sh 2893`，脚本实际 37 个 shape）：
   base vs cand 各跑一遍（各自 20000 行 + 建索引），`diff` 共 **27 行 = 仅 `order_rel` 一个 hunk**，
   10 个 id 不同（`769…` vs `6313…`），与任务书「唯一允许差异 order_rel」一致（浮点同分排序不稳定，非本改动路径）。
   其余全部逐字节一致，包括：
   `cn_2terms=8001`、`cn_1term=4668`、`cn_common=11334`、`cn_mixed=7332`、`en_3terms=11000`、
   `missing=0`、`cn_2terms_set=8001 / 80002665 / 1 / 19999`（COUNT/SUM/MIN/MAX）、
   `limit_20=20`、`limit_offset=10`。
   原始：`.tmpdev/tri/c3/logs/dump-{base,cand}.txt`、`dump-diff.txt`。
2. **5 个仓库用例**（`MODE=verify`，结果文件比对）：
   `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`
   —— **全部 VERIFY PASS**（`.tmpdev/tri/c3/logs/verify-cand.log`）。
3. **DDL 专项 14 项**：**14 OK / 0 FAIL**（建表建索引、命中数、docid→rowkey、分区表、双索引、
   online DDL、删/重建索引、清库）。
4. **命中数**：16 个 A/B 轮 ×2 臂 + 2 次 dump，全部为 **8001 / 11000 / 7332 / 20**，两臂逐轮一致。

## 6. `build_*` / `tokenize_*` 是否受影响（实测）

改动只在 `src/storage/retrieval/` 的无分数 bitmap 合并路径，二者不在该路径上；实测确认：

- **build**（每臂一次 full-load，n=1，冷缓存噪声大）：
  run1 A/B：total **6.340 / 6.307 s**（−0.5%）、ik_all 2.816 / 2.790、ik_content 2.260 / 2.239、beng 1.247 / 1.260；
  run2：B ik_all 2.789、ik_content 2.254、beng 1.244（A 的 ik_all 6.874 s 为冷缓存离群，剔除）。
  ⇒ 无系统性变化（DDL/索引构建不经过检索迭代器）。
- **tokenize**：16 轮配对 ik **−7.85%±2.72%**、beng **+5.66%±4.82%**，两者**符号相反**且 MAD 6~12%；
  tokenize SQL 根本不进入检索路径，且两臂 tokenize 源码完全相同（`git status` 只差我的 4 个 retrieval 文件），
  这是同机噪声/代码布局效应，不是功能影响（类别平均后约 −1%）。
- 两臂二进制来源：先构建 cand（改动在树），`cp` 恢复 4 个文件后构建 base；
  两次构建之间 `git status --porcelain` **完全一致**（仅 C1/C2 的 8 个文件，无他人文件变动）。
  bin-base 首次构建曾因 `cp -a` 保留旧 mtime 未重编（与 cand 字节相同，`cmp` 发现），
  `touch` 后强制重编，最终 base/cand 字节不同（1332844328 vs 1332844344）。

## 7. 未完成部分与原因

1. **批次 256→1024 未做**：受 §2 的 eval ctx / datum 数组约束；按任务书要求放弃，未强行改。
   理论收益也已从任务书假设的 ~9% 修正为实测 **~4.2%**（固定开销 0.53 µs/批，不是 1 µs）。
2. **Q-2 未改**：实测上限 0.7%（count-only 已是 popcount 实现），1.5% 门槛不可达。
3. `order_rel` 仍有差异：任务书允许；它不走 bitmap 路径，与本改动无关，属于浮点同分排序不稳定。
4. 未做交换臂位置的第三次 A/B、未做并发/多 tablet 压力测试（bitmap 多 tablet 路径由
   `bitmap_dedup` 用例与 dump 覆盖；本改动不改变 tablet 处理逻辑）。
5. 交付时 `build_release/src/observer/seekdb` 已按最终工作树重编（含 C2 之后的 beng parser 改动），
   因此它**不等于** A/B 测过的 `bin/cand.seekdb`；A/B 证据严格对应 `bin/cand.seekdb`。

## 8. 复现命令

```bash
# A/B（16 轮 = 2 组）
.tmpdev/tri/with_heavy_lock.sh bash -c 'bash .tmpdev/tri/c3/ab_pingpong.sh \
  .tmpdev/tri/c3/bin/base.seekdb .tmpdev/tri/c3/bin/cand.seekdb 8 2'
# 正确性
.tmpdev/tri/with_heavy_lock.sh bash -c 'bash .tmpdev/tri/c3/dump_both.sh \
  .tmpdev/tri/c3/bin/base.seekdb .tmpdev/tri/c3/bin/cand.seekdb'
.tmpdev/tri/with_heavy_lock.sh bash -c 'bash .tmpdev/tri/c3/verify_cand.sh'
```

## 9. 交付物与二进制校验

| 角色 | 路径 | md5 | 说明 |
|---|---|---|---|
| base（A 臂） | `.tmpdev/tri/c3/bin/base.seekdb` | `30fb7ff70c1eed55620fde363377d2ff` | 开工前源码（+ 当时 C1/C2 改动） |
| cand（B 臂） | `.tmpdev/tri/c3/bin/cand.seekdb` | `8b7a1bdd23767d0ba2be6ddcb01dbe18` | A/B 与全部正确性检查用的就是它 |
| 最终集成 | `build_release/src/observer/seekdb` | `14e924102a106fb25e13c896cd5e2bfd` | 收尾时按最新工作树重编（含 C2 之后新增的 beng parser 改动），与 cand 不同属预期 |

- base/cand 两次构建的 `cp` 都在同一个 `with_heavy_lock.sh bash -c` 内完成；base 的 md5 曾与
  `build_release` 校验一致。最终集成二进制另跑了 smoke：命中 **8001 / 11000 / 7332 / 20**，可启动可查询。
- 注意：收尾时 smoke 的 `build_total_sec=20.9 s`（同日 A/B 两臂均为 6.3 s），差异来自当时工作树里
  C1 正在进行的 DDL 侧改动/打点，不是本改动（本改动不在 DDL 路径；A/B 两臂 build 6.34/6.31 s）。
