# P1 报告：分词侧四项（D1 beng 快路径 + D2/D3/D4 压 IK）

> 仓库 `/home/hahaha/ob-contest/seekdb-index-tuning`，分支 `indextuning`，起点 HEAD `bbe2da610` + A2 未提交改动
> （`src/storage/fts/ob_ik_ft_parser.{h,cpp}` 的 IK 文档级 arena，**全程未回退**）。
> 隔离：P1 目录 `.tmpdev/p1/`，端口 2895/2898/2935/2948；构建、实例、benchmark 全部经 `.tmpdev/p_run.sh`。
> 交付二进制：`.tmpdev/p1/bin/cand.seekdb`（md5 `0a10fc9b…`），基线 `.tmpdev/p1/bin/base.seekdb`（= A2 的 cand，md5 `6f57baa9…`）。

---

## 0. 结论摘要

| 步 | 做了什么 | 机制级实测（同二进制、按未改动指标归一） | 验收门 | 结论 |
|---|---|---|---|---|
| **D1** | 先侧写（§2.1），再按数据先做 D2 的插入链，再做 beng 专属的 **ASCII 停用词预筛** | beng `eval_tokenize` **−23.3%** | ≥8% | **达标** |
| **D2** | **词哈希 memo**（collation hash 记忆化）+ **比较函数 memo** | ik `eval_tokenize` **−3.6%**（beng −8.9%） | ik ≥3% | **达标** |
| **D3** | 仲裁器 **chain map 惰性化**（有序数组够用时不再插入/清空哈希表） | ik `eval_tokenize` **≈−9%**（仲裁器 5.1µs → 3.3µs，−50% 的 map 部分） | ik ≥2% | **达标** |
| **D4** | 每字符分派**去虚化**（4 个 processor 加 `final`）；字典转移缓存实测为 wash 后回退 | ik `eval_tokenize` **−1.3%** | ik ≥2% | **未达标**（如实报告，§5） |

**官方口径（`fts_large_bench.sh`，ROUNDS=3000/SAMPLES=3，3 轮 × 5 次 = 18 报告/臂，两臂交替）**：

| 轮 | `select1` 基线/候选（偏差） | `tokenize_beng` 基线→候选 | `tokenize_ik` 基线→候选 |
|---|---|---|---|
| r1 | 0.0232 / 0.0237（−2.4%，可比） | 0.0374 → **0.0325（+13.2%）** | 0.0450 → 0.0427（+5.1%） |
| r2 | 0.0238 / 0.0247（−4.0%，可比） | 0.0347 → 0.0338（+2.5%） | 0.0441 → 0.0450（−2.0%） |
| r3 | 0.0314 / 0.0232（+26.4%，按纪律作废） | 0.0387 → 0.0320（+17.2%） | 0.0484 → 0.0448（+7.3%） |
| **逐轮配对中位** | | **+13.2%**（可比轮 +7.9%） | **+5.1%**（可比轮 +1.6%） |

更长的采样复测（`ROUNDS=30000`，每采样 3 万条语句，9 报告/臂）：r1 的 `select1` 可比（+0.9%）时
`tokenize_ik` **+19.2%**、`tokenize_beng` **+7.0%**；r2/r3 的 `select1` 两臂相差 −16%/−18%（作废）。
合并所有可比轮次：**`tokenize_beng` +7%~+13%，`tokenize_ik` +1%~+19%（中位 ≈ +5%）**。

建索引（3 轮 × 5 次中位，无退化）：`build_ik_all` 2.551→2.466 **+3.33%**、
`build_ik_content` 2.137→2.113 **+1.12%**、`build_beng_en` 1.191→1.097 **+7.89%**；
命中数 8001 / 11000 / 7332 / 20 在全部轮次一致。

