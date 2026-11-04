# FTS 构建侧第二轮：⑥ 映射表合并评估 + DDL 固定开销（Agent A 报告）

> 执行对象：`docs/prompt-A-build-side-two-items.md`；仓库 `indextuning`，起点 HEAD `5d07654a4`。
> 并发环境：Agent B（分词侧）在同一工作树改 `ob_expr_tokenize.cpp` / `ft dict` / `ik/*` 等文件；
> 验收用的 clean A/B 是**共享 HEAD 基线** vs 交付二进制（两者都不含 B 的改动，见 §1）。
> 执行时间：2026-11-03 23:27 ~ 次日 00:15（本机 24 核 / 15 GB，`select1` 随宿主负载漂移）。

## 0. 结论（TL;DR）

| 子任务 | 结论 | 验收（3 块交替中位） | 交付代码 |
|---|---|---|---|
| **A/⑥ 两张映射表合并构建** | **不实施（无代码）**：本地墙钟收益上限 0；源表扫描只走主键（不扫 LOB），提示词前提不成立；且第 2/3 次 ALTER 复用映射表，⑥ 对 `build_ik_content`/`build_beng_en` 的收益恒为 0，三指标 ≥4% 门结构上不可达 | — | 无 |
| **B/DDL 固定开销** | **落地（低于门仍保留，orchestrator 决定）**：删除 direct-load 校验和上报后的无条件 `usleep(100ms)`；`wait_ddl_finish` 前 5 s 改为 10 ms 轮询 | **+4.11% / +3.85% / +11.84%**（`ik_all`/`content` 未到 5% 门，`beng` 过门） | 2 文件，+15/−3 |

> **验收口径说明（重要）**：最终 clean A/B（共享 HEAD 基线 vs `bin-A-final`）为
> **`build_ik_all +4.11%`、`build_ik_content +3.85%`、`build_beng_en +11.84%`**，
> 前两项**低于 5% 门**。orchestrator 的决定是**保留**：两处都是纯固定延迟删除
> （不碰数据通路、不跳过 checksum、共 15 行），平台按 1.8x 折算仍有 +2~6%。
> 本文如实记录「未达门但保留」及其风险，不做粉饰。

## 1. 测量环境与口径

- 机器：24 核 / 15 GB，宿主有外部负载（`load average` 2.7~3.3），`build_ik_all` 单次可在 2.4~5 s 漂移；
  因此全部结论来自**同机两二进制、3 块交替、每臂全新实例、取中位**（`.tmpdev/dual/A/ab_bins_A.sh`，复制自
  `.tmpdev/ab_bins.sh`，端口 2893、实例目录在 `.tmpdev/dual/A/`）。
- **污染控制（已修正）**：本会话第一版 A/B 的 control/candidate 构建于 23:37:55 / 23:39:32，
  而 Agent B 把临时 CJK 分词改动回退到 HEAD 是 23:41:43 —— 两次构建时工作树仍夹着 B 的临时分词代码，
  因此那一版 `+7.81% / +5.21% / +10.71%` **不作为验收口径**（已作废）。
- **验收口径**：由 orchestrator 用**共享 HEAD 基线** `.tmpdev/shared/bin-head-baseline.seekdb`
  对交付二进制 `.tmpdev/dual/A/bin-A-final.seekdb` 重跑 3 块 clean A/B
  （artifacts `.tmpdev/ab-1103-235203`、日志 `.tmpdev/dual/A/clean-ab.log`），两个二进制都**不含**
  B 的改动、也不含任何临时计时，所以差异只来自本文 2 个文件。

## 2. 子任务 A/⑥：映射表合并（评估结论：不实施）

### 2.1 先确认前提：映射表是否仍在关键路径外

基线二进制（`bin-head-baseline.seekdb`）在 20k 文档、全新实例上跑官方三次 ALTER，从服务端日志
按 `INSERT ... INTO <aux>` 发送时刻到该表的 `build index sstable finish` 还原每张 aux 表的构建窗口：

