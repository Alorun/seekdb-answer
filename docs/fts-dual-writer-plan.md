# FTS 双 writer 合并：修订实施计划

> 本文是对 [`docs/fts-dual-writer-design.md`](fts-dual-writer-design.md) 的**修订版实施计划**，目标不变
> （FTS 索引构建 −20%+），但修正了原设计中的一处关键错误假设，并把落地拆成 6 个可独立验收的里程碑。
>
> 前置阅读：`docs/fts-optimization-report.md` §3.2（构建成本拆解）、§5.1.2（D1 专项 A/B）、§10（复现命令）。
> 本文所有行号基于仓库当前快照（`git_head f598b01a1` + 工作区改动），改动落地后会漂移，请以函数名为准。

---

## 0. 实施状态（M0/M1 已完成，2026-11-02）

| 里程碑 | 状态 | 结论 |
|---|---|---|
| M0 测量门槛 | **完成** | 见 §2.4。分词 0.49 s/表，占单表流水线 ~21%；**主导项是扫描外发/背压 0.65 s + 排序 0.5 s + 写入 0.45 s**。双 writer 的净收益上限被压到 **0~5%**（详见 §2.5） |
| M1 一次分词产出两套行 | **完成并验证** | `ob_das_domain_utils.cpp` 新增 `generate_fulltext_word_rows_dual_impl()` + `FTS_DUAL_ROWS_CHECK` 自检；3000 文档 × 2 张表逐字段比对 **mismatch=0** |
| M2~M5 计划层/排序层/会话层/DDL 编排 | 未开始 | 框架级改造，见 §4；按 §2.5 的实测结论，建议先决策是否值得投入 |

> **M0 的两个否决性发现（务必先读 §2.4/§2.5）**：
> 1. 每张分词表只有 **1 个扫描线程**，扫描+分词+外发是串行前端（1.15 s / 2.45 s 墙钟）。
>    把两张表合并成"一次扫描"会让这一个线程外发 **2×693k 行**，前端反而从 1.15 s 涨到 ~1.8 s，
>    除非同时把 scan DFO 并行度提到 ≥2（而 docid 的幂等生成依赖扫描顺序，属另一项改造）。
> 2. "让 doc_word 流天然有序从而省掉第二次排序"这条捷径**不成立**：`ObDocId::get_string()`
>    是 `{tablet_id, seq_id}` 的裸 16 字节小端编码（`ob_object.cpp:891`），字节序 **不等于** seq 序，
>    所以按扫描顺序产出的 docid 不是 doc_word 主键序。

## 0.1 结论摘要

| 条目 | 原设计 | 修订后 |
|---|---|---|
| 收益预期 | −25~30% | **先测后定**：理论净收益 5~16%，中位 5~10%；**M0 实测排序成本 < 分词成本 1/3 时才可能接近 16%** |
| 关键假设 | "两个 writer 各自维护 sorter，天然满足排序要求" | **不成立**：writer 不排序，排序在计划内的 SORT 算子里，且切片边界按**目标表 PK 序**采样。两表 PK 序不同 ⇒ **必须两路排序** |
| 表写入归属 | 一个 DAG 写两张表 | 直载 sstable / 补数据 / checksum / redo replay 全部以 **单个 `ddl_task_id` + 单表 schema** 为 key ⇒ 需"第二张表的会话与任务归属"（方案 A）或改造为多表 DAG（方案 B） |
| 实施 | 3 步 | **6 个里程碑（M0 测量门槛 + M1~M6）**，每步独立验收、独立回退 |
| 并行度 | 未涉及 | 现状两张表是**并发**构建；合并后必须保持两条分支并发，否则墙钟会退步（见 §2.3） |

---

## 1. 已确认的代码事实（证据表）

改这个方向之前必须承认下面 13 条事实，它们是原设计 §3.2(b)/§3.3 需要修正的依据。

| # | 环节 | 代码位置 | 结论 |
|---|---|---|---|
| A1 | aux 构建 SQL 生成 | `ObDDLTaskUtil::generate_build_replica_sql`（`src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp:310`），INSERT 串在 `:537` | 每张 aux 表一条独立 SQL：`INSERT /*+ parallel(N) opt_param('ddl_task_id',..) opt_param('enable_newsort','false') use_px */ INTO aux (...) SELECT ... FROM data AS OF SNAPSHOT ... ORDER BY <dest rowkey>` |
| A2 | ORDER BY 的来源 | 同文件 `:441-478` 取**目标表** `get_rowkey_info()` → `order_column_ids`；`:285` `generate_order_by_str` | fts_index 按 `(word, docid)`、fts_doc_word 按 `(docid, word)`。**同一次扫描不可能同时满足两种序** |
| A3 | DDL 插入计划形状 | `ObDelUpdLogPlan::create_online_ddl_plan`（`src/sql/optimizer/ob_del_upd_log_plan.cpp:952-992`） | `STAT COLLECTOR(SAMPLE_SORT) → EXCHANGE(range shuffle) → SORT(按目标 PK) → PX SSTABLE INSERT → EXCHANGE`。堆表走 `EXCHANGE → PDML INSERT → EXCHANGE`（无 sort/stat collector） |
| A4 | 切片边界从哪来 | `ObDynamicSamplePieceMsgCtx::build_whole_msg`（`src/sql/engine/px/datahub/components/ob_dh_sample.cpp:403`）→ `slice_store->get_or_insert_schedule_info`（`:438`）→ `ObServer::get_or_insert_schedule_info`（`src/observer/ob_server.cpp:225`）→ 落库 `ObDDLSliceInfo.part_ranges_`（`src/rootserver/ddl_task/ob_ddl_task.cpp:2382`） | 切片（每个 tablet 的 PK 区间切点）是**运行时按目标表 PK 序采样、按 `ddl_task_id` 持久化**的。第二张表必须有自己的采样与持久化 |
| A5 | 切片信息消费 | `ObDDLInsertDag::update_tablet_range_count`（`src/storage/ddl/ob_ddl_insert_dag.cpp:80`）→ `load_idempotent_ddl_tablet_slice_counts`（`src/rootserver/ob_local_management_service.cpp:2972`，数据源 `part_ranges_`） | DAG 用本 task 的 slice counts 建立"每 tablet 的 slice 数 / 全局偏移"，供幂等 autoinc/docid 与有序 writer 使用 |
| A6 | fts 行展开位置 | `ObTableScanOp::inner_get_next_fts_index_row`（`src/sql/engine/table/ob_table_scan_op.cpp:3633`）、`fetch_next_fts_index_rows`（`:3652`）、`fill_generated_fts_cols`（`:3691`）；spec 标志在 `src/sql/code_generator/ob_static_engine_cg.cpp:3681` | 分词在**扫描算子内部**完成（`T_FUN_SYS_WORD_SEGMENT` / `DOC_ID` / `WORD_COUNT` / `DOC_LENGTH` 四个伪列），一个源行展开成 N 个输出行。**这是"只分词一次"唯一可能的汇合点** |
| A7 | 行生成 | `ObFTIndexRowCache::segment`（`src/sql/das/ob_das_domain_utils.cpp:84`）→ `ObDASDomainUtils::generate_fulltext_word_rows`（`:306`）；列顺序常量在 `:43-44` | 两种列顺序只差 word/docid 交换（`[word,docid,cnt,len]` vs `[docid,word,cnt,len]`），**同一份分词结果的一个置换即可产出两套行**（M1 省事的关键） |
| A8 | 有序直载写入 | `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row`（`src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp:461`）、`get_tablet_info_from_row`（`:53`）、`switch_slice_if_need`（`:512`） | 用 `DIRECT_INSERT_ORDERED_WRITER`（`ObRsSliceWriter`）：**要求输入已按目标表 PK 有序**，切片切换靠 tablet_id + slice_idx；每行一次 `eval_current_row` + `append_row` |
| A9 | 直载会话 | `ObPxSubCoord::start_ddl`（`src/sql/engine/px/ob_px_sub_coord.cpp:695-727`）、`get_participants`（`:761`） | 会话参数只有**一个** `table_id` + 一组 participants；participants 由 `ObDASUtils::get_related_tablet_loc(*loc, table_id)` 从 SQC 的 table loc 里解析 |
| A10 | 相关表 tablet 解析 | `ObDASCtx::build_related_tablet_loc`（`src/sql/das/ob_das_context.cpp:230`）、`ObDASUtils::get_related_tablet_loc`（`src/sql/das/ob_das_utils.cpp:82`）、`loc_meta.related_table_ids_`（CG 填充：`src/sql/code_generator/ob_dml_cg_service.cpp:2634`、`ob_tsc_cg_service.cpp:1358`） | **已有现成机制**按分区把"另一张表的同名分片"挂到同一 tablet loc 链上（本地索引/LOB 就是这么做）。第二张表的 tablet 解析与 participants 可复用此机制，不必新造 |
| A11 | 直载 DAG 单表绑定 | `ObDirectInsertSessionImpl::start`（`src/storage/ddl/ob_direct_insert.cpp:209`，`dag_param.ddl_task_param_.target_table_id_ = param.table_id_`、`tablet_ids_ = participants_`）、`ObDDLIndependentDag::init_by_param`（`src/storage/ddl/ob_ddl_independent_dag.cpp:82`）、`fill_ddl_table_schema`（`:106`）、`init_tablet_context_map`（`:111`）、writer `create`（`ob_direct_insert.cpp:419`，`fill_writer_param` 用 dag 的 schema + tablet context） | 一个 DAG = 一张表 schema + 一套 tablet context。要在一个 DAG 里写第二张表，等于改 `ObDDLIndependentDag` / `ObDDLTableSchema` / `ObWriteMacroParam` / merge & replay |
| A12 | 完成与校验 | `ObIndexBuildTask::process` 状态机（`src/rootserver/ddl_task/ob_index_build_task.cpp:247-282`）、`verify_checksum`（`:1111`）、`enable_index`（`:1236`）；父任务 `ObFtsIndexBuildTask::prepare_aux_index_tables`（`src/rootserver/ddl_task/ob_fts_index_build_task.cpp:550`）+ `wait_aux_table_complement`（`:835`）+ `dependent_task_result_map_` | 每张 aux 表都有自己的 `ObIndexBuildTask`：REDEFINITION（灌数）→ VALIDATE_CHECKSUM → TAKE_EFFECT（enable_index）。父任务按 4 个 task id 等待。**complement/checksum/enable 不能省，只能改由谁触发** |
| A13 | 逐 tablet 构建 RPC | `ObDDLLocalBuildExecutor::construct_request_arg`（`src/rootserver/ddl_task/ob_ddl_local_build_executor.cpp:310`）→ `obcall::ObDDLLocalBuildArg`；参数结构 `ObDDLLocalBuildExecutorParam`（`ob_ddl_local_build_executor.h:31`） | 若要"一个构建请求同时喂两张表"，这里是**唯一**需要新增字段的跨边界结构（source/dest tablet 成对出现） |