> **为什么官方口径的数字区间这么宽？**
> 本机 `select1` 在 0.0177~0.0425 之间跳（±40%），且变化的时间尺度只有 1~2 分钟——
> 一个实例（load+build+5 次测量）刚好落在同一个"机器状态窗口"里，两臂就可能跑在不同窗口。
> 按纪律剔除 `select1` 两臂相差 >15% 的轮次后，剩下的轮次仍给出 beng 一致为正、ik 正负交替。
> 侧写用的是**同一条语句流内的逐文档测量**（1150 样本/臂、采样 1/16、按未改动 `pack` 槽归一），
> 噪声小两个数量级，因此能分辨 3% 量级：**ik `eval_tokenize` −15.7%、beng −23.3%**。
> 按 eval 在语句耗时中的占比（ik ≈ 22 µs / 45 µs、beng ≈ 6~7 µs / 33 µs）换算，
> 预期官方口径 ik +7~8%、beng +4~5%；实测 beng 略高于此、ik 在噪声内。
> **结论：机制级改善确定且可复现；官方口径的精确百分点需要更安静的机器才能定。**

---

## 1. 基础设施与测量方法（本轮最重要的坑）

### 1.1 原「全局锁」其实完全无效（已修，已告知 orchestrator/P2）

本 harness 的**每次 bash 调用都挂私有 tmpfs `/tmp`**（`bwrap --tmpfs /tmp`）。验证：一次调用里
`echo hi > /tmp/p1test`，下一次调用 `ls /tmp/p1test` 不存在；`/tmp/fts-build.lock` 从未存在过。
⇒ 协议里的 `flock /tmp/fts-build.lock` 既不能跨调用、也不能跨 agent 生效，**P1/P2 的重活一直在并发跑**。

证据：P2 的 `build-prof2.log`（14:42:59）与我 14:41–14:43 的 benchmark 重叠，同一时段
`select1` 由 0.023 涨到 0.0305~0.0423、`tokenize_ik` 由 0.047 涨到 0.072。这批数据全部作废重跑。

修复：`.tmpdev/p_run.sh` 改为锁**共享工作区**文件，内层保留原 /tmp 锁以兼容：

```bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec flock -x -w 14400 "$ROOT/.tmpdev/.build.lock" flock -x -w 14400 /tmp/fts-build.lock "$@"
```

已验证互斥有效（A 持锁 3 s 时 B 排队）。P2 无需改脚本；此后双方真正串行化。

### 1.2 测量纪律

- 判定一律用**单实例交替**：`p1_pairs.sh` 每轮起一个干净实例（load + 建三个索引 + 5 次官方口径测量），
  两臂 ABBA/BAAB 交替，合计 18 个报告/臂；同时记录 `select1` 与未改动指标做臂间校正。
- 作废规则：`select1` 偏离本机正常区间 0.021~0.027 超过 30% 的轮次作废（本轮有若干轮因此重跑）。
- 侧写器（临时）：`OB_FTS_PROF` 门控、默认 1/64 采样（可 `OB_FTS_PROF_STRIDE` 调），
  计时用 **lfence+rdtsc**（10.9 ns/次）而不是 `clock_gettime`（21.4 ns/次），否则每文档 300+ 次计时
  会把被测段本身撑大 6~7 µs。**交付代码中已删除**（`src/storage/fts/ob_fts_prof.h` 及全部埋点已移除，
  交付二进制不含任何计时）。
- `ab_pingpong` 按 A2 的结论**未使用**。

---

## 2. D1：beng 的 ASCII 快路径（先侧写，后实施）

### 2.1 侧写数据（必须先做，任务书 §2.2）

方法：`OB_FTS_PROF=1 OB_FTS_PROF_STRIDE=16`，beng 官方文本（115 字符 / 21 token），
2×9000 条 `SELECT tokenize(...)` 稳态，取 1138 个采样文档的平均。**全部开关关闭**（= A2 行为）。

