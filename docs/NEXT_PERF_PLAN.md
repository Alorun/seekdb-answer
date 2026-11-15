# S5 之后的性能细节审计与下一步

## S5 基线

- 平台 E = **4706.698345**；旧最好 4448.644112；+258。
- 目标 5500；差 **794**。prompt 28.5 暂缓，先做性能。

## 关键事实：判题器是“一发一收”

官方 `STDIO_PROTOCOL.md` 明确：

> 判题器按“发一个请求、等一个响应”执行。

所以以下两条**不能**作为性能杠杆，不要浪费时间：

- 查询批处理/多请求共享扫描：driver 不会在收到上一个响应前发下一个；
- 写组提交跨批合并 fsync：每批 100 条提交后 driver 等响应，100 次 fsync 省不掉。

性能只能从**单请求服务端 CPU + 单批 fsync + B/M**里挖。

## 本地分项（10k×128，最终代码）

### 查询

| 项 | uniform exact | 说明 |
|---|---:|---|
| server exec | 36.8 µs | `MVDB_PROFILE` |
| q7 扫描 | 19.4 µs | 53% |
| JSON/SQL parse | 4.5 µs | 12% |
| format | 2.5 µs | 7% |
| dispatch/exact/pool | 10.4 µs | 28% |
| 客户端/Docker 管道 | ~94 µs | Docker q1000 131 ms |

- filtered 查询：客户端中位数 33.6 µs，服务端约 12–15 µs；parse+format 约 7 µs，
  占 filtered 的一半以上。
- clustered exact：40.9 µs；clustered ann：19.2 µs（仅 ann 模式；exact 配置不生效）。

### 写入

| 项 | 值 | 说明 |
|---|---:|---|
| 总 wall | 244–266 ms | 100 批 × 100 行 |
| fdatasync | ~160 ms | 100 次，不可省 |
| exec（行/向量/WAL 编码） | 64 ms | 26% |
| parse（JSON 10.8 + SQL 19.4） | 30 ms | 12% |
| 客户端/管道 | ~10 ms | |

### B / M

| 指标 | 值 | 组成 |
|---|---:|---|
| B | 5.26 MB | WAL payload 4.44 + 头/CRC/页 ~0.8；原始向量 5.12 |
| M | 13.4 MB | 向量 5.12 + q8 1.28 + filter block 1.28 + rows/map ~1.6 + locations 0.5 + 进程 ~4.2 |

## 先做零代码实验：性能配置切 ann

代码已经内置 q7 IVF ann：uniform 的 ann 就是 exact q7 路径（无损失）；clustered 的 ann
server exec 19.2 µs vs exact 40.9 µs，initial/UPDATE+DELETE/restart recall 均 1.0。
S5 是 exact 配置（E=4706.698345）；如果平台允许在提交配置里选 ann，且性能正确性只按
Recall@10 ≥0.95 检查，**切 ann 是最大的一次单次收益**，uniform 数据下也不会变慢。

- 风险：功能用例仍按各自请求的 `search_mode`；ann 配置只影响性能 benchmark。
- 如果官方把 exact 作为硬性配置/性能按 exact 校验，回退 exact。
- 操作：下一次正式提交把查询模式设为 `ann`，只比较 E；不升或回退就继续下面的细节优化。

## S6 已做结果（2026-11-15）

| 项 | 结果 | 决定 |
|---|---|---|
| #1 fast SELECT | 4 次中位数 parse 7.83→6.41 µs（-18%）；wall 80.6→80.5（同一次噪声内）；18 条 SQL 差分 0 | 保留 |
| #3 直接向量解析 | 写 exec 593.5→547.8 µs/请求（-7.7%）；wall 238.6→233.0 ms（-2.3%）；20000 随机+维度/malformed 差分 0 | 保留 |
| #6 q7 64B 对齐 | 4 次中位数 76.7→78.1 µs，差异在噪声内 | **回退，不提交** |

fast SELECT 的 parse 收益真实存在，但 wall 被客户端/扫描淹没，不能按 18% 线性外推；
直接向量解析的 exec 收益稳定，wall 受 fdatasync 限制；q7 对齐没有稳定收益，已回退。
S6 代码 commit `8289a36d`，29 单元/raw 49.5。

## 可挖的细节（按优先级）

| # | 优化 | 预期 | 风险 | 说明 |
|---:|---|---|---|---|
| 1 | **fast SELECT parser**：只匹配 benchmark 的 `SELECT id, L2_DISTANCE(v,'...') [AS d] FROM t [WHERE c='x'] ORDER BY ... LIMIT n`，不匹配走原 parser | filtered 查询 -25~30%，unfiltered -8~10%；E +40–80 | 中 | 严格 pattern + fallback；public/JSONL/SQL edges 全跑 |
| 2 | **fast INSERT parser + zero-copy JSON sql**：benchmark 的 `INSERT INTO t VALUES (...),(...);`，SQL 字段不复制 | 写 CPU -15~20 ms；T_write -6~8%；E +25–40 | 中 | 只处理简单字面量；列名/转义/注释/多语句全部 fallback |
| 3 | **bulk vector literal parse**：一次扫描 100 个 `'[...]'`，复用十进制快路径 | 写 exec -5~10 ms；E +10–20 | 低-中 | 与 `parseVectorLiteral` 位级差分 |
| 4 | **M：filter block 改回 row-id 索引**（释放 1.28 MB） | M 13.4→12.1；E +30–50；filtered 查询慢约 5% | 中 | 净收益小；先 A/B 再决定 |
| 5 | **M：row map/Row 改 arena + 排序 vector** | M -1.0~1.5 MB；E +40–70 | 高 | 碰持久化/undo/pk 顺序；必须全量回归 |
| 6 | **q7 扫描 kernel**：两遍/预取/q7Dot4 微调 | 19.4→17.5 µs；E +20–30 | 低-中 | 两遍本地 35.8 vs 36.8，2.7% 在噪声内；要更多样本 |
| 7 | **WAL 编码**：简化 Huffman/位平面 | B -0.2~0.4 MB；E +15–25 | 中 | D7 显示 raw 会 B 4.84→5.69；不要直接关 Huffman |
| 8 | q8 4-bit / AVX-512 / float32 exact | 本地都不赚 | 高 | 4-bit 更慢；AVX 是 f3fb066 的 0 分路径；float32 3% |
| 9 | 查询批处理 / 写组提交 | — | — | driver 一发一收，不可行 |

## 现实目标

- 1+2+3+6+7 的安全合计约 **+100–170**；E 约 **4800–4900**。
- 再加 4/5 的 M 优化约 **+70–120**；E 约 **4900–5000**。
- 5500 仍需要 top 队的 **更低 M/B + 更快单请求引擎**；本地方向只能先把 794 里的
  200–300 拿回来，不能再靠 exact 的 2–3% 微调。

## 下一步

1. 实现 #1 fast SELECT parser，先对 filtered/unfiltered 1000 查询做 A/B；只保留稳定 >8% 的版本。
2. 实现 #2/#3 写路径，测 T_write 与 B/M；任何 parse 改动都必须过 public 20、JSONL 20/95、SQL edges 158×2。
3. 做 #5 的 M 设计和单独 worktree 原型；不直接改主分支。
4. 每步都跑 release gate + 官方 JSONL 20；性能 A/B 用同机同数据 5 次中位数。
