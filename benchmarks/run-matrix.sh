#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
data_dir=${1:-benchmark-data}
result_root=${2:-benchmark-results-matrix}
memories=${BENCH_MEMORIES:-"512MB 1GB 4GB"}

if [ -e "$result_root" ]; then
  echo "refusing to replace existing result root: $result_root" >&2
  exit 5
fi

read -r -a memory_values <<< "$memories"
if [ "${#memory_values[@]}" -eq 0 ]; then
  echo "BENCH_MEMORIES must contain at least one memory budget" >&2
  exit 2
fi

mkdir -p "$result_root"
combined_summary="$result_root/summary.tsv"
printf 'memory\tcase\tengine\tmean_s\tstddev_s\tmin_s\tmax_s\tpeak_rss_bytes\trows\tchecksum\n' \
  > "$combined_summary"

for memory in "${memory_values[@]}"; do
  case $memory in
    *[!A-Za-z0-9.]*|'')
      echo "invalid memory budget in BENCH_MEMORIES: $memory" >&2
      exit 2
      ;;
  esac
  result_dir="$result_root/$memory"
  BENCH_MEMORY="$memory" "$script_dir/run.sh" "$data_dir" "$result_dir"
  awk -v memory="$memory" 'NR > 1 { print memory "\t" $0 }' \
    "$result_dir/summary.tsv" >> "$combined_summary"
done

printf 'benchmark matrix results written to %s\n' "$result_root"
