# FTS 内存与 I/O 优化实现提示词

## 目标

针对全文索引构建路径，减少解析器、LOB 临时 buffer、FTS 行对象和 batch buffer 的重复分配，降低内存抖动和写入等待。

当前 benchmark 结果：

- `build_ik_all_sec=4.077`
- `build_ik_content_sec=3.031`
- `build_beng_en_sec=1.734`
- `build_total_sec=8.859`
- 两个 IK 构建合计约占三项构建时间的 80.2%

本任务只实现内存与 I/O 方向的具体优化，不扩展为通用 SQL、存储层或查询链路重构。

## 工作区和执行约束

1. 先运行 `git status --short`，保留其他 Agent 的未提交修改，不 reset、不覆盖、不清理。
2. 先完整阅读当前实现和调用链，再集中修改。不要每改一个点就编译。
3. 最多分三步实现，每步完成后只进行必要的局部检查。
4. 允许一次增量编译相关 target；不要反复完整编译。
5. 允许一次最小 FTS 正确性验证和一次固定参数 benchmark。若实例不可用，记录阻塞，不伪造结果。
6. 不修改 benchmark 数据、SQL、评分脚本、基线、计时逻辑或索引语义。
7. 不通过跳过 LOB 读取、减少文档、减少 token 或降低必要 flush 获得性能。
8. 优化必须保持 source snapshot、事务可见性、retry 和 PX worker 隔离。

## 重点代码路径

优先检查：

- `src/sql/engine/table/ob_table_scan_op.cpp/.h`
- `src/sql/das/ob_das_domain_utils.cpp/.h`
- `src/storage/fts/ob_fts_parser_helper.cpp/.h`
- `src/storage/fts/ob_fts_stop_word.cpp/.h`
- `src/rootserver/ddl_task/ob_fts_index_build_task.cpp/.h`
- `src/rootserver/ddl_task/ob_ddl_local_build_executor.cpp/.h`

已知热点：

- `ObTableScanOp::fetch_next_fts_index_rows()` 中每个文档局部创建 `ObArenaAllocator`；
- `ObTextStringHelper::read_real_string_data()` 需要临时文本 buffer；
- `ObFTIndexRowCache::segment()` 每个文档生成目标 row；
- `generate_fulltext_word_rows()` 按当前词项数量申请 row 数组；
- DAS FTS iterator 已有 batch row 输出和 allocator reuse 机制，应优先复用，不要另造通用写入框架。

# 第一步：复用 FTS LOB allocator、parser 临时 buffer 和 row capacity

## 实现目标

消除文档热路径中重复构造临时 allocator，复用 FTS scan 的临时内存和已分配的 row/datum capacity。

## 具体要求

1. 将 `fetch_next_fts_index_rows()` 中的局部 LOB allocator 提升为 `ObTableScanOp` 的成员。
2. 在 scan 初始化、reset、close、rescan 和 retry 路径明确 allocator 生命周期。
3. 每篇文档开始前只清理本篇文档的临时内容，优先使用 `reuse()` 或等价 API，不能销毁并重新创建 allocator。
4. 保证 `read_real_string_data()` 返回的文本在当前 `segment()` 调用期间有效。
5. 不把指向临时 allocator 的 `ObString` 保存到下一篇文档或异步队列。
6. 复用 `ObFTIndexRowCache` 中已有 row buffer 和 datum buffer capacity：
   - 容量足够时只重置 count/index；
   - 超出容量时再扩容；
   - 不要每个文档都重新申请相同大小的 `ObDatumRow` 数组。
7. 保留现有 `word_map_`、parser helper 和 stop-word 逻辑，不修改分词规则。
8. 失败、取消、rescan 和 retry 时不得释放后仍被 row 或 datum 引用的内存。
9. 不把 parser 的长期状态和当前文档临时 buffer 混用，避免 reset parser 状态。

## 第一步边界

只处理已有单任务、单 scan 路径的 allocator 和 row capacity 复用。不要同时实现 producer、跨任务 cache、外部 spill 或通用 DAS allocator 改造。

# 第二步：实现 FTS 有界 batch，复用 batch buffer 并按字节 flush

## 实现目标

减少逐文档生成和逐行写入造成的内存分配与 flush 频率，同时限制峰值内存。

## 具体要求

1. 在现有 FTS DML iterator 或 row cache 路径中实现有界 batch。
2. batch 至少有两个限制：
   - 目标行数上限；
   - 估算字节数上限。