| ALTER | aux 表 | 构建窗口（send→finish） | 完成时刻 |
|---|---|---|---|
| #1 ik_all | `fts_rowkey_doc` | **1.405 s** | 25.525 |
| #1 ik_all | `fts_doc_rowkey` | **1.118 s** | 26.363 |
| #1 ik_all | `fti_all_ik` | 2.812 s | 28.147 |
| #1 ik_all | `fti_all_ik_fts_doc_word` | **2.962 s（关键路径）** | 28.337 |
| #1 parent | `WAIT_AUX_TABLE_COMPLEMENT` 结束 | — | 28.448 |

（时刻为日志时间 23:30:xx。）最后完成的映射表 `doc_rowkey` 比最终等待**早 1.97 s** 完成，
`rowkey_doc` 早 2.81 s。另一次健康窗口（`logs/eval-nosleep`，同样 20k）为
映射 0.173 / 0.185 s、`fts_doc_word` 2.467 s，slack 仍为 2.29 s。上一轮报告的结论（映射表被分词表
完全遮住）在本轮**复现成立**。

### 2.2 量化「合并能省多少工作」

提示词的前提是「源表有 20k × 483B 的 LOB，扫描比读 aux 表贵」。实际抓取内层 SQL（`__idx_..._fts_rowkey_doc`）：

```sql
INSERT /*+ enable_parallel_dml parallel(2) ... use_px */
INTO `__idx_500003_fts_rowkey_doc` (`id`)
SELECT /*+ index(`docs` primary) ... */ `id` AS `id` FROM `docs` AS OF SNAPSHOT ... ORDER BY 1
```

- 映射表构建**只扫主键**（`index(docs primary)`，取 `id`，docid 由生成列按 rowkey 计算），
  **不读 `content` LOB**；20k 行主键 ≈ 1~2 MB 微块，而不是提示词假设的 ~10 MB LOB。
- 派生路线（路线 1：从已建好的 `rowkey_doc` 读回再排序）省下的是「一次 20k 行主键扫描 + docid 生成」，
  但要多读一次同等规模的 aux 表并多一次按 docid 排序，净扫描字节几乎为 0；
  且 `doc_rowkey` 必须等 `rowkey_doc` 完成才能开始（当前是并行），并不能缩短最终等待。
- 路线 2（一次扫描两个 writer）省一次主键扫描 + 一套 PX/直载固定开销（~90 ms/表），
  但四张表本来就并行、映射表在关键路径之外，墙钟上限仍是 0。

### 2.3 结构性理由：⑥ 对三个 build 指标中的两个恒为 0

第 2、3 次 ALTER（`build_ik_content` / `build_beng_en`）的 `prepare_rowkey_doc_table()` /
`prepare_aux_table()` 返回 `res.ddl_task_id_ < 0`（映射表已就绪、数据已完备，直接复用），
日志中这两次 ALTER 没有 `fts_rowkey_doc`/`fts_doc_rowkey` 的 INSERT。
因此 ⑥ 只可能影响 `build_ik_all` 一项，`build_ik_content` 与 `build_beng_en` 的收益**恒为 0**，
而验收门要求三项各 ≥4% —— ⑥ **在结构上不可能通过本轮门**。

**结论：不实施，不保留代码**（本地墙钟上限 0 + 无法通过三指标门 + 省下的不是 LOB 扫描）。
如果后续平台侧按「总工作量/存储」计分，⑥ 的理论价值上限也只是首 ALTER 里被遮住的那 ~0.2 s 并行工作，
不足以支撑带风险的框架改造（`docs/fts-dual-writer-plan.md` M2~M5）。

## 3. 子任务 B：DDL 固定开销（落地）

### 3.1 先量准：1 行表的 0.55 s 花在哪

用 `OB_FTS_PROF` 门控的临时计时（构建后已删除）在 1 行表上拆解。`ObIndexSSTableBuildTask::process()`
的分解（4 张 aux 表各一次，并发执行）：

| 段 | 耗时 |
|---|---|
| `generate_build_replica_sql`（含 schema guard） | < 1 ms |
| `user_sql_proxy->write(INSERT ... SELECT)` | **196.6 ms** |
| 校验和检查（`check_finish_report_checksum`） | 1.4 ms |
| 合计（`sstable_task`） | **198.0 ms** |

