# 分词侧第二轮报告：③ 深挖 `tokenize_fulltext` + ⑧ 语句层

> 执行对象：`docs/prompt-B-tokenize-side-two-items.md`；分支 `indextuning`，HEAD `5d07654a4`。
> 执行时间：2026-11-03 23:27 ~ 2026-11-04 00:00。本机 24 核 / 15 GB。
> 并发：Agent A 在同一工作树改构建/DDL 侧（报告中的「最终二进制」因此包含 A 的改动，见 §8）。
> **最终交付：分词侧 0 行代码改动。** 官方口径实测基线 **未达标**（§0.1），但剩余上下文不足以
> 安全实现并验证 `make_json+pack` 直写（§9 第 2 项）；唯一的候选改动按 <3% 门回退（§4）。

## 0. 结论（TL;DR）

| 项 | 结论 | 数据 |
|---|---|---|
| **A1 四段相位** | **完成**：`tokenize_fulltext` 内部**没有**「未归属 9 µs」，四项之和 = fulltext | IK：helper 0.2 + map 0.1 + segment 16.5 + make_json 0.7 = **17.5** = fulltext 17.5 µs |
| **A2 选一条实施** | 按数据选 `segment` 的 CJK trie 查找复用；8 轮配对 **−5.23%（se 3.36%）**，无正收益 → **按 3% 门回退** | 见 §4、§7 |
| **B1 四个对照** | **完成**：把语句层 ~7 µs 拆成「JSON 表达式语句固定开销 ~2.2 + 通用 ~300 B 结果单元序列化 ~4.6」，后者与 `repeat('x',300)` 同量级，**不是 tokenize 特有** | 见 §3 |
| **B2 实施** | **未保留**：上一轮 `print_string_array` 快速路径已把 tokenize 特有的序列化成本消掉；本轮未找到 ≥3% 的剩余项 | — |
| **目标（官方口径）** | **未达成**：官方 3000×3 实测 `tokenize_ik` **0.0537 ms**（>0.050）、`tokenize_beng` **0.0385 ms**（>0.034）；9000 轮 pingpong 的 0.0441 不能与该目标比较 | 见 §0.1、§7 |

## 0.1 口径更正（重要，2026-11-04）

先前版本用 9000 轮 pingpong 的绝对值宣称「已达标」，这是**错误**的，特此更正：

- 官方口径 `tools/benchmark/fts_large_bench.sh` 是 ROUNDS=3000 / SAMPLES=3，**每个样本新起一个
  `mysql` 客户端进程**；客户端启动 + 会话建立 + `SET NAMES` 等固定开销约 30 ms，平摊到 3000 轮
  ≈ **+10 µs/op**。9000 轮把固定开销稀释到 ≈3.3 µs/op，所以 9000 轮绝对值系统性偏低，
  **不能**与提示词 0.050 / 0.034 的目标比较。
- 目标只能按官方口径（与 `reports/fts/indextuning.txt` 同参数）衡量。

**官方口径实测（当前最终二进制，分词代码 = HEAD）**，来源 `.tmpdev/report-official-verify-235814.txt`：

| 指标 | 实测 | 目标 | 结论 |
|---|---|---|---|
| `select1_avg_ms` | 0.0208 | — | 机器状态对照 |
| `tokenize_ik_avg_ms` | **0.0537**（stdev 0.0084，median 0.0546） | ≤0.050 | **未达成** |
| `tokenize_beng_avg_ms` | **0.0385**（stdev 0.0037） | ≤0.034 | **未达成** |

因此：**本轮结束时两个 tokenize 目标均未达成**。§7 的 9000 轮数据只用于同一口径内的配对比较，
其绝对值不是达标证据。

## 1. 测量环境与口径