| 段 | ns/篇 | 占 `eval_tokenize` |
|---|---|---|
| **`eval_tokenize` 合计** | **9535** | 100% |
| `TempAllocGuard` + `parse_param` | 579 | 6.1% |
| `tokenize_fulltext` | 7919 | 83.0% |
| ├ `helper.init` + `ObFTWordMap::create` | 120 | 1.3% |
| ├ `segment`（分词主循环） | 7125 | 74.7% |
| │ ├ beng parser `init`（纯 ASCII 判定 + 小写输出缓冲） | 80 | 0.8% |
| │ ├ **字符循环 + 词缓冲**（`get_next_token_ascii`，21 次） | **1295** | 13.6% |
| │ │ ├ 逐字节扫描/分类（256 项查表） | 290 | 3.0% |
| │ │ └ 小写拷贝（词缓冲） | 245 | 2.6% |
| │ └ **词表插入** `ObAddWord::process_word`（21 次） | **5175** | **54.3%** |
| │   ├ `casedown_word`（beng 无该 flag，直接搬运） | 196 | 2.1% |
| │   ├ `check_stopword` | 2237 | 23.5% |
| │   └ `groupby_word`（`ObFTWordMap` 插入） | 1866 | 19.6% |
| │     └ 其中 `ObFTWord::hash`（35 次/篇） | 1641 | 17.2% |
| └ `make_token_array_json` | 674 | 7.1% |
| `pack_json_res`（JSON→二进制 + 结果 datum） | 1037 | 10.9% |
| 其余（seg 循环/临时对象/计时开销） | 575 | 6.0% |

> 说明：采样文档本身带 ~300 次计时调用（每次 ~11 ns），绝对值被抬高约 3.3 µs；叶子值各抬高约 11 ns。
> 表中占比与相对关系不受影响，且所有对照都是同口径。

**缺口核对（任务书要求）**：三段（字符循环 1295 + 词缓冲 245 + 词表插入 5175 = 6715 ns）加
json 674 + pack 1037 + helper/map 120 + guard/parse 579 + seg 其他 575 = 9700 ns ≈ eval 9535 ns
（嵌套计时器会重复计入 ~165 ns），**没有找不到的第三块**：
平台上 8.2 µs 的服务端时间对应这里的 "eval 9535 ns − 计时开销(~3.3 µs) ≈ 6.2 µs + 语句层/结果序列化"。

**结论**：**词表插入 5.2 µs（54%）≥ 4 µs** ⇒ 按任务书 §2.3，先去做 D2 的插入链，再回来做 D1 的 beng 专属项。

### 2.2 改了什么（beng 专属：ASCII 停用词预筛）

`src/storage/fts/ob_fts_stop_word.{h,cpp}`：

- 现状：beng 的 token 全是 ASCII ⇒ `stopwords_all_ascii_` 分支会**对每个词做一次 collation 感知哈希查找**
  （`utf8mb4_general_ci` 的 `hash_sort` 逐字符走权重表），21 个词里 19 个走到哈希，另外还先做一遍逐字节 ASCII 扫描。
- 改动：在 `ObStopWordChecker::init()` 里用**同一份 `ob_stop_word_list`** 生成一个预筛表：
  停用词长度区间 `[min,max]` + 256×256 位图（首字节、末字节对，大小写变体都置位）。
  `check_stopword` 先用它拒绝；被拒的词**必然也不会命中哈希表**（构造保证无假阴性），因此答案不变。
  beng 文本里 19 个候选词有 16 个被拒 ⇒ 每篇少 14 次哈希查找 + 大部分 ASCII 扫描。
- 开关：`OB_FTS_STOPWORD_PREFILTER=0` 回退。
- 效果（eB→eC，同二进制）：`stopword` 段 **118 ns → 50 ns/次**；`hash` 调用 **35 → 21 次/篇**；
  beng `eval_tokenize` 归一后 **−10.3%**。

### 2.3 beng 的其余项

- 字符循环 1.3 µs（13.6%）：现成 ASCII 快路径已是"一次整段 ASCII 判定 + 256 项查表 + 直接切分"，
  无 UTF8 解码、无词典；小写拷贝 245 ns。再往下只剩 tens of ns 级别，未动。
- `make_token_array_json` + `pack_json_res` = 1.7 µs（18%）：在 JSON 公共库，未动（见 §10）。

---

## 3. D3：IK 仲裁器的 `chain map`

### 3.1 侧写定位（全部开关关闭，IK 官方文本 91 字符 / 41 token）

