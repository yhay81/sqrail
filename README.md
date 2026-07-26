# sqrail

**SQL in, files out.**

sqrail is a small, non-interactive CLI for coding agents that already know how
to write SQL. It executes one read-only DuckDB query over local CSV, TSV, JSON,
and Parquet files through a deliberately narrow command surface.

It is not a natural-language interface, an agent framework, or a new query
engine. The model writes the SQL. sqrail provides predictable file binding,
streamed execution, bounded resources, and machine-readable output.

```sh
sqrail run \
  -t sales=sales.csv \
  -t drugs=drugs.parquet \
  -o result.parquet \
  - <<'SQL'
SELECT d.name, sum(s.amount) AS total
FROM sales s
JOIN drugs d USING (drug_id)
GROUP BY d.name
ORDER BY total DESC
SQL
```

## Install

Prebuilt archives for Linux and macOS on x86-64 and Arm64 are published on the
[Releases](https://github.com/yhay81/sqrail/releases) page. Each release includes
`SHA256SUMS` and GitHub build-provenance attestations.

Extract the archive and place `bin/sqrail` on `PATH`. No language runtime,
package manager, daemon, or database server is required. Linux archives target
glibc 2.35 or newer and statically include the C++ runtime.

## Contract

```text
sqrail schema FILE...
sqrail run [-t NAME=FILE]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR] [SQL|-]
```

- `schema` returns one JSON object per input file.
- `-t NAME=FILE` binds a read-only file to a SQL table name.
- `-` reads SQL from stdin and avoids shell-quoting large queries.
- SQL must parse as exactly one `SELECT`, `VALUES`, or `WITH` query.
- Without `-o`, rows are streamed as JSONL to stdout.
- With `-o`, the format follows the output extension.
- Existing outputs are never overwritten, and failed queries do not leave a
  partial destination file.
- Diagnostics are a single JSON object on stderr.
- Row order is undefined unless the SQL contains `ORDER BY`.
- No configuration file is read.
- No model or network service is embedded.

The normative details, JSON schemas, failure semantics, and exit codes are in
[CLI contract](docs/CONTRACT.md).

An agent can learn the complete interface with:

```sh
sqrail --agent-help
```

## Supported files

| Input | Output |
|---|---|
| CSV, `.csv.gz`, `.csv.zst` | CSV, `.csv.gz`, `.csv.zst` |
| TSV, `.tsv.gz`, `.tsv.zst` | TSV, `.tsv.gz`, `.tsv.zst` |
| JSON, JSONL, NDJSON, optionally `.gz`/`.zst` | JSON, JSONL, NDJSON, optionally `.gz`/`.zst` |
| Parquet | Parquet |

Externally compressed Parquet and bzip2/xz streams are deliberately rejected.

## Build

Requirements:

- CMake 3.22+
- a C++17 compiler
- Git or network access during the first configure

DuckDB v1.5.5 is fetched at an immutable commit during configure. Only one query
engine is linked into the resulting executable.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sqrail --parallel
ctest --test-dir build --output-on-failure
```

Then:

```sh
./build/sqrail schema data.csv
./build/sqrail run -t data=data.csv 'SELECT count(*) AS rows FROM data'
```

## Resource controls

```sh
sqrail run \
  --memory 1GB \
  --threads 2 \
  --spill /fast-nvme/sqrail-spill \
  -t data=large.parquet \
  -o result.parquet \
  - < query.sql
```

`--memory` configures DuckDB's memory limit. It is not an operating-system hard
RSS limit. sqrail disables insertion-order preservation by default so unordered
file transformations can use less memory.

## Safety boundary

sqrail prevents SQL write statements and accidental output replacement. It is
not a sandbox: a permitted DuckDB `SELECT` can still invoke file-reading
functions. Run it with the same filesystem permissions and trust boundary as
the agent that supplies the SQL.

## Performance

The committed harness builds sqrail and the DuckDB CLI from the same source
revision, checks logical output equivalence, and records time and peak RSS. See
[Benchmark policy](docs/BENCHMARKS.md), [harness](benchmarks/README.md), and the
[v0.1.0 baseline](docs/BASELINE.md).

The objective is DuckDB-class execution without a second runtime and with a
smaller, deterministic agent-facing contract—not a claim that the embedded
DuckDB engine is faster than itself.


## License

MIT. Release binaries statically link DuckDB; see
[Third-party notices](THIRD_PARTY_NOTICES.md).
