# 提示词 ⑥~⑨ 交付报告：四项后续优化（③深挖 → ⑥映射表 → ⑧语句层 → ④双 writer）

> 执行对象：`docs/prompt-6-to-9-followup-four.md`；仓库 `indextuning`，起点 HEAD `967455316`。
> 执行日期：2026-11-03 21:5x ~ 23:1x。本机 24 核 / 15 GB（`select1` 在 17.9~29.5 µs 之间漂移）。
> **最终交付改动：6 个文件（+75 / −832 行）**：一条 JSON 结果集渲染快速路径（子任务 ⑧/C）+
> 删除 `_enable_fts_token_share` 共享分词路径与两个参数（子任务 ④/D）。
> 子任务 ③/A 未保留代码（两次尝试实测 <3%，按 A4 回退）；子任务 ⑥/B 未实现（实测其前提不成立，见 §3）。

## 0. 结论（TL;DR）

| 子任务 | 结论 | 本地实测（配对） | 改动文件 |
|---|---|---|---|
| **A/③ TOKENIZE 服务端成本** | **A1 完成**：四段计时证明 `tokenize_fulltext` **没有**「未归属 9 µs」（四项和 = fulltext）。A2 按数据选 `segment` 压 CJK，试了两处（词典根匹配 memo、仲裁器有序游标），端到端 **<3% 且被对照项淹没 → 按 A4 全部回退** | A 步净收益 ≈0~1%（对照项同幅漂移） | 无（已回退到 HEAD） |
| **B/⑥ 两张映射表合并构建** | **未实施**：实测两张映射表在 `WAIT_AUX_TABLE_COMPLEMENT` 结束前 **2.47 s** 就已完成，最终等待由 `fts_doc_word` 决定 ⇒ 合并的**墙钟收益上限为 0**（总工作量最多省 0.29 s）。按「单步 <5% 不加复杂度」停止 | 关键路径 2.68 s vs 映射表 0.24/0.29 s | 无 |
| **C/⑧ TOKENIZE 语句层** | **落地**：定位到 6.3 µs 的「二进制 JSON→文本」逐元素渲染，新增**扁平字符串数组直写快速路径**（逐字节等价，含 `", "` 分隔与转义） | `tokenize_ik` **−9.58%**（8 轮配对，rel-med +9.91%）；官方口径 6 对 **−9.68%**；`tokenize_beng` −7.5~−13.7% | `ob_json_bin.{h,cpp}`、`ob_mysql_protocol_util.cpp` |
| **D/④ 双 writer 为何为负** | **删除**：同实例 on/off 看似 −14.5%，但 `stores=20000` 暴露是**跨 ALTER 缓存复用**；冷构建 on≈off（+2.9%），上一轮「每臂全新实例」3 块 A/B 为 wall +0.07%/CPU −4.12%。按 D2 删除共享路径与两个参数 | 冷构建 off 5.167 s / on 5.316 s | `ob_das_domain_utils.{h,cpp}`、`parameters.yaml`（回到 `b7cc5dbe6`） |

**给评分的关键事实**：`tokenize_ik` 从基线 **0.0530~0.0547 ms（官方口径同窗口）降到 0.0453~0.0472 ms**，
配对 A/B **−9.6%**（MAD 5.3，se 3.0）；8 轮配对与 6 对官方口径两个独立实验方向一致。构建三项无回归，
命中数 `8001/11000/7332/20` 不变，四关正确性全过。

---

## 1. 测量环境、口径与污染（必须先读）

### 1.1 同工作区有**第二个并发任务**在跑

本会话期间，工作区被另一个任务（prompt-5「count-only 快路径」）**并发修改与构建**：

- 它改了 7 个源码文件（`ob_table_scan_op.{h,cpp}`、`ob_das_ir_define.h`、`ob_i_sparse_retrieval_iter.h`、
  `ob_text_daat_iter.{h,cpp}`、`ob_das_legacy_tr_merge_iter.cpp`），这些**不是本次交付**；