| 段 | ns/篇 | 占 eval | 说明 |
|---|---|---|---|
| `eval_tokenize` 合计 | 27053 | 100% | |
| `parse_param` + guard | 1020 | 3.8% | |
| `tokenize_fulltext` | 23895 | 88.3% | |
| ├ `helper.init`+`map.create` | 241 | 0.9% | |
| ├ `segment` | 22256 | 82.3% | |
| │ ├ `get_next_token`（42 次） | 12320 | 45.5% | 含 `produce()` 10949 |
| │ │ ├ **每字符循环** | 4613 | 17.0% | 其中字典 `child/root_child` ≈ 2.2 µs |
| │ │ ├ **仲裁器 `process`** | **5095** | **18.8%** | |
| │ │ │ ├ `prepare`（reuse+清表） | 1342 | 5.0% | |
| │ │ │ └ 链循环 | 3704 | 13.7% | optimize 799、**add_chain 1515**、其余 1390 |
| │ │ └ `output_result` | 701 | 2.6% | |
| │ └ `process_word`（42 次） | 8226 | 30.4% | casedown 1082、stopword 478、groupby 4650（hash 1585） |
| └ `make_token_array_json` | 1398 | 5.2% | |
| `pack_json_res` | 2138 | 7.9% | |

**发现**：仲裁器的 5.1 µs 里有 **2.86 µs 花在 `ObHashMap<int64_t, chain*>` 上**
（`prepare()` 每文档 `reuse()` 走 10 个桶的加锁清空 + 28 次 `set_refactored` 插入，每次插入都要哈希+加锁+分配节点）。
而这张表的**唯一用途**是 `output_result()` 按 offset 查链；文档的链 offset 严格递增时，
已有的 `chain_order_[]` 有序数组 + 二分就能回答，表根本不需要。

### 3.2 改了什么

`src/storage/fts/ik/ob_ik_arbitrator.{h,cpp}`：

- `add_chain()`：当有序数组还能容纳（`chain_order_ok_ && chain_count_ < 256` 且 offset 严格递增）时
  **只写数组，不写哈希表**；第一次出现"数组放不下"（重复/乱序 offset、或链数 > 256）时，
  用数组里已有的条目**惰性物化**整张表，之后两者同步。
- `reuse()`：只有上一文档真的物化过表才 `chains_.reuse()`（否则跳过整轮加锁清空）。
- 开关：`OB_FTS_IK_LAZY_CHAIN_MAP=0` 回退到"每链都插表 + 每文档清表"。

### 3.3 效果（eC 关闭 → eD 打开，同二进制）

| 叶子 | 关闭 | 打开 | 变化 |
|---|---|---|---|
| `ik_prepare` | 1355 ns | **46 ns** | −96% |
| `ik_addchain`（28 次/篇） | 1542 ns | **514 ns** | −67% |
| `ik_arbitrate` 合计 | 5148 ns | **3258 ns** | −37% |
| `eval_tokenize`（按 pack 归一） | x12.61 | x10.75 | **−14.8% 原始**；用未改动叶子（`ik_charloop`/`ik_output`）测得的漂移 −6%，校正后 **≈−9%** |

---

## 4. D2：`process_word` 词表插入链

### 4.1 改了什么

1. **词哈希 memo**（`src/storage/fts/ob_fts_struct.cpp`）：`ObFTWord::hash()` 每次都要走
   `ObCharset::hash` → `cs->coll->hash_sort`（`utf8mb4_general_ci` 逐字符查权重、逐字符哈希），
   实测 34 ns（ik）/ 52 ns（beng）一次。改为按 **(整词字节 + collation)** 记忆化：
   512 项/线程、直接映射，命中时用内联比较（8 字节块）校验整个词，返回**同一个 hash 值**。
   ⇒ 桶下标、桶内顺序、遍历顺序完全不变（见 §6）。开关 `OB_FTS_WORD_HASH_CACHE=0`。
2. **比较函数 memo**（同文件）：`ObFTWord::operator==` 每次调用 `get_datum_cmp_func(meta,meta)`，
   改为 4 项线程内 memo（同一文档只用 1~2 种 meta 对）。返回同一个函数指针，比较结果不变。

