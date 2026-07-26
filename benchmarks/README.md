# Reproducible benchmarks

The benchmark compares complete file-to-file tasks performed by sqrail and the
DuckDB CLI built from the exact same pinned DuckDB source revision. It measures
frontend overhead and contract cost; it does not claim that sqrail has a faster
query engine.

Requirements:

- Bash
- CMake 3.25+, Ninja, and a C++20 compiler
- `hyperfine`
- `jq`
- Python 3 and GNU/BSD `time` for the out-of-core resource harness
- `llvm-profdata` matching the Clang compiler for PGO training

Build both frontends:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DSQRAIL_BUILD_DUCKDB_SHELL=ON
cmake --build build-bench --target sqrail shell --parallel 2
```

For host-specific optimization, use `cmake --preset native`. To train and apply
instrumentation PGO across sqrail and the embedded engine:

```sh
cmake --preset pgo-generate
cmake --build --preset pgo-generate
benchmarks/train-pgo.sh
cmake --workflow --preset pgo-use
```

Portable release results, native results, and PGO results are separate benchmark
cohorts. A host-tuned binary must never be published as a portable release.

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

For an explicit out-of-core sort with sampled peak spill usage:

```sh
BENCH_ROWS=10000000 BENCH_DIM_ROWS=1000000 \
  benchmarks/generate.sh benchmark-data-10m build-bench/_deps/duckdb-build/duckdb

SQRAIL_BIN=out/build/native/sqrail \
BENCH_MEMORY=128MiB \
BENCH_MAX_SPILL=2GiB \
  benchmarks/run-out-of-core.sh benchmark-data-10m benchmark-results-out-of-core
```

The spill monitor samples every 10 ms, so `peak_spill_bytes` is a lower bound.
The memory setting is DuckDB's buffer limit; `peak_rss_bytes` also includes code,
allocator, compression, and query-state memory.