### 1.1 直接推论

1. **原设计 §3.3 的第一条（"现有 sorter 是 writer 内部的"）错误。** writer 侧没有 sorter；sorter 是计划里的 `SORT`（A3），
   且切片切点由 `STAT COLLECTOR` 按目标表 PK 采样后持久化（A4/A5）。
   ⇒ 合并方案必须自己造出**第二路有序流**，代价 ≈ 一次全量排序（~960k 行/表）。
2. **省掉的分词只有一次**（A6/A7），所以收益上限 = 一次分词的成本；下限可能为 0（若第二路排序吃掉它）。
3. **表归属是硬约束**（A11/A12）：不能简单"在插入算子里多开一个 writer"，除非同时解决第二张表的
   schema/tablet context/slice info/complement/checksum/replay 归属。

---

## 2. 修正后的收益模型

### 2.1 现状成本结构（20k 文档、`ik(content)`，数据来自报告 §3.2 + `report-final-212538.txt`）

| 项 | 每表 | 说明 |
|---|---|---|
| 扫描 + LOB 读 | ~0.02 s | `inner_get_next_row_implement` 1 µs/doc；LOB 物化实测 0 µs |
| 分词 + 词表 + 行生成 | ~0.4 s | 6~38 µs/doc，取 ~20 µs ⇒ 0.4 s；报告称两次分词合计约占构建 33% |
| 排序（按目标 PK） | **待测（M0）** | 960k 行、4 列、`enable_newsort=false` |
| 直载写入 | **待测（M0）** | 1.3~2.4 µs/行 的口径来自报告 §3.2，需按表复核 |
| DDL 固定开销 | 报告口径：准备 35% + 收尾 23%（以"无输出行"的 beng 构建测得） | 与本次改动基本正交（已被 D1 优化过一部分） |

### 2.2 合并后的净收益

```
# CPU 口径（两表求和）；墙钟因两表并发取 max，故折算时除以实际并发度 2
T_before = 2 × (scan + segment + sort + write) + fixed
T_after  = 1 × scan + 1 × segment + 2 × sort + 2 × write + fixed
净收益   = segment / T_before                              # 只有这一项被消除
```

以报告口径（`segment ≈ 0.4 s`、`T_before ≈ 2.5 s`）代入 ⇒ **理论 16%**；
若把"第二路排序无法与第一路重叠"的额外串行损失算进去（现状两表是并发构建的），
实际落在 **5~10%**。**这就是为什么必须先做 M0 再决定是否继续**。

### 2.3 并行度护栏（容易踩的坑）

现状：两张表各自一个 DDL 子任务、各自 DOP=2，**墙钟是两条流水线的重叠**（报告 §7 已实测 4 张 aux 表在同一窗口完成）。
合并后若退化成"单 DFO 内先 A 后 B 串行"，即使总 CPU 少了一点，墙钟也可能变差。
因此 M3 必须二选一：

- **M3-A（推荐）**：单计划、两个 insert 分支（两条 DFD 并发），每个分支自己的
  `STAT COLLECTOR + EXCHANGE(range) + SORT + INSERT`；
- **M3-B（保守）**：单 DFO、单 insert 算子内"两次过排序"（先 A 后 B），
  代价是并行度下降，需要把 DOP 提高到 `2×原 DOP` 补偿（需实测，报告里 DOP=4 在旧结构下无收益，不能直接照搬结论）。

### 2.4 M0 实测（20k 文档，`ik(content)`，DOP=2，单次 ALTER，本机）

计时埋点：`ObTableScanOp::fetch_next_fts_index_rows`（分词/LOB/扫描线程存活）+
`ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row`（首行/写入）。