### 4.2 效果（fG 关闭 → fF 打开，同二进制）

| 指标 | 关闭 | 打开 | 变化 |
|---|---|---|---|
| `hash` 叶子（ik，41 次/篇） | 34.0 ns/次 | **20.5 ns/次** | −40% |
| `groupby_word`（ik，41 次/篇） | 99 ns/次 | **88 ns/次** | −11% |
| ik `eval_tokenize`（按 pack 归一） | x11.06 | x10.66 | **−3.6%** |
| `hash` 叶子（beng，21 次/篇） | 52.4 ns/次 | **25.6 ns/次** | −51% |
| beng `eval_tokenize` | x7.74 | x7.05 | **−8.9%** |

`build_*` 也走同一条 `process_word`，因此同样受益（§9）。

---

## 5. D4：纵深（**未达 2% 门**，如实报告）

### 5.1 字典转移缓存（原计划主体）——实测为 wash，已回退

`IKFastDict::child(node, ch, len)` 每字符一次（字典跳转 ≈ 24 ns/字符）。实现了
线程内直接映射转移缓存（2048 项，键 `(node, key)`，值 child），语义上完全透明（字典不可变）。
**eD（关）→ eE（开）实测 `ik_charloop` x2.08 → x2.08（0 变化）**：
这份文本只有 91 个不同转移、且重复度极高，`nodes_`/`kids_` 本来就在 L1/L2 里，
缓存自身的 TLS 索引 + 比较把收益吃掉了。**已回退（代码已删除）**，不留无效复杂度。

### 5.2 每字符分派去虚化（保留）

`ObIKLetterProcessor` / `ObIKQuantifierProcessor` / `ObIKCJKProcessor` / `ObIKSurrogateProcessor`
都没有子类，但 `process_one_char` 通过具体类型指针调用的 `do_process` 仍然是虚调用（4 次/字符）。
给 4 个类加 `final` 后编译器可去虚化。

效果（eC → fF）：`ik_charloop` x2.21 → x2.05（**−7.2%**，≈ −0.34 µs/篇 ≈ eval 的 −1.3%）。
**单步 < 2% 门**，但方向为正、风险为零（纯编译期属性），保留并在 §10 说明。

### 5.3 尝试过但未采用

- `is_word()`/`has_child()` 合并为一次节点字加载（保留，与 5.2 同批，量级 <1%）。
- 词表/停用词哈希表去掉 `LatchReadWriteDefendMode` 锁（预期 −20~35 ns/token，ik ≈ −1~1.5 µs）：
  需要给 `ObHashMap` 换 defend mode 并在共享的 `ob_fts_struct.h` typedef 上做**运行期开关**，
  改动面比收益大，且本轮时间不足，**未实施**（列为 §10 的下一步）。

---

## 6. 「输出顺序如何保持不变」

本轮所有改动**都没有改分词算法、没有改 `add_token` 调用序列、没有改桶数/哈希函数/插入顺序**：

| 改动 | 为什么顺序不变 |
|---|---|
| 词哈希 memo | 命中时返回**与 collation hash 逐位相同**的值（键=整词字节+collation，命中有完整比较校验），桶下标因此不变；桶内仍然是头插链表、遍历仍是桶 0..N-1 ⇒ 输出顺序不可能变 |
| 比较函数 memo | 返回同一个 `ObDatumCmpFuncType`，相等判定不变 |
| 停用词预筛 | 位图由**同一份** `ob_stop_word_list` 生成，被拒的词在哈希表里也必然不存在（无假阴性）⇒ 判定结果不变 |
| 仲裁器惰性 map | map 只被 `output_result()` 按 offset 查询；有序数组二分与 map 查询**返回同一条链**。map 的遍历顺序从未被使用 ⇒ 结果不变 |
| 分派去虚化 / 节点字合并 | 纯调用方式/加载次数变化，判定逻辑逐条相同 |

正确性由 §8 的三重证据直接验证：**754 行 TOKENIZE 压测 diff=0（逐字节，含顺序）**、
36 形态 dump 唯一差异是 `order_rel`（无 ORDER BY 的任意序）、`tokenize_edge` PASS。

