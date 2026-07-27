#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$script_dir/.." && pwd)
result_dir=${1:-benchmark-results-startup}

sqrail_bin=${SQRAIL_BIN:-"$repo_dir/build-bench/sqrail"}
duckdb_bin=${DUCKDB_BIN:-"$repo_dir/build-bench/_deps/duckdb-build/duckdb"}
runs=${BENCH_RUNS:-50}
warmup=${BENCH_WARMUP:-5}
cache_state=${BENCH_CACHE_STATE:-}
cache_evidence=${BENCH_CACHE_CONTROL_EVIDENCE:-}

for command in "$sqrail_bin" "$duckdb_bin" hyperfine jq python3; do
  if ! command -v "$command" >/dev/null 2>&1 && [ ! -x "$command" ]; then
    echo "required command not found: $command" >&2
    exit 2
  fi
done
for number in "$runs" "$warmup"; do
  case $number in
    ''|*[!0-9]*)
      echo "BENCH_RUNS and BENCH_WARMUP must be non-negative integers" >&2
      exit 2
      ;;
  esac
done
if [ "$runs" -eq 0 ]; then
  echo "BENCH_RUNS must be greater than zero" >&2
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
if [ -e "$result_dir" ]; then
  echo "refusing to replace existing result directory: $result_dir" >&2
  exit 5
fi
mkdir -p "$result_dir"

printf -v sqrail_command '%q' "$sqrail_bin"
printf -v duckdb_command '%q' "$duckdb_bin"

hyperfine \
  --shell=none \
  --warmup "$warmup" \
  --runs "$runs" \
  --export-json "$result_dir/raw.json" \
  --export-markdown "$result_dir/summary.md" \
  --command-name sqrail-version "$sqrail_command --version" \
  --command-name duckdb-version "$duckdb_command --version" \
  --command-name sqrail-select-one "$sqrail_command run 'SELECT 1'" \
  --command-name duckdb-select-one "$duckdb_command -no-stdin -c 'SELECT 1'"

parameters=$(
  jq -cn \
  --arg hyperfine "$(hyperfine --version | tr '\n' ' ')" \
  --argjson runs "$runs" \
  --argjson warmup "$warmup" \
  '{
    hyperfine: $hyperfine,
    runs: $runs,
    warmup: $warmup
  }'
)
python3 "$script_dir/capture-environment.py" \
  --output "$result_dir/environment.json" \
  --repository "$repo_dir" \
  --sqrail "$sqrail_bin" \
  --duckdb "$duckdb_bin" \
  --cache-state "$cache_state" \
  --cache-control-evidence "$cache_evidence" \
  --parameters-json "$parameters"

printf 'startup benchmark results written to %s\n' "$result_dir"
