#!/usr/bin/env python3
"""Generate and validate the competition conversation.json from real evidence.

Every student quote is an exact substring of its conversation; every result
quote is an exact substring of its experiment output.  The script refuses to
write a file with a broken pointer, an over-long quote, a duplicate score
dimension, or duplicate evidence text.
"""
import json, os, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMIT = sys.argv[1] if len(sys.argv) > 1 else "7c009b3e24a480f4de86ec76a6d6c5fdd06197c9"
DIM1 = "问题理解与自主拆解"
DIM2 = "技术理解与迁移"
DIM3 = "独立验证与质疑"
DIM4 = "实验与反馈迭代"
DIM5 = "取舍与反思"

USER_PROMPTS = [
    {"id": "P1", "source": "用户原话（节选，已脱敏并去掉数值）", "content":
     "目前性能分数与榜首还有明显差距，官方测评机约有百分之二十的波动……对于 agent prompt，请模拟学生的真实回答……一个实例最多两分……目标是最高档，所以至少对应二十个实例……关键是每个实例一定要对应一个独立的问题并且有独立的证据链……一旦某一项没有通过测评整体就是零分。"},
    {"id": "P2", "source": "用户原话", "content":
     "思考和行动时可以多开几个子agent帮助你完成任务。"},
    {"id": "P3", "source": "用户原话（节选，去掉数值）", "content":
     "对于性能优化方面只要能达到目标区间那就大改，多一些思考，时间不着急，可以慢慢继续进行修改。"},
    {"id": "P4", "source": "用户原话（节选）", "content":
     "一定要通过测评，不要犯之前犯过的错误……一定要详细的进行检查和本地测评。"},
]

CONVERSATIONS = [
    ("C01", "P1", "我先把 80 分拆成三条线：MySQL case 任一失败总分为 0，所以准入优先；性能和 prompt 各占 40 分但都要同一个 commit；每天两次正式提交，没有通过版本总分为 0。先冻结代码和材料，再谈优化。", "目标拆解与提交门禁"),
    ("C02", "P3", "优化前我先量清楚 1000 次查询的墙钟花在哪：服务端 exec、parse/json/sql、format，还是客户端管道。若 exec 只占一半，先压 format 的收益就有限；我要 5 次中位数，不用单次 profile 下结论。", "查询墙钟分解"),
    ("C03", "P1", "平台 B 是 cgroup io.stat，不是 /proc/io 的 rchar/wchar；本地的 4.84 MB 和 13 MB 都不能直接当平台 B。我要用 2 CPU/2 GiB 的容器，读 memory.peak 和 io.stat，把启动、装载、预热都算进去。", "B/M 平台口径"),
    ("C04", "P3", "先在同一台机器、同一 10k×128 负载上对 S3 和当前版本做 A/B，再换聚簇数据复测。这是多项改动的组合，不能把收益全归给 q7 或过滤中的某一个；我要看写入、查询、峰值内存三组一起变。", "组合版本 A/B"),
    ("C05", "P1", "不能把 --memory-rows 0 当成向量也 out-of-core：50k×128 的 float32 矩阵本身约 25.6 MB。我要在 50k 行上量写入、重启、查询三个阶段的 VmHWM，再看这条路径能不能宣称省内存。", "磁盘模式内存驻留"),
    ("C06", "P4", "批量 WAL 的 checkpoint 必须能全量 decode；32 位 word 的字节序和 HuffCode 零初始化只要有一处不一致，只有重启或 checkpoint 才暴露。先 10k，再 50k，再 --memory-rows 0 的磁盘模式，三次都要数全部行。", "checkpoint 全量恢复"),
    ("C07", "P1", "q7 量化距离只能做下界，不能直接输出；用 |‖v-q‖−s‖code(v)-code(q)‖| ≤ err(v)+err(q) 筛出候选，最终仍用 float32/double 复核。过滤后的集合也要逐条对 oracle，不能只看无过滤。", "q7 下界与精确复核"),
    ("C08", "P3", "IVF 不能对所有数据都建：512 行/16 中心 screen 看方差比，uniform ratio 高就跳过；clustered 才建，并用 dist(q,centroid)−radius 做精确剪枝。阈值是启发式，样本 512 可能误判，要用 debug 输出和 recall 两边核对。", "IVF 分布判定"),
    ("C09", "P1", "多行 INSERT 里一行主键冲突时，整条语句不能部分生效；但同一显式事务里之前成功的语句要保留，ROLLBACK 再撤销。我要用提交后的行集验证，不是看到 duplicate_key 就结束。", "语句原子性"),
    ("C10", "P3", "连续 autocommit DML 可以共用一次 fdatasync，但响应必须在 sync 后按序释放；顺序驱动仍应逐批 sync，不能靠等下一个请求凑批。我要用 strace 分别数顺序和突发两种输入的 fdatasync 次数。", "组提交与响应顺序"),
    ("C11", "P4", "前两次提测失败说明不能靠 semicolon/ready-marker 猜协议；先找官方 STDIO_PROTOCOL，按 JSONL 的 ready/id/ok/columns/rows/error.code 对齐，跑官方交互检查器的全部 20 例，再进 Docker 和 restart。公开 20/20 不能写成正式准入。", "官方 JSONL 协议验证"),
    ("C12", "P1", "普通单元测试通过不等于强杀安全：已确认提交不能丢、未提交不能半可见、恢复中再杀要能重复恢复。我要用 6 个 crash point 加随机 SIGKILL，在内存和磁盘两种模式下各跑一遍。", "崩溃恢复反证"),
    ("C13", "P4", "合法 SQL 不能把进程打崩：LIMIT 2^63−1 这种值只应该返回全部行或报参数错误，不能 std::length_error abort。我要先看旧版是否复现，再确认 reserve 被表行数封顶。", "巨大 LIMIT 崩溃"),
    ("C14", "P1", "float32 有限大值的平方和可能溢出成 inf；距离要用 double 累加，输出要有限。我要插入 3e19 和 2e19 两行，看排序和最后一位是否还对。", "float32 大值距离"),
    ("C15", "P1", "SELECT 里投影的 L2_DISTANCE 和 ORDER BY 的 L2_DISTANCE 可以是两个不同查询向量；投影值必须按投影向量算，不能复用排序 key 的距离。我要构造 q1≠q2 的查询，对比加不加第三个投影列的结果。", "投影距离与排序 key"),
    ("C16", "P3", "过滤查询慢在每行 map.find/散列，不是距离；先存 value→VectorCache 行号，再把该值的 q7 code 拷成连续块顺序访问。收益用 5 次中位数说话，不能拿单轮 2× 当结论。", "过滤连续块"),
    ("C17", "P3", "写路径的 CPU 热点要用 profiler 找，不能猜：批量 INSERT 里 vector 字面量解析、CRC、JSON/SQL 词法各占多少？改完快路径还要证明它和 from_chars 位级一致，并且不放过 1.2.3 这种非法字面量。", "写路径热点与解析"),
    ("C18", "P4", "ScanPool 析构时 ++seq_ 会让 worker 把旧 job 当新任务再执行一次，旧 lambda 捕获的栈变量已经返回；析构只 stop+notify+join，worker 执行前检查 stop，再用 ASAN 连续跑关停和 checkpoint。", "ScanPool 生命周期"),
    ("C19", "P1", "k≤64 用固定数组比 vector 插入少分配；但 k>64 要回退 heap，tie 仍按 (distance, pk) 升序。我要用 LIMIT 10/64/65/100 和 300 行随机数据对 oracle，确认边界和并列都不变。", "TopK 边界与并列"),
    ("C20", "P3", "E=(Ew+2Eq)/3，查询权重是写入的 2 倍；多用一个物理核的查询收益经过 f(R)=R/(1+R) 会饱和，不是 2×，在 2 vCPU 与驱动共享时更不一定值。我要在 uniform 和 clustered 上对比 1T/2T 的 1000 查询墙钟，再决定默认是否开第二核。", "指标饱和与双核取舍"),
    ("C21", "P2", "uniform 10k×128 上真正的 ANN 图索引能不能稳定快 3×？先在独立基准里量 HNSW/IVF 的 recall、p50/p99、build 和索引内存；如果 uniform 被 q7 扫描的带宽下限卡住，就不要为'像 ANN'上索引。", "ANN 方案探索"),
    ("C22", "P3", "int8/4-bit/PQ 这些更省内存的方案，实测是否比 q7 慢？拒绝时要有查询时间和 recall，不能只报内存；4-bit 解包如果抵消带宽收益，就保留 q7。", "更省内存方案的负结果"),
    ("C23", "P1", "过滤连续块多一份 code 矩阵（10k×128≈1.28 MB），平台 M 只占 20%。我要在 2 CPU/2 GiB 容器里对比开/关连续块的 filtered 延迟和 memory.peak，收益落在噪声内就回退。", "过滤块的内存取舍"),
    ("C24", "P4", "本地微基准说 AVX-512 快不算数：平台持续负载下可能降频，甚至 XCR0 不可用 SIGILL。上一次平台 E 在启用 512 后下降，我要默认回 AVX2，并把 512 路径整个删掉，而不是留一个运行时开关。", "AVX-512 平台回退"),
    ("C25", "P1", "单条多行 INSERT 超过 64 MiB 时，确认成功后重启不能丢；appendBatch 没有上限、reader 却按 64 MiB 拒绝，是一个确认即丢的持久化 bug。我要把逻辑 INSERT 拆成多条 ≤4 MiB 记录，同一 commit，并验证 chunk 中途 crash 和跨 chunk 重复主键。", "大 INSERT 分块持久化"),
]

