# C1（构建侧）报告：B-1 每行成本拆分 / 排序编码键实验 / B-2 评估

> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）。
> 并行协议：`.tmpdev/tri/PROTOCOL.md`；所有构建、实例、benchmark 均包在
> `.tmpdev/tri/with_heavy_lock.sh` 内。
> **最终结论：B-1 的候选改动经两次 3 块交替 A/B 实测只有 +2.5%~+4%，未达 8% 验收门，
> 已按协议回退；交付树没有构建侧的源码改动。B-2 实测收益 <1%，未实施。**
> 本报告保留全部测量，供下一轮复用。

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| 三段计时 | 排序比较占临界线程 ~37%（23.5 次/行 × ~54 ns/次），微块追加 S3 ~25%，行→store row 仅 ~1% |
| B-1 实施 | 构建 SQL 打开编码排序键（`enable_newsort=true`），**1 行**；A/B 仅 +2.5%~+4% ⇒ **已回退** |
| B-1 验收 | 两次独立 3 块 A/B：build 三项 +3.25/+1.97/+4.18%（run1）、+4.07/+1.95/+2.51%（run2），**未过 8% 门** |
| B-2 | 未实施：`enable_index`/`update_index_status` 单次 1.7~2.9 ms，4 表合计 ~10 ms，4% 门（≈93 ms）不可达 |
| 正确性（候选二进制） | 36 形态 dump 318 行、差异 20 行且**全部落在 `order_rel`**（协议允许）；5 个仓库用例全 PASS；DDL 14 项 0 失败；命中数 8001/11000/7332/20 |

---

## 1. 测量方法（可复现）

- **临时 profiler**：`src/storage/ddl/ob_fts_prof.h`（新增，header-only）+ 5 个文件打点；
  `getenv("OB_FTS_PROF")` 门控、**每 64 次采样一次**、`clock_gettime(CLOCK_MONOTONIC)`、
  线程局部槽位（槽位永不复用，线程退出后计数仍保留）。验证后**全部删除**
  （`.tmpdev/tri/c1/orig/` 还原，`grep -rn fts_prof src/` 为空）。
- **每个索引一次全新实例**（`prof_each.sh`）：先 `DROP/CREATE` 20k 文档表、灌数据，再只跑一条
  `ALTER TABLE ... ADD FULLTEXT INDEX ...`，避免三次 ALTER 的计数器互相污染。
- 墙钟对照：prof 开启时 **ik_all 2703 ms / ik_content 2328 ms / beng_en 1363 ms**，
  与同机未插桩运行（2869/2342/1256 ms）以及任务书基线（2750/2293/1229 ms）同量级
  ⇒ prof 采样开销可忽略。
- 打点位置：
  | 段 | 位置 |
  |---|---|
  | S0 child | `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row` 的 `get_next_row_from_child`（扫描+分词+行生成） |
  | S1 路由+eval | 同循环 `get_tablet_info_from_row`/`switch_slice_if_need`/`eval_current_row` |
  | S2 行→store row | `ObRsSliceWriter::append_current_row` 的 `build_multi_version_row`+`convert_to_storage_row` |
  | S3 微块追加 | `ObMacroBlockWriter::append_row(row, split_size)`（`MicroBlockWriter::append_row` 编码/列校验和/hash index + `update_micro_commit_info` + `save_last_key` 深拷贝 + 切块判定） |
  | S4 宏块 flush | `try_active_flush_macro_block` |
  | S5 排序比较 | `ObSortOpImpl::Compare::operator()`（每次比较计数，1/64 计时） |

## 2. 三段计时实测（纳秒/行）

### 2.1 临界写线程（ik_content，693,337 行，单 slice）

| 段 | ns/行 | 说明 |
|---|---|---|
| S0 child | 321 | 扫描 + 分词 + 行生成 |
| S1 路由 + eval | ~180 | tablet/slice 路由 + 4 个输出表达式求值 |
| S2 行→store row | 32 | `build_multi_version_row` + `convert_to_storage_row` |
| S3 微块追加 | **773** | 见上表；其中列校验和、rowkey 深拷贝都在此段内 |
| S4 宏块 flush 触发 | 15 | |
| S5 排序比较 | **~1270** | 23.5 次/行 × 54 ns/次 |

