# MiniVectorDB 详细实施计划

| 项目 | 内容 |
| --- | --- |
| 文档版本 | v3.1 / 2026-11-09（v1.0–v3.0 同日） |
| 对应赛题 | `db.md`：实现支持 SQL、持久化存储与事务的轻量向量数据库（总分 80） |
| 目标仓库 | 官方 `oceanbase/seekdb` 的 `2026-competition-round2` 分支（tip `c842a368c`）；提交仓库**建议使用真正的官方 Fork**（当前 `Alorun/seekdb-answer` 是非 fork 克隆，见 §12.5） |
| 语言决定 | **C++20**（已确认，不再变更） |
| 截止时间 | **2026-11-18 比赛结束**（以平台实际截止时刻为准；最后提交至少留 4 小时缓冲） |
| 提交节奏 | **每天计划 2 次正式提交（平台上限 2 次），具体时刻自定**（建议两班之间留出结果回流时间）；额度当日清零、不结转；提交前必须通过与规范相关的发布门禁 |
| 题包状态 | **官方题包/测试驱动不公开展示**；只能靠赛题说明、平台预检、正式提交反馈和协议自适配来逼近 |
| 当前状态 | 仓库已 public、分支已推送；**v0.4 已实现**：磁盘模式、WAL checkpoint、AVX2 距离、有界 TopK、mysqltest 兼容、崩溃/随机 fuzz 与 SQL 边界回归；`agent/conversation.json` 已生成；待平台预检/协议反馈 |
| 一句话策略 | 用 9 天、最多约 20 次提交打一场"发布列车"：每次提交都是可构建、可运行、材料同 commit 的合规候选；先拿功能准入，再抢性能，最后用早期通过版本兜底。 |

**变更记录**

1. 语言锁定 C++20。
2. 排期从"10 个工作日"改为 **11/09–11/18 的逐日双提交列车**，含具体提交窗口与门禁。
3. 题包确认"不公开"：新增 **协议适配层 + 预检探针 + 正式提交反馈闭环 + stderr 诊断日志**，不再假设能提前冻结协议。
4. 新增 **发布门禁（Release Gate）**、**已知通过版本兜底**、**最终参评版本选择** 和 **提交台账**。
5. 明确"合规"的边界：交付物、构建、协议自测、材料、流程可保证；隐藏用例是否通过无法在提交前保证，因此用每日 2 次额度换反馈。
6. **v2.1**：提交时刻不再固定为具体钟点，只保留"每天最多 2 次、截止日提前"的约束；排期表改为"日期 + 第 1/2 班"。
7. **v2.1**：新增仓库可见性/可拉取的硬门槛（当前仓库私有，平台拉不到），并把它提升到与功能准入同级的风险。
8. **v2.1**：明确区分"平台反馈原文"与"`conversation.json` 学习材料"；只保存提示词不足以拿 prompt 分。
9. **v2.2**：补充对 `Alorun/seekdb-answer` 的 API 核对结果（私有、admin=true、fork=false）与公开/镜像的具体步骤。
10. **v2.2**：写明 `conversation.json` 的真实性红线与"10 分钟访谈"采集流程；Agent 只做结构化/脱敏/引用校验，不代写或编造学生证据。
11. **v2.3**：仓库已转 public，`2026-competition-round2` 分支已推送，匿名 `git clone --depth 1` 验证通过；HTTPS PAT 只读（push 403），推送改走 SSH。
12. **v2.3**：证据流程确定为"Agent 起草问题 → 学生回答 → Agent 整理成稿 → 学生确认 → 写入文件"；整理稿不使用学生原话，但不得改变事实。
13. **v2.4**：澄清官方 `oceanbase/seekdb` 本来就有 `2026-competition-round2` 分支（tip `c842a368c`）；提交用的 fork 之前缺同名分支，现已在**同一 commit** 上补齐，两边 SHA 一致。
14. **v2.5**：区分"基于官方 round2 开发"与"GitHub fork 关系"；当前仓库 `fork:false`，建议做真正的官方 Fork 并把同一分支推过去，或先向组委会确认非 fork 公开仓库是否可接受。
15. **v2.6**：v0.1 代码落地（C++20）；本地 10k×128 写入约 0.49 s、1000 次精确 TopK 约 0.86 s、重启约 0.04 s；Docker 构建/运行/重启通过。
16. **v2.7**：v0.2 加入 WAL 位置索引与磁盘模式（超过内存阈值不再全表驻留）、干净关闭时的 WAL checkpoint 压缩、AVX2/FMA 距离内核、行数据/事务/checkpoint/恢复崩溃注入与完整回归；本地 1000 次精确 TopK 约 0.67 s。
17. **v2.8**：v0.3 把普通 ORDER BY + LIMIT 改为有界堆、无 ORDER BY + LIMIT 提前停止、距离 TopK 只为入选行复制行数据；1000 次精确 TopK 降到约 0.40 s。
18. **v2.9**：真实证据材料 `agent/conversation.json`（8.7 KiB，同内容根副本）已生成；发布门禁（含 Docker 内存/磁盘冒烟与匿名可拉取）PASS。
19. **v3.0**：v0.4 修复 SIGTERM 执行 pending 输入、解析错误丢弃后续语句、跨 chunk 的 token 截断、int64 ORDER BY 精度、索引名跨表冲突；补 `IF [NOT] EXISTS`、`DROP TABLE`、`TRUNCATE`、`SET`/`USE` 兼容；SQL 边界 158 项 × 2 模式、随机崩溃 800 次迭代均通过；ANN 原型评估后决定不启用（uniform 10k 收益不足）。
20. **v3.1（提测失败后修复）**：从官方 `2026-competition-round2` 新提交 `a3dba559c` 找到公开用例 `mysqltest/`：每个非空行是一条完整 SQL（无分号），结果断言包含 `affected_rows`/`columns`/`rows`。提交版默认 semicolon + 无 DML 输出，导致驱动无法执行/无法断言，全部用例失败。现已把默认改为 **line framing + `OK <affected>`**；本地用公开用例逐条校验通过（`SELECT category` 为 unordered）。

> 本文档是实施计划，不包含实现代码。所有带 ⚠️ 的条目表示依赖题包/平台确认，不能靠猜测硬编码。

---

## 0. 执行摘要

1. **生死线是功能准入**：任何一个 MySQL case 错误、崩溃、超时或资源超限，该次提交总分为 0。因此不允许"为了提交而提交"；每次提交都必须是自测范围内可信的候选。
2. **性能分是相对排名**：10k 条 × 128 维 × 1000 次 TopK 的规模很小，SIMD 精确扫描 + 过滤下推就能拿到常数级优势；ANN 是"有测量依据 + recall 有余量"才启用的加分项，且必须有精确回退。
3. **`CREATE VECTOR INDEX` 与 `ann` 模式先做语义正确**：`ann` 只要求 Recall@10 ≥ 0.95，内部用精确扫描也能通过。功能准入不押注 ANN 质量；性能阶段再替换为真正的 HNSW。
4. **崩溃恢复是第二大风险**：强杀模型下用"WAL 提交标记 + fdatasync + 原子 manifest 切换 + 恢复只读重放"把不确定窗口压到一个点；用 14 个注入点反复验证。
5. **协议是最大未知**：题包不公开，必须把 I/O 层做成可切换 profile，并把"平台预检"当作免费探针；早期两次提交允许用于协议校准，但每次仍是完整合规包。
6. **每天 2 次提交是一台发布列车**：上午/晚上各一班；每班有明确目标、发布门禁、台账和回退版本。**一旦拿到第一个"已知全部通过"的版本，就必须每天结束时保有它**；在那之前，把拿到准入作为最高优先级。
7. **Agent prompt 40 分**：从第一天维护实验台账，每天为当次提交生成一份同 commit 的 `conversation.json` 快照；不编造、不超 128 KiB。

---

## 1. 赛题解构

### 1.1 计分漏斗

```
实现 ──► MySQL case 全量通过？──否──► 总分 0（性能、prompt 均不排名）
                │是
                ├──► 性能 benchmark（3 轮，中位数 E）──► 排名 r_perf ──► S_perf = max(40-0.5(r-1), 0)
                └──► Agent prompt（3 轮评审，中位数）──► 排名 r_prompt ─► S_prompt = max(40-0.5(r-1), 0)
                                        S_total = S_perf + S_prompt
```

- 功能准入不按比例给分：**一个用例失败 = 0**。
- 性能项：3 个有效轮次各算 E，取中位数、保留 6 位小数后排名；并列按竞赛排名（1、1、3）。
- prompt 项：五个维度分别取三次评审的中位数后相加得原始分，按原始分排名；原始分不封顶。
- 两项独立：性能无效只是性能 0 分，prompt 仍可得分；反之亦然。
- 最终参评版本只能选一次，必须与 `conversation.json`、源码、Dockerfile 同一个 commit。

**推论**：开发顺序必须是 `功能正确性 → 崩溃/资源健壮性 → 性能 → prompt 材料`，任何阶段都不能让已有功能回退。

### 1.2 硬性约束（不可协商）

| 约束 | 内容 | 设计含义 |
| --- | --- | --- |
| 形态 | 单机、单进程、单客户端、同一时刻最多一个显式事务 | 不需要并发控制、锁管理器、连接池；单线程事件循环即可 |
| 接口 | stdin 读 SQL，stdout 写结果，stderr 写日志；不监听端口 | 协议层独立、可配置；stdout 必须只有评测输出 |
| 数据目录 | 所有数据/日志/索引/临时文件写入 `/data` | 路径集中管理，禁止写到 `/tmp` 或内存文件系统规避 I/O 统计 |
| 依赖 | 可用通用文件/序列化/数学/SQL 解析库；**不得**用 SQLite/RocksDB 等引擎，**不得**调 Faiss/VSAG/HNSWlib 等索引库 | 存储、执行、索引、事务全部自研；建议零第三方运行时依赖 |
| 构建 | Dockerfile 从源码构建；正式执行禁止联网；依赖构建期锁定 | 多阶段构建、固定基础镜像 digest、`--network none` 验证 |
| 资源 | 正式测试有 CPU/内存/磁盘/时间限制，且数据量可超过内存 | 存储与查询必须流式、有界内存，不能全量载入 |
| 持久化 | COMMIT 成功响应前完成持久化；重启不丢已确认提交 | `fdatasync` 提交屏障；未提交事务不得部分生效 |
| 崩溃模型 | 仅进程强制终止；不模拟断电/坏盘/外部篡改 | 页缓存会保留；但尾部半条记录、写到一半的 manifest 仍需处理 |
| 提交 | 每天最多 2 次正式提交；锁定 commit SHA/镜像/材料 | 预算有限，必须"先自测后提交"；预检不占额度 |
| 语义 | 不支持 NULL/JOIN/聚合/子查询/ALTER/FK/嵌套事务/主键更新；不支持语句必须返回约定错误 | 解析器要"严格拒绝"，不能静默解释成别的含义 |

### 1.3 功能语义清单（实现与验收的对照表）

| # | 能力 | 必须行为 | 易错点 | 验证方式 |
| --- | --- | --- | --- | --- |
| F1 | CREATE TABLE | BIGINT、VARCHAR(n)、VECTOR(n)；一个 BIGINT 主键；每表最多一个向量列 | 重复建表、非法类型/维度、缺主键、多主键、多向量列 | DDL 正反用例 |
| F2 | INSERT | 多行 INSERT；主键唯一；VARCHAR 按码点长度校验；向量 float32 校验 | 多行中某行失败必须整条语句回滚；PK 重复；NaN/Inf/溢出 | 语句原子性用例 |
| F3 | SELECT | 普通列投影、`SELECT *`、`=`/`<`/`<=`/`>`/`>=`、AND、ORDER BY、LIMIT | ORDER BY 并列按主键升序；无 ORDER BY 按集合比较；LIMIT 语义 | 差分测试 |
| F4 | L2_DISTANCE | 欧氏距离；输入向量字面量；维度校验 | 用平方距离比较但输出必须开方；float32 存储后计算；格式/精度 | 与 double 真值比对容差 |
| F5 | 精确 TopK | `ORDER BY distance LIMIT K`；距离并列按 PK 升序 | 不能先全局 TopK 再过滤；不足 K 条返回全部 | 暴力真值对拍 |
| F6 | 过滤 TopK | 在 WHERE 命中集合内计算 TopK | 过滤列可能是 VARCHAR/BIGINT；空集合返回空 | 构造反例：全局最近但被过滤 |
| F7 | UPDATE | 按主键或条件；多列 SET；主键不可更新 | 语句失败不能留下部分修改；更新向量后检索结果一致 | 事务前后快照对比 |
| F8 | DELETE | 按主键或条件；无条件时删全表 | 删除后向量检索不可再命中；回滚恢复 | 快照对比 |
| F9 | 事务 | BEGIN/COMMIT/ROLLBACK；读己写；autocommit；语句失败不污染事务 | 事务内向量检索与应用层扫描一致；ROLLBACK 后索引/可见集恢复 | 状态机用例 |
| F10 | 持久化 | 正常关闭/重启后表定义与数据一致 | 关闭时回滚未提交事务；目录/manifest 落盘 | 重启用例 |
| F11 | 崩溃恢复 | 强杀后重启；已提交不丢；未提交不全有；恢复中再杀可重复恢复 | commit 标记先于数据可见；尾部半记录；checkpoint 原子性 | 注入式 kill 矩阵 |
| F12 | 索引 | `CREATE VECTOR INDEX name ON t(v)`；建索引后增删改反映到检索 | 索引陈旧、删除后仍返回、事务未提交数据泄漏 | 交错 DML + ann 查询 |
| F13 | 资源 | 数据量 > 内存仍可写入/查询/恢复 | 全量载入、每行分配、mmap 全文件、页缓存峰值 | cgroup 限额压测 |
| F14 | 错误 | 不支持的语句/类型/语法返回约定错误码 | 静默成功、输出混入 stderr、错误后会话不可继续 | 错误码用例 |

