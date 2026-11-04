# A2 报告：`tokenize_ik` 收尾（0.0473 → 目标 0.0410）

> 仓库 `/home/hahaha/ob-contest/seekdb-index-tuning`，分支 `indextuning`，开工 HEAD `bbe2da610`（工作树 src 干净）。
> 隔离：端口 2894/2897/2896/2898，目录 `.tmpdev/a2/`，编译全部经
> `flock /tmp/fts-build.lock`（`.tmpdev/tri/with_heavy_lock.sh` 同锁）。

## 0. 一句话结论

测量推翻了提示词 §3 的成本假设：`tokenize_ik` 里最大的一块不是"字典跳转"也不是 `ctx.add_token`，
而是**每 ~56 篇文档就发生一次的 IK 解析器整体重建（含三本词典重解，单次 ~727 µs）**，
平摊 **13.0–14.2 µs/文档 ≈ 总时间 43%**。把"每文档对象"放进可回收的文档级 arena 后，
该重建彻底消失（6144 篇里 fresh init 从 **110 次降到 2 次**），tokenize 稳态服务端时间从 ~30 µs 降到 ~17 µs。

**落地改动：2 个文件、47 行新增 / 13 行修改**，`tokenize_ik` 本机实测 **+3.9% ~ +10.2%（不同批次）**，
最干净的单实例交替法 15 样本/臂为 **0.0520 → 0.0468 ms（+10.0%）**。

---

## 1. 三块计时（实测，`OB_FTS_PROF` 门控、1/64 采样，验证后已全部删除）

官方 IK 文本（91 字符 / 41 token 每篇），跑 200 预热 + 2×3000 语句，取同一 worker 线程的稳态累计值，
单位 µs/文档：

| 块 | 实测 | 占稳态总时间 |
|---|---|---|
| **解析器重建（fresh init，含三本词典重解）** | **13.0–14.2（平摊）** | **43%**（一次性支出 727 µs × 每 ~56 篇一次） |
| `ObAddWord::process_word`（词表插入链） | 5.5–5.7 | 18% |
| ├ `groupby_word`（`ObFTWordMap` 插入） | 3.9（95 ns/token） | |
| ├ `casedown_word` | 1.25（30 ns/token） | |
| └ `check_stopword` | 0.57（14 ns/token） | |
| 每字符循环（charloop） | 4.5–4.7 | 15% |
| ├ 其中字典跳转 `dict.child/root_child/is_word/has_child` | 2.2–2.3（24 ns/字符） | 7% |
| └ 其余（分类、letter/quantifier/surrogate、ctx 记账） | ~2.3 | |
| 仲裁器 `arbitrator_.process` | 3.4–3.5 | 11% |
| `output_result` | 0.63–0.67 | 2% |
| `make_token_array_json` | 0.78–0.81 | 3% |
| `token_map.create` + `helper.init` | 0.16 + 0.07 | <1% |
| `free_token_iter` | 0.02 | — |
| `ctx.add_token`（**提示词假设的大头**） | **0.03** | **0.1%** |
| `parse_param`+`pack_json`+`TempAllocGuard`（在 `tok_full` 之外） | 2.9 | 10% |

**它们如何解释 22.3 µs 服务端**：稳态 `eval_tokenize` = 16.6 µs（30 µs 减去 13 µs 平摊重建），
加上语句层/协议序列化即为提示词口径的 22.3 µs。提示词的三块预期（字典 4~6 µs、词表插入 5~7 µs、其余 1~3 µs）
只覆盖了 16.6 µs 里的一半，**没有覆盖"解析器每文档重建"这 13 µs**，所以按原假设去做字典/插入优化最多只能拿到个位数点。

顺手证伪的三条：
- `ctx.add_token`（`ObFTSortList` 插入）**17 ns/次、每文档 0.03 µs**，完全不是瓶颈；
- 字典跳转 24 ns/字符（80+ 字符文本 ≈ 2.2 µs），比预期低一个量级，`IKFastDict` 已经很紧；
- 停用词/lowercase 都已是 fast path（14/30 ns/token）。

