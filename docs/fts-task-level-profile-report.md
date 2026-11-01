# FTS Task-Level Profile Report

## 结论摘要

脚本级 FTS benchmark 已成功完成；任务级 tablet、子任务重叠和阶段计时仍未采集，因为这些数据必须在构建执行期间实时记录。

## 运行参数与环境

- 基准命令：`./tools/benchmark/fts_large_bench.sh`
- 默认参数：`ROWS=20000`、`BATCH=500`、`SKIP_LOAD=0`
- 其他默认参数：`ROUNDS=3000`、`QUERY_ROUNDS=200`、`SAMPLES=3`、`WARMUP=30`
- Git commit：`5bce9e06d1281347ad61344e4b01fab9158c58a2`
- 工作区：dirty，已有修改保留
- 主机：Linux x86_64，24 个逻辑 CPU
- 连接目标：`127.0.0.1:2881`，用户 `root`

## 执行结果

连接探测及基准前置连接收到：

```text
ERROR 2002 (HY000): Can't connect to server on '127.0.0.1' (115)
```

因此未执行建库、灌数、CREATE FULLTEXT INDEX、TOKENIZE 或 MATCH 测量，也没有可用的 parent task、child task、tablet、PX worker 或阶段计时数据。

## 后续连接恢复与脚本级基准

随后使用已构建的 `build_release/src/observer/seekdb` 在隔离目录
`/home/hahaha/seekdb-fts-profile` 启动实例，连接恢复正常：`SELECT 1` 返回 1，版本为
`5.7.25-OceanBase seekdb-v1.4.0.0`。完整 benchmark 已按上述参数执行成功，原始结果见
`docs/fts-task-level-profile-benchmark-local-20261101.txt`。

本次脚本级结果如下；这些是构建和查询路径的整体耗时，不能替代任务级时间线：

- `build_ik_all_sec=4.077`，`build_ik_content_sec=3.031`，`build_beng_en_sec=1.734`，`build_total_sec=8.859`
- `tokenize_ik_avg_ms=0.0770`，`tokenize_beng_avg_ms=0.0494`
- `query_cn_avg_ms=1.3201`，`query_beng_avg_ms=2.0124`，`query_mixed_avg_ms=1.5737`，`query_limit_avg_ms=1.8054`
- 命中数：中文 `8001`、英文 `11000`、混合 `7332`、LIMIT `20`

本次构建完成后查询 `oceanbase.__all_ddl_task_status` 为空，PX worker 统计没有可回溯的已结束 worker 记录，因此 tablet 行数、子任务重叠、实际 worker 利用率及阶段等待时间仍为 `NA`。要获取这些字段，必须在下一次构建期间实时采集或加入低开销的任务/tablet 汇总插桩。

## 指标与公式

任务级和 tablet 级指标均为 `NA`，原因是构建完成后没有可回溯的任务和 worker 记录。计划中的利用率公式为：

`active worker time / (configured parallelism * build wall time)`

本次没有实际采样周期、worker-active time 或 parallelism 运行时数据，不能计算 overlap、并行利用率、扫描/LOB/分词/写入耗时。

## 数据来源与缺口

- 连接错误来自第一次实例未启动时的 MySQL 客户端输出；随后隔离实例连接和完整 benchmark 均成功。
- 两个 CSV 文件使用表头并以 `NA` 表示未采集数据。
- 没有新增插桩代码，因此不存在插桩开销或精度影响。
- 要获取任务级字段，需要在下一次构建前启用低开销阶段汇总插桩或实时采集。

## 输出文件

- [fts-task-level-profile-tablets.csv](fts-task-level-profile-tablets.csv)
- [fts-task-level-profile-events.csv](fts-task-level-profile-events.csv)
- [fts_large_bench.sh](../tools/benchmark/fts_large_bench.sh)