### 1.4 明确不做（防止范围蔓延）

- NULL、三值逻辑、JOIN、聚合、子查询、HAVING、DISTINCT、UNION。
- ALTER TABLE、DROP TABLE、外键、视图、触发器、存储过程。
- 嵌套事务、保存点语法、多客户端并发、分布式。
- 主键更新、`INSERT ... ON DUPLICATE KEY`、REPLACE。
- 完整 MySQL 类型系统（DECIMAL、DATE、TEXT、JSON 等）。
- OR / NOT / LIKE / IN（除非题包明确要求；解析到就报"不支持"错误，不能当 AND 处理）。

---

## 2. 仓库与环境基线

### 2.1 仓库现状（已核对）

| 事实 | 说明 |
| --- | --- |
| HEAD | `c842a368c` "2026 competition one more round"，本地分支 `2026-competition-round2`，`upstream/2026-competition-round2` 指向同一 commit |
| 官方分支 | `oceanbase/seekdb` 的 `2026-competition-round2` **存在**，tip 也是 `c842a368c`；它只有 37 个脚手架文件，不包含题包/协议 |
| fork 分支 | 提交用的 `Alorun/seekdb-answer` 之前没有同名分支；现已创建并推送到同一个 `c842a368c`，与官方分支内容完全一致 |
| 工作树内容 | 该分支被裁剪为 **37 个 tracked 文件**：仅保留 `.github/`、`.devcontainer/`、`.agents/`、构建 CI 脚本、`Dockerfile`、`conversation.json` 等脚手架；seekdb 源码全部删除 |
| 占位文件 | 根目录 `Dockerfile` 与 `conversation.json` 均为 **0 字节** |
| 测试题包 | 工作区内**未发现**公开样例、协议检查工具、测试驱动或题包（已搜索 `minivector`/`round2`/题包相关命名） |
| 远端 | `origin` 指向个人仓库，`upstream` 指向 oceanbase/seekdb；提交需推送到 `2026-competition-round2` 分支 |
| 远端可获取性 ✅ | 仓库已转 **public**；`2026-competition-round2` 已创建并推送（commit `c842a368c...`）；无凭据 `git ls-remote` 与 `git clone --depth 1` 均成功（11/09 验证）。注意：HTTPS PAT 当前只读（push 返回 403），推送走 SSH（身份 `25ALN`）；提交 URL 用 `https://github.com/Alorun/seekdb-answer.git`，不带 token |
| 安全 ⚠️ | `.git/config` 的 `origin` URL 内嵌了 GitHub PAT（明文），存在泄露风险；建议改用 credential helper 并轮换该 token，任何提交材料都不得包含 |

**结论**：这是一次从零实现（greenfield）。不要试图恢复/依赖 seekdb 原有向量索引与存储代码——规则要求存储、执行、索引、事务核心自行实现；把本仓库当作"空仓库 + 交付脚手架"使用。

### 2.2 本机环境

| 项 | 值 | 用途 |
| --- | --- | --- |
| OS | Linux x86_64（Ubuntu 24.04 系） | 与正式容器一致 |
| 编译器 | g++ 13.3.0 | C++20 |
| 构建 | CMake 3.28.3 | 生成 Ninja/Make |
| Docker | 29.1.3 | 本地容器验证 |
| CPU | 16 逻辑核，支持 AVX2/SSE4.2，无 AVX-512 | SIMD 内核需运行时派发 |
| 内存 | 约 13 GiB | 本地压测够用；不能代表正式限额 |
| 磁盘 | 根分区仅剩约 8 GB | 镜像/构建产物要控制体积，及时清理 |

### 2.3 题包缺口（官方不公开，只能逼近）

⚠️ 官方题包/测试驱动不公开展示，以下协议细节**无法在提交前冻结**。实现必须做成可配置/可替换，不能写死；宁可多兼容，不要猜死一种格式。

| # | 未知项 | 影响 | 临时默认/探测方式 |
| --- | --- | --- | --- |
| U1 | 语句边界如何界定（分号/空行/特殊分隔） | 整个 I/O 层 | **解析完备性分帧**（见 §2.4.2），同时兼容逐行/整段/多语句 |
| U2 | 结果集格式（表头、分隔符、行数、DML 响应） | 所有用例比对 | 协议 profile（默认 mysqltest 风格）；预检/提交反馈切换 |
| U3 | 错误码表与错误文本格式 | 全部负向用例 | 可配置映射表；默认 MySQL 风格；stderr 记录内部码 |
| U4 | 就绪标识（内容/时机） | 启动握手 | 恢复完成后输出可配置 marker，默认 `ready` |
| U5 | 正常关闭方式（EOF/命令/信号） | 持久化与回滚 | 全部支持：EOF、SIGTERM、SIGINT、`exit`/`quit` |
| U6 | exact/ann 模式如何切换（参数/会话变量/特殊语句） | 检索路径 | 参数、环境变量、`SET`、hint 多路支持；默认 exact |
| U7 | 浮点输出精度/是否带容差 | TopK 输出 | 高精度输出（如 `%.9g`）交容差；探针确认 |
| U8 | 标识符大小写、转义、注释规则 | 解析器 | MySQL 常见规则 + 宽松兼容；解析器单测覆盖 |
| U9 | 索引准备时间上限、资源限额 | 索引方案选择 | 先做轻量/可回退方案，测后决定 |
| U10 | 提交配置形态（模式写在哪） | exact/ann 固定方式 | 默认 exact，并在 README/配置中显式声明；多路覆盖 |
| U11 | 数据分布与过滤列基数 | 性能调优 | 用题面公开描述 + 多种子生成压测 |

**行动**：以 `docs/PROTOCOL.md` 记录"当前采用的 profile + 证据来源 + 待验证假设"；每次预检/正式提交后更新。协议不是一次性冻结，而是**每个提交版本演进的配置**。

### 2.4 隐藏题包下的协议策略

**核心原则**：输入层尽量多兼容；输出层用一个可切换 profile；用免费的预检和早期正式提交换信息；每次提交仍然必须是完整合规包。

#### 2.4.1 信息渠道（按优先级）

1. **平台环境预检**（不占正式额度）：仓库/commit 可获取、镜像构建、启动、协议、公开样例。每次正式提交前必跑，输出全部留存。
2. **正式提交反馈**：用例通过情况、失败类别、必要日志（可能包含我们的 stderr）。这是隐藏协议的主要学习来源。
3. **赛题说明与示例**：SQL 形态、示例输出风格、stderr 约束、评分规则。
4. **官方答疑/公告**：如果平台有补充说明。
5. **形态推断**：最后手段，明确标注为假设，不做不可逆设计。

#### 2.4.2 输入层：用"解析完备性"分帧

不假设"一行一条"或"必须带分号"，而是维护输入缓冲并反复尝试解析：

- 缓冲中能**完整解析出一条语句** → 立即执行（即使没有分号）；
- 解析器返回"语句尚未结束（EOF）" → 继续读下一块；
- 解析器返回语法错误且不在字符串/注释中 → 输出错误，丢弃到下一个分号或行边界；
- 同时支持：`;` 分隔多语句、跨行、字符串/注释内分号、`delimiter` 指令。

这样可同时兼容"整段脚本管道"和"逐行发送"两种驱动实现，且不会把多行 `CREATE TABLE` 误当完整语句。

#### 2.4.3 输出层：profile 化

| profile | 启动 | SELECT | DML | 错误 | 适用假设 |
| --- | --- | --- | --- | --- | --- |
| `mysqltest`（默认） | `ready` | 表头 + TAB 分隔行，无边框 | 无输出 | `ERROR <code>: <msg>` | 最接近 mysqltest `.result` |
| `mysql-batch` | `ready` | 表头 + TAB 行 | 无输出 | `ERROR <code> (<sqlstate>): <msg>` | MySQL 客户端风格 |
| `verbose` | `ready` | 表头 + TAB 行 | `OK <n>` | 同上 | 仅本地自测，不用于正式提交 |

互斥格式无法同时满足；profile 由启动参数/环境变量选择，默认 `mysqltest`。预检或首次提交反馈显示格式不符时，下一次提交切换 profile（并保留切换记录）。

#### 2.4.4 就绪标识与 stdout 纪律

- 恢复 + 索引重建全部完成后输出 ready 行，并立即 `fflush`；
- 默认 `ready`，支持 `--ready-marker` 配置；
- ready 之前 stdout **不得有任何字节**；启动诊断全部走 stderr；
- 若预检对启动/握手有反馈，优先按反馈调整。

#### 2.4.5 错误码

- 内部 `ErrorCode` 枚举 + 单一映射表（一处修改，全局生效）；
- 默认 MySQL errno/SQLSTATE 风格，错误文本单行、无控制字符；
- 每个错误同时写 stderr：内部错误码、语句类型、位置（行/列）；**默认不记录 SQL 原文**，避免把隐藏用例内容带进日志；
- 题包不公开意味着错误码是**不可完全消除的风险**；通过失败类别与日志逐步修正，不在错误码上做不可逆耦合。

#### 2.4.6 模式切换（exact/ann）

- 支持：`--query-mode=exact|ann`、环境变量 `MVDB_QUERY_MODE`、`SET query_mode='ann'`、`SET @@query_mode='ann'`、SQL hint `/*+ ann */`；
- 默认 `exact`（最安全）；
- 提交配置形态未知（U10）：以"默认 exact + README 显式声明 + 多路支持"兜底；若平台配置能指定，按平台方式设置。

#### 2.4.7 正常关闭

- stdin EOF、`exit`/`quit`、SIGTERM/SIGINT：回滚活动事务 → 必要落盘 → 退出 0；
- SIGKILL：不捕获，靠恢复逻辑；
- 关闭期间不输出无关内容到 stdout。

#### 2.4.8 stderr 诊断日志（合法且关键）

- 启动：版本、git SHA、协议 profile、查询模式、data-dir、恢复耗时；
- 每条语句：类型（SELECT/INSERT/…）、长度、内容哈希、解析/执行结果、耗时；**默认不记录 SQL 原文**；本地调试构建可开详细日志，但不用于正式提交；
- 关键事件：恢复重放事务数/记录数、checkpoint generation、索引构建耗时、错误码；
- 日志限流/限大小（如滚动 2 MiB），**不得包含完整数据、密钥、token**；
- stdout 永远只有协议输出。

#### 2.4.9 前几个提交的定位

- 第 1–2 次提交的目标不是通过全部隐藏用例，而是验证：镜像可构建、程序可启动、握手/结果格式/错误形态是否匹配；
- 仍然必须通过发布门禁（交付物、构建、协议自测、材料、流程全部合规）；
- 若失败类别指向协议，下一次提交优先修协议，不继续堆功能；
- **协议 A/B 规则**：若 S1 在下一班前没有返回可判读的反馈，S2 用"代码不变、仅切换协议 profile/ready marker"的对照版本，避免同时改代码和协议导致无法归因；若 S1 已明确协议错误，则 S2 直接修协议；若 S1 表明协议正确，则 S2 按计划加功能；
- 协议校准预算：**2–4 次提交**（见 §13）；每次只改一个协议变量（profile / marker / 错误格式），并记录在 `docs/PROTOCOL.md`。

#### 2.4.10 不能保证的事（必须如实承认）

- 隐藏用例的精确期望文本、错误码、浮点格式；
- 就绪标识字符串、模式切换方式、提交配置形态；
- 隐藏数据分布与资源限额。

这些只能通过"预检 + 提交反馈 + 可切换 profile"逐步收敛；计划中已预留对应提交次数，不把不确定性伪装成确定结论。

---

## 3. 技术选型

### 3.1 结论

| 维度 | 选择 | 理由 |
| --- | --- | --- |
| 语言 | **C++20（已确认）** | 仓库本身是 C++ 项目；对内存布局、SIMD、`pread`/`fdatasync`、cgroup 内存峰值有完全控制；无 GC/运行时抖动；单二进制部署 |
| 构建 | CMake + Ninja，Release (`-O3 -DNDEBUG`)；开发另开 ASan/UBSan 构建 | 构建快、容器内可复现 |
| 依赖 | **零第三方运行时依赖**；测试脚本用 Python | 规避离线构建、版本锁定、供应链风险；解析器/CRC/序列化自研 |
| 并发 | 单线程 | 题目单连接；消除并发正确性风险；索引构建在就绪/DDL 时同步完成 |
| 存储 | 追加写 WAL（同时作为提交日志与近期数据） + 不可变段文件 + 原子 manifest | 写路径只有顺序写 + 一次 fsync；崩溃恢复只需"重放已提交事务" |
| 索引 | exact：SIMD 暴力扫描 + 过滤下推；ann：先语义回退，后接 HNSW（IVF 备选） | 先保证准入；再按实测 E 决定是否启用 ANN |
| 容器 | 多阶段构建，`ubuntu:24.04`（固定 digest），静态/自包含二进制 | 与题面示例一致；无网络 |

### 3.2 为什么不用其他方案

- **Rust**：同样可行且有内存安全优势，但仓库/工具链是 C++ 生态，且本机 Cargo 依赖缓存不确定；C++ 在 SIMD 内联、`mmap`/文件 API 上更直接。若团队更熟 Rust，可替换，架构不变。
- **Go**：GC 峰值与页缓存叠加不利于 `memory.peak` 指标；SIMD 需汇编/第三方库。
- **Python**：10k×128×1000 的扫描即使 numpy 可用也难保证 T/B/M 排名；启动与内存也不可控。
- **SQLite/DuckDB 等**：规则禁止作为存储/执行引擎；仅可作为本地测试 oracle 的参考（注意不要链接进交付物）。

### 3.3 目录结构（目标）

