# Submission ledger

One row per formal submission. A submission is a candidate only after the
platform confirms all MySQL cases pass.

| slot | date/time | commit SHA | image digest | conversation sha256 / bytes | gate | precheck | formal result | admission | local E_write | local E_query | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 |  |  |  |  |  |  |  |  |  |  |  |
| S2 |  |  |  |  |  |  |  |  |  |  |  |
| S3 |  |  |  |  |  |  |  |  |  |  |  |
| S4 |  |  |  |  |  |  |  |  |  |  |  |
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
