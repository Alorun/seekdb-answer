# P2（构建+查询侧）报告：D5 / D6 / D7

> 仓库 `/home/hahaha/ob-contest/seekdb-index-tuning`，分支 `indextuning`，
> 起点 HEAD `bbe2da610` + 工作区 A2 改动（`src/storage/fts/ob_ik_ft_parser.{h,cpp}`，未回退）。
> 与 Agent P1（分词侧）并行，全程使用 `.tmpdev/p_run.sh`（工作区全局锁 `.tmpdev/.build.lock`）。
> 所有本地数字为**本机**（24 核 / 15 GB，tenant `max_cpu=4`）；平台 build ≈ 本机 1.8 倍，
> **相对收益不随机器缩放**。

---

## 0. 结论速览

| 项 | 改动 | 本地 A/B（块交替，中位） | 门 | 判定 |
|---|---|---|---|---|
| **D5** | DDL index direct load 跳过微块 merge verify（`OB_DDL_SKIP_MICRO_VERIFY`） | ik_all **+3.69%** / ik_content **+5.34%** / beng **+5.92%** | ≥3% | **过** |
| **D6** | direct-load 写路径：跳过 `check_order` + `save_last_key` 固定缓冲 + commit-info hoist（`OB_DDL_SKIP_ORDER_CHECK`） | ik_all +3.32% / ik_content +3.15% / beng **+2.14%** | ≥3% | **未达标 + 4377 正确性失败 ⇒ 已整项回退** |
| **D7** | 位图批量消费：tablet 缓存 + `ensure_block` 外提（`OB_FTS_QUERY_BATCH_ADD`） | query_cn **+13.68%** / beng **+16.78%** / mixed **+13.43%**（最终二进制配对中位；另一轮 +17.01/+14.57/+8.52%） | ≥2%（目标 6%） | **过** |

测量纪律说明：

- 14:37–14:43 的测量作废：当时 `/tmp/fts-build.lock` 在本 harness 的私有 `/tmp` 下**互不可见**，
  P1/P2 的重活实际并发（本机 ALTER 被抬到 6.1/2.3/8.2 s，`select1` 0.04）。
  P1 修好锁（工作区 `.tmpdev/.build.lock`）后所有数字重测；本报告只使用**串行化之后**的窗口。
- 每轮记录 `select1_avg_ms` 作机器对照（正常 0.021–0.027）；两臂偏差 >15% 的块/轮作废。
- build 用 3–4 块交替取中位；`build_ik_all` 的 9–16 s 离群块作废重跑（本轮 Run1 出现 12.5 s 离群）。

---

## 1. D5 侧写：0.86 µs/行 拆成四段（强制先做）

### 1.1 方法

新增临时 profiler `src/storage/ddl/ob_fts_prof.h`（`OB_FTS_PROF` 门控、**每 64 次采样一次**、
`clock_gettime(CLOCK_MONOTONIC)`、16 槽、每线程槽位永不复用、`self=%p` 区分线程、输出到
`FTS_PROF_FILE`），打点 6 个文件：

| 段 | 插桩位置 |
|---|---|
| S0 child | `ObPxMultiPartSSTableInsertOp::get_next_row_from_child`（扫描+分词+行生成，P1 领域，仅作对照） |
| S1 路由+eval | `write_ordered_slice_by_row` 的 `get_tablet_info`/`switch_slice`/`eval_current_row` |
| S2 行→store row | `ObRsSliceWriter::build_multi_version_row` |
| S3 行→store row | `ObRsSliceWriter::convert_to_storage_row` |
| S4 微块追加+宏块 flush（总） | `ObRsSliceWriter` 里 `storage_slice_writer_->append_row` |
| S5 check_order | `ObMacroBlockWriter::append_row(row, split_size)` 的 `check_order` |
| S6 微块追加（总） | 同函数的 `append()`（含缓冲满时的 `build_micro_block`） |
| S7/S8/S9/S10 | `update_micro_commit_info` / `save_last_key` / split 判定+`build_micro_block` / `try_active_flush_macro_block` |
| S11..S14 | `ObMicroBlockEncoder` 的 `copy_and_append_row` / 列校验和 / 行校验和 / `inner_init`+`process_out_row_columns` |
| SORT | `ObSortOpImpl::Compare::operator()`（计数每次比较，1/64 计时） |

