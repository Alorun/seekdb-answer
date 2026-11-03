# FTS 构建「一次分词喂两张 aux 表」落地报告（prompt-4）

> 执行对象：`docs/prompt-4-dual-writer-merge.md`
> 前置分析：`docs/fts-dual-writer-design.md`、`docs/fts-dual-writer-plan.md`（M0/M1 已完成）
> 本报告日期：2026-11-03；工作区 `indextuning`，改动未提交。

## 1. 结论（TL;DR）

| 项 | 结果 |
|---|---|
| 落地方式 | **一次分词、两次消费**：进程内共享分词缓存 + 生产者/消费者领选（leader election），**不是** prompt 的"一个 PX 算子两个 writer"（原因见 §5） |
| 开关 | `_enable_fts_token_share`（**默认 False**）+ `_fts_token_share_mem_limit_mb`（默认 128） |
| 本机 wall（3 块交替 A/B，中位） | `build_ik_all` **+0.07%**、`build_ik_content` **+0.04%**、`build_beng_en` **+0.00%** → **无变化** |
| 进程 CPU（同 3 块，中位） | `-4.12% / -4.15% / -1.14%`（每 ALTER 省 0.25~0.30 CPU-s） |
| 共享命中率 | **95.3%**（57,201 / 60,000 可共享次数）；缓存 0 eviction |
| 正确性 | 36 形态 dump 仅 `order_rel` 并列组不同（唯一允许差异）；4 个仓库用例开/关均 PASS；DDL 15 项全过；命中数 `8001/11000/7332/20` 不变；22,000 份文档自检 mismatch=0 |
| 判定 | 按 prompt-4 §6.6「某步实测收益 <5% 即停止」→ **本步收益不足，默认保持关闭**。理论上限也只有 ~7%（见 §7） |

**一句话**：机制本身已验证可用且结果逐词一致，但本机 wall 收益为 0、CPU 只省 4%，达不到 prompt-4 的「每个 build 指标 ≥12%」，也不足以抵消复杂度，因此**不默认启用**。

## 2. 改动文件

| 文件 | 内容 | 规模 |
|---|---|---|
| `src/sql/das/ob_das_domain_utils.cpp` | 共享分词缓存（内容寻址 + 16 分片 LRU）、领选/等待协议、命中路径行生成、`segment()` 接入、`FTS_TOKEN_SHARE_CHECK` 自检、统计日志 | +813 行 |
| `src/sql/das/ob_das_domain_utils.h` | `ObFTIndexRowCache::parser_sig_`（分词器身份，作为缓存键） | +4 行 |
| `rust/config/parameters.yaml` | `_enable_fts_token_share`、`_fts_token_share_mem_limit_mb` | +15 行 |
| `.tmpdev/tokshare_probe.sh`、`ab_build_toggle.sh`、`equiv_share.sh`、`verify_share.sh` | 命中率/正确性/A-B/等价性脚本（仅测试用，不进产品） | 新增 |

其余文件未改。`docs/fts-delivery-index.md` 的 §4 第 3 项已加本报告链接。

## 3. 机制（为什么不是"纯缓存"）

一个全文索引的两张分词 aux 表由**两个独立 DDL 子任务并发**构建：

- `fts_index`：`[word, docid, cnt, len]`，PK `(word, docid)`；
- `fts_doc_word`：`[docid, word, cnt, len]`，PK `(docid, word)`。

两者对同一文档文本做**完全相同**的分词；分词是 `(文本, parser 名+属性, collation)` 的纯函数，因此第二次可以直接复用第一次的结果。

### 3.1 实测发现 1：两表是 lockstep，纯缓存几乎不命中

第一版实现是"纯内容寻址缓存"（谁先算完谁写，另一个查）。20k 文档 3 次 ALTER 的统计：

```
FTS_TOKEN_SHARE_STAT(lookups=120000, hits=4645, stores=59999, evictions=0, ...)
```