```
seekdb-answer/
├── CMakeLists.txt
├── Dockerfile                     # 多阶段：build → runtime
├── README.md                      # 运行说明、SQL 范围、错误、示例（4.2 要求）
├── docs/
│   ├── PLAN.md                    # 本文档
│   ├── PROTOCOL.md                # 协议 profile、证据与待验证假设
│   ├── DESIGN.md                  # 存储/事务/索引设计（4.3 要求）
│   ├── TESTING.md                 # 测试矩阵与崩溃注入说明
│   └── RUNBOOK.md                 # 本地/容器操作手册
├── agent/
│   ├── conversation.json          # 最终学习材料（≤128 KiB）
│   └── evidence/                  # 原始实验台账（不进最终 JSON，但用于组装）
├── src/
│   ├── main.cpp                   # 参数、启动、信号、session 循环
│   ├── common/                    # status/error、crc32c、bitmap、编码、字符串
│   ├── io/                        # 协议解析、结果格式化、flush
│   ├── sql/                       # lexer、parser、ast、executor
│   ├── catalog/                   # 表/列/索引元数据
│   ├── storage/                   # record、wal、segment、manifest、checkpoint、recovery
│   ├── txn/                       # 事务状态机、overlay、可见性
│   └── vector/                    # distance(SIMD)、topk、filter、ann(hnsw/ivf)
├── tools/                         # driver.py、gen_data.py、bench.py、build_conversation.py
├── tests/
│   ├── unit/
│   ├── sql/                       # .test + .result（mysqltest 风格）
│   ├── crash/
│   ├── resource/
│   └── perf/
└── scripts/
    ├── build.sh
    ├── check.sh                   # 一键跑全部本地门禁
    └── crash_matrix.sh
```

预估代码量：核心 6k–9k 行 C++（解析器 1.2k、存储/恢复 2k、执行器 1.5k、向量 1k、协议/工具 0.8k），测试与脚本 2k 行。按 11/09–11/18 的 9 个自然日排期，必须**优先保证正确性、控制复杂度、每天可交付**；性能优化只做有测量依据的部分。

---

## 4. 总体架构

### 4.1 分层

```
            stdin (SQL 文本)
                 │
        ┌────────▼────────┐
        │ io/session      │  语句分帧、模式切换、ready、flush、退出
        └────────┬────────┘
                 │ Statement
        ┌────────▼────────┐
        │ sql/parser      │  lexer → AST → 语义校验
        └────────┬────────┘
                 │
        ┌────────▼────────┐
        │ sql/executor    │  DDL/DML/SELECT/TXN 分派；结果集/影响行数
        └───┬────────┬────┘
            │        │
   ┌────────▼──┐  ┌──▼─────────────┐
   │ txn       │  │ storage        │
   │ overlay   │  │ catalog        │
   │ 可见性     │  │ WAL/segment    │
   └────────┬──┘  │ manifest       │
            │     │ checkpoint     │
            │     │ recovery       │
            │     └──┬─────────────┘
            │        │
        ┌───▼────────▼───┐
        │ vector          │  distance / topk / filter / ann
        └───┬─────────────┘
            │
        ┌───▼─────────────┐
        │ io/formatter    │  → stdout
        └─────────────────┘
```

### 4.2 执行模型

- **单线程、同步**：读一条语句 → 解析 → 执行 → 输出 → flush。没有后台 flush 线程；checkpoint 在安全点同步执行。
- **状态**：`Database`（catalog + table stores）、`Session`（当前事务、查询模式）、`Protocol`（格式配置）。
- **错误处理**：内部统一 `Status{code, message}`；语句级错误不终止进程；I/O/一致性错误写 stderr 并按题包约定退出（避免带病继续）。
- **确定性**：不使用哈希表迭代顺序决定输出顺序；排序/并列规则显式实现；随机只用于测试。

### 4.3 三条关键数据流

**写入（autocommit 或事务内语句）**

```
解析/校验 → 定位旧版本（PK map/overlay）→ 生成行镜像
→ 追加到语句级缓冲 → 语句成功：写入 WAL 文件（顺序 write）
→ [COMMIT 时] 追加 COMMIT 标记 → fdatasync
→ 发布到内存可见结构（bitmap/PK map）→ 输出成功
```

**查询**

```
解析 → 选择数据源（PK 点查 / 全表流式扫描）
→ 谓词下推（先过滤，再算距离）
→ TopK（(dist, pk) 比较）或普通排序
→ 投影/格式化 → 输出
```

**恢复**

```
读 manifest（不存在则初始化空库）
→ 打开段文件（校验）→ 全量置 live
→ 顺序重放 WAL：先收集已 COMMIT 的事务，再按 LSN 顺序应用
→ 截断尾部半条记录
→ 按需重建 ANN 索引（完成前不 ready）
→ 输出 ready
```

---

## 5. 存储设计

### 5.1 `/data` 布局

```
/data/
├── MANIFEST                    # 原子替换；catalog 快照 + 段/WAL 列表 + 校验和
├── wal/
│   └── wal-<generation>.log    # 当前代 WAL（也是 checkpoint 之后的数据源）
├── seg/
│   └── seg-<generation>-t<table_id>.dat   # checkpoint 产出的不可变段（仅活行）
├── idx/
│   └── t<table_id>-ann-<generation>.bin   # 可选：持久化 ANN（可重建）
└── tmp/                        # checkpoint/索引构建临时文件
```

- 每个正式用例使用独立 `/data`，程序启动时自动初始化，不依赖外部配置。
- 所有路径可由 `--data-dir` 指定，默认 `/data`。
- 文件名带 generation，禁止原地覆盖不可变段。

### 5.2 记录与 WAL 格式

统一记录头（小端）：

```
偏移  字段          说明
0     u32 payload_len
4     u32 crc32c       payload 的校验（含类型/头字段更稳妥，实现时定稿）
8     u8  type         1=INSERT 2=UPDATE 3=DELETE 4=COMMIT 5=CREATE_TABLE 6=CREATE_INDEX
9     u8  flags        预留
10    u16 reserved
12    u64 lsn          单调递增日志序列号
20    u64 txn_id       0 表示系统/自动提交也可用独立 id
28    u64 table_id
36    u64 pk
44    payload
```

行镜像（payload）：

```
按 schema 列顺序编码：
  BIGINT  : i64 小端
  VARCHAR : u32 byte_len + UTF-8 bytes（无 NUL 结尾）
  VECTOR  : dim × float32 小端（原始字节）
```

- 只存一份行镜像，**不做列式压缩**（先保证正确性；列式作为性能优化项，见 §10）。
- `UPDATE` 记录完整新行镜像（PK 不变）；`DELETE` 只带 PK；`COMMIT` 带 op_count、首末 LSN、commit_ts。
- DDL 记录携带序列化后的 schema / 索引定义。
- 每条记录长度上限（如 16 MiB）防御畸形输入；超限报错而不是崩溃。
- `crc32c` 用软件表或 SSE4.2 指令；校验失败视为尾部截断（恢复时停止重放）或损坏（报错）。

### 5.3 可见性：位图 + 事务 overlay

不实现通用 MVCC 快照，只做本题需要的"已提交 + 当前事务未提交覆盖"：

- 每个数据源（WAL 文件 / 段文件）维护 **live bitmap**：第 i 位 = 第 i 条记录是否属于当前已提交的活行。
- `PK map`：`pk → (source_id, ordinal)`，只指向最新已提交版本；用于点查、唯一性检查和去重。
- **事务 overlay**：
  - `pending[pk] = RowRef`（本事务新版本，位于 WAL 中尚未提交的区域）或 `DELETED`；
  - 事务内查询：基表 live 记录先按 overlay 替换/删除，再 union overlay 新增记录；
  - 提交：先落盘 COMMIT 标记并 fsync，再把 overlay 应用到位图与 PK map；
  - 回滚：丢弃 overlay + 截断本事务 WAL 区域；基表从未被修改，天然恢复。
- 这样 ROLLBACK 不需要 undo 日志，也不会出现"部分应用"。

**为什么不用页式 heap + undo**：本题单事务、数据量小，页式方案需要 buffer pool、脏页回刷、undo 链、torn page 处理，复杂度和 bug 面远大于收益。追加写方案把崩溃窗口收敛到"commit 标记是否完整"一个点。

### 5.4 语句原子性

- 每条语句开始时记录 `wal_offset_at_start` 和 overlay 快照。
- 语句执行期间新记录顺序追加；若中途校验/执行失败，回退 overlay，并把 WAL `ftruncate` 回语句起点（单线程，安全；截断前先丢弃/刷新写缓冲；崩溃时没有 COMMIT 标记，恢复会忽略）。
- 多行 INSERT 的任一行失败 → 整条语句不生效；事务内之前的语句仍有效。

### 5.5 Manifest 与 checkpoint（原子切换）

Manifest 采用自定义二进制 TLV（magic/version/payload_len/crc），内容：

```
generation, next_table_id, next_txn_id, catalog(表/列/索引定义),
segments[{path, table_id, rows, crc}],
active_wal{path, start_lsn},
checkpoint{last_commit_lsn}
```

Checkpoint 流程（同步，阻塞写入）：

1. 选择 generation+1，创建 `wal-<g+1>.log`（空）并 fsync。
2. 流式扫描当前 live 记录，按表写新段 `seg-<g+1>-t*.dat`（顺序写、大批量），fsync 每个段。
3. 写 `MANIFEST.tmp`（引用新段/新 WAL/新 generation），fsync。
4. `rename(MANIFEST.tmp, MANIFEST)`，fsync 目录 —— **原子提交点**。
5. 删除旧 generation 的段与 WAL（失败可延迟到下次启动清理）。

崩溃分析：

- 步骤 4 之前崩溃：旧 manifest 有效，新文件是孤儿，启动时按 generation 清理。
- 步骤 4 之后崩溃：新 manifest 有效，旧文件是孤儿，安全删除。
- 不会出现"manifest 指向未写完的段"：所有被引用文件先 fsync，再写 manifest。

**触发策略**：

- **只在安全点触发**：无活动事务、无未完成语句；有活动事务时先跳过，避免把 overlay 与快照混淆。
- 正常关闭时可选做一次 checkpoint（保证下次启动快）。
- WAL 超过阈值（如 128 MiB）或空闲时触发；**写入 benchmark 的 10k 行数据量远小于阈值，因此测量期间不会 checkpoint**（避免把整理成本计入 T）。
- 资源测试的大数据场景按阈值触发，避免 WAL 无限增长。

### 5.6 内存与超内存策略

| 结构 | 每行成本 | 10k 行 | 1000 万行 | 对策 |
| --- | --- | --- | --- | --- |
| PK map（开放寻址） | ~24 B | 240 KB | ~240 MB | 达到阈值切磁盘有序索引（见下） |
| live bitmap | 1 bit | 1.25 KB | 1.25 MB | 无需优化 |
| 事务 overlay | 仅改动行 | 小 | 大事务需控制 | 阈值 spill 到临时文件（P2） |
| 扫描缓冲 | 1–4 MiB | 固定 | 固定 | 有界 |
| ANN 索引 | ~(M×2×4+dim×4) B/行 | ~7 MB | 数 GB | 超预算则不建/退化为 exact |

- **禁止**全表载入：扫描按 1–4 MiB 块 `pread`，逐条解码，TopK 只保留 K 条。
- **PK map 退化路径**（P2）：维护磁盘上按 PK 排序的 `(pk, source, ordinal)` 稀疏索引 + 内存每 64 条一个锚点；点查二分 + 顺序扫；checkpoint 时重建。只有在实测大表内存超限时才启用。
- **页缓存也计入 M**：不做无意义的用户态全量缓存；依赖内核页缓存即可。必要时对超大顺序扫描使用 `posix_fadvise(DONTNEED)` 控制峰值（作为可调参数，测量后决定）。

### 5.7 持久化与 fsync 策略（合规性）

- 每个提交（autocommit 语句或 COMMIT）在返回成功前对 WAL 执行 `fdatasync`。
- 创建/rename 文件后对目录 `fsync`，保证目录项持久。
- **不采用"只 write 不 fsync"**：虽然题面崩溃模型只强杀进程、页缓存可存活，但题面明确"不能通过省略落盘换取分数"，且正式环境可能用更严格的检查；合规优先。
- 记录批量写：100 行 INSERT 合并为一次 `write` + 一次 `fdatasync`，减少系统调用与元数据开销。
- `fallocate` 预分配 WAL，减少文件扩展带来的元数据 I/O。

---

## 6. 事务设计

### 6.1 状态机

```
IDLE ──BEGIN──► ACTIVE ──COMMIT──► IDLE
                  │  ▲
                  │  └── 语句失败（仅回退该语句，事务仍 ACTIVE）
                  └──ROLLBACK──► IDLE
```

- `BEGIN` 时已在事务中 → 按题包错误（嵌套事务不支持）。
- 无显式事务时，每条数据修改语句自动提交（语句本身就是事务）。
- `COMMIT`/`ROLLBACK` 在 IDLE 时：按题包语义（MySQL 为 no-op；若题包要求报错则按报错）。⚠️ U 待确认。
- DDL 在事务中：默认**报"不支持在事务中执行 DDL"**，或按题包做隐式提交；绝不能静默改变事务边界。⚠️ U 待确认。

### 6.2 提交时序（关键）

```
1. 校验事务仍 ACTIVE
2. 把事务缓冲中的记录写入 WAL（先 flush 应用层写缓冲，再顺序 write）
3. 追加 COMMIT 记录（含 op_count/首末 LSN/commit_ts）
4. fdatasync(WAL)
5. 应用 overlay：更新 live bitmap、PK map；释放 overlay
6. 输出 COMMIT 成功
```

