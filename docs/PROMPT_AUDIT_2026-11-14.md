# Prompt 审计与新版开发提示词

> 2026-11-14 19:05 更新：本轮已用 `scripts/build_conversation.py` 生成 25 单元材料（五维 5/5/6/4/5，raw 42.0，52 KiB），每个单元的学生话与结果分别引用、结果均来自本 commit 的本地命令。旧 13 单元材料保留在 `agent/candidates/conversation.stable13.json` 作为回退；下面关于旧材料的审计仍保留，作为历史记录。

## 结论与交付边界

已完整阅读 `/home/aln/文档/db.md` 的 §2.4、§3.4、§4.4，检查 SUBMISSIONS、FAILURE_LEDGER、PROMPT_PERF_LESSONS、PLAN 的学习证据流程，对照 dd82df996、954f61118、当前 HEAD 的材料，并查看当前存储、执行器与协议代码。

新版 `agent/development_prompt.md` 是可实际交给开发 Agent 的工作提示词，覆盖当前代码关键模块、不变量、失败反例、回退和验证要求。它是本次新拟定的 Agent 文本，不倒填成历史学生发言。没有做从空仓库重新生成代码的实验，不能声称逐字复现现有源码。

`agent/candidates/conversation.audit.json` 内嵌完整新版 prompt、本次实际测试输出、源码文件摘要和历史归属说明。它是审计/证据补全候选，**不是已经验证会涨分的正式替代文件**。原 `agent/conversation.json` 与根目录副本均未覆盖；12.5 分版本可继续回退。没有修改数据库源码、推送或正式提交。

## 官方规则与历史推断必须分开

- 官方：单文件 UTF-8 JSON，顶层对象，≤128 KiB；不强制 `units` 数组或五维必须都有实例。原 release_gate 的64 KiB/至少5实例/证据必须等于整个字段，是更严格的本地约定，不是官方业务结构要求。保留原门禁以免改变既有发布流程，新候选用独立校验器。
- 官方：每实例只在一个维度计一次，最高2分，原始分不封顶；三轮逐维中位数后相加。13实例最高26原始分；若目标是30原始分，至少15个真实满档独立实例；超过30至少16个。这个数学下界不是建议凑数量。若目标是赛事排名换算30分，则取决于名次，不能由材料保证。
- 官方：评委只读这个JSON，Agent技术解释和工具日志不自动证明学生理解；合成内容不能作为真人学习证据。摘录须指向JSON字符串，≤240字符，并区分学生与结果。
- 历史观察：24/20/18实例的材料被报无效，13/15实例曾有效。缺少三轮报告、材料类型、引用校验与具体复核原因，不能推断存在“18实例数量上限”或“改自评分导致掉分”的因果。
- `NEEDS_REVIEW`、评审结构错误和平台故障应区分于学生材料零分。不要自行截断、缩放得分以掩盖错误。应索取同一锁定提交的报告和复核结论。

## 已定位的材料缺陷

1. 当前 C7 note 写“角色说明，非学生证据”，却被 U6 作为学生证据；理由另谈 AVX2/DDL，引用却是 Huffman 修复。
2. U12 讨论负优化，指向 E24 prompt 得分教训；U13 讨论 T/B/M，却指向 AVX-512下降。字符串匹配通过不能证明语义匹配。候选已改连对应历史输出。
3. C8、E14、E15 存在重复记录编号（不是重复JSON键）。已为候选重新分配唯一编号。
4. 部分 `raw_output` 放的是其他实验内容，无法当原始输出认证；候选去掉这些跨题字段，并保留相关实验原有output。
5. 旧状态日期、代码版本、旧正式成绩、未来计划混用；例如源码已有XCR0检查和双核worker，历史材料还概括“已回退、不默认开启”。候选使用文件SHA256绑定当前源码，旧结论单列为历史。
6. dd82df996 保留较短的用户原文节选；后续归一化稿扩展为学生技术发言。本地未找到逐条确认记录，不认定为已核实学生贡献，也不据此直接指控历史材料编造。

