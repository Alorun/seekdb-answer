# 设计：TOKENIZE 分词器整体重写（服务端 −30%）

> 目标：`tokenize_ik_avg_ms` / `tokenize_beng_avg_ms` **−25~30%**
> 前置实测见 `docs/fts-optimization-report.md`（§3.1 分词成本拆解）
>
> **实施结果（已完成）：端到端 −14%，服务端三相位 −23%。**
> 逐字节等价、逐步 A/B、放弃项与原因见 [`docs/fts-tokenizer-rewrite-report.md`](fts-tokenizer-rewrite-report.md)。

## 1. 现状与成本拆解（实测）

一次 `SELECT tokenize('<80 字中文>', 'ik')` 约 **72~90 µs**，拆分如下：

| 组成 | 成本 | 来源 |
|---|---|---|
| 客户端往返 + 语句开销（`SELECT 1` 对照） | **24~27 µs** | 硬地板，不可优化 |
| `parse_param`（parser 名、属性 JSON 解析、字典加载句柄） | **4.0~4.6 µs** | `ObExprTokenize::parse_param` |
| `tokenize_fulltext` | **26~33 µs** | `ObExprTokenize::tokenize_fulltext` |
| └ helper init + 词表创建 | ~3~5 µs | `ObFTParseHelper::init` / `ObFTWordMap::create` |
| └ IK 逐字解析（CJK 处理器） | **~10 µs**（120 ns/char × 80） | 已存命中遍历 56 ns/char + 新匹配起点 64 ns/char |
| └ 词表插入（`add_token` → `ObFTWordMap`） | ~7 µs（~48 token） | 哈希插入 + word 拷贝 |
| └ JSON 数组构建（`make_token_array_json`） | ~3~5 µs | `ObJsonArray`/`ObJsonString` 逐 token 建树 |
| `pack_json_res`（JSON 树 → 二进制 + 写 datum） | **1.8~2.0 µs** | `ObJsonWrapper::get_raw_binary` |

结论：**服务端 ~46 µs**，其中 CJK 解析只占 10 µs；剩余 36 µs 分布在词表插入、JSON 建树、
字符分类、其余三个处理器（letter/quantifier/surrogate）与 `parse_param`。

## 2. 目标与不可变约束

**目标**：服务端 46 µs → **26~30 µs**（−35~43%），使总耗时 72 µs → 50~55 µs（**−24~30%**）。

**不可变约束（必须逐字节保持）**：

1. `TOKENIZE(...)` 的返回字符串必须**完全相同**（含 token 顺序）；
   ⚠️ 当前输出顺序是 `ObFTWordMap`（哈希桶）的遍历顺序，**不是**字典序、也不是文本出现顺序。
   因此任何替换词表结构的方案都必须复现同样的顺序；否则 `tokenize_edge` 用例与官方 TOKENIZE
   校验都会失败。
2. 分词语义（哪些字符成词、停用词过滤、大小写折叠、中文数字、ngram 切分）不得变化。
3. 不得减少文档/字符处理量、不得跳过 LOB 读取或停用词检查。
4. 不改变 `OS_FTS_TOKENIZE` 表达式的签名、错误码与参数校验行为。

## 3. 方案设计（按收益排序）

### 3.1 JSON 输出直写二进制（预计 −3~5 µs）

现状：`make_token_array_json()` 逐 token `ObJsonString::set_value` + `append` 到 `ObJsonArray`，
再由 `ObJsonWrapper::get_raw_binary` 序列化（pack 只 1.9 µs，说明建树本身也不昂贵，收益有限，
但可与 3.2 合并实现）。

设计：跳过语法树，直接按 seekdb 二进制 JSON 布局写 `[ "w1", "w2", ... ]`：
- 先算出总长度（每个 string 的 header + payload），一次分配；
- 依序写入 array 头 + 各 string node；
- 输出必须与当前 `get_raw_binary` 的结果**逐字节一致**（用 dump 校验）。

⚠️ 前置验证：先用一个最小用例确认 `ObJsonWrapper` 的二进制布局对"纯字符串数组"是稳定且可复现的；
若存在 format 版本差异或对齐填充规则，则放弃本节，只做 3.2~3.5。

### 3.2 词表：定长数组 + 保持哈希桶顺序（预计 −5~7 µs）

现状：`ObFTWordMap` 每 token 一次哈希插入 + `ObString` key 拷贝到 map 内存。

设计（两种，按可行性择一）：

- **方案 A（保守，推荐）**：保留 `ObFTWordMap` 的哈希函数与桶数计算
  （`ft_word_bkt_cnt = MIN(MAX(len/2, 2), 997)`），但把桶内链表换成**定长数组 + 计数**
  （每桶容量 8，溢出回退链表），消除逐 token 的节点分配；遍历顺序不变（桶序 + 桶内插入序）。
- **方案 B（激进）**：单遍解析把 token 直接 append 到定长数组（容量 `len/2 + 8`），
  再用一个"局部哈希表"做计数去重；输出时按原桶序重排。**必须证明重排后的顺序与现状一致**，
  否则放弃。

验收关键：`tokenize_edge` 用例的 14 组输出逐字节一致（它覆盖了大小写、停用词、中文数字、
全角、日韩、emoji、ngram）。

### 3.3 字符分类查表化（预计 −3~4 µs）

现状：每个字符走 `classify_first_char()`（多分支 + DFA/dict 查询）。

设计：
- ASCII（`< 0x80`）：一张 256 项常量表，直接得到 `{LETTER, DIGIT, USELESS, SPACE}`；
- CJK 区间：一次范围比较（用已算好的 code point，避免重复解码）；
- 其余走现有慢路径。
- 分类函数保持纯函数、无状态，避免引入跨文档状态。

