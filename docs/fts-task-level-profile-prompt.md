# FTS Task-Level Profile Collection Prompt

请在当前 seekdb 工作区采集全文索引构建的任务级性能数据，供后续 Agent 分析任务划分、并行执行、流水线衔接和数据分发。此任务只做观测，不实现性能优化。

## 当前工作区

当前工作区有其他 Agent 的未提交 FTS 优化修改。先运行 `git status --short` 并记录状态；保留所有现存修改，不重置、不覆盖、不清理其他 Agent 的文件。只在确有必要时增加最小化观测代码。

已有代码入口包括：

- `src/rootserver/ddl_task/ob_fts_index_build_task.cpp`：父 FTS task、辅助表子任务创建和完成状态。
- `src/rootserver/ddl_task/ob_ddl_local_build_executor.cpp`：按 source tablet 建立 build context 并派发本地构建任务。
- `src/sql/engine/table/ob_table_scan_op.cpp`：FTS 源行读取、LOB 读取、分词和生成目标行。
- `src/sql/das/ob_das_domain_utils.cpp`：每文档词项和索引行生成。
- DAS/DML 写入路径：目标行缓冲、flush 和存储写入。

先检查这些路径已有的计数器、task 状态时间、trace 和 PX worker 统计，优先复用。不要假设配置的 `parallelism` 等于实际运行 worker 数。旧 quick profile 来自干净的旧版本，只能作为背景参考，不能用来填充当前任务级数据。

## 采集范围

使用现有 `tools/benchmark/fts_large_bench.sh` 完成一次完整 FTS 构建测量，默认参数保持基准一致：`ROWS=20000`、`BATCH=500`、`SKIP_LOAD=0`。记录实际参数、代码版本、工作区状态、机器/集群配置和报告路径。基准脚本会重建 `fts_large_bench`，只能连接专用测试实例；不得修改基准数据、SQL、计时逻辑、评分脚本或基线。

尽量一次编译、一次带观测的完整测量。不要在每个插桩点之后反复编译、启动和跑全量测试。若环境无法运行，报告具体阻塞和已采集到的部分数据，不得估算或编造缺失值。

## 必须采集的数据

### 1. 每个 tablet 的输入和输出量

每个 FTS 辅助表子任务、每个 source tablet 单独记录：

- parent task ID、child task ID、目标辅助表类型/ID、source tablet ID、destination tablet ID、所在节点。
- source rows scanned、physical rows（如果现有统计提供）、非空文档数、生成的目标行数。
- 文本 payload 字节数；LOB 文档数、LOB payload 字节数和 LOB 读取次数。按 `read_real_string_data()` 后实际文本长度统计，并明确这不是磁盘压缩字节数。不要将文本字节与 LOB 字节重复计入同一个总数。
- 文本长度直方图，至少包含 `0`、`1-64`、`65-256`、`257-1024`、`1-4KB`、`4-16KB`、`>16KB` 桶。

优先复用现有 `row_scanned_`、`physical_row_count_`、`row_inserted_` 等统计；新增计数时按 tablet/task 累加，禁止逐行日志。

### 2. 子任务时间线和重叠

分别记录父任务阶段、domain FTS index 子任务、doc-word 子任务及每 tablet build task 的开始/结束时间：

- 子任务创建/提交、实际 build 开始、源扫描开始/结束、写入开始/结束、子任务完成、父任务观察到完成。
- 时间戳和持续时间；跨节点时注明时钟来源/偏差。优先用同进程 monotonic clock 计算持续时间，用可对齐的 trace 或同步 wall clock 比较跨节点重叠。
- domain index 与 doc-word 子任务的实际重叠时长、各自占比，以及整个构建期间同时运行的最大/平均子任务数。

区分“父任务串行发起创建 RPC”和“子任务实际执行区间”；不能把提交顺序当成构建执行顺序。

### 3. 配置并行度和实际利用率

对每个子任务记录：

- 原始请求 parallelism、`decide_parallelism()` 之后的 parallelism、传给 tablet/local build 的 parallelism。
- 实际活跃 PX worker 数的时间序列或低频采样统计，包括平均值、最大值、worker-active time 和利用率。
- 定义利用率公式，例如 `active worker time / (configured parallelism * build wall time)`，说明采样周期和缺失数据。

从 trace/PX 统计或现有运行时计数获取实际 worker 活跃度。若做不到，明确标为未采集；不能用配置值代替实际值。

### 4. 扫描、分词、LOB 和写入等待

按子任务和 tablet 尽量拆分以下 wall time / CPU time：

- 获取源行及扫描等待。
- LOB 真实文本读取时间和字节数。
- 分词与词频聚合时间。
- 目标行构造时间。
- DAS/DML 缓冲、flush、存储写入等待时间。
- 可用时记录 RPC、调度排队、锁等待和下游背压时间。

明确计时边界，注明哪些是包含子阶段的累计时间，哪些是互斥阶段。并行 worker 的 CPU 时间总和可以大于任务 wall time，需分别报告。观测代码要使用累加计数器或阶段级采样，不能在每行输出 trace/log。

## 观测实现要求

1. 先审查现有统计和 trace，再决定缺哪些数据。
2. 缺指标时只增加 FTS DDL 范围内的低开销计数/计时，按 tablet/task 汇总，在阶段完成时输出一次。
3. 不修改扫描、分词、写入语义，不改变并行度、任务顺序或测试工作负载来影响结果。
4. 若插桩仅用于本次采集，采集完成后只撤销自己新增的临时代码，保留其他 Agent 的改动；若保留观测代码，必须默认关闭且不产生逐行日志。
5. 完整记录插桩代码的影响和数据精度。不要声称插桩计时等同于无插桩基准耗时。

## 共享结果文件

采集完成后将结果写到仓库内，保证其他 Agent 可直接读取：

- 主报告：`docs/fts-task-level-profile-report.md`
- 每 tablet 明细：`docs/fts-task-level-profile-tablets.csv`
- 任务事件时间线：`docs/fts-task-level-profile-events.csv`

主报告必须包含：结论摘要、运行环境和精确参数、task/tablet 汇总、两个子任务的 overlap 计算、parallelism 利用率、scan/LOB/tokenize/write 阶段耗时、数据来源与公式、已知缺口、插桩影响、生成文件路径。CSV 使用 UTF-8 和表头，时间单位写入列名或表头说明；无数据使用 `NA` 并附原因，不写零冒充有效测量。

最后检查三个结果文件完整且能从当前工作区读取，报告中给出运行命令和 benchmark 原始报告路径。不要只把结果留在 `/tmp`、终端输出或本机日志中。