| 相位 | 每表 | 说明 |
|---|---|---|
| 源扫描 + LOB 物化 | 0.02 s / 0.3 ms | 与报告一致，不是瓶颈 |
| **分词 + 词表 + 行生成** | **0.48~0.49 s** | 即 `segment()`，单线程 |
| **扫描线程存活（含向 exchange 外发 693k 行 + 背压）** | **1.12~1.21 s** | 外发/背压约 0.63~0.72 s |
| 排序 + exchange 到首行 | ~0.45~0.6 s | `first_row − scan_wall` |
| 写入 `append_row` | 0.45 s | 两 worker 合计 |
| `eval_current_row` + tablet 定位 | 0.10~0.19 s | `tail_us − append_us` |

总量：693,336 行/表（34.7 词/文档），两表合计 ~4.6 CPU-s，墙钟 2.45 s（CPU/wall ≈ 2.2，非算力受限）。

### 2.4.1 扫描前端内部拆解（M0b，比 §2.4 更精确）

同一个 20k 文档构建，在扫描算子内部逐相位计时（每张分词表各一个线程）：

| 相位 | 表 A（fts_index） | 表 B（fts_doc_word） | 占前端 |
|---|---|---|---|
| 源行扫描 `inner_get_next_row_implement` | 29.4 ms | 30.0 ms | 3% |
| LOB 物化 | 0.3 ms | 0.3 ms | 0% |
| **`segment()` 合计** | **492 ms** | **528 ms** | **55%** |
| ├─ `ObFTParseHelper::segment`（IK 分词 + 词表插入） | **420 ms** | **452 ms** | **47%** |
| └─ 行生成（`ObDatumRow` 分配 + 4 列填充） | 36 ms | 37 ms | 4% |
| `fill_generated_fts_cols`（4 个 datum 拷到输出列） | 47 ms | 45 ms | 5% |
| gap（exchange 发送 + 背压） | 293 ms | 315 ms | 33% |
| 扫描线程存活合计 | 894 ms | 949 ms | 100% |

结论修正：
1. **前端的主项是 IK 分词 + 词表（47%）**，不是行生成（4%）。所以"合并两次分词"省下的是分词本身的 CPU，
   但两张表的分词**本来就是并行的**（各自一个扫描线程），**省不到墙钟**。
2. 前端第二项是 exchange 发送/背压（33%），属 PX 通用通道，合并后会变成"一个线程发两路"，反而更长（§2.5）。
3. 行生成/填充合计仅 9%，不值得优化。

### 2.4.2 `enable_newsort` 已被证伪（5 块交替 A/B）

`build_ik_content`（20k 文档，每次全新实例 + 灌数 + 单次 ALTER，命中数 3333 作有效性校验）：

| 臂 | 5 次墙钟（s） | 中位 |
|---|---|---|
| 现状 `enable_newsort=false` | 2.451 / 2.449 / 2.562 / 2.449 / 2.551 | **2.451** |
| 试验 `enable_newsort=true` | 2.452 / 2.453 / 2.549 / 2.450 / 2.448 | **2.450** |

配对差 +0/+4/−13/+1/−103 ms ⇒ **零收益**。已回退（回退后二进制 md5 与基线一致）。

### 2.4.3 写路径内部拆解（b3 专项，693,336 行/表）

| 环节 | 耗时 | 每行 |
|---|---|---|
| `build_multi_version_row`（4 列拷贝到存储 row） | 15.3 ms | 0.022 µs |
| `convert_to_storage_row`（arena reuse + lob/reshape 检查 + NULL/长度校验） | 12.7 ms | 0.018 µs |
| **`ObTabletSliceWriter::append_row`（宏块构建）** | **455.4 ms** | **0.657 µs** |

三者合计 483 ms，**其中 94% 在宏块构建**（`ObMacroBlockWriter::append_row`：`check_order` 比主键 + 逐行 checksum + `save_last_key` 拷主键 + 微块切分判断）。
另一 worker（497,337 行）同样为 0.657 µs/行，与行数线性。

### 2.4.4 b3（批量写入）的结论：不可行/收益极小

1. `ObMacroBlockWriter::append_batch` 内部仍是 **逐行 `to_datum_row` + `append_row_inner`**（`ob_macro_block_writer.cpp:708-721`），
   批量接口**不会**降低 0.657 µs/行的宏块构建成本。
2. SQL 侧可批量的只有 `build_multi_version_row` + `convert_to_storage_row`，合计 **28 ms/表（4%）**。
3. 要让"分行发送"变成"批量发送"，必须让扫描端矢量化，而 CG 对 FTS aux 构建是**显式关闭**的：
   `ob_static_engine_cg.cpp:3684  spec.max_batch_size_ = 0; // TODO ... after support post-building fts index vectorization`；
   PDML 插入算子全家也无 `inner_get_next_batch`（探针实测 `insert_max_batch=0`，其子算子 `child_max_batch=16` 已具备批量能力）。
4. 因此 b3 的收益上限 ≈ transmit 0.30 + eval 0.15 + fill 0.05 ≈ 0.5 s/表，**且需要端到端矢量化（多天工作量）才能拿到，
   实际落地约 3~5%**。已关闭该方向。

### 2.4.5 DDL 固定开销的修正（此前高估）

按 DDL 任务日志时间线（同一次 2.551 s 的 `ALTER ... ADD FULLTEXT INDEX`）：

| 阶段 | 时间 |
|---|---|
| `init` → `prepare_aux_index_tables`（提交 4 个子任务） | **77 ms** |
| 4 个 aux 子任务构建 + `wait_aux_table_complement` 轮询 | **~2.2 s** |
| checksum / enable_index / 收尾（最后 3 个 `remove_ddl_task`） | **~0.25 s** |

即固定开销只有 **~0.3 s（12%）**，不是早先"beng 零行对照"推出来的 ~40%——那次对照把
"实例首次建 FTS 索引的一次性成本（建词典表等）"算进去了。**合并 DDL 子任务（b2）最多只能拿回 ~0.2 s（8%）。**

### 2.4.6 并行构建切片（第 1 项）的验证与 P3 实验

**价值验证（同一实例、同一份 20k 数据、都不是首次建索引）**

| 表形态 | `ALTER ... ADD FULLTEXT INDEX (content) WITH PARSER ik` | 命中 |
|---|---|---|
| 非分区（每表 1 个扫描线程） | 2.540 / 2.438 s | 3333 |
| 4 分区（每 tablet 一个扫描线程） | **1.864 / 1.847 s** | 3333 |

⇒ 让整条 per-tablet 流水线并行 = **−27%**。

**P3（docid 幂等公式）单独落地失败——实测证据**

在 `ObExprDocID::generate_doc_id` 里把 DDL 构建期的 docid 改为
`generate_idempotent_value(K=1, idx=0, interval=10000, row_idx)`（每 exec ctx 一个计数器，tablet 变化时重置），
20k 三索引构建直接报 **`ERROR 1062 Duplicated primary key`**。

探针（`FTS_DOCID_PROBE`，逐次打印 tablet / 计数器 / ddl_task_id）给出**精确根因**：

- 两张**分词表**：docid 在**扫描期**生成（`ObTableScanOp::fetch_next_fts_index_rows` 里 eval `T_FUN_SYS_DOC_ID`），
  每张表 1 个扫描任务 ⇒ 一个计数器，0..19999 正常；
- 两张**映射表**（rowkey_doc / doc_rowkey）：docid 在该 PDML 插入算子的 **2 个 insert worker** 里 eval
  （生成列被推迟到 `eval_current_row`），两个 worker 各自的 exec ctx 都从 `row_idx=0` 开始 ⇒ docid 0..9999 出现两遍 ⇒ 主键冲突。

