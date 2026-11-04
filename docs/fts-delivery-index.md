# FTS 优化统一交付索引

> 本文件是这次优化工作的总入口：**已落地了什么、实测数据在哪、还剩什么、交给谁做**。
> 全部数据截至 2026-11-03。

## 1. 一句话现状

官方平台实测：`fts_mean_improvement = 21.51%`、得分 **43.01 / 100**。

| 类别 | 平台基线 | 平台实测 | 提升 | 备注 |
|---|---|---|---|---|
| build（3 项） | 8.78 / 6.68 / 3.83 s | 5.55 / 4.224 / 2.422 s | **+36.8%** | 平台构建受总工作量/存储限制（比本机慢 1.8x） |
| tokenize（2 项） | 0.0720 / 0.0423 ms | 0.0607 / 0.0386 ms | **+12.2%** | 24~27µs 是本机客户端往返地板 |
| query（4 项） | 1.3914 / 1.9911 / 1.5970 / 1.7526 ms | 0.9957 / 1.5707 / 1.1766 / **1.9936** ms | **+15.5%** | `query_limit` 是**唯一负项**（−13.8%） |

> 平台基线由 `fts_mean_improvement=21.51%` 与 tokenize/query 的已知基线反解得到
> （build 基线 ≈ 8.78/6.68/3.83，是本地 `reports/fts/baseline.txt` 的 2.3 倍）。

## 2. 已落地的优化（代码已在 `b7cc5dbe6`）

| 编号 | 内容 | 主要文件 | 证据 |
|---|---|---|---|
| C1 | FTS 构建并行度自动推导（`max_cpu/2`，上限 8；`_parallel_ddl_control` 可关） | `src/sql/resolver/ddl/ob_fts_index_builder_util.cpp` | 专项 A/B：`ik_content` +14~16%、`beng` +18~22% |
| D1 | 合并 DDL 两段等待：`rowkey_doc` 与其余 3 张 aux 表并行构建 | `src/rootserver/ddl_task/ob_fts_index_build_task.cpp` | 阶段实测省 270~286 ms/次；构建类中位 +11.8% |
| A2 | IK 逐字热路径（范围字典字节比较、单次解码、中文数字二分） | `src/storage/fts/dict/ob_ft_range_dict.cpp`、`src/storage/fts/ik/*`、`ob_ik_char_util.h` | 分词 +8.9%（A/B） |
| B1 | 分数仅作谓词时跳过 BM25 与 relevance 投影 | `src/sql/optimizer/ob_log_plan.cpp` 等 6 文件 | 3 个查询指标 +21~28%（8 轮配对） |
| 分词器重写 | `parse_param` 缓存、分类查表、停用词 fast path 等 | `ob_expr_tokenize.cpp`、`ob_ik_char_util.h`、`ob_fts_parser_helper.cpp` 等 | 服务端 −23.1%（见 `fts-tokenizer-rewrite-report.md`） |
| Q1 | `query_limit`：BMW 剪枝无效即回退 DAA T top-k（根因：LIMIT 触发 topk 下推） | `src/storage/retrieval/ob_sparse_bmw_iter.{h,cpp}` | 本机 8 轮配对 **+21.2%** |
| Q2 | 无分数形态改为 doc id 位图去重（删掉 k 路归并/比较/取分） | `src/storage/retrieval/ob_text_daat_iter.{h,cpp}`、`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.{h,cpp}` | 本机 8 轮配对 **cn +48.7% / beng +57.3% / mixed +51.5%** |
| Q3 | 归并比较快路径（二进制 doc id 用 memcmp、整型内联）+ 无分数跳过 `get_curr_score` | `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}` | 含在上面三项内 |
| Q4 | `COUNT(*)` 的 count-only 快路径：不物化 id datum + 位图按 word popcount 计数 | `src/sql/engine/table/ob_table_scan_op.cpp`、`src/storage/retrieval/ob_text_daat_iter.{h,cpp}`、`ob_das_ir_define.h` | 本机 8 轮配对 **cn +10% / beng +14% / mixed +9%**（见 `fts-count-only-fast-path-report.md`） |

