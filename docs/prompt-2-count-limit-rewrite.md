# 提示词 ②：`COUNT(*)` 套 `LIMIT` 子查询的代数改写

> 自包含提示词。你是唯一执行者，请完整读完再动手。
> 仓库：`/home/hahaha/ob-contest/seekdb-index-tuning`（分支 `indextuning`）
>
> ⚠️ 本项是提示词 ①（让 LIMIT 在 FTS 扫描里早停）的**兜底路线**：
> 只有当 ① 无法安全下发 limit 时才做本项。如果 ① 已达标（`query_limit ≤ 0.35ms`），
> **不要**再做本项 —— 语义改写风险高于"早停"。

## 0. 目标与价值

`query_limit_avg_ms` 基线 1.7526、当前 **1.9936（−13.8%，九项里唯一负项）**。
该查询形状是：

```sql
SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('<多词>') LIMIT 20) t;
```

**目标**：`query_limit_avg_ms` ≤ **0.95 ms**（相对基线 +46%），查询类均值约 +10 个百分点。

## 1. 数学依据（本项的核心）

对满足下面全部前提的计划，下列两式**恒等**：

```
SELECT COUNT(*) FROM (SELECT <任意非聚合列> FROM t WHERE <cond> LIMIT n) x
   ≡
SELECT LEAST(n, (SELECT COUNT(*) FROM t WHERE <cond>))
```

前提（少一条都不能改写）：
1. 内层是**单表来源**（无 JOIN / 无集合运算 / 无派生表嵌套）；
2. 内层无 `DISTINCT`、无 `GROUP BY`、无 `HAVING`、无聚合函数、无窗口函数；
3. 外层只有 `COUNT(*)`（或 `COUNT(常量)`），外层无 `WHERE` / `GROUP BY` / `HAVING` / 窗口；
4. 内层的 `LIMIT` 是常量或可求值表达式，且没有 `OFFSET`
   （有 OFFSET 时结论变为 `LEAST(MAX(n - offset, 0), count)`，本项**不支持 OFFSET**，直接放弃改写）；
5. 内层 `ORDER BY` 不影响结果（`COUNT(*)` 与外层无关），可以安全忽略。

## 2. 为什么改写后能变快（已实测）

改写后是普通 `COUNT(*)`，会命中已有的"分数只作谓词"gate
（`src/sql/optimizer/ob_log_plan.cpp` 中的 `score_free_shape` / `presence_only` 逻辑），
进而走**文档 ID 位图去重**路径（`ObTextDocIdBitmapIter`）：

| 查询 | 实测 |
|---|---|
| `COUNT(*)`（无 LIMIT，位图路径） | **0.87 ms** |
| `COUNT(*) FROM (SELECT id ... LIMIT 20)` | 1.57~1.99 ms |
| `SELECT id ... LIMIT 1`（多 token） | 1.74~2.85 ms |

## 3. 实施步骤

### 第 1 步：定位改写入口
- 候选位置：`src/sql/optimizer/` 下的变换规则（`ob_transform_*`），或
  `ob_log_plan` 在构造聚合算子时的入口；也可以放在 resolver/rewrite 阶段。
- 先读现有规则如何匹配"子查询 + 聚合"，沿用同一套 `ObTransformCtx` / `ObDMLStmt` 结构，
  **不要**新造一套遍历框架。

### 第 2 步：实现匹配与改写
- 匹配上面 §1 的 5 条前提（逐条写检查并加注释说明"为什么少一条不行"）；
- 把内层 `LIMIT n` 与外层 `COUNT(*)` 折叠为对原始表来源的 `COUNT(*)`，
  再包一层 `LEAST(n, count)` 表达式（`T_FUN_COUNT` + `ObExprOperator` 已有的 LEAST/IF 表达即可，
  必要时用 `CASE WHEN count > n THEN n ELSE count END`）；
- 改写后必须保证：列的元信息、`stmt` 的引用计数、`ObQueryCtx` 里的表来源仍然合法（沿用现有规则的做法）。

