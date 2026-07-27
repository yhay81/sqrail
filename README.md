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

```sh
brew install yhay81/tap/sqrail
```

Prebuilt archives for Linux and macOS on x86-64 and Arm64 are published on the
[Releases](https://github.com/yhay81/sqrail/releases) page. Each release includes
`SHA256SUMS` and GitHub build-provenance attestations.

Extract the archive and place `bin/sqrail` on `PATH`. No language runtime,
package manager, daemon, or database server is required. Linux archives target
glibc 2.35 or newer and statically include the C++ runtime.

## Contract

```text
sqrail schema FILE...
sqrail check [-t NAME=PATH]... [--memory SIZE] [--threads N] [--timeout DURATION] [SQL|-]
sqrail run [-t NAME=PATH]... [-o FILE] [--memory SIZE] [--threads N]
           [--spill DIR [--max-spill SIZE]] [--timeout DURATION] [SQL|-]
```

- `schema` returns one JSON object per input path.
- `check` validates SQL and returns its JSON physical plan without execution.
- `-t NAME=PATH` binds a file, glob, or partitioned Parquet directory.
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
| Parquet file, glob, or partitioned directory | Parquet |

Externally compressed Parquet and bzip2/xz streams are deliberately rejected.

## Build

Requirements:

- CMake 3.25+
- Ninja
- a C++20 compiler
- Python 3 for the smoke-test JSON assertions
- Git and network access for the first `BUNDLED` configure, or an installed
  DuckDB CMake package for a `SYSTEM` build

The default `BUNDLED` provider fetches DuckDB v1.5.5 at an immutable commit
during the first configure and statically links it. This is the provider used
for official sqrail releases:

```sh
cmake --workflow --preset dev
cmake --workflow --preset release
```

Distribution packages can build entirely from the release source archive and
link an installed DuckDB package instead. The package must export DuckDB's CMake
configuration with the `core_functions`, `parquet`, and `json` extensions:

```sh
CMAKE_PREFIX_PATH=/path/to/duckdb cmake --workflow --preset system
cmake --install out/build/system --prefix out/install/system --component sqrail
```

`CMAKE_PREFIX_PATH` may be set to the DuckDB package prefix when it is outside
the platform's default search path. The system provider links DuckDB's shared
library, reports its actual runtime version, and does not install a second copy
of DuckDB's bundled licences.

The repository also supplies `strict`, `sanitize`, `fuzz`, `native`,
`pgo-generate`, and `pgo-use` presets. The pinned Ubuntu 24.04/LLVM 18
development container runs the developer workflow on creation. Then:

```sh
./out/build/release/sqrail schema data.csv
./out/build/release/sqrail run -t data=data.csv 'SELECT count(*) AS rows FROM data'
```

## Resource controls

```sh
sqrail run \
  --memory 1GB \
  --threads 2 \
  --spill /fast-nvme/sqrail-spill \
  --max-spill 100GB \
  --timeout 10m \
  -t data='warehouse/**/*.parquet' \
  -o result.parquet \
  - < query.sql
```

`--memory` configures DuckDB's memory limit. It is not an operating-system hard
RSS limit. sqrail disables insertion-order preservation by default so unordered
file transformations can use less memory. A unique, owner-only workspace is
created beneath `--spill` for each process and removed after the database
closes; sibling files are never exposed to SQL. `--max-spill` caps temporary
storage, and `--timeout` interrupts DuckDB planning or execution after the
deadline.

## Safety boundary

sqrail rejects write statements, disables DuckDB external access and extension
loading, locks the configuration, and allowlists only bound inputs, the private
output temporary path, and the private workspace created beneath an explicit
spill root. This blocks SQL from reading arbitrary local paths, including
existing siblings under the spill root. It is still process-level defense in
depth, not an operating-system sandbox; use an OS sandbox for hostile SQL or
malformed data.

## Performance

The committed harness builds sqrail and the DuckDB CLI from the same source
revision, checks logical output equivalence, and records time and peak RSS. See
[Benchmark policy](docs/BENCHMARKS.md), [harness](benchmarks/README.md), and the
[measured baseline](docs/BASELINE.md).

The objective is DuckDB-class execution without a second runtime and with a
smaller, deterministic agent-facing contract—not a claim that the embedded
DuckDB engine is faster than itself.


## License

MIT. Official release binaries statically link DuckDB; distribution packages
may link a packaged DuckDB shared library. See
[Third-party notices](THIRD_PARTY_NOTICES.md).