## 新 prompt 如何对应代码

| 提示词约束 | 当前代码落点 | 独立验收依据 |
| --- | --- | --- |
| JSONL主动ready、typed rows、逐条flush、shutdown | src/main.cpp、src/jsonl.cpp | 官方交互检查器 |
| float32输入、Unicode、失败语句原子性 | src/common.cpp、parser.cpp、executor.cpp | SQL边界双模式 |
| 批量WAL、Huffman、恢复/压缩共用解码 | src/common.cpp、database.cpp、executor.cpp | checkpoint与崩溃测试 |
| q7误差下界、double复核、TopK并列 | src/executor.cpp | NumPy暴力oracle |
| row_version、过滤行号、IVF半径下界 | src/database.h、executor.cpp | 变更/回滚、uniform/clustered差分 |
| 同步后响应、两路任务生命周期、SIMD回退 | src/main.cpp、executor.cpp | crash、关停；本次未重跑ASAN |

官方在线 STDIO_PROTOCOL 明确使用 JSONL；赛题概述仍有非JSON封装描述，必须记录冲突并用冻结题包检查，不能只挑一种文字推断。来源：https://raw.githubusercontent.com/oceanbase/seekdb/2026-competition-round2/STDIO_PROTOCOL.md 。

## 本次实际验证

- Release构建通过。
- 本地text公开20例通过；另外使用 `/tmp/official/protocol_check/run.py --command ... --package ./tests/public_cases --output /tmp/prompt-audit-20261114-official`：官方JSONL交互20例全部PASS，报告 `passed: true`。这是本地进程，不是Docker或正式隐藏评测。
- 当前build：SQL边界内存/磁盘各158检查，0失败。第一次默认脚本还测到build-lead旧二进制，候选生成器重跑时用 `--bin ./build/minivectordb` 排除它，不把旧构建当当前证据。
- 5000×128、各30查询，uniform/clustered两组 exact 的ID/顺序/距离均匹配oracle；ann recall@10=1.0000；缓存失效/回滚/并列检查通过。
- 崩溃测试固定种子20261114，内存/磁盘各100轮，共200轮；6个确定性崩溃点和66次随机强杀，0失败。
- 10k批量插入、checkpoint阈值1、重启查询通过；该脚本只核对一个TopK，不夸大为全量数据逐行校验。
- 本次未做Docker、ASAN、cgroup性能、从零prompt重建或正式评分。

## 真正补足30分证据所缺的输入

不是再写漂亮的学生台词，而是找回原始历史对话/确认记录与平台三轮报告。可以由学生选择真实做过的独立实验，用自己的话回答以下问题；没有做过或不知道可以直接写明。此处问题是Agent拟定，尚无学生回答，不放入已计分证据。

1. 当时哪条实际响应或失败日志使你否定了文本协议假设？你自己提出了什么验证，得到什么结果？
2. q7只精排固定数量候选为什么不够证明exact？你当时预测过哪个反例，实际怎样验证？
3. 失败多行INSERT处于显式事务中时，你预计此前成功的修改应怎样保留？你用过什么输入验证？
4. 批量WAL在checkpoint重启后失败时，你参与提出过哪些可能原因，什么结果排除了其中一个？
5. 你拒绝的某项优化，在同机同负载的哪组测量下不值得上线？换一种分布后结论是否仍成立？

回答后仍需记录具体输入输出、学生解释及必要复测；同一链不能拆成多实例。简单认可Agent答案或统一回复“确认”不能替代独立理解。新增结果与学生解释在JSON内分别保存，才有合规提升高档实例的基础。

## 工具

`python3 scripts/refresh_prompt_material.py --verify` 会真实重跑本地测试并向stdout输出候选文件的apply_patch补丁，**不自动写文件**。由编辑工具应用补丁。`python3 scripts/check_prompt_material.py agent/candidates/conversation.audit.json` 校验官方大小上限、JSON、编号、pointer与引用。结构PASS不代表真实性认证或得分。
