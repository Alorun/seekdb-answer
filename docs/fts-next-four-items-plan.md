# 四项优化实施方案（目标：三类之和 150% → 满分）

> 口径：综合 = (build 类均值 + tokenize 类均值 + query 类均值) / 3。
> 满分要求综合 ≥ 50%，即**三类之和 ≥ 150%**，不要求每项都到 50%。
>
> 平台口径现状（2026-11-03 反解 + 修复后外推）：
> build **+36.8%**、tokenize **+19.5%**、query **+45.4%** → 和 **101.7%**（约 68 分）。
> 缺口 **+48.3 个百分点**。

| 编号 | 措施 | 预期类别增量 | 归属类别 |
|---|---|---|---|
| ① | FTS 扫描里让 `LIMIT` 真正早停 | query **+19** | query |
| ② | `COUNT(*)` 套 `LIMIT` 子查询的代数改写（① 的兜底路线） | query +10 | query |
| ③ | TOKENIZE 服务端再降 30% | tokenize **+11** | tokenize |
| ④ | 构建双 writer 合并（省一次分词/一份行生成） | build **+13** | build |

全做后：101.7 + 19 + 11 + 13 = **144.7%**（约 96.5 分）；② 若与 ① 叠加可再 +2~3。

---

## ① FTS 扫描里让 LIMIT 真正早停（query 类 +19）

### 证据（已实测）
```
1 token  (6667 命中): SELECT id ... LIMIT 1 = 0.303 ms   COUNT(*)(位图) = 0.636 ms
2 token  (9333 命中): SELECT id ... LIMIT 1 = 1.740 ms   COUNT(*)(位图) = 0.872 ms
3 token (11999 命中): SELECT id ... LIMIT 1 = 2.845 ms   COUNT(*)(位图) = 0.908 ms
LIMIT 1 / 5 / 10 / 16 / 20 / 32 / 64 在 2 token 上全是 1.62~1.70 ms（与 limit 值无关）
```
结论：**单 token 能早停（0.30 < 0.64），多 token 完全不能**，成本随 token 数增长而与本应取的行数无关。
`query_limit`（`COUNT(*) FROM (SELECT id ... LIMIT 20)`）因此停在 1.57ms，相对基线只有 +10.5%，
而同类另外三项已 +54~59%。

### 根因假设与确认方法（先做 30 分钟探针，不改逻辑）
两个可能，逐个排除：
1. **limit 没下发到扫描**：`ObDASLegacyTRMergeIter::create_sparse_retrieval_iter()` 里
   `sr_iter_param_.limit_param_ = &ir_rtdef_->get_inv_idx_scan_rtdef()->limit_param_;`
   （`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp:376`）。
   在 DAAT 的 `get_next_rows` 入口打印 `limit_param_->is_valid() / limit_ / offset_`：
   - 若 `is_valid()==false` → 优化器/CG 没把 LIMIT 下推进 FTS 扫描，走 (A)；
   - 若有效但仍扫全量 → 是批量填充问题，走 (B)。
2. **批量填充**：`ObTableScanOp` 每次向 DAS 要 256 行，DAAT 的 `get_next_rows` 会填满 256 才返回，
   上层的 LIMIT 只取 1 行就丢弃其余 255 —— 单 token 时这 256 行便宜，多 token 时每行都要付
   归并 + 回表代价，于是"取 1 行"也要 ~1.7ms。

### 实施步骤
**(A) 若 limit 未下发**：在计划/代码生成阶段把 LIMIT 下推进 text retrieval scan。
- 位置：`src/sql/optimizer/ob_log_plan.cpp` 里生成 `ObLogTableScan` 的 limit 相关逻辑，
  以及 `ObDASIRScanCtDef`/`ObDASIRScanRtDef` 的 limit 参数传递。
- 只要满足"FTS 扫描是无序输出（无 ORDER BY 分数、无 DISTINCT）"就可以安全下发：
  返回的**行集合不变**，只是少读。
- 改动后必须验证 `ORDER BY MATCH(...)` 的 pushdown topk 路径不受影响（那条走 `has_pushdown_topk`）。

**(B) 若已下发但被批量填充吃掉**：让 DAAT 的批次容量按剩余 limit 收缩。
- 位置：`src/storage/retrieval/ob_sparse_daat_iter.cpp` 的 `get_next_rows` 入口
  （已有 `real_capacity = MIN(capacity, max_batch_size_)`）：
  ```cpp
  int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
  if (iter_param_->limit_param_->is_valid()) {
    const int64_t remain = iter_param_->limit_param_->limit_;
    real_capacity = MIN(real_capacity, MAX(remain, 1));
  }
  ```
  注意 offset 已由 `cache_result` 处理（`input_row_cnt_ <= offset` 时不计入），
  所以这里按 `limit_` 收缩即可；`cache_result` 已有的 `output_row_cnt_ >= limit_` 提前返回保持不变。
