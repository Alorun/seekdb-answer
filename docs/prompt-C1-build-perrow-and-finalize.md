# 提示词 C1：构建侧两项（B-1 每行成本 + B-2 收尾合并）

> 自包含提示词。你可以与另外两个 agent **并行**工作（它们分别改分词侧与查询侧），
> 因此**必须遵守 §0 的测量隔离协议**，否则你的 A/B 会被别人的改动污染（上一轮已发生）。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）

## 0. 并行工作协议（先读，否则你的测量无效）

1. **开始前**把你将要修改的每个文件复制一份到 `.tmpdev/c1/orig/`（保持相对路径）。
   `git checkout` 不可用（`.git` 只读），恢复只能靠 `cp`。
2. **A/B 的正确做法**（关键）：
   - 构建"候选二进制" `bin-cand`；
   - 用 `cp` 把你改过的文件**恢复成开工前版本**，构建"基线二进制" `bin-base`；
   - `cp` 恢复你的改动；
   - 这两个二进制**只差你的改动**，即使树里还夹着其他 agent 的改动也不影响归因。
3. **构建要串行**：其他 agent 也在同一 `build_release` 上编译，
   所有 `cmake --build` 必须包一层文件锁：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
   （`TMPDIR` 必须指定，否则 Rust 组件因沙箱只读临时目录失败；没有 `flock` 就人工确认无并发编译。）
4. **实例隔离**：用你自己的端口与目录（建议 `--port 2891`、`--base-dir .tmpdev/c1/inst`），
   绝不使用 2881（其他 agent 可能占用）。
5. **不要 `pkill -f seekdb`**（会杀掉沙箱包装进程）；停实例用 `sdb.py stop --base-dir <dir>`。
6. **磁盘**：实例启动要求可用 >10GB，低于阈值报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`。

## 1. 当前基线（本机，三次中位；平台约 1.8 倍）

```
build_ik_all      2.750 s      （基线 3.754，+26.7%）
build_ik_content  2.293 s      （基线 2.954，+22.4%）
build_beng_en     1.229 s      （基线 1.640，+25.1%）
```

**目标**：三项各再降 **≥8%**（对应 build 类别 +20 个点以上）。

## 2. B-1（最高优先）：压缩"每行 0.96 µs"

### 2.1 依据
本机 `ik_content` = 2.29 s 分解为：**固定地板 0.45~0.55 s**（1 行数据建索引也要 0.55 s，已含 Agent A
删掉的 100 ms `usleep`）+ **1.92 M 行 × 0.96 µs ≈ 1.84 s**。
1.92 M 行 = `fts_index` 960k + `fts_doc_word` 960k + 两张映射表 40k。
**所以 build 的对手是每行 0.96 µs**，不是固定地板。

### 2.2 第一步（必须）：把 0.96 µs 拆成三段
在下列位置加临时计时（`getenv("OB_FTS_PROF")` 门控，采样 1/64，验证完删除）：

| 段 | 位置 |
|---|---|
| 行→store row 编码 | `src/storage/ddl/ob_tablet_slice_writer.cpp` 的 `append_row` → `ObMacroBlockWriter::append_row`（`src/storage/blocksstable/ob_macro_block_writer.cpp:684`）→ 微块编码 `ObMicroBlockWriter` |
| 微块追加 / 宏块 flush 触发 | 同上，`append_row(row, micro_block_size)` 与 `try_active_flush_macro_block()` |
| 排序比较 | DDL 内部 SQL 带 `ORDER BY`，排序在 PX 侧 sorter。统计每行平均比较次数与比较耗时（可在 sorter 的比较函数里累加计数） |

**产出**：三段各占多少纳秒/行。若三段之和仍明显小于 0.96 µs，继续找（列校验和
`report_ddl_column_checksum`、rowkey 编码、`ObDatumRow` 拷贝）。

### 2.3 按数据选一条实施（只做最大的一项）
- **若排序比较 >40%**（预测约 0.5 µs/行）：DDL 排序键是 `(word, docid)` 字符串键。
  评估把"比较排序"换成**分桶 + 段内排序**：按 word 首字节（或首 2 字节）分桶，
  桶内再按 `(word, docid)` 排。字符串键的公共前缀比较被消除，比较次数可降一个量级。
  ⚠️ 必须保证输出**仍然按目标表 PK 全局有序**（直载硬要求）；
- **若编码 >40%**：检查 `ObDatumRow` → store row 是否有可省的一次拷贝；
- **若列校验和 >20%**：**不能跳过**（正确性校验），只能看能否批量化（确认现在是否已按批）。

### 2.4 验收门
- build 三项各降 **≥8%**（3 块交替取中位）；
- 索引内容等价：36 形态 dump 仅允许 `order_rel` 差异；5 个仓库用例全过；
- **直载顺序**必须验证：建索引后 `SELECT ... MATCH` 返回正确结果集（顺序错会导致查询错）；
- 单步 <4% 就回退。

## 3. B-2：合并收尾阶段的 schema 变更

### 3.1 依据
`TAKE_EFFECT` 阶段（`enable_index` / `update_index_status_in_schema` / `release_ddl_kvs`）约 **20 ms/表**，
4 张 aux 表 ⇒ 约 80 ms；`ob_ddl_executor_util.cpp` 里 `wait_ddl_finish` 的轮询已由 Agent A 改过。
**只动收尾，不动数据通路** —— 因此不会重蹈"合并构建通路导致并行度下降 7.1%"的覆辙。

### 3.2 做法
- 先量：`TAKE_EFFECT` 里各步骤耗时（4 张表分别计时），确认是否真的 ~80 ms；
- 若确认：把 4 张表的 `enable_index` / schema 状态更新合并为**一次 schema 变更**；
- 若无法安全合并：改为**并行提交**这 4 次收尾（注意 schema 服务的并发限制与错误传播）。

### 3.3 验收门
- build 三项各降 **≥4%**；
- 建索引后立即可查（`SELECT ... MATCH` 返回正确结果）、`DROP INDEX` 正常、
  online DDL 加列后索引仍可用（DDL 专项 14 项）；
- 5 个仓库用例 + 命中数 8001/11000/7332/20 不变。

## 4. 正确性四关（每步都过）

1. 36 形态 correctness dump（`TMPDIR=/tmp bash .tmpdev/correctness_dump.sh <实例目录>`），
   与改动前逐字节比对，唯一允许差异是 `order_rel`；
2. 5 个仓库用例：
   `for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback count_only_fast_path; do MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done`
3. DDL 专项：`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`（14 项，0 失败）；
4. benchmark 命中数：`8001 / 11000 / 7332 / 20`。

## 5. 测量纪律
- **3 块交替取中位**（`TMPDIR=/tmp bash .tmpdev/ab_bins.sh <bin-base> <bin-cand> 3`）；
- `build_ik_all` 偶发 9~16 s 离群（clog 提交拥塞，与代码无关）⇒ **该块作废、重跑**；
- 宿主负载会让单次 build 在 2.4~4.5 s 之间乱跳，**禁止**用单次跑 vs 固定基线判定。

## 6. 交付要求
报告写清：
1. 三段计时的实测值（纳秒/行），以及它们如何解释 0.96 µs；
2. 选了哪一项实施、改了哪些文件、为什么；
3. 3 块交替 A/B 的三项 build 数据（中位 + 每块原始值 + 标注离群块）；
4. 正确性证据（dump 差异行数、5 个用例、DDL 14 项、命中数）；
5. B-2 是否实施；若未实施说明原因；
6. 未完成部分与原因；本地数字与平台分数的区分（平台 build ≈ 本机 1.8 倍）。
