# Performance baselines

This document records development-host measurements across sqrail releases,
including the current DuckDB v2.0.0 baseline and preserved v0.1.0 through v0.2.1
cohorts. They are hardware-specific evidence rather than cross-machine release
guarantees. Re-run the committed harness before making a comparison against a
newer sqrail, DuckDB, compiler, or machine.

This report separates a comparative frontend benchmark from resource and write
sanity checks. It is not a query-engine superiority claim: both compared
executables contain the exact same DuckDB source revision.

## v0.4 (DuckDB v2.0.0) baseline

Measured on 2026-11-12 from the portable Release/IPO build tree (with event-driven
execution control and synchronous uncompressed JSON byte-limit enforcement):

| Component | Value                                               |
| --------- | --------------------------------------------------- |
| Host      | Apple M2 Pro, 12 CPU cores, 32 GiB RAM              |
| OS        | macOS 27.2, arm64                                   |
| Compiler  | Apple Clang 21.0.0                                  |
| CMake     | 4.4.5                                               |
| DuckDB    | v2.0.0 (`d54cbbe3df17e8e5ce68312f483b4155836112aa`) |
| sqrail    | 0.4.1+ (untagged working tree on `e7e4ebae2cf1`)    |

### Executable and warm-cache startup

| Metric                                              |                           Result |
| --------------------------------------------------- | -------------------------------: |
| Unstripped release executable                       |                 58,466,872 bytes |
| Stripped release executable (`strip -x`)            |                 50,045,272 bytes |
| Stripped gzip `-9` size                             |                 17,973,665 bytes |
| Dynamic dependencies                                | system libc++ and libSystem only |
| `sqrail --version` maximum RSS                      |                  2,703,360 bytes |
| `sqrail --version` mean (50 warm-cache runs)        |                   2.58 ± 0.11 ms |
| `duckdb --version` mean (50 warm-cache runs)        |                  20.79 ± 0.66 ms |
| `sqrail run 'SELECT 1'` mean (50 warm-cache runs)   |                  18.40 ± 2.69 ms |
| `duckdb -no-init -no-stdin -c 'SELECT 1'` (50 runs) |                  20.64 ± 0.67 ms |

The startup runner (`benchmarks/run-startup.sh`) performs five warmups before
recording 50 Hyperfine runs (`cache.state=warm`), so these timings reflect warm
page-cache process startup rather than cold-cache disk reads. `sqrail --version`
returns before initializing DuckDB's static extension registry.

### Twelve-case comparative suite versus pinned DuckDB v2.0.0 CLI

Both frontends were built from DuckDB commit
`d54cbbe3df17e8e5ce68312f483b4155836112aa`. The deterministic 1,000,000-row
dataset (`benchmarks/generate.sh`) contains 1,000,000 fact rows, 100,000
dimension rows, a 49,669,166-byte Parquet fact file, a 65,666,371-byte CSV fact
file, and an 897,598-byte Parquet dimension file.

Each timed value is the mean of five warm-cache runs after one warmup
(`cache.state=warm`), with a 512 MB DuckDB memory limit, an isolated 8 GiB spill
cap, and two threads. Peak RSS comes from one separate profiled run. Every
result pair produced identical row counts and logical checksums.

| Workload                     | DuckDB CLI mean | sqrail mean | DuckDB CLI RSS | sqrail RSS |
| ---------------------------- | --------------: | ----------: | -------------: | ---------: |
| `scan_parquet_selective`     |        42.05 ms |    34.45 ms |       53.4 MiB |   55.7 MiB |
| `scan_csv_selective`         |       149.08 ms |   140.82 ms |      102.0 MiB |   97.9 MiB |
| `scan_parquet_nonselective`  |       166.52 ms |   156.10 ms |      237.5 MiB |  224.8 MiB |
| `aggregate_low_cardinality`  |        42.62 ms |    35.76 ms |       56.0 MiB |   48.3 MiB |
| `aggregate_high_cardinality` |        56.35 ms |    41.35 ms |       62.3 MiB |   60.7 MiB |
| `join_small_large`           |        44.07 ms |    35.47 ms |       62.5 MiB |   55.3 MiB |
| `join_large_large`           |        51.96 ms |    48.59 ms |       82.2 MiB |   73.8 MiB |
| `sort_parquet`               |       234.20 ms |   210.68 ms |      247.4 MiB |  253.1 MiB |
| `distinct_parquet`           |        93.49 ms |    85.13 ms |       95.8 MiB |   87.6 MiB |
| `window_parquet`             |        93.62 ms |    89.92 ms |       91.6 MiB |   89.3 MiB |
| `csv_to_parquet`             |       246.58 ms |   237.92 ms |      172.5 MiB |  167.3 MiB |
| `parquet_to_jsonl`           |       351.92 ms |   218.16 ms |      103.8 MiB |  102.5 MiB |