每个索引一个**全新实例**（`prof_run.sh`），只跑一条 `ALTER ... ADD FULLTEXT INDEX`，dump 在
`ObTabletSliceWriter::close()`。prof 源在验证后已从工作树删除。

测量窗口：**14:43:59–14:44:15（锁修复后）**，ALTER 墙钟 **2711 / 2328 / 1314 ms**
（ik_all / ik_content / beng_en），与干净基线一致 ⇒ 采样开销可忽略。
（第一次 14:37–14:40 的 prof 窗口作废：并发导致 18.5/6.1/8.2 s。）

### 1.2 四段拆分（ik_content，写线程每行，共 **2376 ns/行**）

写线程行数 693–704 k（每张 aux 表一个写线程，两张表并发）。

| 段 | ns/行 | 占比 | 细项 |
|---|---|---|---|
| **行→store row 编码**（S2+S3） | **40.3** | **1.7%** | build_multi_version_row 22.0 + convert_to_storage_row 18.3 |
| **微块追加 + 宏块 flush**（S4 总） | **985.9** | **41.5%** | 见下 |
| — 微块 build/编码/压缩/校验（摊还） | 461.5 | 19.4% | `build_micro_block`（含 merge verify 的压缩往返 + 全行重读） |
| — 逐行 copy（encoder `copy_and_append_row`） | 92.0 | 3.9% | |
| — 列校验和（`cal_column_checksum`，4 列） | 69.1 | 2.9% | |
| — 行校验和（`cal_row_checksum`） | 42.4 | 1.8% | merge verify level 2 时逐行计算 |
| — `check_order`（全 rowkey 排序校验） | 74.9 | 3.2% | |
| — `save_last_key`（rowkey 深拷贝） | 51.3 | 2.2% | |
| — update_commit / split+build / flush | 55.9 | 2.4% | 19.4 + 20.6 + 15.9 |
| — 其它（函数/守卫/编码器 init 等） | 138.8 | 5.8% | |
| **排序比较**（`ObSortOpImpl::Compare`） | **924.6** | **38.9%** | 24.58 cmp/行 × 37.6 ns/cmp |
| child + 路由（S0+S1，P1 领域，对照） | 425.3 | 17.9% | 289.1 + 136.2 |

> 四段口径：行→store row = 40；微块追加+flush = 986（其中**列+行校验和 112 ns = 4.7%**）；
> 排序比较 = 925；child+路由 = 425（不在这四段内，单列作对照）。合计 2376 ns/行。

ik_all（813,664 行）同口径：行→store row 39.6、微块追加 937.2、排序 956.1、child+路由 353.1，
合计 2286.0 ns/行；排序 25.14 cmp/行 × 38.0 ns/cmp。
（beng_en 的 S9 在该窗口出现一次 1960 ns/行的宏块 flush 离群，分段占比不可用，只用其排序 854.6 ns/行。）

### 1.3 与「0.86 µs/行」的对账

- 任务书 0.86 µs/行 =（墙钟 − 固定地板）/ 总输出行数，是**记账口径**：ik_content 墙钟 2.33 s、
  固定地板 ~0.5 s、输出行数按 960k×2 估 ⇒ 0.86 µs。
- 本轮实测每张 aux 表输出 ~0.70 M 行（两张共 1.41 M），**两个 aux 表并发构建**（各一个写线程），
  所以单线程逐行成本 2.38 µs 被摊成墙钟 ~1.19 µs/行，再加固定地板后与 0.86 µs/行同量级。
- 因此**降每行成本 X ns ≈ 降墙钟 X/2376**（写线程就是临界路径：单线程忙时 1.67 s × 2 表并发
  + 地板 0.5 s ≈ 2.33 s 实测墙钟）。

### 1.4 选型