### 第 3 步：收敛影响面（强烈建议）
若不想引入通用规则，**只匹配下面这种形态**，把影响面限制在 FTS 上：
- 内层 WHERE 恰好只有一个条件，且该条件是 `T_FUN_MATCH_AGAINST` 或 `T_OP_BOOL(MATCH_AGAINST)`；
- 内层表上存在全文索引、`mode_flag_ == NATURAL_LANGUAGE_MODE`。
这样与现有 B1 gate 的条件完全对齐，风险最小。

## 4. 硬约束

1. 改写后结果必须与改写前**完全相等**（含 `n > 命中数` 与 `n < 命中数` 两种边界）；
2. 前提不满足时**必须不改写**（宁可不优化，不能改错）；
3. 不改 benchmark 数据/SQL/计时/评分/基线；不改 MATCH/BM25 语义；
4. 提供开关（如 `_enable_count_limit_rewrite`），默认开启，异常可一键关闭。

## 5. 必须新增的测试（本项最容易出错的地方）

在 `tools/deploy/mysql_test/test_suite/fts_index/t/` 新增用例（并记录 `.result`、注册进
`tools/deploy/mysql_test/psmalltest.py`）：

**正向**（应改写、结果正确）：
```sql
-- 命中数 > n：结果恒为 n
select count(*) from (select id from t where match(c) against('a b') limit 3) x;   -- 期望 3
-- 命中数 < n：结果等于命中数
select count(*) from (select id from t where match(c) against('a b') limit 100) x; -- 期望 命中数
-- n = 0
select count(*) from (select id from t where match(c) against('a b') limit 0) x;   -- 期望 0
```

**负向**（不得改写，且结果仍正确）：
```sql
-- 内层 DISTINCT
select count(*) from (select distinct id from t where match(c) against('a b') limit 3) x;
-- 内层 GROUP BY / 聚合
select count(*) from (select id, count(*) from t where match(c) against('a b') group by id limit 3) x;
-- 外层带 WHERE
select count(*) from (select id from t where match(c) against('a b') limit 3) x where x.id > 1;
-- 带 OFFSET
select count(*) from (select id from t where match(c) against('a b') limit 3 offset 2) x;
```
负向用例的期望值必须与**改写前的引擎**一致；如果不能确定，先记录当前引擎的输出作为期望。

## 6. 验证清单（每一步都做）

```bash
cd /home/hahaha/ob-contest/seekdb-index-tuning
TMPDIR=$PWD/.tmpdev/tmp cmake --build build_release --target seekdb -j12   # 必须带 TMPDIR
df -h .                                                                    # 需 >10GB 可用
```
1. 36 形态 correctness dump 逐字节一致（唯一允许差异：`order_rel`）；
2. 4 个仓库用例全过：
   `for t in no_score_match tokenize_edge bitmap_dedup bmw_topk_fallback; do MODE=verify TMPDIR=/tmp bash .tmpdev/gen_result.sh $t; done`
3. 新增用例正/负向全过；
4. DDL 专项 `TMPDIR=/tmp bash .tmpdev/probe_ddl_sanity.sh`（14 项，0 失败）；
5. benchmark 命中数 `8001 / 11000 / 7332 / 20`；
6. 性能：8 轮配对 A/B（`ab_pingpong.sh`，不要用单次跑 vs 固定基线）。

## 7. 环境坑

1. `git checkout/stash` 失败（`.git` 只读），对比改动用 `cp` 备份；
2. 不要 `pkill -f seekdb`（会杀了沙箱包装进程）；停实例用 `sdb.py stop`；
3. 实例启动一律 `sdb.py start --binary build_release/src/observer/seekdb --base-dir <dir> --port 2881`；
4. 磁盘低于阈值时服务端报 `OB_SERVER_OUTOF_DISK_SPACE(-4184)` 直接拒绝启动。

## 8. 交付要求

报告写清：
1. 改写规则匹配了哪些条件、为什么这些条件足够；
2. 改了哪些文件、开关名与默认值；
3. 正/负向用例的实测结果（贴 SQL 与输出）；
4. `query_limit_avg_ms` 的 8 轮配对数据（中位 + stderr）；
5. 是否触发了 B1 gate 与位图路径（可用 `EXPLAIN` 里 `calc_relevance=false` 佐证）；
6. 未完成部分与原因；本地数字与平台分数的区分。