- 第 4 步返回前绝不输出成功。
- 第 4 步后、第 6 步前崩溃：恢复时 COMMIT 标记存在 → 完整生效；题面允许"提交请求未收到响应"的两种结果。
- 第 3 步只写了一半 → CRC/长度不合法 → 恢复视为未提交 → 完整回滚；不会部分生效。
- 第 5 步是纯内存操作，不涉及新的持久化。

### 6.3 读己写与一致性

- 事务内 SELECT / TopK：基表可见集 + overlay 合并。因为 TopK 可能在 exact/ann 两条路径上执行，必须共用同一个"可见行视图"抽象，避免"应用层扫描能看到、向量检索看不到"。
- 事务内 UPDATE 向量后立即 TopK：overlay 中是新向量，距离按新值计算。
- ROLLBACK 后：overlay 丢弃，基表位图未变，TopK 自然恢复；需专门用例验证"回滚前后结果集合与顺序完全一致"。

### 6.4 失败与边界

| 场景 | 期望 |
| --- | --- |
| 多行 INSERT 第 3 行 PK 冲突 | 整条语句 0 行生效，事务可继续 |
| UPDATE 中途向量解析失败 | 所有已改行回退，事务可继续 |
| COMMIT 时磁盘写失败 | 不输出成功；事务保持 ACTIVE 或按题包报错；不能假装成功 |
| 事务中 DDL | 报错/隐式提交（按题包） |
| Ctrl-D / SIGTERM 时有活动事务 | 回滚，然后 checkpoint/退出 |

---

## 7. 恢复设计

### 7.1 启动流程

```
1. 解析参数，确保 /data 存在
2. 读 MANIFEST（校验 magic/version/crc）
   - 不存在：初始化空 manifest（generation=1，空 catalog）
3. 打开并校验所有段文件（大小/crc/记录边界）
4. 按 manifest 顺序重放 active WAL：
   a. 顺序扫描有效记录（长度/CRC 校验）
   b. 第一遍：记录哪些 txn_id 拥有完整 COMMIT 记录
   c. 第二遍：按 LSN 顺序应用已提交事务的操作（幂等：以记录内容为准）
   d. 记录最后一个有效记录的结束偏移
5. ftruncate 掉尾部不完整记录（若存在），fsync
6. 重建/加载 ANN 索引（若 catalog 中有索引）；完成前不输出 ready
7. 输出 ready，进入会话循环
```

- 恢复期间**只读**，不写数据文件；使"恢复中再次被杀"天然幂等。
- 重放应用逻辑与正常运行提交后的应用逻辑复用同一函数，避免两套语义。
- 大 WAL 的重放也要流式（两遍扫描），不能把全部操作读进内存。

### 7.2 崩溃注入矩阵

| # | 注入点 | 期望重启后状态 |
| --- | --- | --- |
| C1 | INSERT 写 WAL 前 | 无该行 |
| C2 | WAL write 中途（半条记录） | 截断尾部，无该行 |
| C3 | WAL 写完、COMMIT 标记前 | 无该行（未提交） |
| C4 | COMMIT 标记写一半 | 无该行 |
| C5 | COMMIT 标记写完、fdatasync 前 | 可能无该行（允许） |
| C6 | fdatasync 返回、未输出成功 | 该行完整存在（允许全有） |
| C7 | 输出成功后 | 该行必须存在 |
| C8 | checkpoint 写新段中途 | 旧 manifest 有效，旧数据完整；孤儿清理 |
| C9 | checkpoint 写新 manifest 前 | 旧版本完整 |
| C10 | rename 后、删除旧文件前 | 新版本完整 |
| C11 | 恢复重放中途 | 再次重启仍得到相同状态 |
| C12 | 索引构建/加载中途 | 丢弃半成品，重建后再 ready |
| C13 | ROLLBACK 后、截断 WAL 前 | 未提交数据不可见 |
| C14 | 大事务写入大量行时 | 要么全有要么全无（按 COMMIT 标记） |

实现方式：`MVDB_CRASH_POINT=<name>[:delay]` 环境变量 + 测试脚本 `kill -9`；每个点重复 ≥20 次（不同随机延迟）。

### 7.3 不变量（测试断言）

1. 已输出 COMMIT 成功的事务，重启后所有修改可见。
2. 未输出 COMMIT 的事务，重启后没有任何修改可见（或完整可见，但不能部分）。
3. 主键唯一，无重复行。
4. 活行位图与 PK map 一一对应（双向一致）。
5. TopK 返回的每个 PK 都存在于当前可见集，且距离与当前行内容一致。
6. manifest 引用的文件都存在且 CRC 正确。
7. 任意时刻最多一个显式事务；恢复后事务状态为 IDLE。

---

## 8. SQL 层设计

### 8.1 语法范围（EBNF 草案）

```ebnf
statement      := create_table | create_index | insert | select | update | delete
                | "BEGIN" | "COMMIT" | "ROLLBACK" | "EXIT" | "QUIT" ;

create_table   := "CREATE" "TABLE" ident "(" column_def {"," column_def} ["," "PRIMARY" "KEY" "(" ident ")"] ")" ;
column_def     := ident type ;
type           := "BIGINT" | "VARCHAR" "(" integer ")" | "VECTOR" "(" integer ")" ;

create_index   := "CREATE" "VECTOR" "INDEX" ident "ON" ident "(" ident ")" ;

insert         := "INSERT" "INTO" ident ["(" ident {"," ident} ")"] "VALUES"
                  tuple {"," tuple} ;
tuple          := "(" literal {"," literal} ")" ;

select         := "SELECT" select_item {"," select_item}
                  ["FROM" ident]
                  ["WHERE" condition]
                  ["ORDER" "BY" order_item {"," order_item}]
                  ["LIMIT" integer] ;
select_item    := "*" | expr ["AS" ident] ;
expr           := column_ref | "L2_DISTANCE" "(" column_ref "," vector_literal ")" ;
condition      := comparison {"AND" comparison} ;
comparison     := column_ref op literal ;
op             := "=" | "<" | "<=" | ">" | ">=" ;

update         := "UPDATE" ident "SET" assignment {"," assignment} ["WHERE" condition] ;
delete         := "DELETE" "FROM" ident ["WHERE" condition] ;

order_item     := (ident | integer) ["ASC" | "DESC"] ;
literal        := integer | string | vector_literal ;
```

实现要点：

- 词法：大小写不敏感关键字；标识符 `[A-Za-z_][A-Za-z0-9_]*`，支持反引号；字符串用单引号，支持 `''`、`\'`、`\\`、`\n`、`\t` 等；支持 `-- `、`#`、`/* */` 注释。
- 语句分帧必须识别字符串/注释内的分号，不能简单 `split(';')`。
- 解析失败给出位置（行/列）与约定错误码；解析器要限制嵌套深度/语句长度，防止恶意输入导致栈溢出。
- 不支持的语法（OR、NOT、LIKE、子查询等）返回"不支持"错误，绝不猜测执行。

### 8.2 语义与执行

| 主题 | 规则 |
| --- | --- |
| 标识符大小写 | 按题包；默认表名大小写敏感、列名不敏感（MySQL 风格），内部统一规范化查找 |
| 投影 | 列引用、`*`、`L2_DISTANCE(...) AS alias`；`*` 按建表列顺序输出 |
| WHERE | 仅 AND 连接的比较；先过滤，再对命中集合计算距离；空集合返回空结果 |
| ORDER BY distance | 识别 alias 或完整表达式；`TopK` 必须"过滤后取 K" |
| ORDER BY 普通列 | 稳定排序；并列追加 PK 升序；支持 ASC/DESC |
| LIMIT | 非负整数；`LIMIT 0` 返回空；是否支持 `LIMIT offset,n` 按题包 |
| 无 ORDER BY | 顺序不承诺；测试按集合/多重集比较 |
| UPDATE/DELETE 无 WHERE | 作用于全表（若题包要求报错则改） |
| UPDATE 主键 | 报"不支持"错误 |
| 列数/类型不匹配 | 语句级错误，0 行生效 |

### 8.3 类型与校验（最容易丢分的细节）

- **BIGINT**：`from_chars` 严格解析有符号 64 位；溢出报错；拒绝空串/带单位/小数。
- **VARCHAR(n)**：UTF-8 解码后按 **Unicode 码点数**计算长度；超长报错；非法 UTF-8 的处理按题包（先严格拒绝，样例对拍后再定）。比较用字节序即可——合法 UTF-8 的字节序与码点序一致。
- **VECTOR(n)**：
  - 输入字面量先按 `[...]` 解析为浮点序列；
  - 维度必须等于 n；
  - 每个值必须有限（拒绝 NaN/±Inf）；
  - 转 float32 时检查溢出（|x| > FLT_MAX），拒绝；
  - 下溢/舍入允许；
  - 存储后按 float32 计算距离。
- **主键**：必须存在、BIGINT、不可为空、不可更新；全表唯一（含同一条多行 INSERT 内部）。
- **每表最多一个 VECTOR 列**；`CREATE TABLE` 时校验。
- 所有校验在**修改任何状态之前**完成（或依赖语句级回滚），避免"半条语句"。

### 8.4 错误码

- 建立 `ErrorCode` 枚举：语法错误、表已存在、表不存在、列不存在、列重复、类型错误、PK 冲突、PK 缺失、维度不符、向量非法、VARCHAR 超长、BIGINT 溢出、不支持的特性、事务状态错误、LIMIT 非法、索引已存在、索引列非法、资源/IO 错误。
- `docs/PROTOCOL.md` 维护"内部错误 → 题包错误码/文本"的映射表；题包未明确前先用稳定自定码；随预检/提交反馈迭代对齐，不在错误码上做不可逆耦合。
- 错误行格式由协议模块统一输出；绝不把错误写到 stdout 之外的地方（除 stderr 日志）。

### 8.5 输出格式

- 结果集：表头 + 行；分隔符/转义按题包（mysqltest 风格通常为 TAB）。
- BIGINT 十进制；VARCHAR 原样；距离按统一精度（先 `%.9g` 或题包要求），避免 `1.4142135` 与 `1.41421356` 的比对失败。
- DML 响应（影响行数）格式按题包。
- 每条语句后 `fflush(stdout)`；用 `write(1, ...)` 或确保 `sync_with_stdio(false)` 后手动 flush。
- 启动日志、调试、崩溃诊断一律 stderr；stdout 在 ready 之前不得有任何字节。

---

## 9. 向量检索设计

### 9.1 精确路径（必须永远可用）

- `L2_DISTANCE` 输出欧氏距离；内部用平方距离比较，最后对 TopK 结果开方。
- 距离计算：float32 输入，按固定顺序累加；提供标量/SSE/AVX2 三个内核，`__builtin_cpu_supports` 运行时派发；编译基线不假设 AVX2。
- TopK：
  - 维护大小为 K 的最大堆（或 K 很小的插入数组），比较键 `(dist, pk)`；
  - 只有 `dist < worst` 或 `dist == worst && pk < worst_pk` 才替换；
  - 结束后按 `(dist asc, pk asc)` 输出；
  - 不足 K 条返回全部。
- 过滤：
  - 先求值 WHERE（字符串等值用长度+memcmp），命中才计算距离；
  - 绝不做"全表 TopK 再过滤"；
  - 空命中集合直接返回空，不构建堆。
- 输出距离与存储值一致：更新后按新向量计算；回滚后按旧向量计算。

### 9.2 过滤与选择性

- 基准查询一半带普通列等值过滤。若过滤后集合很小，**直接暴力扫描命中集合**通常最快（无需 ANN）。
- 若过滤列基数低（如 category 只有 10 个值，命中 1000 行），扫描 1000 行 × 128 维成本很低。
- 后续可选优化（有测量依据才做）：
  - 等值过滤的 bitmap 索引（值 → 记录位图），DML 时增量维护；
  - 选择性估算（采样/基数统计）选择 "pre-filter scan" vs "ANN + filter"；
  - 过滤列字典编码，减少字符串比较。

### 9.3 ANN 选型与决策门

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| 精确 SIMD 扫描 | 结果 100% 正确；实现简单；无构建成本 | 每次查询 O(N·d) | **基线必做** |
| IVF-Flat | 构建快、内存低、实现中等 | 均匀分布数据 recall 提升有限；过滤查询不友好 | 备选 |
| HNSW | 查询快、recall 高、10k 规模构建可接受 | 实现复杂（图、删除、过滤）；内存较高 | **性能阶段首选** |
| 树/量化（PQ） | 内存小 | 精度/实现成本高 | 不做 |

**硬性决策门**：只有在以下条件同时满足时才把提交配置切到 `ann`：

1. 在公开分布 + ≥5 个随机种子上，有过滤/无过滤两组 Recall@10 均 ≥ 0.98（留 0.03 余量）。
2. 相对 exact 基线，本地测得 E 提升 ≥ 10%（按 §10 公式）。
3. 功能测试（索引维护、事务、回滚）在 ann 模式下全通过。
4. 索引构建时间/内存不超过题包准备上限。

否则 ann 模式内部走精确扫描（正确性优先，性能损失可接受）。

### 9.4 索引对象与维护

- **exact 模式永远走扫描，完全忽略索引**：即使表上已有 ANN 索引，exact 查询也必须返回精确 TopK。
- `CREATE VECTOR INDEX idx ON t(v)`：
  - 校验表、列、维度、是否已有索引；
  - catalog 记录索引定义（名字、表、列、参数、generation）；
  - 构建内存索引（P2），完成前 DDL 不返回成功；
  - 建索引期间的读请求阻塞（单线程，天然串行）。
- **索引是派生结构**：以已提交 live 行为准构建；崩溃/重启后若 catalog 有索引，则在 ready 前重建（或加载校验通过的持久化快照）。
- 事务可见性：
  - 已提交数据：走索引；
  - 事务内未提交的 INSERT/UPDATE：overlay 单独暴力计算，再与索引结果归并；
  - 事务内 DELETE：从索引结果中过滤掉；
  - 这样"事务内读己写"与"回滚恢复"都不依赖索引回滚。
