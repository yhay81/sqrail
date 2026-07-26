# CLI contract

This document defines the sqrail v0.2 command and process contract.

## Commands

```text
sqrail schema FILE...
sqrail check [-t NAME=PATH]... [--memory SIZE] [--threads N] [--timeout DURATION] [SQL|-]
sqrail run [-t NAME=PATH]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR [--max-spill SIZE]] [--timeout DURATION] [SQL|-]
sqrail --agent-help
sqrail --help
sqrail --version
```

Options may appear before or after the SQL positional argument. Every option
that takes a value may appear at most once, except `-t`, which may be repeated
for distinct case-insensitive table names.

`NAME` must match `[A-Za-z_][A-Za-z0-9_]*`. `PATH` may name one regular file, a
glob whose matches have one format, or a directory containing a partitioned
Parquet dataset. Directory traversal is recursive and ignores non-Parquet
files. Matches are canonicalized, sorted, deduplicated, and must be non-empty.
The first `=` separates a table name from its path.

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

| Extension | Input | Output |
|---|---:|---:|
| `.csv` | yes | yes |
| `.tsv`, `.tab` | yes | yes |
| `.json` | yes | yes, one JSON array |
| `.jsonl`, `.ndjson` | yes | yes, one object per line |
| `.parquet` | yes | yes |

Text formats may additionally end in `.gz` or `.zst`. External compression of
Parquet and `.bz2`/`.xz` streams are rejected.

## Standard streams

Without `-o`, each result row is encoded as one typed JSON object followed by a
newline on stdout. JSON is RFC 8259-compatible: non-finite floating-point values
are represented as `null`, including inside nested lists and structs. No status
text is written to stdout.

With `-o`, successful execution is silent. The output format follows the file
extension. The completed temporary file is atomically linked to the requested
name, which fails if that name already exists. The private temporary name is
then removed. A filesystem that cannot create a same-directory hard link
returns `OUTPUT_COMMIT` rather than weakening the no-overwrite guarantee.

Streaming stdout cannot be rolled back. If execution fails after rows have
already been emitted, stdout may contain a valid partial JSONL prefix. Use
`-o` when the consumer requires all-or-nothing output.

## Resource controls

- `--memory SIZE` accepts a positive decimal number followed by `B`, `KB`, `MB`,
  `GB`, `TB`, `KiB`, `MiB`, `GiB`, or `TiB`.
- `--threads N` accepts an integer from 1 through 1024.
- `--spill DIR` creates the root when necessary, then creates a unique,
  owner-only process workspace beneath it as DuckDB's temporary directory.
  Existing sibling files are not allowlisted, and the workspace is removed
  after DuckDB closes.
- `--max-spill SIZE` requires `--spill` and caps DuckDB temporary storage.
- `--timeout DURATION` accepts a positive duration from `1ms` through 7 days
  using `ms`, `s`, or `m`, and interrupts work at the deadline.

`check` accepts `--memory`, `--threads`, and `--timeout`, but not spill options.
The memory value is a DuckDB memory limit, not a hard operating-system RSS
limit. sqrail sets `preserve_insertion_order=false`. Without `--spill`, external
temporary storage is disabled.

## Check output

`check` binds inputs, validates the read-only statement, and emits a strict JSON
physical plan without executing the query:

```json
{"ok":true,"plan":[{"name":"UNGROUPED_AGGREGATE","children":[],"extra_info":{}}]}
```

## Schema output

`schema` writes one JSON object per input path:

```json
{"file":"/absolute/data.csv","files":1,"columns":[{"name":"id","type":"BIGINT","nullable":true}]}
```

## Diagnostics

Every handled failure writes exactly one UTF-8 JSON object to stderr. Invalid
bytes in operating-system arguments are replaced with U+FFFD:

```json
{"ok":false,"code":"INPUT_NOT_FOUND","message":"input file not found: missing.csv"}
```

Exit codes are stable:

| Exit | Class |
|---:|---|
| 0 | success |
| 2 | command usage |
| 3 | input file or format |
| 4 | SQL parse, bind, or execution |
| 5 | output path or commit |
| 70 | unexpected internal failure |
