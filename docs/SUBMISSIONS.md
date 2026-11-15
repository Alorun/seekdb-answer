# Submission ledger

One row per formal submission. A submission is a candidate only after the
platform confirms all MySQL cases pass.

## Current candidate (not formally submitted)

| field | value |
|---|---|
| branch / commit | `2026-competition-round2` / `c6642bd24` (`106c6834b` fast parser/INSERT, `2287143e8` WAL fixed24, `7ee37514a` id echo, `c6642bd24` query scratch/write micro-opts) |
| code commit | `c6642bd24`; the material commit is its child |
| conversation | 88,003 bytes, sha256 `9853d7c4b294cfc1725e050ba077bb92d495f6e87fa8e616ca1d333ced715569`, 42 units / raw 73.5 |
| local gate | PASS: `release_gate.py --require-conversation` (anonymous remote, Docker build + smoke); independent clean-room audit GO |
| official public | protocol_check 20/20 (Docker tip image), public 20/20, marker 20/20 |
| SQL edges | 158 checks x 2 modes, 0 failures |
| recall | 5000x128 uniform/clustered exact oracle match; ann recall 1.0 |
| crash/checkpoint | 200 crash iterations 0 failures; checkpoint 10k/50k/disk restart counts PASS; fixed24 mixed restart PASS |
| local perf A/B | Docker 2 CPU/2 GiB, 10k x 128: B 5.26→4.36 MB (-17%), write ~290→276 ms, q1000 ~184→177 ms, M 13.4 MB; interleaved query A/B 69.0→63.4 ms; WAL 4,435,274→3,539,527 bytes (-20.2%); fast INSERT parse 30.4→20.3 ms, SQL 20.2→9.3 ms, exec 63.0→55.5 ms |
| formal admission/perf | S7 pending; S5 historical: E=4706.698345; prompt=28.500000 |


| slot | date/time | commit SHA | image digest | conversation sha256 / bytes | gate | precheck | formal result | admission | local E_write | local E_query | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 | 2026-11-09 | eade40b445b9fa991be27375d4c6b58463b94803 |  | 0b9e9e71 / 8677 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | semicolon + no DML status; see agent/evidence/platform/2026-11-09.md |
| S2 | 2026-11-09 | 2be8b38a57515021111b9a89d7c151a5f79466d4 |  | c7ac94cb / 10285 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | marker framing/error names missing; marker fix not submitted |

| S3 | 2026-11-10 | 04960665bfde08bfc4e0b2a5720af5896cd7f6f6 |  | b3857e97 / 16055 | PASS | not run | admission PASS; E=2432.687188; prompt=4.00000 | pass | 0.15 s | 0.27 s | JSONL protocol; top performance 4237.889320, top prompt 8.0 |
| S4 | 2026-11-11 | 46150c520d2a9a9c24e83b75fac7b11a4f0289a5 |  | d695151e / 42378 | PASS | not run | local: write 0.286 s, query 0.099 s vs S3 baseline 0.583/0.201; device B ~6.8 MB; prompt raw 41.5 (24 units) | not run | 0.286 s | 0.099 s | v4.0 q7 exact + group commit |
| S5 | 2026-11-15 | 50c06311c596df07a72ffe811649d4bc09fd6e6d |  | 1b4255a1 / 55396 | PASS | PASS (platform) | E=4706.698345; prompt=28.500000 | pass | 0.244 s | 0.049 s | q7 IVF + exact/semantics; 26 units; top prompt 67; next: 40+ units |
| S6 | 2026-11-15 | 8289a36dcf63cbf9f50e4b1ae5694af82a289480 |  | 26a55471 / 63898 | PASS | not run | local A/B only: fast SELECT parse 7.83→6.41 µs; direct vector write exec 593.5→547.8 µs/req; q7 alignment reverted | not run | 0.233 s | 0.080 s | 29 units/raw 49.5; material commit is this slot |
| S7 | 2026-11-15 | c6642bd24 |  | 9853d7c / 88003 | PASS | not run | local only: WAL -20.2%, Docker B 4.36 MB, write 276 ms, q1000 177 ms; fast INSERT write CPU -12%; all local suites PASS | not run | 0.267 s | 0.165 s | 42 units/raw 73.5; material commit is the child of this row |
| S8 |  |  |  |  |  |  |  |  |  |  |  |
| S9 |  |  |  |  |  |  |  |  |  |  |  |
| S10 |  |  |  |  |  |  |  |  |  |  |  |
| S11 |  |  |  |  |  |  |  |  |  |  |  |
| S12 |  |  |  |  |  |  |  |  |  |  |  |
| S13 |  |  |  |  |  |  |  |  |  |  |  |
| S14 |  |  |  |  |  |  |  |  |  |  |  |
| S15 |  |  |  |  |  |  |  |  |  |  |  |
| S16 |  |  |  |  |  |  |  |  |  |  |  |
| S17 |  |  |  |  |  |  |  |  |  |  |  |
| S18 |  |  |  |  |  |  |  |  |  |  |  |
| S19 |  |  |  |  |  |  |  |  |  |  |  |
| S20 |  |  |  |  |  |  |  |  |  |  |  |

Fields:

- `gate`: result of `python3 scripts/release_gate.py --require-conversation`.
- `precheck`: platform precheck output summary (verbatim log stored under
  `agent/evidence/platform/`).
- `formal result`: failure categories / pass.
- `admission`: `pass` / `fail`.
- `local E_*`: optional local proxy measurements.

Rules: never force-push; keep every submitted SHA reachable; never delete a
passing submission from this ledger.
