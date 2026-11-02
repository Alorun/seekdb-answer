# 设计：FTS 构建的双 writer 合并（一次扫描 + 一次分词喂两张 aux 表）

> 目标：FTS 索引构建耗时 **−25~30%**
> 前置实测见 `docs/fts-optimization-report.md`（§3.2 构建成本、§5.1.2 D1 专项 A/B）
>
> **[修订]** 本文件 §3.3 与 §3.2(b) 中"两个 writer 各自维护 sorter，天然满足排序要求"的判断经代码核对**不成立**：
> 直载 writer 不排序，排序在计划内的 `SORT` 算子里，且切片切点按**目标表 PK 序**采样后按 `ddl_task_id` 持久化。
> 两张表 PK 序不同 ⇒ 合并后仍需**第二路全量排序**，收益上限因此从 25~30% 下修到 5~16%（中位 5~10%），
> 且"一次计划写两张表"需要解决第二张表的直载会话/任务归属。请以
> [`docs/fts-dual-writer-plan.md`](fts-dual-writer-plan.md)（修订实施计划，含 M0 测量门槛与证据表）为准。

## 1. 现状与问题

一个全文索引在 seekdb 里由 **4 张 aux 表**承载：

| aux 表 | 内容 | 是否分词 |
|---|---|---|
| `rowkey_doc`（ROWKEY_DOC_ID） | rowkey ↔ docid 映射 | 否 |
| `doc_rowkey`（DOC_ID_ROWKEY） | docid ↔ rowkey 映射 | 否 |
| `fts_index`（domain index aux） | `[word, docid, word_count, doc_len]` 倒排 | **是** |
| `fts_doc_word`（FTS_DOC_WORD） | `[docid, word, word_count, doc_len]` 正排 | **是** |

两张分词表各自被一个独立的 DDL 子任务（`ObIndexBuildTask`）构建，各自的 PX plan 都是
`INSERT INTO aux SELECT ... FROM <data_table>`，因此**同一份源文本被扫描两次、LOB 读两次、IK 分词两次**。

实测（20k 文档、`ik(content)`，见报告 §3.2 与 §5.1.2）：

- 4 张 aux 表并发构建的窗口约 **2.45 s**，是构建的主要耗时（其余 DDL 阶段合计 ~90 ms，D1 已把 rowkey_doc 的等待隐藏掉）。
- 其中重复分词约占构建时间的 **33%**（两次 ~20 µs/doc 的分词 × 20k 文档 ≈ 0.8 s）。
- 构建期 CPU/wall 只有 2.2~2.5（租户 4 vCPU），说明瓶颈在**每行/每文档的串行工作**而不是算力，因此"少做一遍分词"可以直接转化为墙钟收益。

## 2. 目标与边界

**目标**：把 `fts_index` 与 `fts_doc_word` 两张表的写入合并到一次源扫描 + 一次分词中，构建 −25~30%。

**边界（不做）**：

- 不改 `rowkey_doc` / `doc_rowkey` 两张映射表的构建方式；
- 不改倒排/正排表的 schema 与行格式（列顺序、列类型、PK 定义都不变）；
- 不改 DDL 状态机的持久化状态枚举含义（可以新增状态，但不能改变已有状态的语义）；
- 不改源数据扫描语义、快照、事务可见性、PX worker 隔离。

## 3. 方案设计

### 3.1 总体思路

把"两个独立 PX 直载计划"改成"**一个 PX 直载计划 + 两个 writer**"：

```
源表扫描（一次）
  → LOB 读一次
  → 分词一次（ObFTParseHelper::segment → ObFTWordMap）
  → 行生成一次，产出两组行：
        A: [word, docid, cnt, len]  → fts_index  writer
        B: [docid, word, cnt, len]  → doc_word   writer
  → 各自排序（按各自目标表 PK）→ 各自直载提交
```

### 3.2 关键改动点

**(a) 行生成：一次分词产出两组行**

`src/sql/das/ob_das_domain_utils.cpp` 里 `generate_fulltext_word_rows()` 已经按 `is_fts_index_aux`
决定列顺序（`word_idx = is_fts_index_aux ? 0 : 1`）。改造方式：

- 新增 `generate_fulltext_word_rows_dual(..., ObDomainIndexRow &index_rows, ObDomainIndexRow &word_rows)`：
  一次 `segment()` 得到 `ObFTWordMap`，然后**同时**填充两组 `ObDatumRow`；
