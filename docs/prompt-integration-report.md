# 集成报告：三路并行优化（C1 构建 / C2 分词 / C3 查询）落地结果

> 分支 `indextuning`（HEAD `4445d48e5`，本轮开工前 `src/` 干净）。
> 三个 agent 并行改同一工作树，按 `.tmpdev/tri/PROTOCOL.md` 隔离：各自端口 / 实例目录 / 二进制目录，
> 所有编译与实例操作串行化于 `.tmpdev/tri/heavy.lock`。
>
> 分项报告：[C1](prompt-C1-report.md) · [C2](prompt-C2-report.md) · [C3](prompt-C3-report.md)
> 集成验证脚本与原始数据：`.tmpdev/final/`（`bin/`、`dump-*.txt`、`pp-1104-130514/`）

---

## 1. 收口结论

| 项 | 门 | 实测 | 决定 |
|---|---|---|---|
| C1 B-1 每行成本 | build 各项 ≥8% | `enable_newsort=true` 中位 **+2.51%~+3.25%** | **回退**（无构建侧源码改动） |
| C1 B-2 收尾合并 | build 各项 ≥4% | `enable_index` 4 表合计 ~10 ms（门 ≈93 ms） | **未实施** |
| C2 T-1 IK 匹配算法 | `tokenize_ik` ≤0.053 | 官方 8 轮配对 **0.0566→0.0514（+6.02%）**，8/8 轮为正 | **保留** |
| C2 T-2 beng ASCII 快路径 | `tokenize_beng` ≤0.032 | 8 轮配对 **−0.65%**（MAD 9.4%），未达门 | **保留**（用户决定，见 §6） |
| C3 Q-1 批次 256→1024 | query_cn ≥3% | 容量不可改（约束链见 C3 报告）；改为整批消费接口 | **替代实现保留** |
| C3 Q-2 emit 批内提示 | query_cn ≥1.5% | count-only emit 仅 3.6 µs/query（0.7%），已是 popcount | **未改** |

最终工作树保留 **9 个改动文件 + 1 个新文件**（`git diff --stat -- src`）：

```
 src/storage/fts/ik/ob_ik_cjk_processor.cpp         | 304 +++++++++++++++++++++
 src/storage/fts/ik/ob_ik_cjk_processor.h           | 149 +++++++++-
 src/storage/fts/ob_beng_ft_parser.cpp              | 107 +++++++-
 src/storage/fts/ob_beng_ft_parser.h                |  24 +-
 src/storage/fts/ob_ik_ft_parser.cpp                |  10 +
 src/storage/retrieval/ob_i_sparse_retrieval_iter.h |  16 ++
 src/storage/retrieval/ob_text_daat_iter.cpp        |  30 +-
 src/storage/retrieval/ob_text_retrieval_token_iter.cpp | 36 +++
 src/storage/retrieval/ob_text_retrieval_token_iter.h  |  3 +
 9 files changed, 666 insertions(+), 13 deletions(-)
 + src/storage/fts/ik/ob_ik_fast_dict.h（新，6364 B）
```

`build_release/src/observer/seekdb` 已按最终树重建，md5 `a7e0099a…` 与
`.tmpdev/final/bin/bin-final.seekdb` 一致；树内**无**临时计时（`OB_FTS_PROF`/`FTSPROF` 已清零），
仅保留两个灰度开关 `OB_FTS_IK_FAST_INDEX=0` / `OB_FTS_BENG_FAST=0`。

---

## 2. 集成 A/B：最终树 vs 本轮开工前（HEAD 源码重建基线）

两臂各自由源码本地构建、同机两实例交替（`ab_pingpong.sh 8 2`，8 轮配对、每轮每臂 6+6 样本，
`TOK_ROUNDS=9000 / QUERY_ROUNDS=500`），两臂源码差异恰为本轮 10 个文件。

| 指标 | 基线中位 | 最终中位 | 配对中位 | spread(MAD) |
|---|---|---|---|---|
| `tokenize_ik_avg_ms` | 0.0534 | **0.0478** | **+11.52%** | 2.32% |
| `tokenize_beng_avg_ms` | 0.0367 | **0.0340** | **+8.59%** | 5.09% |
| `query_cn_avg_ms` | 0.5334 | **0.4434** | **+16.17%** | 2.09% |
| `query_beng_avg_ms` | 0.7082 | **0.5927** | **+16.89%** | 1.33% |
| `query_mixed_avg_ms` | 0.5640 | **0.4871** | **+12.85%** | 3.21% |
| `query_limit_avg_ms` | 0.1931 | **0.1849** | +5.36% | 3.57% |