- 共享基线：`.tmpdev/shared/bin-head-baseline.seekdb`（HEAD 源码构建，sha256 `52f4f634…`）。
- 相位计时：`getenv("OB_FTS_PROF")` 门控、1/64 采样，插桩后**已全部删除**；最终二进制无任何计时/日志。
- 四对照与相位：同一新实例、端口 2894、每个对照 9000 条 × 3 组（= 官方 3000×3 的 3 倍窗口）。
- 配对：`.tmpdev/dual/B/ab_pingpong_B.sh`（`ab_pingpong.sh` 的隔离副本：实例在
  `.tmpdev/dual/B/inst-pp-*`，端口 2894/2895，每臂全新实例、逐轮交替、每轮 `SKIP_LOAD=1`）。
- 本窗口 `select1` 在 **16.9–26.9 µs** 之间漂移（见 §7 的 A/A 对照）。

## 2. A1：四段相位实测（IK 80 字 / BENG 字符串，中位，µs）

| 段 | IK | BENG | 说明 |
|---|---|---|---|
| `parse_param` | 0.5 | 0.4 | 已缓存，稳定 |
| `tokenize_fulltext` | **17.5** | **4.8** | |
| ├ `helper_init` | 0.2 | 0.1 | 线程局部 init 缓存生效 |
| ├ `map_create` | 0.1 | 0.1 | 桶数 = len/2（IK 120） |
| ├ **`segment`** | **16.5** | **4.3** | 绝对主项 |
| └ `make_json` | 0.7 | 0.4 | 建 ObJsonArray/ObJsonString |
| `pack_json_res` | 2.2 | 1.1 | 树→二进制 JSON |
| **`eval_tokenize`** | **20.3** | **6.4** | |
| 服务端 wall − select1 | **27.1** | 12.5 | |

- IK 四项之和 `0.2+0.1+16.5+0.7 = 17.5` **精确等于** fulltext，**没有提示词假设的未归属 9 µs**。
  （上一轮在更慢的机器窗口得到 fulltext 17.6、segment 16.0，与本轮一致。）
- `segment` 内部再拆（插桩本身带来约 8 µs 开销，只能用相对量级）：CJK 处理器 8.3 >
  仲裁器 3.2 > add_word 3.6 > 逐字分类 1.4；扣掉计时开销后 CJK ≈ 6–7 µs、add_word ≈ 2.4 µs。
- IK 产生 **41 个 token**，BENG **20 个**（`addword_cnt` 采样）。

## 3. B1：四个对照（同一实例，wall µs/op，9000×3）

| 对照 | 窗口 1 | 窗口 2 | 用途 |
|---|---|---|---|
| `SELECT 1` | 19.7 | 19.4 | 纯往返 + 最小语句开销（不可优化） |
| `SELECT tokenize('', 'ik')` | 21.9 | 21.8 | 分词≈0，剩 JSON 表达式语句层 |
| `SELECT repeat('x', 300)` | 24.2 | 28.3 | 与 tokenize 输出体量相近的**普通 300 B 字符串** |
| `SELECT tokenize('<80 字>', 'ik')` | **46.8** | **48.4** | 完整路径 |

**语句层 ~7 µs 的定位（相减）**：

- `tokenize(IK) − SELECT 1 = 27.1 µs`；其中 `eval_tokenize` 20.3 µs（§2）⇒ **语句层 + 返回路径 6.8 µs**；
- `tokenize('') − SELECT 1 = 2.2 µs` = JSON 表达式本身的固定语句开销（与 token 数无关）；
- 余下 `6.8 − 2.2 = 4.6 µs` 是「约 300 B 结果单元」的序列化/计费，而**普通字符串** `repeat('x',300)`
  相对 `SELECT 1` 也要 4.5–8.9 µs ⇒ 这部分是通用结果单元成本，**不是 tokenize 特有**；
- 结论：提示词的「语句层 ~8 µs」确实存在，但上一轮 `print_string_array` 扁平字符串数组直写
  已把其中 tokenize 特有的逐元素渲染消掉（本轮 `pack_json_res` 仅 2.2 µs，`make_json` 0.7 µs），
  剩余部分是通用 300 B 结果路径，改动它会碰所有 JSON/字符串结果，收益 <3%，不做。

