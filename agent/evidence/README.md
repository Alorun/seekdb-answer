# Evidence workflow

This directory holds the real material used to build `agent/conversation.json`.
It is not a substitute for the JSON itself; the platform only reads the JSON.

## Per submission (about 10 minutes)

1. The agent proposes 3-5 questions about what actually happened in this work
   block. The student confirms or edits the question list.
2. The student answers briefly (one or two points is enough).
3. The agent turns the answers into a concise first-person draft. The draft
   does not copy the student's wording, but it must not add facts, conclusions or
   results the student did not state.
4. The student confirms or corrects the draft line by line.
5. Only after confirmation, append it to `journal/YYYY-MM-DD.md` with
   `confirmed: true` and a timestamp.
6. `result_evidence` (command output, platform feedback) is stored verbatim and
   never rewritten.

## Rules

- No invented prompts, student understanding, experiment output or platform
  results.
- No raw platform hidden-test content in stderr logs; diagnostics record only
  statement type/length/hash.
- If a block produced no new understanding or result, write "no new evidence"
  instead of padding.
- `conversation.json` must be <= 64 KiB, valid UTF-8 JSON, object at the top
  level, no duplicate keys and no NaN/Infinity.

## Files

- `journal/YYYY-MM-DD.md`: confirmed evidence drafts and raw command output.
- `platform/`: verbatim precheck / formal submission feedback.
- `conversation.json` (in `agent/`): assembled and validated final material.