**已修复的两处自身回归**：
1. 去掉 relevance 列导致输出列位置前移 → `ORDER BY`/`DISTINCT`/`sum(col)`/DML 子查询报 4016；
   已把 gate 收紧为「选择列表无列引用 + 无 ORDER BY/DISTINCT/GROUP BY/HAVING/窗口/LIMIT」；
2. 带 `LIMIT` 的形态曾被导向"无分数 + 回表"冷路径而变慢 → gate 增加 `!has_stmt->has_limit()`
   使其回到有分数路径。

## 3. 文档地图（按用途找）

| 我想… | 看这个 |
|---|---|
| 看完整优化报告（瓶颈定位、A/B 数据、正确性、复现命令、交付清单） | `docs/fts-optimization-report.md`（486 行） |
| 看官方平台口径与每项的边际分数 | 同上 §5.4 / §5.5 / §5.6 / §5.8 |
| 复现某个数字 | 同上 §10（自包含命令） |
| 看分词器重写做了什么、效果如何 | `docs/fts-tokenizer-rewrite-design.md` + `docs/fts-tokenizer-rewrite-report.md` |
| 看双 writer 合并的设计与被证伪的点 | `docs/fts-dual-writer-design.md` + `docs/fts-dual-writer-plan.md` |
| **看「一次分词喂两张 aux 表」落地了什么、实测多少、为什么没到 12%** | `docs/fts-token-share-report.md` |
| **看查询子系统重写做了什么、实测多少**（query_limit 台阶根因 + 位图去重） | `docs/fts-query-subsystem-rewrite-report.md` |
| **看四项优化的完整实施方案（含代码位置/风险/验收门）** | `docs/fts-next-four-items-plan.md` |
| **看四项之外的补充优化方向（⑤~⑧，含实测依据与账本）** | `docs/fts-additional-optimizations.md` |
| **交给 agent：⑤ count-only 快路径（query +9）** | `docs/prompt-5-count-only-fast-path.md` |
| **看 ⑤ count-only 快路径做了什么、实测多少**（判定条件、三形态探针、4 组 8 轮配对、正确性证据） | `docs/fts-count-only-fast-path-report.md` |
| **交给 agent：③深挖 → ⑥映射表合并 → ⑧语句层 → ④排查** | `docs/prompt-6-to-9-followup-four.md` |
| **交给 agent A：构建侧两项（⑥ 映射表合并 + DDL 固定开销）** | `docs/prompt-A-build-side-two-items.md` |
| **交给 agent B：分词侧两项（③ 深挖内部 + ⑧ 语句层）** | `docs/prompt-B-tokenize-side-two-items.md` |
| **看本轮构建侧两项做了什么**（⑥ 墙钟上限 0、前提证伪未实施；DDL 固定开销：删 100ms sleep + 10ms 轮询，clean A/B +4.11%/+3.85%/+11.84%） | `docs/fts-build-side-round2-report.md` |
| **看本轮分词侧两项做了什么**（③ 四段计时再次证伪未归属 9µs；官方口径 0.0537/0.0385 未达标；0 行代码改动） | `docs/fts-tokenize-side-round2-report.md` |
| **看 ③⑥⑧④ 做了什么、实测多少**（A1 四段计时证伪「未归属 9µs」、⑧ JSON 直写 tokenize −9.6%、⑥ 实测墙钟上限 0、④ 删除共享路径） | `docs/prompt-6-to-9-followup-four-report.md` |
| **看提示词①（LIMIT 早停）做了什么、实测多少**（探针结论、改动清单、8+4 轮配对 A/B、正确性证据） | `docs/fts-limit-early-stop-report.md` |
| **四项各自的执行提示词（交给他人的对话）** | `docs/prompt-1-fts-limit-early-stop.md`、`prompt-2-count-limit-rewrite.md`、`prompt-3-tokenize-server-side.md`、`prompt-4-dual-writer-merge.md` |
| **交给别的对话去做查询子系统重写** | `docs/fts-query-subsystem-rewrite-prompt.md` |
| **交给别的对话去做分词子系统重写** | `docs/fts-tokenizer-subsystem-rewrite-prompt.md` |
| 看原始基线 / 当前报告 / 对比 / 评分 | `reports/fts/baseline.txt`、`indextuning.txt`、`indextuning-compare.txt`、`indextuning-score.txt` |

