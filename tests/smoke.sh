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

bounded_result=$("$sqrail_bin" run --max-rows 2 \
  'SELECT i FROM range(2) AS rows(i) ORDER BY i')
test "$bounded_result" = '{"i":0}
{"i":1}'

expect_error 4 RESULT_LIMIT "$test_dir/result-limit.json" \
  "$sqrail_bin" run --max-rows 2 \
  'SELECT i FROM range(3) AS rows(i) ORDER BY i'

stats_result=$("$sqrail_bin" run --stats \
  'SELECT i FROM range(3) AS rows(i) ORDER BY i' 2>"$test_dir/stats.json")
test "$(printf '%s\n' "$stats_result" | wc -l | tr -d ' ')" = "3"
python3 - "$test_dir/stats.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle)
assert value["ok"] is True
assert value["schema_version"] == 1
assert value["sqrail_version"] == "0.3.0"
assert value["command"] == "run"
assert value["rows"] == 3
assert value["bytes"] > 0
assert value["elapsed_ms"] >= 0
assert value["input_files"] == 0
assert value["destination"] == "stdout"
PY

comment_result=$("$sqrail_bin" run 'SELECT 7 AS value; -- trailing comment')
test "$comment_result" = '{"value":7}'

joined=$("$sqrail_bin" run \
  -t status="$test_dir/status.tsv" \
  -t trials="$test_dir/trials.jsonl" \
  'SELECT count(*) AS matched FROM status JOIN trials USING (drug_id) WHERE phase >= 2')
test "$joined" = '{"matched":2}'

"$sqrail_bin" run --stats \
  -t sales="$test_dir/sales.csv" \
  -o "$test_dir/result.parquet" \
  'SELECT * FROM sales WHERE amount >= 15 ORDER BY amount' \
  2>"$test_dir/file-stats.json"
python3 - "$test_dir/file-stats.json" "$test_dir/result.parquet" <<'PY'
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle)
assert value["rows"] == 2
assert value["bytes"] == os.path.getsize(sys.argv[2])
assert value["input_files"] == 1
assert value["destination"] == "file"
PY

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

python3 - "$test_dir/result.parquet" "$gzip_output" "$json_output" <<'PY'
import os
import stat
import sys

for path in sys.argv[1:]:
    assert stat.S_IMODE(os.stat(path).st_mode) & 0o077 == 0, path
PY

expect_error 4 RESULT_LIMIT "$test_dir/file-result-limit.json" \
  "$sqrail_bin" run --max-rows 2 -o "$test_dir/limited.parquet" \
  'SELECT i FROM range(3) AS rows(i)'
test ! -e "$test_dir/limited.parquet"
test "$(find "$test_dir" -name 'limited.parquet.sqrail-tmp-*' | wc -l | tr -d ' ')" = "0"

expect_error 5 OUTPUT_LIMIT "$test_dir/stdout-byte-limit.json" \
  "$sqrail_bin" run --max-output-bytes 10B \
  'SELECT 123456789 AS value'
expect_error 5 OUTPUT_LIMIT "$test_dir/file-byte-limit.json" \
  "$sqrail_bin" run --max-output-bytes 1KiB -o "$test_dir/byte-limited.parquet" \
  'SELECT i, md5(i::VARCHAR) AS payload FROM range(10000) AS rows(i)'
test ! -e "$test_dir/byte-limited.parquet"
test "$(find "$test_dir" -name 'byte-limited.parquet.sqrail-tmp-*' | wc -l | tr -d ' ')" = "0"

set +e
printf 'SELECT 123456789' |
  "$sqrail_bin" run --max-sql-bytes 5B - \
    >"$test_dir/sql-limit-stdout" 2>"$test_dir/sql-limit.json"
sql_limit_status=$?
set -e
test "$sql_limit_status" -eq 2
test ! -s "$test_dir/sql-limit-stdout"
grep -q '"code":"SQL_LIMIT"' "$test_dir/sql-limit.json"

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
strict_dataset_result=$("$sqrail_bin" run --strict-schema -t data="$test_dir/dataset" \
  'SELECT year, sum(amount) AS total FROM data GROUP BY year ORDER BY year')
test "$strict_dataset_result" = "$dataset_result"
"$sqrail_bin" schema --strict-schema "$test_dir/dataset" | grep -q '"files":2'

