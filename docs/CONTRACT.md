# CLI contract

This document defines the sqrail v0.3 command and process contract.

## Commands

```text
sqrail schema [--memory SIZE] [--threads N] [--timeout DURATION]
              [--max-input-files N] [--strict-schema] FILE...
sqrail check [-t NAME=PATH]... [--memory SIZE] [--threads N]
             [--timeout DURATION] [--max-rows N] [--max-input-files N]
             [--max-sql-bytes SIZE] [--strict-schema] [SQL|-]
sqrail run [-t NAME=PATH]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR [--max-spill SIZE]] [--timeout DURATION]
           [--max-rows N] [--max-output-bytes SIZE] [--max-input-files N]
           [--max-sql-bytes SIZE] [--stats] [--strict-schema] [SQL|-]
sqrail --agent-help
sqrail --help
sqrail --version
```

Options may appear before or after the SQL positional argument. Every option
that takes a value may appear at most once, except `-t`, which may be repeated
for distinct case-insensitive table names. `--` ends option parsing, which
allows a SQL argument or schema path beginning with `-`.

`NAME` must match `[A-Za-z_][A-Za-z0-9_]*`. `PATH` may name one regular file, a
glob whose matches have one format, or a directory containing a partitioned
Parquet dataset. Directory traversal is recursive and ignores non-Parquet
files. Matches are canonicalized, sorted, deduplicated, and must be non-empty.
The first `=` separates a table name from its path.

A multi-file binding is read by column name. Columns introduced or removed
between files are included in the combined schema and missing values become
`null`. `--strict-schema` instead requires every matched file to have identical
column names, order, and inferred types; otherwise the command fails with
`SCHEMA_MISMATCH`. This option is available to `run`, `check`, and `schema`.

## Query contract

- SQL is the dialect of the DuckDB version reported by `sqrail --version`.
- Exactly one statement is accepted.
- Its parsed statement type must be `SELECT`; this includes `VALUES` and
  queries beginning with `WITH`.
- DDL, DML, `COPY`, `ATTACH`, `INSTALL`, `LOAD`, PRAGMA statements, and multiple
  statements are rejected.
- Bound files are temporary views and are never modified.
- Row order is undefined unless the query contains `ORDER BY`.

After table binding, sqrail allowlists the exact canonical input files and
disables DuckDB external access. Extension autoloading, automatic installation,
and community extensions are disabled before the configuration is locked.
Unbound file-reading functions therefore cannot open arbitrary local paths.

## Formats

| Extension           | Input |                   Output |
| ------------------- | ----: | -----------------------: |
| `.csv`              |   yes |                      yes |
| `.tsv`, `.tab`      |   yes |                      yes |
| `.json`             |   yes |      yes, one JSON array |
| `.jsonl`, `.ndjson` |   yes | yes, one object per line |
| `.parquet`          |   yes |                      yes |

Text formats may additionally end in `.gz` or `.zst`. External compression of
Parquet and `.bz2`/`.xz` streams are rejected.

## Standard streams

Without `-o`, each result row is encoded as one typed JSON object followed by a
newline on stdout. JSON is RFC 8259-compatible: non-finite floating-point values
are represented as `null`, including inside nested lists and structs. No status
text is written to stdout.

With `-o`, successful execution is silent. The output format follows the file
extension. On POSIX, the completed temporary file is atomically linked to the
requested name, which fails if that name already exists, and the private
temporary name is then removed. A filesystem that cannot create a
same-directory hard link returns `OUTPUT_COMMIT` rather than weakening the
no-overwrite guarantee. On Windows, a no-replace, write-through move provides
the corresponding commit. POSIX outputs have no group or other permissions.
Windows outputs have a protected DACL for the current user and do not inherit
access rules.

With `--stats`, a successful `run` writes exactly one compact JSON object to
stderr after the result is complete:

```json
{
  "schema_version": 1,
  "sqrail_version": "0.3.0",
  "ok": true,
  "command": "run",
  "rows": 3,
  "bytes": 57,
  "elapsed_ms": 18,
  "input_files": 1,
  "destination": "stdout"
}
```

`rows` and `bytes` describe the emitted result, `input_files` counts resolved
files across all bindings, and `destination` is `stdout` or `file`. No success
statistics are emitted after a failure. `--stats` is not accepted by `check`.

Streaming stdout cannot be rolled back. If execution fails after rows have
already been emitted, stdout may contain a valid partial JSONL prefix. Use
`-o` when the consumer requires all-or-nothing output.

## Resource controls

- `--memory SIZE` accepts a positive decimal number followed by `B`, `KB`, `MB`,
  `GB`, `TB`, `KiB`, `MiB`, `GiB`, or `TiB`. Common `K`, `M`, `G`, and `T`
  short forms are also accepted for DuckDB resource limits.
