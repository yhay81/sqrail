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

Measure frontend startup paths independently from data workloads:

```sh
SQRAIL_BIN=build-bench/sqrail \
DUCKDB_BIN=build-bench/_deps/duckdb-build/duckdb \
BENCH_RUNS=50 \
benchmarks/run-startup.sh benchmark-results-startup
```

This records raw Hyperfine samples, a Markdown summary, exact tool versions, and
host metadata for version-only and `SELECT 1` processes. The default five
warmups make this a warm-cache measurement; it must not be described as cold
start.

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

`summary.tsv` contains mean, standard deviation, minimum, maximum, Hyperfine
user/system CPU time, one separately profiled peak-RSS observation, output
bytes, row count, and a logical checksum. Every sqrail result must match the
DuckDB CLI row count and checksum or the run fails.
The default suite covers 12 scan, filter, aggregate, join, blocking, and
conversion cases. Final result files are removed after validation so the
working set contains at most one benchmark output. Set `BENCH_KEEP_OUTPUTS=1`
only when inspecting generated results is worth the additional disk space.

Run the complete suite at the policy memory budgets:

```sh
SQRAIL_BIN=build-bench/sqrail \
DUCKDB_BIN=build-bench/_deps/duckdb-build/duckdb \
BENCH_MEMORIES="512MB 1GB 4GB" \
BENCH_MAX_SPILL=8GiB \
BENCH_RUNS=5 \
benchmarks/run-matrix.sh benchmark-data benchmark-results-matrix
```

The combined `benchmark-results-matrix/summary.tsv` adds the memory budget to
each result row. Individual hyperfine data and environment manifests remain in
the per-budget subdirectories. Both frontends receive isolated explicit spill
directories under the result tree and the same `BENCH_MAX_SPILL` limit. The
runner safely clears only directories carrying its private benchmark marker.

Before writing data, `generate.sh` estimates required capacity and requires 2x
headroom by default. Preview a large generation request without writing rows:

```sh
BENCH_ROWS=100000000 BENCH_DIM_ROWS=1000000 BENCH_DRY_RUN=1 \
  benchmarks/generate.sh benchmark-data-100m build-bench/_deps/duckdb-build/duckdb
```

Tune the conservative estimate with `BENCH_BYTES_PER_ROW_ESTIMATE` or the
multiplier with `BENCH_CAPACITY_HEADROOM` only after measuring the target
filesystem. An insufficient-capacity result is a safety stop, not a benchmark
failure.

The default run uses one warmup and therefore records `cache.state=warm`.
`BENCH_WARMUP=0` records `first-run-uncontrolled`; it is not a cold-cache
claim. A caller that independently controls the operating-system page cache may
set `BENCH_CACHE_STATE=cold-controlled` only together with a non-empty
`BENCH_CACHE_CONTROL_EVIDENCE` describing the exact mechanism. The environment
manifest also records OS, architecture, CPU model/count, RAM, storage capacity,
compiler, git commit/dirty state, binary sizes and SHA-256 digests, dataset
manifest digest, parameters, and locale without copying arbitrary environment
variables.

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

To evaluate the agent-facing contract rather than engine execution, use the
[agent task-completion protocol](AGENT_EVALUATION.md). Its primary
identity-concealed runner, [`agent-eval/run.py`](agent-eval/run.py), uses the
eight-task [`agent-tasks.json`](agent-tasks.json) corpus, hides tool identity
behind `./rail`, randomizes the crossed schedule, retains raw agent events, and
scores every oracle before revealing the condition allocation. It can cross
tasks with clean, noisy-workspace, superseded-handoff, and prior-error contexts.
The [contract experiment log](agent-eval/EXPERIMENTS.md) records low-cost-model
pilots separately from v0.3 release evidence.

The complementary v0.3 artifact gate uses the twelve-task
[`agent-tasks-v0.3.json`](agent-tasks-v0.3.json) corpus. After recording
attempts through `agent-run.py` as JSONL, independently recompute the evidence
and enforce completeness with:

```sh
python3 benchmarks/agent-eval.py agent-results.jsonl \
  --artifacts agent-artifacts \
  --data benchmark-data \
  --sqrail build-bench/sqrail \
  --duckdb build-bench/_deps/duckdb-build/duckdb \
  > agent-evaluation-report.json
```

See [the agent protocol](AGENT_EVALUATION.md) for both lifecycles. A report
derived only from recorded success booleans cannot pass the release gate.