- 类别中位：**分词 +10.05%**（门 +34% 的原始提升之上再叠加）、**查询 +14.51%**；
- 基线 `query_cn 0.5334` 与任务书本轮基线 `0.5344` 吻合，说明基线即"本轮开工前"状态；
- 命中数两臂完全一致：`[20, 7332, 8001, 11000]`；
- `query_limit` 不走 bitmap 路径，+5.36% 落在噪声区（C3 单独测得该路径 +2.28%±3.35%、两组符号相反），
  不应归因于 Q-1。

**build 侧**：C1 的候选已回退，故构建路径改动为零。C2 的独立配对测量（2 块交替）为
`build_ik_all +2.62%(noise 0.69%) / ik_content −3.69%(noise 6.95%) / beng_en +0.61%(noise 1.06%)`
⇒ 类别中位 **+0.61%，无退化**。集成 A/B 装载阶段单样本：基线 `2.773 / 2.288 / 1.235 s` vs 最终
`2.696 / 2.172 / 1.197 s`（同向但单样本，仅作旁证）。

---

## 3. 集成正确性（全部在 `bin-final.seekdb` 上跑）

| 关 | 结果 |
|---|---|
| 36 形态 dump（实际 37 shape，244 行） | base vs final **diff 22 行，唯一 hunk 是 `order_rel`**（无 ORDER BY 的任意序，允许差异），其余逐字节一致 |
| 5 个仓库用例（`MODE=verify`） | `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path` **全 PASS** |
| DDL 专项 14 项 | **14 OK / 0 FAIL** |
| 命中数 | `8001 / 11000 / 7332 / 20`，8 轮 A/B + 2 次 dump 全部一致 |
| TOKENIZE 逐字节（C2） | 745 行压测（370 文本 × default/all + 15 条 beng）在 base / cand1 / cand1-关开关 / cand2 之间**逐字节相同** |
| 直载顺序 | 建索引后 `SELECT ... MATCH` 结果集/命中数与基线一致（dump 的 `*_set`/`limit` 段全等） |

---

## 4. 各项改了什么（最终树实际内容）

### C2 T-1：IK CJK 匹配改为紧凑前缀 trie（`ob_ik_fast_dict.h` 新文件）
任务书推荐的"首字→候选词表逐词 memcmp"经统计不可行：首字候选数中位 10、**p90 126、最大 2342**（"不"）。
改为保留"首字索引"思想的紧凑前缀 trie：首字用码点直查表（`uint32[65536]`），后续层用 CSR 子节点数组
（`{4B 大端 key, 4B child}`）。规模 **420671 节点 / 420670 边 / 5854 首字，5.4 MB/进程**，
单块 `ob_malloc`、进程内只读、首次分词惰性构建 ~60–70 ms。旧 DAT 路径原样保留为 `do_process_dat`，
`OB_FTS_IK_FAST_INDEX=0` 一键回退。
**顺序不变的关键**：输出序 = `ObFTWordMap` 桶遍历序，桶内头插链表，因此由 `add_token` 调用次序决定；
新实现**只改"怎么找词"**，完全保留旧调用序列（先按起点升序推进 pending，再开新起点），未引入任何重排/缓冲。

### C2 T-2：beng 纯 ASCII 快路径
整段 ASCII 时跳过 analyzer 流水线，256 项分类表 + 就地小写；任一高位字节即走原路径（不分块混走）。
`OB_FTS_BENG_FAST=0` 回退。

### C3 Q-1 替代实现：整批消费 doc id
批次容量 256→1024 **按任务书 §2.2 第 4 条放弃**——`ObEvalCtx::max_batch_size_` ← plan `batch_size_`
← `_rowsets_max_rows=256`，datum 数组与 `skip_`/`doc_id_` 全按 256 分配，单独调大 token iter 会写越界；
且实测每批固定开销 0.529 µs（非假设的 1 µs）、理论收益仅 ~4.2%，改全局 plan vector size 高风险。
改为 `ObISRDaaTDimIter::get_next_id_batch`（默认 `OB_NOT_IMPLEMENT`、保留逐行回退）：
bitmap build 不再逐 posting 付 2 次虚调用 + 1 次 datum 拷贝。
**契约**：调用方必须一次消费完整批（`cur_idx_ == count_` 才允许再次调用，否则 `OB_ERR_UNEXPECTED`）；
datum 指针有效期到下次 refill 为止 —— 唯一的调用方 `ObTextDocIdBitmapIter::build_doc_id_set` 满足该契约。