- `--threads N` accepts an integer from 1 through 1024.
- `--spill DIR` creates the root when necessary, then creates a unique,
  owner-only process workspace beneath it as DuckDB's temporary directory.
  Existing sibling files are not allowlisted, and the workspace is removed
  after DuckDB closes.
- `--max-spill SIZE` requires `--spill`, accepts the same resource-size syntax
  as `--memory`, and caps DuckDB temporary storage.
- `--timeout DURATION` accepts a positive duration from `1ms` through 7 days
  using `ms`, `s`, or `m`. Its absolute deadline begins when command handling
  starts and covers input discovery, schema inference, planning, execution,
  and output finalization.
- `--max-rows N` accepts an integer from 1 through 9223372036854775806. sqrail
  requests at most `N + 1` final rows and returns `RESULT_LIMIT` if the extra row
  exists. A file destination is not committed after this failure. Streaming
  stdout may already contain a valid prefix of at most `N` rows.
- `--max-output-bytes SIZE` accepts the explicit `B`, `KB`, `MB`, `GB`, `TB`,
  `KiB`, `MiB`, `GiB`, and `TiB` byte-size units and interrupts file output
  after it grows beyond the cap. The `K`/`M`/`G`/`T` resource shorthands are
  deliberately excluded from exact byte caps. Its decimal part may have at
  most six digits and is converted to an exact integer byte count by
  truncating any sub-byte remainder. JSONL stdout is checked before each
  buffered write. File destinations are never committed on
  `OUTPUT_LIMIT`; stdout may already contain a valid prefix.
- `--max-input-files N` accepts an integer from 1 through 1000000000 and stops
  recursive directory or glob expansion as soon as the cumulative count
  exceeds the cap.
- `--max-sql-bytes SIZE` uses the same exact byte-size syntax as
  `--max-output-bytes` and bounds either the positional SQL string or
  incremental stdin reads before parsing.

`check` accepts `--memory`, `--threads`, `--timeout`, `--max-rows`,
`--max-input-files`, `--max-sql-bytes`, and `--strict-schema`, but not output,
output-byte, spill, or stats options. `schema` accepts `--memory`, `--threads`,
`--timeout`, `--max-input-files`, and `--strict-schema`. The memory value is a
DuckDB memory limit, not a hard operating-system RSS limit. sqrail sets
`preserve_insertion_order=false`. Without `--spill`, external temporary
storage is disabled.

## Check output

`check` binds inputs, validates the read-only statement, and emits result-column
metadata, resolved input counts, and a strict JSON physical plan without
executing the query:

```json
{
  "schema_version": 1,
  "sqrail_version": "0.3.0",
  "ok": true,
  "columns": [{ "name": "total", "type": "HUGEINT", "nullable": true }],
  "inputs": [{ "table": "sales", "files": 1 }],
  "plan": [{ "name": "UNGROUPED_AGGREGATE", "children": [], "extra_info": {} }]
}
```

With `--max-rows`, the reported columns and plan describe the bounded wrapper
that execution would use.

## Schema output

`schema` writes one JSON object per input path:

```json
{
  "schema_version": 1,
  "sqrail_version": "0.3.0",
  "file": "/absolute/data.csv",
  "files": 1,
  "columns": [{ "name": "id", "type": "BIGINT", "nullable": true }]
}
```

## Diagnostics

Every handled failure writes exactly one UTF-8 JSON object to stderr. Invalid
bytes in operating-system arguments are replaced with U+FFFD:

```json
{
  "schema_version": 1,
  "sqrail_version": "0.3.0",
  "ok": false,
  "code": "INPUT_NOT_FOUND",
  "message": "input file not found: missing.csv"
}
```

`SIGINT` and `SIGTERM` interrupt active planning, schema inference, or execution
and return exit 4 with `QUERY_INTERRUPTED`; private output and spill artifacts
are removed. `SIGPIPE` is converted to an exit 5 `STDOUT_WRITE` diagnostic so
the same cleanup runs when a downstream stdout consumer closes early.
Windows console Ctrl+C and Ctrl+Break use the same structured interruption
path.

All machine-readable objects currently use `schema_version: 1` and include the
emitting `sqrail_version`. Additive fields may appear within a schema version;
consumers must ignore unknown fields. A removal, rename, type change, or
semantic incompatibility requires a new `schema_version`.

Exit codes are stable:

| Exit | Class                         |
| ---: | ----------------------------- |
|    0 | success                       |
|    2 | command usage                 |
|    3 | input file or format          |
|    4 | SQL parse, bind, or execution |
|    5 | output path or commit         |
|   70 | unexpected internal failure   |