即：**docid 的生成点在不同 aux 表之间都不一致（扫描期 vs 插入期），切片也不一致（1 vs 2）**。
因此"按切片幂等的 docid"必须先满足两个前提：

1. **四张 aux 表的 docid 都在同一位置、同一套切片下生成**（推荐全部前移到扫描期，扫描任务数即切片数）；
2. **P1/P2/P3 同一次落地**：父任务算出数据表 PK 的 K 段切点 → 四张表按同一组切点并行扫描 → docid 用
   `generate_idempotent_value(K, slice_idx, interval, row_in_slice)`；表间切片一致才能保证 docid 一致。

该实验已回退，工作区与二进制回到已验证基线（`7362740e2902…`）。**下一步的第一件事是"把映射表的 docid 也前移到扫描期"**
（让扫描算子投影 docid 生成列），否则任何切片化 docid 都会在映射表上重复。

### 2.4.7 docid 位置无关化的两次尝试（P0' 实验记录）

目标：让 docid 变成 `(tablet, rowkey)` 的纯函数，从而与扫描顺序/并行度无关，这是并行构建切片的**地基**。

**尝试 A：exec ctx 计数器 + 幂等公式（K=1）** → **`ERROR 1062 Duplicated primary key`**。
反汇编/backtrace（`ObPxTransmitOp::fetch_first_row` → `ObOperator::get_next_row` → `ObExpr::eval`）证明：
两张**映射表**的 docid 由 PDML 插入算子的 **2 个 worker** 逐行 eval（生成列被推迟到 insert），
两张**分词表**的 docid 由**扫描期** eval（1 个扫描任务）。生成点与切片数都不同 ⇒ 计数器各自从 0 开始 ⇒ 重复。

**尝试 B（架构上正确）：把 rowkey 作为 DOC_ID 的第二个参数**
- `ObFuncExprOperator` 新增 arity `ZERO_OR_TWO`（`ob_expr_operator.h` + `ob_raw_expr.cpp` 的 `check_param_num`）；
- `ObExprDocID::generate_doc_id`：2 个参数时 `seq = (uint64)rowkey`（仅单列整型主键，其余回落序列）；
- `ObDMLResolver::fill_doc_id_expr_param` 追加 rowkey 列引用作为第二参数。
→ **`ERROR 4019 Size overflow`**（ALTER 在 ~0.34 s 内失败，服务端日志里没有对应的 -4019，说明发生在生成列表达式/解析期路径上）。

**尝试 C（B1 路线：改生成列文本）** → **`ERROR 4016 Internal error`**
- `ObFtsIndexBuilderUtil::generate_doc_id_column` 把生成列文本改成 `DOC_ID(`pk`)`（仅单列整型主键）；
- `ObExprDocID` 支持 2 参数（arity `ZERO_OR_TWO`），2 参数时 `seq = (uint64)rowkey`；
- `ObDMLResolver::fill_doc_id_expr_param` 检测到解析出的 rowkey 参数时，用 `set_param_exprs(tablet_expr, rowkey)` 保留它；
- 另把 rowkey 记为 docid 列的 **cascaded column**（让构建 SQL 一并 select 出来）。
→ 构建能跑起来（耗时 7.8 s，说明 aux 构建确实执行了），但最终报 4016；服务端日志里没有对应的错误明细。
**疑似根因**：`fill_doc_id_expr_param` 里对 schema 解析出的 rowkey 参数做 `CopySchemaExpr` 复制，
得到的列引用不在 stmt 的 table item/计划列集合里 → 求值时 4016。正确做法应像 `part_expr` 那样
**从 stmt 的 table item 构造列引用**（`ColumnItem`/`get_column_expr`），而不是复制 schema 表达式。

**结论**：要改 docid 语义，不能走"DML 解析期给 DOC_ID 加参数"这条路（会踩生成列的表达式校验/长度路径）。
**尝试 D（B1 修正版）**：把 rowkey 列引用改为从 stmt 的 column item 取（`get_column_expr_by_id`），
仍报 **4016**，且这次在 ~0.3 s 内失败 ⇒ 失败点在 **CREATE INDEX / 建 aux schema**，不是构建 SQL。

**决定性证据（`oceanbase.__all_column` 元数据）**：docid 生成列在**所有表里是同一列（column_id 18）**：

| 表 | column_id 16 | 列 18（docid） | 列 19..21 |
|---|---|---|---|
| 数据表 `t` (500003) | `id` | `__doc_id_...` flags=4194305 | word_segment/word_count/doc_length |
| rowkey_doc (500006) | `id` | `__doc_id_...` flags=4325376 | 无 |
| fts_index (500008) | **无** | `__doc_id_...` flags=4194304 | word_segment/word_count/doc_length |
| doc_word (500009) | **无** | `__doc_id_...` flags=4194304 | 同 fts_index |

即：**docid 是数据表与三张 aux 表共享同一个 column_id 的生成列**，而两张分词表 aux **没有 rowkey 列**。
把生成列表达式改成引用 rowkey（`DOC_ID(`id`)`）会让 aux 表上的列依赖指向不存在的列 ⇒ 建 aux schema 直接失败（4016）。

**结论（架构级）**：在"不改 aux 表 schema"的约束下，**docid 无法做到位置无关**——
共享生成列要求表达式只能依赖两张分词表也有的列（即无依赖）。因此：

- 任何"并行扫描 + 序列 docid"都会破坏四张表之间的 docid 一致性；
- 任何"位置无关 docid"都要改 aux 表的 docid 列为普通列（= 改 aux schema）；
- 想拿 27%，只能在放开该约束后做（把 aux 的 docid 改成普通列 + 扫描期按切片幂等生成），
  或保留现状。

已试四条（1062 / 4019 / 4016 / 4016），可行的两条：
1. **改生成列文本**：aux 表创建时把数据表的 docid 生成列定义为 `DOC_ID(<pk_col>)`
   （`ObFtsIndexBuilderUtil::generate_doc_id_column` 里目前硬编码 `"DOC_ID()"`），
   解析器天然得到 2 个参数；需要处理已存在该生成列（`col_exists`）的表（重建索引时列已存在，文本不能改）。
2. **不动 docid**：把四张 aux 表统一到同一套扫描切片（映射表也从 2 个 worker 改为同一切片），
   即 P1+P2 先行；docid 仍用序列但保证四张表的行消费顺序一致。

### 2.4.8 放开"aux docid 列可为普通列"约束后的实施结果（B1 完整路线）

放开约束后按四步实施，逐步实测：

| 步骤 | 改动 | 结果 |
|---|---|---|
| S1 | **aux/index 的 docid 列改为普通列**（`ObIndexBuilderUtil::del_column_flags_and_default_value` 对 `is_doc_id_column()` 清 flag+默认值） | ✅ **构建成功**（单独开启即可用，不报错） |
| S2 | 构建 SQL 里 rowkey_doc 的插入列表**补上 docid 列**（`generate_build_replica_sql` 里去掉 `is_doc_id_column` 的跳过） | ✅ 生成的 SQL 变成 `INTO ... (id, __doc_id_...) SELECT id, __doc_id_ ...`（已在日志确认） |
| S3 | 数据表 docid 生成列表达式改为 **`DOC_ID(pk)`** + rowkey 记为 cascaded 列 | ❌ 仍 **`ERROR 4016`** |
| S4 | 打开 docid 生成列的依赖解析（`cascaded_generated_column` 去掉 `!is_doc_id_column()`） | ❌ 仍 **4016** |