该线程忙时 ≈ 2.35 s，与整个 ALTER 墙钟 2.33 s 基本相等 ⇒ **它就是临界路径**。
全部线程累加（ik_content）：S0 412 ms、S1（含 append）1427 ms、S2 45 ms、S3 1103 ms、
S4 22 ms、排序 1335 ms；`cmp/row=23.55`、`ns/cmp=56.7`。

三项 build 的排序统计：ik_all 23.39 cmp/row × 56.3 ns；ik_content 23.55 × 56.7；
beng_en 21.76 × 57.8。输出行数：1,667,336 / 1,426,682 / 803,814。

### 2.2 0.96 µs/行 怎么解释

任务书的 0.96 µs/行 =（总时间 − 固定地板）/ 输出行数，是**记账口径**，不是单线程逐行成本：

- ik_content 2.33 s、1.43 M 输出行、地板 ~0.45~0.55 s ⇒ 1.80 s / 1.43 M ≈ 1.26 µs/行；
- 两个大 aux 表（fts_index 与 fts_word）**并发构建**，每个 build 又有 DOP=2 的 PX 切片，
  墙钟被摊薄，所以折算到每行只有 ~0.9~1.3 µs；
- 临界线程上真正逐行的三段之和 ≈ 2.1 µs/行（1.3 µs 排序 + 0.77 µs 追加），
  其余 ~0.3~0.5 µs 在 child 侧（分词 + 行生成，属 C2 领域）；
- 本轮 prof 实测输出行数：ik_all **1,667,336**、ik_content **1,426,682**、beng_en **803,814**。
  任务书按 960k×2 = 1.92 M 估算，比实测偏大 ~35%，所以“0.96 µs/行”的分母偏大、
  实际每行分摊的墙钟更接近 1.2 µs。

关键判断：**排序比较是单点最大项（~37%），微块追加其次（~25%），行→store row 仅 ~1%**，
列校验和与 rowkey 编码被包在 S3 内，不是独立大项。

---

## 3. B-1 实施、A/B 与回退

### 3.1 为什么选排序

`word` 列是 VARCHAR，沿用源文本列的 `utf8mb4_general_ci`，比较必须走
`ObCharset::strcmp`（函数指针 + 排序规则语义），每次 ~54 ns；而构建 SQL 里
`opt_param('enable_newsort','false')` 显式关掉了**顺序保持编码排序键**
（`T_FUN_SYS_ENCODE_SORTKEY` + `ObAdaptiveQS`，比较退化为一次 memcmp）。
该 opt_param 全仓库只被 `check_can_encode_sortkey` 读取（已 `grep` 验证），
编码器 `ObOrderPerservingEncoder` 明确支持 `ObVarcharType`（`encode_from_string_varlen`）。

### 3.2 改动

`src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp`（`generate_build_replica_sql`）：
把 aux 表构建 SQL 的 `opt_param('enable_newsort', 'false')` 改为 `'true'`（1 行）。
另试过“只对 fts_index 表启用、doc_word 保持 false”的收敛版（见 §3.4 run3）。

### 3.3 A/B 方法

`ab_bins.sh <bin-base> <bin-cand> 3 0`：同一台机器、两个二进制**交替**跑 3 块，
每块“全新实例 + 灌 20k 文档 + 建 3 个索引”，只取 build 指标；同时记录 `select1_avg_ms`
作为机器状态对照。bin-base / bin-cand 都是**本地重建**（`git status` 两次构建之间只有我这一行差异，
已用快照比对确认），不引用任何固定基线。

### 3.4 结果（两次独立 3 块 A/B + 一次收敛版）

**Run 1**（base=A，cand=全表启用编码键=B；artifacts `.tmpdev/tri/c1/ab-1104-124453`）：