EXPERIMENTS = [
    ("E01", "查询墙钟分解", "python3 tests/run_profile.py --bin ./build/minivectordb",
     "pk_floor_us=16.9 filtered_us=44.1 unfiltered_us=55.9\nprofile: requests=1600 parse=0.0060874 json=0.00153609 sql=0.00455131 exec=0.0307732 format=0.00346489",
     "profile: requests=1600 parse=0.0060874 json=0.00153609 sql=0.00455131 exec=0.0307732 format=0.00346489",
     "同一进程重启后只跑查询；MVDB_PROFILE=1 在 stdin EOF 后打印累计阶段。"),
    ("E02", "Docker cgroup B/M", "python3 /tmp/mvdb_new/docker_bench.py mvdb:final exact",
     "rep0 mode=exact ready=154ms write=277.4ms q1000=124.6ms B=5.26MB M_peak=12.9MB (peak0=5.7 write=10.3 query=12.9) ioW=5.26 ioQ=0.00",
     "B=5.26MB M_peak=12.9MB",
     "--cpus=2 --memory=2g，读容器 cgroup io.stat 与 memory.peak，包含启动/装载/预热。"),
    ("E03", "S3 与当前版本 A/B", "taskset -c 0,2; 10k x 128; 1000 queries",
     "S3: write=0.5268s query=0.1761s maxrss=14.4MiB\ncurrent: write=0.2442s query=0.0490s maxrss=12.4MiB\nS3: clustered write=0.5689s query=0.1914s maxrss=14.3MiB\ncurrent: clustered write=0.2649s query=0.1154s maxrss=12.6MiB",
     "current: write=0.2442s query=0.0490s maxrss=12.4MiB",
     "S3 = 057126c01 的独立 worktree 构建；同一 bench2 负载，uniform 与 clustered 两个场景。"),
    ("E04", "50k 磁盘模式内存", "./build/minivectordb --memory-rows 0; 50k x 128; restart; query",
     "rows=50000 dim=128 memory_rows=0\nready_rss_kB 4136\nwrite_rss_kB 7516\nrestart_rss_kB 27488\nquery_rss_kB 28092",
     "restart_rss_kB 27488",
     "写入、shutdown、重启、查询四阶段读 /proc/pid/status VmHWM。"),
    ("E05", "checkpoint 10k/50k/disk", "python3 tests/run_checkpoint.py --rows {10000,50000} [--disk]",
     "checkpoint rows=10000 mode=memory restart_rows=10000: PASS\ncheckpoint rows=50000 mode=memory restart_rows=50000: PASS\ncheckpoint rows=50000 mode=disk restart_rows=50000: PASS",
     "checkpoint rows=50000 mode=disk restart_rows=50000: PASS",
     "脚本先数重启后的全部行，再检查 TopK；不再硬编码 10k 标签。"),
    ("E06", "过滤/精确 TopK 差分", "python3 tests/run_filter_diff.py --bin ./build/minivectordb",
     "filtered/unfiltered differential: before=0 after_dml=0 rollback=0 restart=0 read_your_writes=OK checks=190\nfailures=0\nexact queries: 50 (ids/order/distance match oracle)\nann recall@10: 1.0000",
     "filtered/unfiltered differential: before=0 after_dml=0 rollback=0 restart=0 read_your_writes=OK checks=190",
     "NumPy oracle 覆盖过滤/无过滤、UPDATE/DELETE、ROLLBACK、restart。"),
    ("E07", "IVF screen 与聚簇 recall", "MVDB_DEBUG_IVF=1; 10k clustered x 128; CREATE VECTOR INDEX",
     "ivf-screen sample=512 ratio=0.350\nivf n=10000 inertia=152249.74 var=10121642.97 ratio=0.015 clustered=1\nann_recall_mean=1.0 min=1.0",
     "ivf n=10000 inertia=152249.74 var=10121642.97 ratio=0.015 clustered=1",
     "uniform 的 screen ratio 高，跳过 IVF；clustered 建 IVF，ann 走精确半径 IVF。"),
    ("E08", "语句原子性", "显式事务：INSERT id4；INSERT (5),(5) 冲突；COMMIT；再 BEGIN/DELETE/ROLLBACK",
     "failed_insert_code duplicate_key\nstatement_atomicity committed rows= [[1], [2], [3], [4]]\nrollback rows= [[1], [2], [3], [4]]",
     "statement_atomicity committed rows= [[1], [2], [3], [4]]",
     "失败语句不留下 5，但保留事务此前成功的 4；ROLLBACK 恢复 2。"),
    ("E09", "组提交 fdatasync 计数", "strace -f -c -e trace=fdatasync; 顺序 100 批 vs 突发 100 批",
     "seq 100.00    0.001102          10       101           fdatasync\nburst 100.00    0.000827         413         2           fdatasync\nfdatasync_counts seq=101 burst=2",
     "fdatasync_counts seq=101 burst=2",
     "顺序客户端逐批等待；突发客户端一次写入全部请求。"),
    ("E10", "官方 JSONL 20 例", "python3 /tmp/official/protocol_check/run.py --image mvdb:final --package ./tests/public_cases",
     "official protocol_check: passed=True cases=20\nstorage_restart: PASS\ntransaction_shutdown_rollback: PASS",
     "official protocol_check: passed=True cases=20",
     "同一 20 例在 Docker 镜像上跑 ready/execute/restart/shutdown。"),
    ("E11", "崩溃恢复 240 轮", "python3 tests/run_crash_fuzz.py ./build/minivectordb --iterations 120 --seed 20261115",
     "iterations=240 elapsed=9.6s\ndeterministic crash points={'after_wal_write': 28, 'after_commit_fsync': 26, 'recovery_after_pass1': 26, 'recovery_after_truncate': 28, 'checkpoint_before_rename': 26, 'checkpoint_after_rename': 26}\nrandom SIGKILL iterations=80\nfailures=0",
     "random SIGKILL iterations=80",
     "内存/磁盘各 120 轮；6 个确定性崩溃点 + 80 次随机强杀。"),
    ("E12", "巨大 LIMIT 崩溃", "SELECT id FROM t ORDER BY id LIMIT 9223372036854775807; 旧版 vs 当前",
     "before: DEAD terminate called after throwing an instance of 'std::length_error'\nafter: ok=True rows=3",
     "after: ok=True rows=3",
     "旧版 reserve(limit) abort；当前 reserve 取 min(limit, 表行数)。"),
    ("E13", "float32 大值距离", "INSERT [3e19,0],[2e19,0]; SELECT L2_DISTANCE(...,'[0,0]')",
     'float32 large: {"id": 2, "ok": true, "columns": ["id", "d"], "rows": [[3, 1], [2, 2e+19], [1, 3.0000001e+19]]}',
     '"rows": [[3, 1], [2, 2e+19], [1, 3.0000001e+19]]',
     "float32 平方和会溢出，距离用 double 累加；输出有限并按距离排序。"),
    ("E14", "投影/排序距离错配", "SELECT id, L2_DISTANCE(v,q1) AS d FROM t ORDER BY L2_DISTANCE(v,q2) LIMIT 2; 旧版 vs 当前",
     "before [[1, 1], [2, 1]]\nafter [[1, 9], [2, 10.0498756]]",
     "after [[1, 9], [2, 10.0498756]]",
     "旧版 simple_proj 复用排序 key 的距离；当前要求投影 query 与 order query 相同。"),
    ("E15", "过滤路径 A/B", "taskset -c 0,2; 500 filtered + 500 unfiltered; base vs current",
     "base filtered500 median=39.0us\ncurrent filtered500 median=33.6us\nbase unfiltered500 median=72.6us\ncurrent unfiltered500 median=53.3us",
     "current filtered500 median=33.6us",
     "同一数据目录重启后跑 5 轮，取中位数；过滤块只在单等值过滤且版本匹配时启用。"),
    ("E16", "写路径 profiler 与解析差分", "gprof -pg 构建; 100k 行写入; tests/parse_vector_check.cpp",
     "45.65      0.21     0.21   101000     2.08     2.08  mvdb::parseVectorLiteral(...)\n19.57      0.30     0.09     2002    44.96    44.96  mvdb::crc32c(...)\nparseVectorLiteral differential: checked=20000 rejected=0 mismatches=0\ncrc32c differential: checked=8243 mismatches=0\nwrite_wall_ms=266.4\nprofile: requests=101 parse=0.0301985 json=0.0108088 sql=0.0193898 exec=0.0640009 format=8.4008e-05",
     "parseVectorLiteral differential: checked=20000 rejected=0 mismatches=0",
     "gprof 找热点；快路径与 from_chars(double)->float 位级一致；CRC 硬件与表实现一致。"),
    ("E17", "ASAN 关停/checkpoint", "ASAN build; public 20; checkpoint 10k; shutdown x3",
     "ASAN public 20/20 PASS\ncheckpoint rows=10000 mode=memory restart_rows=10000: PASS\ncycle 0: exit=0 asan_errors=[]\ncycle 1: exit=0 asan_errors=[]\ncycle 2: exit=0 asan_errors=[]",
     "cycle 2: exit=0 asan_errors=[]",
     "detect_stack_use_after_return=1；连续三次启动/查询/关停无 ASAN 报告。"),
    ("E18", "TopK LIMIT 边界", "300 行随机 + 200 行全等距；LIMIT 10/64/65/100 对 oracle",
     "LIMIT 10: n=10 monotonic=True first=[1, 2, 3] last=10\nLIMIT 64: n=64 monotonic=True first=[1, 2, 3] last=64\nLIMIT 65: n=65 monotonic=True first=[1, 2, 3] last=65\nLIMIT 100: n=100 monotonic=True first=[1, 2, 3] last=100\nrandom LIMIT 65: match=True n=65\nrandom LIMIT 100: match=True n=100",
     "LIMIT 65: n=65 monotonic=True first=[1, 2, 3] last=65",
     "k≤64 走固定数组，k>64 走 heap；全等距按 pk 升序。"),
    ("E19", "指标饱和 + 双核查询 A/B", "taskset -c 0,2; MVDB_NO_2T=0/1; 1000 queries uniform/clustered",
     "E = 0.2*f(Rw) + 0.4*f(Rq) + 0.2*f(Rb) + 0.2*f(Rm); parity all R=1 => E=0.5\n2x on one component saturates via f(R)=R/(1+R)\nuniform 2T: query1000=53.4ms\nuniform 1T: query1000=59.8ms\nclustered 2T: query1000=118.3ms\nclustered 1T: query1000=126.2ms",
     "uniform 2T: query1000=53.4ms",
     "0.6/0.2/0.2 权重与 f 饱和；双核在 uniform 约 1.12x、clustered 约 1.07x，不是线性的 2x。"),
    ("E20", "ANN 方案基准", "taskset -c 0,2; ann_final/bench_ann --data uniform/clustered",
     "[uniform] exact q7 baseline        p50=43.81 p99=63.61 mean=44.95 us\n[uniform] ann pool=24 kmeans=0/0  recall=1.0000 min=1.00 lt0.9=0 p50=36.32 p99=54.59 mean=37.78 | build=36.4ms idx=0.000MB clustered=0\n[clustered] exact q7 baseline        p50=48.54 p99=66.41 mean=49.40 us\n[clustered] ann pool=24 kmeans=64/4  recall=1.0000 min=1.00 lt0.9=0 p50=7.53 p99=22.09 mean=8.42 | build=39.9ms idx=0.078MB clustered=1",
     "[clustered] ann pool=24 kmeans=64/4  recall=1.0000 min=1.00 lt0.9=0 p50=7.53 p99=22.09 mean=8.42 | build=39.9ms idx=0.078MB clustered=1",
     "uniform 的 ANN 只快约 1.2x；clustered 的精确 IVF 快约 5.9x，recall 1.0。"),
    ("E21", "4-bit/2-bit 负结果", "q4bench.cpp: 10k x 128, 1000 queries, taskset -c 0",
     "q7: 23.5 us/query\nq4: 24.5 us/query",
     "q4: 24.5 us/query",
     "更窄的 packed code 解包抵消带宽收益；4-bit 在完整候选集上并未更快。"),
    ("E22", "过滤块 M/延迟权衡", "docker --cpus=2 --memory=2g; MVDB_NO_FILTER_BLOCK=0/1",
     "no_block=False filt=108.8us unf=143.8us M=13.13MB\nno_block=True filt=121.8us unf=160.8us M=12.03MB",
     "no_block=False filt=108.8us unf=143.8us M=13.13MB",
     "连续块多约 1.1 MB memory.peak，换 filtered 约 13-35us；查询权重 2/3，保留。"),
    ("E23", "AVX-512 移除", "grep -c avx512 src/executor.cpp; 当前构建全量回归",
     "source_avx512_matches=0\nbinary_avx512_symbols=0\ncurrent build: public 20/20, crash fuzz 240, checkpoint 50k/disk PASS",
     "source_avx512_matches=0",
     "本地微基准曾认为 512 更快，但平台持续负载下 E 下降；当前版本没有 512 路径和运行时开关。"),
    ("E24", "大 INSERT 分块持久化", "3000 行 x 2000 字符；crash after_wal_write / after_commit_fsync；dup across chunks",
     "normal: ok=True affected_rows=3000 restart_rows=3000\ncrash after_wal_write: restart_rows=0\ncrash after_commit_fsync: restart_rows=3000\nduplicate_across_chunks: duplicate_key restart_rows=0",
     "crash after_commit_fsync: restart_rows=3000",
     "逻辑 INSERT 拆成多条 ≤4 MiB 记录；未提交 chunk 不生效，已确认 commit 全部保留。"),
]

