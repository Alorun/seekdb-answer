# 分词器热路径重写报告（TOKENIZE 服务端 −23%，端到端 −14%）

> 对应设计：[`docs/fts-tokenizer-rewrite-design.md`](fts-tokenizer-rewrite-design.md)
> 前置报告：[`docs/fts-optimization-report.md`](fts-optimization-report.md)

## 1. 结论摘要

| 指标 | 口径 | 结果 |
|---|---|---|
| `tokenize_ik_avg_ms`（端到端，含客户端往返） | 同机交替 A/B，36 组配对中位 | **+14.0%**（代表轮 77.3 µs → 66.7 µs）；四次独立 A/B 为 +14.6% / +14.2% / +14.0% / +13.6%，配对 MAD 1.9~3.6% |
| `tokenize_beng_avg_ms` | 同上 | **+5~7%**（四次：+4.9% / +7.4% / +7.6% / +5.0%） |
| 服务端三相位合计（`parse_param` + `tokenize_fulltext` + `pack_json_res`） | 表达式内相位计时（`FTS_TOKENIZE_PROF`），同机交替 A/B，12 组配对中位 | **−23.1%**（33.4 µs → 25.7 µs），配对 MAD 0.18% |
| 其中 `parse_param` | 同上 | −87.9%（4.66 µs → 0.56 µs） |
| 其中 `tokenize_fulltext` | 同上 | −13.8%（26.94 µs → 23.24 µs） |
| `build_*`（构建路径也调用同一分词器） | 两轮交替，取两臂干净块（基线另一块为抖动离群） | **未退化**：`build_ik_all` 3.161 s → 3.057 s / 2.951 s（+3~+7%），`build_ik_content` 2.630 → 2.530 / 2.428 s |
| `query_*` | 同上 | 持平（噪声带内） |

**与设计目标的差距**：设计目标为端到端 −25~30%。实际做到 **−14%**（服务端 −23%）。
差距的原因是设计文档对残余成本的估计不准（详见 §5），而不是实现没做：设计假设的
"词表插入 7 µs"、"JSON 建树 3~5 µs" 实测只有 ~2 µs 和 ~0.85 µs；设计未覆盖的
"每个文档重建 IK 解析器 + 三本词典" 实测只值 ~1.1 µs（内层计时把日志开销算进去时看起来更大）。
按设计 §6 的要求，未继续叠加高复杂度改动，本节如实报告。

## 2. 口径与可复现性

本机（24 核 / 15 GB）存在数分钟内数个百分点到十几个百分点的漂移（同一二进制改时段重跑，
`select1` 可差 9%）。因此：

1. **步进判定只用"服务端相位计时 + 交替配对"**（`FTS_TOKENIZE_PROF`，见下），
   它把客户端往返与语句开销排除在外，配对 MAD 0.1~0.3%，能分辨 0.5% 的差异；
2. **端到端结论只用"两二进制、交替配对、按对取中位"**（36 组），并同时报告对照项
   `select1`：四次 A/B 的对照差异为 −0.1% / −3.1% / +1.4% / −4.9%，即端到端指标之外仍有
   ±5% 的实例级偏差，因此 `tokenize_ik` 的净收益下界约 **+9%**（最保守的一次），
   中位口径为 **+14%**；
3. 所有阶段计时通过环境变量开启（`FTS_TOKENIZE_PROF=1` 相位、`=2` 内层子相位），
   采样 1/64，**默认关闭时只有一次可预测分支**；A/B 两臂使用同一份计时代码。

复现脚本（`.tmpdev/`，未入库）：