**命中率 3.9%**。原因：两个子任务的扫描线程速度相同、同时开始，几乎总是**同时**处理同一份文档，双方都 miss。缓存写开销（~1.5µs/文档）还会让后发的一方慢慢追平初始的几十毫秒启动差（见 §4.3 的 A/B：`hits=132/10000`）。

### 3.2 实测发现 2：等待必须用微秒级定时器，否则 wall 翻倍

改成"先到者当 leader、另一个等结果"后，命中率立刻到 45%，但 wall 从 2.85s 涨到 **6.20s**。原因是 Linux 默认 per-thread timer slack（50µs）会把 `usleep(10)` 拉长到 **62µs**（实测，见下表），而一次分词只需 ~25µs，等的人反而成了关键路径。

| 设置 | `usleep(10)` 实测 | 结果 |
|---|---|---|
| 默认 slack | 62 µs | `build_ik_all` 6.20s（**+118%**） |
| `prctl(PR_SET_TIMERSLACK, 1µs)` | 11 µs | `build_ik_all` 2.95s（回到基线） |

因此实现里对"需要等待的线程"设置 1µs slack（每线程一次性），并把等待改成 `sched_yield` + 2/4/6…16µs 退避：

- 等待上限：32 次重试（单文档 ~300µs 上限）；
- 连续 64 个文档都"等而无获"则本线程停止等待（防 leader 卡死导致整轮变慢）；
- leader 发布后等待方立即命中，所以等待时间 ≈ 它本来要花的分词时间。

### 3.3 缓存本身

- 键：`murmurhash64(text, seed ^ collation ^ parser_sig)`；命中时 **memcmp 全文**校验，哈希冲突只会 miss，不会返回错行；
- 值：文本副本 + `(word, count)` 列表 + `doc_length`，一次 `ob_malloc` 块；
- 上限 `_fts_token_share_mem_limit_mb`（默认 128MB，0 = 关闭），16 分片，每分片 LRU，超限从最旧淘汰；20k 文档实测占用 75MB、0 淘汰；
- 命中路径把 token 拷进扫描算子的 arena（原有生命周期约定），所以缓存淘汰不会造成悬垂；
- 关闭时（默认）只多一次配置读取，**行为与现状完全一致**。

## 4. 实测数据

### 4.1 3 块交替 A/B（同一二进制，切换运行时配置；每臂全新实例 + 重新灌数）

`TMPDIR=/tmp bash .tmpdev/ab_build_toggle.sh <bin> 3 20000`，`wall/cpu` 同列（cpu = 观测进程 utime+stime）：

| 指标 | wall 基线 | wall 开启 | 差 | CPU 基线 | CPU 开启 | 差 |
|---|---|---|---|---|---|---|
| `build_ik_all` | 2.848 | 2.846 | **+0.07%** | 7.290 | 6.990 | **+4.12%** |
| `build_ik_content` | 2.428 | 2.427 | **+0.04%** | 6.030 | 5.780 | **+4.15%** |
| `build_beng_en` | 1.324 | 1.324 | **+0.00%** | 2.630 | 2.600 | **+1.14%** |

原始值（3 块，单位 s；无离群块）：

```
wall off : [[2.848,2.427,1.327],[2.852,2.428,1.324],[2.845,2.526,1.322]]
wall on  : [[2.846,2.427,1.323],[2.844,2.424,1.324],[2.846,2.427,1.327]]
cpu  off : [[7.26,6.03,2.63],[7.29,5.92,2.64],[8.06,6.33,2.61]]
cpu  on  : [[6.98,5.78,2.60],[7.83,6.05,2.62],[6.99,5.73,2.60]]
```

### 4.2 命中率与自检

20k 文档、3 次 ALTER（`ik_all` / `ik_content` / `beng`），开启后统计：

```
FTS_TOKEN_SHARE_STAT(lookups=290000, hits=57201, stores=57802, evictions=0, failed=0, bytes=75207476)
```

- 可共享次数上限 = 20k × 3 = 60,000（每 ALTER 两表各一遍）→ **命中率 95.3%**；
- `lookups` 大于 12 万的部分是等待方的重试探测，属预期；
- 自检 `FTS_TOKEN_SHARE_CHECK=1`（每次命中都重新分词并逐词比对）：