`fts_doc_word` 关键路径 = 2.962 s（20k），其中固定部分 ~196 ms。进一步定位到
`ObDDLStorageUtil::report_ddl_sstable_checksum()`（`src/storage/ddl/ob_ddl_common_storage_impl.cpp:230`）：
在 checksum 内层 SQL 上报**成功之后**无条件 `ob_usleep(100 * 1000)`。这段 sleep 位于每张 aux 表的
direct-load merge DAG 内，因此**每个 ALTER、每张 aux 表、每个 tablet 都付一次**。

另一处固定等待在语句层：`ObDDLExecutorUtil::wait_ddl_finish()` 以 **100 ms** 为轮询间隔等 DDL 任务完成
（`src/sql/engine/cmd/ob_ddl_executor_util.cpp`），实测一次 ALTER 在 parent SUCCESS 之后又等了 96 ms，
另一次等了 36 ms —— 纯轮询相位，0~100 ms 随机。

其它候选（`PREPARE`/`GENERATE_*_SCHEMA` 共 ~60 ms、`TAKE_EFFECT` 15~20 ms、4 张表收尾 ~20 ms、
PX/direct-load 初始化 ~90 ms）单项都小于上面两项，按「只做最大的一项」不动。

### 3.2 改动

1. `src/storage/ddl/ob_ddl_common_storage_impl.cpp`：删除 checksum 上报后的无条件
   `ob_usleep(100ms)`。checksum 上报本身（`report_ddl_checksum` → `sql_proxy.write`）不变，
   外层 `check_finish_report_checksum` 的校验也不变；若上报记录暂不可见，外层返回 `OB_EAGAIN`
   并由 DDL 任务重试路径处理（`check_and_wait_old_complement_task` 会识别已完成的 checksum 而不重跑 INSERT）。
2. `src/sql/engine/cmd/ob_ddl_executor_util.cpp`：`wait_ddl_finish()` 的轮询改为
   **前 5 s 用 10 ms、之后回 100 ms**，避免长 DDL 的内层 SQL 负载放大。

### 3.3 A/B 原始值（clean A/B：HEAD 基线 vs 交付二进制，3 块交替，每臂全新实例）

artifacts：`.tmpdev/ab-1103-235203`。每块第一条报告是完整 run（建库 + 灌 20k + 三次 ALTER），
下表只取第一条的 build 数值；`select1`/`raw_load_rows_per_sec` 取同一时刻，作为机器状态对照。

| 块 | 臂 | build_ik_all | build_ik_content | build_beng_en | select1（build 前） | raw_load rows/s |
|---|---|---|---|---|---|---|
| 1 | HEAD 基线 | 2.750 | 2.331 | 1.343 | 0.0221 | 81633 |
| 1 | 交付二进制 | 2.635 | 2.261 | 1.181 | 0.0214 | 86957 |
| 2 | 交付二进制 | 2.644 | 2.248 | 1.184 | 0.0216 | 86957 |
| 2 | HEAD 基线 | 2.748 | 2.338 | 1.430 | 0.0205 | 85106 |
| 3 | HEAD 基线 | 2.747 | 2.343 | 1.332 | 0.0196 | 85470 |
| 3 | 交付二进制 | 2.624 | 2.227 | 1.214 | 0.0211 | 86957 |
| **中位** | **HEAD 基线** | **2.748** | **2.338** | **1.343** | **0.0205** | 85106 |
| **中位** | **交付二进制** | **2.635** | **2.248** | **1.184** | **0.0214** | 86957 |
| **提升** | | **+4.11%** | **+3.85%** | **+11.84%** | — | — |
| **绝对节省** | | **0.113 s** | **0.090 s** | **0.159 s** | | |

- 无 9~16 s 离群块；build 指标块间噪声 0.50% / 0.76% / 3.65%（beng 的 3.65% 来自基线 A-2 的
  1.430 s 单元抖动，剔除后基线中位 1.338 s、提升 +11.5%，结论不变）。
- 两臂机器状态对照接近：`select1` 0.0205 vs 0.0214 ms（差 4%），`raw_load` 85.1k vs 87.0k rows/s（差 2%）。
- 另一次独立 clean 复测（23:53，同两二进制）为 `+2.88% / +9.20% / +10.54%`：方向一致，但 content 的
  基线在那一窗口偏高（2.435 s），块间波动更大；不作为主口径。