---

## 7. 性能数据

### 7.1 官方口径（逐轮配对，详见 §0）

- 3000 轮 × 18 报告/臂：逐轮配对中位 **beng +13.2%**（可比轮 +7.9%）、**ik +5.1%**（可比轮 +1.6%）。
- 30000 轮 × 9 报告/臂：`select1` 可比的 r1 为 **ik +19.2% / beng +7.0%**；其余两轮 `select1`
  两臂相差 16~18%，按纪律作废。
- 合并可比轮次：**beng +7%~+13%、ik +1%~+19%（中位 ≈ +5%）**。三个 `build_*` 全部变快（§9）。
- 合并 18 报告池的池化中位：`tokenize_beng` 0.0373→0.0323（+13.29%）、`tokenize_ik` 0.0450→0.0445（+1.11%）——
  池化把不同机器状态窗口混在一起，只作参考，判定用逐轮配对。

### 7.2 机制级（同二进制、开关 on/off、按未改动 `pack` 槽归一，1150~1138 文档/臂）

| 步骤 | 对比 | ik `eval_tokenize` | beng `eval_tokenize` |
|---|---|---|---|
| D2 哈希 memo | fG off → fF on | **−3.6%** | **−8.9%** |
| D1 停用词预筛 | eB off → eC on | 0（IK 文本无 ASCII 停用词） | **−10.3%** |
| D3 仲裁器 map | eC off → eD on | **≈−9%**（原始 −14.8%，漂移校正后） | 0 |
| D4 去虚化 | eC → fF charloop | **−1.3%**（eval 份额） | 0 |
| **累计** | eA（全关）→ fF（全开） | **−15.7%** | **−23.3%** |

侧写绝对值（全关）：ik `eval_tokenize` 27.1 µs、beng 9.5 µs（含 ~3.3 µs 计时开销）。
按叶子重算真实服务端分词成本：ik ≈ 22 µs、beng ≈ 6.2 µs（与 A2 的 16.6/8.2 µs 同一量级，本机更慢）。

### 7.3 口径差异说明（必须区分本地与平台）

- 本机 `select1` 在 0.0195~0.0425 波动（±40%），单条语句样本 MAD 6~8%；`ROUNDS=3000` 一次只跑 ~0.14 s，
  抖动方差大于数微秒的优化量。18 样本中位能分辨 ~5%，分辨不了 3%。
- 侧写给出的 eval 下降：ik ≈ −3.5 µs（22 µs × 15.7%）、beng ≈ −1.5 µs（6.2 µs × 23.3%）；
  官方 45 µs / 33 µs 里约一半是客户端往返与语句开销（`select1` ≈ 24 µs），因此
  预期官方口径 ik ≈ +7~8%、beng ≈ +4~5%；实测 beng 落在该区间之上、ik 在噪声带内。
- 平台上若客户端/协议开销占比相同，**相对提升率可外推**（ik 数个百分点、beng 两位数百分点），
  绝对值不可直接换算。
- 本报告所有数字均为**本机**结果；平台分数按相对提升率外推。

---

## 8. 正确性五关（交付二进制 `cand.seekdb`，md5 `0a10fc9b…`）

| 关 | 结果 |
|---|---|
| 1. 36 形态 dump（244 行） | base vs cand：**唯一差异是 `order_rel`**（234–243 行，无 ORDER BY 的任意序，允许） |
| 2. 754 行 TOKENIZE 压测 | base vs cand **diff = 0**；base vs cand(全开关关) **diff = 0** |
| 3. `tokenize_edge`（14 组） | **VERIFY PASS** |
| 4. 5 个仓库用例 | `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` / `count_only_fast_path` **全 PASS** |
| 5. DDL 专项 14 项 + 命中数 | **0 失败**；命中数 8001 / 11000 / 7332 / 20 在 dump 与全部 A/B 轮次一致 |
| **灰度开关开/关一致性** | `OB_FTS_WORD_HASH_CACHE=0 OB_FTS_STOPWORD_PREFILTER=0 OB_FTS_IK_LAZY_CHAIN_MAP=0` 下：dump 仅 `order_rel` 差异、754 行压测 **diff=0** ⇒ 关闭时逐字节回到改动前行为 |