- 索引结果必须二次校验：PK 存在、未删除、满足过滤、无重复；不足 K 条时用精确扫描补齐（保证结果条数与正确性；recall 仍由真实近邻决定）。

### 9.5 Recall 验证

- 生成器：公开分布（均匀/聚类两套）、多种子；查询向量同分布 + 边界（全 0、重复点、极端值）。
- 真值：Python/暴力 C++ 计算 `(dist, pk)` TopK。
- 指标：`Recall@10 = |近似结果 ∩ 真值| / min(10, N_filtered)`；空集合不计入平均。
- 扫描参数曲线：HNSW `efSearch ∈ {16,32,64,128,256}`；IVF `nprobe`；记录 recall 与 P50/P99 延迟，选"recall ≥ 0.98 且延迟最低"的点。
- 过滤场景单独统计：过滤后 N 从 1 到 N 全范围，验证阈值切换逻辑。

### 9.6 持久化索引（可选）

- 文件格式：magic/version/table_id/generation/data_version/entry_count/CRC + 图/簇数据。
- 加载时校验数据版本（如 checkpoint generation + live 行数 + 校验和）；不匹配则删除并重建。
- 构建时写 `.tmp` 再 rename，避免半成品被加载。
- 若准备时间上限紧张，优先"不持久化、启动重建"（10k 规模构建约百毫秒到秒级），把复杂度留给正确性。

---

## 10. 性能工程

### 10.1 指标模型（题面公式）

对写入、查询分别：

```
R_T = (T_base + 1ms) / (T + 1ms)
R_B = (B_base + 1MiB) / (B + 1MiB)
R_M = (M_base + 1MiB) / (M + 1MiB)
f(R) = R / (1 + R)
E_item = 0.6·f(R_T) + 0.2·f(R_B) + 0.2·f(R_M)
E = (E_write + 2·E_query) / 3
```

- 三轮取中位数，保留 6 位小数，按 E 排名；查询权重是写入的 2 倍。
- 正确性/持久化/资源任一失败 → 性能 0 且不占名次；Recall@10 两组均需 ≥ 0.95。
- B 包含启动、装载、索引准备、预热到测量结束及写回；M 包含页缓存。

**推论**：

1. 查询优化优先于写入优化（权重 2:1）。
2. 索引构建的内存/IO 会进 B/M，不能无限堆资源。
3. 启动与恢复也计入 B/M；启动要快、少读无用数据。
4. 不要把数据写到 `/data` 之外规避统计（违规且平台会检查）。

**合规红线**（任何一条都可能被判定无效）：

- 不针对隐藏随机种子预存/预计算答案；数据分布公开不等于答案公开。
- 不把数据、日志、索引、临时文件放到未计量目录或外部服务。
- 不省略提交落盘；不以"杀进程后页缓存还在"为理由跳过 fsync。
- 不调用现成数据库引擎/向量索引库；不加载网络资源。
- 不修改/绕过测试驱动、不伪造结果输出。

### 10.2 写路径优化清单（按收益排序）

| 优化 | 预期收益 | 风险/备注 |
| --- | --- | --- |
| 每批 100 行合并为一次 write + 一次 fdatasync | 高（系统调用/元数据） | 必须保持提交语义 |
| 顺序追加、预分配 WAL | 中 | 注意文件扩展与 fsync 目录 |
| 行编码零拷贝/一次成型 | 中 | 避免每行多次分配 |
| 避免逐行 CRC 的重复计算 | 低 | CRC 一次算完 |
| 批量解析多行 INSERT | 中 | 解析器复用缓冲 |
| 关闭无关日志（Release） | 低 | stderr 仍保留错误 |
| 事务内多条语句合并提交 | 高（若测试允许） | 测试按批次提交，收益有限 |

### 10.3 读路径优化清单

| 优化 | 预期收益 | 风险/备注 |
| --- | --- | --- |
| AVX2/FMA 距离内核 + 运行时派发 | 高（4–8×） | 需校验数值稳定性/并列 |
| 过滤先于距离计算 | 高（过滤查询） | 必须语义正确 |
| 顺序大块 `pread` + 解析偏移预计算 | 高 | 避免逐条 syscall |
| TopK 小顶堆/插入数组 | 中 | K=10 时插入数组可能更快 |
| 行镜像中向量前置、字符串后置 | 中 | 只需向量时少读/少解码 |
| 列式向量 sidecar（可选） | 高（内容列很长时） | 增加写入复杂度/一致性风险 |
| 过滤 bitmap 索引（可选） | 中 | 维护成本、内存 |
| HNSW（ann 模式） | 高 | recall/构建成本/复杂度 |
| 预取/软件流水 | 低–中 | 先测量再上 |

### 10.4 内存与 I/O 权衡

- 10k×128 的向量数据仅 5.12 MB，页缓存足够；**不要**再复制一份到用户态（M 会翻倍）。
- B 指标只统计设备读写；重复读缓存不增加 B，但增加 T；顺序访问友好。
- M 包含页缓存：大表扫描会让 page cache 增长，必要时用 `fadvise` 控制；但要权衡 T。
- 索引内存：HNSW 约 `N·(dim·4 + M·2·4 + 常数)`；10k 约 7–10 MB；大表超预算则不建。
- 避免 `mmap` 整个大文件（地址空间/缺页/统计不可控）；用 `pread` 有界缓冲。

### 10.5 本地 benchmark 方法

1. `tools/gen_data.py`：按公开分布生成 10k 行、128 维、带 category/content 的 SQL 批文件（每批 100 行）与 1000 条查询（一半带过滤）。
2. `tools/bench.py`：
   - 启动容器/进程，记录启动到 ready 时间；
   - 发送写入批次，记录端到端墙钟；
   - 建索引、预热；
   - 发送查询，记录端到端墙钟；
   - 从 cgroup v2 `memory.peak` / `io.stat` 读取 M/B（本地用 `systemd-run --scope` 或 docker `--cgroup-parent` 近似）。
3. 每个配置跑 3 轮，取中位数；同时跑正确性校验（重启后数据、Recall）。
4. 对比矩阵：exact/scalar、exact/SIMD、exact/SIMD+filter、HNSW(ef 扫描)、IVF(nprobe 扫描)。

### 10.6 阶段目标（假设值，实测后修正）

| 指标 | MVP 目标 | 优化目标 |
| --- | --- | --- |
| 启动到 ready（10k 行） | < 300 ms | < 150 ms |
| 写入 10k 行（100 批） | < 1.5 s | < 0.8 s |
| 1000 次 exact 查询 | < 1.5 s | < 0.6 s |
| 1000 次 ann 查询（recall ≥ 0.98） | — | < 0.25 s |
| 峰值内存 | < 128 MiB | < 64 MiB |
| 写入 I/O | < 30 MiB | < 15 MiB |

> 目标只用于判断"是否值得继续优化"，最终以平台基线与三轮中位数排名为准。

---

## 11. 测试与质量

### 11.1 测试金字塔

| 层级 | 内容 | 工具 | 门禁 |
| --- | --- | --- | --- |
| 单元 | lexer/parser、类型校验、CRC、bitmap、记录编解码、距离内核 | C++ 自带断言/轻量测试 main | 每次提交 |
| SQL 用例 | 每类语义正/反用例 + 期望输出 | 自建 mysqltest 风格 runner | 每次提交 |
| 差分 | 随机 DML/事务/查询 vs Python 真值模型 | Python 脚本 | 每日 |
| 崩溃 | §7.2 注入矩阵 | 环境变量 + kill -9 | 每日/发版前 |
| 资源 | cgroup 限额、大数据、磁盘满 | docker/systemd-run | 发版前 |
| 性能 | 3 轮 benchmark + recall | bench.py | 性能阶段 |
| 协议 | 原始 stdin/stdout 字节级用例、stdout 纯净 | driver.py | 每次提交 |
| 容器 | 离线构建、启动、重启、强杀 | Docker | 发版前 |

### 11.2 差分测试（发现语义 bug 的主力）

- Python 模型：`tables: dict[pk → row]` + 事务快照；TopK 用 double 真值；支持与产品相同的 SQL 子集。
- 随机序列：建表 → 若干 INSERT/UPDATE/DELETE/SELECT/事务（含失败语句、回滚）→ 重启 → 继续。
- 比较：结果集按 `(dist, pk)` 或集合比较；错误码；表内容快照；重启前后一致。
- 至少 500 个随机种子；失败用例最小化后固化为回归用例。

### 11.3 崩溃注入

- 崩点通过 `MVDB_CRASH_POINT` 编译进测试构建（Release 不启用，避免影响性能）。
- 脚本对每个崩点：启动 → 执行到崩点 → `kill -9` → 重启 → 校验不变量 → 记录结果。
- 额外做"恢复中再次强杀"（连续 3 次）与"checkpoint 中强杀"。
- 用 `fsync` 调用计数/日志确认提交屏障生效（仅测试构建）。

### 11.4 资源与容器

- 用 docker `--memory=256m --cpus=2` + cgroup 统计复现正式限制。
- 大数据用例：1M/5M 行、128 维，验证插入、点查、TopK、重启、恢复不 OOM；监控 `memory.peak`。
- 磁盘限额：`--storage-opt size` 或 loop 文件系统；验证写满时报错而不崩溃/不丢已提交数据。
- 离线构建：`docker build --network=none`（依赖已 COPY/vendor）。
- 启动超时/查询超时：用脚本注入慢查询验证不会挂死。

### 11.5 常见坑位清单（测试必须覆盖）

- [ ] 多行 INSERT 中间失败
- [ ] 主键重复（含同语句内）
- [ ] VARCHAR 恰好 n 个码点 / n+1 / emoji / 中文
- [ ] 向量维度不符、NaN、Inf、1e39、`-0`、空数组、多余逗号
- [ ] 距离并列（构造等距向量）
- [ ] 过滤后不足 K 条
- [ ] 空表 / 空结果集
- [ ] ORDER BY 普通列 + LIMIT
- [ ] 事务内读己写（插入、更新、删除三种）
- [ ] 事务回滚后 TopK 恢复
- [ ] 语句失败后事务继续并提交
- [ ] 重启后索引（若持久化）与数据一致
- [ ] 强杀时 stdout 不残留半条结果
- [ ] 错误语句后会话继续可用
- [ ] 浮点输出精度/格式
- [ ] stdout 无日志污染
- [ ] `/data` 之外无写入

### 11.6 CI 门禁

`scripts/check.sh` 顺序执行并短路：

```
build (Release) → unit → sql suite → differential(100 seeds) → crash(抽样) → protocol → stdout purity
```

任何一步失败即停止，输出可复现命令。正式提交前跑 `check.sh --full`（500 seeds + 全崩点 + 容器）。

---

## 12. 交付物设计

### 12.1 Dockerfile

- 多阶段：
  - `build` 阶段：`ubuntu:24.04`（固定 digest）+ 安装 `g++`/`cmake`/`ninja`（apt 版本锁定），`COPY` 源码，CMake 构建；
  - `runtime` 阶段：仅拷贝二进制与必要运行库，`mkdir /data`，`VOLUME ["/data"]`；
  - `ENTRYPOINT ["/app/minivectordb"]`，`CMD ["--data-dir","/data"]`。
- 构建阶段可联网（安装编译工具），但依赖版本要锁定、可复现；**运行阶段禁止联网**，程序不得发起任何网络调用。
- 镜像体积控制在 ~100 MB 内（本地磁盘紧张）。
- 验证：`docker build` 一次成功 → `docker run --network=none --rm -i -v ... --cpus=2 --memory=2g` 跑通全流程 → `docker kill` 后重启校验持久化。

### 12.2 README / 运行说明（题面 4.2 必须）

- 语言与版本、构建方式、启动入口、`/data` 说明、SQL 支持范围、错误处理。
- 从空目录开始的完整示例：建表 → 写入 → 普通查询 → 向量 TopK → COMMIT → ROLLBACK → 重启。
- 容器运行命令与 stdin/stdout 协议说明；stderr 日志说明。
- 已知限制（不支持特性）。

### 12.3 设计文档（题面 4.3）

- 记录布局、WAL/manifest、checkpoint、恢复流程。
- 向量索引方案与 recall 数据。
- 事务提交时序、崩溃不变量。
- 已实现功能清单 + 已知限制 + 本地测试记录。

### 12.4 `conversation.json` 证据工程（40 分，独立排名）

**关键事实**：评审只读这一个 JSON（≤128 KiB、UTF-8、顶层对象、无重复键/NaN）；只认学生自身有证据的理解与验证；Agent 的优秀输出不自动算学生理解；不能编造。

**先区分两类保存对象（容易混淆）**：

| 对象 | 内容 | 用途 | 存放 |
| --- | --- | --- | --- |
| 平台反馈 | 预检/正式提交返回的失败类别、日志原文、提交时间、SHA、镜像 digest | 校准协议、定位隐藏失败 | `docs/SUBMISSIONS.md` + `agent/evidence/platform/` |
| 学习材料 | 学生真实 prompt、Agent 对话、学生自己的解释、实验命令与输出、修正复测、反思 | `conversation.json` 评分（40 分） | `agent/evidence/` + `agent/conversation.json` |

**只保存提示词是不够的**：评分要 `student_evidence`（学生自己的话）和 `result_evidence`（实际结果）分开引用，且要覆盖"理解、验证、修正、迁移、取舍"五类证据；单纯堆提示词/对话数量、篇幅、调用次数都不加分。提示词要保存，但更重要的是保存**你基于结果做出的判断、修正和复测**。

**真实性与分工（硬规则）**：