---

## 2. 改了什么、为什么

`src/storage/fts/ob_ik_ft_parser.{h,cpp}`：

1. `ObIKFTParser` 新增一个**文档级 arena** `doc_alloc_`（`ObArenaAllocator`，`ObMemAttr("IKDocPool")`）
   和入口 `doc_alloc()`；
2. `TokenizeContext` 与 4 个处理器（letter/quantifier/cjk/surrogate）改从 `doc_alloc()` 分配，
   不再从"长生命周期 arena"（`allocator_`，装着词典与 parser 本体）分配；
3. `reset_document_state()` 在把这些对象析构掉之后调用 `doc_alloc_.reuse()`，把整块内存**归还给下一个文档**。

**为什么原来是 13 µs**：`ObIKParserCache`（线程内缓存 parser）用 `alloc_.used() < 512 KB` 判断能否复用；
而每个文档的 ctx + 4 个处理器（内含 `IKHitArray(32)`/`IKFastHitArray(32)` 等内联大数组）大约吃掉 9 KB，
**每 ~56 篇文档就把 512 KB 撑满**，于是 `cache.destroy()` → 下一个文档 `parser->init()` 重新
`init_dict()`（`hub_->load_cache` ×3 + `build_dict_from_cache` ×3，主词典按首字分 range，创建上万个
`ObFTCacheDict`），实测 **727 µs/次**。旧代码的 `allocator_.free()` 在 arena 上是 no-op，
所以这些每文档对象只会累积、不会被回收。

**生命周期为什么安全**：ctx 与处理器的所有内存都只在单篇文档内使用（token 文本指向输入 buffer，
不指向 arena）；文档结束前它们先被显式析构，随后 `reuse()` 把整块内存交给下一篇文档；
长生命周期对象（三本词典、`ObFTCacheDict`、parser 本体、`segmenters_` 链表、仲裁器自带的 arena）仍在
`allocator_` / 仲裁器 arena 里，不受影响。`reset()`（parser 销毁）时 `doc_alloc_` 作为成员被析构。

**灰度开关**：`OB_FTS_IK_DOC_POOL=0` 让 `doc_alloc()` 退回 `allocator_` 并跳过 `reuse()`，
即逐字节回到改动前的行为（§5 有开关开/关一致性证据）。另一既有开关 `OB_FTS_IK_FAST_INDEX` 不变。

---

## 3. "输出顺序如何保持不变"

本改动**完全没有触碰分词算法与输出通路**：不改 `add_token` 调用序列、不改 `ObFTWordMap` 的桶数/哈希/插入顺序、
不改仲裁器、不改 JSON 构造。改的只是"每文档对象从哪个 allocator 拿内存"。因此
`TOKENIZE(...)` 的输出逐字节不变，由 §5 的 754 行压测（diff=0）与 36 形态 dump 直接证明。

---

## 4. 性能数据

### 4.1 官方口径（`fts_large_bench.sh`，ROUNDS=3000 / SAMPLES=3，块模式两臂交替）

| 批次 | 基线 A 中位 | 候选 B 中位 | 提升 | 噪声 |
|---|---|---|---|---|
| 2 块（A/B/B/A，warm=2） | 0.0560 | **0.0503** | **+10.18%** | 18.6% |
| 3 块（warm=1） | 0.0508 | **0.0488** | **+3.94%** | 12.0% |

（`tokenize_beng` 同批次为 +12.28% / −1.45%，两臂 beng 源码完全相同 ⇒ 属噪声，不作为收益。）

### 4.2 单实例交替采样（15 样本/臂，最干净的口径）

