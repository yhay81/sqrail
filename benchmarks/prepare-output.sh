#!/usr/bin/env bash
set -euo pipefail

output=${1:?output path is required}
case $output in
  */outputs/*.parquet)
    rm -f -- "$output"
    ;;
  *)
    echo "refusing to remove unexpected benchmark path: $output" >&2
    exit 5
    ;;
esac