```
FTS_TOKEN_SHARE_CHECK(checked=22000, mismatch=0)
mismatch_lines=0
```

### 4.3 被否掉的两个中间版本（留档）

| 版本 | 命中率 | wall |
|---|---|---|
| 纯缓存（无协调） | 3.9% | 与基线一致（≈0 收益） |
| 领选 + 默认 slack 的 `usleep(10)` | 45% | `ik_all` 6.20s（+118%） |
| 领选 + 1µs slack + 退避 | 95.3% | 与基线一致 |

## 5. 与 prompt-4 第 2/3 步的差异及原因（代码证据）

prompt-4 第 2 步要求"一个 PX 直载算子持有两个 writer，按行对 append"，第 3 步要求 DDL 编排让 `fts_index` 子任务带写 `fts_doc_word`。核对代码后这两个前提不成立：

1. **writer 没有 sorter，且要求输入按目标表 PK 有序**
   `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row()`（`src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp:461`）
   用 `DIRECT_INSERT_ORDERED_WRITER` → `ObRsSliceWriter`（`src/storage/ddl/ob_tablet_slice_writer.h:169`），
   `append_row` 直接进 `ObMacroBlockWriter`（`ob_tablet_slice_writer.cpp:225`），**写入器内部没有排序**。
   排序在计划里（`STAT COLLECTOR(SAMPLE_SORT) → EXCHANGE(range) → SORT(按目标 PK) → PX SSTABLE INSERT`，
   `src/sql/optimizer/ob_del_upd_log_plan.cpp:952`），而两张表 PK 序不同（`(word,docid)` vs `(docid,word)`），
   所以"同一路行对喂两个 writer"会把 B 表写成乱序。
2. **切片切点按目标表 PK 采样并按 `ddl_task_id` 持久化**
   `ob_dh_sample.cpp:403` → `ObServer::get_or_insert_schedule_info` → `ObDDLSliceInfo.part_ranges_`，
   第二张表需要自己的采样与落库（M3），否则其 DAG 建不出 tablet/slice context。
3. **一个直载会话 = 一个 `ddl_task_id` + 一个 `table_id` + 一组 participants**
   `ObPxSubCoord::start_ddl()`（`src/sql/engine/px/ob_px_sub_coord.cpp:695`）、
   `ObDirectInsertSessionImpl::start`（`src/storage/ddl/ob_direct_insert.cpp:209`）。
   在一个计划里写第二张表 = 起第二个会话（M4），并解决其 complement/checksum/enable 归属（M5）。

结论：真正的"一个计划两个 writer"是 `docs/fts-dual-writer-plan.md` 里 M2~M5 的框架级改造（估 10~14 人日，M3 风险最高）。
本次改为在**不动计划/会话/DDL 状态机**的前提下，把"两份分词"降为"一份分词"——即 prompt-4 想消除的那部分 CPU：

```
before: 2 × (scan + segment + fill + sort + write) + fixed
after : 2 × (scan + fill + sort + write) + 1 × segment + fixed   # segment 只做一次
```

## 6. 正确性证据

| 项 | 方法 | 结果 |
|---|---|---|
| 索引内容等价 | 36 形态 `correctness_dump`：基线二进制（默认关）vs 开启二进制（含自检） | 244 行 dump，**1 个 hunk 差异，全部在 `order_rel` 并列组**（prompt 允许的唯一差异）；tokenize / MATCH 数 / 分数 / boolean / limit 全部逐字节一致 |
| 自检 | `FTS_TOKEN_SHARE_CHECK=1`：每个缓存命中都重新分词并逐词比对（词、词频、doc_len、顺序） | 22,000 份文档 `mismatch=0` |
| 仓库用例 | `fts_index.no_score_match / tokenize_edge / bitmap_dedup / bmw_topk_fallback` | 开关**关**：4/4 PASS；开关**开**：4/4 PASS |
| DDL 专项 | 建索引 / 分区表 / 双索引并存 / online DDL 加列 / 删索引 / 重建 / `SELECT id` 映射 / ik 索引，共 15 步 | 开关**开**：15/15 OK（第一次跑因磁盘 97% 触发 `-4184` 失败 3 项，清理磁盘后全过） |
| benchmark 命中数 | `cn/beng/mixed/limit` | `8001 / 11000 / 7332 / 20`，与改动前一致（开/关两种模式都一致） |

