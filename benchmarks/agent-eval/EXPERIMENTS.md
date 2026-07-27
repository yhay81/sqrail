# Agent-contract experiment log

This log records engineering pilots used to refine `sqrail --agent-help`. It is
not a statistically powered product comparison. The cohorts below evaluated
the v0.2.2 command surface with a 100,000-row pilot dataset, the sqrail arm,
seed `20260727`, and deterministic machine scoring. The four realistic context
profiles were `clean`, `noisy_workspace`, `superseded_handoff`, and
`prior_error`.

## 2026-07-28 decision-first help

The original 254-word help described commands before telling the agent how to
choose among them. In the noisy join condition, both Luna and Gemini 3.6
unnecessarily inspected schemas. The 250-word selected version puts a two-way
decision and stop rule first:

1. names/types are the requested result: run `schema` once;
2. every other task: trust the stated table/column names and run the SQL once;
3. after success: stop without listing, reading, querying, or verifying output.

The exact historical candidate is
[`help-candidate-decision-first.txt`](help-candidate-decision-first.txt).

| Cohort | Valid successes | Mean data calls | Median wall s | Notes |
|---|---:|---:|---:|---|
| GPT-5.6 Luna Low, original help | 15/15 | 1.133 | 17.751 | One additional run was invalidated by host disk exhaustion after producing a correct artifact. |
| GPT-5.6 Luna Low, decision-first | 16/16 | 1.000 | 17.088 | Four tasks × four contexts. |
| Gemini 3.6 Flash Low, original help | 16/16 | 1.125 | 9.034 | Four tasks × four contexts. |
| Gemini 3.6 Flash Low, decision-first | 16/16 | 1.000 | 8.607 | Four tasks × four contexts. |
| Gemini 3.5 Flash Low, decision-first | 16/16 | 1.000 | 12.482 | Resolved-model audit passed for every run. |
| Claude Haiku 4.5 Low, decision-first | 16/16 | 1.813 | 31.955 | Claude Code OAuth on a Max subscription; zero model, safety, or protocol mismatches. |
| GPT-OSS 120B Medium, decision-first | 15/15 | 1.467 | 21.105 | One additional attempt was provider overload before any model response. |
| Qwen 3.5 9B via local Ollama, final stop wording | 1/1 | 4.000 | 116.256 | Noisy join only; correct but much less efficient. |

The selected wording also passed a final noisy-workspace regression on all four
representative tasks: Luna Low 4/4 and Gemini 3.6 Flash Low 4/4, both with
exactly one data call per run. A local 9B run improved from a logically correct
artifact followed by a 180-second agent timeout and nine data calls to a
completed 116.256-second run with four calls. One observation is not evidence
of a stable local-model effect.

Haiku completed every representative task but inspected or verified the join
more often than the other hosted models: the four join contexts averaged 4.25
data calls. A stricter 249-word candidate explicitly prohibited `schema` for
joins and prohibited all commands after success. It passed the same four joins
but still averaged 3.75 calls, so it was rejected rather than replacing the
broader 250-word contract. Its exact text is retained as
[`help-candidate-haiku-strict.txt`](help-candidate-haiku-strict.txt).

## Model and infrastructure exclusions

- `gpt-5.5-nano` was not a documented or callable model. The documented
  low-cost API model is
  [`gpt-5.4-nano`](https://developers.openai.com/api/docs/models/gpt-5.4-nano),
  but the installed Codex CLI reported that it is unsupported with the active
  ChatGPT account. This is an availability result, not a task failure.
- [`gemini-3.5-flash-lite`](https://ai.google.dev/gemini-api/docs/latest-model)
  is available through the Gemini API, but was not listed by the installed Agy
  runtime.
- Current Claude Haiku is
  [`claude-haiku-4-5-20251001`](https://platform.claude.com/docs/en/about-claude/models/overview).
  After Claude Code OAuth login, the `haiku` alias resolved to that exact model
  for all 16 runs. The CLI reported Claude Max subscription authentication,
  no API-key source, and no overage use.
- `gpt-oss:20b` was not installed locally. The Mac had sufficient unified
  memory but initially insufficient disk capacity to fetch the weights. The
  runner now supports it through `--agents local --local-model gpt-oss:20b`.
- `agy --model gemini-3.5-flash-low` unexpectedly resolved to Gemini 3.5 Flash
  Medium. That 16-run cohort was excluded. The human-readable model selector
  `Gemini 3.5 Flash (Low)` resolved correctly. The runner now extracts the Agy
  resolved label and reports model-selection mismatches.
- A provider high-traffic response and an evaluation-log write failure caused
  by disk exhaustion are classified as infrastructure errors. They do not
  reduce model task-completion rates.

## Interpretation

The strongest result is not that every small model is equally efficient. A
short decision-first contract kept four inexpensive hosted model cohorts at
16/16 and eliminated redundant data calls for Luna and Gemini 3.6. Haiku was
equally correct and safe but less obedient to the one-shot rule. The 4B local
model described the correct next action but never issued a tool call. The 9B
local model could complete tasks, but ignored one-shot guidance and was roughly
an order of magnitude slower than Gemini 3.6 Flash Low on the tested join.
Local open-weight support remains useful for privacy and offline use, not yet
as the default performance path.

Future public claims require at least five repetitions, both blinded data-CLI
arms, all eight tasks, all four contexts, and no model-selection mismatches or
infrastructure failures in the analyzed cohort.

Under the no-direct-API constraint, the remaining useful expansion is more
repetitions with the authenticated Codex, Claude Code, and Agy runtimes.
Flash-Lite and nano remain excluded until a non-API authenticated runtime
offers them.

## v0.3 transfer

The v0.3 contract adds schema-evolution, result-limit, statistics, and safety
options, so the v0.2.2 help cannot be copied byte-for-byte. Its validated
decision rule was compressed into the 170-word v0.3 help:

> Names/types result: schema once. Otherwise trust stated names/types, run once,
> and stop after success.

Claims about v0.3 agent behavior require a fresh cohort using the v0.3 binary
and exact help digest. Historical v0.2.2 results remain evidence for the
decision rule, not release evidence for the expanded v0.3 contract.