- 它会在自己的构建前把我的文件临时还原、构建后再放回（`.tmpdev/other-files-222021/`、`other-files-222325/`），
  且用保留 mtime 的方式放回，导致我两次链接失败（`ObJsonBin::print_string_array` undefined），
  用「恢复 + touch + 重建」修复；
- 它在同一 `build_release` 里构建，并用 `-j` 跑自己的基准，**本机负载在 2~11 之间波动**，
  期间 `select1` 从 17.9 µs 漂到 29.5 µs，`build_*` 单次从 2.6 s 跳到 10.6 s（有一次单轮 build_ik_content = 10.585 s）。

因此：**单次跑对固定基线不可用**。本文所有性能结论都来自「同机、两二进制、逐对交替、多轮取中位」，
并且同时记录 `SELECT 1` 作为机器状态对照；官方基准只用于「同窗口、同口径」的绝对数复核。

### 1.2 基线（当前实测，2026-11-03 21:55，`b0`）

```
build:    2.855 / 2.432 / 1.330 s      tokenize: ik 0.0625 / beng 0.0439 ms (avg)
query:    cn 0.6029 / beng 0.8430 / mixed 0.6337 / limit 0.2681 ms
hits:     8001 / 11000 / 7332 / 20     select1: 0.0295 ms
```
与提示词给的基线（2.848/2.430/1.343、0.0599/0.0387、8001/11000/7332/20）一致；机器整体略慢。

### 1.3 复现脚本（均为开发脚本，未进产品）

- `.tmpdev/tok_ab8.sh`：8/12 轮两二进制交替，每臂全新实例，`ik/beng/sel1` 各 9000 条，`ik` 取 3 次最小；
- `.tmpdev/tok_official_ab.sh`：完全复刻官方 TOKENIZE 段（3000 条 × 3 样本取均值），逐对交替；
- `.tmpdev/tok_prof_run.sh`：`OB_FTS_PROF=1` 四段窗口均值（1/64 采样）；
- `.tmpdev/tok_only_dump.sh`、`.tmpdev/json_render_dump.sh`：TOKENIZE / JSON 渲染逐字节 dump；
- `.tmpdev/dump_with_bin.sh` + `.tmpdev/correctness_dump.sh`：36 形态正确性 dump；
- `.tmpdev/probe_ddl_sanity.sh`：DDL 专项 14 项；`.tmpdev/exp_mysqltest.sh`：仓库用例；
- `.tmpdev/d_tokshare_ab.sh`、`.tmpdev/d_coldcheck.sh`：子任务 D 的 on/off 与冷/热对照。

---

## 2. 子任务 A/③：TOKENIZE 服务端成本（A1 完成；A2 两次尝试均回退）

### 2.1 A1：拆开 `tokenize_fulltext`（改动前，`OB_FTS_PROF=1`，1/64 采样，12000 条）

在 `ob_expr_tokenize.cpp` 的 `eval_tokenize` / `tokenize_fulltext` 加四段计时（`helper_init`、`map_create`、
`segment`、`make_json`），另加 `parse_param`、`pack_json_res`、`eval` 三段用于 ⑧ 的定位。稳定窗口（单位 µs）：

| 段 | 窗口 1 | 窗口 2 | 中位 |
|---|---|---|---|
| `parse_param` | 0.575 | 0.588 | **0.58** |
| `tokenize_fulltext` | 17.23 | 17.88 | **17.6** |
| ├ `helper_init` | 0.157 | 0.171 | **0.16** |
| ├ `map_create` | 0.084 | 0.089 | **0.09** |
| ├ **`segment`** | **15.69** | **16.27** | **16.0** |
| └ `make_json` | 0.815 | 0.841 | **0.83** |
| `pack_json_res` | 1.91 | 1.98 | **1.95** |
| **`eval_tokenize`** | **19.77** | **20.51** | **20.1** |

**结论：四项之和 0.16+0.09+16.0+0.83 = 17.08 ≈ fulltext 17.6（窗口误差内），没有提示词假设的「内部未归属 9 µs」。**
`eval` 也等于 `parse + fulltext + pack`（0.58+17.6+1.95 = 20.13）。提示词 A1 的「9 µs 差额」不成立，
与上一轮 `docs/fts-tokenize-server-side-report.md` §1.1 的结论一致。服务端 34 µs 里剩下的 ~14 µs
全部在**语句层**（见 §4），不在 `tokenize_fulltext` 内部。