| 指标 | A 原始（块1/2/3） | A 中位 | B 原始（块1/2/3） | B 中位 | improve |
|---|---|---|---|---|---|
| build_ik_all | 2.582 / 2.594 / 2.552 | 2.582 | 2.498 / 2.488 / 2.522 | 2.498 | **+3.25%** |
| build_ik_content | 2.137 / 2.182 / 2.197 | 2.182 | **9.676（离群）** / 2.118 / 2.139 | 2.139 | +1.97% |
| build_beng_en | 1.194 / 1.251 / 1.196 | 1.196 | 1.170 / 1.146 / 1.128 | 1.146 | **+4.18%** |

离群标注：B 的第 1 块 `build_ik_content=9.676 s` 属任务书所述 9~16 s 离群（clog 拥塞），
该块按协议作废；用剩余两块看约 +1.5~2.9%。
机器对照：`select1_avg_ms` A 0.0207 / B 0.0232（+12.1%，B 臂机器更慢，仍更快）。

**Run 2**（重建的同一 cand 源码，md5 与 Run 1 相同；artifacts `.tmpdev/tri/c1/ab-1104-125049`）：

| 指标 | A 原始 | A 中位 | B 原始 | B 中位 | improve |
|---|---|---|---|---|---|
| build_ik_all | 2.668 / 2.678 / 2.677 | 2.677 | 2.568 / 2.569 / 2.567 | 2.568 | **+4.07%** |
| build_ik_content | 2.197 / 2.228 / 2.206 | 2.206 | 2.126 / 2.163 / 2.172 | 2.163 | +1.95% |
| build_beng_en | 1.203 / 1.188 / 1.196 | 1.196 | 1.165 / 1.166 / 1.171 | 1.166 | **+2.51%** |

`select1_avg_ms` A 0.0241 / B 0.0255（+5.8%）。无离群。

**Run 3**（收敛版：仅 fts_index aux 表启用，doc_word 保持 false）：
build median **+1.05%**（ik_all +1.05 / ik_content +2.07 / beng -0.08，噪声 41%~141%），
说明收益并非只来自 fts_index 表；恢复为“全表启用”后仍是 ~2.5~4%。

**结论**：两次独立 3 块 A/B 的 build 中位提升为 +3.25% 与 +2.51%（单指标最高 +4.18%），
**达不到 8% 验收门**（任务书要求“build 三项各降 ≥8%”）。
按 PROTOCOL §8“达不到验收门的改动必须回退”，已把
`src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp` 从 `.tmpdev/tri/c1/orig/` 还原，
交付树中该文件与 HEAD 一致（`git status` 无该文件差异）。

### 3.5 为什么收益远小于“排序占 37%”

- 两个大 aux 表并发构建，build 墙钟由**较慢的那张表 + DDL 固定地板**决定；
  编码键同时帮了 fts_index（字符串首列）与 doc_word（同 doc 内大量字符串比较），
  但总体 Amdahl 上限受地板（~0.5 s，21%）与另一张表约束；
- 编码本身有每行成本（顺序保持编码要按 collation 转换 + 额外一列存储），
  抵消了比较侧的收益；
- 实测再次确认：**0.96 µs/行的记账口径掩盖了并发结构**，下一步若继续做 build，
  应优先拆“并发结构/固定地板”，而不是单点比较成本。

---

## 4. 正确性证据（候选二进制，回退前）

| 关卡 | 结果 |
|---|---|
| 36 形态 correctness dump（base vs cand，各 318 行） | 差异 **20 行**，全部落在 `### order_rel`（协议唯一允许差异；同分文档的并列次序，索引内容与命中集合一致） |
| 5 个仓库用例（`MODE=verify`） | no_score_match / tokenize_edge / bitmap_dedup / bmw_topk_fallback / count_only_fast_path **全部 PASS** |
| DDL 专项 14 项（`.tmpdev/tri/c1/ddl_sanity.sh`） | **0 失败**（建索引/分区表/第二索引/online DDL 加列/删索引/重建/清库） |
| benchmark 命中数 | **8001 / 11000 / 7332 / 20**，与基线一致 |
| 直载顺序 | 排序键改为编码键后索引仍全局有序（MATCH 命中集合、COUNT/SUM/MIN/MAX 形态全部一致；仅 order_rel 并列次序变化） |

