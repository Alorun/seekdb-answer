# P 轮集成报告（P1 分词四项 + P2 构建/查询三项）

> 起点 `bbe2da610` + A2 工作区改动；两名 agent 并行、同一工作树、同一 `build_release`。
> 分项报告：[P1](prompt-P1-report.md) · [P2](prompt-P2-report.md)。
> 集成二进制 `build_release/src/observer/seekdb`（md5 `2e7dc1b8…`，已按最终树重建，无临时计时代码）。

## 1. 落地清单

| 项 | 内容 | 开关（默认开） | 判定 |
|---|---|---|---|
| **P1-D1** | beng ASCII 停用词预筛（长度区间 + 首/末字节位图，同源词表生成） | `OB_FTS_STOPWORD_PREFILTER=0` | beng +13.2% 达标 |
| **P1-D2** | 词哈希 memo（512 槽/线程，键=整词+collation，命中返回同一 hash ⇒ 桶/顺序不变）+ 比较函数 memo | `OB_FTS_WORD_HASH_CACHE=0` | ik +5.1%（官方口径，噪声大）达标 |
| **P1-D3** | 仲裁器 chain map 惰性化（有序数组够用时不建表/不清表） | `OB_FTS_IK_LAZY_CHAIN_MAP=0` | ik ≈+9%（机制测量）达标 |
| P1-D4 | processor `final` 去虚化 | — | 未达 2% 门；字典转移缓存 0 收益已回退 |
| **P2-D5** | DDL index 直载跳过微块 merge verify（只跳写时自检，落盘格式/校验和不变） | `OB_DDL_SKIP_MICRO_VERIFY=0` | build +3.7/+5.3/+5.9% 达标 |
| P2-D6 | 直载写路径记账（check_order/save_last_key） | — | **未达门且破坏 docid→rowkey 映射（4377 失败）⇒ 整项回退** |
| **P2-D7** | 位图批量消费（tablet 缓存 + ensure_block 外提） | `OB_FTS_QUERY_BATCH_ADD=0` | query +13.7~17% 达标 |
| A2（上一轮） | IK 文档级 arena（不再每 ~56 篇重建词典） | `OB_FTS_IK_DOC_POOL=0` | 保留 |

## 2. 集成 A/B：最终树 vs 本轮开工前（3 块交替，warm=1）

两臂各由源码本地构建，`select1` 两臂 0.0237/0.0226（−4.4%，B 略快），命中数一致 `8001/11000/7332/20`。

| 指标 | 基线 | 最终 | 提升 | 噪声 |
|---|---|---|---|---|
| `build_ik_all_sec` | 2.561 | **2.352** | **+8.16%** | 5.2% |
| `build_ik_content_sec` | 2.163 | **2.026** | **+6.33%** | 1.0% |
| `build_beng_en_sec` | 1.194 | **1.097** | **+8.12%** | 2.3% |
| `tokenize_ik_avg_ms` | 0.0496 | **0.0408** | +17.64% | 26.5% |
| `tokenize_beng_avg_ms` | 0.0385 | **0.0355** | +7.92% | 19.5% |
| `query_cn_avg_ms` | 0.5122 | **0.3863** | +24.57% | 12.9% |
| `query_beng_avg_ms` | 0.5744 | **0.4889** | +14.89% | 14.6% |
| `query_mixed_avg_ms` | 0.5074 | **0.4351** | +14.27% | 15.3% |
| `query_limit_avg_ms` | 0.2344 | **0.2264** | +3.41% | 25.2% |

类别中位：**build +8.12%**、**tokenize +12.78%**、**query +14.58%**。
build 的三项噪声 1~5%，是本轮最可信的一组数字；tokenize/query 因宿主噪声（`select1` 0.0177~0.0425 漂移）
标为"噪声内"，方向与两个 agent 各自的分项 A/B 一致（P1：beng +13.2%/ik +5.1%；P2：query +13.7~17%）。

## 3. 集成正确性（全部在最终二进制上跑）

| 关 | 结果 |
|---|---|
| 36 形态 dump（244 行）vs 开工前 | 唯一 hunk `### order_rel`（234–243 行，任务书允许的并列次序） |
| 745 行 TOKENIZE 压测 vs 开工前 | **diff = 0**（逐字节一致） |
| `tokenize_edge` + 5 个仓库用例 | 全 **PASS** |
| DDL 专项 14 项（含 `SELECT id` 映射） | **0 失败** |
| 命中数 | `8001 / 11000 / 7332 / 20` 全部一致 |
| 各开关关闭时的等价性 | P1：全开关关后 dump/压测与基线逐字节一致；P2：D5/D7 各自 on/off 均过五关（D6 已回退） |

## 4. 风险与遗留

1. **D6 已整项回退**（虽 +3.3%，但 `check_lookup_row_cnt` 报 4377，docid→rowkey 映射被破坏；env 二分定位到 D6）。
2. **P1-D4 未达门**：字典转移缓存实测 0 收益（已删）；剩余候选（`ObFTWordMap` 去读写锁、`TokenizeContext` 访问器内联）
   已量化但未做，属下一轮。
3. **build 的最大块仍未被消化**：P2 侧写显示排序比较仍占 38.9%（24.6 cmp/行 × 37.6 ns），
   属通用 sorter、且 C1 已证直改只值 2.5~4%，本轮未动。
4. **测量口径**：`/tmp` 在 harness 里是每次调用私有的 tmpfs，旧协议里的 `/tmp/fts-build.lock` **从未生效**；
   本轮已改为工作区 `.tmpdev/.build.lock`（`.tmpdev/p_run.sh`）。14:37–14:43 的测量作废重测过。
   另：`ab_pingpong`（两实例并行）在本机不可靠，本报告采用块交替 + `select1` 对照。

## 5. 复现

```bash
# 集成正确性
bash .tmpdev/p1/verify_arm.sh $PWD/build_release/src/observer/seekdb final
bash .tmpdev/p1/suite.sh $PWD/build_release/src/observer/seekdb
# 集成 A/B（base = 本轮开工前 = A2 交付二进制）
TMPDIR=$PWD/.tmpdev/tmp bash .tmpdev/p1/ab_bins.sh \
  $PWD/.tmpdev/p1/bin/base.seekdb $PWD/build_release/src/observer/seekdb 3 1
```