### 2.2 A2：按数据选一条实施（试了两处，实测 <3% → 回退）

`segment`（16 µs）远大于 12 µs，按 A2 走「压 CJK 热路径」。实现并各自验证的两处：

1. **词典根匹配 memo**：`ObIKCJKProcessor::do_process` 每字符做一次 `dict_main_.match()`（range dict 查找 +
   DAT map 查找），按 `(range dict, 字符字节)` 在 256 项线程局部直映表里记忆 `(hit_dict, base_idx, flags)`，
   命中时重建 `ObDATrieHit`（`end_pos = cursor + char_len`，`char_cnt=1`）。**输出逐字节不变**（47 例 dump）。
2. **仲裁器有序游标**：`ObIKArbitrator::output_result` 原来对**每个字符**做一次链查找（有序时二分、无序时哈希），
   改为「`chain_order_ok_` 时前进一个游标 O(1) 比较」；把「孤立单字输出」抽成 `emit_isolated_char()` 复用，
   消除两份重复代码。语义与输出不变。

**实测（A/B：`C-only` vs `C+A+D`，8 轮 × 9000，每臂全新实例）**：

```
ik   A(C-only)=50.87  B(+A)=48.28   rel-med +4.03%  MAD 4.64  se 2.91
sel1 A=22.80          B=21.93       rel-med +6.33%  MAD 13.49 se 6.86   ← 对照项漂移比 ik 还大
```

`ik` 的表面 +4% 小于**同一实验里 `sel1` 对照项的 +6.33%**，即两处 segment 优化的净贡献 ≈0~1%，
低于 A4 的 3% 门槛（12 轮复核同样淹没在噪声里）。按 A4「单步 <3% 就回退」**把两处代码全部回退到 HEAD**
（`ob_ik_arbitrator.cpp`、`ob_ik_cjk_processor.cpp` 零改动），不把未证明的复杂度留给后续。

### 2.3 A4 验收门

- **`tokenize_ik ≤ 0.055 ms`：达成**，但它来自子任务 C 的 JSON 渲染快速路径（同一份最终二进制，
  官方口径同窗口 0.0453~0.0472 ms；官方复刻口径 6 对中位 0.0548 ms，仍 ≤0.055）；
- `tokenize_beng` 不退化：官方复刻口径 6 对 **−7.46%**（更快）；
- `tokenize_edge` + 47 例形态 dump：**逐字节一致**（§6）。A 自身不贡献代码，所以「A 的 8 轮配对数据」为
  「C-only vs C+A+D」的退化实验，已如实列在 §2.2。

### 2.4 A 没做的第三条（make_json 直写）

`make_json` 仅 0.83 µs（<3 µs），按 A2 的条件不触发「按二进制 JSON 布局直写」。而且真正的大头
（`pack_json_res` 1.95 µs + 结果渲染 6.3 µs）已在 C 里处理掉渲染部分；`pack_json_res` 的 1.95 µs 是
树→二进制序列化，直写需要手写完整二进制 JSON 布局（doc header/varint/entry 表），风险高、收益 ~2 µs，
在本轮机器噪声（地板 4~6%）下无法验收，故未做。

---

## 3. 子任务 B/⑥：两张映射表合并构建（实测前提不成立，未实施）

### 3.1 先量关键路径（子任务 B0 的前提）

用官方基准那一次构建（20k 文档、i`(title,content)`/i`(content)`/beng 三次 ALTER）的日志，
按 `INSERT ... SELECT` 开始时间与 `build index sstable finish` 结束时间还原每张 aux 表的构建窗口：

