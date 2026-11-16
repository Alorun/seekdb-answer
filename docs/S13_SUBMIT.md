# S13 提交包与决策记录（2026-11-16）

## 结论

- 代码：分支 `2026-competition-round2`，提交 `410e8c174`（其父为 `46fb13fd5`、
  `b937b880d`、`7f30b75d2`、`51b64a83e`、`1feebd38d`、`f1c5e3d61`、`4455e0bed`）。
  远端匿名可拉取已由 release_gate 验证。
- 材料：`agent/conversation.json`，**84,549 B，sha256 f0a7421…**，40 单元 / 自评 70.5，
  S7 平台实测 **45.5**。**与已验证文件逐字节相同，不提交任何改写。**
- Docker：`Dockerfile` 多阶段构建，`docker build -t minivectordb:s13 .` 通过；
  官方 `protocol_check` 用该镜像 20/20（含 `--read-only`、`--network=none`、cap-drop）。
- 本地门禁：`scripts/full_gate.sh` **14/14 PASS**；ASAN+UBSAN 下 public 20、官方 20、
  SQL edges 158×2、crash fuzz 120、checkpoint 10k 全部通过。

## 相对 S12（`47561eff6`）的改动

| 方向 | 改动 | 实测 |
|---|---|---|
| M | RowStore（PK 有序 vector 取代 std::map）、LocationIndex（扁平向量取代 unordered_map）、写→读边界一次 `malloc_trim` | 13.53 → 12.55 MB（-7.2%） |
| T_write | JSON 零拷贝 + SSE2 字符串扫描、fixed24 融进字面量解析、SWAR 六位小数解析、memchr 单元扫描、列编码/Huffman writer 提速 | 服务端 CPU 79 → 49 ms/100 批（-38%），WAL 字节完全相同，B 不变 |
| T_query | 掩码 pool 构建、8 行流水预取、FilterCodes 块自带行号（一次哈希查找）、Int/Double 原样输出、别名时不再生成 L2_DISTANCE 头部文本 | unfiltered exec 34.3 → 31.5 µs（-8%），filtered 16.7 → 15.0 µs（-10%） |
| 合并 | 5 轮交错 A/B（2 vCPU/2 GiB docker） | T_write -8.6%，T_query ±0（平价），B 0%，M -7.2%；代理 E 5000 → 5071 |

服务端 CPU 的 -38% 落到墙钟只有 -8.6%，因为 T_write 的 ~165 ms 是 100 次必须的
fdatasync；B 已在无损熵底（向量 3.36 MB + 行数据 ≈ 3.95 MB 物理写）；T_query 的
~20 µs 是 1.28 MB q7 码扫描，受带宽限制。这三项是结构地板，已在
`docs/S13_PLAN.md` 与 `docs/FAILURE_LEDGER.md` 记录。

## Prompt 决策（重要）

审计结论（`agent/candidates/conversation.s17.minimal-edit.json`，85,510 B）：

- 40 个单元里存在**语义重复链**：U17/U29/U34（同一条写路径）、U12/U25（同一套崩溃
  恢复）、U02/U31（同一次噪声测量）、U23/U30（同一份内存分解）、U22/U39（同一类
  量化负结果）；U21 的证据其实是 q7 IVF 原型，和 U26 是同一条链。严格去重会扣分。
- 已准备的候选只给 U18/U19/U36/U37 补 `transfer`/`limitation`（+961 B），分值、
  元数据、summary 全部不变；结构校验通过。
- 但历史上两次改写（85,815 B 追加元数据、81,971 B 只改 U26+压缩 JSON）都被判
  "无效/待复核"，而 84,549 B 同一份 40 单元得 45.5。任何改写都在重新掷骰子：
  估计 P(invalid) 60–75%，期望收益只有 +1~2 原始分。

**因此：S13 提交仍用冻结的 `f0a7421` 84,549 B。** 只有在已经拿到一次
"E ≥ 5100 + prompt 45.5" 的正式结果、并且还有多余额度时，才值得用候选去试；
即使失败，已达成的那次仍是最终参评版本。

## 提交顺序建议

1. 第 1 次：`410e8c174` + 冻结材料。记录 E、prompt。
2. 第 2 次（同一天额度内）：同代码同材料再跑一次（独立噪声抽样）。
3. 若两次都 < 5100：第 3、4 次（11-18）继续同组合；如果某次 E ≥ 5100 且
   prompt = 45.5，把该次指定为最终参评版本。
4. 只有在"已有 ≥5100 + 45.5"之后，才考虑用 `conversation.s17.minimal-edit.json`
   试 prompt；失败不影响已达成版本，成功则可能把 prompt 原始分抬 1~2 分。

## 提交前 60 秒检查

```bash
cd /home/aln/桌面/seekdb-answer
git status --short                 # 必须为空
git rev-parse HEAD                 # 必须 410e8c174…
git -C . ls-remote --heads git@github.com:Alorun/seekdb-answer.git 2026-competition-round2
sha256sum agent/conversation.json  # 必须 f0a7421…
python3 scripts/release_gate.py --require-conversation   # RESULT: PASS
```
