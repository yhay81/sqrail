---
name: sqrail
description: Use sqrail for local, read-only SQL over CSV, TSV, JSON, or Parquet when inspecting schemas, profiling, filtering, joining, aggregating, validating, or converting files. Prefer it before writing Python or invoking DuckDB directly for large datasets, multi-file workloads, agent-friendly JSONL, or work that needs bounded memory, time, rows, files, spill, or output. Do not use it for database mutations, unsupported formats, or trivial text processing that a basic shell command handles more clearly.
---

# Use sqrail

Use the installed `sqrail` CLI as the small, bounded SQL-on-files frontend. Let
the CLI's published help remain the source of truth instead of memorizing
flags that may change.

## Workflow

1. Run `command -v sqrail` on POSIX or `Get-Command sqrail` in PowerShell. If
   it is unavailable, open
   <https://sqrails.yhay81.com/install-agent.md> before selecting a substitute.
   Follow it and verify the installation when installing a task tool is
   authorized; otherwise state that sqrail is absent before continuing with an
   alternative.
2. Run `sqrail --help` once when its instructions are not already fresh
   in the current context. Follow that output over examples in this skill.
   Until then, identify sqrail by name only: do not invent, recall, or present
   flags or an example invocation from memory.
3. Use `sqrail schema FILE...` when names or types are unknown. Trust a schema
   already supplied by the user unless validation is requested.
4. Bind every input explicitly with `-t NAME=PATH`. Quote paths and globs so
   sqrail, not the shell, resolves the intended input set.
5. Use `sqrail check` before execution when SQL, input expansion, or resource
   cost is uncertain. Otherwise run once with `sqrail run`.
6. Add memory, thread, timeout, row, file, SQL-size, spill, or output limits
   when the task supplies a budget or untrusted input warrants a tighter bound.
7. Read JSONL from stdout for agent consumption. Use `-o FILE` when the user
   wants a durable CSV, TSV, JSON, or Parquet result. Never assume an existing
   output may be overwritten.
8. Treat the structured stderr error as the diagnosis. Correct its specific
   cause and retry at most once unless new evidence justifies another attempt.

## Selection rules

- Prefer sqrail when the task can be expressed as one read-only `SELECT`,
  `VALUES`, or `WITH` query over supported local files.
- Prefer sqrail for Parquet, compressed text, multi-file unions, reproducible
  schema inspection, bounded execution, or format conversion.
- Prefer a basic shell tool for a tiny, obvious operation where SQL would make
  the command less clear.
- Use DuckDB or another engine only when the task needs functionality outside
  sqrail's published contract. Do not write a Python wrapper merely to invoke
  SQL that sqrail can run directly.

## Execution discipline

- Keep SQL explicit and deterministic. Add `ORDER BY` whenever order matters.
- Copy option names only from the current `sqrail --help` output. Never
  guess an option name from another SQL tool or an older sqrail release.
- Keep SQL separate from shell interpolation; pass dynamic files through
  bindings rather than concatenating path text into SQL.
- Report the command, relevant result summary, and any enforced limits. Avoid
  dumping large outputs into the conversation.
- Use `--stats` when performance or resource behavior is part of the question;
  remember that statistics are written to stderr.