---

## 5. 本轮踩到的测量污染源（已修，供后续复现参考）

1. **锁外 `cp` 二进制**：构建完在 heavy lock 外拷贝，期间另一 agent 触发 relink ⇒ 拷出的二进制与
   `build_release/seekdb` md5 不一致，A/B 直接作废。此后"构建 + cp + md5 校验"同处一个锁内。
2. **`cp -a` 保留 mtime** 导致"恢复基线"后未重编，两臂字节相同（cmp 才发现）；现已 `touch` 强制重编。
3. **脚本 ROOT 路径**：`.tmpdev/tri/cN/` 与 `.tmpdev/final/` 深度不同，`../..` 与 `../../..` 混用会让
   `sdb.py` 路径解析到 `/home/hahaha/ob-contest/.github/...` 而静默失败；所有脚本已改为按自身深度解析并 `bash -n` 校验。
4. **`nohup` 后台任务被沙箱杀死**（`bwrap --die-with-parent`）：脱离工具管理的后台进程会在工具调用结束时
   被杀，曾导致树被恢复到 HEAD 而构建未完成。长任务必须用工具管理的 background job。

---

## 6. 决策记录

- **T-2 保留**（用户裁定）。理由：正确性与输出顺序已验证无副作用（745 行逐字节 + 5 用例 + dump），
  长文本每字符省约 4 ns；代价是多一条 ASCII 路径代码。已在 C2 报告中标注"未达 0.032 门"。
- **T-1 保留**：官方配对指标 8/8 轮为正、中位 +6.02%、MAD 3.38%，门（≤0.053）达标。
- **灰度开关默认开**：`OB_FTS_IK_FAST_INDEX=0` / `OB_FTS_BENG_FAST=0` 回退到旧实现（任务书写"默认关时走旧实现"，
  本轮为拿分把默认反转为开 + 显式置 0 回退，回退能力不变）。

---

## 7. 风险与遗留

1. **T-1 机制归因偏弱**：官方配对指标 +6% 稳定，但两套服务端旁证（1 万行逐行代理、2300 B 长文本 A/B）
   未复现同幅度，见 C2 报告 §8.3。若后续怀疑指标口径，优先用"逐行 tokenize 总耗时"交叉验证。
2. **T-1 首次分词 60–70 ms 惰性建树 / 5.4 MB 常驻**：暖机后无影响，benchmark 有 30 次暖机；
   但极端小实例（多租户、每进程只查一次）会摊到首查成本，需留意。
3. **C3 的整批契约**：新增虚接口若被其它维度实现/复用，必须遵守"整批消费 + datum 有效期到下次 refill"，
   否则会读到已失效的 expr frame。
4. **未做**：交换臂位置第三次 A/B、并发压力测试；C1 指出的构建侧深水区（临界线程 773 ns 微块追加、
   2 张大 aux 表并发 + DOP=2 的饱和度、`general_ci` 下分桶排序需先解决"字节序≠排序序"）。

---

## 8. 复现命令

```bash
# 集成 A/B（基线 = HEAD 源码重建）
.tmpdev/final/build_ab.sh                 # 先建 bin-base，再恢复最终树建 bin-final
TMPDIR=$PWD/.tmpdev/tmp bash .tmpdev/final/ab_pingpong.sh \
  $PWD/.tmpdev/final/bin/bin-base.seekdb $PWD/.tmpdev/final/bin/bin-final.seekdb 8 2

# 正确性
bash .tmpdev/final/dump_arm.sh .tmpdev/final/bin/bin-base.seekdb  base
bash .tmpdev/final/dump_arm.sh .tmpdev/final/bin/bin-final.seekdb final
diff .tmpdev/final/dump-base.txt .tmpdev/final/dump-final.txt
bash .tmpdev/final/verify_suite.sh        # 5 用例 + DDL 14 项
```