- 同步检查 `ObTextDocIdBitmapIter`（它固定按 `max_batch_size_` 产行）是否需要同样收缩。

### 风险与回退
- 唯一语义风险：错误的早停会**少返回行**。因此必须验证：
  `SELECT id ... LIMIT n` 的**行数与行集合**与改动前一致（无 ORDER BY 时只校验行数与集合，
  不校验顺序），且 `COUNT(*) FROM (... LIMIT 20)` 恒为 20。
- 回退：改动集中在 1~2 处，逐处可独立回退；建议用一个开关（如 `_enable_fts_limit_pushdown`）灰度。

### 验收
- `query_limit_avg_ms` ≤ **0.35 ms**（当前 1.57，基线 1.7526 → +80% 以上）；
- `SELECT id ... LIMIT 1` 在 2/3 token 上 ≤ 0.35 ms；
- 命中数不变（20/8001/11000/7332），4 个仓库用例全过，36 形态 dump 无新增差异。

---

## ② `COUNT(*)` 套 `LIMIT` 子查询的代数改写（① 的兜底，query 类 +10）

若 ① 因计划层限制无法安全下发，用代数改写拿到同样的收益。

### 依据
`SELECT COUNT(*) FROM (SELECT <cols> FROM t WHERE <cond> LIMIT n) x` 的结果恒等于
`LEAST(n, (SELECT COUNT(*) FROM t WHERE <cond>))`，**前提**：内层无 DISTINCT / GROUP BY /
聚合 / HAVING / 窗口函数 / 集合运算，外层无 WHERE / GROUP BY / HAVING。

### 实施步骤
1. 在优化器里新增（或扩展）一条改写规则，匹配上述形态后把内层 LIMIT 与聚合折叠成 `LEAST(n, COUNT(*))`；
2. 折叠后是普通 `COUNT(*)`，会命中已有的"分数只作谓词"gate → 走 **位图去重**路径（实测 0.87ms）；
3. 位置：`src/sql/optimizer/`（`ob_transform_*` 或 `ob_log_plan` 的聚合下推处）。
   若不想引入通用规则，可只在"内层唯一表源 + WHERE 恰为单个 MATCH + 自然语言模式"时改写，
   把影响面限制在 FTS 上。

### 风险与回退
- 这是**通用语义改写**，风险高于 ①：任何"内层行数被其它算子约束"的遗漏都会算错行数。
- 必须限定上述前提，并加负向用例（内层带 DISTINCT / GROUP BY / 聚合时**不得**改写）。
- 回退：规则级开关（session 变量或 `_enable_*`），默认开、异常即关。

### 验收
- `query_limit` ≤ 0.95 ms（位图路径 + 20 行产出）；
- 新增用例：同一批数据下 `COUNT(*) FROM (… LIMIT 20)` 与 `LEAST(20, COUNT(*))` 结果相等，
  且 ≥20 命中时恒为 20、<20 命中时等于命中数；
- 负向用例：内层带 DISTINCT / GROUP BY / 聚合时**结果不变**（改写未触发）。

---

## ③ TOKENIZE 服务端再降 30%（tokenize 类 +11）

### 现状（实测）
`tokenize_ik` ≈ **0.0593 ms**（本机，`select1`=0.0254），其中客户端往返 **不可优化**。
已测相位：`parse_param` 0.56µs、`tokenize_fulltext` **23.2µs**、`pack_json_res` 1.9µs。
`tokenize_fulltext` 内已知：CJK 逐字 ≈10µs、词表 ≈2µs、JSON ≈1µs ⇒ **约 10µs 未归属**。

### 第 0 步（必须先做）：把那 10µs 拆开
在 `ObFTParseHelper::init`、`ObFTWordMap::create`、`segment()`、`make_token_array_json`
四处加临时计时（`OB_FTS_PROF` 门控，用完删除），跑 4000 次取稳态，得到四项均值。
**没有这一步就不要动代码**——上一轮的教训是"以为的大头其实只有 1.1µs"。

### 按结论选一条（预期最大者优先）
1. 若 `init`（重建解析器 + 三本词典）占 ≥3µs：做**解析器/词典句柄缓存**
   —— 线程局部、容量 8、键为 `(parser_name, properties 的哈希)`，值为已解析的属性结构
   （不持有临时 allocator 指针）；或退一步做成"每个 `ObFTParseHelper` 实例内复用"。
2. 若 `segment` 仍占 ≥12µs：继续压 CJK 热路径
   —— `match_with_hit` 调用次数、`ctx.add_token` 的 word 拷贝（可共享同一次分词的内存）、
   字母/数量词/代理对处理器的空转调用合并（按字符类型位掩码一次分派）。