- **不得由 Agent 编造**学生 prompt、学生理解、实验命令/输出或平台结果；`conversation.json` 必须对应真实发生的交互和可复现实验。
- 平台反馈由学生**原样**贴给 Agent；Agent 只做归档、截取、脱敏、定位，不改写结果。
- Agent **可以**：根据真实记录生成 JSON 结构、把长文压缩成 ≤240 字符的引用、校验 JSON pointer、翻译/润色（不改变事实）、起草"需要学生回答的问题"。
- 学生**必须提供**：每个证据单元的判断/解释/取舍（可以很短、很口语），并确认改写后的引用没有改变原意。
- 如果不想用原始 prompt：可以脱敏、改措辞、用摘要，但必须由学生确认"这确实是我当时问的/想的"；**不能凭空生成一段"学生 prompt"**。更稳的做法是：以后每个工作阶段开始前，由 Agent 起草 prompt、学生确认并实际发出，这样它就是真实 prompt。
- 只交提示词、没有学生自己的结果解释，通常最多落在 0.5 档（只有规划/提问）；编造材料会被判无效，风险远大于收益。

**每次提交前的 10 分钟证据采集（已确认流程）**：

> 流程：**Agent 起草问题 → 学生回答 → Agent 整理成稿 → 学生确认 → 写入文件**。整理稿不使用学生原话，但不得改变事实、不得添加学生没说过的结论。

1. 每个工作阶段结束时，Agent 根据本次实际发生的事起草 3–5 个问题（只问这次真正做过的），学生可增删；**问题清单先给学生确认**。
2. 学生用中文简短回答（一两条要点即可，不需要长文）。
3. Agent 把回答整理成第一人称的 `student_evidence` 草稿（书面化、去口语、保留具体判断/数字/取舍），`result_evidence` 则保持命令输出/平台反馈原样，不改写。
4. 把整理稿发给学生**逐条确认**；只有学生明确回复"确认"（或指出修改并再确认）后，才写入 `agent/evidence/journal/YYYY-MM-DD.md`，并标注 `confirmed: true` 与时间。
5. Agent 再从确认稿拆出 `student_evidence` / `result_evidence`，组装 `conversation.json`；未确认的内容一律不写入，标 `未确认` 并排除。
6. 如果当天没有新理解/新结果，就写"本班无有效新证据"，不硬凑；`material_notes` 如实说明"部分学生表述经本人确认后整理"。
7. 整理稿必须保留**具体动作、数字、判断和取舍**，避免"我理解了/我验证了"这类泛泛表述；否则即使确认了，也会像 Agent 文本，拿不到分。可在每个证据单元加 `confirmed: true` 与确认时间（不是隐藏思维链）。

**问题模板（按本次实际内容裁剪）**：

1. 这次改了什么？为什么先做它？
2. 你预测会发生什么？实际结果是什么（贴平台反馈/命令输出）？
3. 结果和预测不一致时，你怎么解释？改了哪里？
4. 改完复测结果如何？
5. 这个结论的适用边界/局限是什么？

**日常流程（从今天开始）**：

1. 每个实验在 `agent/evidence/` 留原始记录：问题、假设、命令、原始输出（截取关键行）、学生回答、确认稿、下一步。
2. **每次提交前**把截至当前的有效单元整理成候选：对应五维度之一，写清 `student_evidence` 与 `result_evidence`（不同片段）。
3. 提交前用 `tools/build_conversation.py` 组装、校验 JSON pointer 可解析、原文可在 JSON 中定位、总大小 <128 KiB。
4. 去重：同一条修正链只取最高档；不把"计划"写成"已验证"；**不编造学生没说过的事实或结论**，整理稿必须经学生确认。
5. **材料随提交锁定**：每次正式提交的 `conversation.json` 不能事后修改。因此：
   - 早期版本也要保持材料真实、完整（不必等"完美"）；
   - 计划在 S18/S20 用"**代码与 best-known-good 完全一致 + 材料更新**"的新 commit，把"最好的代码"和"最成熟的证据"组合到同一次提交；
   - 若最终只能选早期通过版本，其材料质量就是当时的水平，不能回填——所以每天都要维护，而不是最后补。

**候选证据单元（需真实执行并记录）**：

| # | 单元 | 维度 | 证据要点 |
| --- | --- | --- | --- |
| E1 | 过滤 TopK 必须先过滤再取 K | 问题理解/验证 | 构造"全局最近但被过滤"反例 → 观察错误结果 → 修正 → 复测 |
| E2 | commit 前 fsync 的必要性 | 技术理解/取舍 | 假设 write 即持久 → kill -9 实验 → 修正屏障 → 复测 |
| E3 | 距离并列按 PK 升序 | 独立验证 | 构造等距向量 → 发现顺序不稳 → 修正比较器 → 复测 |
| E4 | Recall 与 efSearch/nprobe 的取舍 | 实验迭代/取舍 | 扫参数曲线 → 解释延迟/召回代价 → 选配置 → 复测 |
| E5 | VARCHAR 长度按码点而非字节 | 技术理解 | 中文/emoji 用例 → 发现字节长度 bug → 修正 → 复测 |
| E6 | 超内存数据的流式 TopK | 实验迭代 | 构造大表 → 观察 memory.peak → 改流式 → 复测 |
| E7 | 崩溃窗口与部分提交 | 独立验证 | 注入 kill 点 → 检查部分生效 → 修正提交顺序 → 复测 |
| E8 | 事务内读己写与回滚后的检索一致性 | 技术迁移 | 双路径（扫描/TopK）对拍 → 发现不一致 → 修正 → 复测 |
| E9 | 预过滤 vs 后过滤的选择性阈值 | 取舍/反思 | 测量不同基数下的两种方案 → 给出阈值与局限 |
| E10 | stdout 协议污染导致驱动失败 | 验证/修正 | 日志误入 stdout → 驱动失败 → 修正 → 复测 |
| E11 | 协议 profile 的取舍与验证 | 验证/反思 | 记录预检/提交反馈 → 切换 profile → 复测（若平台反馈可用） |
| E12 | 提交门禁漏项的复盘 | 取舍/反思 | 一次门禁失败 → 定位 → 补自动化检查 → 复测 |

**JSON 结构草案**（字段名可调整，但证据必须在字符串字段内）：

```json
{
  "schema_version": 1,
  "synthetic": false,
  "material_notes": "真实开发对话摘录；已脱敏；时间范围…",
  "user_prompts": [
    {"id": "P1", "content": "学生原始提示词…"}
  ],
  "conversations": [
    {"id": "C1", "role": "student", "content": "我对过滤 TopK 的理解是…", "ref": "P1"}
  ],
  "experiments": [
    {
      "id": "E1",
      "hypothesis": "先全局 TopK 再过滤会漏掉正确结果",
      "command": "…",
      "output": "实际输出片段…",
      "analysis": "学生自己的解释…",
      "follow_up": "修正后复测输出…"
    }
  ],
  "decisions": [
    {"id": "D1", "decision": "…", "tradeoff": "…", "evidence": ["E4", "E9"]}
  ],
  "retrospective": "学生复盘…",
  "limitations": ["…"]
}
```

**注意**：

- 根目录现有 0 字节 `conversation.json` 是脚手架占位；正式材料默认放 `agent/conversation.json`。为避免入口差异，提交时在两处放**完全相同**的内容（同 commit、同摘要）；若平台明确只读其一，另一处保留同内容也不会造成歧义。⚠️
- 不提交密钥/token；路径不含用户名等敏感信息；提交前用脚本扫描 `github_pat_`、`ghp_`、`Bearer ` 等模式。
- 材料必须对应同一 commit，不能事后替换；"代码不变、只更新材料"的提交必须是一个**新 commit**，并在台账注明 `code_diff = none`。
- 合成材料必须标 `synthetic: true`；本项目使用真实开发证据，标 `false`。

### 12.5 仓库与分支状态（已就位，持续校验）

**当前状态（2026-11-09 12:34 验证）**：

- 官方 `oceanbase/seekdb` **本来就有** `2026-competition-round2` 分支，tip 是 `c842a368c...`；提交用的 fork `Alorun/seekdb-answer` 之前缺这个分支。
- fork 已转为 **public**；同名分支已创建并推送，指向与官方完全相同的 commit `c842a368c...`；两边 `git ls-remote` 返回同一 SHA。
- **无凭据** `git ls-remote` 能拿到分支 SHA；**无凭据** `git clone --depth 1 --branch 2026-competition-round2` 成功（约 536 KB，37 个脚手架文件）。
- HTTPS PAT 当前是**只读**（`git push` 返回 403）；推送改用 **SSH**（身份 `25ALN`，已验证可写），本分支 upstream 已设为 `git@github.com:Alorun/seekdb-answer.git`。
- API 显示 `fork: false`：内容是官方仓库的克隆，不是 GitHub "Fork" 关系；平台通常只认 URL + 分支 + SHA，若组委会明确要求 fork 关系，再重新 Fork 官方仓库并推分支。

**官方分支 vs GitHub fork 关系（重要）**：

- 官方 `oceanbase/seekdb` 的 `2026-competition-round2` 存在，tip = `c842a368c`；我们当前分支也指向同一个 commit，**开发基点是官方 round2，不是另起炉灶**。
- 但 `Alorun/seekdb-answer` 的 API 显示 `fork: false`（它是克隆/导入，不是 GitHub Fork 关系）。赛题原文只要求"使用 seekdb 仓库的 `2026-competition-round2` 分支"并提交 URL/分支/SHA，**没有明文要求必须是 GitHub Fork**；但实际是否校验 fork 关系未知，不能把"大概率不查"当成确定结论。
- **风险**：如果平台或人工审核默认"必须是官方 fork"，非 fork 仓库可能被质疑。最稳做法是做一个**真正的 GitHub Fork**，把同一分支推过去，提交新 fork 的 URL/SHA；旧仓库保留作备份。
- 迁移步骤：
  1. 打开 `https://github.com/oceanbase/seekdb` → Fork → 选择自己的账号（如 `25ALN`）→ 仓库设为 public；
  2. 确认新 fork 里有 `2026-competition-round2`（没有就从官方 fetch 后 push；或直接把本地分支 push 上去）；
  3. 本地加 remote（如 `fork`），推 `2026-competition-round2`；不要 force-push；
  4. 用新 fork 的 HTTPS URL + 分支 + SHA 提交，重新做匿名 `ls-remote`/clone 验证。
- 如果不想换仓库：先向组委会确认"公开克隆仓库 + 同名分支"是否接受；得到明确答复前，不要假设一定合规。

**每班维护规则**：

1. **推送走 SSH**，不要在提交 URL 里放 token；`git push` 后确认远端分支指向新 SHA。
2. **永不 force-push** 已提交历史；所有提交只追加。
3. 每次推送后做一次**匿名校验**（模拟平台）：
   ```bash
   git ls-remote https://github.com/Alorun/seekdb-answer.git refs/heads/2026-competition-round2
   ```
   返回的 SHA 必须等于本次提交 SHA；必要时用临时目录 `git clone --depth 1 --branch ...` 再验一次。
4. 仓库必须保持 public；若可见性被改回 private，平台会拉取失败——发现后立即修复并在台账记录。
5. 每次提交打 tag `submit/YYYYMMDD-a` 或 `submit/YYYYMMDD-b`，保留 SHA/镜像 digest/材料摘要。
6. 提交给平台的 URL 只写 `https://github.com/Alorun/seekdb-answer.git`，不带任何凭据。
7. 若平台实际要求私有仓库或特定 fork 关系，先与组委会确认，再按确认结果调整；不要用"本机能拉"代替"平台能拉"。

---

## 13. 排期与提交节奏（2026-11-09 → 11-18，每日双提交）

> 原则：**先纵切、再补全、后优化；每天两班发布列车，每班都有完整合规包和明确假设；任何时候都保留一个已知通过版本兜底。**

### 13.1 总体节奏

- 每天计划两个提交班次（S-A / S-B），**具体时刻自定**（平台上限为每天 2 次）；建议两班之间留出结果回流时间（例如午后一班、晚间一班），但不强制。11/18 截止日尽量提前提交，至少留 4 小时缓冲，避免平台拥堵或提前截止。
- **每班提交的内容都必须通过 §13.3 发布门禁**；不合格的候选一律不上。
- 每班先试**主候选**（新代码）。若主候选未过门禁，立即改发**备用候选**：best-known-good 代码不变 + 更新 `conversation.json`/文档的新 commit（`git diff` 必须为空），备用候选同样要过门禁。这样既尽量保证每日两次，又不引入回归，还能提升 prompt 材料成熟度。
- 最多 20 次正式提交（11/09 两次 + 11/10–11/17 十六次 + 11/18 两次）。额度不结转；**唯一允许少发的情况是首日极早期连一个合规包都没有**——此时宁可少发一班，也不发明知不合规的包。
- 早期提交允许"功能未全覆盖"：目标是换回失败类别/协议信息；但交付物、构建、启动、材料、流程必须合规。

### 13.2 逐日双提交列车

