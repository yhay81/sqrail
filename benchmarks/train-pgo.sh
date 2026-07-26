#!/usr/bin/env bash
set -euo pipefail

sqrail_bin=${1:-out/build/pgo-generate/sqrail}
profile_file=${2:-out/build/pgo-generate/pgo/sqrail.profdata}
training_rows=${SQRAIL_PGO_TRAIN_ROWS:-2000000}
raw_directory=$(dirname "$sqrail_bin")/pgo/raw
training_directory=$(mktemp -d)
trap 'rm -rf "$training_directory"' EXIT

if [ ! -x "$sqrail_bin" ]; then
  echo "instrumented sqrail binary is missing: $sqrail_bin" >&2
  exit 2
fi
if ! command -v llvm-profdata >/dev/null 2>&1; then
  echo "llvm-profdata is required and must match the instrumenting Clang" >&2
  exit 2
fi
case $training_rows in
  ''|*[!0-9]*|0)
    echo "SQRAIL_PGO_TRAIN_ROWS must be a positive integer" >&2
    exit 2
    ;;
esac

mkdir -p "$raw_directory" "$(dirname "$profile_file")" "$training_directory/spill"
rm -f -- "$raw_directory"/*.profraw

"$sqrail_bin" run --threads 2 \
  "SELECT sum(i) AS total FROM range(${training_rows}) rows(i)" >/dev/null

"$sqrail_bin" run --threads 2 -o "$training_directory/fact.parquet" \
  "SELECT i AS id, i % 10000 AS group_id, sin(i::DOUBLE) AS score
   FROM range(${training_rows}) rows(i)"

"$sqrail_bin" run --memory 256MiB --threads 2 \
  --spill "$training_directory/spill" --max-spill 1GiB \
  -t fact="$training_directory/fact.parquet" \
  "SELECT group_id, count(*) AS rows, avg(score) AS mean_score
   FROM fact WHERE id % 7 = 0 GROUP BY group_id ORDER BY group_id" >/dev/null

"$sqrail_bin" run --threads 2 -o "$training_directory/result.jsonl.zst" \
  -t fact="$training_directory/fact.parquet" \
  "SELECT id, score FROM fact WHERE id % 20 = 0 ORDER BY id"

"$sqrail_bin" schema "$training_directory/fact.parquet" >/dev/null
"$sqrail_bin" check -t fact="$training_directory/fact.parquet" \
  "SELECT group_id, max(score) FROM fact GROUP BY group_id" >/dev/null

if ! compgen -G "$raw_directory/*.profraw" >/dev/null; then
  echo "training produced no raw profiles in $raw_directory" >&2
  exit 1
fi

llvm-profdata merge -sparse "$raw_directory"/*.profraw -o "$profile_file"
llvm-profdata show --counts --all-functions "$profile_file" >/dev/null
printf 'PGO profile: %s\n' "$profile_file"
