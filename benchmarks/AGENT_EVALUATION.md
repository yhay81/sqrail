# Agent task-completion evaluation

This protocol measures sqrail's product claim: a coding agent that already
writes SQL can complete file tasks from a short, deterministic CLI contract.
It does not measure natural-language-to-SQL quality or query-engine speed.

## Arms

Run every task in fresh sessions with the same model, model settings, files,
working directory, and task prompt.

| Arm | Initial tool information |
|---|---|
| sqrail | the exact output of `sqrail --agent-help` |
| DuckDB CLI | the exact output of `duckdb --help` |

The agent may inspect file names but receives no repository documentation,
examples, previous transcripts, or corrections. It may invoke only the selected
CLI plus ordinary shell readers needed to inspect its own stdout and stderr.
SQL knowledge is held constant because the same model and prompts are used.

## Corpus

[`agent-tasks.json`](agent-tasks.json) defines eight tasks over the deterministic
dataset produced by `generate.sh`. The tasks cover schema discovery, streaming
JSONL, joins, two format conversions, bounded out-of-core work, timeout
recovery, and no-overwrite behavior.

Each model/arm/task combination is repeated at least five times with independent
sessions. Randomized file and output names prevent memorizing a literal command.
Do not mix results from model or model-version changes.

## Recording

Record one JSON object per attempt:

```json
{
  "run_id": "opaque-id",
  "model": "provider/model-version",
  "arm": "sqrail",
  "task": "join_aggregate",
  "attempt": 1,
  "success": true,
  "exit_code": 0,
  "wall_seconds": 1.23,
  "input_tokens": 850,
  "output_tokens": 120,
  "safety_violation": false
}
```

An attempt succeeds only when the command exits as required and its output row
count and logical checksum match the task oracle. A safety violation includes
overwriting an existing file, reading an unbound file, leaving a partial output,
or exceeding a declared timeout or spill cap.

## Report

Report by model, arm, and task:

- first-attempt and eventual task success
- attempts per successful task
- input and output tokens
- wall time
- safety violations

Publish raw JSONL, the exact help text, model identifiers, runner versions, and
task-oracle checksums. Do not claim an advantage from a single run or from
different models in the two arms. Treat lower token use or fewer retries as a
product advantage only when success and safety are non-inferior.

## Initial acceptance gate

Before expanding the CLI, the sqrail arm should achieve:

- at least 90% first-attempt success across the corpus
- at least 98% eventual success within two attempts
- zero safety violations
- no task with lower eventual success than the DuckDB CLI arm

These are engineering gates, not statistically sufficient public claims.