四段里可动的最大单点是**微块 build 的 merge verify**：默认配置
`micro_block_merge_verify_level=2` 会让每个微块做「压缩 → 解压 → 全行重读 → 重算行校验和 → 比对」，
在 DDL 直载路径上是纯自检（见 §1.5 实验），且它还强制逐行行校验和（42 ns）。
排序比较虽然占 38.9%，但 C1 已证明「编码排序键」只值 2.5–4%（编码成本抵消），且属于通用 sorter，
本轮不动；校验和按任务书要求**不跳过**（列校验和保留，DDL 仍用它做源/目标一致性校验）。

### 1.5 配置实验（先验证收益再写代码）

用**同一个基线二进制**，只改动态配置 `micro_block_merge_verify_level`（2 → 0），
每档 2 轮、每轮全新实例 + 20k 文档 + 三条 ALTER：

| level | ik_all | ik_content | beng_en | hits |
|---|---|---|---|---|
| 2（默认）r1 | 2672 | 2226 | 1191 | 8001 |
| 0 r1 | 2507 | 2122 | 1132 | 8001 |
| 2 r2 | 2708 | 2228 | 1206 | 8001 |
| 0 r2 | 2530 | 2134 | 1127 | 8001 |
| **中位降幅** | **+6.4%** | **+4.4%** | **+5.8%** | 一致 |

⇒ 跳过 merge verify 在三条 build 指标上都有 4.4–6.4%，足够过 D5 的 3% 门。

---

## 2. D5 实施

### 2.1 改了什么

只对 **DDL index 表直载**把该 writer 的 merge verify 级别设为 `NONE`：

1. `ObMicroBlockBufferHelper::set_micro_block_merge_verify_level()`（新增，控制写后校验）；
2. `ObMacroBlockWriter::set_micro_block_merge_verify_level()`（新增，同时改 micro writer 的
   逐行行校验和与 buffer helper 的写后校验，二者必须一致）；
3. `ObDDLMacroBlockWriter::set_micro_block_merge_verify_level()` 转发；
4. `ObDDLStorageUtil::init_macro_block_writer()`：`param.is_index_table_` 时设为 `NONE`，
   **env 开关 `OB_DDL_SKIP_MICRO_VERIFY`（默认开，=0 一键还原）**。

文件：`src/storage/blocksstable/ob_macro_block_writer.{h,cpp}`、
`src/storage/ddl/ob_ddl_macro_block_writer.h`、`src/storage/ddl/ob_ddl_common_storage_impl.cpp`。

### 2.2 为什么安全

- 只跳过**写时自检**；落盘格式不变：`block_checksum_` 只存在于 `ObMicroBlockDesc`（写后校验用），
  不序列化进微块头（持久化的是 `data_checksum_`，仍然计算）。因此索引内容与基线逐字节等价（压缩确定性）；
- 列校验和**没有跳过**：`report_ddl_sstable_checksum` 仍按 sstable meta 的列校验和上报，
  DDL 任务仍做源/目标列校验和比对；
- `is_index_table_` 限定在索引（含 FTS aux 表）直载；主表直载路径不受影响；
- 直载 batch append 路径本来就不做 `check_order`，说明「信任构建 SQL 的排序」在本代码库已是既有前提。

### 2.3 A/B 数据（3–4 块交替）

二进制：`bin/d5-base.seekdb`（md5 `d5d81a17…`，同树无 D5）vs `bin/d5-cand.seekdb`（md5 `bd7b1097…`）。
两次构建之间 P1 的 12 个改动文件 md5 完全一致（`.tmpdev/p2/logs/gitstate-*.txt`）。

**Run1（3 块，artifacts `ab-1104-150309`）——含离群，作废**：

| 块 | 臂 | ik_all | ik_content | beng | select1 | 备注 |
|---|---|---|---|---|---|---|
| 1 | A | 2.622 | 2.157 | 1.163 | 0.0257 | 干净 |
| 1 | B | 2.424 | 2.055 | 1.093 | 0.0267 | 干净 |
| 2 | A | **12.484** | **7.092** | **4.345** | 0.0249 | **clog 离群，作废** |
| 2 | B | 2.391 | 2.037 | 1.075 | 0.0289 | 干净 |
| 3 | A | **7.061** | **6.402** | 1.116 | 0.0313 | 负载抖动（load 0.647 s），**作废** |
| 3 | B | 2.399 | 2.068 | 1.108 | 0.0249 | 干净 |