## 4. A2：选了哪条、改了什么、为什么回退

**选择依据**：`segment` 16.5 µs ≫ 其余各项，且提示词 A2 的三个方向中，
「`add_token` 只存 offset/length」「处理器按类型一次分派」在 HEAD 里**已经实现**
（`ObIKToken` 只存指针/偏移，`ObIKFTParser::process_one_char` 已是 typed 直接调用 + 状态跳过），
因此只剩「减少 trie 查找次数」。

**候选改动**（已回退，备份在 `.tmpdev/dual/B/backup/cjk-attempt/`）：

| 文件 | 改动 |
|---|---|
| `src/storage/fts/dict/ob_ft_dict.h` | `ObIFTDict` 增加 `get_word_code()` / `match_with_hit_with_code()`（非纯虚，默认走原路径） |
| `src/storage/fts/dict/ob_ft_dat_dict.{h,cpp}` | 把 `match_with_hit` 拆成 `get_code()` + `match_with_code()`，供复用 code |
| `src/storage/fts/dict/ob_ft_cache_dict.{h,cpp}` | 覆写上述两个接口 |
| `src/storage/fts/ik/ob_ik_cjk_processor.cpp` | 同一字符扩展多个 active hit 时，按 range dict 指针缓存 trie code，只查一次 |

**原理**：一个字符要扩展 N 个 active hit，旧代码对每个 hit 都在 DAT 的 `ObArrayHashMap`
里重新做一次 `find(word, code)`；同一个字符 + 同一个 range dict 的 code 是相同的。

**实测结果**（8 轮配对，见 §7）：`tokenize_ik` **−5.23%（se 3.36%）**，`tokenize_beng` +0.15%
（beng 不经过该路径，是干净的对照）。拆分把「每字符新起一个 match」的公共路径从 1 次调用变成
2 次调用，抵消了复用收益。**按「单步 <3% 回退」的门槛回退，并恢复 HEAD。**

**为什么不再试别的**：官方口径下 ik 0.0537、beng 0.0385（§0.1），目标**未达成**；但剩余项的
上限都很低——`make_json`+`pack` 合计 2.9 µs（直写预计省 1.5–2 µs，ik 仍约 0.0517，beng 全路径
才 1.5 µs，均到不了目标），语句层 6.8 µs 里 4.6 µs 是通用 300 B 结果单元、非 tokenize 特有；
真正能动 `segment` 的方案需要改 `ObFTWordMap` 结构（触碰桶顺序硬约束）或直接写二进制 JSON
（需复刻 `serialize_json_container` 的头部/条目/偏移/inline 逻辑，风险高）。在剩余上下文内
无法对后者完成「实现 + 官方 4 对 A/B + 46 条 dump + `tokenize_edge`」全套验证，按
「不留未验证代码」停止，详见 §9 第 2 项。

## 5. 输出顺序如何保持不变（本轮为何没有风险）

- `TOKENIZE(...)` 的 token 顺序来自 `ObFTWordMap`（`ObHashTable`）的**桶遍历顺序**，不是插入顺序。
  证据：`tokenize('数据库 the 索引 a','ik')` → `["a","索引","数据库","the"]`。
- 本轮候选改动只缓存「字符 → trie code」的查找结果，**完全没有动** `ft_word_bkt_cnt`、
  `ObFTWord` 的 hash/相等、`groupby_word`/`set_or_update` 的插入顺序或桶结构，
  因此即便保留也不会改变顺序；回退后更是 HEAD 原样。
- 证据：46 条 TOKENIZE 用例（含上述顺序用例、空串、大小写、中文数字、ngram、全角/日韩/emoji）
  base vs final **逐字节 0 行差异**（§6）。

## 6. 正确性证据（最终二进制 `bin-B-final.seekdb`，= HEAD 分词代码 + Agent A 构建侧改动）