失败点判定：S1+S2 下 aux 构建 SQL 已经带上 docid（日志可见），但一旦数据表的 docid 表达式引用 rowkey，
构建 SQL 的**计划阶段**就失败（探针 `DOCID_CG_PROBE`/`DOCID_EVAL_PROBE` 均未触发 ⇒ 未进入执行）。
即：**PDML insert-select 计划里对 docid 生成列的求值尚不支持列参数**，需要改
`ObDMLResolver` 的生成列求值路径 / `ObOptimizer` 对该生成列的列投影，而不是 schema 层（schema 已能建出来）。

**结论**：27% 的剩余工作已经收敛成"让 PDML 计划能求值带 rowkey 参数的 docid 生成列"这一件事：
S1、S2 已验证可用，S3 的 schema 文本也已被接受，卡点仅在计划阶段（4016）。所有改动已回退，工作区回到已验证基线。

### 2.4.9 最终结论：27% 的两条可行路径与各自的硬约束

**已证实的价值**：4 分区（每 tablet 一个扫描线程）比非分区快 **27%**（1.86 s vs 2.44~2.54 s，命中一致）。
要让非分区表拿到同样的并行度，必须让"一次 ALTER 的四张 aux 表"在**同一套数据表切片**下并行扫描，
而 docid 必须在四个构建之间保持一致。围绕 docid 已做 6 次实现尝试，结论收敛为：

| 路径 | 内容 | 硬约束 |
|---|---|---|
| **P-A：docid 位置无关**（`seq = f(rowkey)`） | aux docid 改普通列（✅ 已验证可用）+ 构建 SQL 补 docid 插入列（✅ 已验证可用）+ 数据表生成列 `DOC_ID(pk)` | ❌ 计划期 4016：**fts 扫描模式的 CG 只接受 docid 表达式的固定形态**（探针确认未进入执行）；要放开需改 `ObTscCgService` 的 docid 特化 + `ObDMLResolver` 的生成列求值 |
| **P-B：切片幂等 docid**（`f(K, slice, row_in_slice)`） | 扫描期设置切片参数（`ObExecContext::set_ddl_idempotent_autoinc_params` 已具备）+ K 段扫描切片 | ❌ 映射表的 docid 与分词表**生成位置不同**（映射表在插入算子、分词表在扫描期），必须先把映射表也改成扫描期生成，且四张表共用同一切片 |

两条路径都各有一步"必须改造 SQL 引擎内部"的工作（P-A 的计划期 docid 求值 / P-B 的映射表 docid 前移），
都不是存储层或 DDL 状态机的改动。**在此之前，构建侧已无可落地的 >5% 优化**：
压缩参数、宏块快路径、`enable_newsort`、重复分词合并（前端已并行）均已实测关闭。

### 2.5 对合并方案的影响（实测后的修正）

1. **省一次分词 = 省 0.49 CPU-s ≈ 4.6 CPU-s 的 ~11% ⇒ 墙钟 ~0.22 s ≈ 9%**；
   但合并把两路外发压到同一扫描线程，前端从 1.15 s（两线程并行）变成 ~1.8 s（单线程），
   **净收益被抵掉，落在 0~5%**。这与设计文档第 9 条"收益 <5% 停止"的门槛正面冲突。
2. 原设计认为"分词占构建 33%"，实测是**占单表流水线 21% / 占 ALTER 墙钟 20%**，
   且扫描外发与排序才是主导项。
3. 若仍要拿到 >10%，必须同时满足：
   (a) 扫描前端并行（≥2 个 scan worker）；
   (b) 第二路排序被消除——但 (b) 与 docid 的字节序编码冲突（§0 发现 2），
       除非引入"按 docid 排序文档"的新算子（20k 个 16 字节键的排序，代价远小于 693k 行的排序），
   这两项都不在原设计范围内。

---

## 3. 目标架构

### 3.1 方案 A（推荐）：一个计划、一次分词、两路有序流、两个直载会话

```
                    data table scan（一次）
                     └─ 分词 + 行生成（一次）→ (index_row, word_row) 成对
                          ├─ 分支 A：STAT COLLECTOR(A 键) → RANGE SHUFFLE(A 切点) → SORT(word,docid) → INSERT(fts_index)
                          └─ 分支 B：STAT COLLECTOR(B 键) → RANGE SHUFFLE(B 切点) → SORT(docid,word) → INSERT(fts_doc_word)
两个 INSERT 各持一个直载会话：
  A 会话：ddl_task_id = fts_index 子任务 id, table_id = fts_index aux
  B 会话：ddl_task_id = fts_doc_word 子任务 id, table_id = fts_doc_word aux
```

要点：

1. **扫描/分词只发生一次**：`ObTableScanOp` 的 fts 模式增加 dual 输出（A6/A7），产出成对行；
   两条分支由 `MATERIAL`（chunk store，必要时落地）扇出，避免二次分词。
2. **两路排序各自采样、各自持久化**（A4）：`get_or_insert_schedule_info` 需要接受"第二个 task id 的第二套 part_ranges"，
   否则 B 分支的切片无处落库，B 的 DAG 建不出 tablet context（A5）。
3. **两个直载会话互不干扰**（A9/A10）：B 会话的 `table_id`/participants 通过
   `loc_meta.related_table_ids_`（A10）从同一个 SQC 的 table loc 解析，**不需要新造 tablet 映射逻辑**。
4. **DDL 归属**（A12）：`fts_doc_word` 子任务保留自己的 task id、自己的 complement/checksum/enable_index，
   只是**不再执行自己的 INSERT**；它的 `REDEFINITION` 阶段改为"等待合并构建完成"。

### 3.2 方案 B（备选）：单计划 + 多表 DAG

把 `ObDDLIndependentDag` 扩成"一个 DAG 持有 N 张表的 schema + N 套 tablet context"，
writer 请求带 `table_id` 以选择 context，merge/replay/checksum 按表区分。
优点：不需要跨任务握手；缺点：改动面覆盖 `storage/ddl` 的建表初始化、macro block 写、redo replay、
`ObDDLTaskParam.target_table_id_`（单值）等，回归风险显著高于方案 A。**仅在方案 A 的跨任务握手被证明不可行时采用。**

### 3.3 明确不做

- 不改两张 aux 表的 schema / 列序 / PK（A7 的置换关系必须由 M1 的用例锁住）；
- 不改源扫描语义、快照、事务可见性；
- 不做"doc_word 从 fts_index 读"（原设计备选方案）：仍要全量重排 960k 行，实测预期与分词同量级，收益≈0；
- 不跳过 LOB 读、不减少文档/token、不降低 flush。

---

## 4. 里程碑

每个里程碑都必须满足：**可独立编译、可独立验证、可独立回退**。任何一步验收不达标即回退该步，不带中间状态继续。

### M0 测量门槛（预计 0.5~1 天）——**先做，结果决定是否继续**

**目标**：把 `T_before` 拆成 `scan / segment / sort / write` 四项，按表、按 worker 记录。
**判定**：`sort_2nd = 排序成本`；若 `segment < sort/3` 且 `segment < T_before × 8%` ⇒ **停止**（按原设计第 9 条）。

**改动**（只加临时计时，不改变行为）：

- `ObDASDomainUtils::generate_fulltext_word_rows`（`ob_das_domain_utils.cpp:306`）：对
  `segment_and_calc_word_count` 与"行分配+填充"两段分别计时；