**Run2（4 块，全部干净，artifacts `ab-1104-150612`）**：

| 块 | A ik_all / content / beng | B ik_all / content / beng | 配对提升（all/content/beng） |
|---|---|---|---|
| 1 | 2.475 / 2.121 / 1.148 | 2.341 / 1.975 / 1.092 | +5.41% / +6.88% / +4.88% |
| 2 | 2.452 / 2.120 / 1.159 | 2.357 / 2.102 / 1.085 | +3.87% / +0.85% / +6.38% |
| 3 | 2.432 / 2.110 / 1.157 | 2.389 / 2.021 / 1.121 | +1.77% / +4.22% / +3.11% |
| 4 | 2.414 / 2.074 / 1.165 | 2.347 / 1.983 / 1.087 | +2.78% / +4.39% / +6.70% |
| **中位（值中位口径）** | **2.442 / 2.115 / 1.158** | **2.352 / 2.002 / 1.0895** | **+3.69% / +5.34% / +5.92%** |
| **逐块配对中位** | | | **+3.32% / +4.30% / +5.63%** |

机器对照：`select1` A 0.0253 / B 0.0253（**+0.00%**）；命中数两臂完全一致
`8001 / 11000 / 7332 / 20`。噪声（两臂各自半极差）：1.25% / 3.17% / 1.65%。

**判定：D5 过 ≥3% 门（两种统计口径下三项都 ≥3%）。**

---

## 3. D6：direct-load 写路径记账（check_order + save_last_key）

### 3.1 依据

侧写显示 `check_order` 74.9 ns/行（3.2%，全 rowkey 排序校验；直载 batch 路径本来就不做）
+ `save_last_key` 51.3 ns/行（2.2%，逐行 arena 分配 + 深拷贝）+ `update_micro_commit_info` 19.4 ns/行（0.8%）。合计 ~146 ns/行 ≈ 6.1%。

### 3.2 改了什么

1. `ObMacroBlockWriter::set_skip_order_check()`：`append_row(row, split_size)` 里跳过
   `check_order`（只跳过校验；`sql_sequence` 归一化在直载路径是 no-op，因为
   `init_datum_row_with_snapshot` 已置 0）；
2. `save_last_key` 快路径：rowkey 深拷贝进 writer 自有的 512 B 缓冲（`last_key_buf_`），
   超过 512 B 或 max/min rowkey 时回退原 arena 路径；lifetime 语义不变（仍是每行覆盖的成员）；
3. `ObDDLMacroBlockWriter` 转发 + `ObDDLStorageUtil::init_macro_block_writer` 对 index 表开启，
   **env `OB_DDL_SKIP_ORDER_CHECK`（默认开，=0 还原）**。

### 3.3 A/B 数据（8 块交替，env on/off，同一二进制）

`bin/d67.seekdb`（D5+D6+D7，D6 由 `OB_DDL_SKIP_ORDER_CHECK` 开关）+ `ab_bins_env.sh`：
A=`OB_DDL_SKIP_ORDER_CHECK=0`（仅 D5），B=`=1`（D5+D6），8 块交替，artifacts `abenv-1104-152629`。

| 块 | A ik_all / content / beng | B ik_all / content / beng | 配对（all/content/beng） |
|---|---|---|---|
| 1 | 2.363 / 2.012 / 1.059 | 2.267 / 1.934 / 1.033 | +4.06% / +3.88% / +2.46% |
| 2 | 2.545 / 2.080 / 1.070 | **9.063** / 1.947 / 1.041 | **离群** / +6.39% / +2.71% |
| 3 | 2.379 / 2.002 / 1.038 | 2.328 / 1.939 / 1.044 | +2.14% / +3.15% / −0.58% |
| 4 | 2.333 / 2.016 / 1.038 | 2.363 / 2.000 / 1.056 | −1.29% / +0.79% / −1.73% |
| 5 | 2.478 / 2.015 / 1.090 | 2.357 / 2.022 / 1.020 | +4.88% / −0.35% / +6.42% |
| 6 | 2.457 / 2.052 / 1.095 | 2.370 / **9.653** / 1.100 | +3.54% / **离群** / −0.46% |
| 7 | 2.353 / 1.981 / 1.105 | 2.280 / 1.949 / 1.085 | +3.10% / +1.62% / +1.81% |
| 8 | 2.330 / 2.000 / 1.077 | **7.465** / 1.900 / 1.024 | **离群（load 也掉）** / +5.00% / +4.92% |

