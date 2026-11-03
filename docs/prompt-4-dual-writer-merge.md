# 提示词 ④：构建双 writer 合并（一次扫描 + 一次分词喂两张 aux 表）

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
> 设计细节见 `docs/fts-dual-writer-design.md`，本提示词给出**修正后的落地顺序**。

## 0. 目标与价值

平台口径：`build_ik_all` 5.55s / `build_ik_content` 4.224s / `build_beng_en` 2.422s
（基线 8.78 / 6.68 / 3.83，各类 +36.8%）。build 类占综合 1/3 权重（3 项均权）。

**目标**：三个 build 指标相对现状再降 **≥12%** ⇒ build 类均值 +13 个百分点。

### 为什么这个方向在平台上成立（与本地不同，务必理解）
- 平台 build 是本机的 **1.8 倍**，但 tokenize/query 与本机**同速**
  ⇒ 平台构建受**总工作量/存储**限制（核数更少），不是并行度不足；
- 一次 FTS 索引构建有 **4 张 aux 表**：

  | aux 表 | 内容 | 是否分词 |
  |---|---|---|
  | `rowkey_doc` | rowkey ↔ docid 映射 | 否 |
  | `doc_rowkey` | docid ↔ rowkey 映射 | 否 |
  | `fts_index` | `[word, docid, word_count, doc_len]` 倒排 | **是** |
  | `fts_doc_word` | `[docid, word, word_count, doc_len]` 正排 | **是** |

- `fts_index` 与 `fts_doc_word` 由两个独立 DDL 子任务各自 `INSERT ... SELECT` 构建，
  **同一份文本被扫描两次、LOB 读两次、IK 分词两次**。
  分词约占构建的 14%（本地实测 0.8s / 5.55s 平台），**省掉一次就直接省 CPU**，在平台上等价于省墙钟。

> 注意：早期设计曾把收益估成 −25%，偏高。按"分词占构建 14%"重新核算，本项合理预期是 **−12~15%**。

## 1. 不可变约束

1. 两张表的 **schema、列顺序、PK、排序要求**都不变；
2. 源扫描语义、快照、事务可见性、PX worker 隔离不变；
3. 不得为省时间跳过 LOB 读取、减少文档或 token、降低 flush；
4. 两张 aux 表必须**同进同退**（任一失败整任务失败，走既有 DDL 重试路径）；
5. 提供配置开关 `_enable_fts_dual_writer`：**默认关**时必须完全等于现状。

## 2. 实施（三步，每步独立验证）

### 第 1 步：一次分词产出两组行
- 位置：`src/sql/das/ob_das_domain_utils.cpp` 的 `generate_fulltext_word_rows()`。
  它已按 `is_fts_index_aux` 决定列顺序（`word_idx = is_fts_index_aux ? 0 : 1`）。
- 新增 `generate_fulltext_word_rows_dual(...)`：
  - 一次 `segment()` 得到 `ObFTWordMap`；
  - **同时**填充两组 `ObDatumRow`：A=`[word, docid, cnt, len]`、B=`[docid, word, cnt, len]`；
  - 两组行共享同一份 word 字符串内存（同一 allocator 生命周期内），避免二次拷贝；
  - 保留现有单表版本供回退。
- 验收：用现有 `correctness_dump`（36 形态）证明两组行的**内容**与单表路径逐字节一致
  （可写一个临时单测函数把两组行 dump 出来比较，验完删除）。

### 第 2 步：一个 PX 直载算子持有两个 writer
- 当前位置与逐行路径：`src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp`
  `ObPxMultiPartSSTableInsertOp::write_heap_slice_by_row()`：
  `get_next_row_from_child` → `eval_current_row` → `get_tablet_info_from_row` →
  `get_or_create_heap_writer` → `slice_writer->append_row`。