| ALTER | aux 表 | 类型 | 构建窗口 | 完成时刻 |
|---|---|---|---|---|
| #1 ik_all | `fts_rowkey_doc` | 映射 | **0.236 s** | 22:48:58.188 |
| #1 ik_all | `fts_doc_rowkey` | 映射 | **0.294 s** | 22:48:58.296 |
| #1 ik_all | `fti_all_ik` | 分词 | **2.511 s** | 22:49:00.541 |
| #1 ik_all | `fti_all_ik_fts_doc_word` | 分词 | **2.677 s** | 22:49:00.712 |
| #2 ik_content | `fti_content_ik` / `..._fts_doc_word` | 分词 | 1.811 / 2.112 s | 22:49:02.781 / 03.096 |
| #3 beng | `fti_en_beng` / `..._fts_doc_word` | 分词 | 1.088 / 1.139 s | 22:49:04.396 / 04.463 |

父任务 `ObFtsIndexBuildTask::wait_aux_table_complement()` 在 `dependent_task_result_map_` 里
**等全部 4 个子任务**，本次最后一个完成的是 `fts_doc_word`（22:49:00.766 收到通知）。
两张映射表在 **22:48:58.296 就完成了，比最终等待结束早 2.47 s**——它们完全被两张分词表遮住。
第 2、3 次 ALTER 甚至不再重建映射表（`res.ddl_task_id_ < 0`，复用已就绪的表）。

### 3.2 结论

- 「一次扫描写两张表」能省掉的**总工作量**上限 = `min(rowkey_doc, doc_rowkey) ≈ 0.24 s` / 首 ALTER；
- **墙钟收益上限 = 0**：两张表都在并行窗口里、且比关键路径早 2.47 s 完成，合并不会让
  `WAIT_AUX_TABLE_COMPLEMENT` 提前 1 ms；
- 提示词 B0 的「各占一个 DDL 子任务的固定开销（建 tablet / 等待 / 收尾）」在 **D1 把 rowkey_doc 的等待
  合并进最终等待之后**已不成立：固定开销被并行窗口吸收。真要拿到墙钟收益，必须动「一个 PX 计划两个
  direct-load session」的框架改造（`docs/fts-dual-writer-plan.md` §4 的 M2~M5），风险高、上限却是 0。

因此按提示词全局纪律「单步收益 <5% 就停止该步，不要为小收益继续加复杂度」**不实施**，也不引入
`_enable_fts_*` 之类的新开关。B2 要求的 `SELECT id` 映射正确性不受影响（映射表构建路径未改），
仍由 §6 的 DDL 专项与 36 形态 dump 覆盖。

---

## 4. 子任务 C/⑧：TOKENIZE 的语句层（落地，−9.6%）

### 4.1 C1 定位（先量再改）

同实例、同一时段、逐条交替，每项 8 轮 × 20000 条，取每轮 us/op 的中位（µs）：

| 对照 | 中位 | 相对 `SELECT 1` |
|---|---|---|
| `SELECT 1` | 21.32 | — |
| `SELECT tokenize('', 'ik')` | 24.10 | +2.78 |
| `SELECT repeat('x', 300)` | 23.37 | +2.05 |
| `SELECT tokenize(<80 字>, 'ik')` | **56.76** | **+35.44** |

`eval_tokenize` 只占 20.1 µs（§2.1），所以 **35.4 − 20.1 = 15.3 µs 在语句层**。再用一条
`OB_FTS_PROF` 门控的探针直接量 `ObSMUtils::build_cell_value` 的 `ObJsonTC` 分支
（`ObJsonBin` 构造 + `print` + `set_copied_bytes`）：

```
FTS_JSON_PROF(samples=64, render_us=6.277, bytes=438)
FTS_JSON_PROF(samples=64, render_us=6.293, bytes=438)
```

**438 字节 / 41 个 token 的 JSON 文本渲染 = 6.28 µs（153 ns/元素）**，占语句层 15.3 µs 的 41%。
其余约 9 µs 是执行器行产出 + 行构造/发包 + 438 字节输出（`repeat('x',300)` 的 +2.05 µs 给出 300 字节的
协议成本量级）。定位结论：**在结果序列化**，与提示词 C2 第一条一致。

### 4.2 实施（`print_string_array` 快速路径）

