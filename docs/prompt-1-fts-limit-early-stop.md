# 提示词 ①：让 LIMIT 在 FTS 扫描里真正早停

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）

## 0. 目标与价值

官方平台口径：`query_limit_avg_ms` 基线 1.7526、当前 **1.9936（−13.8%，是九项指标里唯一的负项）**；
其余三个查询指标已 +21~28%。查询类占综合的 1/3 权重，`query_limit` 占查询类的 1/4。

**目标**：`query_limit_avg_ms` ≤ **0.35 ms**（相对基线 +80%），使查询类均值再 +19 个百分点。

**不许改变返回的行集合与行数**（无 ORDER BY 时允许顺序不同，但集合必须一致）。

## 1. 已实测的证据（不要重复推导）

同一张 20k 文档表、`ik` 解析器，`SELECT id FROM docs WHERE MATCH(content) AGAINST(...) LIMIT n`：

| token 数（命中数） | `LIMIT 1` | `COUNT(*)`（位图路径） |
|---|---|---|
| 1 词（6667） | **0.303 ms** | 0.636 ms |
| 2 词（9333） | **1.740 ms** | 0.872 ms |
| 3 词（11999） | **2.845 ms** | 0.908 ms |

并且 2 词时 `LIMIT 1/5/10/16/20/32/64` 全部是 **1.62~1.70 ms**（与 limit 值无关）。

结论：**单 token 能早停，多 token 完全不能**；成本随 token 数增长，与本应取的行数无关。
`query_limit`（`COUNT(*) FROM (SELECT id ... LIMIT 20)`）实测 1.57~1.99 ms。

## 2. 第一步：30 分钟探针，先确认根因（不改逻辑）

两个可能，必须用数据排除，不要猜：

**(a) LIMIT 有没有下发到 FTS 扫描？**
- 参数来源：`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp:376`
  `sr_iter_param_.limit_param_ = &ir_rtdef_->get_inv_idx_scan_rtdef()->limit_param_;`
- 在 `ObSRDaaTIterImpl::get_next_rows`（`src/storage/retrieval/ob_sparse_daat_iter.cpp`）入口
  打印一次 `limit_param_->is_valid() / limit_ / offset_`（用 `getenv("OB_FTS_PROF")` 门控，用完删除）。
- 跑 `SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20;`
- 若 `is_valid()==false` → 走方案 **A**；若有效但仍扫全量 → 走方案 **B**。

**(b) 是否被"批量填满"吃掉？**
- `ObTableScanOp` 每批向 DAS 要 256 行（`rowset=256`），DAAT 的 `get_next_rows(capacity=256)`
  要把 256 行都产出才返回；上层的 LIMIT 只取 1 行、丢弃其余 255。
- 验证：在 `get_next_rows` 里打印每次调用的 `capacity` 与本批 `count`，看单次调用是否总在填满 256。

## 3. 实施（二选一，按探针结论）

### 方案 A：把 LIMIT 下推进 text retrieval scan
- 位置：`src/sql/optimizer/ob_log_plan.cpp` 生成 `ObLogTableScan` 的 limit 相关逻辑，以及
  `ObDASIRScanCtDef` / `ObDASIRScanRtDef` 的 limit 参数传递链。
- 安全前提（必须逐条成立才可下发）：FTS 扫描没有 `ORDER BY MATCH(...)`（即
  `has_pushdown_topk()` 为假）、没有 DISTINCT/GROUP BY/窗口函数、没有分数过滤。
  满足时返回的行**集合不变**，只是少读。
- 注意：`ORDER BY MATCH(...) DESC LIMIT n` 走的是 pushdown topk（BMW）路径，
  已有 `has_pushdown_topk` 门控，改这条链路时**不要影响它**（改完必须回归
  `fts_index.bmw_topk_fallback`）。

### 方案 B：DAAT 批次按剩余 limit 收缩
- 位置：`src/storage/retrieval/ob_sparse_daat_iter.cpp` 的 `get_next_rows` 入口，
  现有 `const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);`
- 改为：
  ```cpp
  int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
  if (OB_NOT_NULL(iter_param_->limit_param_) && iter_param_->limit_param_->is_valid()) {
    real_capacity = MIN(real_capacity, MAX(iter_param_->limit_param_->limit_, 1));
  }
  ```
  `offset` 已由 `cache_result` 处理（`input_row_cnt_ <= offset_` 的行不计入 `count`），
  无需在此扣减；`cache_result` 里已有的 `output_row_cnt_ >= limit_` 提前返回保持不变。
- 同时检查 `ObTextDocIdBitmapIter::emit_rows`（`src/storage/retrieval/ob_text_daat_iter.cpp`）
  的 `real_capacity` 是否也需要同样收缩。

## 4. 硬约束

1. 返回的**行集合与行数必须与改动前一致**（无 ORDER BY 时顺序不保证，集合必须一致）；
2. `COUNT(*) FROM (SELECT id ... LIMIT 20)` 必须恒为 `20`（命中数 ≥20 时）；
3. 不改 benchmark 数据/SQL/计时/评分/基线；不跳过 LOB 读取、不减 posting 扫描量（除非 limit 确实允许）；
4. 不改变 MATCH/BM25 语义与分数。

## 5. 验证（每一步都做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
# 构建：必须指定 TMPDIR，否则 Rust 组件编译时无法写临时文件
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12
# 磁盘：实例启动要求可用空间 >10GB，否则报 OB_SERVER_OUTOF_DISK_SPACE(-4184)
df -h .
```

- **行集合一致性**：改动前后各跑一遍
  `SELECT id FROM docs WHERE MATCH(content) AGAINST('<2 词>') LIMIT 20`（排序后比较集合）；
- 36 形态 correctness dump 逐字节一致（唯一允许差异：`order_rel` 的并列组选取）；
- 4 个仓库用例必须 PASS：
  `for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done`
- DDL 专项：`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`（14 项，0 失败）；
- benchmark 命中数：`cn=8001 / beng=11000 / mixed=7332 / limit=20`；
- **性能**：同机两二进制交替、8 轮配对取中位
  `TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <旧二进制> <新二进制> 8 2`
  （本机 `select1` 漂移 0.0204~0.0285，**禁止**用"单次跑 vs 固定基线"下结论）。

## 6. 环境坑（都踩过）

1. `git checkout/stash` 会失败（`.git` 只读）。要对比改动，用 `cp` 备份文件再改回来；
2. 不要 `pkill -f seekdb` —— 会杀掉沙箱包装进程（其命令行含路径），要杀实例用
   `python3 .github/script/seekdb/sdb.py stop --base-dir <dir>`；
3. 实例一律用 `.github/script/seekdb/sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2881` 启动；
4. `build_ik_all` 偶发 9~16s 离群（约 1/5，clog 提交拥塞，与代码无关）——看 build 指标时连跑 3 次取中位。

## 7. 交付要求

报告里写清：
1. 探针结论（limit 是否下发、每次调用填满多少行）+ 证据；
2. 采用 A 还是 B，改了哪些文件、每处作用；
3. 改动前后 `query_limit_avg_ms` 与 `SELECT id LIMIT n` 的 8 轮配对数据（中位 + stderr）；
4. 行集合一致性证据、4 个用例、dump 差异行数、命中数；
5. 未完成的部分及原因；本地数字与平台分数的区分。
