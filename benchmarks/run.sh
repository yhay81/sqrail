#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$script_dir/.." && pwd)
data_dir=${1:-benchmark-data}
result_dir=${2:-benchmark-results}

sqrail_bin=${SQRAIL_BIN:-"$repo_dir/build-bench/sqrail"}
duckdb_bin=${DUCKDB_BIN:-"$repo_dir/build-bench/_deps/duckdb-build/duckdb"}
memory=${BENCH_MEMORY:-512MB}
max_spill=${BENCH_MAX_SPILL:-8GiB}
threads=${BENCH_THREADS:-2}
runs=${BENCH_RUNS:-5}
warmup=${BENCH_WARMUP:-1}
keep_outputs=${BENCH_KEEP_OUTPUTS:-0}
cache_state=${BENCH_CACHE_STATE:-}
cache_evidence=${BENCH_CACHE_CONTROL_EVIDENCE:-}

for size in "$memory" "$max_spill"; do
  case $size in
    ''|*[!A-Za-z0-9.]*)
      echo "memory and spill limits must use compact size syntax such as 512MB or 8GiB" >&2
      exit 2
      ;;
  esac
done
for command in "$sqrail_bin" "$duckdb_bin" hyperfine jq python3; do
  if ! command -v "$command" >/dev/null 2>&1 && [ ! -x "$command" ]; then
    echo "required command not found: $command" >&2
    exit 2
  fi
done

for number in "$threads" "$runs" "$warmup"; do
  case $number in
    ''|*[!0-9]*)
      echo "thread, run, and warmup values must be integers" >&2
      exit 2
      ;;
  esac
done
if [ "$threads" -eq 0 ] || [ "$runs" -eq 0 ]; then
  echo "BENCH_THREADS and BENCH_RUNS must be greater than zero" >&2
  exit 2
fi
if [ "$keep_outputs" != 0 ] && [ "$keep_outputs" != 1 ]; then
  echo "BENCH_KEEP_OUTPUTS must be 0 or 1" >&2
  exit 2
fi
if [ -z "$cache_state" ]; then
  if [ "$warmup" -gt 0 ]; then
    cache_state=warm
  else
    cache_state=first-run-uncontrolled
  fi
fi
case $cache_state in
  warm|cold-controlled|first-run-uncontrolled)
    ;;
  *)
    echo "BENCH_CACHE_STATE must be warm, cold-controlled, or first-run-uncontrolled" >&2
    exit 2
    ;;
esac
if [ "$cache_state" = cold-controlled ] && [ -z "$cache_evidence" ]; then
  echo "cold-controlled runs require BENCH_CACHE_CONTROL_EVIDENCE" >&2
  exit 2
fi

for file in fact.parquet fact.csv dim.parquet manifest.json; do
  if [ ! -f "$data_dir/$file" ]; then
    echo "benchmark input missing: $data_dir/$file" >&2
    exit 3
  fi
done

if [ -e "$result_dir" ]; then
  echo "refusing to replace existing result directory: $result_dir" >&2
  exit 5
fi
mkdir -p "$result_dir/outputs" "$result_dir/hyperfine"

export SQRAIL_BIN="$sqrail_bin"
export DUCKDB_BIN="$duckdb_bin"
export BENCH_DIM="$data_dir/dim.parquet"
export BENCH_MEMORY="$memory"
export BENCH_MAX_SPILL="$max_spill"
export BENCH_THREADS="$threads"

printf 'case\tengine\tmean_s\tstddev_s\tmin_s\tmax_s\tuser_s\tsystem_s\tpeak_rss_bytes\toutput_bytes\trows\tchecksum\n' \
  > "$result_dir/summary.tsv"