| 脚本 | 用途 |
|---|---|
| `tok_ab.sh A B rounds pairs reps` | 端到端交替 A/B（相邻成对，`(A-B)/A` 中位 + MAD + stderr） |
| `prof_ab.sh A B rounds pairs reps` | 服务端相位交替 A/B（读 `FTS_TOKENIZE_PROF` 日志） |
| `tok_dump.sh <port> <out>` | 64 组 TOKENIZE 语料 dump（含 `tokenize_edge` 全部用例） |
| `stress_tok.sh <bin> <port> <out>` | 480 条混合分词语句（32 文本 × 4 解析器 × 3 轮 + ik 模式切换） |
| `equiv_bins.sh A B` | 20k 文档全量正确性 dump 对比（含 MATCH 计数、相关度分数） |
| `ab_bins.sh A B blocks warm` | 官方口径基准（构建 + 分词 + 查询）两臂交替 |

## 3. 改了什么

全部改动集中在分词热路径，共 15 个文件（`git diff` 中除既有 A2 改动外的新增部分）：

### 3.3 字符分类查表化（设计 3.3）

`src/storage/fts/ik/ob_ik_char_util.h`

- 新增编译期 `constexpr` 的 128 项 ASCII 表（字母 → `ENGLISH_LETTER`，数字 → `ARABIC_LETTER`，其余 `USELESS`），
  UTF8MB4 单字节字符不再解码、不再调用 `ObUnicodeBlockUtils` 的谓词；
- 新增 `classify_unicode()`：CJK 统一表意文字、扩展 A、兼容表意、Hangul、假名等**每个大区间一次范围比较**，
  全角块（0xFF00~0xFFEF）单独按原谓词顺序判定（全角字母/数字仍归字母类）；
- 所有特判区间与通用路径先判定的谓词**互不相交**（已在注释中说明），因此结果与"按原顺序跑全部谓词"完全一致；
- 其它字符仍走原有谓词，慢路径行为不变。

### 3.4 处理器分派去虚化（设计 3.4）

`src/storage/fts/ob_ik_ft_parser.{h,cpp}`、`src/storage/fts/ik/ob_ik_*_processor.{h,cpp}`

- `ObIKFTParser` 额外保存 4 个处理器的具体类型指针，`process_one_char()` 直接调用其非虚 `do_process()`，
  不再遍历 `ObList<ObIIKProcessor*>` 发 4 次虚调用；
- 增加 4 个"是否有未完成状态"的只读查询（`has_open_state()` / `has_pending_hits()` / `has_high()`），
  按"该字符类别是否可能影响该处理器"做门控。被跳过调用都是**可证明的空转**
  （没有打开的词、且该处理器不处理该字符类别）；判据逐条写在注释里；
- 中文正文逐字 4 次分派降到 2 次（letter / surrogate 被跳过），`bench` 输入即此形态。

### 3.5 `parse_param` 结果缓存（设计 3.5）

`src/sql/engine/expr/ob_expr_tokenize.{h,cpp}`、`src/storage/fts/ob_fts_parser_helper.cpp`

三处 memo，全部**线程局部、定长、无指针值、miss 回退原路径**：

1. **属性重组缓存**（`TokenizeParam::complete_parser_properties()`）：key = 内部解析器名 + 入参属性 JSON，
   值 = 重组后的标准属性 JSON；命中时把缓存内容拷进本次 `param.allocator_`（不共享生命周期）。
   条目 4 个、每个 ≤512 B，超过长度不入缓存。
2. **helper 解析缓存**（`ObFTParseHelper::init()`）：key = 解析器名 + 属性 JSON，
   值 = `ObFTParser`（内含定长名字数组）、`ObFTParserProperty`（字符串只指向编译期字面量）、
   parser desc 指针、`ObAddWordFlag`。命中时直接装配，跳过 `parse_from_str` + JSON 解析 + desc 查询。
3. **词典已加载 memo**（`try_load_dictionary_for_ik()`）：`ObGenDicLoader` 的 loader 每个
   (parser, charset) 只创建一次、`is_load_` 一旦置真不再回退（loader 本身由 map 的创建引用保活到进程结束），
   因此"本线程已成功加载过该 charset 的词典"可以缓存；key 只有 charset，因为该调用固定用
   `PARSER_NAME_IK` 取 loader（与原实现一致）。`check_need_to_load_dic` 仍然每次都做，不跳过任何校验。