- 命中数两臂一致：`8001 / 11000 / 7332 / 20`。
- **作废的首版 A/B**（control/candidate 都夹着 B 的临时分词改动）：`+7.81% / +5.21% / +10.71%`，
  仅供对照，不用于验收。

## 4. 正确性证据（最终二进制 `bin-A-final.seekdb`，= HEAD + 本文 2 个文件）

| 关卡 | 结果 | 证据文件 |
|---|---|---|
| **DDL 专项 14 项** | **14/14 OK**（含建索引/命中/`SELECT id` 映射/分区表/双索引/online 加列/删索引/重建/清库） | `logs/correctness-234729/ddl_sanity.txt` |
| **5 个仓库用例** | `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path` **全部 PASS** | `logs/correctness-234729/mt-*.txt` |
| **36 形态 dump** | 318 行，仅 `order_rel`（并列组选取，规则允许）不同；其余 **逐字节一致** | `dump-ctrl.txt` vs `dump-final.txt` |
| **命中数** | 两臂一致 `8001 / 11000 / 7332 / 20`（clean A/B 与首版 A/B 均一致） | `.tmpdev/ab-1103-235203/*.txt` |
| **`SELECT id` 映射** | DDL 专项第 3 项 OK；`emitted document sets (order independent)` 段两臂一致 | 同上 |
| **checksum 回归** | 最终二进制 86/86 次 aux 构建 `ret=0`、0 次 `checksum not all report`、0 次 checksum 重试（压力测试，§4.2）；DDL 14 项无失败 | `logs/stress-235609/seekdb.log` |

> `order_rel` 的差异来自并列分数的选取顺序（`LIMIT` 场景），与映射/DDL 无关，属规则内允许差异。

### 4.1 最终二进制定点复核

- **1 行表固定成本地板**（`a_tiny_bench.sh`，6 张 1 行表，ik）：最终二进制中位 **0.349 s**
  （min 0.242 / max 0.381），对照提示词给的 0.55 s 地板；同一二进制只切换 sleep 的受控实验为
  on 0.431 / 0.532 → off 0.385 / 0.389（中位），与「删掉 ~100 ms + 10 ms 轮询」一致。
- **20k 单次复核**（`a_eval20k.sh`，非 A/B，仅作 sanity）：`ik_all 2.741 / ik_content 2.799 / beng 1.201`，
  tiny 0.329 s；aux 构建 12/12 `ret=0`、0 次 checksum 重试。该次 `ik_content` 的 doc_word 窗口只有 1.99 s
  但整条 ALTER 2.80 s，属于 DDL 启动/rc 抖动（与代码无关；A/B 的 3 块中位才是验收口径）。

### 4.2 checksum 竞态压力测试（针对删除 sleep 的风险）

脚本 `.tmpdev/dual/A/a_checksum_stress.sh`（交付二进制，全新实例）：

- **阶段 1**：连续创建 20 张 1 行表，每张都建 ik 全文索引并**立即**查询（`MATCH` 命中数 + `SELECT id` 映射），
  再 `DROP TABLE` —— 共 20×4 = **80 次 direct-load aux 构建**（含每次重建映射表）；
- **阶段 2**：20k 文档上连续两次 ALTER（ik_all / ik_content），随后删除两个索引。

结果：

| 检查项 | 结果 |
|---|---|
| `build index sstable finish(ret=0)` | **86 / 86** |
| `checksum not all report` / checksum 重试模式 | **0 / 0** |
| 阶段 1 立即查询/映射失败 | **0 / 20** |
| `OB_EAGAIN` | 6 次，全部是**外层 ALTER 语句** `alter_table_in_trans`（`ob_ddl_service.cpp:12499`）在阶段 1 快速连续 DDL 时的 schema 版本冲突重试；全部重试后成功，**不在 checksum 路径**；阶段 2 的两次 20k ALTER **一次也没有** |
| 删索引（含 20k 上的两个索引） | 正常 |

结论：删除 `report_ddl_sstable_checksum` 后的固定 sleep **未观察到 checksum 竞态**；
即使外层校验偶发看不到记录，也会走 `OB_EAGAIN` 重试并最终成功（本次 86 次构建无一触发该路径）。

## 5. 未完成项 / 风险 / 本地数字与平台分数的区分