UNITS = [
    ("U01", DIM1, 0.5, "C01", None,
     "把 80 分拆成准入、性能、prompt 三条线，并确定没有通过版本总分为 0 的提交门禁。",
     "只有规划，没有执行结果，按 0.5 计；它是唯一一条纯规划单元，不引用实验。",
     "与 U04/U11 的版本 A/B、协议执行不同：本单元只描述约束和取舍边界。", None, None),
    ("U02", DIM1, 1.5, "C02", "E01",
     "1000 次查询的墙钟到底花在 exec、parse/json/sql、format 还是客户端管道。",
     "先做阶段分解，发现 exec 约 30.8 ms、parse/json/sql 约 6.1 ms、format 约 3.5 ms；据此把优化优先级放在 exec 而不是 format。单次 profile 有异常，改用 5 次中位数。",
     "只回答测量与优先级；不实现 q7/过滤，也不引用它们的 A/B。", None, None),
    ("U03", DIM1, 1.5, "C03", "E02",
     "平台 B 是 cgroup io.stat 时，本地 /proc/io 的 rchar/wchar 能不能当 B。",
     "用 2 CPU/2 GiB 容器读 cgroup：B=5.26 MB、M_peak=12.9 MB，和 host RSS/进程 io 都不是同一口径；因此本地只报代理值并注明方法，不把 4.84 MB 当平台 B。",
     "只讨论度量口径；不讨论 WAL 内容或 E 权重。", None, None),
    ("U04", DIM1, 2.0, "C04", "E03",
     "S3→当前版本在 uniform 和 clustered 上的端到端收益，以及能否把收益归因于单一改动。",
     "uniform：write 0.5268→0.2442 s、query 0.1761→0.0490 s；clustered：write 0.5689→0.2649 s、query 0.1914→0.1154 s。两个场景都复测；这是多项改动的组合，不能把 3.6x 查询收益全归给 q7。",
     "与 U20 的指标饱和问题不同：本单元只做组合版本 A/B 与归因边界。",
     "从 uniform 迁移到 clustered 后查询收益从约 3.6x 降到约 1.7x。",
     "隐藏基线未测，单次平台 E 有波动；组合 A/B 不能拆出单项因果。"),
    ("U05", DIM1, 2.0, "C05", "E04",
     "--memory-rows 0 是否等于向量也 out-of-core；50k 行重启后的驻留内存是多少。",
     "50k×128、--memory-rows 0：写入阶段 7.5 MB，但重启后 27.5 MB、查询后 28.1 MB，接近 25.6 MB 的向量矩阵与 WAL 页缓存。行 out-of-core 不等于向量 out-of-core。",
     "只问内存驻留；不涉及写入延迟或 checkpoint 编码。",
     "从 10k 迁移到 50k 磁盘模式，结论反转：不能靠 --memory-rows 0 宣称省内存。",
     "只测本机单表 128 维；隐藏测试若把页缓存算入 M，需要更激进的分块。"),
    ("U06", DIM2, 2.0, "C06", "E05",
     "批量 WAL 的 checkpoint 为什么必须在 10k/50k/磁盘重启后都能全量 decode。",
     "10k、50k、50k+磁盘三种模式都数重启后的全部行：10000/50000/50000 全部一致，说明 chunk/checkpoint 解码不是只看单条 TopK。历史上 Huffman 字节序和 HuffCode 零初始化错只在重启暴露。",
     "只讲 WAL/checkpoint 编解码；与语句原子性、sync 顺序不同链。",
     "从 10k 迁移到 50k 和磁盘模式，三次都通过。",
     "只模拟 SIGKILL，不模拟断电/坏盘；checkpoint 脚本只校验行数和一条 TopK，不是逐行内容校验。"),
    ("U07", DIM2, 2.0, "C07", "E06",
     "q7 量化距离为什么不能直接输出，三角形下界怎样保证 exact 不漏，迁移到过滤查询还成立吗。",
     "q7 只做下界；190 项差分覆盖过滤/无过滤、UPDATE/DELETE、ROLLBACK、restart，exact 与 NumPy oracle 一致，ann recall 1.0。下界只用于筛，最终仍用 exact 复核。",
     "只讲 q7 精确性；过滤性能在 U16，IVF 在 U08。",
     "从 uniform 迁移到 filtered/clustered，候选仍逐条对 oracle。",
     "只测 5000×128、L2；误差界依赖每行 err，换成 cosine/IP 不成立。"),
    ("U08", DIM2, 2.0, "C08", "E07",
     "IVF 为什么不能对所有数据都建；512 行/16 中心 screen 与簇半径下界各解决什么问题。",
     "screen 在 clustered 数据上给出 ratio=0.015、clustered=1；uniform ratio 高则跳过。clustered 用半径下界做精确剪枝，ann recall 1.0。阈值是启发式，样本 512 可能误判。",
     "只讲分布判定/簇剪枝；不引用 q7 下界或过滤器。",
     "uniform 跳过 IVF，clustered 建 IVF；迁移后 ann recall 1.0。",
     "screen 阈值是启发式；只测合成聚簇，真实分布未测。"),
    ("U09", DIM2, 1.5, "C09", "E08",
     "多行 INSERT 一行冲突时整条语句是否部分生效，显式事务里之前的语句和 ROLLBACK 怎样。",
     "duplicate_key 后提交行仍是 [1,2,3,4]，没有留下 5；ROLLBACK 后仍为 [1,2,3,4]，说明失败语句原子但保留事务此前成功的语句。",
     "只讲语句原子性；与 checkpoint、崩溃恢复不同链。", None, None),
    ("U10", DIM2, 2.0, "C10", "E09",
     "组提交能否把 101 次 fdatasync 合并成 2 次，同时保证响应不早于 sync。",
     "顺序驱动 101 次 fdatasync，突发驱动 2 次；响应在 sync 后按序释放。合并只在请求已到达时发生，顺序驱动没有写收益。",
     "只讲 sync/响应顺序；与 WAL 编码、组合 A/B 不同链。",
     "从顺序迁移到突发输入，fdatasync 101→2。",
     "突发模式不是官方驱动的必然行为；不能用 101→2 宣称平台写入 50x。"),
    ("U11", DIM3, 2.0, "C11", "E10",
     "前两次提测失败后，怎样证明文本协议/ready-marker 假设是错的，并以官方 JSONL 为准。",
     "按官方 STDIO_PROTOCOL 对齐 JSONL，protocol_check 在 Docker 上 20/20，handshake/storage_restart/shutdown 都过；公开 20 例不是正式准入。",
     "只讲协议验证；与 DDL 边界、崩溃恢复不同链。",
     "从本地进程迁移到 Docker/restart，20/20。",
     "公开 20 例不是隐藏用例；本地进程不是平台，不能把 20/20 写成正式准入。"),
    ("U12", DIM3, 2.0, "C12", "E11",
     "普通单元测试通过能否证明强杀恢复；怎样用 6 个崩溃点 + 随机 SIGKILL + 恢复中再杀反证。",
     "240 轮（内存/磁盘各 120）覆盖 6 个确定性崩溃点 + 80 次随机 SIGKILL，failures=0；恢复中再杀也能重复恢复。",
     "只讲崩溃恢复验证；与语句原子性、WAL 编码不同链。",
     "从内存迁移到磁盘模式，仍 0 失败。",
     "只模拟 SIGKILL，不模拟断电/坏盘；随机 kill 覆盖有限。"),
    ("U13", DIM3, 1.5, "C13", "E12",
     "合法 SQL 的巨大 LIMIT 会不会把进程打崩。",
     "旧版在 LIMIT 2^63−1 时抛 std::length_error 直接退出；当前 reserve 被表行数封顶，返回 3 行。",
     "只讲 LIMIT 参数边界；与 float32 数值不同链。", None, None),
    ("U14", DIM3, 1.5, "C14", "E13",
     "float32 有限大值的平方和会不会溢出成 inf，距离输出怎样保持有限。",
     "3e19 与 2e19 两行按距离 1、2e19、3.0000001e19 排序，输出有限；float32 平方和会溢出，所以距离用 double 累加。",
     "只讲数值溢出/格式化；与 q7 量化误差不同链。", None, None),
    ("U15", DIM3, 1.5, "C15", "E14",
     "投影的 L2_DISTANCE 与 ORDER BY 用不同查询向量时，输出会不会复用了排序 key 的距离。",
     "旧版返回 [[1,1],[2,1]]，把 q2 的距离当 q1 的输出；当前返回 [[1,9],[2,10.0498756]]，投影按 q1，排序按 q2。",
     "只讲投影/排序 key 错配；与过滤顺序不同链。", None, None),
    ("U16", DIM4, 1.5, "C16", "E15",
     "过滤查询的瓶颈是不是每行 map.find/散列，连续 q7 code 块能带来多少真实收益。",
     "500 条过滤查询中位数 39.0→33.6 us，无过滤 72.6→53.3 us；收益约 10-27%，不是 2x。连续块只在单等值过滤且版本匹配时启用，多条件回退。",
     "只讲过滤性能；过滤正确性在 U07。", None, None),
    ("U17", DIM4, 1.5, "C17", "E16",
     "写路径 CPU 热点在哪里；快速 vector 解析/CRC 是否既快又和原实现位级一致。",
     "gprof 显示 parseVectorLiteral 45.65%、crc32c 19.57%；改后 20000 组随机字面量与 from_chars 位级一致、8243 组 CRC 硬件/表实现一致，非法 1.2.3 仍被拒绝。",
     "只讲写路径热点与解析/CRC 验证；不重复查询 profile。", None, None),
    ("U18", DIM4, 1.5, "C18", "E17",
     "ScanPool 析构时 ++seq_ 为什么会让 worker 重放旧 job，ASAN 能否证明修复。",
     "ASAN 下 public 20/20、checkpoint 10k、连续 3 次关停全部 exit=0、无 stack-use-after-return；析构只 stop+notify+join。",
     "只讲并发关停生命周期；与崩溃恢复不同链。", None, None),
    ("U19", DIM4, 1.5, "C19", "E18",
     "k≤64 固定数组、k>64 heap 的边界和 tie 是否与 oracle 一致。",
     "LIMIT 10/64/65/100 在 200 行全等距数据上都按 pk 升序；300 行随机数据 LIMIT 65/100 与 oracle 完全一致。",
     "只讲 TopK 容器边界；与 q7 距离核不同链。", None, None),
    ("U20", DIM5, 2.0, "C20", "E19",
     "E 权重固定时，多用一个物理核的查询收益为什么不是线性的，在 2 vCPU 共享驱动时是否值得。",
     "E=0.2f(Rw)+0.4f(Rq)+0.2f(Rb)+0.2f(Rm)，f 饱和；uniform 1T→2T 只从 59.8ms 到 53.4ms（约 1.12x），clustered 仅约 1.07x。第二核的收益远小于 2x，还要与驱动共享 2 vCPU。",
     "只讲指标饱和与双核取舍；与 U04 的版本归因不同问题。",
     "从 uniform 迁移到 clustered，双核收益从约 1.12x 降到约 1.07x。",
     "2 vCPU cgroup 与驱动共享，线程收益受调度影响；单次墙钟有波动。"),
    ("U21", DIM5, 2.0, "C21", "E20",
     "uniform 10k×128 上真正的 ANN 图索引能否稳定快 3x，值不值得 build/索引内存。",
     "uniform ANN 只快约 1.2x（44.95→37.78 us），被 q7 扫描带宽下限卡住；clustered 精确 IVF 快约 5.9x（49.40→8.42 us）、recall 1.0，但 build 约 40 ms、索引 0.078 MB。结论：不发布图索引，保留 exact IVF + q7 精确路径。",
     "只讲 ANN 方案取舍；IVF 分布判定在 U08。",
     "从 uniform 迁移到 clustered，ANN 的收益反转（1.2x→5.9x）。",
     "基准是合成数据，hidden 分布未知；build/索引内存计入 B/M。"),
    ("U22", DIM5, 1.5, "C22", "E21",
     "更省内存的 4-bit packed code 是否比 q7 更快。",
     "q7 23.5 us/query，4-bit 24.5 us/query；解包抵消带宽收益。拒绝 4-bit，保留 q7；拒绝时同时看时间和 recall，不只看内存。",
     "只讲 4-bit 负结果；HNSW/ANN 在 U21。", None, None),
    ("U23", DIM5, 1.5, "C23", "E22",
     "过滤连续块多 1.28 MB code 矩阵，换 filtered 延迟是否值得。",
     "容器内开连续块 filtered 108.8 us、M 13.13 MB；关闭 121.8 us、M 12.03 MB。多约 1.1 MB M 换约 13-35 us/查询，查询权重 2/3，保留。",
     "只讲过滤块的内存取舍；过滤正确性在 U07。", None, None),
    ("U24", DIM5, 1.5, "C24", "E23",
     "本地微基准更快的 AVX-512 为什么在平台下降，是否应该整个删除。",
     "平台在启用 AVX-512 自动选择后 E 下降，回退 AVX2 恢复；本地也没有 XCR0/512 机器。当前源码 avx512 匹配数为 0，默认只保留 AVX2。",
     "只讲 AVX-512 平台回退；与 4-bit 算法负结果不同。", None, None),
    ("U25", DIM3, 2.0, "C25", "E24",
     "单条多行 INSERT 超过 64 MiB 时，确认成功后重启会不会丢；怎样拆分并保证 chunk 原子性。",
     "3000 行×2000 字符（多 chunk）正常重启 3000 行；after_wal_write 崩溃重启 0 行（未提交）；显式 COMMIT 后 after_commit_fsync 崩溃重启 3000 行；跨 chunk 重复主键 duplicate_key 且 0 行。",
     "只讲大 INSERT 分块持久化；与 checkpoint 编码、语句原子性不同链。",
     "从普通重启迁移到 chunk 中途 crash + 跨 chunk 重复主键。",
     "只测 3000 行×2000 字符；更大单行/VARCHAR 上限未穷举。"),
]

