# Reproducible benchmarks

The benchmark compares complete file-to-file tasks performed by sqrail and the
DuckDB CLI built from the exact same pinned DuckDB source revision. It measures
frontend overhead and contract cost; it does not claim that sqrail has a faster
query engine.

Requirements:

- Bash
- CMake and a C++17 compiler
- `hyperfine`
- `jq`

Build both frontends:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DSQRAIL_BUILD_DUCKDB_SHELL=ON
cmake --build build-bench --target sqrail shell --parallel 2
```

Generate deterministic local data and run:

```sh
BENCH_ROWS=10000000 benchmarks/generate.sh \
  benchmark-data build-bench/_deps/duckdb-build/duckdb

SQRAIL_BIN=build-bench/sqrail \
DUCKDB_BIN=build-bench/_deps/duckdb-build/duckdb \
BENCH_MEMORY=512MB \
BENCH_THREADS=2 \
BENCH_RUNS=5 \
benchmarks/run.sh benchmark-data benchmark-results
```

`summary.tsv` contains mean, standard deviation, minimum, maximum, one separately
profiled peak-RSS observation, row count, and a logical checksum. Every sqrail
result must match the DuckDB CLI row count and checksum or the run fails.

The default run uses one warmup and therefore reports warm-cache measurements.
Set `BENCH_WARMUP=0` to preserve first-run timings, but do not describe them as
cold-cache results unless the operating-system page cache was independently
controlled and recorded.