- 两组行共享同一份 word 字符串内存（同一 allocator 生命周期内），避免二次拷贝；
- 现有单表版本保留，供非合并路径使用（保证可回退）。

**(b) 写入：一个 PX 算子持有两个直载 writer**

`src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp` 当前逐行路径：

```cpp
get_next_row_from_child(&cnt);          // 含分词与行生成
eval_current_row(datums);
get_tablet_info_from_row(...);
get_or_create_heap_writer(tablet_id, slice_writer);   // 每行一次哈希查找
slice_writer->append_row(...);
```

合并版本需要一个"行对"接口：child 每次返回**一组行对**（A 行 + B 行），算子按行对分别
`append_row` 到两个 writer：

- 新增 `ObPxMultiPartSSTableInsertOp::write_dual_slice_by_row()`：
  持有 `TabletWriterMap index_writer_map_` 与 `word_writer_map_`（或一个 key 为 (tablet_id, 表类型) 的复合 map，**并缓存上一行的 writer 指针**，消除每行哈希查找）；
- child 输出两组 datum：建议在 `ObPxSSTableInsertOpSpec` 里增加 `dual_output_`（第二组输出 expr 列表）与目标 tablet id 槽位。

**(c) DDL 编排：谁负责构建这两张表**

现状：`ObFtsIndexBuildTask::prepare_aux_index_tables()` 一次提交 3 个 aux 子任务
（`doc_rowkey`、`domain_index_aux`、`fts_doc_word`），各自独立构建。

推荐方案（风险最小）：

1. 新增一种"合并子任务"类型：仍由 `fts_index` 子任务（domain_index_aux）承担，但它的 PX plan
   额外带一个 `fts_doc_word` 的目标 tablet 与其输出列定义；
2. `fts_doc_word` 子任务不再独立提交构建，只保留**建表 + 补数据登记**；
3. 父任务 `dependent_task_result_map_` 仍登记 4 个 task id，但 `fts_doc_word` 的 task 在合并
   构建完成后即标记完成（或直接不登记，由合并任务同时上报两张表的 completion）。

> 备选方案（若上述改动在 DDL 层过于分散）：保留两个子任务，但让 `fts_doc_word` 子任务的
> PX plan 从 `fts_index` 表读（而不是从源表读），从而复用倒排数据。此方案免去分词，但需要对
> 960k 行做一次"列重排 + 重新按 (docid, word) 排序"，实测排序成本与分词相当，**不推荐**。

### 3.3 排序与提交

- 直载要求每个 writer 的输入按其目标表 PK 有序：
  - `fts_index`：(word, docid)
  - `fts_doc_word`：(docid, word)
- 现有 sorter 是 writer 内部的，两个 writer 各自维护自己的缓冲与 sorter，天然满足。
- **提交原子性**：两张表必须在同一个 DDL 任务里"同进同退"。要求：
  - 任一 writer 写入失败 → 整个合并任务失败 → 走既有 DDL 失败/重试路径；
  - `close()` 顺序：两个 writer 都 `close()` 成功后才上报 shared completion；
  - checksum：两张表各自上报（沿用现有 `report_ddl_column_checksum` 机制）。

## 4. 实施步骤（建议顺序）

1. **先做行生成**：新增 `generate_fulltext_word_rows_dual()`，用单测/`correctness_dump` 证明
   两组行与现有单表路径逐字节一致。
2. **再做 writer 合并**：新增 `write_dual_slice_by_row()`（含 writer 指针缓存），先用一个可控的
   SQL 计划（`INSERT ... SELECT` 同时写两张临时表）验证行数/内容一致。
3. **最后接 DDL 编排**：让 `fts_index` 子任务带写 `fts_doc_word`，`fts_doc_word` 子任务不再独立构建。
4. 每步都用验收标准里的等价性检查；任何一步不达标即回退该步。

## 5. 风险与回滚

| 风险 | 说明 | 缓解 |
|---|---|---|
| 两表数据不一致 | 一个 writer 成功、另一个失败 | 合并任务内"同进同退"；失败时整任务重试，两表一起重做 |
| 排序键混用 | 两个 sorter 共用缓冲导致顺序错乱 | 每个 writer 独立的 sorter/buffer，不共享 |
| 并行度下降 | 合并后只有一个 PX plan，可能无法利用原有 2 个 plan 的并发 | 合并 plan 内部仍需按 DOP 切分；必要时对两个 writer 分别设 DOP |
| DDL 状态语义 | 恢复/重试路径依赖状态枚举 | 只新增状态，不改既有状态含义；保留 `fts_doc_word` 独立构建的代码路径作为回退开关（例如配置项 `_enable_fts_dual_writer`） |