- 改为"行对"接口：child 每次返回 **A 行 + B 行**，算子按行对分别 append 到两个 writer：
  - `ObPxSSTableInsertOpSpec` 增加第二组输出 expr 与目标 tablet id 槽位；
  - 两个 writer 各用自己的 sorter/buffer（**不要共享**，否则排序键会串）；
  - 顺手缓存上一次的 `(tablet_id, writer)` 指针，省掉每行哈希查找
    ——注意：实测该项单独只有 **+0.08%**，**不要把它当收益来源**，只是顺手。
- 验收：先用一个可控 SQL（同时对两张临时表 `INSERT ... SELECT`）验证行数与内容一致。

### 第 3 步：DDL 编排
- 位置：`src/rootserver/ddl_task/ob_fts_index_build_task.cpp`
  `prepare_aux_index_tables()` 会一次性提交 3 个子任务
  （`doc_rowkey`、`domain_index_aux`、`fts_doc_word`）；`dependent_task_result_map_` 容量为 4。
- 目标：由 `fts_index` 子任务带写 `fts_doc_word`，`fts_doc_word` 子任务不再独立构建
  （只保留建表与补数据登记，或直接不登记、由合并任务同时上报两张表的 completion）。
- 只**新增** DDL 状态，不改既有状态枚举含义（状态值持久化在任务记录里，改语义会影响恢复）。
- 验收：开关打开时一次 ALTER 成功建出 4 张表且索引可查；开关关闭时行为与现状一致。

## 3. 验证（每步都做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12   # 必须带 TMPDIR
df -h .                                                                    # 需 >10GB 可用
```

1. **索引内容等价**：36 形态 correctness dump 逐字节一致（唯一允许差异：`order_rel` 的并列组选取）；
2. 4 个仓库用例全过：
   `for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done`
3. **DDL 专项 14 项**：`TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`
   （建索引/分区表建索引/双索引并存/online DDL 加列/删索引/重建/`SELECT id` 映射）；
4. benchmark 命中数 `8001 / 11000 / 7332 / 20`；
5. **性能**：3 块交替 A/B 取中位（`TMPDIR=/tmp bash .tmpdev/ab_bins.sh <旧> <新> 3`），
   build 每项 ≥12% 才算达标；`build_ik_all` 偶发 9~16s 离群（约 1/5），
   出现离群时该块作废、重跑；
6. **健壮性**：kill -9 实例后重启，DDL 能正确恢复或失败重试；开关开/关两种模式都能构建成功。

## 4. 风险与回退

| 风险 | 缓解 |
|---|---|
| 两表数据不一致 | 合并任务内同进同退；失败整任务重试，两表一起重做 |
| 两个 sorter 混用 | 每个 writer 独立 sorter/buffer，不共享 |
| DDL 恢复语义被破坏 | 只新增状态，不改既有状态含义；保留 `fts_doc_word` 独立构建的代码路径作回退 |
| 并行度下降 | 合并后的 PX plan 内部仍需按 DOP 切分；必要时对两个 writer 分别设并行 |

**回退**：把 `_enable_fts_dual_writer` 置 0 即完全退回现状；提交前必须验证两种模式都能跑通。

## 5. 环境坑

1. `git checkout/stash` 失败（`.git` 只读），对比改动用 `cp` 备份再改回；
2. 不要 `pkill -f seekdb`（会杀沙箱包装进程）；停实例用 `sdb.py stop`；
3. 实例启动一律 `sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2881`；
4. 磁盘不足时服务端报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)` 拒绝启动；
5. 本机 `select1` 漂移 0.0204~0.0285（±15%），**禁止**用单次跑 vs 固定基线判定。

## 6. 交付要求

报告写清：
1. 三步各自的完成情况与改动文件；
2. 每步的验证证据（行内容一致性、DDL 14 项、命中数）；
3. 3 块交替 A/B 的 build 三项数据（中位 + 每块原始值，标注离群块）；
4. 开关开/关两种模式的结果；
5. 未完成部分与原因；本地数字与平台分数的区分；
6. 如果某一步实测收益 <5%，**停止该步**并说明原因，不要为小收益继续加复杂度。
