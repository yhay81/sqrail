#!/usr/bin/env bash
set -euo pipefail

sqrail_bin=${1:?sqrail binary path is required}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

expect_error() {
  expected_status=$1
  expected_code=$2
  error_file=$3
  shift 3

  set +e
  "$@" >"$test_dir/unexpected-stdout" 2>"$error_file"
  actual_status=$?
  set -e

  if [ "$actual_status" -ne "$expected_status" ]; then
    echo "expected exit $expected_status, got $actual_status: $*" >&2
    cat "$error_file" >&2
    exit 1
  fi
  test ! -s "$test_dir/unexpected-stdout"
  grep -q "\"code\":\"$expected_code\"" "$error_file"
}

printf 'drug_id,name,amount\n1,alpha,10\n2,beta,20\n1,alpha,15\n' > "$test_dir/sales.csv"
printf 'drug_id\tstatus\n1\tactive\n2\tinactive\n' > "$test_dir/status.tsv"
printf '{"drug_id":1,"phase":2}\n{"drug_id":2,"phase":3}\n' > "$test_dir/trials.jsonl"
cp "$test_dir/sales.csv" "$test_dir/odd ' sales.csv"
gzip -c "$test_dir/sales.csv" > "$test_dir/sales.csv.gz"

schema=$("$sqrail_bin" schema "$test_dir/sales.csv")
printf '%s\n' "$schema" | grep -q '"name":"drug_id"'
printf '%s\n' "$schema" | grep -q '"name":"amount"'
printf '%s\n' "$schema" | python3 -c 'import json,sys; json.load(sys.stdin)'
python3 - "$sqrail_bin" <<'PY'
import json
import os
import subprocess
import sys

