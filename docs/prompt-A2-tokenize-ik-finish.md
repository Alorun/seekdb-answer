# 提示词 A2：把 `tokenize_ik` 从 0.0473 压到 0.0410 ms（路线 A 收尾）

> 自包含提示词。你可以与构建侧/查询侧的 agent **并行**工作，必须遵守 §0 的隔离协议。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`，HEAD `bbe2da610`）
>
> 本项是"路线 A"的收尾：上一轮（C2）已把 IK 的**双数组 trie 换成紧凑 trie**（`IKFastDict`），
> 拿到 `tokenize_ik` +6%。本轮的 8.9 个点缺口**全部压在分词类**上，目标是再拿 −13%。

## 0. 并行隔离协议（先读，否则 A/B 无效）

1. 开工前把要改的文件 `cp` 到 `.tmpdev/a2/orig/`（`.git` 只读，恢复只能靠 `cp`）。
2. **A/B 做法**：构建 `bin-cand` → `cp` 恢复为开工前版本 → 构建 `bin-base` → `cp` 恢复改动。
   两臂只差你的改动，别人的并行改动对两臂是常数。
3. **构建串行**（三人共用一个 `build_release`）：
   ```bash
   flock /tmp/fts-build.lock bash -c 'cd /home/hahaha/ob-contest/seekdb-index-tuning && \
     TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12'
   ```
   （`TMPDIR` 必须指定，否则 Rust 组件编译失败。）
4. 实例用**自己的端口 2894**、目录 `.tmpdev/a2/inst`；不要用 2881/2891/2892/2893。
5. 不要 `pkill -f seekdb`；停实例用 `sdb.py stop`。磁盘需 >10GB。

## 1. 当前状态与目标

平台实测（官方 3000×3 口径）：

| 指标 | 基线 | 当前 | 提升 |
|---|---|---|---|
| `tokenize_ik_avg_ms` | 0.0720 | **0.0473** | **+34.3%** |
| `tokenize_beng_avg_ms` | 0.0423 | 0.0332 | +21.5% |

综合 47.02%（94.05 分），缺口 +8.9 个点。**只在分词侧补**：
- 目标 `tokenize_ik` ≤ **0.0410 ms**（提升到 43.0%）、`tokenize_beng` ≤ **0.0296 ms**（提升到 30.0%）；
- 若只能改 ik，则 ik 需要 ≤ **0.0350 ms**（提升到 51.4%）才能单独补满 —— 所以**优先做 ik**，
  有余力再做 beng。

### 成本上限（必须理解，避免做无用功）
`tokenize` 的官方测量是 3000 条独立 SQL 除总墙钟，**客户端往返 + 语句开销约 25 µs 不可优化**
（`select1` 实测 0.025）。所以：
- `tokenize_ik` 0.0473 → 服务端约 **22.3 µs**；要到 0.0410 需服务端 **16 µs**（−28%）；
- `tokenize_beng` 0.0332 → 服务端约 **8.2 µs**；要到 0.0296 需服务端 **4.6 µs**（−44%）。

## 2. 上一轮已完成的（不要重复做）

1. `IKFastDict`（`src/storage/fts/ik/ob_ik_fast_dict.h`）：**紧凑前缀 trie**，替代双数组 trie；
   - 根级（`len<=3` 的 CJK 字符）用 `1<<16` 码点直接表；
   - 子节点用 `kids_[key_, child_]` 数组 + **二分查找**；
   - 节点字 `WORD_FLAG` 位标记"词在此结束"；
   - 灰度开关 `OB_FTS_IK_FAST_INDEX`（=0 走旧双数组实现）。
2. `IKFastHitArray`（`ob_ik_cjk_processor.h`）：定长数组 + 保序 erase，替代 `ObList<ObDATrieHit>`。
3. 输出顺序严格保持不变（`ObFTWordMap` 哈希桶序）：改造只改"怎么找到词"，**没有改 `add_token` 调用序列**。
4. 已通过：36 形态 dump（仅 `order_rel` 差异）、**745 行 TOKENIZE 压测逐字节相同**、
   `tokenize_edge` 14 组、5 个仓库用例、DDL 14/14。

**已证否**：CJK trie 查找"复用"（−5.23%）；beng ASCII 快路径（−0.65%，落在噪声内，未保留）。

## 3. 第一步（强制）：把剩余 ~22 µs 再拆成三块

上一轮只测到 `segment` ≈ 16.5 µs（80 字）与 `tokenize_fulltext` ≈ 17.5 µs，
但**没有再往下拆**。本轮先做这一段（`getenv("OB_FTS_PROF")` 门控、采样 1/64、验完删除）：

| 块 | 计时点 | 预期 |
|---|---|---|
| **字典跳转** | `do_process_fast` 里 `dict.child()/root_child()/is_word()` 的全部耗时 | 80 字 × 2~3 跳 ≈ 4~6 µs |
| **词表插入** | `ctx.add_token()` 全程（含 `ObFTWordMap` 的 hash 查找 + 插入 + 可能的 word 拷贝） | 48 token × 100~150 ns ≈ 5~7 µs |
| **其余** | 字符分类、`hits_` 维护、`TokenizeContext` 记账 | 1~3 µs |

跑 4000 次取稳态。**读数决定下面做哪一条**，不要凭直觉选。

## 4. 按数据实施（只做最大的一块；两块相当就都做）

### 4.1 若「词表插入」最大（预测概率最高）
1. **先确认 `add_token` 是否拷贝 word**：看 `TokenizeContext::add_token` 与
   `ObFTWordMap` 的 key 语义（`ObString` 是指向源文本还是复制进 map 的 allocator）。
   - 若是复制：评估改为**只存 `(offset, length)` 指针**。前提是 map 的生命周期**短于**源文本 buffer：
     - TOKENIZE 路径：源文本来自表达式参数，生命周期覆盖整个调用 ✓；
     - 构建路径：源文本来自 LOB buffer，生命周期覆盖单篇文档的处理 ✓；
     - 必须逐路径确认后再改，并在报告里写清依据。
2. **预分配**：`ObFTWordMap::create(bkt_cnt)` 的桶数按 `len/2` 估算；
   确认是否每文档都重建 map（若可复用则复用，参考已有 `reusable_word_map` 机制）。
3. **换更快的插入**：若当前是通用 `ObHashMap`（带节点分配或链表），
   评估**开放寻址 + 内联 key 比较**的专用小表（容量 64/128，词数 <100 时更快）。
   ⚠️ 桶序（遍历顺序）必须与现状一致，否则输出顺序会变 —— 见 §5。

### 4.2 若「字典跳转」最大
1. **转移缓存**：direct-mapped 表（建议 8K~16K 项），键 `hash(node, ch)`，值 `child`。
   纯缓存、语义透明（miss 时回落到二分查找）。跨 3000 次执行会快速变热，单次 80 字也能命中部分。
2. **根级扩展**：现在 `len<=3` 走码点直接表；确认 4 字节字符（emoji、扩展 B 区汉字）的二分查找
   是否在中文文本里出现（一般不会），若出现比例 >5% 再优化。
3. **减少 `is_word()` 的额外访存**：`WORD_FLAG` 已与 children offset 合并在同一字里 ✓，
   确认没有二次加载。

### 4.3 若「其余」最大
字符分类已查表化；检查 `TokenizeContext` 的 `add_token` 记账（`doc_len`/`char_cnt` 累加）是否可合并。

## 5. 硬约束（逐字节不变，违反即失败）

1. **`TOKENIZE(...)` 返回字符串逐字节不变，含 token 顺序**；
   顺序来源见 `docs/prompt-C2-report.md` §2（`ObFTWordMap` 桶下标 + 桶内**头插链表**）——
   **任何改动都不得改变 `add_token` 的调用序列，也不得改变 map 的桶数/哈希函数**；
2. 分词语义不变（成词规则、停用词、大小写折叠、中文数字、ngram、全角/日韩/emoji）；
3. 不得减少处理的字符/token 数、不得跳过停用词检查；
4. **`build_*` 不得退化**（构建走同一个 `segment()`）；
5. 保留灰度开关（`OB_FTS_IK_FAST_INDEX` 已有，新的改动也要能一键回退）；
6. **禁止**做"把 `tokenize(常量)` 常量折叠缓存"这类改动：IK 词典内容可随库变化，
   tokenize 不是纯函数，折叠会改变语义。

## 6. 验收门

1. **官方口径**（`tools/benchmark/fts_large_bench.sh`，ROUNDS=3000 / SAMPLES=3）：
   `tokenize_ik` ≤ **0.0410 ms**；若做了 beng，`tokenize_beng` ≤ **0.0296 ms**；
2. 8 轮配对 A/B（`TMPDIR=/tmp bash .tmpdev/ab_pingpong.sh <bin-base> <bin-cand> 8 6`）：
   配对中位 ≥3% 才保留；**注意两臂可能存在 4~5% 的臂间偏差**（用 `select1` 与未改指标做对照，
   报告里必须写出对照值）；
3. **逐字节**：36 形态 dump（仅 `order_rel` 允许差异）+ **745 行 TOKENIZE 压测**
   （`.tmpdev/tri/c2/stress_tok.sql`，370 文本 × 2 输出模式 + 15 个 beng 用例）
   + `tokenize_edge` 14 组，全部逐字节相同；
4. 5 个仓库用例 + DDL 专项 14 项 + 命中数 `8001/11000/7332/20`；
5. `build_*` 用块模式 A/B 确认未退化（≥3 块）。

## 7. 环境坑（都踩过）

1. `git checkout/stash` 失败（`.git` 只读）；用 `cp` 备份恢复；
2. 不要 `pkill -f seekdb`（会杀沙箱包装进程）；
3. 实例启动：`sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2894`；
4. 磁盘 <10GB 时服务端报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)` 拒绝启动；
5. 本机 `select1` 在 0.0204~0.0358 漂移，**禁止**用单次跑 vs 固定基线；
   `tokenize_beng` 样本波动可达 ±20%，至少 8 样本再下结论。

## 8. 交付要求

报告写清：
1. 三块（字典跳转 / 词表插入 / 其余）的实测纳秒数与占比，以及它们如何解释 22.3 µs 服务端；
2. 选了哪一块、怎么改、为什么（若改 `add_token` 的 word 拷贝，必须写清"生命周期为何安全"）；
3. **"输出顺序如何保持不变"的说明**；
4. 官方口径的 `tokenize_ik` / `tokenize_beng` 数值（3000×3）+ 8 轮配对数据（含 `select1` 与未改指标的对照）；
5. 正确性证据（dump 差异行数、745 行压测、`tokenize_edge`、5 用例、DDL 14 项、命中数）；
6. `build_*` 是否退化；
7. 未完成部分与原因；本地数字与平台分数的区分。
