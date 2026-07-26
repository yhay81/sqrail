#!/usr/bin/env bash
set -euo pipefail

engine=${1:?engine is required}
stats_file=${2:?stats file is required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

case $(uname -s) in
  Darwin)
    /usr/bin/time -l "$script_dir/run-engine.sh" "$engine" 2>"$stats_file"
    awk '/maximum resident set size/ { print $1; found = 1; exit }
         END { if (!found) exit 1 }' "$stats_file"
    ;;
  Linux)
    /usr/bin/time -v "$script_dir/run-engine.sh" "$engine" 2>"$stats_file"
    rss_kib=$(
      awk -F: '/Maximum resident set size/ {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
        print $2
        found = 1
        exit
      }
      END { if (!found) exit 1 }' "$stats_file"
    )
    printf '%s\n' "$((rss_kib * 1024))"
    ;;
  *)
    echo "RSS measurement is not supported on $(uname -s)" >&2
    exit 2
    ;;
esac