- `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row`（`ob_px_sstable_insert_op.cpp:461`）：
  记录 `loop_total / first_row_done / append_total / row_count`。

**两个必须避开的坑（本项目已实测踩到）**：

1. **不要用 `thread_local` 计数器把扫描侧和插入侧串起来**：扫描/分词跑在 PX worker 线程，
   `write_ordered_slice_by_row` 的循环跑在根 DFO 线程，`thread_local` 读出来恒为 0。
   用**进程级原子量**，或在扫描算子上加成员变量再在算子结束时打日志。
2. **二分法**：`first_row_done` 在一次 **blocking sort** 里包含"整段扫描+分词+排序"，
   所以 `sort ≈ first_row_done − segment_global`；再用"产出 0 行"的构建（如对中文文本用 `beng`）取一次基线交叉验证。

**输出**：一张表（每表 / 每 worker 的 scan、segment、sort、write、rows），写进报告。

### M1 一次分词产出两套行（预计 1 天）——**已完成**

**目标**：结构等价、零行为变化（默认关闭）。

**落地状态**：`src/sql/das/ob_das_domain_utils.cpp`
- `generate_fulltext_word_rows_dual_impl()`：一次 `segment()`，分配 `2N` 行，前 N 行
  `[word,docid,cnt,len]`、后 N 行 `[docid,word,cnt,len]`，两组行按下标一一对应；
- `FTS_DUAL_ROWS_CHECK=1` 自检：在同一文档上重跑 dual 生成，与单表路径逐字段比对，
  日志 `FTS_DUAL_ROWS_CHECK(checked, mismatch, skipped)`；默认关闭、不影响构建结果；
- 验证：
  1. 行级：3000 文档 ×（fts_index + fts_doc_word）两套行逐字段比对 **mismatch=0**；
  2. 端到端：20k 文档完整基准（`_enable_fts_dual_writer` 尚未引入，即现状路径）命中数
     `8001 / 11000 / 7332 / 20` 与改动前完全一致；36 形态 correctness dump 与参考比对
     **仅 `order_rel` 并列组不同**（设计允许的唯一差异），其余逐字节一致；
  3. 构建耗时同量级：`build_ik_all 2.951` / `build_ik_content 2.554` / `build_beng_en 1.442` s。
- 说明：函数目前是文件内 static（避免改 `ob_das_domain_utils.h` 触发全量重编），
  M2 接入扫描算子时再提升为 `ObDASDomainUtils::generate_fulltext_word_rows_dual`。
- **刻意没做**：把单个文档的 word 按词序排序输出。M0 之后确认这样做无收益（§0 发现 2）。

- `src/sql/das/ob_das_domain_utils.h/.cpp`：新增

```cpp
// 一次 segment，填充两组行；两套行共享同一份 word 字符串（同一 allocator 生命周期）。
static int generate_fulltext_word_rows_dual(common::ObIAllocator &allocator,
                                            storage::ObFTParseHelper *helper,
                                            const common::ObObjMeta &ft_obj_meta,
                                            const ObDatum &doc_id_datum,
                                            const ObString &fulltext,
                                            ObDomainIndexRow &index_rows,
                                            ObDomainIndexRow &word_rows);
```

- 实现只做：`segment_and_calc_word_count` 一次 → `rows_buf` 分配 `2×N` → 第一个 `N` 按
  `[word,docid,cnt,len]`、后 `N` 按 `[docid,word,cnt,len]` 填（A7）。
  保留现有单表函数供非合并路径使用（可回退）。
- `ObFTIndexRowCache`（`ob_das_domain_utils.h:35`）：加 `dual_` 模式，`segment()` 同时产出两组行，
  `get_next_row()` 改为 `get_next_row_pair(index_row, word_row)`（两组行**一一对应**，第 i 个词在两套里的下标相同）。
- 用例：新增 `tools/deploy/mysql_test/test_suite/fts_index/t/` 之外的**单测**（仓库 `unittest/` 目录），
  对同一批文本比较"单表路径 vs dual 路径"的 4 列取值逐字段一致；再补一个形态用例锁定列序。

**验收**：全量 `correctness_dump` 与改动前逐字节一致（此时还未接写入，应完全无差异）。

### M2 计划层双目标（预计 2~3 天）

**目标**：让一个 DML 插入计划携带"第二目标表"，但先只做 **A 表写入 + B 表空转**（验证管线打通）。

- `ObPxMultiPartSSTableInsertSpec`（`ob_px_sstable_insert_op.h:44`）：新增
  `dual_enabled_`、`dual_table_id_`、`dual_task_id_`、`dual_tablet_id_expr_`（B 表分片 expr）、
  `dual_row_desc_`（B 表分区槽位）。序列化宏同步（`OB_SERIALIZE_MEMBER`）。
- 注入方式（不碰 SQL 文本解析）：在 `generate_build_replica_sql`（`ob_ddl_common_rs_impl.cpp:310`）
  里追加 `opt_param('fts_dual_table_id', X) opt_param('fts_dual_task_id', Y)`；CG 侧读 plan 的 opt_param 填 spec。
- B 表分片解析首选 **`ObExprCalcPartitionId`**（`src/sql/engine/expr/ob_expr_calc_partition_id.h`，已支持
  `related_table_ids_`）为 B 表生成一个计算分片 tablet id 的输出 expr；备选是插入算子内部用
  `tablet_id → ObDASTabletLoc → get_related_tablet_loc(*loc, dual_table_id)` 解析。
- `ObDASCtx` / loc meta：把 `dual_table_id` 加进插入表 loc 的 `related_table_ids_`
  （填充点参考 `ob_dml_cg_service.cpp:2634`），**这样 `ObPxSubCoord::get_participants`（`ob_px_sub_coord.cpp:761`）
  天然就能给出 B 表的 participants**（A10 的复用，务必先做这个验证，可省掉大量新代码）。

**验收**：`EXPLAIN` 能看到 B 表分片 expr；构建成功；`grep` 日志确认 B 表 tablet id 正确；
A 表行数与改动前一致。

> **先决决策（实测后新增）**：M2 之前请先用 §2.4 的数据确认"值得做"。
> 若结论是继续，则 M3 必须按"两路排序"实现；若想省掉第二路排序，需要新算子
> "扫描期物化 + 按 docid 字节序输出"（见 §2.5 第 3 条），那是 M3 之外的额外工作量。

### M3 两路有序流（预计 3~5 天）——**风险最高**

**目标**：B 分支产出按 `(docid, word)` 有序的流，且切片切点按 B 表 PK 采样并落库到 B 的 task id。

- `src/sql/engine/px/datahub/components/ob_dh_sample.cpp:403`：`ObDynamicSamplePieceMsgCtx`
  支持**第二套 sample store / sort_def**，`build_whole_msg` 里对 B 表调用
  `get_or_insert_schedule_info(dual_task_id, ...)`，把 B 的 `part_ranges` 落库（A4）。
- `src/sql/engine/px/ob_ddl_slice_store.h:31` + `ObServer::get_or_insert_schedule_info`（`ob_server.cpp:225`）：
  接口按 task id 区分即可（现有签名已带 task_id，主要是调用侧要传第二个 id）。
- 计划形态：`ObDelUpdLogPlan::create_online_ddl_plan`（`ob_del_upd_log_plan.cpp:952`）
  增加 dual 分支；扇出点用 `MATERIAL`（chunk store，超阈值落地）保证**只分词一次**。
