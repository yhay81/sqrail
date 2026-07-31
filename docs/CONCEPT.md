# Concept

## Thesis

Coding agents already generate useful SQL. They do not need another
natural-language-to-SQL layer. They need a local command that makes files look
like named SQL tables and behaves predictably enough to learn from a very short
description.

sqrail optimizes the tool boundary, not the language model and not the query
engine.

```text
agent-written SQL
       |
       v
name=file bindings -> DuckDB -> JSONL stdout or one output file
```

## Design constraints

1. **One engine**
   A release contains DuckDB only. Backend selection is not exposed to agents.

2. **SQL remains SQL**
   sqrail does not add a query DSL. Queries use the pinned DuckDB dialect and
   are restricted to one read-only `SELECT`, `VALUES`, or `WITH` statement.

3. **The complete interface must fit in a small prompt**
   `sqrail --help` is normative, complete, and intentionally short.

4. **stdout is data**
   Successful rows go to stdout. Diagnostics go to stderr as one JSON object.

5. **Stream instead of collect**
   stdout rows are fetched incrementally. File outputs use DuckDB's `COPY`
   pipeline. sqrail does not build an in-memory copy of a full result.

6. **Inputs are read-only and outputs are explicit**
   Files bound with `-t` are exposed as temporary views. Existing outputs are
   rejected rather than silently replaced. A completed private file is
   committed with an atomic no-replace hard link only after the query succeeds.

7. **Resource use is part of the public contract**
   Memory, parallelism, spill location, deadline, and final row count can be
   stated at invocation time.

8. **Lightweight is multidimensional**
   We measure compressed size, installed size, cold start, idle RSS, peak RSS,
   temporary disk, and runtime. Binary size alone is insufficient.

9. **No hidden intelligence**
   There is no embedded model, prompt, agent loop, daemon, telemetry, or cloud
   dependency.

## Non-goals

- replacing DuckDB
- accepting natural-language questions
- offering an interactive database shell
- managing persistent databases
- hiding SQL errors from the caller
- acting as a hostile-code or filesystem sandbox
- automatically switching between query engines
- supporting every DuckDB extension

## Performance objective

"Fastest" and "lowest memory" conflict. sqrail therefore uses a constrained
objective:

```text
minimize wall-clock time
subject to peak RSS <= M
and exact output equivalence
```

Initial benchmark budgets are 512 MB, 1 GB, and 4 GB. The important workloads
are scans, filtering, projection, CSV-to-Parquet conversion, low- and
high-cardinality aggregation, small-to-large and large-to-large joins, sorting,
distinct, and window functions.

DuckDB is the product engine because it currently offers the strongest complete
combination of SQL support, file readers, vectorized execution, and out-of-core
operators. Alternative engines may be evaluated in benchmark-only code, but are
never shipped together.

## Why C++

DuckDB is implemented in C++. A small C++ frontend avoids a second language
runtime and binding layer, keeps control over linking, and leaves a direct path
to specialized table functions or operators if profiling identifies a
repeatable bottleneck.

Writing a new SQL engine is not part of the initial plan. Custom C++ execution
work begins only after a reproducible benchmark demonstrates a narrow bottleneck
that cannot be solved through schema, query planning, or DuckDB configuration.

## Success criteria

- an agent succeeds after reading only `--help`
- identical task behavior across non-interactive shells
- no full-result materialization in the frontend
- no accidental output replacement
- reproducible performance reports under fixed memory budgets
- one release executable with no language runtime or non-system shared library
- a compressed release size comparable to the upstream DuckDB CLI
