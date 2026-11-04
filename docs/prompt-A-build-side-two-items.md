# 提示词 A：构建侧两项优化（⑥ 映射表合并 + DDL 固定开销）

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
>
> 本文件含**两个子任务**，必须先做 A（映射表合并）再做 B（DDL 固定开销），
> 每步有独立验收门，不达标就回退并进入下一步。

## 0. 当前基线（实测，3 次中位，`select1`=0.0224~0.0254）

```
build_ik_all      2.848 s      （基线 3.754，+24.1%）
build_ik_content  2.434 s      （基线 2.954，+17.6%）
build_beng_en     1.346 s      （基线 1.640，+17.9%）
```

**目标**：三项各再降 **≥5%**（合计约 +8 个百分点到 build 类别均值）。

平台背景（重要）：平台 build 是本机的 **1.8 倍**（5.13 / 4.38 / 2.42 s），
而 tokenize/query 与本机同速 ⇒ 平台构建受**总工作量/存储**限制，不是并行度不足。
但注意下面的教训：**盲目合并数据通路会牺牲并行度**（见 §A2 的历史结论）。

---

# 子任务 A：⑥ 两张映射表合并构建

## A1. 依据

一次 FTS 索引构建有 4 张 aux 表：

| aux 表 | 内容 | 构建方式 |
|---|---|---|
| `rowkey_doc`（ROWKEY_DOC_ID） | rowkey → docid | `ObFtsIndexBuildTask::prepare_rowkey_doc_table()` 单独提交一个 DDL 子任务 |
| `doc_rowkey`（DOC_ID_ROWKEY） | docid → rowkey | 在 `prepare_aux_index_tables()` 里与另两张一起提交 |
| `fts_index` | `[word, docid, cnt, len]` | 同上，需分词 |
| `fts_doc_word` | `[docid, word, cnt, len]` | 同上，需分词 |

两张映射表是**同一份 `(rowkey, docid)` 数据的正反方向**，各自对源表做一次扫描 + 排序 + 直载写入，
并各占一个 DDL 子任务的固定开销（建 tablet / 等待 / 收尾）。行数很小（20k 文档即 20k 行/表），
所以**它们的数据量不是瓶颈，固定开销才是**。

## A2. 历史结论（务必先读，避免重蹈覆辙）

上一轮做过"两张分词表（`fts_index` + `fts_doc_word`）合并为一次扫描"的尝试
（`_enable_fts_token_share`），结论是**净损失**：4 对交替实测 on 比 off **慢 7.1%**
（3.913 s vs 4.191 s），原因是**原来两张表各自一个扫描线程并行分词，合并后变成一个线程分词 + 两个 writer**，
在 2~4 vCPU 租户上牺牲了并行度。该实现与两个参数已被删除（`ob_das_domain_utils.cpp` 净减 822 行）。

⇒ 因此本子任务**只合并两张"无分词、行数很小"的映射表**，不要碰两张分词表。

## A3. 两条可行路线（先做 30 分钟评估，选其一）

**路线 1（推荐先试）：从已建好的映射表派生另一张**
- 两表是互逆映射。可以先建 `rowkey_doc`（按 rowkey 排序），再用
  `INSERT INTO doc_rowkey SELECT docid, rowkey FROM rowkey_doc ORDER BY docid` 派生 `doc_rowkey`。
- 好处：省掉一次**源表扫描**（源表有 20k × 483B 的 LOB，扫描比读 aux 表贵）；
  派生只需对 20k 行做一次排序（廉价）。
- 注意：`doc_rowkey` 仍需按其 PK 排序，直载要求不变。

**路线 2：一次扫描写两张映射表**
- 复用一个"双 writer"的直载路径（参考已删除的 `_enable_fts_token_share` 实现思路，
  但**只用于映射表**，不改分词表的并行结构）。
- 风险：与 A2 的教训相同（一个扫描线程喂两个 writer），但因为映射表不涉及分词，
  扫描本身很便宜，固定开销的节省可能超过并行损失 —— **必须实测**。

## A4. 验收门
- build 三项各降 **≥4%**（3 块交替取中位；`build_ik_all` 偶发 9~16s 离群，该块作废重跑）；
- **`SELECT id` 的映射正确性必须单独验证**（它依赖这两张表）：
  同一批数据下 `SELECT id FROM t WHERE MATCH(...)` 的结果集合改动前后一致；
- 4 个仓库用例 + DDL 专项 14 项全过；命中数 8001/11000/7332/20 不变。