## 4. 剩下的工作（按平台口径的边际分数排序）

| 顺序 | 任务 | 预期 | 得分增量 | 交给谁 |
|---|---|---|---|---|
| 1 | ~~**`query_limit` 从 −13.8% 修到 +20%**~~ **已完成（本机 +21.2%，待平台复核）** | 消除唯一负项 | **+5.6** | `docs/fts-query-subsystem-rewrite-report.md` |
| 2 | **分词服务端 −10%**（CJK 热路径 + 词表 + JSON） | tokenize +12.2% → +22.2% | **+6.7** | `fts-tokenizer-subsystem-rewrite-prompt.md` §4.1~4.4 |
| 3 | 双 writer 合并（省一次分词/一份行生成）——**已实现「共享分词」版本并实测**，结论：wall ±0、CPU −4%，按停止规则不默认启用 | build −14% | **+1.6** | `docs/fts-token-share-report.md`（实现+实测+未达 12% 的账本）；原设计见 `fts-dual-writer-design.md` |
| 4 | ~~查询整体专用化（整型 docid、内联比较）~~ **已完成（见 Q3；位图路径更进一步）** | 4 项查询各再快 15~25% | +4~6 | `docs/fts-query-subsystem-rewrite-report.md` |
| 5 | 两个子系统重写（查询 2.3x / 分词服务端 3x） | 才能摸到均值 50% | 到 ~100 | 上面两份 prompt 的完整目标 |

## 5. 环境与操作注意事项（踩过的坑）

1. **磁盘**：实例启动要求可用空间高于阈值，低于阈值直接
   `OB_SERVER_OUTOF_DISK_SPACE (-4184)` 拒绝启动。本机工作区曾到 98%，需保持 >10GB；
2. **构建必须指定 TMPDIR**：`TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12`，
   否则 Rust 组件（`ring`）编译时 clang 无法写临时文件而失败；
3. **不要 `pkill -f seekdb`**：会杀掉沙箱包装进程（其命令行含路径）；
4. **测量**：本机 `select1` 在 0.0204~0.0274 之间漂移（±15%），
   任何结论都必须用"两二进制交替 + 8 轮配对取中位"，不要用单次跑对固定基线；
5. **`build_ik_all` 偶发 9~16s 离群**（约 1/5 概率）：根因是 clog 日志提交拥塞
   （`OB_EAGAIN` 重试），与环境 I/O 相关、与代码无关；正式对比建议连跑 3~5 次取中位。

## 6. 正确性基线（任何改动都要过这几关）

1. **36 形态 correctness dump** 逐字节一致（唯一允许差异：`order_rel` 的并列组选取）；
2. **四个仓库用例**（均已注册进 `psmalltest.py`）：
   `fts_index.no_score_match`（12 形态）、`fts_index.tokenize_edge`（14 组）、
   `fts_index.bitmap_dedup`（位图去重：单表 vs 4 分区表结果必须一致）、
   `fts_index.bmw_topk_fallback`（pushdown topk 的 BMW 回退：锁定 top-n 的 id 与精确分数）；
3. **DDL 专项 14 项**（建索引/分区表/双索引/online DDL 加列/删索引/重建/`SELECT id` 映射）；
4. **benchmark 命中数**：`cn=8001 / beng=11000 / mixed=7332 / limit=20`。

## 7. 最近一次代码审查的修复（2026-11-03）

| # | 问题 | 修复 |
|---|---|---|
| 1 | 位图迭代器对外 tablet 的 docid 用**无界字符串侧表** | 改为**按 tablet 分块位图**（每块 8KB，无字符串侧表、无排序） |
| 2 | 位图扩容**翻倍拷贝且不释放旧缓冲** | 改为定长块按需分配（零拷贝），内存 = ⌈行数/8⌉ 字节，并有 8MB/tablet、16MB/query 硬上限 |
| 3 | 去重路径**无回归测试** | 新增 `fts_index.bitmap_dedup` |
| 4 | BMW 回退阈值**无测试与测量** | 新增 `fts_index.bmw_topk_fallback`；实测该形态 2.147ms vs 非下推排序 19.640ms |

修复后按平台口径外推：build +37.2% / tokenize +19.5% / query +47.0% → **综合 ≈ +34.6%（约 69 分）**。