mkdir "$test_dir/evolved"
"$sqrail_bin" run -o "$test_dir/evolved/a.parquet" \
  'SELECT 1 AS id, 10 AS old_value'
"$sqrail_bin" run -o "$test_dir/evolved/b.parquet" \
  'SELECT 2 AS id, 20 AS new_value'
evolved_result=$("$sqrail_bin" run -t data="$test_dir/evolved" \
  'SELECT * FROM data ORDER BY id')
test "$evolved_result" = '{"id":1,"old_value":10,"new_value":null}
{"id":2,"old_value":null,"new_value":20}'
expect_error 3 SCHEMA_MISMATCH "$test_dir/strict-schema.json" \
  "$sqrail_bin" run --strict-schema -t data="$test_dir/evolved" \
  'SELECT * FROM data'
expect_error 3 INPUT_LIMIT "$test_dir/input-limit.json" \
  "$sqrail_bin" schema --max-input-files 1 "$test_dir/evolved"
evolved_schema=$("$sqrail_bin" schema --memory 64MiB --threads 1 --timeout 5s \
  "$test_dir/evolved")
printf '%s\n' "$evolved_schema" | python3 -c \
  'import json,sys; value=json.load(sys.stdin); assert value["files"] == 2; assert [column["name"] for column in value["columns"]] == ["id", "old_value", "new_value"]'
expect_error 3 SCHEMA_MISMATCH "$test_dir/strict-schema-inference.json" \
  "$sqrail_bin" schema --strict-schema "$test_dir/evolved"
evolved_plan=$("$sqrail_bin" check -t data="$test_dir/evolved" \
  'SELECT * FROM data')
printf '%s\n' "$evolved_plan" | python3 -c \
  'import json,sys; value=json.load(sys.stdin); assert [column["name"] for column in value["columns"]] == ["id", "old_value", "new_value"]; assert value["inputs"] == [{"table":"data","files":2}]'
expect_error 3 SCHEMA_MISMATCH "$test_dir/strict-schema-check.json" \
  "$sqrail_bin" check --strict-schema -t data="$test_dir/evolved" \
  'SELECT * FROM data'

mkdir "$test_dir/evolved-csv"
printf 'id,old_value\n1,10\n' > "$test_dir/evolved-csv/a.csv"
printf 'id,new_value\n2,20\n' > "$test_dir/evolved-csv/b.csv"
evolved_csv_result=$("$sqrail_bin" run -t data="$test_dir/evolved-csv/*.csv" \
  'SELECT * FROM data ORDER BY id')
test "$evolved_csv_result" = "$evolved_result"
expect_error 3 SCHEMA_MISMATCH "$test_dir/strict-csv-schema.json" \
  "$sqrail_bin" run --strict-schema -t data="$test_dir/evolved-csv/*.csv" \
  'SELECT * FROM data'

mkdir "$test_dir/evolved-json"
printf '{"id":1,"old_value":10}\n' > "$test_dir/evolved-json/a.jsonl"
printf '{"id":2,"new_value":20}\n' > "$test_dir/evolved-json/b.jsonl"
evolved_json_result=$("$sqrail_bin" run -t data="$test_dir/evolved-json/*.jsonl" \
  'SELECT * FROM data ORDER BY id')
test "$evolved_json_result" = "$evolved_result"
expect_error 3 SCHEMA_MISMATCH "$test_dir/strict-json-schema.json" \
  "$sqrail_bin" run --strict-schema -t data="$test_dir/evolved-json/*.jsonl" \
  'SELECT * FROM data'

plan=$("$sqrail_bin" check -t sales="$test_dir/sales.csv" \
  'SELECT sum(amount) AS total FROM sales')
printf '%s\n' "$plan" | python3 -c \
  'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["columns"] == [{"name":"total","type":"HUGEINT","nullable":True}]; assert value["inputs"] == [{"table":"sales","files":1}]; assert value["plan"][0]["name"] == "UNGROUPED_AGGREGATE"'
bounded_plan=$("$sqrail_bin" check --max-rows 2 \
  'SELECT i FROM range(1000000000) AS rows(i)')
printf '%s\n' "$bounded_plan" | python3 -c \
  'import json,sys; value=json.load(sys.stdin); assert value["columns"] == [{"name":"i","type":"BIGINT","nullable":True}]; assert value["plan"][0]["name"] == "STREAMING_LIMIT"'

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
expect_error 2 INVALID_MAX_ROWS "$test_dir/max-rows-value.json" \
  "$sqrail_bin" run --max-rows 0 'SELECT 1'
