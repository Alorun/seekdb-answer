# FTS 构建任务划分与流水线优化实现提示词

## 目标

在当前 seekdb 工作区实现全文索引构建优化，重点降低两个 IK 全文索引的重复任务调度、重复扫描和重复分词开销。

现有 benchmark 已确认：

- `build_ik_all_sec=4.077`
- `build_ik_content_sec=3.031`
- `build_beng_en_sec=1.734`
- `build_total_sec=8.859`
- 两个 IK 构建合计约占三项索引构建时间的 80.2%

本任务只优化索引构建任务划分、并发执行、流水线衔接和数据分发。必须保持 TOKENIZE 结果、MATCH 语义、辅助表内容、行数和 checksum 一致。

## 工作区约束

1. 先执行 `git status --short`，保留其他 Agent 的现有修改，不 reset、不覆盖、不清理。
2. 当前工作区可能包含分词热路径优化修改，必须兼容这些修改。
3. 不修改 benchmark 数据、SQL、计时逻辑、评分脚本或基线文件。
4. 不通过减少输入数据、跳过索引构建、降低必要处理或硬编码结果获得性能。
5. 不反复执行完整编译和 20,000 行 benchmark。实现前先完成代码路径审查，集中修改后进行一次增量编译；只在编译通过后做一次最小正确性验证和一次完整 benchmark。
6. 如果当前构建工具、测试实例或资源不足，记录具体阻塞，不伪造性能数据。

## 当前代码重点审查范围

优先阅读并沿调用链确认：

- `src/rootserver/ddl_task/ob_fts_index_build_task.cpp/.h`
- `src/rootserver/ddl_task/ob_ddl_local_build_executor.cpp/.h`
- `src/rootserver/ddl_task/ob_ddl_tablet_scheduler.cpp/.h`
- `src/sql/engine/table/ob_table_scan_op.cpp`
- `src/sql/das/ob_das_domain_utils.cpp/.h`
- `src/storage/fts/ob_fts_parser_helper.cpp/.h`
- `src/storage/fts/ob_fts_stop_word.cpp/.h`
- DDL task 序列化、重试、取消和状态持久化代码

先确认当前两个辅助表子任务的创建顺序、提交顺序、父任务等待逻辑、source tablet 分发方式和已有 parallelism 决策。不要假定配置 parallelism 等于实际 worker 数。

# 阶段一：两个 IK 子任务并发调度

## 目标

让 `fti_all_ik` 和 `fti_content_ik` 两个相互独立的 IK 构建子任务可以重叠执行，避免当前串行等待。

目标执行关系：

```
parent_start
  ├── submit ik_all    -> child_start -> child_end
  └── submit ik_content -> child_start -> child_end
parent_wait
  └── both completed
```

BENG 索引是否并行由现有资源和任务语义决定；本阶段至少保证两个 IK 子任务不再被父任务无必要地串行等待。

## 实现要求

1. 修改父 FTS task 的子任务提交和状态机，使两个 IK 子任务在同一构建窗口内提交。
2. 父任务必须分别跟踪：
   - child task ID；
   - source/destination table ID；
   - child 状态；
   - child 返回码；
   - child retry 次数或现有 retry 状态；
   - child 完成时间；
   - parent 观察到完成的时间。
3. 父任务只有在两个 IK 子任务均成功后才能进入后续阶段。
4. 任一子任务失败时：
   - 停止或取消另一个仍运行的子任务；
   - 保持现有错误码传播语义；
   - 保持现有 retry 白名单和重试行为；
   - 不留下孤儿任务、未清理的临时资源或错误的成功状态。
5. 保留任务恢复和序列化语义。重启或 retry 后不能因为“已提交”状态丢失 child task，也不能重复创建不可区分的子任务。
6. 不简单把两个任务都设置为完整最大 parallelism。复用现有 parallelism 决策和 worker 资源限制；如需拆分预算，使用明确的总预算和最小并行度。
7. 不修改索引行格式、排序键、分词规则或 DML 写入语义。
8. 增加低开销阶段计时或复用已有时间字段，至少能记录两个 child 的：
   - submit time；
   - actual build start time（若现有接口可获得）；
   - finish time；
   - parent observed finish time。
   计时默认开启或可控，但禁止逐行日志。
9. 若当前框架只能做到“并发提交”而无法精确获得 actual start time，明确记录这个数据缺口，不用 submit time 冒充执行开始时间。

## 阶段一验证

实现完成后只做：

1. 一次增量编译相关 target。
2. 一次针对任务状态和失败路径的最小检查。
3. 一次完整 FTS benchmark，使用固定参数：
   - ROWS=20000
   - BATCH=500
   - SKIP_LOAD=0
   - ROUNDS=3000
   - QUERY_ROUNDS=200
   - SAMPLES=3
   - WARMUP=30
4. 对比：
   - 三个 build 指标；
   - build_total；
   - TOKENIZE 平均耗时；
   - 四个 MATCH 平均耗时；
   - 所有命中数；
   - 两个 IK child 的时间区间和 overlap。
5. 如果性能没有提升，保留正确实现并说明 CPU、磁盘或 worker 竞争原因，不继续无目的扩大修改。

# 阶段二：共享 IK source tablet 扫描和分词

## 目标

