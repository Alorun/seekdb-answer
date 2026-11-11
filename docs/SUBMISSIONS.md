# Submission ledger

One row per formal submission. A submission is a candidate only after the
platform confirms all MySQL cases pass.

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