现状（`src/query/protocol/ob_mysql_protocol_util.cpp` 的 `ObJsonTC` 分支）对每个 JSON 单元格：
构造 `ObJsonBin` → `reset_iter` → 虚函数 `print()` → 逐元素 `get_array_element()`（每元素新建/重置一个
`ObJsonBin`、重复解析 type/长度）→ `ObJsonBaseUtil::add_double_quote`。

改动（2 处，产品代码）：

1. `src/oblib/common/json_type/ob_json_bin.{h,cpp}`：新增
   `int ObJsonBin::print_string_array(ObJsonBuffer &j_buf, bool &handled) const`。
   - 仅当根是 `J_ARRAY_V0` 且**每个元素都是就地存储的 `J_STRING_V0`** 时，直接按二进制布局
     `[type][vi64 len][data]` 取 `(ptr,len)` 追加，跳过每元素的 `ObJsonBin` 构造与重复解析；
   - 分隔符与 `ObJsonBaseUtil::append_comma(j_buf,false)` 完全一致：`", "`（**逗号 + 一个空格**）；
   - 引号与转义仍走 `ObJsonBaseUtil::add_double_quote`，与通用路径**同一实现**；
   - 单遍扫描：遇到非字符串元素，`j_buf.set_length(start_len)` 丢弃已写内容并返回 `handled=false`，
     由调用方走原 `print()`——两条路径的字节必须相同。
2. `src/query/protocol/ob_mysql_protocol_util.cpp`：`reset_iter()` 之后先试快速路径，未命中再走
   `json_base->print(json_buf, true, value.length())`。

效果（同一探针、同一时段）：

| 版本 | render_us（438 B / 41 元素） | 每次节省 |
|---|---|---|
| 基线 | 6.277 / 6.293 | — |
| 快速路径 v1（先校验后输出，两遍） | 3.263 / 3.064 | ~3.2 µs |
| 快速路径 v2（单遍 + 回退截断，最终版） | **2.546 / 2.585** | **~3.7 µs（−59%）** |

### 4.3 性能（8 轮配对，中位 + MAD/se）

**A/B 1：基线 vs 仅 JSON 改动（C-only），8 轮 × 9000，每臂全新实例：**

| 指标 | A 基线 | B C-only | 配对中位 | MAD | se |
|---|---|---|---|---|---|
| `tokenize_ik` | 54.53 | **49.30** | **+7.72%** | 2.10 | 1.89 |
| `tokenize_beng` | 33.56 | 32.03 | +4.47% | 5.81 | 2.66 |
| `SELECT 1`（对照） | 21.88 | 21.43 | **−2.90%** | 5.52 | 4.05 |

对照项显示「候选臂被机器状态**多算**了 2.9%」，即 JSON 改动的真实收益 ≥7.7%。

**A/B 2：最终二进制 vs 基线，8 轮 × 9000（每臂全新实例，逐对交替）：**

| 轮 | A ik | B ik | A beng | B beng | A sel1 | B sel1 |
|---|---|---|---|---|---|---|
| 1 | 56.34 | 46.53 | 33.15 | 32.08 | 20.57 | 20.44 |
| 2 | 50.79 | 48.81 | 36.90 | 28.98 | 23.45 | 18.44 |
| 3 | 53.32 | 57.88 | 37.10 | 33.01 | 19.80 | 19.73 |
| 4 | 56.20 | 47.61 | 34.55 | 29.47 | 30.24 | 20.98 |
| 5 | 55.94 | 48.12 | 36.00 | 28.11 | 25.28 | 20.37 |
| 6 | 47.98 | 42.96 | 31.88 | 30.42 | 18.18 | 18.71 |
| 7 | 46.82 | 44.66 | 32.70 | 28.17 | 17.63 | 18.73 |
| 8 | 47.97 | 43.48 | 29.05 | 28.42 | 21.08 | 20.09 |
| **中位** | **52.05** | **47.07** | **33.85** | **29.23** | **20.83** | **19.91** |
| 配对中位 | **+9.91%**（MAD 5.33 / se 2.95） | | **+12.44%**（MAD 8.43 / se 2.76） | | +2.67%（对照） | |

