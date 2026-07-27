#!/usr/bin/env bash
set -euo pipefail

output=${1:?output path is required}
spill_directory=${2:-}
keep_output=${3:-0}
if [ "$keep_output" != 0 ] && [ "$keep_output" != 1 ]; then
  echo "keep-output flag must be 0 or 1" >&2
  exit 2
fi
case $output in
  */outputs/*.parquet|*/outputs/*.jsonl)
    if [ "$keep_output" -eq 0 ]; then
      rm -f -- "$output"
    fi
    ;;
  *)
    echo "refusing to remove unexpected benchmark path: $output" >&2
    exit 5
    ;;
esac

if [ -n "$spill_directory" ]; then
  marker="$spill_directory/.sqrail-benchmark-spill"
  if [ -e "$spill_directory" ] || [ -L "$spill_directory" ]; then
    if [ ! -d "$spill_directory" ] || [ -L "$spill_directory" ] || [ ! -f "$marker" ]; then
      echo "refusing to clean unmarked benchmark spill path: $spill_directory" >&2
      exit 5
    fi
    find "$spill_directory" -mindepth 1 -maxdepth 1 \
      ! -name .sqrail-benchmark-spill -exec rm -rf -- {} +
  else
    mkdir -p -- "$spill_directory"
    : > "$marker"
  fi
fi