```
base: 0.0538 0.0527 0.0490 0.0522 0.0501 0.0556 0.0523 0.0507 0.0461 0.0629 0.0490 0.0491 0.0520 0.0478 0.0522
cand: 0.0453 0.0496 0.0452 0.0623 0.0454 0.0424 0.0456 0.0521 0.0637 0.0469 0.0468 0.0459 0.0553 0.0430 0.0492
```
中位：**base 0.0520 → cand 0.0468 ms（+10.0%）**；两臂离群值（0.0629 / 0.0637）各一个，方向一致。

### 4.3 8 轮配对（`ab_pingpong.sh base cand 8 2`，两实例并行）——**口径本身不可用，如实记录**

| 批次 | tokenize_ik（原始配对） | tokenize_beng | `select1` 臂间偏差 | 归一化 (ik/select1) |
|---|---|---|---|---|
| run1 | **−4.93%**（MAD 6.89%） | −0.73% | 双向、最大 ±10% | 8 轮中 6 轮 B 更差 |
| run2 | **−6.20%**（MAD 9.22%） | −3.86% | **6/8 轮 B 慢 20~30%** | 8 轮中 5 轮 B 更好 |

**两次 pingpong 都给出负值，且彼此不一致**：run1 原始 −4.93%、归一化后多数轮 B 更差；
run2 原始 −6.20%，但 B 臂 `select1` 在 6/8 轮慢 20~30%（例：r6 A 0.0191 / B 0.0248），
归一化后多数轮 B 更好。两次的原始值都被少数离群轮主导（run1 的 r4/r8 = 0.0586/0.0563，
run2 的 r2/r6 = 0.0587/0.0513），MAD 达 6.9%~9.2%。

**结论：在当前宿主机（load 3.4~4.1、两实例并发、`select1` 在 0.019~0.030 跳动）下，
`ab_pingpong` 无法分辨本项改动，两次结果的符号都不可采信。** 判定改用单实例口径
（§4.1 官方块模式、§4.2 单实例交替采样）——这正是评分时的形态：只跑一个实例。
如果按 §6.2 严格照字面执行（只看 pingpong 配对中位），本项应判为"未过门"；
按官方口径与单实例交替法，本项为 **+4%~+10%**。报告两种结论，供决策。

### 4.4 侧写前后对比（同一台机、同一文本）

| 指标 | 改前 | 改后 |
|---|---|---|
| 每 6144 篇的 fresh init 次数 | 110 | **2**（仅进程启动） |
| 平摊重建成本 | 13.0–14.2 µs/篇 | ~0 |
| `parser_desc->segment` | 13.2–14.3 µs | 0.5–0.7 µs |
| 稳态 `eval_tokenize` | ~30 µs | **~17 µs** |

---

## 5. 正确性（全部在 `bin/cand.seekdb` = `.tmpdev/a2/bin/cand.seekdb`，md5 `6f57baa9…`）

| 关 | 结果 |
|---|---|
| 36 形态 dump（实际 37 shape，244 行） | base vs cand **diff 22 行，唯一 hunk 是 `order_rel`**（无 ORDER BY 的任意序，允许差异） |
| **754 行 TOKENIZE 压测**（`.tmpdev/tri/c2/stress_tok.sql`，370 文本 × default/all + 15 beng） | base vs cand **diff 0 行**（逐字节相同） |
| `tokenize_edge`（14 组） | **VERIFY PASS** |
| 5 个仓库用例 | `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path` **全 PASS** |
| DDL 专项 14 项 | **0 失败** |
| 命中数 | `8001 / 11000 / 7332 / 20` 在 dump 与全部 A/B 轮次一致 |
| **灰度开关开/关一致性** | `OB_FTS_IK_DOC_POOL=0` 下 dump 与 base 只差 `order_rel` 的 22 行、754 行压测 **diff 0** ⇒ 开关关闭时逐字节回到旧行为 |

---

## 6. `build_*` 是否退化

块模式 A/B（3 块，warm=1，两臂交替）：