剔除 3 个 clog/负载离群块后的**逐块配对中位**：ik_all **+3.32%**、ik_content **+3.15%**、
beng_en **+2.14%**。机器对照：`raw_load_rows_per_sec` 两臂差 <10%（无系统性偏置），
`select1` 块间噪声大（±10~35%），故不作为唯一裁判。

**beng_en 只有 ~2%**（英文 token 的 collation 比较更便宜，`check_order` 成本更低），未过 3% 门。

### 3.4 正确性失败与回退（决定性）

为把 beng 补过 3%，又加了「每个微块只折叠一次 commit info」的 hoist（`update_micro_commit_info`
由逐行改为每微块首行）。复测前先跑仓库用例，结果 **D6 打开时两项用例失败**：

- `bitmap_dedup`、`count_only_fast_path` → `OB_ERR_DEFENSIVE_CHECK (4377)`：
  `check_lookup_row_cnt`，`lookup_rowkey_cnt=4, lookup_row_cnt=0`
  —— **docid→rowkey 的 lookup 一行都没查到**，即 `SELECT id` 映射被破坏。
- 用 env 逐项二分（同一二进制）：全关 **PASS**；只开 D5 **PASS**；只开 D6 **FAIL**；只开 D7 **PASS**。
  → 问题由 D6 引入（`check_order` 跳过 / `save_last_key` 缓冲 / commit hoist 之一），
  而 D6 的收益本来就只有 beng 不达标；**按协议整项回退**（工作树已无 D6 代码），
  不为了凑 3% 保留有正确性风险的改动。

> 为什么 suspect `check_order`：它不只是校验，还会把 `sql_sequence` 归一化并驱动 rowkey 顺序
> 保证；直载 batch 路径虽然不调用它，但那是另一条通路。`save_last_key` 缓冲则改了下发给
> 微块索引的 `last_rowkey_` 的生命周期。两者都可能让 mapping 表的 sstable 索引边界出错，
> 与 4377 的症状一致。时间关系未继续定位到具体子项——D6 无论如何过不了 3% 门。

### 3.5 结论

**D6 未达标（beng ~2.1%），且候选改动引入正确的 docid→rowkey 映射破坏，已整项回退。**
交付树只保留 D5（已过门）与 D7（已过门）。

---

## 4. D7：位图批量消费（query side）

### 4.1 依据（C3 + 本轮侧写）

C3 实测（query_cn 0.50 ms 量级）：bitmap build 426 µs/query，其中倒排扫描 51.9%
（17.4 ns/posting、0.53 µs/批、53.2 批/query）、消费侧 48.1%（16.2 ns/posting）。
批次容量 256→1024 被 `eval_ctx->max_batch_size_` 硬约束，已放弃。

本轮在同一二进制里加了临时 query profiler（`OB_FTS_PROF`、每 256 次查询输出一行到
`/tmp/fts_qprof.log`，交付前删除），实测：

| 运行（OB_FTS_PROF） | 查询数 | batches | postings | build ms | scan ms | consume ms | consume/posting | consume/scan |
|---|---|---|---|---|---|---|---|---|
| ba=0（旧逐 posting） #1 | 256 | 14778 | 3,536,469 | 103.7 | 69.2 | 34.0 | **9.6 ns** | **0.491** |
| ba=1（新批量路径） #2 | 256 | 15199 | 3,536,469 | 103.5 | 85.4 | 17.4 | **4.9 ns** | **0.203** |
| ba=0 #3（机器较慢） | 256 | 14778 | 3,536,469 | 185.8 | 123.9 | 61.1 | 17.3 ns | **0.493** |
| ba=1 #4（机器较慢） | 256 | 14833 | 3,536,469 | 146.6 | 120.9 | 24.7 | 7.0 ns | **0.204** |