result = subprocess.run(
    [os.fsencode(sys.argv[1]), b"\xff"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    check=False,
)
assert result.returncode == 2
payload = json.loads(result.stderr.decode("utf-8"))
assert payload["code"] == "UNKNOWN_COMMAND"
assert "\ufffd" in payload["message"]
PY

multi_schema=$("$sqrail_bin" schema "$test_dir/sales.csv.gz" "$test_dir/odd ' sales.csv")
test "$(printf '%s\n' "$multi_schema" | wc -l | tr -d ' ')" = "2"

result=$("$sqrail_bin" run --memory 256MB --threads 1 \
  -t sales="$test_dir/sales.csv" \
  'SELECT drug_id, sum(amount) AS total FROM sales GROUP BY drug_id ORDER BY drug_id')

expected='{"drug_id":1,"total":25}
{"drug_id":2,"total":20}'
test "$result" = "$expected"

stdin_result=$(printf 'SELECT count(*) AS rows FROM sales;\n' |
  "$sqrail_bin" run -t sales="$test_dir/sales.csv.gz" -)
test "$stdin_result" = '{"rows":3}'

values_result=$("$sqrail_bin" run \
  'WITH input(value) AS (VALUES (1), (2)) SELECT sum(value) AS total FROM input')
test "$values_result" = '{"total":3}'

comment_result=$("$sqrail_bin" run 'SELECT 7 AS value; -- trailing comment')
test "$comment_result" = '{"value":7}'

joined=$("$sqrail_bin" run \
  -t status="$test_dir/status.tsv" \
  -t trials="$test_dir/trials.jsonl" \
  'SELECT count(*) AS matched FROM status JOIN trials USING (drug_id) WHERE phase >= 2')
test "$joined" = '{"matched":2}'

"$sqrail_bin" run \
  -t sales="$test_dir/sales.csv" \
  -o "$test_dir/result.parquet" \
  'SELECT * FROM sales WHERE amount >= 15 ORDER BY amount'

parquet_schema=$("$sqrail_bin" schema "$test_dir/result.parquet")
printf '%s\n' "$parquet_schema" | grep -q '"name":"amount"'

gzip_output="$test_dir/result.csv.gz"
"$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$gzip_output" \
  'SELECT * FROM sales ORDER BY drug_id, amount'
gzip -t "$gzip_output"
test "$(gzip -dc "$gzip_output" | wc -l | tr -d ' ')" = "4"
"$sqrail_bin" schema "$gzip_output" | grep -q '"name":"amount"'

zstd_output="$test_dir/result.csv.zst"
"$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$zstd_output" \
  'SELECT * FROM sales'
test "$("$sqrail_bin" run -t compressed="$zstd_output" \
  'SELECT count(*) AS rows FROM compressed')" = '{"rows":3}'

json_output="$test_dir/result.jsonl.gz"
"$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$json_output" \
  'SELECT drug_id, amount FROM sales'
test "$("$sqrail_bin" run -t compressed="$json_output" \
  'SELECT sum(amount) AS total FROM compressed')" = '{"total":45}'

strict_json=$("$sqrail_bin" run \
  "SELECT 'NaN' AS text, CAST('NaN' AS DOUBLE) AS special, [CAST('Infinity' AS DOUBLE)] AS nested")
printf '%s\n' "$strict_json" | python3 -c \
  'import json,sys; value=json.load(sys.stdin, parse_constant=lambda token: (_ for _ in ()).throw(ValueError(token))); assert value == {"text":"NaN","special":None,"nested":[None]}'

strict_json_file="$test_dir/strict.json.zst"
"$sqrail_bin" run -o "$strict_json_file" \
  "SELECT CAST(value AS DOUBLE) AS value FROM (VALUES ('NaN'), ('Infinity'), ('-Infinity')) input(value)"
"$sqrail_bin" run -t strict="$strict_json_file" \
  'SELECT count(*) AS rows FROM strict WHERE value IS NULL' | grep -q '"rows":3'

mkdir -p "$test_dir/dataset/year=2025" "$test_dir/dataset/year=2026"
"$sqrail_bin" run -o "$test_dir/dataset/year=2025/part-0.parquet" \
  'SELECT 1 AS id, 10 AS amount'
"$sqrail_bin" run -o "$test_dir/dataset/year=2026/part-0.parquet" \
  'SELECT 2 AS id, 20 AS amount'
dataset_result=$("$sqrail_bin" run -t data="$test_dir/dataset" \
  'SELECT year, sum(amount) AS total FROM data GROUP BY year ORDER BY year')
test "$dataset_result" = '{"year":2025,"total":10}
{"year":2026,"total":20}'
test "$("$sqrail_bin" run -t data="$test_dir/dataset/**/*.parquet" \
  'SELECT count(*) AS rows FROM data')" = '{"rows":2}'
"$sqrail_bin" schema "$test_dir/dataset" | grep -q '"files":2'

plan=$("$sqrail_bin" check -t sales="$test_dir/sales.csv" \
  'SELECT sum(amount) AS total FROM sales')
printf '%s\n' "$plan" | python3 -c \
  'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["plan"][0]["name"] == "UNGROUPED_AGGREGATE"'

spill_root="$test_dir/spill/nested"
mkdir -p "$spill_root"
printf 'private\n' > "$spill_root/not-spill-data.txt"
spill_result=$("$sqrail_bin" run --spill "$spill_root" --max-spill 64MiB \
  'SELECT 1 AS value')
test "$spill_result" = '{"value":1}'
expect_error 4 QUERY_FAILED "$test_dir/spill-read.json" \
  "$sqrail_bin" run --spill "$spill_root" \
  "SELECT content FROM read_text('$spill_root/not-spill-data.txt')"
test "$(find "$spill_root" -maxdepth 1 -name '.sqrail-spill-*' | wc -l | tr -d ' ')" = "0"

expect_error 5 OUTPUT_EXISTS "$test_dir/error.json" \
  "$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$test_dir/result.parquet" \
  'SELECT * FROM sales'

expect_error 4 QUERY_FAILED "$test_dir/query-error.json" \
  "$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$test_dir/failed.csv" \
  'SELECT missing_column FROM sales'
test ! -e "$test_dir/failed.csv"
test "$(find "$test_dir" -name 'failed.csv.sqrail-tmp-*' | wc -l | tr -d ' ')" = "0"

expect_error 4 MULTIPLE_STATEMENTS "$test_dir/multiple.json" \
  "$sqrail_bin" run 'SELECT 1; SELECT 2'
expect_error 4 READ_ONLY_QUERY "$test_dir/read-only.json" \
  "$sqrail_bin" run 'CREATE TABLE forbidden(i INTEGER)'
expect_error 2 DUPLICATE_TABLE "$test_dir/duplicate-table.json" \
  "$sqrail_bin" run -t Sales="$test_dir/sales.csv" -t sales="$test_dir/sales.csv" \
  'SELECT * FROM sales'
expect_error 2 DUPLICATE_OPTION "$test_dir/duplicate-option.json" \
  "$sqrail_bin" run --threads 1 --threads 2 'SELECT 1'
expect_error 2 INVALID_THREADS "$test_dir/threads.json" \
  "$sqrail_bin" run --threads 999999999999999999999999 'SELECT 1'
expect_error 2 INVALID_MEMORY "$test_dir/memory.json" \
  "$sqrail_bin" run --memory 0MB 'SELECT 1'
expect_error 2 MAX_SPILL_REQUIRES_SPILL "$test_dir/max-spill.json" \
  "$sqrail_bin" run --max-spill 64MiB 'SELECT 1'
expect_error 2 INVALID_MAX_SPILL "$test_dir/max-spill-value.json" \
  "$sqrail_bin" run --spill "$test_dir/spill" --max-spill invalid 'SELECT 1'
expect_error 2 INVALID_TIMEOUT "$test_dir/timeout-value.json" \
  "$sqrail_bin" run --timeout 0ms 'SELECT 1'
expect_error 2 CHECK_SPILL "$test_dir/check-spill.json" \
  "$sqrail_bin" check --spill "$test_dir/check-spill" 'SELECT 1'
expect_error 4 QUERY_TIMEOUT "$test_dir/timeout.json" \
  "$sqrail_bin" run --timeout 1ms \
  'SELECT sum(left_side.i * right_side.i) FROM range(1000000) left_side(i), range(1000000) right_side(i)'
expect_error 4 QUERY_FAILED "$test_dir/external-access.json" \
  "$sqrail_bin" run "SELECT content FROM read_text('/etc/hosts')"

mkdir "$test_dir/empty-dataset"
expect_error 3 EMPTY_DATASET "$test_dir/empty-dataset.json" \
  "$sqrail_bin" schema "$test_dir/empty-dataset"

printf 'not really bzip2\n' > "$test_dir/data.csv.bz2"
expect_error 3 UNSUPPORTED_COMPRESSION "$test_dir/compression.json" \
  "$sqrail_bin" schema "$test_dir/data.csv.bz2"

gzip -c "$test_dir/result.parquet" > "$test_dir/result.parquet.gz"
expect_error 3 UNSUPPORTED_COMPRESSION "$test_dir/parquet-compression.json" \
  "$sqrail_bin" schema "$test_dir/result.parquet.gz"

# Two processes targeting the same absent path must never both succeed or
# replace one another. The atomic hard-link commit makes exactly one winner.
set +e
"$sqrail_bin" run -o "$test_dir/race.parquet" \
  'SELECT i FROM range(1000000) AS rows(i)' 2>"$test_dir/race-1.json" &
race_pid_1=$!
"$sqrail_bin" run -o "$test_dir/race.parquet" \
  'SELECT i FROM range(1000000) AS rows(i)' 2>"$test_dir/race-2.json" &
race_pid_2=$!
wait "$race_pid_1"
race_status_1=$?
wait "$race_pid_2"
race_status_2=$?
set -e

if ! { [ "$race_status_1" -eq 0 ] && [ "$race_status_2" -eq 5 ]; } &&
   ! { [ "$race_status_1" -eq 5 ] && [ "$race_status_2" -eq 0 ]; }; then
  echo "expected one race winner and one exit 5; got $race_status_1/$race_status_2" >&2
  exit 1
fi
test -f "$test_dir/race.parquet"
test "$(find "$test_dir" -name 'race.parquet.sqrail-tmp-*' | wc -l | tr -d ' ')" = "0"
test "$("$sqrail_bin" run -t race="$test_dir/race.parquet" \
  'SELECT count(*) AS rows FROM race')" = '{"rows":1000000}'

agent_help=$("$sqrail_bin" --agent-help)
printf '%s\n' "$agent_help" | grep -q 'SQL is one SELECT'
test "$(printf '%s\n' "$agent_help" | wc -w | tr -d ' ')" -le 130
"$sqrail_bin" --version | grep -q '^sqrail 0.2.1 (DuckDB v1.5.5)$'