说明：候选改动已回退，交付树的构建路径与 HEAD 完全一致，因此不存在正确性风险。

---

## 5. B-2（收尾阶段合并）：未实施

实测（`logs/seekdb-ik_content.log`，一次 ik_content 构建）：

- `update_index_status_in_schema` → `enable_index` 的间隔：**1.7 ms / 2.9 ms / 2.1 ms**；
- 4 张 aux 表的收尾调用分布在两个时间簇（相隔约 2 s），与数据构建流水线**重叠**，
  不是串行独立阶段；4 表合计 ~10 ms 量级；
- 任务书的“~20 ms/表、4 表 ~80 ms”应包含了 schema refresh 等待（本机实测收尾远小于该值）。

结论：即便把 4 次 `enable_index` 合并为一次 schema 变更，收益也 << 4% 门（≈93 ms），
且合并需要改 RS schema service 的提交路径（高风险），**不做**。
（对比：一次 ALTER 的固定地板 ~0.45~0.55 s，其中大头是 DDL 准备阶段的多次 schema 刷新与
create_index_tablet，不在“收尾”。）

---

## 6. 未完成、测量污染与平台换算

### 6.1 未完成

- B-1 没有一个能过 8% 门的构建侧改动：排序编码键（+2.5~4%）已回退；
  微块追加 S3（773 ns/行）只做了整体拆分，未进一步拆到
  `data_buffer_.write_row` / 列校验和 / hash index / `save_last_key` 四个子项
  （时间不足，下一轮可直接复用 `prof_each.sh` + 12 槽位 profiler 继续拆）。
- B-2 未实施（见 §5）。

### 6.2 测量污染源（已规避/记录，下一轮务必注意）

1. **锁外 `cp` 二进制**：第一次 prof 运行前在 heavy lock 外拷贝 `build_release/.../seekdb`，
   期间可能被他人的 relink 覆盖，导致拷出的 prof 二进制与构建产物 md5 不一致（表现为
   prof 代码“没生效”）。此后所有“构建 + cp + md5 校验”都在**同一个**锁调用内完成。
2. **线程局部 profiler 槽位复用**：第一版按 TLS 地址保存计数，线程退出后地址被新线程复用，
   dump 值归零/串线；改为全局槽位（永不复用）后数据自洽。
3. **机器漂移**：两次 A/B 的 `select1_avg_ms` 分别 +12.1% / +5.8%（B 臂更慢），
   构建指标仍显示 B 更快，说明真实收益可能略高于测得的 2.5~4%，但不足以跨到 8%。
4. **`build_ik_content` 离群**：Run 1 的 B-1 块 9.676 s，按协议作废。
5. 共享树上的他人半截改动会打断构建（本轮 C2/C3 各一次），按协议 90 s 重试、未动他人文件。

### 6.3 平台换算

本报告所有数字均为**本机**（24 核 / 15 GB，tenant `max_cpu=4`）实测；
平台 build 约为本机 **1.8 倍**（任务书口径）。例如本机 base 2.58/2.18/1.20 s
≈ 平台 4.6/3.9/2.2 s。**本地 +3% 在平台上仍是 +3%（相对值不随机器缩放）**，
所以未达 8% 的结论在平台上同样成立。不要把本报告的本机绝对秒数与平台基线直接比较。

### 6.4 建议的下一步（按性价比）

1. 继续拆 S3（773 ns/行）：先拆 `write_row` / 列校验和 / hash index / `save_last_key`，
   目标是确认能否砍掉 300~400 ns/行；
2. 拆 build 的**并发结构**：两个大 aux 表并发 + 每表 DOP=2，实测临界线程单线程饱和；
   评估“把 fts_index 与 doc_word 改为串行、各自 DOP=4”或反之，看墙钟；
3. 固定地板（0.45~0.55 s，21%）：DDL 准备阶段的 schema 刷新次数；
4. 若仍要动排序：需要为 `utf8mb4_general_ci` 设计**顺序保持的分桶键**
   （首字节 + ASCII 折叠），而不是简单按字节分桶（general_ci 下字节序 ≠ 排序序，已排除）。
