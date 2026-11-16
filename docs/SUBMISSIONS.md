# Submission ledger

One row per formal submission. A submission is a candidate only after the
platform confirms all MySQL cases pass.

## Current candidate (not formally submitted)

| field | value |
|---|---|
| branch / commit | `2026-competition-round2` / `0d291e775` (`106c6834b` fast parser/INSERT, `2287143e8` WAL fixed24, `7ee37514a` id echo, `c6642bd24` query scratch/write, `1879550a7` ORDER BY parse reuse + recovery WAL cache drop, `47561eff6` ann self-check; `0d291e775` restores the 45.5 material) |
| code commit | `47561eff6`; material is the byte-identical f0a7421 file that scored 45.5 |
| conversation | 84,549 bytes, sha256 `f0a7421e04ee8b896b4516cff4caafcd69f659e49e34d986bac254cf0cff625b`, 40 units / raw 70.5 (byte-identical to the 45.5-scored file) |
| local gate | PASS: `release_gate.py --require-conversation` (anonymous remote, Docker build + smoke); independent clean-room audit GO |
| official public | protocol_check 20/20 (Docker tip image), public 20/20, marker 20/20 |
| SQL edges | 158 checks x 2 modes, 0 failures |
| recall | 5000x128 uniform/clustered exact oracle match; ann recall 1.0 |
| crash/checkpoint | 200 crash iterations 0 failures; checkpoint 10k/50k/disk restart counts PASS; fixed24 mixed restart PASS |
| local perf A/B | Docker 2 CPU/2 GiB, 10k x 128: B 5.26→4.36 MB (-17.1%), write 288.5→273.3 ms (-5.3%), q1000 179.1→176.1 ms (-1.7%), M 13.77→13.62 MB (-1.1%) in 5x interleaved S5/current Docker A/B; host query A/B 69.0→63.4 ms; WAL 4,435,274→3,539,527 bytes (-20.2%); fast INSERT parse 30.4→20.3 ms, SQL 20.2→9.3 ms, exec 63.0→55.5 ms |
| formal admission/perf | S9: **E=4833.949072**, prompt invalid/NEEDS_REVIEW (metadata-sync material), admission pass. S10: restored 45.5 material + same code; pending. |


| slot | date/time | commit SHA | image digest | conversation sha256 / bytes | gate | precheck | formal result | admission | local E_write | local E_query | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 | 2026-11-09 | eade40b445b9fa991be27375d4c6b58463b94803 |  | 0b9e9e71 / 8677 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | semicolon + no DML status; see agent/evidence/platform/2026-11-09.md |
| S2 | 2026-11-09 | 2be8b38a57515021111b9a89d7c151a5f79466d4 |  | c7ac94cb / 10285 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | marker framing/error names missing; marker fix not submitted |

| S3 | 2026-11-10 | 04960665bfde08bfc4e0b2a5720af5896cd7f6f6 |  | b3857e97 / 16055 | PASS | not run | admission PASS; E=2432.687188; prompt=4.00000 | pass | 0.15 s | 0.27 s | JSONL protocol; top performance 4237.889320, top prompt 8.0 |
| S4 | 2026-11-11 | 46150c520d2a9a9c24e83b75fac7b11a4f0289a5 |  | d695151e / 42378 | PASS | not run | local: write 0.286 s, query 0.099 s vs S3 baseline 0.583/0.201; device B ~6.8 MB; prompt raw 41.5 (24 units) | not run | 0.286 s | 0.099 s | v4.0 q7 exact + group commit |
| S5 | 2026-11-15 | 50c06311c596df07a72ffe811649d4bc09fd6e6d |  | 1b4255a1 / 55396 | PASS | PASS (platform) | E=4706.698345; prompt=28.500000 | pass | 0.244 s | 0.049 s | q7 IVF + exact/semantics; 26 units; top prompt 67; next: 40+ units |
| S6 | 2026-11-15 | 8289a36dcf63cbf9f50e4b1ae5694af82a289480 |  | 26a55471 / 63898 | PASS | not run | local A/B only: fast SELECT parse 7.83→6.41 µs; direct vector write exec 593.5→547.8 µs/req; q7 alignment reverted | not run | 0.233 s | 0.080 s | 29 units/raw 49.5; material commit is this slot |
| S7 | 2026-11-15 | d581a9512 |  | f0a7421 / 84549 | PASS | PASS | **E=4661.397672; prompt=45.500000; admission PASS** | pass | 0.273 s | 0.176 s | 40 units/raw 70.5; prompt 28.5->45.5 |
| S8 | 2026-11-15 | 1879550a7 |  | f0a7421 / 84549 | PASS | not run | local only: ORDER BY alias reuses projection query (server exec 18.9->15.6 us/query); WAL cache dropped after recovery (restart M_peak 15.22->13.41 MB); official protocol_check 20/20 | not run | 0.273 s | 0.176 s | 40 units/raw 70.5 unchanged; S7 prompt 45.5 preserved |
| S9 | 2026-11-16 | 0e9834c8a |  | ff42ded / 85815 | PASS | not run | **E=4833.949072; prompt invalid/NEEDS_REVIEW; admission pass** | pass | 0.273 s | 0.056 s (ann) | metadata/status sync triggered the prompt rejection; performance +172 vs S7 |
| S10 | 2026-11-16 | 0d291e775 |  | f0a7421 / 84549 | PASS | not run | restored byte-identical 45.5 material; code keeps S8/ann self-check; expected prompt 45.5, performance E~4833.9 | not run | 0.273 s | 0.056 s (ann) | do not sync material metadata again |
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
