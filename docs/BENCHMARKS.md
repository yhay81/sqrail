# Benchmark policy

Performance claims are accepted only when the benchmark is:

- reproducible from a committed script
- run from local storage
- verified by output checksum
- measured under an explicit memory budget
- reported with engine, compiler, OS, CPU, RAM, and storage versions
- run with cold and warm filesystem cache results kept separate

## Metrics

- wall-clock time
- CPU time
- peak resident set size
- bytes read and written
- temporary spill bytes
- output bytes and checksum
- executable size
- cold-start latency

## Initial workload matrix

| Class | Required cases |
|---|---|
| Scan | CSV and Parquet; narrow and wide projection |
| Filter | selective and non-selective |
| Aggregate | low and high cardinality |
| Join | small-large and large-large |
| Blocking | sort, distinct, and window |
| Conversion | CSV to Parquet and Parquet to JSONL |

Each workload is run at 512 MB, 1 GB, and 4 GB where the operating system and
dataset size make the budget meaningful.

The first baseline is the upstream DuckDB CLI using equivalent SQL and settings.
sqrail must not claim a query-engine speed advantage when both paths execute the
same DuckDB plan. Its initial performance claims concern startup, frontend
overhead, result streaming, resource configuration, and task completion under a
fixed contract.
