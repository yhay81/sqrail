# Performance baselines

This document preserves development-host measurements from v0.1.0 through
v0.2.1. They are historical, hardware-specific evidence rather than current
release guarantees. Re-run the committed harness before making a comparison
against a newer sqrail, DuckDB, compiler, or machine.

This report separates a comparative frontend benchmark from a development
sanity check. It is not a query-engine superiority claim: both compared
executables contain the same DuckDB source revision.

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