`scan`（倒排扫描）两臂代码完全相同，用它当机器速度对照：**consume/scan 从 0.491–0.493 稳定降到
0.203–0.204**（两轮各自一致），即消费侧成本**减半**。按 benchmark 里 scan ≈ 300 µs/query 折算，
每查询省 ~0.29×300 ≈ **85 µs**，对应 query_cn ~0.43 ms 的 **~15–20%**。

所以剩下的可动项是**消费侧每个 posting 的固定开销**：`add_doc_id` 每条 posting 都做一次
线性 tablet 查找 + 一次 out-of-line `ensure_block` 调用（该方法带 while 分配分支，不会被内联）。

### 4.2 改了什么

`ObTextDocIdBitmapIter::add_doc_id_batch()`（新）：整批消费时
（1）缓存上一个 tablet 的 `TabletBits*`，同一 batch 内不重复线性查找；
（2）只有 `block_idx >= blocks_.count()` 时才调用 `ensure_block`（常见路径 0 次调用）；
（3）位图置位与 `used_bits_` 语义与逐条路径完全一致。
`build_doc_id_set()` 的缓冲批次分支改调新接口；**env `OB_FTS_QUERY_BATCH_ADD`（默认开，=0 还原
逐 posting 路径）**。文件：`src/storage/retrieval/ob_text_daat_iter.{h,cpp}`。

### 4.3 A/B 数据

`bin/d67.seekdb` + `ab_bins_env.sh`：A=`OB_FTS_QUERY_BATCH_ADD=0`，B=`=1`，4 块交替；
每块 full+2 warm、每份 SAMPLES=3 ⇒ 每臂 36 个查询样本；按块配对 + `select1` 归一化。
**独立跑了两轮**（第一轮在带 profiler 的同一代码二进制上，第二轮在最终交付二进制上）：

**Run A（最终交付二进制 md5 `2e7dc1b8…`，artifacts `abenv-1104-154848`）**：

| 指标 | A 中位 | B 中位 | 逐块配对（blk1/2/3/4） | 配对中位 | MAD | select1 归一化 |
|---|---|---|---|---|---|---|
| query_cn | 0.5087 ms | 0.4293 ms | +8.98% / +21.38% / +12.46% / +14.90% | **+13.68%** | 2.96% | **+10.96%** |
| query_beng | 0.6284 | 0.5169 | +14.95% / +22.67% / +5.13% / +18.62% | **+16.78%** | 3.86% | +12.80% |
| query_mixed | 0.5311 | 0.4526 | +9.73% / +13.75% / +13.12% / +27.23% | **+13.43%** | 2.01% | +7.48% |
| query_limit（阴性对照，不走位图） | 0.2484 | 0.2447 | −20.41% / +13.00% / +3.98% / −1.05% | +1.46% | 7.03% | +1.85% |

**Run B（带 profiler 的同一代码，artifacts `abenv-1104-151905`）**：
query_cn **+17.01%**、beng **+14.57%**、mixed **+8.52%**（配对中位）。
两轮方向、量级一致；阴性对照 `query_limit` 两轮都在噪声内（+1.46%、−7.32%）。

- `select1` 逐块：−10.6% / −0.8% / −10.8% / +9.9%（无系统性漂移），ab_bins 汇总 A 0.0277 / B 0.0279（+0.72%）。
- 命中数两臂一致 `8001 / 11000 / 7332 / 20`。
- **注意测量偏差**：同一套块模式在 D5（纯构建侧改动）上也测出过 query_cn +11%，
  说明该 benchmark 的查询段本身带 ~10% 量级的臂间偏差；因此 D7 的主证据是上面的
  qprof 机制测量（consume/scan 比值减半），A/B 只作旁证（两轮独立、方向一致）。

---

## 5. 正确性五关（证据）

全部在交付二进制 `bin/d67.seekdb`（md5 `2e7dc1b8…`，D5+D7）上完成，**同一二进制**用 env 开关
on/off 两条分支对比（源码完全一致，见 §0 说明）：

