# 交给下一个 agent 的任务提示词（可直接粘贴）

## 0. 一句话任务

在 seekdb × AMD hipVS CAGRA 赛题里，把 `CREATE VECTOR INDEX ... lib=cuvs` 的端到端构建时间
做到相对冻结 V5 基线 **3.0x** 以上（当前最好 1.731x / 12.99 分），同时保持：
Recall 硬门槛、查询吞吐 ≥0.9× 基线（最好 ≥1.5×）、显存 ≤45GB 且 ≤1.25× 基线。
**不要**为了速度牺牲任何门槛：任一门槛失败该次提交 0 分。

## 1. 环境（AMD notebook，非 Docker）

- 128 核 / 503GB 内存 / 单卡 AMD gfx1100（`rocm-smi` 可见），ROCm 7.2.4 在 `/opt/rocm`，hipVS 在 `/opt/hipvs`。
- 官方依赖已预置：`/opt/seekdb-deps-cache`；仓库 `deps/init/dep_create.sh` 秒级完成（会软链 `deps/3rd`）。
- **JupyterLab 的 Terminal 打开会 403**，只能用 notebook 的 Python cell；长任务必须用
  `subprocess.Popen(..., start_new_session=True)` 脱离内核，否则内核崩了任务就没了。
- 编译：

```bash
cd <repo>
export HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so      # 3 分钟
cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb -j16                        # 首次 25-40 分钟，增量几分钟
```

`-j` 不要超过 32（unity 单元峰值 3–4G，123 并发会把 Jupyter 一起 OOM 掉）。

## 2. 评测与仓库现状

- 评分：`4(门槛) + 14·clamp(ln B/ln 3) + 2·clamp(ln Q/ln 1.5)`，
  `B = B_sift^0.4·B_hidden^0.6`（每条目查询加速是 Top-10/Top-100 的几何平均）。
- 冻结基线：`zihaomu/seekdb` 分支 `amd-oceanbase-official-contest-base-v5`，commit `634888d9b`。
- 当前最好成绩（评测机实测）：分支 `ladder/it50bulk`，commit `eef08916b`，
  **build 1.731x / query 1.781x / 全部门槛 pass = 12.99 分**。这是保底，不要覆盖它。
- 该分支已实现（可复用）：
  - `tools/hipvs_bridge/kernel_build/custom_builder.hip`：自定义 CAGRA 建图入口（NN-descent 或 IVF-PQ 或 graft），
    后面接 `vendor/.../cagra/graph_core.cuh` 的 `sort_knn_graph` + `optimize`（哈希剪枝 + 反向边）→ `cuvsCagraIndexFromArgs`；
  - `tools/hipvs_bridge/seekdb_cuvs_bridge.c`：C ABI、建图/搜索调参开关（`SK_CUVS_*` 环境变量）、搜索缓冲复用、阶段计时写 `$SK_CUVS_TIMING_FILE`；
  - DDL 批量 append（`src/storage/ddl/ob_ddl_pipeline.cpp` 的 `append_row_file` + `is_cuvs_index()`）；
  - `tools/bench/`：可复现测量（250K 导入、冷 DDL 计时、首查计时、4096 条单行 SQL 的 Top-10/Top-100 QPS 与精确 GT Recall、分段计时）。
- 报告：`docs/gpu-vector-index-hipvs-cuvs/contest-build-report.md`（含所有已排除路径）。

## 3. 已经量出来的关键事实（不要再重复测）

评测机（用 `it100=1.108x`、`it50=1.731x` 两组官方结果反解）：

```
build(Hidden 30s 基线) = DDL ≈3.1s + GPU建图
GPU建图(50 迭代, int 384) ≈14.2s      → 1.731x
GPU建图(100 迭代)         ≈25.7s      → 1.108x
```

本机（128 核 / gfx1100，`tools/bench` 分段）真实 SIFT250K×128：

```
it50 总 26.5s = GPU 22.5s + DDL 3.2s
GPU 22.5s = NN-descent 固定开销 ~7.5s + 50×0.29s/轮 + prune ~2s + serialize/LOB ~0.5s
DDL 3.2s  = 扫表+提交 ~2.9s + append 0.34s（批量 append 之后）
```

结论：

- **3x 需要评测机 GPU 建图 ≤6.9s**，而 NN-descent 固定开销 4.4s + 50 迭代（recall 下限）9.8s → 结构上到不了；
- **Hidden R@100 的悬崖在 50 迭代**：45 迭代官方实测 0 分（`hidden250k:recall@100_baseline_delta`），
  30/20 迭代同样挂；加宽搜索束（`search_width=16`）也补不回来；
- DDL 已经不大了（3.1s），批量 append 只再省得动 0.3s 级；
- `search_width` 从 4 提到 16 让查询快 1.78x（CAGRA 每轮扩展 width 个父节点，width 大反而收敛快），
  但查询分已经封顶（1.5x），它只能用来给 recall 留余量。

## 4. 已实测排除的路径（照抄原因，别再试）