## 7. 为什么达不到 12%（收益账本）

平台口径 `build_ik_content = 4.224s`，其中分词约 **0.4s/表**（prompt-4 §0 自己给的 0.8s 是两张表合计）：

- 只能省掉**两张表中一张**的分词 → 上限 = 0.4s ≈ **9.5%**（`ik_all` 两列文本更长约 11%）；`beng` 是空格切分，上限 ~4%；
- 本次协调开销（缓存写入 + 等待方探测 + 命中拷贝）实测约 0.05s/ALTER，净省 **0.25~0.30 CPU-s ≈ 4%**；
- 本机 ALTER 的 CPU/wall ≈ 2.5（4 vCPU 租户，未打满）→ **CPU 节省完全不转化为 wall**，所以本机 wall A/B 必然是 0（实测 0.00~0.07%）；
- 只有平台确实是"总工作量受限"时，CPU 节省才会 1:1 变成 wall → 预期 **−4~5%**，仍低于 12% 与 prompt 的 5% 门槛。

要拿到 ≥12% 只能走"计划级合并"（一个扫描 + 一次分词 + 两路排序 + 两个直载会话），即 prompt-4 的原方案，且额外要解决 §5 的三条硬约束；即便如此，本地 wall 收益按 `fts-dual-writer-plan.md` §2.5 也只有 0~5%。

## 8. 开关与建议

```sql
-- 打开（默认 False；关闭时行为与改动前完全一致）
alter system set _enable_fts_token_share = true;
-- 内存上限（MB，0 = 直接关闭缓存）
alter system set _fts_token_share_mem_limit_mb = 128;
```

`_enable_fts_token_share` 为 `False` 时：`segment()` 只多一次配置读取，不建缓存、不加锁、不等待，结果与改动前逐字节一致（4 个仓库用例已在默认配置下复跑 PASS）。

**建议**：

1. **本机/主线保持默认关闭**：wall 收益 0、CPU −4%，不值得为此常开一个 128MB 缓存与等待逻辑；
2. 如果要在平台上验证"工作量受限"这一假设，可在同一实例上 `alter system set` 开后跑 3 项 build，预期 −4~5%（若平台不是 CPU 受限则 ~0，`beng` 可能 −1%）；
3. 想要真正 ≥12%，只能做 `docs/fts-dual-writer-plan.md` 的 M3/M4/M5（两路排序 + 第二个直载会话 + DDL 编排），本报告未做，且不建议在没有平台 A/B 通道时开工。

## 9. 复现命令

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12

# 命中率 + 3 项 build wall + 命中数（check=1 时逐词自检）
PORT=2947 bash .tmpdev/tokshare_probe.sh build_release/src/observer/seekdb 1 1 20000

# 3 块交替 A/B（同一二进制切换开关）
PORT=2948 bash .tmpdev/ab_build_toggle.sh build_release/src/observer/seekdb 3 20000

# 36 形态等价（基线 vs 开启+自检）
PORT=2952 bash .tmpdev/equiv_share.sh <baseline.bin> <share.bin>

# DDL 15 项 + 4 个仓库用例（开关打开 + 自检）
PORT=2951 bash .tmpdev/verify_share.sh <share.bin>
```

> 环境坑复现记录：磁盘使用率 ≥90% 时 observer 直接 `OB_SERVER_OUTOF_DISK_SPACE(-4184)` 拒绝启动（本次被这条坑掉一次 DDL 验证，清了 10GB 旧二进制/实例目录后恢复）；`build_ik_all` 偶发 9~25s 离群（2 次 A/B 中各命中 1 次），该块作废重跑。
