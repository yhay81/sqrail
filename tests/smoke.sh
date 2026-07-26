#!/usr/bin/env bash
set -euo pipefail

sqrail_bin=${1:?sqrail binary path is required}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

printf 'drug_id,name,amount\n1,alpha,10\n2,beta,20\n1,alpha,15\n' > "$test_dir/sales.csv"
printf 'drug_id\tstatus\n1\tactive\n2\tinactive\n' > "$test_dir/status.tsv"
printf '{"drug_id":1,"phase":2}\n{"drug_id":2,"phase":3}\n' > "$test_dir/trials.jsonl"

schema=$("$sqrail_bin" schema "$test_dir/sales.csv")
printf '%s\n' "$schema" | grep -q '"name":"drug_id"'
printf '%s\n' "$schema" | grep -q '"name":"amount"'

result=$("$sqrail_bin" run --memory 256MB --threads 1 \
  -t sales="$test_dir/sales.csv" \
  'SELECT drug_id, sum(amount) AS total FROM sales GROUP BY drug_id ORDER BY drug_id')

expected='{"drug_id":1,"total":25}
{"drug_id":2,"total":20}'
test "$result" = "$expected"

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

if "$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$test_dir/result.parquet" \
  'SELECT * FROM sales' 2>"$test_dir/error.json"; then
  echo "expected existing output to be rejected" >&2
  exit 1
fi
grep -q '"code":"OUTPUT_EXISTS"' "$test_dir/error.json"

if "$sqrail_bin" run -t sales="$test_dir/sales.csv" -o "$test_dir/failed.csv" \
  'SELECT missing_column FROM sales' 2>"$test_dir/query-error.json"; then
  echo "expected invalid SQL to fail" >&2
  exit 1
fi
test ! -e "$test_dir/failed.csv"
test "$(find "$test_dir" -name 'failed.csv.sqrail-tmp-*' | wc -l | tr -d ' ')" = "0"
grep -q '"code":"QUERY_FAILED"' "$test_dir/query-error.json"

"$sqrail_bin" --agent-help | grep -q 'SQL is DuckDB SQL'