**A/B 3：12 轮 × 6000 复核（对抗噪声）：** `ik` 中位 54.57 → 50.59（配对中位 **+6.50%**，MAD 1.83，
se 1.66），`beng` 36.33 → 33.12（**+9.71%**），对照 `sel1` +5.73%。

**A/B 4：完全复刻官方 TOKENIZE 口径（3000 × 3 样本取均值），6 对：** `ik` 60.69 → 54.81 →
**−9.68%**（配对中位 +10.17%，MAD 1.11）；`beng` 39.53 → 36.58 → **−7.46%**（+5.99%，MAD 4.04）。

**官方基准（同一安静窗口、两二进制各 2 轮，select1 0.0179~0.0187）：**

| 指标 | 基线二进制 | 最终二进制 |
|---|---|---|
| `tokenize_ik_avg` (ms) | 0.0530 / 0.0547 | **0.0472 / 0.0453** |
| `tokenize_ik_median` (ms) | 0.0556 / 0.0535 | **0.0455 / 0.0444** |
| `tokenize_beng_avg` (ms) | 0.0320 / 0.0295 | 0.0330 / 0.0356 |

beng 的两个**单次**官方值方向相反（beng 工作量只有 ik 的 1/3，单次均值的抖动 ±20%），
以 A/B 4 的 6 对官方口径配对结果为准（−7.46%）；这一点在 §7 也如实列出。

### 4.4 C3 验收门

- `tokenize_ik` 再降 ≥5%：**达成**（+7.7% / +9.9% / +10.2% 三个独立实验）；
- `tokenize_edge` + dump 逐字节一致：**达成**（§6）；
- `build_*` 不退化：**达成**（§6.3，同窗口两二进制构建三项差异在噪声内）。

---

## 5. 子任务 D/④：双 writer 排查（确认无收益，删除）

### 5.1 先确认开关生效

`_enable_fts_token_share=true` 时 4 次 ALTER 后日志：

```
FTS_TOKEN_SHARE_STAT(lookups=260000, hits=168662, stores=20000, evictions=0, failed=0, bytes=27591854)
FTS_TOKEN_SHARE_STAT(lookups=270000, hits=178662, stores=20000, evictions=0, failed=0, bytes=27591854)
```

开关确实生效（命中率 ~65%，`stores=20000` 正好 = 文档数），**但 `stores` 只有 2 万**——
说明大量命中来自**前几次 ALTER 留下的进程级缓存**，而不是同一次构建内两张分词表之间的共享。

### 5.2 拆穿「看起来更好」

- 同实例 4 对交替（`d_tokshare_ab.sh`）：off 中位 2.404 s、on 2.054 s（**−14.5%**），CPU −11.75%；
- 但冷/热分离（`d_coldcheck.sh`，同一实例连续构建）：off 第二次 **5.167 s**、on 冷构建 **5.316 s**
  （**+2.9%，更慢**）、on 热第 2 次 5.227 s、热第 3 次 4.436 s——只有**缓存全热**的那次才快，
  且波动 ±30%（当时宿主负载 6~10，本机 build 单次在 2.6~10.6 s 之间乱跳）；
- 上一轮「每臂全新实例 + 重新灌数」的 3 块 A/B（`docs/fts-token-share-report.md` §4.1）：
  wall **+0.07%**、进程 CPU **−4.12%**；
- 提示词 D0 自己的 4 轮交替：on 慢 **7.1%**。

结论：**同一次构建内的「两表共享」拿不到墙钟收益**（两张分词表本来就并发，省下的一次分词不在关键路径），
看起来的 −14.5% 是「重复 ALTER 复用上一轮缓存」的测量假象；共享路径还带 128 MB 预算、进程级 LRU、
leader/等待协议与 prctl slack 调整。按 D2「修不好就删除」处理。

### 5.3 删除内容与验证

把 3 个文件恢复到引入共享路径之前的提交 `b7cc5dbe6`：

