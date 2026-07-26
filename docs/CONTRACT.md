# CLI contract

This document defines the sqrail v0.1 command and process contract.

## Commands

```text
sqrail schema FILE...
sqrail run [-t NAME=FILE]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR] [SQL|-]
sqrail --agent-help
sqrail --help
sqrail --version
```

Options may appear before or after the SQL positional argument. Every option
that takes a value may appear at most once, except `-t`, which may be repeated
for distinct case-insensitive table names.

`NAME` must match `[A-Za-z_][A-Za-z0-9_]*`. `FILE` must name an existing regular
file. The first `=` separates a table name from its file path, so paths may
contain additional `=` characters.

## Query contract

- SQL is the dialect of the DuckDB version reported by `sqrail --version`.
- Exactly one statement is accepted.
- Its parsed statement type must be `SELECT`; this includes `VALUES` and
  queries beginning with `WITH`.
- DDL, DML, `COPY`, `ATTACH`, `INSTALL`, `LOAD`, PRAGMA statements, and multiple
  statements are rejected.
- Bound files are temporary views and are never modified.
- Row order is undefined unless the query contains `ORDER BY`.

This read-only statement restriction prevents accidental writes; it is not a
security sandbox. DuckDB file-reading functions remain available inside a
permitted query.

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
newline on stdout. No status text is written to stdout.

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
- `--spill DIR` creates the directory when necessary and selects it as DuckDB's
  temporary directory.

The memory value is a DuckDB memory limit, not a hard operating-system RSS
limit. sqrail sets `preserve_insertion_order=false` and disables automatic
extension installation and loading.

## Schema output

`schema` writes one JSON object per file:

```json
{"file":"/absolute/data.csv","columns":[{"name":"id","type":"BIGINT","nullable":true}]}
```

## Diagnostics

Every handled failure writes exactly one JSON object to stderr:

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
