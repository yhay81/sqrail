#!/usr/bin/env bash
set -euo pipefail

engine=${1:?engine is required}
: "${BENCH_QUERY:?BENCH_QUERY is required}"
: "${BENCH_FACT:?BENCH_FACT is required}"
: "${BENCH_FACT_READER:?BENCH_FACT_READER is required}"
: "${BENCH_DIM:?BENCH_DIM is required}"
: "${BENCH_OUTPUT:?BENCH_OUTPUT is required}"
: "${BENCH_OUTPUT_FORMAT:?BENCH_OUTPUT_FORMAT is required}"
: "${BENCH_MEMORY:?BENCH_MEMORY is required}"
: "${BENCH_THREADS:?BENCH_THREADS is required}"

query=$(<"$BENCH_QUERY")

case $BENCH_OUTPUT_FORMAT in
  parquet)
    copy_options="FORMAT PARQUET"
    ;;
  jsonl)
    copy_options="FORMAT JSON, ARRAY false"
    ;;
  *)
    echo "unsupported benchmark output format: $BENCH_OUTPUT_FORMAT" >&2
    exit 2
    ;;
esac

case $engine in
  sqrail)
    : "${SQRAIL_BIN:?SQRAIL_BIN is required}"
    "$SQRAIL_BIN" run \
      --memory "$BENCH_MEMORY" \
      --threads "$BENCH_THREADS" \
      -t fact="$BENCH_FACT" \
      -t dim="$BENCH_DIM" \
      -o "$BENCH_OUTPUT" \
      "$query"
    ;;
  duckdb)
    : "${DUCKDB_BIN:?DUCKDB_BIN is required}"
    sql_quote() {
      local value=$1
      value=${value//\'/\'\'}
      printf '%s' "$value"
    }
    fact=$(sql_quote "$BENCH_FACT")
    dim=$(sql_quote "$BENCH_DIM")
    output=$(sql_quote "$BENCH_OUTPUT")
    case $BENCH_FACT_READER in
      read_parquet|read_csv_auto)
        ;;
      *)
        echo "unsupported benchmark reader: $BENCH_FACT_READER" >&2
        exit 2
        ;;
    esac
    "$DUCKDB_BIN" -no-stdin -c "
SET memory_limit = '$(sql_quote "$BENCH_MEMORY")';
SET threads = ${BENCH_THREADS};
SET preserve_insertion_order = false;
CREATE TEMP VIEW fact AS SELECT * FROM ${BENCH_FACT_READER}('${fact}');
CREATE TEMP VIEW dim AS SELECT * FROM read_parquet('${dim}');
COPY (${query}) TO '${output}' (${copy_options});
" >/dev/null
    ;;
  *)
    echo "unknown benchmark engine: $engine" >&2
    exit 2
    ;;
esac
