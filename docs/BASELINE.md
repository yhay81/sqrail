# Initial local baseline

This is a development sanity check, not a comparative performance claim.

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
| Uncompressed executable | 41 MiB |
| gzip `-9` size | 12,616,146 bytes |
| Dynamic dependencies | system libc++ and libSystem only |
| `sqrail --version` wall time | approximately 0.01 s |
| `sqrail --version` maximum RSS | 2,916,352 bytes |

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