| 指标 | 基线 | 候选 | 提升 | 噪声 |
|---|---|---|---|---|
| `build_ik_all_sec` | 2.689 | 2.683 | +0.22% | 1.19% |
| `build_ik_content_sec` | 2.230 | 2.203 | **+1.21%** | 0.70% |
| `build_beng_en_sec` | 1.165 | 1.169 | −0.34% | 1.12% |
| 类别中位 | | | **+0.22%（无退化）** | |

构建走同一个 `segment()`，因此同样受益于"不再每 ~56 篇重建解析器"；实测在噪声内、`ik_content` 略快。
（另一批次里 `build_ik_all` 曾出现 9.469 s 的已知离群块，按纪律作废。）

---

## 7. 未完成部分与口径

1. **0.0410 的门槛未在本机直接验证到**：本机当前处于高噪声状态（宿主 load 3.4~4.1，`select1` 0.019~0.030 波动，
   单块内 3 个样本可在 0.043~0.067 之间跳）。本机 cand 绝对值为 0.0468~0.0503 ms，
   而提示词的平台基线 0.0473 对应平台候选预计 **≈0.042~0.043 ms**（相对提升率不随机器缩放）。
   若要把 0.0410 也吃下来，还需再砍 ~4 µs 服务端，即剩余分词工作的 ~25%。
2. **下一步最有价值的方向（按本次实测排序）**：
   - `process_word` 链 5.5 µs（其中 `ObFTWordMap` 插入 3.9 µs / 41 token）。桶数、哈希与插入顺序被"输出逐字节不变"锁死，
     只能改实现不能改语义（例如同哈希同桶数的专用小表），风险中等、收益上限 ~3 µs；
   - 仲裁器 3.4 µs（链的构造与 `optimize`），需保持输出序列一致；
   - 每字符循环其余 2.3 µs。
3. **副作用 / 注意**：每个 parser 多一个 `ObArenaAllocator`（稳态保留几 KB~几十 KB），
   比原来"512 KB 上限 + 周期重建"更省；`OB_FTS_IK_DOC_POOL=0` 可一键回退。
4. **测量方法教训**（本轮踩到）：`ab_pingpong`（两实例并行）在当前宿主机负载下会出现 **20~30% 的实例级臂偏差**，
   本轮两次 pingpong 的负值全部来自该偏差；单实例交替 + `select1` 归一化 + 块模式三者一致时才能下结论。
5. 本报告所有数字均为**本机**结果；仅相对提升率可外推到平台分数，绝对值不可直接换算。

---

## 8. 复现

```bash
# 候选/基线二进制（两臂只差这 2 个文件）
.tmpdev/a2/build_base.sh          # 先按 HEAD 构建 base，再恢复候选并重建，md5 校验
# 单实例交替采样（最干净的 tokenize 口径）
bash .tmpdev/a2/ik_ab.sh $PWD/.tmpdev/a2/bin/base.seekdb base 3
bash .tmpdev/a2/ik_ab.sh $PWD/.tmpdev/a2/bin/cand.seekdb cand 3
# 官方口径块模式
TMPDIR=$PWD/.tmpdev/tmp bash .tmpdev/a2/ab_bins.sh \
  $PWD/.tmpdev/a2/bin/base.seekdb $PWD/.tmpdev/a2/bin/cand.seekdb 3 1
# 正确性（dump + 754 行压测）
bash .tmpdev/a2/verify_arm.sh $PWD/.tmpdev/a2/bin/base.seekdb base OB_FTS_IK_DOC_POOL=0
bash .tmpdev/a2/verify_arm.sh $PWD/.tmpdev/a2/bin/cand.seekdb cand
diff .tmpdev/a2/logs/dump-base.txt .tmpdev/a2/logs/dump-cand.txt      # 只应有 order_rel
diff .tmpdev/a2/logs/stress-base.txt .tmpdev/a2/logs/stress-cand.txt  # 应为空
bash .tmpdev/a2/suite.sh          # 5 用例 + DDL 14 项
```
