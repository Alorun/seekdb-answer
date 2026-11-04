# 提示词 C3：查询侧低风险收尾（Q-1 倒排扫描批次 + Q-2 emit 批内提示）

> 自包含提示词。你可以与另外两个 agent **并行**工作（构建侧 / 分词侧），
> 因此**必须遵守 §0 的测量隔离协议**。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
>
> 查询类当前已达 **+69.3%**，本文件只剩约 **+6 个点**空间，属于收尾性质：
> **门槛严格、收益小就回退**，不要为了 1% 引入复杂度。

## 0. 并行工作协议（先读）

1. **开工前**把要改的文件复制到 `.tmpdev/c3/orig/`（`.git` 只读，恢复只能靠 `cp`）。
2. **A/B 正确做法**：构建 `bin-cand` → `cp` 恢复为开工前版本 → 构建 `bin-base` →
   `cp` 恢复你的改动；两个二进制只差你的改动。
3. **构建串行**：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
4. **实例隔离**：用自己的端口 `--port 2893`、目录 `.tmpdev/c3/inst`，不要用 2881。
5. **不要 `pkill -f seekdb`**；停实例用 `sdb.py stop`。磁盘需 >10GB。

## 1. 当前基线

```
query_cn     0.5344 ms（基线 1.3914，+61.6%）   平台 0.5262
query_beng   0.7237 ms（基线 1.9911，+63.7%）   平台 0.7255
query_mixed  0.6333 ms（基线 1.5970，+60.3%）   平台 0.6024
query_limit  0.2671 ms（基线 1.7526，+84.8%）   平台 0.1877
```

**目标**：`query_cn / beng / mixed` 各再快 **≥3%**（query 类别 +6 个点）。

### 已做过的（不要重复）
- 无分数 gate（跳过 BM25）、文档 ID 位图去重（按 tablet 分块）、count-only（不物化 id datum）、
  LIMIT 下推、`COUNT(*)` 套 `LIMIT` 代数改写（`query_limit` 已 0.19~0.27ms）。

### 已证否的（不要重复）
- 手工 k 路归并替代堆（−0.69%）；去 `filter_on_demand` 的 guard（+0.14%）；
- 计划缓存问题（字面量 vs 预处理只差 8%，解析仅 0.037 ms）。

## 2. Q-1：倒排扫描批次 256 → 1024

### 2.1 依据
`query_cn` 0.534 ms 的成本分解：12669 条 posting × **21 ns ≈ 266 µs**（倒排扫描，接近地板）
+ 位图构建/emit 约 40~60 µs + 语句层约 37 µs + 约 150 µs 未细分。
若存储扫描存在**每批固定开销**（约 1 µs/批），12669/256 = **50 批** ⇒ 约 50 µs 可省（约 −9%）。

### 2.2 做法
1. 先在 `ObTextRetrievalTokenIter::get_next_batch`
   （`src/storage/retrieval/ob_text_retrieval_token_iter.cpp`）加计时：
   记录每次调用的 `capacity`、返回的 `count`、以及该次调用耗时；
2. 判断每批固定开销是否显著（若每批耗时远大于 count × 21 ns，则固定开销显著）；
3. 若是：把批次容量从 `max_batch_size_`（256）提高到 **1024**，注意两者的约束：
   - `iter_param_->max_batch_size_` 与 `eval_ctx->max_batch_size_`（表达式 vector size）
     可能绑定，必须同时调整或确认调用方接受更大 `count`；
   - `ObTextDocIdBitmapIter::emit_rows` 的 `real_capacity` 也要同步（它取三者的 MIN）；
4. 若约束无法打破（例如 `eval_ctx->max_batch_size_` 是编译期常量），**放弃本项**，
   在报告里说明，不要强行改。

### 2.3 验收门
- `query_cn` 提升 **≥3%**（8 轮配对中位）；若仅 1~2% 则回退；
- 命中数不变（8001/11000/7332/20）；5 个仓库用例全过；
- 36 形态 dump 仅 `order_rel` 差异。

## 3. Q-2：`emit_rows` 的批内提示（低优先，顺手做）

### 3.1 依据
`ObTextDocIdBitmapIter::emit_rows`（`src/storage/retrieval/ob_text_daat_iter.cpp`）在 count-only 模式下
已经跳过 datum 物化；但 `guard.set_batch_idx(count)` 与循环结构里可能还有 per-row 可省调用。

### 3.2 做法
- 检查 `emit_rows` 里是否还有 per-row 的表达式/标志操作可外提；
- 确认 `count_only_` 路径下 `emit_buffer_` 完全未使用（可省去分配）。

### 3.3 验收门
- `query_cn` 提升 **≥1.5%** 才保留（本机噪声地板）；否则回退。

## 4. 正确性四关（每步都过）

1. 36 形态 dump，唯一允许差异 `order_rel`；
2. 5 个仓库用例：`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path`；
3. DDL 专项 14 项；
4. 命中数 `8001 / 11000 / 7332 / 20`。

## 5. 测量纪律
- **8 轮配对取中位**（`TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <bin-base> <bin-cand> 8 2`）；
- 查询类噪声地板约 1.5%，`select1` 漂移 0.0204~0.0358；
- 禁止用单次跑 vs 固定基线判定。

## 6. 交付要求
报告写清：
1. 每批扫描的实测（capacity / count / 每批耗时 / 每 posting 摊到的固定开销）；
2. 是否改动批次容量；若因约束放弃，写明具体约束在哪；
3. 每项的 8 轮配对数据（中位 + stderr）；
4. 正确性证据；
5. `build_*` / `tokenize_*` 是否受影响（应无影响，需确认）；
6. 未完成部分与原因。