两个 IK 索引对相同 source tablet 的扫描和 IK 分词只执行一次，再将结果分别交给两个独立 consumer。共享扫描和分词，保持排序、目标行格式和写入流程独立。

目标结构：

```
source tablet scan
    -> read document
    -> IK tokenize once
    -> bounded intermediate stream
          ├── ik_all consumer
          └── ik_content consumer
```

## 明确边界

本阶段只共享：

- source tablet 的读取；
- LOB/文本获取；
- IK parser 调用；
- 词项和词频计算。

本阶段暂不重构：

- 通用 SQL scan 算子；
- 全部 DDL 任务框架；
- BENG parser；
- 查询执行链路；
- 全局缓存；
- 所有索引类型；
- 通用排序器或存储写入器。

两个 consumer 必须继续独立处理：

- 目标辅助表布局；
- 目标行构造；
- 排序键；
- flush；
- DML/存储写入；
- 失败、重试和清理。

## Producer 输出契约

新增 FTS 专用 producer 或等价的最小中间接口。每条中间记录至少应能表达：

- source tablet ID；
- document ID；
- 字段标识（title/content）；
- token；
- token frequency；
- document length；
- 目标 consumer 所需的分区或排序信息；
- producer 序号或可恢复的 batch 序号。

中间记录必须拥有明确的内存所有权。禁止把 parser 的临时 buffer、LOB 临时内存或已释放 arena 指针直接放入队列。

## 有界队列和背压

1. producer 到两个 consumer 使用有界队列或等价的有界 batch buffer。
2. 队列达到上限时 producer 必须阻塞或安全 spill，禁止无限增长。
3. 任一 consumer 变慢时，producer 必须受到背压。
4. producer、两个 consumer 和队列都必须支持：
   - 正常完成；
   - producer 失败；
   - consumer 失败；
   - parent 取消；
   - retry；
   - shutdown。
5. 先实现内存有界 batch；只有当前代码已有可靠临时文件/DDL spill 生命周期时才加入 spill。不要为了扩大范围自行设计通用 spill 系统。
6. 所有队列节点、批次 buffer 和 parser 临时资源在成功、失败、取消和 retry 路径都必须释放。

## Tablet 粒度和调度

以 source tablet 或有限 tablet batch 作为 producer 工作单元：

```
tablet -> scan -> tokenize -> enqueue
                       ├── consumer ik_all
                       └── consumer ik_content
```

要求：

1. 同一个 source tablet 只能由一个 producer 扫描一次。
2. 两个 consumer 必须能识别 tablet 边界，避免跨 tablet 错误合并。
3. producer 和 consumer 的完成状态必须可追踪。
4. 一个 tablet 失败时不能被两个 consumer 分别重复处理。
5. retry 必须从安全的 tablet/batch 边界重新开始，避免重复写入或丢数据。
6. 不改变 source snapshot、事务可见性和 DDL 快照语义。
7. 不使用跨任务全局缓存；PX worker、snapshot 和 retry 必须保持隔离。

## 正确性检查

至少验证：

1. 现有 TOKENIZE SQL 的结果与优化前一致。
2. 中文、英文和混合 MATCH 查询命中数一致。
3. `fti_all_ik` 和 `fti_content_ik` 的辅助表行数一致。
4. 相同输入文档的 token、word count、document length 一致。
5. 空文本、NULL/非空约束、超长 LOB、重复词、停用词和多字节字符结果一致。
6. producer/consumer 中途失败后 retry 不会重复或遗漏目标行。
7. 两个 consumer 的输出布局仍符合各自辅助表 schema。
8. 对共享 scan、队列生命周期和取消路径至少增加一个聚焦回归用例；不要只依赖耗时 benchmark。

## 阶段二验证

只在阶段一正确且有收益后执行：

1. 一次集中增量编译。
2. 一次最小 FTS 正确性回归。
3. 一次完整固定参数 benchmark。
4. 记录：
   - 两个 IK 构建耗时；
   - producer scan 时间；
   - tokenize 时间；
   - 两个 consumer 排序和写入时间；
   - queue 等待和背压时间；
   - 实际并行 worker 数；
   - 结果行数和 MATCH 命中数。
5. 将结果写入仓库共享文件，不能只留在终端或 `/tmp`。

## 禁止事项

- 不修改 benchmark 数据、SQL、评分器或基线。
- 不删除必要的扫描、分词、排序或写入步骤。
- 不通过全局 parser/cache 跨 snapshot 或 retry 复用。
- 不将配置 parallelism 当作实际利用率。
- 不逐行打印日志。
- 不引入与本阶段无关的查询优化、通用存储重构或排序器重写。
- 不在没有编译和正确性证据时声称性能提升。
- 不反复编译；优先一次代码审查、一次集中修改、一次增量编译、一次验证。

## 最终交付

最终报告必须说明：

1. 修改的文件和关键调用链。
2. 阶段一和阶段二分别完成了什么。
3. 哪些部分因当前框架限制未实现。
4. 失败、取消、retry 和恢复语义如何保持。
5. 编译命令和结果。
6. 正确性测试命令和结果。
7. benchmark 参数、原始报告路径和完整指标。
8. 优化前后 build、TOKENIZE、MATCH 数据。
9. 两个 IK 子任务 overlap 和实际 worker 利用率。
10. 未采集数据必须写 `NA` 并说明原因，禁止估算。