3. 达到任一限制时，将 batch 交给现有 DAS write buffer/flush 路径，然后整体 reuse batch 内存。
4. 不将整个 source tablet 或整张表加载到内存。
5. batch 中的 token、document length、word count 和 doc ID 必须拥有明确的内存所有权。
6. 如果现有 `ObDomainIndexRow`、`ObChunkDatumStore` 或 DAS write buffer 已能复用，就直接扩展其容量或 reuse 逻辑，不新增平行的写入系统。
7. 长文本或超大单文档必须能单独处理，不能因为 batch 限制截断 token 或跳过文档。
8. batch 边界不能破坏：
   - source snapshot；
   - tablet 边界；
   - 目标行排序；
   - retry 和失败清理；
   - consumer 写入顺序要求。
9. 不为了减少 flush 而无限增大 batch；默认使用保守的固定上限，并集中定义为常量或配置项。

## 第二步边界

只优化当前 FTS 构建的 batch 和 buffer 生命周期。不要重新设计外部排序器、临时文件系统、通用 spill 框架或所有索引类型的批处理。

# 第三步：在 IK 构建中复用一次 LOB 读取和分词结果

## 实现目标

为两个使用 IK 的辅助索引减少相同 source tablet 的重复文本读取和分词。

两个目标索引：

- `fti_all_ik(title, content)`
- `fti_content_ik(content)`

对 `content` 字段，应尽量形成：

```
一次 source scan
  -> 一次 LOB 读取
  -> 一次 IK 分词
  -> 两个独立输出 batch
       -> fti_all_ik writer
       -> fti_content_ik writer
```

## 具体要求

1. 只在两个 IK parser 名称和 parser properties 完全一致时共享结果。
2. producer 或等价复用层必须使用 bounded batch，不能使用全局无限缓存。
3. 共享数据至少包含：
   - document ID；
   - 字段标识；
   - token；
   - token frequency；
   - document length。
4. consumer 仍然独立负责：
   - 目标 row 布局；
   - 目标排序键；
   - 目标表写入；
   - flush；
   - 失败和 retry。
5. consumer 不得再次读取原始 LOB 或再次调用 IK parser。
6. token 字符串必须复制到 batch 自己拥有的内存中，不能引用已 reset 的 scan allocator。
7. producer、两个 consumer 和 batch 必须处理：
   - 正常完成；
   - 任一方失败；
   - parent 取消；
   - retry；
   - shutdown。
8. 失败时不能让另一个 consumer 继续消费不完整 batch，也不能遗留临时内存。
9. 如果当前任务框架无法在不大范围重构的情况下安全共享两个子任务的 scan，可以只实现同一任务内部的 batch 复用，并在报告中明确说明；不要引入跨任务全局缓存作为替代。
10. 保持两个辅助表的输出内容、行数、token、词频和 document length 完全一致。

## 第三步边界

不要扩展到：

- BENG parser；
- 查询执行；
- 全局 parser cache；
- 跨 snapshot/retry 的 token cache；
- 通用 SQL scan 算子；
- 通用存储层落盘策略；
- 完整外部 spill 系统。

# 统一正确性和性能验证

只做简单、必要的验证：

1. 一次增量编译相关 target，例如：
   ```bash
   cmake --build build_release --target ob_storage -j2
   ```
   如果实际依赖目标不同，使用对应最小 target，并记录命令。
2. 运行 `git diff --check`。
3. 执行现有 FTS 正确性用例，至少覆盖：
   - TOKENIZE 中文和英文；
   - 中文、英文、混合 MATCH；
   - LIMIT 查询；
   - 空文本、重复词和停用词。
4. 在专用实例上最多运行一次固定 benchmark：
   ```bash
   ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
   SAMPLES=3 WARMUP=30 SKIP_LOAD=0 \
   bash tools/benchmark/fts_large_bench.sh
   ```
5. 只需要确认：
   - 构建命令成功；
   - TOKENIZE/MATCH 结果没有变化；
   - 三个 build 指标和 `build_total_sec` 没有明显退化；
   - benchmark 中的命中数一致。
6. 不要求本地采集完整 allocator 分配次数、每 tablet 内存、每阶段 I/O 或平台级 worker 利用率。除非已有统计可直接读取，否则不要新增详细插桩。
7. 本地 benchmark 与平台环境不同，不要据此宣称最终评分提升。若无法证明收益，只报告“功能正确且未观察到明显负优化”。
8. 不因本地结果波动而反复编译或反复跑全量 benchmark。

## 最终交付

报告必须包含：

- 修改文件；
- 三步中实际完成的步骤；
- 未完成步骤及具体原因；
- allocator、row buffer、batch 和 LOB 复用的实现位置；
- 错误、取消和 retry 路径处理；
- 编译命令和结果；
- FTS 正确性验证结果；
- 一次 benchmark 的 build 指标、TOKENIZE/MATCH 指标和命中数；
- 明确说明没有采集的详细任务级数据，不得估算；
- 不得把“编译成功”或“耗时波动”写成确定的性能提升。