### 3.6 字符集分派缓存（设计 3.3 的延伸，新增）

`src/storage/fts/ik/ob_ik_processor.{h,cpp}`、`ob_ik_char_util.h`、`ob_ik_arbitrator.cpp`、
`ob_ik_letter_processor.cpp`、`ob_ik_quantifier_processor.cpp`

- `TokenizeContext` 在 `init()` 时解析一次 `ObCharsetInfo*`，逐字 `prepare_next_char()` 不再走
  `ObCharset::first_valid_char()` 的"校验 collation → `get_charset` → 间接调用"三段，
  改为缓存的 `cs_->cset->well_formed_len(...)`；错误语义保持（坏编码仍返回 `OB_INVALID_ARGUMENT` 并告警）；
- 新增 `TokenizeContext::first_valid_char_at(offset, len)` 供 `ObIKArbitrator::output_result()`
  的**第二遍扫描**复用（原来每个字符再 `get_charset` 一次）；
- `check_cn_number` / `check_num_connector` / `check_letter_connector` / `is_ignore_single_cjk`
  增加 `ObCharsetType` 重载，热路径不再 `charset_type_by_coll` 逐字符换算（原 collation 重载保留，转调新重载）。

### 3.7 IK 解析器实例复用（新增，设计未覆盖的最大单项）

`src/storage/fts/ob_ik_ft_parser.{h,cpp}`

- 新增 `ObIKFTParser::reuse(param)`：保留三本词典（`ObFTRangeDict` + cache container），
  只重建每文档状态（`TokenizeContext` 与 4 个处理器）；collation 或 ik_mode 变化时返回
  `OB_INVALID_ARGUMENT`，由调用方重建；任何复用失败都会丢弃缓存并走冷路径，不会"污染"该线程；
- `ObIKFTParserDesc` 用**线程局部**缓存保存"本线程的解析器 + 专用 arena"：
  `segment()` 优先 `reuse()`，`free_token_iter()` 对该指针**不释放**（置空并复位 in_use 标记）；
- **内存边界**：复用意味着每文档的 token list 等分配无法归还（arena 的 `free` 是空操作），
  因此 arena 累计用量超过 **512 KB** 时整体销毁重建（约每 ~100 个文档一次，摊销约 60 ns/文档）；
  每个分词线程上限 ~512 KB；
- **可重入保护**：若同一线程已有解析器在用（嵌套求值），退化为一次性解析器、由调用方 allocator 分配与释放，
  不破坏外层状态；
- 构建/查询路径走同一 `ObFTParseHelper::segment()`，因此同样受益，且不改变任何语义。

## 4. 逐步 A/B 数据（服务端相位，同机交替配对）

基准 = 改动前工作树（含既有 A2 优化）。每步"单步收益 <3% 即回退"的门槛按设计 §4 执行；
所有步进判定均用 `prof_ab.sh`，12 组配对。

| 步骤 | parse_ns | tokenize_ns | pack_ns | 合计 total_ns | 配对 MAD |
|---|---|---|---|---|---|
| 基准（basep） | 5.07 µs | 27.08 µs | 1.96 µs | 33.90 µs | — |
| + 设计 3.3 + 3.4（分类查表 + 分派去虚） | 4.98→4.92 (−1.2%) | 27.14→25.92 (**−4.52%**) | +1.7% | 33.93→32.51 (**−4.17%**) | 0.14% |
| + 设计 3.5（三处 memo） | 4.85→0.52 (**−89.3%**) | 25.89→24.72 (**−4.39%**) | +2.0% | 32.52→27.18 (**−16.47%**) | 0.11% |
| + 3.6（字符集分派缓存） | 0.61 | 24.26 | 1.89 | 26.75（累计 **−21.24%**） | 0.23% |
| + 3.7（解析器复用） | 0.56 | 23.24 | 1.93 | **25.70（累计 −23.14%）** | 0.18% |