**回滚**：配置项置 0 时完全退回现状（两个独立子任务）。提交前必须验证两种模式都能跑通。

## 6. 验收标准

1. **构建耗时**：`build_ik_content` 与 `build_ik_all` 相对现状各降 **≥20%**（同一台机器、
   交替 A/B、3 块取中位；方法论见报告 §5.1）。
2. **正确性**：
   - `correctness_dump`（36 形态）与改动前逐字节一致（唯一允许差异：`order_rel` 的并列组选取）；
   - 新增/既有两个仓库用例 `fts_index.no_score_match`、`fts_index.tokenize_edge` 通过；
   - DDL 专项 14 项（建索引、分区表、online DDL 加列、删索引、重建索引、docid→rowkey 映射）全过；
   - `fts_index` 套件结果与改动前一致。
3. **健壮性**：kill -9 实例后重启，DDL 能正确恢复或失败重试；配置项开/关两种模式都能构建成功。
4. 不修改 `tools/benchmark/*`、`reports/fts/baseline.txt`、评分脚本。

---

## 7. 给执行 agent 的提示词

```
背景
seekdb 的全文索引由 4 张 aux 表承载，其中 fts_index 与 fts_doc_word 两张表各自用独立的 DDL
子任务从源表 INSERT...SELECT 构建，导致同一份文本被扫描两次、IK 分词两次。实测这两张表的并发
构建占一次 ALTER 的 ~2.45s，其中重复分词约占 33%。目标是把它们合并为"一次扫描 + 一次分词 +
两个 writer"，构建耗时降低 20% 以上。

必读
- docs/fts-optimization-report.md（§3.2 构建成本拆解、§5.1.2 D1 专项 A/B、§10 复现命令）
- 本文件 §3 的方案设计
- src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp（write_heap_slice_by_row / write_batch_slice）
- src/sql/das/ob_das_domain_utils.cpp（generate_fulltext_word_rows）
- src/rootserver/ddl_task/ob_fts_index_build_task.cpp（prepare_aux_index_tables / wait_aux_table_complement）
- src/storage/ddl/ob_tablet_slice_writer.cpp（append_row）

要求
1. 分三步落地：① 一次分词产出两组行（保留单表版本）② 一个 PX 算子双 writer 写入（缓存 writer
   指针，避免每行哈希查找）③ DDL 编排让 fts_index 子任务带写 fts_doc_word。
2. 每步独立可验证；任何一步验收不达标就回退该步，不要带着不确定的中间状态继续。
3. 不得改变：源扫描语义、快照/事务可见性、PX worker 隔离、两张表的 schema 与列顺序、排序要求。
4. 不得为了省时间跳过 LOB 读取、减少文档或 token、降低 flush。
5. 提供一个配置项（如 _enable_fts_dual_writer），默认关闭时完全等于现状，用于回退。
6. 编译用 `cmake --build build_release --target seekdb -j12`（机器磁盘紧张，不要开满线程）。
7. 每步完成后必须执行：
   - 等价性：36 形态 correctness dump 与改动前逐字节比对（除 order_rel 并列组）；
   - 仓库用例：mysqltest 跑 fts_index.no_score_match 与 fts_index.tokenize_edge（本地需
     --no-defaults --skip-ssl，且用例含 --result_format 4 时需剥离该指令、用 record 模式）；
   - DDL 专项：建索引 / 分区表建索引 / 双索引并存 / online DDL 加列 / 删索引 / 重建索引 /
     SELECT id（验证 docid→rowkey 映射）；
   - 性能：同一台机器上两个二进制交替 A/B，3 块取中位（不要用单次跑对固定基线）。
8. 最终报告需要包含：改了哪些文件、三步各自完成情况、A/B 数据（含配对中位与 stderr）、
   正确性证据、未完成部分及原因、回退开关的验证结果。
9. 若发现某一步的收益低于 5%，直接停止该步并在报告中说明，不要为小收益继续加复杂度。
```
