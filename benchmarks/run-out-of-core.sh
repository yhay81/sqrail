#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$script_dir/.." && pwd)
data_dir=${1:-benchmark-data-10m}
result_dir=${2:-benchmark-results-out-of-core}
sqrail_bin=${SQRAIL_BIN:-build/sqrail}
memory=${BENCH_MEMORY:-128MiB}
max_spill=${BENCH_MAX_SPILL:-2GiB}
threads=${BENCH_THREADS:-2}
timeout=${BENCH_TIMEOUT:-10m}
cache_state=${BENCH_CACHE_STATE:-first-run-uncontrolled}
cache_evidence=${BENCH_CACHE_CONTROL_EVIDENCE:-}

for command in "$sqrail_bin" jq python3; do
  if ! command -v "$command" >/dev/null 2>&1 && [ ! -x "$command" ]; then
    echo "required command not found: $command" >&2
    exit 2
  fi
done
case $threads in
  ''|*[!0-9]*|0)
    echo "BENCH_THREADS must be a positive integer" >&2
    exit 2
    ;;
esac
for size in "$memory" "$max_spill"; do
  case $size in
    ''|*[!A-Za-z0-9.]*)
      echo "memory and spill limits must use compact size syntax such as 128MiB" >&2
      exit 2
      ;;
  esac
done
case $timeout in
  ''|*[!A-Za-z0-9.]*)
    echo "BENCH_TIMEOUT must use compact duration syntax such as 10m" >&2
    exit 2
    ;;
esac
if [ ! -f "$data_dir/fact.parquet" ] || [ ! -f "$data_dir/manifest.json" ]; then
  echo "out-of-core benchmark input is incomplete: $data_dir" >&2
  exit 3
fi
if [ -e "$result_dir" ]; then
  echo "refusing to replace existing result directory: $result_dir" >&2
  exit 5
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

mkdir -p "$result_dir/spill"
output_file="$result_dir/sorted.parquet"
stats_file="$result_dir/time.txt"

case $(uname -s) in
  Darwin)
    time_command=(/usr/bin/time -lp)
    ;;
  Linux)
    time_command=(/usr/bin/time -v)
    ;;
  *)
    echo "resource measurement is not supported on $(uname -s)" >&2
    exit 2
    ;;
esac

start_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
set +e
"${time_command[@]}" "$sqrail_bin" run \
  --memory "$memory" \
  --threads "$threads" \
  --spill "$result_dir/spill" \
  --max-spill "$max_spill" \
  --timeout "$timeout" \
  -t fact="$data_dir/fact.parquet" \
  -o "$output_file" \
  'SELECT event_id, drug_id, event_date, value
   FROM fact
   ORDER BY value DESC, event_id' 2>"$stats_file" &
query_pid=$!
set -e

peak_spill_kib=0
while kill -0 "$query_pid" 2>/dev/null; do
  current_spill_kib=$(du -sk "$result_dir/spill" | awk '{print $1}')
  if [ "$current_spill_kib" -gt "$peak_spill_kib" ]; then
    peak_spill_kib=$current_spill_kib
  fi
  sleep 0.01
done

set +e
wait "$query_pid"
query_status=$?
set -e
end_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
if [ "$query_status" -ne 0 ]; then
  cat "$stats_file" >&2
  exit "$query_status"
fi

case $(uname -s) in
  Darwin)
    peak_rss_bytes=$(
      awk '/maximum resident set size/ { print $1; found = 1; exit }
           END { if (!found) exit 1 }' "$stats_file"
    )
    user_seconds=$(
      awk '$2 == "user" { print $1; exit }
           $1 == "user" { print $2; exit }' "$stats_file"
    )
    system_seconds=$(
      awk '$2 == "sys" { print $1; exit }
           $1 == "sys" { print $2; exit }' "$stats_file"
    )
    ;;
  Linux)
    peak_rss_kib=$(
      awk -F: '/Maximum resident set size/ {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
        print $2
        found = 1
        exit
      }
      END { if (!found) exit 1 }' "$stats_file"
    )
    peak_rss_bytes=$((peak_rss_kib * 1024))
    user_seconds=$(
      awk -F: '/User time/ {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
        print $2
        exit
      }' "$stats_file"
    )
    system_seconds=$(
      awk -F: '/System time/ {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
        print $2
        exit
      }' "$stats_file"
    )
    ;;
esac

validation=$(
  "$sqrail_bin" run -t result="$output_file" \
    'SELECT count(*) AS rows, bit_xor(hash(row_value)) AS checksum
     FROM result AS row_value'
)
rows=$(printf '%s\n' "$validation" | jq -er '.rows')
checksum=$(printf '%s\n' "$validation" | jq -er '.checksum')
elapsed_seconds=$(
  awk -v start="$start_ns" -v end="$end_ns" 'BEGIN { printf "%.6f", (end - start) / 1000000000 }'
)

jq -n \
  --arg sqrail "$("$sqrail_bin" --version)" \
  --arg data "$data_dir/fact.parquet" \
  --arg memory "$memory" \
  --arg max_spill "$max_spill" \
  --argjson threads "$threads" \
  --argjson elapsed_seconds "$elapsed_seconds" \
  --argjson user_seconds "$user_seconds" \
  --argjson system_seconds "$system_seconds" \
  --argjson peak_rss_bytes "$peak_rss_bytes" \
  --argjson peak_spill_bytes "$((peak_spill_kib * 1024))" \
  --argjson output_bytes "$(wc -c < "$output_file" | tr -d ' ')" \
  --argjson rows "$rows" \
  --arg checksum "$checksum" \
  '{
    sqrail: $sqrail,
    data: $data,
    memory: $memory,
    max_spill: $max_spill,
    threads: $threads,
    elapsed_seconds: $elapsed_seconds,
    user_seconds: $user_seconds,
    system_seconds: $system_seconds,
    peak_rss_bytes: $peak_rss_bytes,
    peak_spill_bytes: $peak_spill_bytes,
    output_bytes: $output_bytes,
    rows: $rows,
    checksum: $checksum
  }' > "$result_dir/summary.json"

parameters=$(
  jq -cn \
    --arg memory "$memory" \
    --arg max_spill "$max_spill" \
    --arg timeout "$timeout" \
    --argjson threads "$threads" \
    '{
      memory: $memory,
      max_spill: $max_spill,
      timeout: $timeout,
      threads: $threads
    }'
)
python3 "$script_dir/capture-environment.py" \
  --output "$result_dir/environment.json" \
  --repository "$repo_dir" \
  --sqrail "$sqrail_bin" \
  --data-manifest "$data_dir/manifest.json" \
  --cache-state "$cache_state" \
  --cache-control-evidence "$cache_evidence" \
  --parameters-json "$parameters"

printf 'out-of-core benchmark results written to %s\n' "$result_dir"