- `ObPxMultiPartSSTableInsertOp`：新增 `write_dual_slice_by_row()`：
  两个 `TabletWriterMap` + **缓存上一行的 writer 指针**（消除每行哈希查找，这是原设计 §3.2(b) 里对的那一半）；
  两个 writer 各自的 `switch_slice_if_need` 用各自的 tablet_id + slice_idx。

**验收**：B 表行数/内容与"独立构建 B 表"逐字节一致（先只用 `INSERT ... SELECT` 写两张临时表验证）；
两份 `part_ranges` 都落库且与独立构建时的切点分布同量级。

### M4 双直载会话（预计 2~3 天）

**目标**：B 表由第二个直载会话写入，数据由 B 自己的 tablet 承载。

- `ObPxSubCoord::start_ddl`（`ob_px_sub_coord.cpp:695`）：启动第二个
  `ObDirectInsertStartParam{ ddl_task_id=dual_task_id, table_id=dual_table_id, participants=B 的 tablets }`；
  `end_ddl` 同步 finish 两个会话。
- `ObDirectInsertStartParam` / `ObDirectInsertSessionImpl` 本身**不需要改**（A11 说它会绑定单表——那就一个表一个会话）。
- 完成顺序（原设计 §3.3 的"同进同退"）：
  - 任一 writer 失败 ⇒ 整个计划失败（`ret` 传播即可，不要吞）；
  - 两个会话的 `complete_px_worker()` 都成功后才算 PX 侧完成；
  - `ObDDLInsertDag` 的 merge/macroblock 各自独立跑，互不共享缓冲。

**验收**：kill -9 后重启，两张表都能正确恢复或一起失败重试；checksum 各自上报。

### M5 DDL 编排 + 回退开关（预计 2~3 天）

**目标**：`fts_doc_word` 子任务不再自己灌数，但仍负责自己的 complement/checksum/enable。

- `ObFtsIndexBuildTask::prepare_aux_index_tables`（`ob_fts_index_build_task.cpp:550`）：
  当 `_enable_fts_dual_writer=1` 时，把 `fts_doc_word` 的 `aux_table_id` / `task_id` 作为参数
  传给 fts_index 子任务（经 `ObIndexSSTableBuildTask` → `ObDDLLocalBuildExecutorParam`（A13）→
  `ObDDLLocalBuildArg`），**不再让它自己执行构建 SQL**。
- `ObIndexBuildTask`（`ob_index_build_task.cpp:247`）：新增"跳过 REDEFINITION 灌数、只等合并构建完成"的路径；
  `VALIDATE_CHECKSUM` / `TAKE_EFFECT` 保持原样（A12）。
- 配置项：`_enable_fts_dual_writer`（默认 **0**，完全等于现状）；放在租户级配置，读取点集中在
  `prepare_aux_index_tables` 与 CG 的 spec 填充处。
- 失败/重试：父任务 `dependent_task_result_map_` 仍登记 4 个 task id；合并任务失败 ⇒ 两张表一起重做。

**验收**：开关 0/1 两种模式都能建索引成功，且 0 模式的构建耗时/结果与改动前逐字节一致。

### M6 验收（预计 1~2 天）

见 §6。

---

## 5. 关键代码草案

### 5.1 dual 行生成（M1）

```cpp
// ob_das_domain_utils.cpp
/*static*/ int ObDASDomainUtils::generate_fulltext_word_rows_dual(
    common::ObIAllocator &allocator, storage::ObFTParseHelper *helper,
    const common::ObObjMeta &ft_obj_meta, const ObDatum &doc_id_datum,
    const ObString &fulltext,
    ObDomainIndexRow &index_rows, ObDomainIndexRow &word_rows)
{
  int ret = OB_SUCCESS;
  int64_t doc_length = 0;
  ObFTWordMap word_map;
  void *rows_buf = nullptr;
  blocksstable::ObDatumRow *rows = nullptr;
  const int64_t bkt = MIN(MAX(fulltext.length() / 10, 2), 997);
  if (0 == fulltext.length()) {
    ret = OB_ITER_END;                             // 与单表路径一致
  } else if (OB_FAIL(word_map.create(bkt, common::ObMemAttr("FTWordMap")))) {
  } else if (OB_FAIL(segment_and_calc_word_count(allocator, helper, ft_obj_meta,
                                                 fulltext, doc_length, word_map, false))) {
  } else if (0 == word_map.size()) {
    ret = OB_ITER_END;
  } else if (OB_ISNULL(rows_buf = static_cast<char *>(
               allocator.alloc(2 * word_map.size() * sizeof(blocksstable::ObDatumRow))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const int64_t n = word_map.size();
    rows = new (rows_buf) blocksstable::ObDatumRow[2 * n];
    int64_t i = 0;
    for (ObFTWordMap::const_iterator it = word_map.begin(); OB_SUCC(ret) && it != word_map.end(); ++it, ++i) {
      blocksstable::ObDatumRow &idx_row = rows[i];      // [word, docid, cnt, len]
      blocksstable::ObDatumRow &word_row = rows[n + i]; // [docid, word, cnt, len]
      if (OB_FAIL(idx_row.init(allocator, 4)) || OB_FAIL(word_row.init(allocator, 4))) {
      } else {
        idx_row.storage_datums_[0].set_datum(it->first.get_word());
        idx_row.storage_datums_[1].shallow_copy_from_datum(doc_id_datum);
        idx_row.storage_datums_[2].set_uint(it->second);
        idx_row.storage_datums_[3].set_uint(doc_length);
        word_row.storage_datums_[0].shallow_copy_from_datum(idx_row.storage_datums_[1]);
        word_row.storage_datums_[1].set_datum(it->first.get_word());
        word_row.storage_datums_[2].set_uint(it->second);
        word_row.storage_datums_[3].set_uint(doc_length);
        if (OB_FAIL(index_rows.push_back(&idx_row)) || OB_FAIL(word_rows.push_back(&word_row))) {
        }
      }
    }
    (void)word_map.destroy();
  }
  return ret;
}
```

> 注意：word 字符串由 `word_map` 的 key 借用解析器 token，必须保证其生命周期覆盖两套行的使用期（与单表路径一致：
> 都在同一个 `reusable_word_map` + arena 生命周期内）。M1 的用例要显式覆盖"连续多文档复用同一 word_map"的场景。

### 5.2 spec 扩展（M2）

```cpp
class ObPxMultiPartSSTableInsertSpec : public ObPxMultiPartInsertSpec {
  // ...
  bool dual_enabled_;
  int64_t dual_table_id_;
  int64_t dual_task_id_;
  ObExpr *dual_tablet_id_expr_;  // B 表分片（ObExprCalcPartitionId，related ids 含 dual_table_id）
  ObDatumRowDesc dual_row_desc_; // B 表分区槽位
};
OB_SERIALIZE_MEMBER((ObPxMultiPartSSTableInsertSpec, ObPxMultiPartInsertSpec),
                    snapshot_query_expr_, regenerate_heap_table_pk_,
                    dual_enabled_, dual_table_id_, dual_task_id_, dual_tablet_id_expr_, dual_row_desc_);
```

### 5.3 双 writer 写入（M3/M4）

```cpp
int ObPxMultiPartSSTableInsertOp::write_dual_slice_by_row()
{
  // 两个 writer map；缓存上一行 (tablet_id, slice_idx) → writer，命中即跳过哈希查找
  while (OB_SUCC(ret)) {
    // 从 A 分支取行：get_next_row_from_child() → eval_current_row(datums_a) → append 到 A writer
    // 从 B 分支取行：同一批 datums 重排为 [1,0,2,3] → append 到 B writer
    // （M3-A：两个分支各自 SORT，行数与顺序相互独立，必须各自 switch_slice_if_need）
  }
  // 两个 writer 都 close() 成功 → 两个会话 complete → 上报完成
}
```

