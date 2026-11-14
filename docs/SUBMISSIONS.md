# Submission ledger

One row per formal submission. A submission is a candidate only after the
platform confirms all MySQL cases pass.

## Current candidate (not formally submitted)

| field | value |
|---|---|
| branch / commit | `2026-competition-round2` / `1a50d05baf22bd96e72be36024cefc24b989c3b1` |
| code commit | `9c129bc4915251e690b2b037276bcfc637dbea3c` (material commit is its child) |
| conversation | 55,055 bytes, sha256 `dfbdecf93fb115bbaf3e8c49aa0b8ea99aff3c043b81b022104d084f5f43012b`, 26 units / raw 44.0 |
| local gate | `release_gate.py --require-conversation` PASS (anonymous remote + Docker smoke) |
| official public | protocol_check 20/20 (Docker tip image), public 20/20, marker 20/20 |
| SQL edges | 158 checks x 2 modes, 0 failures |
| recall | 5000x128 uniform/clustered exact oracle match; ann recall 1.0 |
| crash/checkpoint | 240 crash iterations 0 failures; checkpoint 10k/50k/disk restart counts PASS |
| local perf A/B | S3→current uniform write 0.5268→0.2442 s, query 0.1761→0.0490 s, RSS 14.4→12.4 MiB; clustered ann server exec 40.5→19.2 µs, Docker q1000 159.3→116.8 ms (old HEAD clustered ann 203.3 ms), recall 1.0; choose ann for the benchmark config if the form allows |
| formal admission/perf | pending platform run for this commit |


| slot | date/time | commit SHA | image digest | conversation sha256 / bytes | gate | precheck | formal result | admission | local E_write | local E_query | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 | 2026-11-09 | eade40b445b9fa991be27375d4c6b58463b94803 |  | 0b9e9e71 / 8677 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | semicolon + no DML status; see agent/evidence/platform/2026-11-09.md |
| S2 | 2026-11-09 | 2be8b38a57515021111b9a89d7c151a5f79466d4 |  | c7ac94cb / 10285 | PASS | not run | MySQL formal failed | fail | 0.49 s | 0.42 s | marker framing/error names missing; marker fix not submitted |

| S3 | 2026-11-10 | 04960665bfde08bfc4e0b2a5720af5896cd7f6f6 |  | b3857e97 / 16055 | PASS | not run | admission PASS; E=2432.687188; prompt=4.00000 | pass | 0.15 s | 0.27 s | JSONL protocol; top performance 4237.889320, top prompt 8.0 |
| S4 | 2026-11-11 | 46150c520d2a9a9c24e83b75fac7b11a4f0289a5 |  | d695151e / 42378 | PASS | not run | local: write 0.286 s, query 0.099 s vs S3 baseline 0.583/0.201; device B ~6.8 MB; prompt raw 41.5 (24 units) | not run | 0.286 s | 0.099 s | v4.0 q7 exact + group commit |
| S5 |  |  |  |  |  |  |  |  |  |  |  |
| S6 |  |  |  |  |  |  |  |  |  |  |  |
| S7 |  |  |  |  |  |  |  |  |  |  |  |
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