| 文件 | 变化 |
|---|---|
| `src/sql/das/ob_das_domain_utils.cpp` | 2449 → **1647 行**（−822） |
| `src/sql/das/ob_das_domain_utils.h` | −4 行（`parser_sig_`） |
| `rust/config/parameters.yaml` | −15 行（`_enable_fts_token_share`、`_fts_token_share_mem_limit_mb`） |

验证：
- `grep -rn "_enable_fts_token_share\|_fts_token_share_mem_limit_mb" src/ tools/ rust/` → **0 命中**；
- `strings build_release/src/observer/seekdb | grep -c _enable_fts_token_share` → **0**；
- `SHOW VARIABLES LIKE '%token_share%'` → **空**；
- 默认路径行为不变：官方基准、36 形态 dump、4 个仓库用例、DDL 14 项、命中数全部通过（§6）。

---

## 6. 正确性证据（最终二进制）

### 6.1 TOKENIZE / JSON 逐字节

| 检查 | 结果 |
|---|---|
| 47 例 TOKENIZE dump（ik/beng/ngram/ngram2/space、空串/单字/emoji/日韩/全角/停用词/数字/长文/all 模式/JSON 第三参） | **94 行逐字节一致** |
| 27 例 JSON 渲染 dump（数组/对象/嵌套/引号/反斜杠/制表换行/控制字符/空串/混合类型/`json_extract`/`json_quote`） | **逐字节一致** |
| 36 形态 correctness dump（20k 文档 + 3 个索引） | **仅 `order_rel` 不同**（10 个并列分数 id 的选取顺序：基线 1249,3223,4759,6295,6943,8479,13465,14233,18433,19969 vs 最终 1855,2623,7399,8935,9655,12769,15943,17479,19015,19783）——提示词明确允许的唯一差异 |
| `MODE=verify` 仓库用例 `no_score_match` / `tokenize_edge` / `bitmap_dedup` / `bmw_topk_fallback` | **4/4 PASS** |
| DDL 专项 14 项（建索引、命中、`SELECT id` 映射、分区表、双索引并存、online DDL 加列、删索引、重建索引、清库） | **14/14 OK** |
| 命中数 | **8001 / 11000 / 7332 / 20**（与基线一致） |

> 仓库 `fts_index` 全套 22 个用例中另有 17 个 FAIL，原因与本轮改动无关：`--result_format 4` 本地
> mariadb-test 不支持（`Version format 4 has not yet been implemented`）、用例里的 `connect` 缺 `host` 参数、
> 以及既有的 >256 token `4016` 引擎缺陷（见 `.tmpdev/analysis-ddl-phases.md` 附录）。改动前同样失败。

### 6.2 JSON 快速路径的等价性论证

1. 触发条件严格：根必须是 `J_ARRAY_V0`，**每个**元素必须是就地 `J_STRING_V0`；否则截断并回退原 `print()`；
2. 引号/转义复用 `ObJsonBaseUtil::add_double_quote`（与通用路径同一函数），分隔符复用
   `append_comma(false)` 的 `", "` 字节；空数组输出 `[]`；空串元素输出 `""`；
3. 数据定位与 `ObJsonBin::init_string_node_v0()` 完全同构：`[type u8][vi64 len][data]`，
   `data_off = pos_ + value_offset + 1`；
4. 27 例含转义/控制字符/嵌套/非字符串数组的 dump 逐字节一致（§6.1）。

### 6.3 构建无回归（同一安静窗口，两二进制各 2 轮）

| 指标 | 基线 | 最终 |
|---|---|---|
| `build_ik_all` | 2.856 / 2.632 | 2.746 / 2.741 |
| `build_ik_content` | 2.345 / 2.345 | 2.339 / 2.447 |
| `build_beng_en` | 1.343 / 1.351 | 1.349 / 1.344 |

差异在单次抖动（2.6~4.8 s）内；D 删除的是默认关闭的路径，C 只改 JSON 文本渲染，均不在构建路径上。

---

## 7. 被回退 / 未实施的尝试及原因