> B 分支的 datum 排列**不要**再申请第二组 expr：`ins_ctdef_.new_row_` 的 4 个 expr 就是
> `[word, docid, cnt, len]`，B 表需要的 `[docid, word, cnt, len]` 是同一批 `ObDatum*` 的下标置换
> `{1,0,2,3}`。这是本方案里最省的一大块（避免第二套表达式树 + 二次求值）。

### 5.4 回退开关

```cpp
// 默认 0：完全等价现状（两个独立子任务）。
// 1：启用合并构建。
// 读取点：(1) ObFtsIndexBuildTask::prepare_aux_index_tables；(2) spec 填充（CG）。
```

---

## 6. 验收与测量规范

### 6.1 先决条件：工作区独占 + 二进制来源校验（**本次实测踩过的坑**）

本工作区曾出现**两个 agent 会话同时改同一棵树、共用同一个 `build_release/`** 的情况，后果是：

- `src/storage/fts/ik/ob_ik_char_util.h` 处于半改状态时构建失败；
- 用一个"半改状态编出来的二进制"测得的 FTS 构建**写入 0 行**（对外表现为 `MATCH` 恒 0 命中），
  而不是报错——**会被误判成"索引构建功能坏了"**；
- 用改动前留存的二进制 `base-214443` 复测同一 SQL 立即恢复正常（1 命中、score≈0.35）。

因此任何性能/正确性结论都必须先过**来源校验**：

```bash
# 1) 记录二进制指纹，并确认它就是你要测的那棵树编出来的
md5sum build_release/src/observer/seekdb
git rev-parse HEAD; git status --short        # 期望：只有你预期内的改动

# 2) 单实例、独占端口、独占 base-dir；启动后先确认"连到的是新实例"
mysql -P$PORT -e "select 1" >/dev/null && \
  ! mysql -P$PORT -e "use <bench_db>" 2>/dev/null   # 新实例里不应已存在 bench 库

# 3) 每次构建前跑一次冒烟：建索引后 MATCH 必须命中（0 命中即二进制可疑，立刻停）
```

### 6.2 性能判定

- 同一台机器、**两个二进制交替 A/B**，按块配对取中位；判定门槛：构建 >2.5%。
- 不要用"单次跑对固定基线"（本机 7~10% 漂移，报告 §11）。
- 必测两种配置：`_enable_fts_dual_writer=0/1`，以及 DOP 的 1/2/4 三档（M3-B 需要更高 DOP 补偿）。
- 记录：`build_ik_all` / `build_ik_content` / `build_beng_en`（含离群，报告 §9 的 clog 抖动与本改动无关但与测量有关）。

### 6.3 正确性

| 项 | 方法 |
|---|---|
| 行内容等价 | M1 单测：单表路径 vs dual 路径逐字段一致；两表行数一致 |
| 36 形态 dump | `.tmpdev/correctness_dump.sh`，与改动前逐字节比对（唯一允许差异：`order_rel` 并列组选取） |
| 仓库用例 | `fts_index.simple_query`、`no_score_match`、`tokenize_edge`、`word_map_reuse`（本地 mysqltest 需 `--no-defaults --skip-ssl`，含 `--result_format 4` 的用例要剥离该指令并用 record 模式） |
| DDL 专项 14 项 | 建索引 / 分区表建索引 / 双索引并存 / online DDL 加列 / 删索引 / 重建索引 / `SELECT id`（docid→rowkey 映射） |
| 健壮性 | kill -9 后重启，DDL 能恢复或失败重试；开关 0/1 两种模式都能构建 |
| checksum | 两张表各自上报；合并任务失败时两表一起重做，不允许"一表成功一表失败" |

---

## 7. 风险表

| 风险 | 触发点 | 缓解 |
|---|---|---|
| 第二路排序吃掉全部收益 | M0 实测 sort 与 segment 同量级 | M0 先测；若 `segment < sort/3` 且 `segment < T_before×8%` 直接停止 |
| 并行度下降导致墙钟变差 | M3-B 单 DFO 串行 | 优先 M3-A 两分支并发；否则提高 DOP 并实测 |
| 两份 slice info 混淆 | `get_or_insert_schedule_info` 按 task id 键 | 两个 task id 严格区分；A/B 分支的 `part_ranges` 分别断言 tablet 集合不重叠 |
| 两表数据不一致 | 一个 writer 成功、另一个失败 | 计划内 `ret` 不吞；两会话都 complete 才上报；失败整任务重试 |
| DDL 状态语义 | 恢复/重试路径依赖状态枚举 | 只新增状态（如 `WAIT_DUAL_BUILD`），不改既有枚举含义 |
| 多表 DAG 改造回归 | 采用方案 B | 默认走方案 A；方案 B 作为独立实验分支，不进主线 |
| 测量被环境/并发污染 | 见 §6.1 | 独占工作区 + 二进制指纹 + 建索引后 MATCH 冒烟 |

---

## 8. 与原设计的逐条差异

| 原设计 | 修订 |
|---|---|
| §3.2(a) 新增 `generate_fulltext_word_rows_dual`，两组行共享 word 内存 | **保留**，并给出可直接落地的草案（§5.1） |
| §3.2(b) "两个 writer 各自维护 sorter，天然满足" | **改为**：两路排序在计划内（`STAT COLLECTOR` + `SORT`），需要**第二套采样与 part_ranges**；writer 侧只做"缓存 writer 指针"这一项 |
| §3.2(b) "在 spec 里增加 `dual_output_` 第二组输出 expr" | **改为**：不需要第二组 expr，B 表列序是 A 表的一组 `ObDatum*` 下标置换 `{1,0,2,3}`（A7） |
| §3.2(c) "由合并任务同时上报两张表的 completion" | **改为**：`fts_doc_word` 保留自己的 task id 与 complement/checksum/enable，只是不再执行自己的 INSERT；避免"一个任务上报两张表的 completion"这种跨表语义 |
| §3.3 "两个 writer 各自维护缓冲与 sorter" | 同上，改为两路排序 + 两套切片信息 |
| §5 风险"并行度下降" | 具体化为 M3-A / M3-B 两种架构，并要求把 DOP 作为验收变量 |
| §7 提示词"分三步" | 拆成 M0~M6（M0 是新增的测量门槛；M2/M3/M4 把"一个 PX 算子双 writer"拆成计划层、排序层、会话层三段分别验收） |

---

## 9. 停止规则与工作量

- **停止规则（原设计第 9 条，保留并前置到 M0）**：若 M0 实测 `segment` 占构建 <8%，或第二路排序使净收益 <5%，
  **立即停止**并把 M0 数据写进报告，不再引入复杂度。
- 工作量估计（单人有经验工程师）：M0 0.5~1 天、M1 1 天、M2 2~3 天、M3 3~5 天、M4 2~3 天、M5 2~3 天、M6 1~2 天，
  合计 **12~18 人日**。其中 M3 的不确定性最大（PX 计划扇出 + 第二套采样）。
- 若时间预算不足以覆盖 M3/M4，建议**只交 M0 的测量结论 + 本计划**，不要交半成品：
  dual 行生成（M1）单独存在时不产生任何收益，却会扩大后续改动的回归面。