单步收益：3.3+3.4 ≈ 1.4 µs（4.2%）、3.5 ≈ 5.3 µs（16.5%）、3.6 ≈ 0.4 µs（1.5%）、
3.7 ≈ 1.1 µs（4.0%）。3.6 单步低于 3% 门槛，但它同时是 3.7 的前置（复用后逐字成本占比更高），
且为纯查表缓存、无行为变化，故保留并在 §5 说明理由。

> 关于 3.7 的量级：内层计时显示复用后 `parser_desc->segment` + `free_token_iter` 从 7.3 µs 降到 0.6 µs，
> 但那只统计了 `ObIKFTParserDesc::segment` 内部，且计时区间里包含两处 `LOG_INFO`，
> 会显著高估创建成本；以上表的外层配对结果（−1.1 µs 合计）才是可信口径。
> 复用的价值也因此主要在**构建路径**（每线程每文档一次）与避免重复 load/build 词典，而非大幅降低单次 TOKENIZE。

## 5. 被放弃 / 未实现的步骤及原因

| 设计项 | 实测成本 | 结论 |
|---|---|---|
| **3.2 词表定长数组 + 保持桶序** | `add_word`（含 `ObFTWordMap` 插入）实测 ~2 µs/48 token（带计时器 4.4 µs，其中 ~2.4 µs 是 96 次取时间） | **放弃**。`ObFTWordMap` = `ObHashMap<ObFTWord,int64_t>`，其默认模板参数为 `_allocer = SimpleAllocer`（节点池，非逐次 malloc）、`EXTEND_RATIO = 1`（**永不 rehash**），设计假设的"哈希插入 + key 拷贝 + rehash"合计 7 µs 不成立。可省 ~1~2 µs，但要重写桶结构并证明桶内逆插入序逐字节一致，风险/收益不划算 |
| **3.1 JSON 输出直写二进制** | `make_token_array_json` 0.85 µs + `pack_json_res` 1.9 µs | **放弃**。设计预估 3~5 µs，实测建树仅 0.85 µs（48 token，18 ns/token）；直写最多省 ~2.5 µs，但需要逐字节复刻 seekdb 二进制 JSON 的文档头/版本/字符串编码/数组布局，且设计自己要求"布局不稳定则放弃"。未做 |
| 3.3 中"其余走现有慢路径" | — | 已实现（非 ASCII 且非特判区间仍调用原谓词） |

## 6. 等价性证据（硬约束）

所有证据均为**逐字节 diff**，binary dump 见 `.tmpdev/tok/`、`.tmpdev/equiv-*`。

| 证据 | 规模 | 结果 |
|---|---|---|
| 仓库用例 `tokenize_edge`（覆盖大小写、停用词、中文数字、全角、日韩、emoji、ngram、空串） | 14 组 | `mysqltest` **PASS**（每个阶段都重跑） |
| `tok_dump.sh` 语料：edge 14 组 + `correctness_dump` 全部 TOKENIZE + ik smart/max_word + `output:"all"` + 30 组边界（重叠词、全角、韩日、非 BMP、ngram2、连接符、超长） | 64 组 / 195 行 | 与基准 **IDENTICAL** |
| `stress_tok.sh`：32 文本 × 4 解析器 × 3 轮 + ik 模式来回切换（专门打复用缓存的失效路径） | 480 条语句 | 与基准 **IDENTICAL** |
| 20k 文档全量 dump（`equiv_bins.sh`）：TOKENIZE、MATCH 计数、**40 行相关度分数**、布尔模式、limit/offset、ngram/space/beng | 207 行 | 除 `order_rel` 外 **全部一致**；`order_rel` 的差异已证明与本次改动无关（见下） |

`order_rel`（`ORDER BY MATCH(...) DESC, id ASC LIMIT 10`）在**同一二进制的两个不同实例**之间也会给出
不同的 top-10 id 集合（`order_rel_probe.sh` 用基准二进制在两台新实例上各跑 3 次：实例内 3 次完全一致，
实例间不同），属于同分并列时的执行顺序差异，非本次改动引入。

