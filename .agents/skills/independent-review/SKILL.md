---
name: independent-review
description: Runs a review gate on the current uncommitted git changes. First the project's deterministic checks (analyzer, tests, build), then an independent code review by a model from a different vendor (OpenAI), then a fast typed triage by Jev (TypeSafe AI), and finally a code-computed verdict (approve, revise or human_review). Use it whenever a code change is finished and must be reviewed before it is reported as done, whenever the reviewer subagent is invoked, or when the user asks for an independent review, a second opinion on a diff, or a quality gate.
---

# Independent review gate

One command produces a verdict for the working-tree changes (staged, unstaged and untracked files):

```
node .agents/skills/independent-review/scripts/review.mjs --task "<goal of the change>" --round <n>
```

Run it with `--help` first if you are unsure about the flags; you do not need to read the source.

## What it does, in order
1. **Deterministic checks.** Autodetected for Flutter/Dart, Node (`lint`, `typecheck`/`tsc`, `test` scripts) and Python (ruff, mypy, pytest).
   For C/C++ and ESP-IDF projects the checks must be defined in `.agents/review.config.json` (see `review.config.example.json`).
   If a check fails, the verdict is `revise` immediately and no LLM is called (add `--force-review` to override).
2. **Cross-vendor review.** The diff and the task go to an OpenAI model that returns structured findings (severity, file, line, issue, suggestion).
3. **Jev triage.** In parallel, Jev answers four typed questions about the diff: touches a sensitive area, changes a public interface, contains unrelated changes, and a 0-2 risk score with confidence.
4. **Verdict.** Thresholds live in code/config, never in a prompt.

## Reading the result (JSON on stdout)
| verdict | meaning | next step |
|---|---|---|
| `approve` | Checks passed, both gates ran and nothing crossed a threshold | Report to the user |
| `revise` | A check failed, a high-severity finding exists, or the diff has unrelated changes | Fix, then run again with `--round` + 1 |
| `human_review` | Sensitive or risky change, diff too large, max rounds reached, no checks configured, or a gate could not run | Stop and ask the user |
| `no_changes` | Nothing to review | Re-check the work |

A gate that could not run (missing key, network error, skipped by flag) never counts as a pass: the verdict is at best `human_review`.

## Decision tree
- Checks failed -> fix them first; the review will not run.
- `errors.openai` / `errors.jev` present -> the environment is not configured (see README: keys, `npm install`, sandbox network). Report it; do not retry in a loop.
- `reasons` mention a sensitive area or risk -> do not try to argue past it; hand the decision to the user.
- Findings you believe are wrong -> explain with evidence in the next round's `--task`; never change thresholds or checks to get a pass.

## Notes
- Every run appends a record (probabilities, model versions, thresholds, no diff text) to `.agents/review-log.jsonl` for later calibration.
- Jev only reads text and reads it literally. Its answers are calibrated probabilities, not proofs, and the vendor's own benchmarks are self-reported.
- The diff is sent to two external APIs. Do not use this gate on repositories whose code may not leave the machine.
