# Agent integration

> Give an agent SQL, explicit files, and a bounded execution envelope.

[Documentation index](README.md) · [CLI contract](CONTRACT.md) ·
[Security](../SECURITY.md) ·
[Agent experiments](../benchmarks/agent-eval/EXPERIMENTS.md)

## Minimal context

Prefer the executable's stable help over a prose tool tutorial:

```sh
sqrail --help
```

The same release interface is available to HTTP-based agents at
<https://sqrails.yhay81.com/help.txt>. The website discovery file is
<https://sqrails.yhay81.com/llms.txt>.

## Install discovery

The canonical public Agent Skill is
[`skills/sqrail`](../skills/sqrail/SKILL.md). GitHub CLI 2.90 or newer can
preview it before placing it in the current host's native user directory:

```sh
gh skill preview yhay81/sqrail sqrail
gh skill install yhay81/sqrail sqrail --agent codex --scope user
```

Replace `codex` with the active host. Verified targets include Claude Code,
Cursor, GitHub Copilot, OpenCode, Cline, Kiro CLI, and Windsurf. GitHub CLI
records source provenance so `gh skill update` can check later releases.

AGY and Antigravity CLI 1.1.8 do not discover the shared `.agents/skills`
project location used by current generic installers. Install the repository as
a native plugin instead:

```sh
agy plugin install https://github.com/yhay81/sqrail
```

Agents that start without sqrail can be given the stable
<https://sqrails.yhay81.com/install-agent.md> bootstrap. It deliberately
contains reviewable Markdown rather than executable shell code and installs the
CLI before the Skill. `npx skills` is a fallback only when Node.js is already
present.

## Decision rule

1. If the user has not supplied names and types, run `sqrail schema FILE...`
   once.
2. Otherwise trust the stated names and types.
3. Bind every input with `-t NAME=PATH`.
4. Use `check` only when a dry plan is materially useful.
5. Run the SQL once and stop after exit code `0`.
6. On failure, parse the single stderr JSON object before deciding whether a
   corrected invocation is justified.

Do not rediscover a stated schema, retry a successful query, or treat unordered
results as ordered.

## Invocation pattern

Pass long or generated SQL on stdin so the shell does not have to interpret it:

```sh
sqrail run \
  -t events=events.parquet \
  --memory 512MB \
  --threads 2 \
  --timeout 30s \
  --max-rows 100000 \
  --max-output-bytes 64MB \
  --max-input-files 32 \
  --stats \
  - < query.sql
```

Without `-o`, successful rows are JSONL on stdout. Diagnostics and
`--stats` output are JSON on stderr. Keep the streams separate.

## Choose limits from the task

| Dimension     | Option                   | Typical reason                                         |
| ------------- | ------------------------ | ------------------------------------------------------ |
| DuckDB memory | `--memory`               | Bound the engine's buffer and operator memory          |
| parallelism   | `--threads`              | Avoid saturating a shared agent host                   |
| wall time     | `--timeout`              | Bound discovery, planning, execution, and finalization |
| spill         | `--spill`, `--max-spill` | Permit larger-than-memory work within a disk budget    |
| result rows   | `--max-rows`             | Reject accidentally broad results                      |
| output bytes  | `--max-output-bytes`     | Protect storage and downstream context                 |
| input count   | `--max-input-files`      | Bound glob and directory expansion                     |
| SQL size      | `--max-sql-bytes`        | Bound generated input size                             |

`--memory` is a DuckDB limit, not a process RSS sandbox. Use operating-system
isolation when SQL or input files are hostile.

## Error handling

sqrail writes exactly one diagnostic JSON object to stderr and selects an exit
code by failure class:

| Exit | Class    | Agent response                                     |
| ---: | -------- | -------------------------------------------------- |
|    0 | success  | consume output and stop                            |
|    2 | usage    | correct invocation or declared limit syntax        |
|    3 | input    | correct an explicit binding or input path          |
|    4 | SQL      | correct the query without broadening file access   |
|    5 | output   | choose a valid, absent destination or output limit |
|   70 | internal | preserve the diagnostic and report a tool failure  |

The full schemas and failure semantics are normative in
the [CLI contract](CONTRACT.md).

## Evaluation

The agent-facing surface is tested with blinded, low-cost-model tasks rather
than optimized only for a single frontier model. Read the
[experiment log](../benchmarks/agent-eval/EXPERIMENTS.md) before changing help
wording or decision rules; small wording changes can alter redundant discovery,
retry, and stop behavior.