| 尝试 | 结论 |
| --- | --- |
| 降迭代 45/40/30/20、降度数 | Hidden R@100 挂（悬崖在 50） |
| `itopk` 768/1024 | 查询直接失败/超时（固定库搜索约束，只能 ≤512） |
| `search_width` 16/32 | 查询更快，但补不回低迭代的 recall |
| 并行 DDL（`_force_parallel_ddl_dop>1`） | 建图任务与 append 流水线是 scan 任务的兄弟节点，会竞态 → 索引不完整 → recall 挂 |
| 库 `ITERATIVE_CAGRA_SEARCH` | 49s（慢 2×），recall 略低 |
| 库 `IVF_PQ`（默认 / 手配 `cuvsIvfPqParams`） | recall 0.016 / 0.29，图是坏的 |
| 自写 IVF-PQ 候选图（`cuvsIvfPqBuild`+`cuvsIvfPqSearch`+精确排序） | 250K 查询、k=图宽度时每查询要排序候选列表 → GPU 100% 跑分钟级 |
| 窄列表深迭代（int 128，60/80/100 轮） | 成本 = 固定 ~7.5s + 每轮 ~0.3s，度数几乎不影响成本 |
| 两套 NN-descent 实现（库 vs vendored GNND） | 速度一致（26.3 vs 25.9s） |
| 子采样 NN-descent + CAGRA 搜索嫁接（1/4、1/8） | 29.8s（更慢）且 recall 0.924（更差）；对 250K 点做一次全量搜索 ≈ 一次 NN-descent 的成本 |

## 5. 唯一还有 3x 余量的方向

**在仓库里手写一个 GPU kNN 构图器**（规则明确允许："允许在答题仓库中实现自定义 GPU 建图"），
绕开 NN-descent 的固定开销和迭代成本，然后复用现成的 `graph::optimize` + 固定库搜索：

1. 候选生成要廉价：例如按块（tile）扫描全库做精确 top-k，或用低比特近似先把候选压到 O(128/点)；
   预算目标：评测机 ≤6.9s，本机 ≤12s。
2. 图质量要顶住 Hidden R@100：本机先用 `tools/bench` 的 SIFT recall 当判据
   （control 是 0.9975/0.9985；掉到 0.99 以下基本没戏），再靠一次提测验证 Hidden。
3. 复用：`sort_knn_graph(res, L2Expanded, data_view, knn)`（精确距离重排）+ `optimize(...)`
   （哈希剪枝+反向边，`final_degree=128`）+ `cuvsCagraIndexFromArgs`（挂 FP32 数据集）。
4. 内存：单次搜索的 scratch 约为 `每查询 ~64KB`（itopk×width 相关），**必须按 2048 行分批**，
   否则 250K 查询一次性分配 ~16GB 直接 RMM OOM。

## 6. 工具用法（本地迭代，10 分钟一轮）

```bash
# 一次跑多个参数组合，输出 build_ms / recall / qps / 分段计时
tools/bench/run_contest_sweep.sh \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --work /work/sweep --memory 32G --skip-syn --port 2991 \
  --sift-base /work/sift_base.fbin --sift-query /work/sift_query.fbin \
  --extra-env "SK_CUVS_TIMING_FILE=/work/sweep/timing.log" \
  --configs "cur:50:256:384:128 cand:30:256:384:128:512:16"
```

- `--configs "[label:]iter:degree:internal:final[:itopk[:width]]"`；
- 没有真 SIFT 时会自动造合成 128d（能看速度和"图是否坏"，不能代替 Hidden）；
- 关键日志行：`SEEKDB_GPU_BUILD`（nnd/sort/prune/attach）、`SEEKDB_BRIDGE_BUILD`、
  `SEEKDB_BRIDGE_SERIALIZE`、`SEEKDB_CUVS_APPEND calls/rows`、`SEEKDB_DDL_PHASE`（scan/append/build/write 时间线）。

## 7. 提测流程

1. 本地验证后提交到 GitHub（SSH 可用：`git push git@github.com:Alorun/seekdb-answer.git <branch>`；
   HTTPS 里那个令牌是只读的，推送会 403）；
2. 平台填「分支 + commit」，一次一个；
3. **只有最高分算数**，失败不扣已有分数，但每次约 30 分钟，别浪费；
4. 提交后看 `build=?x / query=?x / gates=?`，门槛失败会带 `testCaseResult.message`（例如 `hidden250k:recall@100_baseline_delta`）。

## 8. 纪律

- 一次只改一个变量；先在本地 `tools/bench` 看 build 和 SIFT recall，再提测；
- 不要动 `ladder/it50bulk`（`eef08916b`）——那是 12.99 分的保底；
- 任何新代码路径都用环境变量开关（默认保持旧行为），这样一次编译能 A/B；
- 编译在 3 分钟内能发现的问题（未声明符号、rvalue 引用、`MEMCPY` 之类）一定要本地先过，别拿提测当编译器；
- 修改 `custom_builder.hip` 后只需重编 bridge（3 分钟），不用重编 seekdb；改 seekdb 源码才需要增量重编。
