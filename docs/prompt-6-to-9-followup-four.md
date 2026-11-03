# 提示词 ⑥~⑨：四项后续优化（③深挖 → ⑥映射表合并 → ⑧语句层 → ④双writer排查）

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`，HEAD `967455316`）
>
> 本文件包含**四个子任务，必须按顺序执行**，每个子任务有独立验收门，不达标就回退并进入下一个。
> 不要同时改两处，否则无法归因。

## 全局基线（当前实测，本机 `select1=0.0239`）

```
build:    2.848 / 2.430 / 1.343 s
tokenize: 0.0599 / 0.0387 ms
query:    cn 0.6585 / beng 0.8334 / mixed 0.7550 / limit 0.2701 ms
hits:     8001 / 11000 / 7332 / 20
```
请把上表当作你每一步的对照基线（同机、同样跑法）。

---

# 子任务 A：③ 深挖 TOKENIZE 服务端成本（目标 tokenize 类 +11 个百分点）

## A0. 现状与两个未知数

- 端到端 `tokenize_ik` ≈ **0.0599 ms**，减去客户端往返（`select1` ≈0.0254）⇒ 服务端 ≈ **34 µs**；
- 已测三相位合计 **25.7 µs**（`parse_param` 0.56 + `tokenize_fulltext` 23.2 + `pack_json_res` 1.9）；
- 差距说明有**两处未归属**：
  1. `tokenize_fulltext` **内部**约 **9 µs**（已知 CJK 120ns/char≈10µs、词表≈2µs、JSON≈1µs、init≈1.1µs，
     合计约 14µs，与 23.2µs 差 9µs）；
  2. 语句层约 **8 µs**（不在任何相位里）——归子任务 C 处理。

## A1. 强制第一步：拆开 `tokenize_fulltext`

在 `src/storage/fts/ob_fts_parser_helper.cpp` 的 `tokenize_fulltext` 路径（或
`src/sql/engine/expr/ob_expr_tokenize.cpp` 的 `ObExprTokenize::tokenize_fulltext`）分四段计时，
`getenv("OB_FTS_PROF")` 门控、采样 1/64、用完删除：

| 段 | 计时点 |
|---|---|
| `helper_init` | `ObFTParseHelper::init()` 前后（属性解析、解析器与三本词典获取） |
| `map_create` | `ObFTWordMap::create()` 前后 |
| `segment` | `helper.segment()` 前后（IK 逐字 + `ctx.add_token` 词表插入） |
| `make_json` | `make_token_array_json()` 前后 |

跑 4000 次取稳态。**四项之和与 23.2µs 的差额就是下一层要查的目标**（可能是表达式求值框架、
内存分配、或 `add_token` 内部的 word 拷贝）。

## A2. 按数据选一条实施（只做收益最大的一项）

- **若 `helper_init` ≥3µs**：做解析器/属性缓存（优先"同一 `ObFTParseHelper` 实例内复用"，
  避免全局状态；线程局部 LRU 只能用固定容量 8、值不得持有临时 allocator 指针）；
- **若 `segment` ≥12µs**：压 CJK 热路径——`ctx.add_token` 的 word 拷贝可改为共享同一次分词的源文本
  （存 `(offset,length)`）、字母/数量词/代理对三个处理器的空转调用按字符类型位掩码合并分派、
  减少 `match_with_hit` 调用次数；
- **若 `make_json` ≥3µs**：按二进制 JSON 布局直写 `["w1","w2",…]`，跳过 `ObJsonArray` 中间树，
  要求与现状 `get_raw_binary` 输出**逐字节一致**。

## A3. 硬约束

1. **`TOKENIZE(...)` 返回字符串逐字节不变，含 token 顺序**；
   当前顺序是 `ObFTWordMap` **哈希桶遍历顺序**（证据：`tokenize_edge.result` 里
   `tokenize('数据库 the 索引 a','ik')` → `["a","索引","数据库","the"]`）；
2. 分词语义不变；`build_*` 不得退化。

## A4. 验收门

- `tokenize_ik` ≤ **0.055 ms**（8 轮配对中位），`tokenize_beng` 不退化；
- `tokenize_edge`（14 组）PASS + 36 形态 dump 的 TOKENIZE 输出逐字节一致；
- 单步 <3% 就回退。

---

# 子任务 B：⑥ 两张映射表合并构建（目标 build 类 +5 个百分点）

## B0. 依据

一次 FTS 索引构建有 4 张 aux 表，其中 **2 张是纯映射表**：

| 表 | 内容 | 当前构建方式 |
|---|---|---|
| `rowkey_doc`（ROWKEY_DOC_ID） | rowkey → docid | `prepare_rowkey_doc_table()` 单独提交一个子任务 |
| `doc_rowkey`（DOC_ID_ROWKEY） | docid → rowkey | 在 `prepare_aux_index_tables()` 里与另外两张一起提交 |

两者是同一份 `(rowkey, docid)` 数据的正反方向，各自对源表做一次扫描 + 排序 + 直载写入，
并各占一个 DDL 子任务的固定开销（建 tablet / 等待 / 收尾）。
平台 build 受总工作量限制（build 是本机 1.8 倍，而 CPU 型指标同速）。

## B1. 实施

1. 复用已有双 writer 基础设施：`_enable_fts_token_share` 那条路径
   （`src/sql/das/ob_das_domain_utils.cpp` 的 dual 行生成 + PX 写算子 + DDL 编排），
   把"一次扫描写两张表"的手法用到**两张映射表**上（注意：映射表不涉及分词，只是列序/方向不同）；
2. 关键点：两张表的排序键不同（`rowkey_doc` 按 rowkey 排，`doc_rowkey` 按 docid 排），
   必须**各自独立 sorter**，不能共享；
3. 两表同进同退：任一失败整任务失败；保留独立构建路径作为回退开关。

## B2. 验收门

- build 三项各降 **≥4%**（3 块交替取中位）；
- **`SELECT id` 的映射正确性必须单独验证**（它依赖这两张表）：
  对同一批数据，`SELECT id FROM t WHERE MATCH(...)` 的结果集合在改动前后一致；
- 4 个仓库用例 + DDL 专项 14 项全过；命中数不变。

---

# 子任务 C：⑧ TOKENIZE 的语句层 8µs（目标 tokenize 类 +4 个百分点）

## C0. 依据

服务端 34µs 里只有 25.7µs 落在三个已测相位，**约 8µs 在语句层**。

## C1. 定位方法（先量再改）

用同一实例跑对照，逐个排除：

| 对照 | 目的 |
|---|---|
| `SELECT 1` | 纯往返 + 最小语句开销（≈0.0254 ms） |
| `SELECT tokenize('', 'ik')` | 空文本 ⇒ 分词开销≈0，剩下的是语句层 + JSON 输出 |
| `SELECT repeat('x', 300)` | 与 tokenize 输出体量相近的字符串 ⇒ 隔离结果序列化成本 |
| `SELECT tokenize('<80 字>', 'ik')` | 完整路径 |

把四者相减，定位 8µs 落在：结果序列化 / 协议写回 / 表达式求值框架 / `TempAllocGuard` 首次分配。

## C2. 实施（按定位结果）

- 若在**结果序列化**：与子任务 A 的 JSON 直写合并，一次产出二进制 buffer 直接交给协议层；
- 若在 **`TempAllocGuard`/分配**：改为复用（同一表达式实例内复用一块临时内存）；
- 若在**框架**：检查是否有可省的 per-call 构造（如 `TokenizeParam` 的重复构造、字符串参数解析）。

## C3. 验收门

- `tokenize_ik` 再降 **≥5%**（8 轮配对中位）；
- `tokenize_edge` + dump 输出逐字节一致；`build_*` 不退化。

---

# 子任务 D：④ 双 writer 为何为负（排查，目标：修好 +13 或明确删除）

## D0. 现状

- 代码已实现，由 `_enable_fts_token_share` 控制，**默认 False**；
- 粗测（2 次/臂）：off 2.449 / 2.858 s、on 2.439 / 2.048 s（看起来 on 更好）；
- 严格 4 轮交替（同一实例、逐对交替）：off 中位 **3.913 s**、on 中位 **4.191 s** ⇒ **on 慢 7.1%**；
- 同轮绝对值在 2.45~4.52 s 之间波动（宿主负载），**样本不足以定论**。

## D1. 排查步骤

1. **先确认开关真的生效**：加一条 `LOG_INFO`（`OB_FTS_PROF` 门控）打印
   "share path engaged, docs=..., shared_tokens=..., fallback=..."，
   验证 on 时确实走了共享路径、没有静默回退；
2. **判断是否牺牲了并行度**：原来是两张表**各自一个扫描线程**并行分词；
   合并后是**一个线程分词 + 两个 writer**。在 2~4 vCPU 的租户上这是净损失。
   验证方法：对比 on/off 时的 `CPU/wall`（`/proc/<pid>/stat` 的 utime+stime 除以墙钟）
   与 PX worker 数量；
3. **检查额外拷贝**：共享路径是否把同一份 token 复制进了两组 `ObDatumRow`（应当共享字符串内存）；
4. **检查内存上限参数**：`_fts_token_share_mem_limit_mb` 是否导致更频繁 flush/spill；
5. **加大样本**：修好后用 **8 对交替**（同一实例）复测，并同时记录 `select1` 作为机器状态对照。

## D2. 结论要求（二选一）

- **修好**：on 相对 off 降 **≥5%**（8 对交替中位）⇒ 保留并考虑默认开启；
- **修不好**：把共享路径的代码**删除**（不要留默认关闭的死代码），并在报告里写明原因，
  同时把 `_enable_fts_token_share` / `_fts_token_share_mem_limit_mb` 两个参数一并移除。

---

# 全局纪律（每一步都要遵守）

1. **构建**：`TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12`
   （不带 TMPDIR 时 Rust 组件会因沙箱只读临时目录而编译失败）；
2. **磁盘**：实例启动要求可用 >10GB，低于阈值报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)`；
3. **不要 `pkill -f seekdb`**（会杀掉沙箱包装进程）；停实例一律 `sdb.py stop --base-dir <dir>`；
4. **不要 `git checkout/stash`**（`.git` 只读）；对比改动请 `cp` 备份文件；
5. **测量**：同机两二进制交替、8 轮配对取中位；本机 `select1` 在 0.0204~0.0358 之间漂移，
   宿主负载还会让 `build_*` 单次在 2.4~4.5 s 乱跳 —— **禁止**用单次跑 vs 固定基线判定；
6. **正确性四关**（每步都过）：36 形态 correctness dump（仅 `order_rel` 允许差异）、
   4 个仓库用例（`no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback`）、
   DDL 专项 14 项、benchmark 命中数（8001/11000/7332/20）；
7. **单步收益 < 门槛就回退**，不要叠加复杂度。

# 交付要求

报告按子任务 A~D 分节，每节写清：
1. 做了什么、改了哪些文件、为什么这么选（要有测量依据）；
2. 每一步的 8 轮配对数据（中位 + stderr / MAD）；
3. 被回退的尝试及原因；
4. 正确性证据；
5. 未完成部分与原因；本地数字与平台分数的区分。