---

# 子任务 B：DDL 固定开销

## B1. 依据与待测问题

实测：**1 行数据的 `ik` 建索引也要 0.55 s**（纯固定成本，与数据量无关）。
DDL 阶段实测（20k 文档，`ob_fts_index_build_task.cpp` 的状态机）：

| 阶段 | 耗时 |
|---|---|
| `PREPARE` | ~6 ms |
| `GENERATE_ROWKEY_DOC_SCHEMA` | ~11 ms |
| `LOAD_DICTIONARY` | 13~17 ms |
| `GENERATE_DOC_AUX_SCHEMA` | 37~52 ms |
| `WAIT_AUX_TABLE_COMPLEMENT` | 主体构建时间（数据相关） |
| `TAKE_EFFECT` | 15~21 ms |

这些状态合计只有 ~90 ms，**远小于 0.55 s 的实测地板** ⇒ 剩下 ~0.45 s 在别处
（每张 aux 表的 PX 计划建立、sstable/macro block 初始化、checksum、4 张表各自的收尾）。
**第一步必须把这 0.45 s 拆开**，否则会像上一轮那样改错地方。

## B2. 第一步：量准固定成本
在 1 行数据的表上建 `ik` 索引，用临时计时（`OB_FTS_PROF` 门控）分别测：
1. 每张 aux 表从"任务开始"到"任务完成"的墙钟（4 张表分别计时，判断是否串行/并行）；
2. `TAKE_EFFECT` 里各步骤（`enable_index`、`update_index_status_in_schema`、`release_ddl_kvs`）；
3. PX 计划的建立与关闭（`ob_px_sstable_insert_op` 的 open/close）；
4. checksum 上报（`report_ddl_column_checksum` / `verify_checksum`）。

## B3. 按数据选一条（只做最大的一项）
- **若收尾（TAKE_EFFECT / schema 更新）占主导**：把 4 张表的 schema 更新/`enable_index`
  合并成**一次 schema 变更**（注意：只改状态机里"收尾"的组织方式，不动数据通路，
  因此不会重蹈 A2 的并行损失）；
- **若每张表的 PX 计划建立占主导**：评估能否把 4 个 PX 计划的**协调开销**合并
  （例如共用一次 coordinator 启动），但要保证 4 个构建仍然**并行**；
- **若 checksum 占主导**：**不要跳过**（属于正确性校验，赛题禁止跳过必要处理），
  只能评估能否合并为一次上报。

## B4. 验收门
- build 三项各降 **≥5%**（3 块交替取中位）；
- `TAKE_EFFECT` / schema 相关改动必须验证：建索引后索引立即可查（`SELECT ... MATCH` 返回正确结果）、
  `ALTER TABLE ... DROP INDEX` 正常、online DDL 加列后索引仍可用；
- DDL 专项 14 项全过；4 个仓库用例全过；命中数不变。

---

# 全局纪律（每步都遵守）

1. **构建**：`TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12`
   （不带 TMPDIR 时 Rust 组件会因沙箱只读临时目录而失败）；
2. **磁盘**：实例启动要求可用 >10GB，低于阈值报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`；
3. **不要 `pkill -f seekdb`**（会杀掉沙箱包装进程）；停实例用 `sdb.py stop --base-dir <dir>`；
4. **不要 `git checkout/stash`**（`.git` 只读），对比改动用 `cp` 备份；
5. **测量**：3 块交替取中位（`TMPDIR=/tmp bash .tmpdev/ab_bins.sh <旧> <新> 3`）；
   `build_ik_all` 偶发 9~16s 离群（clog 提交拥塞，与代码无关）→ 该块作废重跑；
   宿主负载会让单次 build 在 2.4~4.5s 乱跳，**禁止**用单次跑 vs 固定基线判定；
6. **正确性四关**：36 形态 dump（仅 `order_rel` 允许差异）、5 个仓库用例
   （`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`）、
   DDL 专项 14 项、命中数 8001/11000/7332/20；
7. **单步收益 <4~5% 就回退**，不要叠加复杂度。

# 交付要求

报告按子任务 A/B 分节：
1. 选了什么路线/哪一项、为什么（要有测量依据）；
2. 改了哪些文件、每处作用；
3. 3 块交替 A/B 的 build 三项数据（中位 + 每块原始值 + 标注离群块）；
4. 正确性证据（`SELECT id` 结果集合对比、DDL 14 项、5 个用例、命中数）；
5. 未完成部分与原因；本地数字与平台分数的区分。
