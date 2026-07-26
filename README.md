# sqrail

**SQL in, files out.**

sqrail is a small, non-interactive CLI for coding agents that already know how
to write SQL. It executes DuckDB SQL over local CSV, TSV, JSONL, and Parquet
files through a deliberately narrow command surface.

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

## Contract

```text
sqrail schema FILE...
sqrail run [-t NAME=FILE]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR] [SQL|-]
```

- `schema` returns one JSON object per input file.
- `-t NAME=FILE` binds a read-only file to a SQL table name.
- `-` reads SQL from stdin and avoids shell-quoting large queries.
- Without `-o`, rows are streamed as JSONL to stdout.
- With `-o`, the format follows the output extension.
- Existing outputs are never overwritten, and failed queries do not leave a
  partial destination file.
- Diagnostics are a single JSON object on stderr.
- Row order is undefined unless the SQL contains `ORDER BY`.
- No configuration file is read.
- No model or network service is embedded.

An agent can learn the complete interface with:

```sh
sqrail --agent-help
```

## Supported files

| Input | Output |
|---|---|
| CSV, compressed CSV | CSV |
| TSV, compressed TSV | TSV |
| JSON, JSONL, NDJSON | JSON, JSONL, NDJSON |
| Parquet | Parquet |

Compression suffixes recognized for CSV and TSV inputs are `.gz`, `.zst`,
`.bz2`, and `.xz`.

## Build

Requirements:

- CMake 3.22+
- a C++17 compiler
- Git or network access during the first configure

DuckDB is pinned and fetched at configure time. Only one query engine is linked
into the resulting executable.

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

## Status

sqrail is an early prototype. The current release targets local, single-node,
file-to-file analytical work. See [Concept](docs/CONCEPT.md) and
[Benchmark policy](docs/BENCHMARKS.md). The first local build measurements are
recorded in [Initial baseline](docs/BASELINE.md).


## License

MIT. Release binaries statically link DuckDB; see
[Third-party notices](THIRD_PARTY_NOTICES.md).