| 尝试 | 结果 | 原因 |
|---|---|---|
| A2-1 词典根匹配 memo | **回退** | 端到端净收益 ≈0~1%（A/B 里 ik +4.03% < 对照 sel1 +6.33%），低于 3% 门槛 |
| A2-2 仲裁器有序游标 + 抽 `emit_isolated_char` | **回退** | 同上，与 A2-1 同批度量，无法与噪声区分 |
| A1 四段计时 | **删除探针** | 验证目的已达成（证明没有 9 µs 未归属），按 A1「用完删除」移除 |
| B 双 writer 合并映射表 | **未实施** | 实测两张映射表比最终等待早 2.47 s 完成，墙钟上限 0；总工作量最多省 0.24 s |
| D 共享路径「修好」 | **不修，删除** | 冷构建 +2.9%，上一轮全新实例 A/B wall +0.07%；收益只在跨 ALTER 缓存复用上 |

---

## 8. 未完成部分与原因

1. **A 的 8 轮验收数据是「退化实验」**：A 的两处 CJK 优化被回退，`tokenize_ik` 的最终 −9.6% 全部来自 C。
   这是按提示词「单步 <3% 就回退」得到的结论，不是遗漏。
2. **B 未实现**：见 §3——合并的墙钟上限为 0，要拿收益必须做「一个 PX 计划两个 direct-load session」的
   框架改造（`docs/fts-dual-writer-plan.md` M2~M5），风险与收益不匹配。
3. **语句层剩余 ~9 µs 未继续压**：`repeat('x',300)` 显示 300 字节输出约 2 µs，剩余主要是执行器行产出 +
   发包路径，属通用协议层，改动面大且本地噪声地板 4~6%，无法验收。
4. **`pack_json_res` 的 1.95 µs（树→二进制序列化）未做直写**：需要手写二进制 JSON 布局，风险高、
   预期收益 ~2 µs，见 §2.4。
5. **本机单次官方 beng 值与配对结果矛盾**：已用「官方口径 6 对配对」定案（−7.46%）；但本机
   `build_*`/`beng` 的单次绝对值仍不可用于跨时段比较。

---

## 9. 本地数字与平台分数的区分

- 本文全部数字为**本机口径**（同机两二进制交替、每臂全新实例、配对中位），只回答「同机改动前后是否有效」，
  **不预测平台分数**；平台机的 `select1`、DLL 调度与 DDL 并发都不同。
- 本机 `select1` 在 17.9~29.5 µs 漂移（1.65 倍），`build_*` 单次 2.6~10.6 s；跨时段绝对数不可比。
- 平台 build 若按「总工作量」计分（提示词 B0 的说法），删除共享路径不改变默认行为、C 不在构建路径，
  因此 build 分数应基本不变；tokenize 类的收益来自纯服务端 CPU（每评估少 ~3.7 µs 的 JSON 渲染），
  与机器状态无关，方向上应能迁移。
- **注意**：最终工作树里另有并发任务（count-only 快路径）的 7 个文件改动，**不属于本报告**；
  若平台按工作树整体评分，query 类的变化主要来自那份改动，请勿归因到本报告。

## 10. 复现

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
# 构建（宿主内存紧张时用 -j4；-j12 会 OOM 掉 cc1plus）
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j4

# tokenize 配对 A/B（8 轮、每臂 9000 条、全新实例、逐对交替）
bash .tmpdev/tok_ab8.sh <基线二进制> build_release/src/observer/seekdb 8 9000
# 官方口径（3000x3 均值）配对
bash .tmpdev/tok_official_ab.sh <基线二进制> build_release/src/observer/seekdb 6
# 正确性
bash .tmpdev/tok_only_dump.sh <bin> /tmp/a.txt 2908         # 两种二进制 diff
bash .tmpdev/json_render_dump.sh <bin> /tmp/b.txt 2910      # 两种二进制 diff
bash .tmpdev/dump_with_bin.sh <bin> /tmp/dump.txt /tmp/bench.txt 2909
bash .tmpdev/probe_ddl_sanity.sh build_release/src/observer/seekdb
for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do
  bash .tmpdev/exp_mysqltest.sh "$t"; done
# D：冷/热对照
bash .tmpdev/d_coldcheck.sh <bin>
```