| 关 | 结果 |
|---|---|
| 46 条 TOKENIZE dump（94 行） | base vs final **diff 0 行** |
| `tokenize_edge`（14 组） | **PASS** |
| `no_score_match` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path` | **全部 PASS** |
| 36 形态 dump vs HEAD 参考（`.tmpdev/dump-final-clean.txt`） | 仅 `order_rel` 段 10 个 id 的并列组选取不同（协议唯一允许差异），其余 0 差异 |
| DDL 专项 14 项 | **14/14 OK**（建索引/分区/双索引/online 加列/删索引/重建/id 映射） |
| 命中数 | `cn=8001 / beng=11000 / mixed=7332 / limit=20`，所有 8 轮运行一致 |
| `tokenize_function` 用例 | FAIL，原因是本机 `mysqltest` 客户端报 `Version format 4 has not yet been implemented`（环境/客户端版本），不是代码；TOKENIZE 语义由 `tokenize_edge` + 46 条 dump 覆盖 |

## 7. 8 轮配对数据（`ab_pingpong_B.sh 8 2`，每臂全新实例，中位 + stderr）

| 实验 | 臂 A（共享基线） | 臂 B | paired 中位 | se | MAD | 说明 |
|---|---|---|---|---|---|---|
| 候选 CJK 复用 | ik 0.0441 | ik 0.0486 | **−5.23%** | 3.36% | 5.06% | beng +0.15%（对照，未走该路径） |
| 最终（HEAD 分词 + A 改动） | ik 0.0444 | ik 0.0477 | **−4.88%** | 2.75% | 6.29% | beng −0.16% |
| **A/A 自检（同一二进制）** | ik 0.0445 | ik 0.0452 | **−0.47%** | 1.58% | 3.87% | beng +3.32%（se 5.20%）⇒ 噪声底 |

**怎么读这三行**：

- 两轮 B 臂（候选与最终）给共享基线的差分几乎一样（−5.2% / −4.9%），而二者之间只差本轮那个
  CJK 改动 ⇒ **CJK 改动本身 ≈ +0.35 µs（约 +0.9%），在噪声内**，回退正确。
- 最终二进制与共享基线的分词源码**逐字节相同**（`git diff` 为空），且 `nm -S` 显示
  `eval_tokenize`/`segment`/`process_word` 的**符号大小完全一致**，仅地址因 A 的改动平移 ~0x800；
  A/A 自检的 ik 差 −0.47%（se 1.58%）给出噪声底。因此 −4.9% **不能归因于分词代码**，
  只能是机器漂移/二进制布局（A 的构建侧改动带来的链接位移）；若要严格隔离，需要用「不含 A 改动」
  的 HEAD 重新构建，本轮受并发协议限制不做。
- **绝对值（口径警告）**：本节是 **9000 轮 pingpong** 口径，A 臂 `tokenize_ik` 中位
  0.0441–0.0459、`tokenize_beng` 0.0287–0.0302；由于每样本少摊了约 7 µs 的客户端固定开销，
  这些数**不能与 0.050 / 0.034 的目标比较**。**官方 3000×3 口径为 ik 0.0537、beng 0.0385，
  两个目标均未达成**（§0.1、`.tmpdev/report-official-verify-235814.txt`）。
  本节数据只用于「同一口径内的配对比较」（CJK 候选 vs 基线、最终 vs 基线的相对差）。

## 8. `build_*` 与 `query_*` 是否退化

- **`build_*`**：分词侧最终 0 改动，构建路径调用同一个 `segment()`，无源码风险。最终二进制单次灌数
  构建 `build_ik_all 2.710 s / build_ik_content 2.245 s / build_beng_en 1.214 s`，
  HEAD 参考 `2.849 / 2.436 / 1.334 s`，无退化（单次值，协议只用于确认量级；`build_*` 对照会混入
  Agent A 的构建侧改动，不能当本轮 A/B 结论）。
- **`query_*`**（最终 8 轮配对，分词无改动）：cn +0.64%、beng +2.63%、mixed +7.36%、
  limit +10.98%（se 1.2–5.6%，A/A 自检同样出现 −2%~+4% 漂移）⇒ 无退化，正负都在噪声内；
  命中数四项完全一致。

## 9. 未完成项与原因；本地数字 vs 平台分数

**未完成（按优先级）**：

1. **`segment` 的深层优化**（CJK ≈6–7 µs / 仲裁器 ≈3.2 µs / add_word ≈2.4 µs）：
   需要改 `ObFTWordMap` 的存储/哈希或把仲裁器链表池化。前者**必然触碰桶顺序硬约束**，
   后者收益 <3% 且状态复杂，按门槛停止。
2. **`make_json` + `pack_json_res`（合计 2.9 µs）**：要在 `tokenize_fulltext` 里直接产出
   `J_ARRAY_V0` 二进制、让 `pack_json_res` 不再重排，需逐字节复刻 `serialize_json_container`
   的头部/条目/var-type/偏移逻辑（~200 行），预估只省 1.5–2 µs（官方 ik ≈0.0517，仍 >0.050；
   beng 全路径才 1.5 µs，更不可能到 0.034）。本轮剩余上下文不足以实现 + 通过官方 4 对 A/B +
   46 条 dump + `tokenize_edge` 全套验证，按「不留未验证代码」停止，**未实施**。
3. **语句层 4.6 µs 的通用 300 B 结果单元序列化**：影响所有 JSON/字符串结果，非 tokenize 特有，
   且 `repeat(300)` 对照证明它与结果体量成正比，不是 tokenize 的额外拷贝。
4. **最终 vs 共享基线的 −4.9%**（源码相同）：需要用不含 Agent A 改动的 HEAD 单独构建才能隔离，
   本轮并发协议下未做；已用 `nm -S` 证明分词热函数代码尺寸一致。

**本地数字 vs 平台分数**：§2/§3/§7 的数字是**本机 wall/µs**，不是平台分数。口径必须分清：
- 9000 轮 pingpong：ik 0.0441–0.0459 / beng 0.0287–0.0302（少摊客户端固定开销）；
- **官方 3000×3：ik 0.0537 / beng 0.0385（目标 0.050/0.034，未达成）**；
- 平台分数会在此基础上再整体放大（历史对照：本地 ~0.0558/0.0479 时平台 0.0607/0.0386，
  约 1.1–1.3× 且分项不同），本报告不据此外推平台得分。

## 10. 复现命令

```bash
# 四段相位 + 四对照（临时插桩版本；正式交付二进制不含插桩）
bash .tmpdev/dual/B/tok_prof.sh <带 OB_FTS_PROF 的二进制> 9000

# 9000 轮 pingpong（只做同口径配对比较，绝对值不可对目标）
TMPDIR=/tmp .tmpdev/dual/with_heavy_lock.sh \
  bash .tmpdev/dual/B/ab_pingpong_B.sh <旧> <新> 8 2

# 官方口径（3000 轮 × 3 样本；绝对值用于目标判定）
#   见 .tmpdev/bench_run.sh / tools/benchmark/fts_large_bench.sh 默认参数

# 正确性
bash .tmpdev/dual/B/tok_dump_B.sh <bin> /tmp/tok.txt 2896
BIN=<bin> bash .tmpdev/dual/B/mysqltest_B.sh
bash .tmpdev/dual/B/dump_with_bin_B.sh <bin> /tmp/dump.txt /tmp/bench.txt 2898
bash .tmpdev/dual/B/ddl_sanity_B.sh <bin>
```

**产物索引**：`pp-1103-233939/`（CJK 候选 8 轮）、`pp-1103-234343/`（最终 8 轮）、
`pp-1103-234608/`（A/A 自检）、`prof-run.log`/`prof-run2.log`（四段/四对照）、
`tokdump-final.diff`、`dump-final.txt`、`mysqltest-234815/`、`correctness-all.log`。