### 5.1 未完成

- **⑥ 映射表合并未实施**：理由见 §2（墙钟上限 0、源表扫描只走主键、且只影响 3 个指标中的 1 个）。
  路线 1/2 都需要改 aux 子任务的「数据源」或「双 writer 直载框架」（`docs/fts-dual-writer-plan.md` M2~M5），
  在无法通过验收门的前提下不应引入。
- 其它固定开销（每张 aux 表 PX/direct-load 初始化 ~90 ms、4 张表 schema 生成 ~60 ms、
  `TAKE_EFFECT`/enable_index ~20 ms）单项都小于已做的两项，按「只做最大的一项」保留现状。

### 5.2 风险

- **低于门仍保留的依据（orchestrator 决定）**：`ik_all +4.11%`、`content +3.85%` 未到 5% 门，
  但两处都是纯固定延迟删除 —— 不碰数据通路、不跳过任何 checksum 步骤、改动共 15 行；
  且被删的等待不随平台变快而缩小，平台折算仍有 +2.1%/+2.1%/+6.6%。按「单步 <4~5% 回退」的纪律，
  这里如实标注为「未达门、经决策保留」，而不是宣称达标。
- 两处改动都在**通用 DDL 路径**（不是 FTS 专用）：
  1. 删除 `report_ddl_sstable_checksum` 后的固定 100 ms sleep —— 校验和上报与校验本身不变，
     仅去掉无条件等待；若极端情况下外层校验暂时看不到记录，会走 `OB_EAGAIN` 重试路径
     （`check_and_wait_old_complement_task` 会识别已完成的 checksum，不会重跑 INSERT），失败可恢复。
     本机 3 次 20k ALTER（12 张 aux 表）+ DDL 专项 14 项 + 5 个用例均未触发重试。
  2. `wait_ddl_finish` 前 5 s 用 10 ms 轮询 —— 只影响语句层等待频率，不影响任何数据/校验路径。
- 若平台对 DDL 内层 SQL 并发更敏感，10 ms 轮询在前 5 s 内多出 ~90 次/DDL 的轻量查询，可忽略。

### 5.3 本地数字 ≠ 平台分数

- 本文 `+4.11% / +3.85% / +11.84%` 是**本机、干净 HEAD 基线 vs 交付二进制、3 块交替**的口径，
  只证明「同机改动有效」；绝对基线（2.748/2.338/1.343）与提示词给的 2.848/2.434/1.346 一致，
  说明该窗口机器状态可用。
- 被删掉的两项都是**绝对墙钟等待**（100 ms sleep、0~100 ms 轮询相位），**不随机器变快而缩小**；
  而平台 build 约是本机 1.8 倍（5.13/4.38/2.42 s）。用实测绝对节省量
  （`ik_all 0.113 s`、`content 0.090 s`、`beng 0.159 s`）直接折算，平台相对收益约为
  **+2.1% / +2.1% / +6.6%** —— 明显小于本机数字，**不要**把本机 +4.1/+3.8/+11.8 外推到平台分数。
- 交付二进制 = HEAD + 本文 2 个文件（已确认二进制内无 `FTS_DDL_PROF`、无 B 的临时符号）；
  `bin-A-final.seekdb` 的链接时间（23:46:08）晚于两个源文件最后修改时间（23:38:01 / 23:38:10）。

## 6. 复现

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
# 构建
TMPDIR=$PWD/.tmpdev/tmp .tmpdev/dual/with_heavy_lock.sh cmake --build build_release --target seekdb -j8
# 3 块交替 A/B（A=control, B=candidate）。
# control 的构建方式：把本文 2 个文件用 `git show HEAD:<path>` 还原后构建；candidate 用工作树构建。
TMPDIR=/tmp .tmpdev/dual/with_heavy_lock.sh bash .tmpdev/dual/A/ab_bins_A.sh \
  <control-bin> <candidate-bin> 3
# 1 行表固定成本地板
.tmpdev/dual/with_heavy_lock.sh bash .tmpdev/dual/A/a_tiny_bench.sh .tmpdev/dual/A/bin-A-final.seekdb t 6 ik
# 正确性
.tmpdev/dual/with_heavy_lock.sh bash .tmpdev/dual/A/a_correctness.sh
```