| # | 日期/班次 | 版本目标（相对上一版） | 必须通过 | 预期/用途 |
| --- | --- | --- | --- | --- |
| S1 | 11/09 第 1 班 | v0.1 垂直切片：CMake/Docker、session/协议 profile、CREATE TABLE、多行 INSERT、简单 SELECT、PK map、WAL/manifest、重启 | 门禁 A（构建/启动/协议自测/材料） | 校准协议与输出格式；预计隐藏用例大量失败 |
| S2 | 11/09 第 2 班 | v0.2 加 UPDATE/DELETE/BEGIN/COMMIT、类型校验、错误表；吸收 S1 反馈。若 S1 无可判读反馈，则改为"代码不变 + 协议 profile/ready marker 对照版" | A + 基础功能自测 | 拿到第一轮失败类别；确认协议或事务/持久化路径 |
| S3 | 11/10 第 1 班 | v0.3 完整 SQL：全部比较/AND/ORDER BY/LIMIT、别名、语句原子性、重启一致 | A + SQL 正反用例 | 修 S1/S2 失败；验证协议 profile 是否需切换 |
| S4 | 11/10 第 2 班 | v0.4 向量：L2、精确 TopK、过滤 TopK、并列、事务读己写；差分 100 seeds | A + 差分/向量用例 | 功能主链路完整；记录 recall=1.0 基线 |
| S5 | 11/11 第 1 班 | v0.5 恢复：崩点矩阵子集、checkpoint、尾部截断、恢复幂等 | A + 崩溃用例 | 把"部分提交"风险压下去 |
| S6 | 11/11 第 2 班 | v0.6 恢复加固 + 资源流式（1M 行快速验证）+ 错误路径 | A + 全崩点 + 资源快速 | 争取第一次本地全绿 |
| S7 | 11/12 第 1 班 | v0.7 Docker 离线运行、协议/错误格式调优、资源限额复测 | A + 全量本地套件 | **力争首次全用例通过**；若通过，锁定 best-known-good |
| S8 | 11/12 第 2 班 | v0.8 CREATE VECTOR INDEX + 索引维护 + ann 精确回退 | A + 索引/ann 用例 | 功能面收口；ann 暂不押注 ANN 质量 |
| S9 | 11/13 第 1 班 | v0.9 bench 脚本 + SIMD 距离内核 + 首轮 E 测量 | A + 性能正确性校验 | 建立性能基线与回归阈值 |
| S10 | 11/13 第 2 班 | v0.10 过滤下推、批量 pread、写路径合并、启动优化 | A + 性能不回归 | E 提升第一轮；保留 exact 回退 |
| S11 | 11/14 第 1 班 | v0.11 HNSW 构建 + recall 扫描（决策门） | A + recall ≥0.98（多种子） | 只有达标才进入 ann 配置 |
| S12 | 11/14 第 2 班 | v0.12 过滤 ANN/暴力切换阈值、索引持久化（可选）、模式选择 | A + 索引维护/事务/回滚 | 确定提交配置走 exact 还是 ann |
| S13 | 11/15 第 1 班 | v0.13 性能第二轮：内存峰值、I/O、fsync 批量、启动/恢复 | A + 三轮稳定 | 争取 E 再提升；记录 B/M |
| S14 | 11/15 第 2 班 | v0.14 资源极限：256 MiB、1M/5M 行、磁盘写满、恢复中再杀 | A + 资源全项 | 消除超内存/恢复类隐藏失败 |
| S15 | 11/16 第 1 班 | v0.15 全量回归 + 模糊测试 + 代码审查 + 缺陷修复 | A + `check.sh --full` | 只修不增；准备冻结 |
| S16 | 11/16 第 2 班 | v0.16 README/设计文档/测试记录 + `conversation.json` 完整草稿 | A + 材料合规 | **功能冻结**；之后只修 bug/文档 |
| S17 | 11/17 第 1 班 | v0.17 最终候选：仅必要修复；性能/恢复复核 | A + 全量门禁 + 预检 | 冲刺最终版本 |
| S18 | 11/17 第 2 班 | v0.18 备份候选：best-known-good 代码不变 + 证据更新，或经验证的小修复 | A + 全量门禁 | 确保有一个"代码已验证 + 材料最新"的候选 |
| S19 | 11/18 第 1 班 | 最终参评候选（best-known-good 或更优且已验证） | A + 预检 | 截止日第一班，优先安全 |
| S20 | 11/18 第 2 班 | 备用：代码不变 + 最终证据/文档；或 S19 反馈后的必要修复 | A + 预检 | 最后一班，不做高风险变更 |

> 日期与班次可按实际结果回流调整，但**每日计划两班、每班合规、保留兜底**三条不变。若某天结果返回很慢，下一班优先"修已知失败类别"，不盲目堆新功能。

**20 次额度预算（动态调整）**

| 用途 | 预算次数 | 说明 |
| --- | ---: | --- |
| 协议校准 | 2–4 | 只改一个协议变量；预检优先，正式提交仅在必要时 |
| 功能主线（SQL/存储/事务/恢复） | 8–10 | 早期允许未全覆盖，但每班要有明确增量 |
| 索引/ANN/性能 | 4–5 | 有测量依据才上；失败可回退 exact |
| 材料/文档/最终组合 | 2–3 | 代码不变 + 证据更新的组合候选 |
| 备用/应急 | 1–2 | 截止日拥堵、临时缺陷 |

> 预算不是硬上限：若协议迟迟不匹配，优先追加协议校准；若功能已全绿，把功能额度转给性能与材料。

### 13.3 发布门禁（每班必过）

| 门 | 检查 | 失败处理 |
| --- | --- | --- |
| G1 版本/分支/可获取性 | 工作在 `2026-competition-round2`；commit 为 40 位 SHA 且可从远端获取；**未认证 `git ls-remote` 能成功**（平台可拉取）；**不 force-push 已提交历史**；打 tag `submit/YYYYMMDD-a` 或 `submit/YYYYMMDD-b` | 修正后重来；已提交 SHA 永久保留；仓库私有则先解决可见性 |
| G2 干净构建 | 从 `git archive` 的干净源码 `docker build --no-cache` 成功；运行阶段无网络；镜像 <150 MB | 补文件/修 Dockerfile；不得用本地预编译产物 |
| G3 启动/协议 | ready 在超时内；ready 前 stdout 为空；smoke SQL 全过；stdout 无日志；每条语句 flush；无监听端口 | 修协议/启动；不得提交 |
| G4 功能回归 | 已实现范围的本地用例通过、无已知 Blocker、语句原子性通过；未实现功能在台账中显式标注 | 修复或降级功能；不提交"已实现范围内带 Blocker"的版本 |
| G5 持久化/恢复 | 重启一致；kill -9 基础矩阵；若本版改过存储/恢复，跑全崩点 | 修复；恢复类问题优先级最高 |
| G6 资源 | 2C/2GiB 容器全流程；只写 `/data`；S6 起加 256 MiB 快速用例 | 修流式/内存；不提交超限版本 |
| G7 材料 | `Dockerfile`、`README.md`、`docs/DESIGN.md`（可为当日版本）、`agent/conversation.json`（合法 JSON、≤128 KiB、指针可解析、同 commit、无密钥）；根目录副本一致 | 重新生成/裁剪；材料不合规不提交 |
| G8 平台预检 | 仓库/commit 可获取、镜像构建、启动、协议、公开样例（若有）通过；输出存档 | 按预检反馈修；预检不过不正式提交 |
| G9 台账 | 记录 SHA、镜像 digest、JSON sha256/大小、门禁结果、预检输出、提交时间、预期风险 | 缺记录视为未完成 |

**"合规"的定义**：G1–G9 全部满足。隐藏用例是否通过无法在提交前保证——这正是每天两班换反馈的原因；但绝不提交"构建失败/启动失败/材料缺失/带密钥/已知回归"的包。

#### 13.3.1 提交前命令清单（示例，实际以 `scripts/release_gate.py` 为准）

```bash
# 1) 分支与提交
git status --porcelain                    # 必须为空
git rev-parse --abbrev-ref HEAD           # 2026-competition-round2
git rev-parse HEAD                        # 40 位 SHA
git ls-tree -r --name-only HEAD | grep -E '^(src/|Dockerfile|agent/conversation\.json|README\.md)'

# 2) 干净构建与冒烟
docker build --no-cache -t mvdb:candidate .
docker run --rm --network=none -i -v /tmp/mvdb-smoke:/data mvdb:candidate < tests/smoke.sql

# 3) 材料
python3 tools/validate_conversation.py agent/conversation.json
sha256sum agent/conversation.json conversation.json
python3 tools/scan_secrets.py .

# 4) 推送并验证远端可获取
git push origin 2026-competition-round2
git ls-remote origin refs/heads/2026-competition-round2

# 5) 平台预检 -> 记录输出 -> 正式提交 -> 写台账
```

> 其中"推送到远端"必须在正式提交前完成，且提交后不得 force-push；tag 与台账用于证明"源码/Dockerfile/材料来自同一 commit"。

### 13.4 提交台账与"已知通过版本"

维护 `docs/SUBMISSIONS.md`（或 `release/submissions.csv`），每次提交一行：

```
slot, datetime, commit_sha, image_digest, json_sha256, json_bytes,
gate_A..G, precheck_result, formal_result, failure_categories,
admission_pass, local_E_write, local_E_query, recall, notes
```

规则：

1. 任一提交经平台确认"MySQL case 全部通过"→ 立即标记 `best-known-good`，记录其 SHA/镜像/材料摘要。
2. **best-known-good 永不删除、永不改写历史**；后续任何版本失败都不影响它作为最终候选。
3. 一旦拿到 `best-known-good`，每天结束时必须保有它；尚未拿到时，它是最高优先级，次日两班都围绕准入。
4. 最终参评版本只能从 `admission_pass = true` 的提交中选；若一个都没有，总分为 0，继续尝试直到截止。

### 13.5 最终参评版本选择

- 候选集：所有 `admission_pass = true` 的正式提交。
- 目标：最大化 `S_total = S_perf + S_prompt`，但两项都是排名换算，提交时无法精确知道名次；用本地代理指标决策：
  - 性能：本地三轮 E 中位数（同硬件、同数据生成器）、启动/恢复时间、内存峰值；
  - prompt：材料完整度、五个维度的独立证据数量与质量、是否超限。
- 优先选择"**代码性能最好 + 材料最新最完整**"的组合；若两者不在同一提交，用 S18/S20 的"代码不变 + 材料更新"提交制造组合候选。
- 若平台在截止后按"最近一次全部通过"默认选择，则务必让**最后一次提交也是通过版本**，或显式指定最终版本。
- 截止前 24 小时不再引入高风险代码变更；最终选择完成后不再改动对应 commit。

### 13.6 每日工作流（模板，不绑定具体时刻）

| 阶段 | 工作 |
| --- | --- |
| 班前 | 修上一班失败类别；完成本班功能/性能目标；跑差分与回归 |
| 门禁 | G1–G9 + 平台预检；不通过则改发备用候选（best-known-good 代码不变 + 材料更新） |
| 提交 | 记录 SHA/镜像 digest/材料摘要/预检输出；更新台账 |
| 班间 | 分析反馈；开发下一班目标；准备下一班的备用候选 |
| 收尾 | 更新实验台账/证据；写次日计划；检查磁盘/镜像/分支状态 |

**第一天（11/09）**：优先解决仓库可见性与分支（若未解决），再做 v0.1 垂直切片与 Docker 骨架；通过门禁后提交 S1；随后补 UPDATE/DELETE/事务，按备用班规则提交 S2。不绑定具体钟点，但**当天至少完成一次合规提交**。

### 13.7 冻结与降级

- **11/16 第二班后功能冻结**：之后只允许 bug fix、文档、证据更新、配置切换；不再引入新模块。
- **11/17 起**：性能改动必须"有测量、有回退、代码 diff 可审查"；否则不做。
- **11/18**：只提交，不改代码；若 S19 失败，S20 用已验证的 best-known-good + 材料。
- 若进度落后，降级顺序：ANN → 索引持久化 → 过滤 bitmap → 某些性能优化；**绝不降级**：功能语义、崩溃恢复、持久化、材料合规。

---

---

## 14. 风险登记表

| # | 风险 | 概率/影响 | 触发信号 | 缓解措施 |
| --- | --- | --- | --- | --- |
| R1 | 题包不公开导致协议/错误码/格式不匹配 | **高/致命** | 预检失败、结果格式全错、错误码不符 | 协议 profile + 解析完备性分帧；预检当免费探针；stderr 诊断；预留 2–4 次提交校准；不在协议上做不可逆耦合 |
| R2 | 崩溃恢复出现部分提交 | 中/致命 | kill 测试出现半行/丢已提交 | COMMIT 标记 + fsync 屏障；恢复只读；崩点矩阵；不变量断言 |
| R3 | 功能覆盖不全（某用例失败→0） | 中/致命 | 本地套件漏项 | 需求→用例追踪矩阵；负向用例；差分测试 |
| R4 | 超内存数据被全量载入 | 中/高 | cgroup OOM/超限 | 流式扫描；有界缓冲；大表压测；PK map 退化方案 |
| R5 | ANN recall 不达标 | 中/中 | 平台 Recall<0.95 | ann 模式 exact 回退；阈值 0.98 留余量；过滤走暴力 |
| R6 | 9 天时间不够，功能未达标 | **高/致命** | 里程碑延期 | 每日两班、先纵切；砍 ANN/持久化索引；保准入；冻结后只修 |
| R7 | 浮点格式/精度不匹配 | 中/中 | 距离断言失败 | 高精度输出；对拍 tolerance；预检确认 |
| R8 | Docker 构建/运行不合规 | 中/高 | 构建失败、运行联网、非源码构建 | 干净 `git archive` + `--no-cache` 构建；运行 `--network=none`；门禁 G2 |
| R9 | 磁盘不足（本地 8 GB） | 中/中 | 构建失败 | 清理 dist/镜像；定期 prune；小镜像 |
| R10 | prompt 材料证据不足/超 128 KiB | 中/高 | 评审低分、材料被拒 | 从第一天记台账；按单元裁剪；工具校验大小/指针；每日快照 |
| R11 | PAT 泄露 | 低/高 | 仓库/日志出现 token | 轮换 token；改 credential helper；提交前扫描 |
| R12 | 索引维护后结果不一致 | 中/高 | 增删改后 ann 查询命中已删行 | 索引结果二次校验；事务 overlay；精确补齐 |
| R13 | 单线程下慢查询阻塞/超时 | 低/中 | 超时用例 | 限制扫描、索引准备；性能阶段优化 |
| R14 | 每日双提交导致仓促回归 | 高/高 | 新班次引入已知 Blocker | 发布门禁；best-known-good 兜底；备用班只做"代码不变+材料"；不 force-push |
| R15 | 平台反馈延迟，当天无法据此修复 | 中/中 | 结果次日才回 | 不阻塞开发；两班按既定里程碑推进；预检即时反馈优先 |
| R16 | 每日维护材料挤占开发时间 | 中/中 | JSON 组装超时、超限 | 台账模板化；`build_conversation.py` 自动化；每天固定 30 分钟 |
| R17 | 早期通过版本材料不成熟，无法回填 | 中/高 | 最终候选材料偏弱 | 每日更新材料；S18/S20 用代码不变+材料更新制造组合候选 |
| R18 | 分支/SHA 不可获取或历史被改写 | 低/致命 | 平台拉取失败 | 只在 `2026-competition-round2` 追加提交；禁止 force-push；每班后验证远端可 fetch；打 tag |
| R19 | 截止日平台拥堵/提前截止 | 中/高 | 提交排队/页面不可用 | 11/18 至少提前 4 小时提交；最终版本 11/17 已就绪 |
| R20 | 仓库不可被平台获取 | 已缓解/持续校验 | 提测时克隆失败/404 | 已转 public + 分支推送 + 匿名 clone 验证（11/09）；每班 push 后复验；保持 public；URL 不带 token |

