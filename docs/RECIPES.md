# Recipes

<!-- tests/smoke.sh runs every sh block in order and requires each line of
every text block to appear in the output. -->

Copy-pasteable sqrail commands for common file tasks. Each recipe states its
inputs, the command, what it writes, and the limit that matters most. The
recipes use a POSIX shell and run in order against the sample files from the
first section, so the whole page can be executed in an empty directory.

The [CLI contract](CONTRACT.md) is normative for every option and diagnostic
used here.

## Sample files

sqrail can write its own fixtures. These commands create a CSV of sales, a
Parquet table of drug names, a JSONL file of trial records, and a
Hive-partitioned Parquet directory of events:

```sh
sqrail run -o sales.csv \
  "SELECT * FROM (VALUES (1, 'alpha', 10), (2, 'beta', 20), (1, 'alpha', 15)) AS sales(drug_id, name, amount)"
sqrail run -o drugs.parquet \
  "SELECT * FROM (VALUES (1, 'Alphazol'), (2, 'Betamab')) AS drugs(drug_id, drug_name)"
sqrail run -o trials.jsonl \
  "SELECT * FROM (VALUES (1, 2, 'active'), (2, 3, 'active'), (3, NULL, 'withdrawn')) AS trials(drug_id, phase, status)"
mkdir -p events/year=2025 events/year=2026
sqrail run -o events/year=2025/part-0.parquet \
  'SELECT i AS event_id, i % 2 + 1 AS drug_id FROM range(4) AS t(i)'
sqrail run -o events/year=2026/part-0.parquet \
  'SELECT i AS event_id, i % 2 + 1 AS drug_id FROM range(4, 10) AS t(i)'
```

## Inspect a schema

- **Inputs:** any supported file, Parquet directory, or glob.
- **Output:** one JSON object per argument on stdout, with the resolved file
  count and each column's name, type, and nullability.
- **Limit that matters:** `--max-input-files` stops directory and glob
  expansion as soon as the cumulative count exceeds the cap.

```sh
sqrail schema --max-input-files 100 sales.csv events
```

The `events` object reports two files and includes the Hive partition column:

```text
{"name":"year","type":"BIGINT","nullable":true}
```

Skip this step when the names and types are already known.

## Convert CSV to Parquet

- **Inputs:** one CSV file bound as `sales`.
- **Output:** nothing on stdout. `-o` selects the format from the extension,
  writes a private temporary file, and commits it atomically. An existing
  destination is never overwritten; sqrail fails with `OUTPUT_EXISTS`
  (exit 5) instead.
- **Limit that matters:** `--memory` bounds the sort. Add `--spill DIR` and
  `--max-spill SIZE` for inputs that do not fit, and `--max-output-bytes` to
  cap the file.

```sh
sqrail run --memory 512MB -t sales=sales.csv -o sales.parquet \
  'SELECT drug_id, name, amount FROM sales ORDER BY drug_id, amount'
```

## Join CSV and Parquet

- **Inputs:** `sales.csv` bound as `sales` and `drugs.parquet` bound as
  `drugs`.
- **Output:** `check` prints the result columns, the bound inputs, and the
  physical plan without executing the query. `run` streams JSONL rows.
- **Limit that matters:** `--max-rows` fails with `RESULT_LIMIT` (exit 4)
  instead of returning a silently truncated result. Streaming stdout may
  already contain a valid prefix of at most that many rows.

Keep the query in a file when quoting would otherwise be awkward, then check it
before running it:

```sh
cat > top-drugs.sql <<'SQL'
SELECT d.drug_name, sum(s.amount) AS total
FROM sales AS s
JOIN drugs AS d USING (drug_id)
GROUP BY d.drug_name
ORDER BY total DESC
SQL
sqrail check -t sales=sales.csv -t drugs=drugs.parquet - < top-drugs.sql
sqrail run --max-rows 100 -t sales=sales.csv -t drugs=drugs.parquet - < top-drugs.sql
```

```text
{"drug_name":"Alphazol","total":25}
{"drug_name":"Betamab","total":20}
```

## Filter partitioned Parquet

- **Inputs:** the `events` directory bound as one table. Every Parquet file
  beneath it is read, and `key=value` directory names such as `year=2026`
  become columns.
- **Output:** JSONL rows on stdout.
- **Limit that matters:** `--max-input-files` fails with `INPUT_LIMIT`
  (exit 3) before a directory that is larger than expected is read.

```sh
sqrail run --max-input-files 100 -t events=events \
  'SELECT drug_id, count(*) AS events FROM events WHERE year = 2026 GROUP BY drug_id ORDER BY drug_id'
```

```text
{"drug_id":1,"events":3}
{"drug_id":2,"events":3}
```

## Validate and aggregate JSONL

- **Inputs:** `trials.jsonl` bound as `trials`.
- **Output:** one JSONL row on stdout. `--stats` adds one versioned success
  object on stderr with the row count, bytes written, elapsed time, and input
  file count.
- **Limit that matters:** `--timeout` puts one deadline on input discovery,
  schema inference, planning, and execution. `--memory` and `--max-rows` bound
  the rest of the work.

A malformed line is never skipped: the command fails with one JSON diagnostic on
stderr and exit 4, so a successful aggregate covers every record. The query
then reports the records that are present but incomplete:

```sh
sqrail run --stats --timeout 30s --memory 256MB --max-rows 10 \
  -t trials=trials.jsonl - <<'SQL'
SELECT
  count(*) AS records,
  count(*) FILTER (WHERE phase IS NULL) AS missing_phase,
  count(DISTINCT drug_id) AS drugs
FROM trials
SQL
```

```text
{"records":3,"missing_phase":1,"drugs":3}
```

When several JSONL files must share one exact schema, add `--strict-schema`;
by default files are combined by column name.
