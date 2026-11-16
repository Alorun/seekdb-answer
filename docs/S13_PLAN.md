# S13 plan: E≈5100 with a frozen 45.5 prompt

## Goal and constraints

- One formal submission must reach platform **E ≥ 5100** while the Agent prompt
  material stays the byte-identical `f0a7421` 84,549 B file that scored **45.5**.
  The platform keeps the best combined result, so a single qualifying
  submission is enough; a later prompt experiment cannot take that away as long
  as the qualifying version is the one designated at the deadline.
- Admission is all-or-nothing: every MySQL case must pass.  Any change that
  cannot clear the full local gate is not integrated, no matter how much E it
  promises.
- Platform E is noisy (~20% between runs has been observed).  A real +5% is
  therefore the minimum, and +10–15% is the target so that a bad roll still
  lands near 5100.

## What E is made of (working model)

`E = 10000 * (E_write + 2*E_query)/3`, `E_* = 0.6*f(R_T) + 0.2*f(R_B) + 0.2*f(R_M)`,
`f(R)=R/(1+R)`, R = baseline/candidate.  Equal-to-baseline on every metric
gives 5000, so our S9 result 4833.949072 means the candidate is slightly
*below* baseline parity overall despite a fast scan.  The three places where a
candidate can win are therefore:

| lever | weight in each project | our current value | feasible target | E sensitivity |
|---|---:|---|---|---|
| M peak memory | 0.2 | 13.0–13.4 MB | 10.4–10.8 MB | ~+110 to +140 |
| T_query | 0.6 | ~45 us server exec + driver | -25–35% server exec | depends on driver share |
| T_write | 0.6 | ~90 ms server + ~190 ms driver | -10–15% server exec | ~+40 to +80 |
| B disk I/O | 0.2 | 4.36 MB | 3.95 MB floor | ~+20 (little headroom) |

B is essentially at the lossless floor: 10k x 128 six-decimal components need
~21 bits each (~3.36 MB) and the WAL already writes 3.95 MB including all row
data, so no meaningful B win exists.  M has ~2.5–3 MB of removable overhead
(duplicate q7 code matrix, per-row heap/map overhead).  T is partly driver
bound, which is why server-side exec reduction is measured separately.

## Measured baseline (this machine, 2 vCPU / 2 GiB docker, swap off)

`python3 tests/run_e_proxy.py --bin ./build/minivectordb --repeat 5` gives the
official-shape workload and reads cgroup `memory.peak` / `io.stat`:

- T_write 284–303 ms, T_query 188–191 ms, B 4.362 MB, M 13.13–13.56 MB.
- B split: 3.953 MB written (WAL) + 0.410 MB read (binary/libs).  Queries add 0.
- M split (cgroup `memory.stat`): anon 11.6 MB, file 0, slab 0.3 MB,
  transient ~0.3 MB.  Anon = vectors 5.12 + q7 codes 1.28 + FilterCodes 1.28 +
  rows/meta ~2.4 + process ~1.4.
- Server-side per-query exec (host profile, one-request-at-a-time driver):
  floor `SELECT id ORDER BY id LIMIT 10` ~1 us, filtered TopK ~25.5 us,
  unfiltered TopK ~50.2 us, format ~3.7 us, SQL parse ~4 us.  Insert-only
  server exec for the 100 batches is ~61 ms (JSON 11 ms + SQL 7 ms + exec 61 ms
  in the full run).
- Two-thread scan: `parallel_scan_` only turns on when another request is
  already buffered; the official driver is one-request/one-response, so the
  scan is single threaded on the platform.  Forcing `MVDB_TWO_THREADS=1`
  reduces reported exec (50.2 -> 22.4 us unfiltered, 25.5 -> 5.2 us filtered)
  but the client wall did not drop in the docker proxy; this needs a clean A/B
  before it can become a default.

## Work streams

1. **Memory (M)**: remove the FilterCodes duplicate code matrix (done in a
   worktree: M 13.24 -> 12.04 MB, T_query not worse, responses byte-identical);
   then compact row storage (map/vector overhead, pk->offset map, payload
   arena).  Hard rule: exact same rows/order/distances, same persistence and
   recovery behaviour.
2. **Write (T_write)**: zero-copy JSON request handling and direct vector
   literal parse into the final Value; WAL encoding must not grow B.
3. **Query (T_query)**: explain the filtered 25 us for 200 rows and the
   unfiltered 50 us split; only keep changes with a reproducible median win.
4. **Prompt (last)**: the frozen file stays the primary material.  A candidate
   that fixes the U21/U26 duplicate (same problem, only max counts) and
   documents the new memory finding is prepared separately, but it is only
   submitted *after* a qualifying E ≥ 5100 + prompt 45.5 run exists.

## Integration and gate

Every candidate is integrated by cherry-pick into the main branch and must pass
`bash scripts/full_gate.sh` (release build, public 20, SQL edges 158x2, recall
uniform+clustered, filter differential 190, marker 20, semantics, checkpoint
10k/50k, crash fuzz 150, clustered ann 3-phase, crc/vector self-checks, and the
official `protocol_check` 20 cases through the official JSONL driver), plus a
5-round 2 vCPU docker A/B against the S12 baseline.

## Submission order

1. Slot 1: new code + frozen 84,549 B material.
2. If E ≥ 5100: designated.  Slot 2 (optional): same code + prompt candidate.
3. If E < 5100: repeat slot 2 with the same combination (noise roll), keep the
   best measured run as the designated version.
