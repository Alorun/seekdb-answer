# 提测失败后的协议修正计划（2026-11-09）

## 1. 已知事实

- 2026-11-09 的正式提测反馈只有一句：**MySQL 正式用例未通过；该条件是准入条件，不计排名分**。平台没有返回失败类别、期望/实际输出或超时位置。
- 官方 `oceanbase/seekdb` 的 `2026-competition-round2` 分支随后更新到 `9f6df2ba5`，公开包 `mysqltest/` 共 **20 个用例、95 条 SQL**，其中：
  - `cases/*.test` **每个非空行是一条完整 SQL，行尾没有分号**；
  - `cases/*.result` 是逐条对应的 JSON 断言，字段为 `{}`、`error`、`affected_rows`、`columns`、`rows` 或 `unordered`；新增 `--restart`（同数据目录重启）和 `--mode ann` 驱动指令；
  - README 明确说明：每个用例从独立空数据库开始，逐行执行并比较断言。
- 失败提交 `eade40b44` 的默认协议是：**semicolon 分帧 + DML 无输出**。

## 2. 失败假设与优先级

| 优先级 | 假设 | 证据 | 结论 |
| --- | --- | --- | --- |
| H1（确定） | 驱动逐行发送无分号 SQL，程序一直等待 `;` | 公开 `.test` 每行无分号；失败提交默认 semicolon | 必然导致所有用例超时或整段解析失败 |
| H2（确定） | 驱动需要 `affected_rows` 才能断言 DML | 公开 `.result` 多处 `affected_rows`；失败提交默认无 DML 输出 | 即使 H1 修好，DML 断言仍无法通过 |
| H3（中） | 驱动对 ready 的处理与批量/交互模式不同 | 平台不返回日志；公开 README 不描述 ready | 用 Docker 环境变量做自适应：已写入 SQL 时不输出 ready，等待时输出 |
| H4（中） | 驱动对 SELECT 的结束判定依赖预期行数 | `.result` 给出精确 `rows` | 输出 header + 精确行；不加额外终止行，避免污染下一条响应 |
| H6（高） | 驱动用 `--ready-marker` 做**逐响应分帧** | 公开驱动约定；失败提交只把标记当启动行 | 传入 marker 时每个响应后输出标记；首条 SQL 已写入时不额外输出启动标记 |
| H7（高） | 错误断言使用**语义名**而不是数字码 | 公开 `.result` 使用 `duplicate_key`/`unsupported_sql`/`dimension_mismatch` | 默认输出语义名；`--numeric-errors` 保留数字码本地套件 |
| H8（确定根因） | 正式协议是 **JSON Lines**，不是文本/ready-marker | 官方 `STDIO_PROTOCOL.md` + `protocol_check/` | 默认 JSONL：ready、execute、typed rows、affected_rows、error.code、shutdown；`--protocol=text` 仅供本地脚本 |
| H5（低） | 驱动要求 JSON 响应 | 题面明确说不再采用 JSON 请求/响应；公开 README 说 `.result` 不是运行协议 | 保持文本 `OK <n>` + TAB 结果 |

## 3. 修改项

1. 默认值改为 `line_mode = true`、`dml_ack = true`：
   - 一行一条 SQL，不要求分号；
   - CREATE/DDL 输出 `OK 0`；
   - INSERT/UPDATE/DELETE 输出精确 `OK <affected_rows>`；
   - SELECT 输出 header + TAB 行，不加额外终止行。
2. 保持 semicolon 模式可用：本地多行脚本用 `--statement-mode semicolon`。
3. 为无换行写入增加 10 ms idle 执行：驱动写完整 SQL 但不发 `\n` 时仍能返回。
4. Docker 镜像设置 `MVDB_ADAPTIVE_READY=1`：
   - 启动时 stdin 已有 SQL（批量驱动）：不输出 ready，避免多一行；
   - 启动时等待输入（交互驱动）：50 ms 后输出 `ready`。
5. 增加官方公开用例的本地执行器 `tests/run_public_cases.py`，按 `.test` 逐行发送、按 `.result` 比较。
6. 门禁容器冒烟改为官方行协议；本地回归显式传旧模式参数，避免默认值变化造成假失败。

## 4. 验证矩阵

| 项目 | 命令 | 期望 |
| --- | --- | --- |
| 官方协议握手/公开用例 | `protocol_check/run.py --image ...`（官方工具） | 握手 + 20/20 PASS |
| 官方公开用例（v4，无标记，本地文本） | `python3 tests/run_public_cases.py --bin ./build/minivectordb` | 20/20 PASS |
| 官方公开用例（v4，marker 分帧） | `python3 tests/run_marker_cases.py` | 20/20 PASS |
| 严格错误语义 | 公开 runner 的 `error` 分支要求 `ERROR <name>:` | 20/20 PASS |
| SQL 边界 | `python3 tests/run_sql_edges.py --bin ./build/minivectordb` | 158 项 × 2 模式，0 失败 |
| smoke/regression | `python3 tests/run_smoke.py ...` / `run_regression.py ...` | 通过 |
| 崩溃 fuzz | `python3 tests/run_crash_fuzz.py ... --iterations 100` | 200 次，0 失败 |
| 容器交互 | `docker run -i ...`，先读 ready 再发 SQL | ready、OK、header 正常 |
| 发布门禁 | `python3 scripts/release_gate.py --require-conversation` | PASS |

## 5. 仍未验证、但无法从平台获得的项

- 驱动 ready 的**确切字符串**（当前为 `ready`，Docker 自适应只解决“是否输出”，不解决“字符串是否匹配”）。
- 错误行是否要求 MySQL 的 `(SQLSTATE)` 或特定文本；当前为 `ERROR <code>: <message>`。
- SELECT 是否要求额外终止行；当前依赖驱动按 `.result` 的预期行数读取。
- 驱动是否把 `{}` 语句也读一行状态；当前每条 DDL/DML 都输出一行 `OK`。

平台只返回“未通过”一句话，以上四项无法再做本地判别。若新版仍失败，需要平台至少给出“超时/格式/结果不匹配”中的一类，才能继续缩小。

## 6. 提交检查

- DB 和 Agent prompt 两个入口使用**同一 branch、同一 40 位 SHA**；
- `agent/conversation.json` 与该 SHA 中的文件字节一致；
- 提交前不 push 新 commit；
- 记录本次 SHA 到 `docs/SUBMISSIONS.md`。
