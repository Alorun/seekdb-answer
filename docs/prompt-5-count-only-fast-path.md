# 提示词 ⑤：`COUNT(*)` 的 count-only 快路径（不物化 id datum）

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`，HEAD `967455316`）

## 0. 目标与价值

平台口径现状：`query_cn` 0.6585 ms、`query_beng` 0.8334 ms、`query_mixed` 0.7550 ms
（三者都已走"无分数 + 位图去重"路径），`query_limit` 已降到 0.2701 ms。

**目标**：让 `query_cn / query_beng / query_mixed` 各自再快 **≥10%**
⇒ 查询类均值 +9 个百分点（三类之和 112.4% → 121.4%，约 +4.5 分）。

**不改语义**：命中数必须仍是 `8001 / 11000 / 7332 / 20`。

## 1. 依据（实测，不要重复推导）

1. `EXPLAIN SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST(...)` 显示
   TSC 的输出是 `output([T_FUN_COUNT(*)])` —— **计数由扫描算子自己完成，
   上层的任何算子都不读 id 列**。
2. 但 `ObTextDocIdBitmapIter::emit_rows`
   （`src/storage/retrieval/ob_text_daat_iter.cpp:405~407`）仍在**逐行**做：
   ```cpp
   id_datums[count].set_string(buf, OB_DOC_ID_COLUMN_BYTE_LENGTH);  // 16B 写
   id_flags.set(count);                                             // 位图写
   id_proj_expr->set_evaluated_projected(*eval_ctx);                // 函数调用
   ```
3. 实测 emit 段约 **160 µs / 800 µs**（8001 行 × 约 20 ns），即该项最多能拿 20%。

## 2. 第一步（必须先做）：确认"id 列是否真的没人读"

不要凭 EXPLAIN 下结论，要用代码事实确认。可行的判断方式（择一，按可行性）：

- **方式 A（推荐）**：`ObDASIRScanCtDef` 里有 `inv_scan_domain_id_col_`（`src/sql/das/ob_das_ir_define.h:190`）。
  检查该 expr 是否出现在本次扫描的**输出列表**里（`ObTableScanOp` 的 `get_spec().output_`，
  或 cg 阶段传给 DAS 的 output exprs）。若不在 → 没有人会读这个 datum → 可走 count-only。
- **方式 B**：在 `ObDASLegacyTRMergeIter::is_doc_id_bitmap_applicable()`
  （`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp:1165`）里
  追加一条"输出里只有聚合表达式"的判断。

**三种形态必须分别验证**（写进测试）：
1. `SELECT COUNT(*) ... WHERE MATCH(...)` → **应启用** count-only；
2. `SELECT id ... WHERE MATCH(...)` → **不得启用**（id 被读）；
3. `SELECT COUNT(*), SUM(id) ... WHERE MATCH(...)` → **不得启用**（id 被读）。

## 3. 实施

1. 在 `ObSparseRetrievalMergeParam`（`ob_i_sparse_retrieval_iter.h`）或
   `ObTextDocIdBitmapIter` 内加一个 `count_only_` 标志，由 `is_doc_id_bitmap_applicable()`
   的判定结果设置（构造函数/`init()` 传入）。
2. `emit_rows` 在 `count_only_` 为真时：
   - 仍然推进位图游标、仍然 `++count`、仍然 `++output_row_cnt_/input_row_cnt_`；
   - **跳过** `set_string` / `id_flags.set` / `set_evaluated_projected`；
   - 注意：此时 `emit_buffer_` 不需要分配，但 **`count` 必须准确**（上层只数它）。
3. 保守：`count_only_` 只在现有位图 gate 的同一组条件下启用
   （无 relevance 投影、无 filter、无 topk、自然语言模式、多 token），不要扩大适用范围。

## 4. 硬约束

1. 命中数与结果集不变；`COUNT(*)`、`COUNT(*), SUM(id)` 等形态结果必须与改动前一致；
2. 不得改动 `SELECT id ...` 形态（它必须继续物化 datum）；
3. 不改 benchmark 数据/SQL/计时/评分/基线；不改 MATCH/BM25 语义。

## 5. 验证（每一步都做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12   # 必须带 TMPDIR
df -h .                                                                    # 需 >10GB 可用
```

1. **三种形态的 EXPLAIN + 结果**：`COUNT(*)` / `SELECT id` / `COUNT(*), SUM(id)`，
   确认只有第一种走了 count-only（可用临时日志或 `EXPLAIN` 佐证），且三者结果不变；
2. 36 形态 correctness dump 逐字节一致（唯一允许差异：`order_rel`）；
3. 4 个仓库用例全过：
   `for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done`
4. DDL 专项：`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`（14 项，0 失败）；
5. benchmark 命中数 `8001 / 11000 / 7332 / 20`；
6. **性能**：同机两二进制交替、8 轮配对取中位
   `TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2`
   判定门槛：`query_cn` 提升 **≥3%** 才保留（本机噪声地板 1.5~2%），目标 ≥10%。

## 6. 环境坑

1. `git checkout/stash` 失败（`.git` 只读），对比改动用 `cp` 备份；
2. 不要 `pkill -f seekdb`（会杀掉沙箱包装进程）；停实例用 `sdb.py stop`；
3. 实例启动：`sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2881`；
4. 宿主负载会让单次基准在 2.4~4.5 s 之间乱跳（`build_*` 尤其明显），
   **禁止**用单次跑 vs 固定基线判定；查询类请用 8 轮配对；
5. `select1` 在 0.0204~0.0358 之间漂移，配对时留意两臂的 `select1` 是否接近。

## 7. 交付要求

报告写清：
1. 你用什么方式确认"id 列不被读取"，以及三种形态的验证结果；
2. 改了哪些文件、`count_only_` 的判定条件；
3. 三个查询指标各 8 轮配对数据（中位 + stderr）；
4. 正确性证据（dump 差异行数、4 个用例、DDL 14 项、命中数）；
5. 未完成部分与原因；本地数字与平台分数的区分。