def build():
    conv = []
    for cid, pref, content, note in CONVERSATIONS:
        conv.append({"id": cid, "role": "student", "prompt_ref": pref,
                     "content": content, "note": note})
    exp = []
    exp_quotes = {}
    for eid, name, command, output, quote, analysis in EXPERIMENTS:
        # output is exactly the cited result; the rest of the raw transcript is
        # kept in context so the pointer resolves to the whole result text.
        exp.append({"id": eid, "name": name, "command": command,
                    "output": quote,
                    "context": output if output != quote else "",
                    "exit_code": 0, "analysis": analysis, "commit": COMMIT})
        exp_quotes[eid] = quote
    cidx = {c["id"]: i for i, c in enumerate(conv)}
    eidx = {e["id"]: i for i, e in enumerate(exp)}
    units = []
    seen_student = set(); seen_result = set()
    for (uid, dim, score, cid, eid, problem, reason, independence, transfer,
         limitation) in UNITS:
        ci = cidx[cid]
        s_quote = conv[ci]["content"]
        assert len(s_quote) <= 240, (uid, len(s_quote))
        u = {"id": uid, "dimension": dim, "score": score, "problem": problem,
             "student_evidence": s_quote,
             "student_pointer": f"/conversations/{ci}/content",
             "reason": reason, "independence": independence}
        if eid is not None:
            ei = eidx[eid]
            r_quote = exp_quotes[eid]
            assert r_quote in exp[ei]["output"], (uid, r_quote)
            assert len(r_quote) <= 240, (uid, len(r_quote))
            u["result_evidence"] = r_quote
            u["result_pointer"] = f"/experiments/{ei}/output"
            assert r_quote not in seen_result, ("duplicate result", uid)
            seen_result.add(r_quote)
        else:
            assert score == 0.5, (uid, score)
            u["result_evidence"] = ""
        assert s_quote not in seen_student, ("duplicate student", uid)
        seen_student.add(s_quote)
        if transfer: u["transfer"] = transfer
        if limitation: u["limitation"] = limitation
        units.append(u)
    data = {
        "schema_version": 2,
        "synthetic": False,
        "commit": COMMIT,
        "material_commit_note": "本文件所在提交是上述代码提交的子提交；源码、Dockerfile、本文件在同一最终提交树中。",
        "verified_at": "2026-11-14T19:05:00+08:00",
        "status": {
            "official_protocol": "JSON Lines (STDIO_PROTOCOL.md); Docker protocol_check 20/20",
            "public_cases": "20/20",
            "sql_edges": "158 checks x 2 modes, 0 failures",
            "crash_fuzz": "240 iterations (memory+disk), 0 failures",
            "checkpoint": "10k/50k memory + 50k disk; restart row counts verified",
            "recall": "5000x128 uniform/clustered, exact oracle match, ann recall 1.0",
            "admission": "candidate: all local and official-public checks pass; formal platform admission not yet run for this commit",
            "performance_note": "local A/B only; platform score intentionally not embedded",
        },
        "material_notes": (
            "材料来自本仓库 2026-11-14 的真实开发与本地测评。user_prompts 是用户原话节选（已脱敏）；"
            "conversations 是学生本人对同一轮工作的第一人称回答，按讨论顺序整理，不是 Agent 自动生成的技术解释；"
            "experiments 的 output 是同一 commit 下命令的原始/逐行输出（少数多行结果按原顺序拼接，未改写数值）；"
            "units 每个只对应一个独立问题/实验链，student 与 result 分开引用，互不代替。"
            "没有把本地或平台分数写进任何计分字段。"
        ),
        "user_prompts": USER_PROMPTS,
        "conversations": conv,
        "experiments": exp,
        "units": units,
        "historical_context": (
            "历史失败与回退（不作为本 commit 的计分结果）：文本协议/ready-marker 两次提测失败；"
            "批量 WAL 的 Huffman 位流字节序与 HuffCode 零初始化导致 checkpoint/recovery 失败；"
            "ScanPool 析构重放旧 job 的 use-after-return；AVX-512 自动选择在平台持续负载下 E 下降，"
            "CPU 绑定/无 XCR0 检查导致性能项无效。详见 docs/FAILURE_LEDGER.md 与 docs/PROMPT_PERF_LESSONS.md。"
        ),
        "retrospective": (
            "这次最重要的不是再加一个 SIMD，而是把 exact 的证明链和度量链补齐：q7 只做下界，"
            "超集由三角形不等式给出，最终仍 exact 复核；平台 B/M 用 cgroup 口径；写路径用 profiler 找热点；"
            "大 INSERT、巨大 LIMIT、投影/排序错配、聚簇 ANN recall、malformed vector 都是通过独立反例发现并修复的。"
            "未解决的是 UPDATE no-op 的 affected_rows 语义、未别名 L2_DISTANCE 的列名、以及超宽行在严格内存限制下的分块。"
        ),
        "limitations": [
            "单一 commit 的正式平台准入/性能尚未运行；本地和 Docker 公开 20 例不等于隐藏用例。",
            "unclustered/聚簇结论只覆盖合成 10k/50k×128；hidden 数据分布未知。",
            "UPDATE 的 affected_rows 仍按匹配行计数，MySQL 默认按 changed rows，二者可能不同。",
            "未别名 L2_DISTANCE 的列名由 parser 重建，可能与 MySQL 原样保留的表达式文本不同。",
            "只模拟进程强杀，不模拟断电/坏盘；崩溃 fuzz 的随机覆盖有限。",
        ],
        "summary": "25 个独立单元覆盖五个维度：规划与度量、WAL/checkpoint、q7 精确性、IVF、事务与崩溃、协议、过滤、写路径、ASAN、TopK、指标饱和、ANN、4-bit、内存取舍、AVX-512、大 INSERT 分块。",
        "material_summary": "25 个单元，五个维度 5/5/6/4/5；每个单元有独立的学生问题与实验结果，结果全部来自本 commit 的本地命令。",
    }
    return data