### 3.4 处理器分派去虚化（预计 −2~3 µs）

现状：`ObFTParseHelper::segment()` 对每个字符遍历 `ObList<ObIIKProcessor*>`（4 个处理器），
对每个都发一次虚调用 `do_process()`。

设计：
- 用一个 8 位掩码表示"本字符类型需要哪些处理器"，一次 `switch` 直接调用具体处理器的成员函数
  （或用函数指针表，但避免 4 次虚调用 + 链表遍历）；
- 空转的处理器不再被调用（现状是每个字符都调 4 次，其中 2~3 次是空转）。

### 3.5 `parse_param` 结果缓存（预计 −3~4 µs）

现状：每次 TOKENIZE 都重新解析 parser 属性 JSON（`ObString properties` → 结构体）。

设计：
- 以 `(parser_name, properties 的哈希)` 为 key 做一个**线程局部**小缓存（容量 8，命中率近乎 100%）；
- 缓存值为已解析的 `ObFTParserProperty`（不持有指向临时 allocator 的指针）；
- 必须有失效机制：properties 变化时（哈希不同）自然 miss；缓存容量固定、无增长；
- 若担心多租户内存，可改为"每个 `ObFTParseHelper` 实例复用上次解析结果"（同一 helper 生命周期内
  属性不变），收益相近且无全局状态。

## 4. 实施顺序与验收

按 3.3 → 3.4 → 3.5 → 3.2 → 3.1 的顺序做（成本从低到高、风险从低到高），每步单独验收：

| 步骤 | 验收 |
|---|---|
| 每一步 | `tokenize_edge` 14 组 + `correctness_dump` 中全部 TOKENIZE 用例逐字节一致 |
| 每一步 | 交替 A/B（同一机器、两二进制、8 轮配对）测 `tokenize_ik` / `tokenize_beng`；单步收益 <3% 则回退该步 |
| 全部完成 | `tokenize_ik` 相对改动前 **≥25%**；`build_*` 不得退化（构建里也调用分词） |

## 5. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 输出顺序变化（最常见） | 每一步都用 `tokenize_edge` 锁死输出；顺序有任何变化立即回退 |
| 二进制 JSON 布局不匹配 | 3.1 单独可回退（保留原 `make_token_array_json` 路径，加配置开关） |
| 定长数组溢出 | 容量按 `len/2+8` 估算，超限回退到现有 `ObFTWordMap` 路径 |
| 线程局部缓存内存 | 容量固定 8 项，值不含指针；或改为 helper 级复用 |

---

## 6. 给执行 agent 的提示词

```
背景
seekdb 的 TOKENIZE 表达式一次调用约 72~90µs，其中 24~27µs 是客户端往返（不可优化），服务端约
46µs。实测拆解：parse_param 4~4.6µs、tokenize_fulltext 26~33µs（其中 CJK 逐字解析约 10µs、
词表插入约 7µs、JSON 建树 3~5µs）、pack_json_res 1.9µs。目标是把服务端降到 26~30µs，整体
TOKENIZE 指标降低 25% 以上。

必读
- docs/fts-optimization-report.md（§3.1 分词成本拆解、§5.1 A/B 方法论）
- 本文件 §3 的方案设计
- src/sql/engine/expr/ob_expr_tokenize.cpp（parse_param / tokenize_fulltext / pack_json_res）
- src/storage/fts/ob_fts_parser_helper.cpp（init / segment / make_token_array_json）
- src/storage/fts/ik/ob_ik_processor.cpp、ob_ik_char_util.h、各 processor
- src/storage/fts/ob_fts_word_map.h（ObFTWordMap）

硬约束（违反任何一条即视为失败）
1. TOKENIZE 的返回字符串必须逐字节不变，**包括 token 顺序**。当前顺序是 ObFTWordMap 哈希桶的
   遍历顺序，不是字典序；任何替换词表结构的方案都必须复现该顺序，否则回退。
2. 分词语义不变：成词规则、停用词过滤、大小写折叠、中文数字、ngram。
3. 不得减少处理的字符/token 数，不得跳过 LOB 读取或停用词检查。
4. 不改变 OS_FTS_TOKENIZE 的参数校验、错误码、签名。

执行方式
1. 按 §4 的顺序做（先 3.3 字符分类查表、再 3.4 处理器分派、再 3.5 parse 缓存、再 3.2 词表、
   最后 3.1 JSON 直写）。
2. 每一步完成后立刻验证，不要攒到最后：
   - 等价性：`tokenize_edge` 用例（14 组）+ 36 形态 correctness dump 中所有 TOKENIZE 输出逐字节比对；
   - 性能：同一台机器上两个二进制交替、8 轮配对取中位（单次跑对固定基线不可信）；
   - 单步收益 <3% 或出现任何输出差异 → 立即回退该步，不要继续叠加。
3. 编译用 `cmake --build build_release --target seekdb -j12`（磁盘紧张，不要开满线程）。
4. 每步保留可回退开关（或保留原代码路径），最终确认开关两侧行为一致后再合并。
5. 全部完成后必须证明 `build_*` 指标没有退化（构建路径也调用同一个分词器）。

最终报告需包含：每步改了什么、每步的 A/B 数据（配对中位 + stderr）、等价性证据（哪些用例、
多少行输出）、被回退的步骤及原因、以及"输出顺序如何保持不变"的说明。
若最终总收益不足 25%，如实报告并停止继续加复杂度。
```