Small advantages on short queries reflect lower CLI initialization overhead
rather than faster SQL execution. In `parquet_to_jsonl` (123,666,439-byte
output), run-to-run variance is dominated by host filesystem writeback flushing
(DuckDB CLI min 188.51 ms / max 603.56 ms; sqrail min 166.44 ms / max
289.04 ms), and the two paths are not semantics-equivalent when inputs contain
non-finite floats because sqrail normalizes `NaN`/`Infinity` to `null`.

### Ten-million-row out-of-core sort (`run-out-of-core.sh`)

Single `first-run-uncontrolled` run sorting 10,000,000 Parquet rows with a
128 MiB DuckDB memory limit, two threads, and a 2 GiB spill cap:

| Metric              |         Observed value |
| ------------------- | ---------------------: |
| Input Parquet       |      496,600,530 bytes |
| Harness wall time   |                1.857 s |
| Maximum process RSS |      256,671,744 bytes |
| Sampled peak spill  |      270,336,000 bytes |
| Output Parquet      |      121,725,094 bytes |
| Verified rows       |             10,000,000 |
| Logical checksum    | `14650138637360173841` |

## Historical v0.1.0 baseline (DuckDB v1.5.5)

Measured on 2026-07-26:

| Component | Value                                  |
| --------- | -------------------------------------- |
| Host      | Apple M2 Pro, 12 CPU cores, 32 GiB RAM |
| OS        | macOS 26.5.1, arm64                    |
| Compiler  | Apple Clang 21.0.0                     |
| CMake     | 4.4.0                                  |
| DuckDB    | v1.5.5                                 |
| sqrail    | 0.1.0                                  |

## Executable

| Metric                         |                           Result |
| ------------------------------ | -------------------------------: |
| Unstripped build executable    |                 43,310,344 bytes |
| Stripped release executable    |                 35,711,416 bytes |
| Stripped gzip `-9` size        |                 11,760,406 bytes |
| Dynamic dependencies           | system libc++ and libSystem only |
| `sqrail --version` wall time   |             approximately 0.01 s |
| `sqrail --version` maximum RSS |                  2,932,736 bytes |

## sqrail versus the pinned DuckDB CLI

The committed harness built both frontends from DuckDB commit
`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`. The deterministic dataset contains
1,000,000 fact rows, 100,000 dimension rows, a 49,668,679-byte Parquet fact
file, and a 65,666,371-byte CSV fact file.

Each timed value is the mean of five warm-cache runs after one warmup, with a
512 MB DuckDB memory limit and two threads. Peak RSS is one separate profiled
run. All result pairs had identical row counts and logical checksums.

| Workload                   | DuckDB CLI mean | sqrail mean | DuckDB CLI RSS | sqrail RSS |
| -------------------------- | --------------: | ----------: | -------------: | ---------: |
| selective Parquet scan     |        38.87 ms |    38.13 ms |       30.9 MiB |   28.4 MiB |
| high-cardinality aggregate |        50.78 ms |    50.86 ms |       49.5 MiB |   44.1 MiB |
| fact-dimension join        |        45.53 ms |    40.87 ms |       39.4 MiB |   40.7 MiB |
| partitioned window         |       120.44 ms |   113.28 ms |       71.7 MiB |   83.0 MiB |
| CSV to Parquet             |       341.25 ms |   327.42 ms |      163.9 MiB |  149.6 MiB |

