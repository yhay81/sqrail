# v0.1.0 local baseline

This report separates a comparative frontend benchmark from a development
sanity check. It is not a query-engine superiority claim: both compared
executables contain the same DuckDB source revision.

Measured on 2026-07-26:

| Component | Value |
|---|---|
| Host | Apple M2 Pro, 12 CPU cores, 32 GiB RAM |
| OS | macOS 26.5.1, arm64 |
| Compiler | Apple Clang 21.0.0 |
| CMake | 4.4.0 |
| DuckDB | v1.5.5 |
| sqrail | 0.1.0 |

## Executable

| Metric | Result |
|---|---:|
| Unstripped build executable | 43,310,344 bytes |
| Stripped release executable | 35,711,416 bytes |
| Stripped gzip `-9` size | 11,760,406 bytes |
| Dynamic dependencies | system libc++ and libSystem only |
| `sqrail --version` wall time | approximately 0.01 s |
| `sqrail --version` maximum RSS | 2,932,736 bytes |

## sqrail versus the pinned DuckDB CLI

The committed harness built both frontends from DuckDB commit
`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`. The deterministic dataset contains
1,000,000 fact rows, 100,000 dimension rows, a 49,668,679-byte Parquet fact
file, and a 65,666,371-byte CSV fact file.

Each timed value is the mean of five warm-cache runs after one warmup, with a
512 MB DuckDB memory limit and two threads. Peak RSS is one separate profiled
run. All result pairs had identical row counts and logical checksums.

| Workload | DuckDB CLI mean | sqrail mean | DuckDB CLI RSS | sqrail RSS |
|---|---:|---:|---:|---:|
| selective Parquet scan | 38.87 ms | 38.13 ms | 30.9 MiB | 28.4 MiB |
| high-cardinality aggregate | 50.78 ms | 50.86 ms | 49.5 MiB | 44.1 MiB |
| fact-dimension join | 45.53 ms | 40.87 ms | 39.4 MiB | 40.7 MiB |
| partitioned window | 120.44 ms | 113.28 ms | 71.7 MiB | 83.0 MiB |
| CSV to Parquet | 341.25 ms | 327.42 ms | 163.9 MiB | 149.6 MiB |

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

| Metric | Result |
|---|---:|
| Wall time | 0.27 s |
| Maximum RSS | 54,886,400 bytes |
| Output size | approximately 35 MiB |
| Verified rows | 1,000,000 |
| Verified distinct buckets | 100 |

The dataset is generated rather than scanned, and filesystem caching was not
controlled. This result only verifies that direct Parquet output works within
the stated 64 MB DuckDB memory limit on the development host.