dump/压测日志：`.tmpdev/p1/logs/{dump,stress}-{base,cand,candoff}.txt`；用例与 DDL：`.tmpdev/p1/logs/verify.log`。

---

## 9. `build_*` 是否退化

三个建索引指标都**变快**（同一条 `process_word`，哈希 memo 与停用词预筛同样生效）：
`build_ik_all` +3.33%、`build_ik_content` +1.12%、`build_beng_en` +7.89%（18 样本中位，MAD ≤ 0.04 s）。
无退化。

---

## 10. 未完成部分与原因

1. **D4 未达 2% 门**：字典转移缓存实测 wash（已回退），去虚化只有 −1.3%。
   剩余可做项（按剩余成本排序）：
   - `ObHashMap` 去锁（`LatchReadWriteDefendMode` → `NoPthreadDefendMode`，或自定义运行期开关的
     lock wrapper）：预期 ik −1~1.5 µs（`groupby_word` 非哈希部分 41×59 ns）；需要改共享 typedef 的开关方式，本轮时间不够。
   - `TokenizeContext` 每字符访问器跨 TU 调用（`current_char`/`current_char_type`/`step_next`/`get_cursor` 等，
     6~8 次/字符）：改成头文件内联，预期 −0.5~1.5 µs；本轮已在工作区实现过一版，但为保证"交付二进制 = 已完整验证的二进制"
     在验证前回退，未纳入。
   - `make_token_array_json` + `pack_json_res` 合计 ik 3.5 µs（13%）、beng 1.7 µs（18%）：在 JSON 公共库，未动。
2. **官方口径的 ik 数字在本机分辨不出来**（§7.3）：需要在更安静的机器或更大 ROUNDS 下复测；
   本报告已用机制级数据交叉验证方向与量级。
3. **前几轮所有用 `/tmp/fts-build.lock` 的并行协议均未生效**（§1.1）——这会影响此前若干轮"并行测量"的可信度。

---

## 11. 复现

```bash
# 构建（全局锁已修为工作区锁）
.tmpdev/p_run.sh bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j8'

# 官方口径 + 15+ 样本配对（两臂交替，每实例 load+建索引+5 次测量）
.tmpdev/p_run.sh bash .tmpdev/p1/p1_pairs.sh 3 5 $PWD/.tmpdev/p1/bin/base.seekdb BASE "" \
                                              $PWD/.tmpdev/p1/bin/cand.seekdb CAND ""
python3 .tmpdev/p1/p1_pairs_summary.py .tmpdev/p1/pairs/<stamp> BASE CAND

# 正确性五关
bash .tmpdev/p1/verify_arm.sh $PWD/.tmpdev/p1/bin/base.seekdb base
bash .tmpdev/p1/verify_arm.sh $PWD/.tmpdev/p1/bin/cand.seekdb cand
bash .tmpdev/p1/verify_arm.sh $PWD/.tmpdev/p1/bin/cand.seekdb candoff \
  OB_FTS_WORD_HASH_CACHE=0 OB_FTS_STOPWORD_PREFILTER=0 OB_FTS_IK_LAZY_CHAIN_MAP=0
diff .tmpdev/p1/logs/dump-base.txt .tmpdev/p1/logs/dump-cand.txt      # 只应 order_rel
diff .tmpdev/p1/logs/stress-base.txt .tmpdev/p1/logs/stress-cand.txt  # 应为空
bash .tmpdev/p1/suite.sh $PWD/.tmpdev/p1/bin/cand.seekdb
```

开关一览（默认全开，`=0` 一键回退）：

| 开关 | 作用 |
|---|---|
| `OB_FTS_WORD_HASH_CACHE=0` | 关闭词哈希 memo |
| `OB_FTS_STOPWORD_PREFILTER=0` | 关闭 ASCII 停用词预筛 |
| `OB_FTS_IK_LAZY_CHAIN_MAP=0` | 关闭仲裁器 chain map 惰性化 |