At this scale, sqrail adds no measurable systematic runtime penalty. Individual
differences should not be interpreted as engine improvements: the query engine,
plans, data, settings, and output checks are the same, while the frontends have
different startup and command-processing paths. The window case also shows why
the project reports memory rather than assuming the smaller interface always
uses less.

Reproduce with [the benchmark instructions](../benchmarks/README.md).

## One-million-row write sanity check

Command:

```sh
sqrail run \
  --memory 64MB \
  --threads 2 \
  -o million.parquet \
  'SELECT i, i % 100 AS bucket, md5(i::VARCHAR) AS digest
   FROM range(1000000) t(i)'
```

Single observed run:

| Metric                    |               Result |
| ------------------------- | -------------------: |
| Wall time                 |               0.27 s |
| Maximum RSS               |     54,886,400 bytes |
| Output size               | approximately 35 MiB |
| Verified rows             |            1,000,000 |
| Verified distinct buckets |                  100 |

The dataset is generated rather than scanned, and filesystem caching was not
controlled. This result only verifies that direct Parquet output works within
the stated 64 MB DuckDB memory limit on the development host.

## v0.2.0 host-tuned development results

These measurements use the same host and DuckDB revision as the v0.1.0
baseline. The portable comparison binary uses a conventional Release build.
The host-tuned binary additionally applies ThinLTO and `-mcpu=native`; it is not
portable and is never used for release archives.

| Workload                                  | Portable mean | Host-tuned mean | Elapsed reduction |
| ----------------------------------------- | ------------: | --------------: | ----------------: |
| 1M-row Parquet grouped aggregate, 15 runs |      105.0 ms |         93.9 ms |             10.5% |
| 1M-row generated strict JSONL, 10 runs    |      612.8 ms |        574.4 ms |              6.3% |

The grouped aggregate used two threads. Both cohorts were warm-cache
measurements; the comparison combines native code generation and ThinLTO and
does not isolate their individual contributions.

An explicit out-of-core run then fully sorted 10,000,000 Parquet rows with a
128 MiB DuckDB memory limit, two threads, and a 2 GiB spill cap:

| Metric              |    Observed value |
| ------------------- | ----------------: |
| Input Parquet       | 496,596,101 bytes |
| Harness wall time   |           2.123 s |
| Maximum process RSS | 276,168,704 bytes |
| Sampled peak spill  | 215,154,688 bytes |
| Output Parquet      | 121,696,159 bytes |
| Verified rows       |        10,000,000 |

The spill sampler runs every 10 ms, so its maximum is a lower bound. DuckDB's
memory setting limits its buffer manager rather than total process RSS. The
committed `run-out-of-core.sh` harness refuses to replace results and validates
the output row count and logical checksum.

## v0.2.1 strict JSON streaming optimization

The expanded matrix exposed a frontend bottleneck that query-engine tuning
cannot fix: converting 10,000,000 Parquet rows to strict JSONL spent most of its
time materializing one DuckDB `Value` and one `std::string` per row. The revised
path reads DuckDB's unified vector buffer directly and allocates only for rows
that contain a non-finite token requiring RFC 8259 normalization.

The before and after binaries used the same Release/IPO build tree, Apple Clang
21, DuckDB commit `d8cdaa33f`, 512 MB memory limit, two threads, and an 8 GiB
explicit spill cap. Each result is the mean of five runs after one warmup.

| 10M-row Parquet-to-JSONL path  |    Mean | Relative to old sqrail |
| ------------------------------ | ------: | ---------------------: |
| DuckDB CLI `COPY`              | 2.207 s |           3.35x faster |
| sqrail before vector fast path | 7.398 s |               baseline |
| sqrail after vector fast path  | 2.728 s |           2.71x faster |

All three outputs contained 10,000,000 rows and the same logical checksum
`13911955137382488891`. The optimized sqrail path remains 23.6% slower than
DuckDB's direct JSON `COPY`; that comparison is not semantics-equivalent when
data contains NaN or infinities, because DuckDB emits bare non-standard tokens
while sqrail maps them to `null`. Unit and smoke tests verify the strict
conversion for scalar and nested non-finite values.