| 关卡 | 命令 | 结果 |
|---|---|---|
| 36 形态 dump（on vs off） | `verify_arm.sh <bin> d67on` / `d67off`（off 带 `OB_DDL_SKIP_MICRO_VERIFY=0 OB_FTS_QUERY_BATCH_ADD=0`） | **唯一差异是 `### order_rel` 一个 hunk（17 行）**，其余逐字节一致 |
| 745 行压测（stress） | 同上脚本内置 | **逐字节一致（0 diff，31537 B）** |
| 5 个仓库用例 | `suite.sh <bin>`（MODE=verify） | no_score_match / tokenize_edge / **bitmap_dedup** / bmw_topk_fallback / **count_only_fast_path** 全部 **VERIFY PASS** |
| DDL 专项 14 项 | `ddl_sanity.sh <bin>`（含建/删/重建索引、分区表、双索引、online DDL 加列、清库） | **0 失败** |
| 命中数 | benchmark load + dump | 两臂均 **8001 / 11000 / 7332 / 20**；dump 的 `cn_2terms=8001`、`en_3terms=11000`、`cn_mixed=7332`、`limit_20=20` |
| **SELECT id（docid→rowkey 映射）** | `ddl_sanity.sh` 第 3 项 `select id from d.t where match(body) against('beta') order by id`；以及 dump 的 limit/ordering 形态 | **OK**；D7 打开/关闭两臂的 id 集合与 SUM/MIN/MAX 完全一致 |

**直载顺序**：D5 之后三条 build 的 MATCH 命中集合、`COUNT/SUM/MIN/MAX` 形态、`LIMIT` 形态与
off 分支完全一致（dump 除 `order_rel` 外逐字节一致），说明直接加载写入的 aux 表仍按目标
rowkey 全局有序、索引可用；这也覆盖了 `fts_rowkey_doc` / `fts_doc_rowkey` 映射表。

`### order_rel` 差异说明：这是 `LIMIT 20` 下同分文档的并列次序（浮点同分排序不稳定），
C1/C3 也观察到同样现象，任务书明确允许该差异；两臂的命中数与 `COUNT/SUM/MIN/MAX` 形态完全一致。

**D5 专项**：微块 merge verify 只跳过写时自检，落盘格式不变（`block_checksum_` 不落盘；
持久化的 `data_checksum_` 仍计算）；列校验和未跳过，DDL 仍做源/目标列校验和比对——
DDL 14 项与 dump 均验证通过。
**D6 专项（已回退）**：D6 打开时 `check_lookup_row_cnt` 报 4377（`lookup_rowkey_cnt=4,
lookup_row_cnt=0`），即 docid→rowkey 映射被破坏；env 二分确认只有 D6 会触发；整项回退后
上述所有关卡恢复 PASS。这正好证明本节第 6 项（SELECT id 映射）是必要关卡。

---

## 6. 未完成、风险与平台换算

### 6.1 未完成

1. **D6 未达标并回退**（见 §3）：8 块配对中位 ik_all +3.32% / ik_content +3.15%，但 beng_en 只有
   **+2.14%**（<3% 门）；随后加入 commit-info hoist 的版本又触发 `check_lookup_row_cnt`(4377)
   的正确性失败。按协议「单步 <3% 回退」「不达验收门必须回退」，D6 整项已从工作树删除。
2. **排序比较（38.9%）没有动**：C1 已两次实测「顺序保持编码排序键」只值 +2.5~4%（编码成本抵消），
   且属于通用 `ObSortOpImpl`（会影响所有 DDL/DML 排序）；本轮时间用于确认更有把握的微块自检项。
   若下一轮继续 build，建议方向是把「word 预编码键」做在 FTS 构建 SQL 的排序键上（不动通用 sorter），
   并用 D5 之后的基线复测。
3. **批次容量 256→1024** 未重试（C3 已证明被 `eval_ctx->max_batch_size_` 硬约束）。

### 6.2 临时插桩与清理

- build 侧 profiler（`src/storage/ddl/ob_fts_prof.h` + 6 文件打点）与 query 侧 profiler
  （`ob_text_daat_iter.cpp` 内 `OB_FTS_PROF` 计数）**均已删除**；交付树 `grep fts_prof/QueryProf` 为空。
- 所有改动都有 env 一键回退：`OB_DDL_SKIP_MICRO_VERIFY`、`OB_FTS_QUERY_BATCH_ADD`（=0 还原）。
- 同一二进制 env on/off 两条分支的输出一致性：36 形态 dump 仅 `order_rel` 差异、745 行压测逐字节一致（§5）。

