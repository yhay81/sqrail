#!/usr/bin/env bash
set -euo pipefail

data_dir=${1:-benchmark-data}
duckdb_bin=${2:-duckdb}
rows=${BENCH_ROWS:-1000000}
dimension_rows=${BENCH_DIM_ROWS:-100000}

case $rows in
  ''|*[!0-9]*|0)
    echo "BENCH_ROWS must be a positive integer" >&2
    exit 2
    ;;
esac
case $dimension_rows in
  ''|*[!0-9]*|0)
    echo "BENCH_DIM_ROWS must be a positive integer" >&2
    exit 2
    ;;
esac

if ! command -v "$duckdb_bin" >/dev/null 2>&1 && [ ! -x "$duckdb_bin" ]; then
  echo "DuckDB CLI not found: $duckdb_bin" >&2
  exit 2
fi

mkdir -p "$data_dir"
for file in fact.parquet fact.csv dim.parquet manifest.json; do
  if [ -e "$data_dir/$file" ]; then
    echo "refusing to replace existing benchmark file: $data_dir/$file" >&2
    exit 5
  fi
done

sql_quote() {
  local value=$1
  value=${value//\'/\'\'}
  printf '%s' "$value"
}

fact_parquet=$(sql_quote "$data_dir/fact.parquet")
fact_csv=$(sql_quote "$data_dir/fact.csv")
dim_parquet=$(sql_quote "$data_dir/dim.parquet")

"$duckdb_bin" -no-stdin -c "
SET preserve_insertion_order = false;
COPY (
  SELECT
    i::BIGINT AS event_id,
    (i % ${dimension_rows})::INTEGER AS drug_id,
    DATE '2020-01-01' + (i % 1825)::INTEGER AS event_date,
    ((hash(i) % 1000000)::DOUBLE / 1000000.0) AS value,
    md5(i::VARCHAR) AS payload
  FROM range(${rows}) AS generated(i)
) TO '${fact_parquet}' (FORMAT PARQUET);
COPY (
  SELECT
    i::BIGINT AS event_id,
    (i % ${dimension_rows})::INTEGER AS drug_id,
    DATE '2020-01-01' + (i % 1825)::INTEGER AS event_date,
    ((hash(i) % 1000000)::DOUBLE / 1000000.0) AS value,
    md5(i::VARCHAR) AS payload
  FROM range(${rows}) AS generated(i)
) TO '${fact_csv}' (FORMAT CSV, HEADER true);
COPY (
  SELECT
    i::INTEGER AS drug_id,
    'class-' || (i % 32)::VARCHAR AS drug_class,
    'compound-' || i::VARCHAR AS compound_name
  FROM range(${dimension_rows}) AS generated(i)
) TO '${dim_parquet}' (FORMAT PARQUET);
" >/dev/null

duckdb_version=$("$duckdb_bin" --version | tr '\n' ' ')
cat > "$data_dir/manifest.json" <<EOF
{
  "rows": ${rows},
  "dimension_rows": ${dimension_rows},
  "duckdb": "${duckdb_version}",
  "fact_parquet_bytes": $(wc -c < "$data_dir/fact.parquet" | tr -d ' '),
  "fact_csv_bytes": $(wc -c < "$data_dir/fact.csv" | tr -d ' '),
  "dim_parquet_bytes": $(wc -c < "$data_dir/dim.parquet" | tr -d ' ')
}
EOF

printf 'generated benchmark data in %s\n' "$data_dir"