3. 若 `make_token_array_json` 占 ≥3µs：直接按二进制 JSON 布局写 `["w1","w2",…]`，
   跳过 `ObJsonArray/ObJsonString` 中间树，要求与现有 `get_raw_binary` **逐字节一致**。

### 硬约束
- TOKENIZE 输出**逐字节不变，含 token 顺序**（当前是哈希桶序，证据见 `tokenize_edge.result`）；
- 分词语义不变；`build_*` 不得退化（构建走同一个 `segment()`）。

### 验收
- `tokenize_ik` ≤ **0.049 ms**、`tokenize_beng` ≤ **0.033 ms**；
- `tokenize_edge`（14 组）与 36 形态 dump 中所有 TOKENIZE 输出逐字节一致；
- 8 轮配对 A/B 判定（单步 <3% 回退）。

---

## ④ 构建双 writer 合并（build 类 +13）

### 平台依据（重要，与本地不同）
平台 build = 5.55 / 4.224 / 2.422 s，是本机的 **1.8 倍**，而 tokenize/query 与本机同速
⇒ 平台构建受**总工作量/存储**限制（核数更少），不是并行度不足。
因此"少做一次分词、少生成一份行"能直接换成墙钟收益。

设计细节见 `docs/fts-dual-writer-design.md`，这里给出落地顺序与修正点：
1. `generate_fulltext_word_rows_dual()`：一次 `segment()` 产出两组行
   （`[word,docid,cnt,len]` 与 `[docid,word,cnt,len]`），共享 word 字符串内存；
   保留现有单表版本用于回退。
2. PX 直载算子双 writer：`ObPxMultiPartSSTableInsertOp` 增加"行对"输出与两个 writer，
   **缓存上一次的 (tablet_id, writer)**，避免每行哈希查找（实测该项收益仅 0.08%，不要指望它）。
3. DDL 编排：`fts_index` 子任务带写 `fts_doc_word`，`fts_doc_word` 子任务不再独立构建；
   两张表**同进同退**（任一失败整任务失败，走既有重试）。
4. 配置开关 `_enable_fts_dual_writer`，默认关时完全等于现状。

### 风险与回退
- 两表数据不一致：靠"同进同退 + 整任务重试"保证；
- 每个 writer 独立 sorter/buffer，不共享；
- 回退：开关置 0。

### 验收
- `build_*` 相对现状降 **≥12%**（8 轮/3 块交替，取中位）；
- 4 个仓库用例 + DDL 专项 14 项全过；索引内容等价（36 形态 dump 仅 `order_rel` 差异）；
- kill -9 重启后 DDL 能恢复或正确重试。

---

## 执行顺序与决策门

| 顺序 | 事项 | 前置 | 决策门（不达标就回退该项） |
|---|---|---|---|
| 1 | ① 的探针（limit 是否下发） | — | 30 分钟内出结论；若既没下发也无法安全下发 → 转 ② |
| 2 | ① 实施（A 或 B） | 探针结论 | `query_limit` ≤0.35ms 且行数/集合不变 |
| 3 | ③ 第 0 步相位拆分 | — | 四项均值必须能解释 ≥20µs，否则先查语句层开销 |
| 4 | ③ 按结论实施 1~2 处 | ③第0步 | `tokenize_ik` ≤0.049ms、输出逐字节不变 |
| 5 | ④ 三步落地 | — | 构建 ≥12%、两表内容一致 |
| 6 | ② 仅在 ① 失败时启用 | ① 结论 | 结果恒等于 `LEAST(n, count)` |

## 全局纪律（每次都做）

1. **构建**：`TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12`；
2. **磁盘**：实例启动要求可用 >10GB，低于阈值报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`；
3. **测量**：同机两二进制交替 + 8 轮配对取中位（`ab_pingpong.sh`），
   不要用"单次跑 vs 固定基线"（本机 `select1` 漂移 0.0204~0.0285，±15%）；
4. **正确性四关**：36 形态 dump（仅 `order_rel` 允许差异）、4 个仓库用例
   （`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback`）、
   DDL 专项 14 项、benchmark 命中数（8001/11000/7332/20）；
5. **单步收益 <3% 或出现任何输出差异 → 回退该步**，不要叠加复杂度。

## 预期结果

| 阶段 | 和（%） | 平均 | 得分 |
|---|---|---|---|
| 现状 | 101.7 | 33.9 | ~68 |
| +① | 120.7 | 40.2 | ~80 |
| +① +③ | 131.7 | 43.9 | ~88 |
| +① +③ +④ | **144.7** | **48.2** | **~96** |
| +② 叠加（若 ① 部分生效） | ~148 | ~49.3 | ~99 |