---

## 15. 待确认决策

| # | 问题 | 现状/建议 | 影响 |
| --- | --- | --- | --- |
| Q1 | 实现语言 | **已确认：C++20** | 全部实现 |
| Q2 | 平台预检/公开样例/协议说明能否拿到？ | 官方题包不公开；**但预检是免费的**。请把每次预检输出、正式提交返回的失败类别和日志原文保存下来 | 协议校准速度；决定前 2–4 次提交的用法 |
| Q3 | 11/18 具体截止时刻、每日额度重置时区 | 按北京时间 23:59 预留，实际至少提前 4 小时提交 | 最后一班的安排 |
| Q4 | 平台是否接受"代码不变、只更新 `conversation.json`/文档"的新 commit 再提交？ | 若接受，用作备用班与最终组合候选；若不接受，备用班改为"经验证的小修复" | 每日两班策略、最终版本组合 |
| Q5 | exact/ann 的"提交配置"以什么形式指定？ | 未知；先默认 exact + 多路支持；若平台有表单/文件，请告知 | 性能项能否用 ann |
| Q6 | 正式提交由谁操作、仓库是否已公开可拉取？ | 需要平台可获取的 HTTPS URL、分支 `2026-competition-round2`、40 位 SHA；仓库已 public，分支已匿名 clone 验证（11/09），继续确认平台是否接受该 URL、是否要求 fork 关系 | 每班交付、平台拉取、最终准入 |
| Q7 | `conversation.json` 是否从本会话/现有记录开始积累？ | 建议从今天起维护真实台账，每次提交生成快照 | prompt 40 分 |
| Q8 | 是否允许参考 `dist/` 下旧的 seekdb 向量索引实验？ | **不建议**：规则要求存储/索引/事务核心自研；只借鉴思路，不复制代码 | 合规 |

> 如果 Q2 完全无法获得，协议风险只能按 §2.4 的 profile + 反馈闭环处理；请务必保留每一次预检/提交返回的原始文本（即使只有一行），这是唯一的外部真值。

---

## 16. 附录 A：功能验收清单（Definition of Done）

### 16.1 SQL / 存储

- [ ] CREATE TABLE：合法三种类型 + 主键；重复表/非法维度/缺主键报错
- [ ] 多行 INSERT：成功；含重复 PK 时整条回滚
- [ ] BIGINT 边界（min/max/溢出）
- [ ] VARCHAR：码点边界、中文、emoji、超长报错
- [ ] VECTOR：维度、NaN、Inf、overflow、非法字面量
- [ ] SELECT * / 列投影 / 别名
- [ ] WHERE 五种比较 + AND；空结果
- [ ] ORDER BY 普通列 + ASC/DESC + LIMIT
- [ ] UPDATE 单列/多列/条件/无条件；主键更新报错
- [ ] DELETE 条件/无条件
- [ ] 重启后表定义、数据、索引定义一致

### 16.2 向量

- [ ] L2 距离与 double 真值误差在容差内
- [ ] 精确 TopK = 暴力真值（含过滤）
- [ ] 不足 K 条返回全部
- [ ] 距离并列按 PK 升序
- [ ] 过滤后的 TopK 不是全局 TopK
- [ ] 更新向量后结果立即变化
- [ ] 删除后不再命中
- [ ] ann 模式返回合法、无重复、满足过滤的当前行；Recall ≥ 0.95

### 16.3 事务

- [ ] BEGIN → INSERT → 同事务 SELECT 可见
- [ ] COMMIT 后重启仍存在
- [ ] ROLLBACK 后行、可见集、TopK 全部恢复
- [ ] 事务内 UPDATE/DELETE 的读己写
- [ ] 语句失败不污染事务，后续可提交
- [ ] autocommit 每条语句持久化
- [ ] 关闭时活动事务回滚

### 16.4 恢复

- [ ] 正常关闭 → 重启一致
- [ ] kill -9 在提交前/中/后各窗口行为正确
- [ ] checkpoint 中强杀可恢复
- [ ] 恢复中再强杀可重复恢复
- [ ] 不变量 1–7 全部成立

### 16.5 资源 / 交付

- [ ] 数据量 > 内存限额时不 OOM，TopK 正确
- [ ] 磁盘写满时报错不崩溃
- [ ] stdout 只有协议输出；ready 前无字节
- [ ] 每条语句及时 flush
- [ ] Docker 离线构建、启动、重启、强杀
- [ ] README 覆盖题面 4.2 全部要求
- [ ] 设计文档覆盖 4.3
- [ ] `agent/conversation.json` ≤128 KiB、合法 JSON、指针可解析
- [ ] 源码/Dockerfile/JSON 同一 commit；分支 `2026-competition-round2`

---

## 17. 附录 B：实验记录模板

每个实验一行/一节，示例：

```markdown
### E04：HNSW efSearch 对 recall/延迟的影响
- 日期/记录号：2026-11-xx / EXP-004
- 假设：efSearch=64 时 Recall@10 ≥ 0.98，P50 延迟 < 0.3 ms
- 数据：10k×128，均匀分布，seed=42；500 条查询（250 过滤）
- 命令：`./bench --mode ann --ef 16,32,64,128 --rounds 3`
- 结果：ef=16: recall 0.961 / P50 0.11ms；ef=64: 0.992 / 0.24ms；ef=128: 0.998 / 0.41ms
- 我的解释：ef 增大扩大候选集，recall 提升但边际递减；过滤查询走暴力更稳
- 决策：ef=64（留 0.03 余量）；过滤基数 < 2000 时走暴力
- 复测：ef=64 在 seed=7/123 上 recall 0.989/0.991
- 局限：仅 10k 规模；更大 N 需重新调参
```

**记录纪律**：

- 保留命令原文与输出关键行（可截断，但不能改写结果）。
- 区分"计划/预测"与"实际执行"。
- 失败与返工同样记录（评审看重修正链，不是一路顺利）。
- 学生自己的解释用中文写，不要复制 Agent 的措辞。

---

## 18. 下一步（11/09–11/10 的具体行动）

### 18.1 今天（11/09）

1. **仓库就位（已完成 11/09）**：
   - 仓库已 public；`2026-competition-round2` 已推送脚手架 commit；
   - 已用无凭据 `git ls-remote` + `git clone --depth 1` 验证可拉取；
   - 后续 push 走 SSH（HTTPS PAT 只读），只追加、不 rewrite。
2. **交付骨架**：`CMakeLists.txt`、`src/main.cpp`、`Dockerfile`（多阶段）、`scripts/release_gate.py`、`docs/PROTOCOL.md` 初稿、`docs/SUBMISSIONS.md` 模板。
3. **v0.1 垂直切片**：协议层（解析完备性分帧 + `mysqltest` profile + ready）、`CREATE TABLE`、多行 `INSERT`、简单 `SELECT`、PK map、WAL/manifest、重启恢复。
4. **自测与门禁**：Docker 干净构建、启动、smoke SQL、重启、kill -9 一次、stdout 纯净、材料占位合规。
5. **预检 + 提交 S1**：若平台预检可用，先跑预检并保存原文；通过门禁后正式提交，记录台账。
6. **v0.2**：UPDATE/DELETE/BEGIN/COMMIT、类型校验、错误表；吸收 S1 可得的反馈；按备用班规则提交 S2。
7. **证据台账**：每班结束按 §12.4 的"Agent 起草问题 → 学生回答 → Agent 整理稿 → 学生确认 → 写入"流程，把真实实验写入 `agent/evidence/`，为 `conversation.json` 准备第一批单元。

### 18.2 明天（11/10）

- 第一班：修 S1/S2 失败类别；完成完整 SQL/别名/ORDER BY/LIMIT/语句原子性 → S3。
- 第二班：向量 L2/TopK/过滤/并列 + 事务读己写 + 差分 100 seeds → S4。
- 晚上：建立性能/崩溃测试的最小版本，避免后面临时补。

### 18.3 需要你提供/确认的输入

- 平台的**预检入口**、**提交入口**、以及每次返回的**原始文本**（失败类别/日志）；
- 11/18 的精确截止时刻与每日额度重置时间；
- 平台是否接受"代码不变、只更新材料"的新 commit 重复提交（Q4）；
- exact/ann 提交配置的指定方式（Q5）；
- 每班结束的 3–5 个问题回答（一句两条即可），以及平台反馈原文；Agent 整理成稿后给你确认，不照抄原话。

> 计划冻结后，任何偏离都要在本文档更新"变更记录"并说明原因，避免范围悄悄膨胀。

## 19. 实现进度（2026-11-09，v0.4）

**已完成**

- C++20 + CMake + 多阶段 Dockerfile；零第三方运行时依赖。
- `/data/wal.log` 追加写：记录头 + CRC32C + 事务 id + COMMIT 标记；提交前 `fdatasync`。
- 行定位索引：`pk -> WAL offset`；小表（默认 ≤50k 行）保留内存行缓存，超阈值自动转磁盘模式，只保留定位索引；`--memory-rows` 可调。
- 恢复：只重放已 COMMIT 的事务；截断未提交/不完整尾部；按 WAL 位置重建定位索引；小表按需载入内存。
- 干净关闭时若 WAL 超过 `--checkpoint-bytes`（默认 64 MiB），把活行/表定义/索引压缩成新 WAL 并原子 rename；崩溃在 rename 前/后均保持一致。
- SQL：`CREATE TABLE`、多行 `INSERT`、`SELECT`（投影/`*`/WHERE/AND/ORDER BY/LIMIT/`L2_DISTANCE`）、`UPDATE`、`DELETE`、`BEGIN/COMMIT/ROLLBACK`、`CREATE VECTOR INDEX`、`SET query_mode`。
- 类型校验：BIGINT 溢出、VARCHAR 码点长度、VECTOR 维度/NaN/Inf/float32 溢出、主键唯一、每表一个向量列。
- 精确 TopK：过滤后取 K、距离并列按主键升序、不足 K 返回全部；AVX2/FMA 运行时派发，标量回退。
- 有界 TopK：普通 `ORDER BY ... LIMIT` 用 K 大小堆；无 `ORDER BY` 的 `LIMIT` 命中 K 条即停；距离 TopK 只为入选行复制行数据。
- 协议开关：`--ready-marker`、`--statement-mode=semicolon|line`、`--dml-ack`、`--query-mode=exact|ann`；环境变量 `MVDB_*` 可覆盖；未知参数忽略。
- 兼容：`CREATE TABLE IF NOT EXISTS`、`DROP TABLE [IF EXISTS]`、`TRUNCATE [TABLE]`、`USE`、常见 `SET` 会话语句（no-op）。
- 崩溃注入：`MVDB_CRASH_POINT` 覆盖写 WAL、commit fsync、checkpoint rename、恢复 pass1/truncate。
- 测试：`tests/run_smoke.py`（内存/磁盘、重启、kill -9）与 `tests/run_regression.py`（暴力真值、事务、错误、崩溃、索引、checkpoint）。

**本地测量（仅参考，非平台成绩）**

| 项 | 结果 |
| --- | --- |
| 10k×128 批量写入（100 行/批，含 fsync） | 约 0.49 s |
| 1000 次精确 TopK（K=10，一半带过滤） | 约 0.40 s |
| 重启恢复（5.6 MB WAL） | 约 0.05 s |
| 峰值 RSS（10k 内存模式） | 约 18 MB |
| 50k×128 磁盘模式（含装载+TopK） | 约 2.36 s，RSS 约 37 MB |
| SQL 边界回归 | 158 项 × 内存/磁盘，0 失败 |
| 随机崩溃 fuzz | 800 次迭代（6 个崩溃点 + 随机 SIGKILL），0 失败 |

**未完成 / 下一阶段**

- 真正的 ANN（HNSW/IVF）；当前 `ann` 仍精确回退（Recall=1.0）。
- 不可变段文件/manifest；当前用 WAL checkpoint 压缩，磁盘模式查询需扫 WAL。
- DDL 在显式事务内目前报错；需按题包决定隐式提交或可回滚。
- 完整崩溃矩阵的随机化压测（已覆盖关键点，但未做数百次随机 kill）。
- 平台协议校准（ready/DML 状态/错误格式目前是可切换假设）。
- 平台预检与首次正式提交反馈；协议 profile 的最终选择。
- ANN：已实现 HNSW 原型并评估；uniform 10k×128 下 recall 0.98 仅比精确快 1.5–2×，决定**不启用**，保留精确（clustered 数据才值得）。
- 不可变段文件/manifest；当前磁盘模式查询仍扫描 WAL。
- line mode 仅支持一行一条；多行 continuation 仍不支持（默认 semicolon 模式完整支持）。
- 官方驱动未验证：ready/DML ack/错误文本/发送分片仍待平台预检与正式反馈。