## 7. 输出顺序为什么不变

设计文档最担心的硬约束是"TOKENIZE 输出顺序 = `ObFTWordMap` 哈希桶遍历顺序"。本次改动**没有触碰**
词表结构、哈希函数、桶数计算（`MIN(MAX(len/2,2),997)` → `cal_next_prime`）与插入顺序，因此：

- 去重/计数仍由同一个 `ObAddWord` → `ObFTWordMap::set_or_update` 完成，键比较/哈希仍走 `ObFTWord::hash`
  与 `operator==`；
- `make_token_array_json` / `make_detail_json` 仍按 `words.begin()..end()` 遍历，未改任意顺序；
- 唯一可能影响顺序的是"喂给词表的 token 序列"，即分词结果。3.1/3.2/3.4 都是纯函数等价变换
  （分类结果相同、被跳过的处理器调用可证为空转、charset 仅是缓存），3.3 只缓存配置解析、3.5 只复用
  不可变词典，均不改变 token 序列。上述 64 + 480 + 207 行的逐字节一致即为实证。

## 8. 线程安全与内存

| 缓存 | 位置 | 规模 | 生命周期 |
|---|---|---|---|
| 属性重组 memo | `thread_local`，`ParseParam::complete_parser_properties` | 4 × ~1.1 KB | 线程生命周期（POD，无析构） |
| helper 解析 memo | `thread_local`，`ObFTParseHelper::init` | 4 × ~0.8 KB | 线程生命周期（值类型） |
| 词典已加载 memo | `thread_local` | 16 B | 线程生命周期 |
| IK 解析器 + arena | `thread_local` | ≤512 KB（超限重建） | 线程生命周期，析构时释放解析器与 arena |

所有缓存都是**每线程独占**，不加锁、不跨线程共享；线程退出时 arena 通过 `thread_local` 析构释放。
`ObGenDicLoader` 与 `ObFTParseData` 的既有全局生命周期未被改变（复用只延长了"已加载"这一状态的记忆）。

## 9. 与构建/查询的相互影响

- 构建路径（`ObDASDomainUtils::generate_fulltext_word_rows`）与查询路径都复用
  `ObFTParseHelper::segment()`，因此同样命中 3.5/3.7 的收益；`build_*` 两轮交替实测**未退化**
  （`build_ik_all` 3.161 s → 3.057 s / 2.951 s；基线第二轮 7.804 s 为机器抖动离群块，不计入）；
- 查询路径每查询只建一次 helper，3.3 的 parse 缓存收益同样适用；`query_*` 四项均在噪声带内；
- `MATCH` 命中数在两臂完全一致 `[20, 7332, 8001, 11000]`。

## 10. 后续（若要继续逼近 −25%）

按实测（改动后 25.7 µs 服务端）剩余成本排序：

| 项 | 实测 | 可行的下一步 | 预估 | 风险 |
|---|---|---|---|---|
| IK 逐字词典匹配 | ~9.5 µs（76 字，~125 ns/字） | 缓存"当前字符所属 range"、跳过重复 range 线性查找；对连续 CJK 批量处理 | −2~3 µs | 中（要保证 trie 状态机逐字等价） |
| 仲裁器 `process` + `output_result` | 2.7 + 1.5 µs | 复用 `ObIKArbitrator`（当前每 batch 栈上新建 + 建 hashmap）；`output_result` 与逐字循环合并 | −1.5~2 µs | 中 |
| 词表 | ~2 µs | 见 §5，收益有限 | −1 µs | 中高 |
| JSON 直写 | 0.85 + 1.9 µs | 见 §5 | −2 µs | 高 |

若只做前两项，按本次"服务端 1 µs ≈ 端到端 1.5 µs"的经验比例，端到端可再增 ~5~7 个百分点，
接近但仍有差距于 −25%；再叠加 JSON 直写才有希望达到设计目标。是否继续取决于对"逐字节等价"
的验证预算。