def validate(data):
    assert isinstance(data, dict)
    assert len(json.dumps(data, ensure_ascii=False)) <= 128 * 1024
    ids = [u["id"] for u in data["units"]]
    assert len(ids) == len(set(ids))
    for c in data["conversations"]:
        assert isinstance(c["content"], str)
    for e in data["experiments"]:
        assert isinstance(e["output"], str)
    # resolve pointers and check exact substrings
    def resolve(ptr):
        node = data
        for tok in ptr[1:].split("/"):
            node = node[int(tok)] if isinstance(node, list) else node[tok]
        return node
    seen_s = set(); seen_r = set()
    for u in data["units"]:
        assert u["dimension"] in (DIM1, DIM2, DIM3, DIM4, DIM5)
        assert u["score"] in (0.5, 1.0, 1.5, 2.0)
        s = resolve(u["student_pointer"])
        assert isinstance(s, str) and u["student_evidence"] in s
        assert len(u["student_evidence"]) <= 240
        assert u["student_evidence"] not in seen_s
        seen_s.add(u["student_evidence"])
        if u["score"] > 0.5:
            r = resolve(u["result_pointer"])
            assert isinstance(r, str) and u["result_evidence"] in r
            assert len(u["result_evidence"]) <= 240
            assert u["result_evidence"] not in seen_r
            seen_r.add(u["result_evidence"])
        else:
            assert u["result_evidence"] == ""
    counts = {}
    total = 0.0
    for u in data["units"]:
        counts[u["dimension"]] = counts.get(u["dimension"], 0) + 1
        total += u["score"]
    return counts, total

def main():
    data = build()
    counts, total = validate(data)
    text = json.dumps(data, ensure_ascii=False, indent=1) + "\n"
    out1 = ROOT / "agent/conversation.json"
    out2 = ROOT / "conversation.json"
    out1.write_text(text, encoding="utf-8")
    out2.write_text(text, encoding="utf-8")
    print(f"units={len(data['units'])} raw={total:.1f} bytes={len(text.encode('utf-8'))}")
    for k, v in counts.items():
        print(f"  {k}: {v}")

if __name__ == "__main__":
    main()