expect_error 2 CHECK_STATS "$test_dir/check-stats.json" \
  "$sqrail_bin" check --stats 'SELECT 1'
expect_error 2 CHECK_SPILL "$test_dir/check-spill.json" \
  "$sqrail_bin" check --spill "$test_dir/check-spill" 'SELECT 1'
expect_error 2 CHECK_OUTPUT_LIMIT "$test_dir/check-output-limit.json" \
  "$sqrail_bin" check --max-output-bytes 1MiB 'SELECT 1'
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

set +e
"$sqrail_bin" run --spill "$test_dir/pipe-spill" --max-spill 64MiB \
  'SELECT i FROM range(1000000) AS rows(i)' 2>"$test_dir/pipe-error.json" |
  head -n 1 >"$test_dir/first-row.jsonl"
pipe_status=${PIPESTATUS[0]}
set -e
test "$pipe_status" -eq 5
test "$(cat "$test_dir/first-row.jsonl")" = '{"i":0}'
grep -q '"code":"STDOUT_WRITE"' "$test_dir/pipe-error.json"
test "$(find "$test_dir/pipe-spill" -name '.sqrail-spill-*' | wc -l | tr -d ' ')" = "0"

for signal_name in TERM INT; do
  case $signal_name in
    TERM)
      signal_label=term
      ;;
    INT)
      signal_label=int
      ;;
  esac
  signal_output="$test_dir/signal-$signal_label.parquet"
  signal_spill="$test_dir/signal-$signal_label-spill"
  "$sqrail_bin" run \
    --memory 64MiB \
    --spill "$signal_spill" \
    --max-spill 1GiB \
    -o "$signal_output" \
    'SELECT i, md5(i::VARCHAR) AS payload
     FROM range(1000000000) AS rows(i)
     ORDER BY payload' \
    >"$test_dir/signal-$signal_label-stdout" \
    2>"$test_dir/signal-$signal_label-error.json" &
  signal_pid=$!
  signal_ready=0
  for _ in {1..500}; do
    if compgen -G "$signal_output.sqrail-tmp-*" >/dev/null; then
      signal_ready=1
      break
    fi
    sleep 0.01
  done
  if [ "$signal_ready" -ne 1 ]; then
    echo "$signal_name test query did not create its private output in time" >&2
    kill -KILL "$signal_pid" 2>/dev/null || true
    wait "$signal_pid" 2>/dev/null || true
    exit 1
  fi
  kill "-$signal_name" "$signal_pid"
  set +e
  wait "$signal_pid"
  signal_status=$?
  set -e
  test "$signal_status" -eq 4
  grep -q '"code":"QUERY_INTERRUPTED"' "$test_dir/signal-$signal_label-error.json"
  test ! -e "$signal_output"
  test "$(find "$test_dir" -name "signal-$signal_label.parquet.sqrail-tmp-*" | wc -l | tr -d ' ')" = "0"
  test "$(find "$signal_spill" -name '.sqrail-spill-*' | wc -l | tr -d ' ')" = "0"
done

agent_help=$("$sqrail_bin" --agent-help)
printf '%s\n' "$agent_help" | grep -q 'Names/types result: schema once'
printf '%s\n' "$agent_help" | grep -q 'run once, and stop after success'
printf '%s\n' "$agent_help" | grep -q 'SQL is one SELECT'
printf '%s\n' "$agent_help" | grep -q -- '--max-rows'
printf '%s\n' "$agent_help" | grep -q -- '--strict-schema'
printf '%s\n' "$agent_help" | grep -q -- '--stats'
printf '%s\n' "$agent_help" | grep -q -- '--max-output-bytes'
printf '%s\n' "$agent_help" | grep -q -- '--max-input-files'
printf '%s\n' "$agent_help" | grep -q -- '--max-sql-bytes'
printf '%s\n' "$agent_help" | grep -q 'check emits columns'
test "$(printf '%s\n' "$agent_help" | wc -w | tr -d ' ')" -le 170
"$sqrail_bin" --version | grep -Eq '^sqrail 0\.3\.0 \(DuckDB v[0-9]+\.[0-9]+\.[0-9]+([.-][^)]*)?\)$'