sql_quote() {
  local value=$1
  value=${value//\'/\'\'}
  printf '%s' "$value"
}

run_case() {
  local case_name=$1
  local fact_file=$2
  local fact_reader=$3
  local query_file=$4
  local output_format=$5
  local expected_fingerprint=
  local output_reader=
  local output_path=

  export BENCH_FACT="$fact_file"
  export BENCH_FACT_READER="$fact_reader"
  export BENCH_QUERY="$query_file"
  export BENCH_OUTPUT_FORMAT="$output_format"

  case $output_format in
    parquet)
      output_reader=read_parquet
      ;;
    jsonl)
      output_reader=read_json_auto
      ;;
    *)
      echo "unsupported benchmark output format: $output_format" >&2
      exit 2
      ;;
  esac

  for engine in duckdb sqrail; do
    export BENCH_OUTPUT="$result_dir/outputs/${case_name}-${engine}.${output_format}"
    export BENCH_SPILL="$result_dir/spill/${case_name}-${engine}"
    result_json="$result_dir/hyperfine/${case_name}-${engine}.json"
    printf -v prepare_command \
      '%q %q %q' "$script_dir/prepare-output.sh" "$BENCH_OUTPUT" "$BENCH_SPILL"
    printf -v engine_command '%q %q' "$script_dir/run-engine.sh" "$engine"

    hyperfine \
      --warmup "$warmup" \
      --runs "$runs" \
      --prepare "$prepare_command" \
      --export-json "$result_json" \
      "$engine_command"

    "$script_dir/prepare-output.sh" "$BENCH_OUTPUT" "$BENCH_SPILL"
    rss=$(
      "$script_dir/measure-rss.sh" "$engine" \
        "$result_dir/hyperfine/${case_name}-${engine}-rss.txt"
    )

    fingerprint=$(
      output_path=$(sql_quote "$BENCH_OUTPUT")
      "$duckdb_bin" -csv -noheader -c \
        "SELECT count(*), bit_xor(hash(row_value))
         FROM ${output_reader}('${output_path}') AS row_value"
    )
    if [ -z "$expected_fingerprint" ]; then
      expected_fingerprint=$fingerprint
    elif [ "$fingerprint" != "$expected_fingerprint" ]; then
      echo "$case_name output mismatch: $fingerprint != $expected_fingerprint" >&2
      exit 1
    fi

    rows=${fingerprint%%,*}
    checksum=${fingerprint#*,}
    output_bytes=$(wc -c < "$BENCH_OUTPUT" | tr -d ' ')
    jq -r \
      --arg case_name "$case_name" \
      --arg engine "$engine" \
      --arg rss "$rss" \
      --arg output_bytes "$output_bytes" \
      --arg rows "$rows" \
      --arg checksum "$checksum" \
      '[
        $case_name,
        $engine,
        (.results[0].mean | tostring),
        (.results[0].stddev | tostring),
        (.results[0].min | tostring),
        (.results[0].max | tostring),
        (.results[0].user | tostring),
        (.results[0].system | tostring),
        $rss,
        $output_bytes,
        $rows,
        $checksum
      ] | @tsv' "$result_json" >> "$result_dir/summary.tsv"

    "$script_dir/prepare-output.sh" "$BENCH_OUTPUT" "$BENCH_SPILL" "$keep_outputs"
  done
}

run_case scan_parquet_selective \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/scan.sql" parquet
run_case scan_csv_selective \
  "$data_dir/fact.csv" read_csv_auto "$script_dir/queries/scan.sql" parquet
run_case scan_parquet_nonselective \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/scan-wide.sql" parquet
run_case aggregate_low_cardinality \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/aggregate-low.sql" parquet
run_case aggregate_high_cardinality \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/aggregate.sql" parquet
run_case join_small_large \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/join.sql" parquet
run_case join_large_large \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/join-large.sql" parquet
run_case sort_parquet \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/sort.sql" parquet
run_case distinct_parquet \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/distinct.sql" parquet
run_case window_parquet \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/window.sql" parquet
run_case csv_to_parquet \
  "$data_dir/fact.csv" read_csv_auto "$script_dir/queries/conversion.sql" parquet
run_case parquet_to_jsonl \
  "$data_dir/fact.parquet" read_parquet "$script_dir/queries/conversion.sql" jsonl

cp "$data_dir/manifest.json" "$result_dir/data-manifest.json"
parameters=$(
  jq -cn \
    --arg memory "$memory" \
    --arg max_spill "$max_spill" \
    --argjson threads "$threads" \
    --argjson runs "$runs" \
    --argjson warmup "$warmup" \
    --argjson keep_outputs "$keep_outputs" \
    '{
      memory: $memory,
      max_spill: $max_spill,
      threads: $threads,
      runs: $runs,
      warmup: $warmup,
      keep_outputs: $keep_outputs
    }'
)
python3 "$script_dir/capture-environment.py" \
  --output "$result_dir/environment.json" \
  --repository "$repo_dir" \
  --sqrail "$sqrail_bin" \
  --duckdb "$duckdb_bin" \
  --data-manifest "$data_dir/manifest.json" \
  --cache-state "$cache_state" \
  --cache-control-evidence "$cache_evidence" \
  --parameters-json "$parameters"

printf 'benchmark results written to %s\n' "$result_dir"
