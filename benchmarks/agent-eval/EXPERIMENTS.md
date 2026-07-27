# Agent-contract experiment log

This log records engineering pilots used to refine `sqrail --agent-help`. It is
not a statistically powered product comparison. All cohorts used the
100,000-row pilot dataset, the sqrail arm, seed `20260727`, and deterministic
machine scoring. The four realistic context profiles were `clean`,
`noisy_workspace`, `superseded_handoff`, and `prior_error`.

## 2026-07-28 decision-first help

The original 254-word help described commands before telling the agent how to
choose among them. In the noisy join condition, both Luna and Gemini 3.6
unnecessarily inspected schemas. The 250-word selected version puts a two-way
decision and stop rule first:

1. names/types are the requested result: run `schema` once;
2. every other task: trust the stated table/column names and run the SQL once;
3. after success: stop without listing, reading, querying, or verifying output.

The exact candidate is
[`help-candidate-decision-first.txt`](help-candidate-decision-first.txt), and is
kept byte-for-byte equal to the compiled `--agent-help` output by tests.

| Cohort | Valid successes | Mean data calls | Median wall s | Notes |
|---|---:|---:|---:|---|
| GPT-5.6 Luna Low, original help | 15/15 | 1.133 | 17.751 | One additional run was invalidated by host disk exhaustion after producing a correct artifact. |
| GPT-5.6 Luna Low, decision-first | 16/16 | 1.000 | 17.088 | Four tasks × four contexts. |
| Gemini 3.6 Flash Low, original help | 16/16 | 1.125 | 9.034 | Four tasks × four contexts. |
| Gemini 3.6 Flash Low, decision-first | 16/16 | 1.000 | 8.607 | Four tasks × four contexts. |
| Gemini 3.5 Flash Low, decision-first | 16/16 | 1.000 | 12.482 | Resolved-model audit passed for every run. |
| GPT-OSS 120B Medium, decision-first | 15/15 | 1.467 | 21.105 | One additional attempt was provider overload before any model response. |
| Qwen 3.5 9B via local Ollama, final stop wording | 1/1 | 4.000 | 116.256 | Noisy join only; correct but much less efficient. |

The selected wording also passed a final noisy-workspace regression on all four
representative tasks: Luna Low 4/4 and Gemini 3.6 Flash Low 4/4, both with
exactly one data call per run. A local 9B run improved from a logically correct
artifact followed by a 180-second agent timeout and nine data calls to a
completed 116.256-second run with four calls. One observation is not evidence
of a stable local-model effect.

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
  It could not be tested because the installed Claude CLI was not authenticated.
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

The strongest result is not that every small model is reliable. It is that a
short decision-first contract kept three inexpensive hosted model cohorts at
16/16 while eliminating redundant data calls for Luna and Gemini 3.6. The 4B
local model described the correct next action but never issued a tool call. The
9B local model could complete tasks, but ignored one-shot guidance and was
roughly an order of magnitude slower than Gemini 3.6 Flash Low on the tested
join. Local open-weight support remains useful for privacy and offline use, not
yet as the default performance path.

Future public claims require at least five repetitions, both blinded data-CLI
arms, all eight tasks, all four contexts, and no model-selection mismatches or
infrastructure failures in the analyzed cohort.

Direct API evaluation of Flash-Lite, Haiku, and nano is the next useful model
expansion. It requires provider credentials and a restricted shell
tool-calling adapter; consumer ChatGPT or Claude subscriptions must not be
assumed to cover separate API usage.