### 6.3 风险与边界

- **D5**：只跳过 DDL index 直载的**写时自检**（encode/compress 往返 + 全行重读 + 行校验和比对）；
  落盘格式不变（`block_checksum_` 不序列化，持久化的 `data_checksum_` 仍计算），列校验和与
  DDL 源/目标列校验和比对保留。作用域限定 `param.is_index_table_`（FTS aux 表 + 普通二级索引）。
- **D7**：位图置位/`used_bits_`/tablet 归属/批次生命周期与逐 posting 路径逐语义一致；`OB_FTS_QUERY_BATCH_ADD=0`
  可回退；5 个仓库用例 + dump + 745 行压测验证。

### 6.4 测量口径与污染记录（下一轮务必注意）

1. **锁**：本 harness 每次 bash 调用挂私有 `/tmp`，`/tmp/fts-build.lock` 互不可见 ⇒ 14:37–14:43 的
   build-prof/基线全部作废（ALTER 被抬到 6.1/2.3/8.2 s、`select1` 0.04）。锁改为工作区
   `.tmpdev/.build.lock`（P1 修）后所有数字重测；本报告只用串行化之后的窗口。
2. **离群块**：D5 Run1 的 A-2/A-3（12.5/7.1 s，clog 拥塞，`select1` 正常）与 D6 的 B-2/B-6/B-8 作废。
   判定一律用逐块配对中位 + 离群标注。
3. **同树构建**：A/B 两臂二进制都在 P1 同一改动状态下构建，构建前后 P1 的 12 个文件 md5 记录在
   `.tmpdev/p2/logs/gitstate-*.txt`，两次构建之间无他人文件变化。
4. **query 段臂间偏差**：纯构建侧改动（D5）在同套块模式上也测出 query_cn +11%，故 query 结论以
   qprof 机制测量（consume/scan）为主、A/B 两轮独立复现为辅。

### 6.5 本机 vs 平台

| 指标 | 本机（本轮） | 平台（≈1.8×，任务书口径） |
|---|---|---|
| build_ik_all | 2.42 s | ~4.4 s |
| build_ik_content | 2.06 s | ~3.7 s |
| build_beng_en | 1.08 s | ~1.95 s |
| query_cn | 0.43–0.51 ms | 0.4331 ms（平台实测基线） |

相对收益（+3.7%/+5.3%/+5.9% 与 query −14%）不随机器缩放；平台绝对值不要与本机秒数直接比较。

### 6.6 交付物与复现

- 源码改动 6 个文件（D5 4 个 + D7 2 个，共 +108/−3 行）：
  `src/storage/blocksstable/ob_macro_block_writer.{h,cpp}`、
  `src/storage/ddl/ob_ddl_macro_block_writer.h`、`src/storage/ddl/ob_ddl_common_storage_impl.cpp`、
  `src/storage/retrieval/ob_text_daat_iter.{h,cpp}`。
- 最终集成二进制 `build_release/src/observer/seekdb` = `bin/d67.seekdb`
  （md5 `2e7dc1b8d99c53c0dc828399cd130569`，按最终工作树构建，含 P1 并行改动）。
- **A/B 证据严格对应当时构建的二进制**：D5 用 `d5-base`/`d5-cand`（已删，md5 `d5d81a17`/`bd7b1097`），
  D7 用 `d67`（md5 `2e7dc1b8`）；交付二进制因包含 P1 后续改动，数值会与本机 A/B 略有差异，属预期。

```bash
# D5 A/B（4 块交替）
.tmpdev/p2/ab_bins.sh <base> <cand> 4 2
# D6/D7 env on/off A/B（同一二进制）
.tmpdev/p2/ab_bins_env.sh <bin> "OB_FTS_QUERY_BATCH_ADD=0" "OB_FTS_QUERY_BATCH_ADD=1" 4 2
# 正确性
.tmpdev/p2/suite.sh <bin>
.tmpdev/p2/verify_arm.sh <bin> on
.tmpdev/p2/verify_arm.sh <bin> off OB_DDL_SKIP_MICRO_VERIFY=0 OB_FTS_QUERY_BATCH_ADD=0
```
