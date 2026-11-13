#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

SQRAIL = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0]]


def invoke(
    *arguments: str,
    stdin: bytes | None = None,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(SQRAIL), *arguments],
        cwd=cwd,
        input=stdin,
        capture_output=True,
        check=False,
    )


def diagnostic(result: subprocess.CompletedProcess[bytes]) -> dict[str, object]:
    value = json.loads(result.stderr.decode("utf-8"))
    assert value["schema_version"] == 1
    assert value["sqrail_version"] == "0.4.2"
    assert value["ok"] is False
    return value


class CrossPlatformTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.csv = self.root / "sales ünicode.csv"
        self.csv.write_text(
            "drug_id,name,amount\n1,alpha,10\n2,beta,20\n1,alpha,15\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_closed_stdout_reports_failure_before_success_stats(self) -> None:
        for arguments in (
            ("run", "--stats", "SELECT 1 AS value"),
            ("check", "SELECT 1 AS value"),
            ("schema", str(self.csv)),
        ):
            with self.subTest(arguments=arguments):
                read_fd, write_fd = os.pipe()
                os.close(read_fd)
                try:
                    result = subprocess.run(
                        [str(SQRAIL), *arguments], stdout=write_fd, stderr=subprocess.PIPE, check=False
                    )
                finally:
                    os.close(write_fd)
                self.assertEqual(result.returncode, 5, result.stderr)
                self.assertEqual(diagnostic(result)["code"], "STDOUT_WRITE")

    def test_input_limit_counts_canonical_files(self) -> None:
        dataset = self.root / "dataset"
        dataset.mkdir()
        real = dataset / "a.parquet"
        created = invoke("run", "-o", str(real), "SELECT 42 AS value")
        self.assertEqual(created.returncode, 0, created.stderr)
        try:
            (dataset / "alias.parquet").symlink_to(real)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symbolic links unavailable: {error}")

        for source in (str(dataset), str(dataset / "*.parquet")):
            with self.subTest(source=source):
                schema = invoke("schema", "--max-input-files", "1", source)
                self.assertEqual(schema.returncode, 0, schema.stderr)
                self.assertEqual(json.loads(schema.stdout)["files"], 1)
                result = invoke("run", "--max-input-files", "1", "-t", f"data={source}",
                                "SELECT * FROM data")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], [{"value": 42}])

        other = dataset / "other.parquet"
        created = invoke("run", "-o", str(other), "SELECT 43 AS value")
        self.assertEqual(created.returncode, 0, created.stderr)
        for source in (str(dataset), str(dataset / "*.parquet")):
            with self.subTest(source=source):
                limited = invoke("schema", "--max-input-files", "1", source)
                self.assertEqual(limited.returncode, 3)
                self.assertEqual(diagnostic(limited)["code"], "INPUT_LIMIT")

        cumulative = invoke("run", "--max-input-files", "1", "-t", f"a={real}",
                            "-t", f"b={dataset / 'alias.parquet'}", "SELECT * FROM a")
        self.assertEqual(cumulative.returncode, 3)
        self.assertEqual(diagnostic(cumulative)["code"], "INPUT_LIMIT")

        for name, source in (("first", real), ("second", other)):
            nested = dataset / name
            nested.mkdir()
            (nested / "part.parquet").write_bytes(source.read_bytes())
        nested_glob = str(dataset / "*" / "*.parquet")
        limited = invoke("schema", "--max-input-files", "1", nested_glob)
        self.assertEqual(limited.returncode, 3)
        self.assertEqual(diagnostic(limited)["code"], "INPUT_LIMIT")
        allowed = invoke("schema", "--max-input-files", "2", nested_glob)
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertEqual(json.loads(allowed.stdout)["files"], 2)

    def test_contract_formats_limits_and_atomic_output(self) -> None:
        schema = invoke("schema", str(self.csv))
        self.assertEqual(schema.returncode, 0, schema.stderr)
        schema_value = json.loads(schema.stdout)
        self.assertEqual(schema_value["schema_version"], 1)
        self.assertEqual(schema_value["sqrail_version"], "0.4.2")
        self.assertEqual([item["name"] for item in schema_value["columns"]], ["drug_id", "name", "amount"])

        result = invoke(
            "run",
            "-t",
            f"sales={self.csv}",
            "SELECT drug_id, sum(amount) AS total FROM sales GROUP BY drug_id ORDER BY drug_id",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            [json.loads(line) for line in result.stdout.splitlines()],
            [{"drug_id": 1, "total": 25}, {"drug_id": 2, "total": 20}],
        )

        output = self.root / "result.parquet"
        written = invoke(
            "run",
            "--stats",
            "-t",
            f"sales={self.csv}",
            "-o",
            str(output),
            "SELECT * FROM sales ORDER BY drug_id, amount",
        )
        self.assertEqual(written.returncode, 0, written.stderr)
        stats = json.loads(written.stderr)
        self.assertEqual(stats["schema_version"], 1)
        self.assertEqual(stats["destination"], "file")
        self.assertEqual(stats["rows"], 3)
        self.assertTrue(output.is_file())
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(output.stat().st_mode) & 0o077, 0)

        before = output.read_bytes()
        existing = invoke("run", "-o", str(output), "SELECT 99 AS value")
        self.assertEqual(existing.returncode, 5)
        self.assertEqual(diagnostic(existing)["code"], "OUTPUT_EXISTS")
        self.assertEqual(output.read_bytes(), before)

        stdout_limit = invoke("run", "--max-output-bytes", "10B", "SELECT 123456789 AS value")
        self.assertEqual(stdout_limit.returncode, 5)
        self.assertEqual(stdout_limit.stdout, b"")
        self.assertEqual(diagnostic(stdout_limit)["code"], "OUTPUT_LIMIT")

        limited_output = self.root / "limited.parquet"
        file_limit = invoke(
            "run",
            "--max-output-bytes",
            "1KiB",
            "-o",
            str(limited_output),
            "SELECT i, md5(i::VARCHAR) AS payload FROM range(10000) AS rows(i)",
        )
        self.assertEqual(file_limit.returncode, 5)
        self.assertEqual(diagnostic(file_limit)["code"], "OUTPUT_LIMIT")
        self.assertFalse(limited_output.exists())
        self.assertEqual(list(self.root.glob("limited.parquet.sqrail-tmp-*")), [])

        sql_limit = invoke("run", "--max-sql-bytes", "5B", "-", stdin=b"SELECT 123456789")
        self.assertEqual(sql_limit.returncode, 2)
        self.assertEqual(diagnostic(sql_limit)["code"], "SQL_LIMIT")

        evolved = self.root / "evolved"
        evolved.mkdir()
        for name, query in (
            ("a.parquet", "SELECT 1 AS id, 10 AS old_value"),
            ("b.parquet", "SELECT 2 AS id, 20 AS new_value"),
        ):
            generated = invoke("run", "-o", str(evolved / name), query)
            self.assertEqual(generated.returncode, 0, generated.stderr)
        input_limit = invoke("schema", "--max-input-files", "1", str(evolved))
        self.assertEqual(input_limit.returncode, 3)
        self.assertEqual(diagnostic(input_limit)["code"], "INPUT_LIMIT")

        timeout = invoke(
            "run",
            "--timeout",
            "1ms",
            "SELECT sum(a.i * b.i) FROM range(1000000) a(i), range(1000000) b(i)",
        )
        self.assertEqual(timeout.returncode, 4)
        self.assertEqual(diagnostic(timeout)["code"], "QUERY_TIMEOUT")

        broken = self.root / "broken.parquet"
        broken.write_bytes(b"not parquet")
        corrupt = invoke("schema", str(broken))
        self.assertNotEqual(corrupt.returncode, 0)
        self.assertIn(diagnostic(corrupt)["code"], {"SCHEMA_INFERENCE_FAILED", "QUERY_FAILED"})

        bad_sql = invoke("run", "SELECT FROM")
        self.assertEqual(bad_sql.returncode, 4)
        bad_sql_payload = diagnostic(bad_sql)
        self.assertEqual(bad_sql_payload["code"], "QUERY_FAILED")
        self.assertFalse(bad_sql_payload["message"].lstrip().startswith("{"))

    def test_help_matches_published_site_help(self) -> None:
        site_help = (Path(__file__).resolve().parents[1] / "site" / "help.txt").read_text(
            encoding="utf-8"
        )
        for flag in ("--help", "-h", "help"):
            with self.subTest(flag=flag):
                result = invoke(flag)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stderr, b"")
                actual = result.stdout.decode("utf-8").replace("\r\n", "\n")
                self.assertEqual(actual, site_help)

    def test_output_race_has_one_winner(self) -> None:
        output = self.root / "race.parquet"
        command = [
            str(SQRAIL),
            "run",
            "-o",
            str(output),
            "SELECT i FROM range(1000000) AS rows(i)",
        ]
        first = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        second = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        first_stdout, first_stderr = first.communicate(timeout=60)
        second_stdout, second_stderr = second.communicate(timeout=60)
        self.assertEqual(first_stdout, b"")
        self.assertEqual(second_stdout, b"")
        self.assertEqual(sorted((first.returncode, second.returncode)), [0, 5])
        losing_stderr = first_stderr if first.returncode == 5 else second_stderr
        self.assertEqual(diagnostic(subprocess.CompletedProcess(command, 5, b"", losing_stderr))["code"], "OUTPUT_EXISTS")
        self.assertTrue(output.is_file())
        self.assertEqual(list(self.root.glob("race.parquet.sqrail-tmp-*")), [])

    def test_unicode_and_long_output_paths(self) -> None:
        unicode_output = self.root / "résult 日本語.parquet"
        result = invoke("run", "-o", str(unicode_output), "SELECT '✓' AS value")
        self.assertEqual(result.returncode, 0, result.stderr)
        inspected = invoke("run", "-t", f"result={unicode_output}", "SELECT * FROM result")
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        self.assertEqual(json.loads(inspected.stdout), {"value": "✓"})

        long_parent = self.root
        for index in range(6):
            long_parent /= f"segment-{index}-" + ("x" * 36)
        long_parent.mkdir(parents=True)
        long_output = long_parent / "long-result.parquet"
        self.assertGreater(len(str(long_output.resolve())), 260)
        result = invoke("run", "-o", str(long_output), "SELECT 42 AS value")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(long_output.is_file())

    @unittest.skipIf(
        os.name == "nt" or (hasattr(os, "geteuid") and os.geteuid() == 0),
        "POSIX non-root mode fault injection",
    )
    def test_unwritable_output_directory_leaves_no_artifacts(self) -> None:
        blocked = self.root / "blocked"
        blocked.mkdir()
        for suffix in ("parquet", "jsonl"):
            with self.subTest(suffix=suffix):
                output = blocked / f"result.{suffix}"
                blocked.chmod(0o500)
                try:
                    result = invoke("run", "-o", str(output), "SELECT * FROM range(100)")
                finally:
                    blocked.chmod(0o700)
                self.assertEqual(result.returncode, 5)
                payload = diagnostic(result)
                self.assertEqual(payload["code"], "OUTPUT_WRITE")
                self.assertFalse(payload["message"].lstrip().startswith("{"))
                self.assertFalse(output.exists())
                self.assertEqual(list(blocked.glob(f"result.{suffix}.sqrail-tmp-*")), [])

        dangling = self.root / "dangling.parquet"
        dangling.symlink_to(self.root / "missing-target.parquet")
        dangling_result = invoke("run", "-o", str(dangling), "SELECT 1 AS value")
        self.assertEqual(dangling_result.returncode, 5)
        self.assertEqual(diagnostic(dangling_result)["code"], "OUTPUT_EXISTS")

    def test_interrupt_cleans_private_artifacts(self) -> None:
        output = self.root / "interrupted.parquet"
        spill = self.root / "spill"
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        process = subprocess.Popen(
            [
                str(SQRAIL),
                "run",
                "--memory",
                "64MiB",
                "--spill",
                str(spill),
                "--max-spill",
                "1GiB",
                "-o",
                str(output),
                (
                    "SELECT i, md5(i::VARCHAR) AS payload "
                    "FROM range(1000000000) AS rows(i) ORDER BY payload"
                ),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
        )
        for _ in range(1000):
            if list(self.root.glob("interrupted.parquet.sqrail-tmp-*")):
                break
            if process.poll() is not None:
                self.fail("interrupt query exited before creating its temporary output")
            time.sleep(0.01)
        else:
            process.kill()
            self.fail("interrupt query did not create its temporary output")

        if os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            process.send_signal(signal.SIGTERM)
        _, stderr = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 4, stderr)
        self.assertEqual(diagnostic(subprocess.CompletedProcess([], 4, b"", stderr))["code"], "QUERY_INTERRUPTED")
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob("interrupted.parquet.sqrail-tmp-*")), [])
        if spill.exists():
            self.assertEqual(list(spill.glob(".sqrail-spill-*")), [])

    def test_crlf_sql_on_stdin(self) -> None:
        queries = [
            (
                b"-- comment 1\r\nSELECT 1 AS one,\r\n--comment 2\r\n2 AS two; -- another comment\r\n",
                {"one": 1, "two": 2},
            ),
            (b"SELECT 1 AS one; \r\n \r\n", {"one": 1}),
            (b"\r\nSELECT\r\n  'a' AS letter\r\n;\r\n\r\n", {"letter": "a"}),
        ]
        for sql, row in queries:
            with self.subTest(sql=sql):
                result = invoke("run", "-", stdin=sql)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], [row])

        for sql in (b" ; \r\n", b"\r\n;\r\n;\r\n", b"\r\n \r\n"):
            with self.subTest(sql=sql):
                empty = invoke("run", "-", stdin=sql)
                self.assertEqual(empty.returncode, 2)
                self.assertEqual(diagnostic(empty)["code"], "EMPTY_SQL")

    def test_utf8_identifiers_hive_strict_all_varchar_and_hints(self) -> None:
        utf8_csv = self.root / "utf8_cols.csv"
        utf8_csv.write_text("薬品名,売上\nアルファ,100\n", encoding="utf-8")
        schema = invoke("schema", str(utf8_csv))
        self.assertEqual(schema.returncode, 0, schema.stderr)
        self.assertIn('"name":"薬品名"', schema.stdout.decode("utf-8"))
        check = invoke("check", "-t", f"sales={utf8_csv}", "SELECT 薬品名, 売上 FROM sales")
        self.assertEqual(check.returncode, 0, check.stderr)
        self.assertIn('"name":"薬品名"', check.stdout.decode("utf-8"))
        run_res = invoke("run", "-t", f"sales={utf8_csv}", "SELECT 薬品名, 売上 FROM sales")
        self.assertEqual(run_res.returncode, 0, run_res.stderr)
        self.assertEqual(run_res.stdout.decode("utf-8").strip(), '{"薬品名":"アルファ","売上":100}')

        hive = self.root / "hive_mixed"
        (hive / "tag=100").mkdir(parents=True)
        (hive / "tag=abc").mkdir(parents=True)
        self.assertEqual(invoke("run", "-o", str(hive / "tag=100" / "a.parquet"), "SELECT 1 AS id").returncode, 0)
        self.assertEqual(invoke("run", "-o", str(hive / "tag=abc" / "b.parquet"), "SELECT 2 AS id").returncode, 0)
        hive_run = invoke("run", "--strict-schema", "-t", f"data={hive}", "SELECT id, tag FROM data ORDER BY id")
        self.assertEqual(hive_run.returncode, 0, hive_run.stderr)
        self.assertEqual(
            [json.loads(line) for line in hive_run.stdout.splitlines()],
            [{"id": 1, "tag": "100"}, {"id": 2, "tag": "abc"}],
        )

        uneven = self.root / "hive_uneven"
        (uneven / "year=2025" / "month=1").mkdir(parents=True)
        (uneven / "year=2026").mkdir(parents=True)
        self.assertEqual(
            invoke("run", "-o", str(uneven / "year=2025" / "month=1" / "a.parquet"), "SELECT 1 AS id").returncode, 0
        )
        self.assertEqual(invoke("run", "-o", str(uneven / "year=2026" / "b.parquet"), "SELECT 2 AS id").returncode, 0)
        uneven_run = invoke("run", "--strict-schema", "-t", f"data={uneven}", "SELECT * FROM data")
        self.assertEqual(uneven_run.returncode, 3)
        self.assertEqual(diagnostic(uneven_run)["code"], "SCHEMA_MISMATCH")

        dirty = self.root / "dirty.csv"
        dirty.write_text("code,qty\n00123,10\nA-900,20\n", encoding="utf-8")
        varchar_run = invoke("run", "--all-varchar", "-t", f"lots={dirty}", "SELECT code, qty FROM lots ORDER BY code")
        self.assertEqual(varchar_run.returncode, 0, varchar_run.stderr)
        self.assertEqual(
            [json.loads(line) for line in varchar_run.stdout.splitlines()],
            [{"code": "00123", "qty": "10"}, {"code": "A-900", "qty": "20"}],
        )

        jsonl_cap = self.root / "capped.jsonl"
        over_cap = invoke("run", "--timeout", "1h", "--max-output-bytes", "7B", "-o", str(jsonl_cap), "SELECT 1 AS x")
        self.assertEqual(over_cap.returncode, 5)
        self.assertEqual(diagnostic(over_cap)["code"], "OUTPUT_LIMIT")
        self.assertFalse(jsonl_cap.exists())
        self.assertEqual(list(self.root.glob("capped.jsonl.sqrail-tmp-*")), [])

        agent_help = invoke("--agent-help")
        self.assertEqual(agent_help.returncode, 2)
        self.assertIn("(use 'sqrail --help')", str(diagnostic(agent_help)["message"]))
        sub_help = invoke("run", "--help")
        self.assertEqual(sub_help.returncode, 2)
        self.assertIn("(use 'sqrail --help')", str(diagnostic(sub_help)["message"]))

    def test_glob_metacharacters_in_directories_and_filenames(self) -> None:
        bracket_dir = self.root / "dir [1]"
        (bracket_dir / "events").mkdir(parents=True)
        (bracket_dir / "sales.csv").write_text("id,amount\n1,10\n", encoding="utf-8")
        (bracket_dir / "item [2].csv").write_text("id,amount\n2,20\n", encoding="utf-8")
        self.assertEqual(
            invoke("run", "-o", str(bracket_dir / "events" / "part.parquet"), "SELECT 7 AS id").returncode, 0
        )

        # Relative literal file, Parquet directory, glob, and missing file when CWD contains '[1]'.
        for relative_source, expected_rows in (
            ("sales.csv", [{"id": 1, "amount": 10}]),
            ("item [2].csv", [{"id": 2, "amount": 20}]),
            ("events", [{"id": 7}]),
            ("*.csv", [{"id": 1, "amount": 10}, {"id": 2, "amount": 20}]),
        ):
            with self.subTest(cwd_source=relative_source):
                schema = invoke("schema", relative_source, cwd=bracket_dir)
                self.assertEqual(schema.returncode, 0, schema.stderr)
                check = invoke("check", "-t", f"data={relative_source}", "SELECT * FROM data", cwd=bracket_dir)
                self.assertEqual(check.returncode, 0, check.stderr)
                run_res = invoke(
                    "run", "-t", f"data={relative_source}", "SELECT * FROM data ORDER BY id", cwd=bracket_dir
                )
                self.assertEqual(run_res.returncode, 0, run_res.stderr)
                self.assertEqual([json.loads(line) for line in run_res.stdout.splitlines()], expected_rows)

        missing_rel = invoke("schema", "missing.csv", cwd=bracket_dir)
        self.assertEqual(missing_rel.returncode, 3)
        self.assertEqual(diagnostic(missing_rel)["code"], "INPUT_NOT_FOUND")

        # Explicit absolute paths under a parent directory containing '[1]'.
        for explicit_source, expected_rows in (
            (str(bracket_dir / "sales.csv"), [{"id": 1, "amount": 10}]),
            (str(bracket_dir / "item [2].csv"), [{"id": 2, "amount": 20}]),
            (str(bracket_dir / "events"), [{"id": 7}]),
            (str(bracket_dir / "*.csv"), [{"id": 1, "amount": 10}, {"id": 2, "amount": 20}]),
        ):
            with self.subTest(explicit_source=explicit_source):
                schema = invoke("schema", explicit_source)
                self.assertEqual(schema.returncode, 0, schema.stderr)
                run_res = invoke("run", "-t", f"data={explicit_source}", "SELECT * FROM data ORDER BY id")
                self.assertEqual(run_res.returncode, 0, run_res.stderr)
                self.assertEqual([json.loads(line) for line in run_res.stdout.splitlines()], expected_rows)

        missing_abs = invoke("schema", str(bracket_dir / "missing.csv"))
        self.assertEqual(missing_abs.returncode, 3)
        self.assertEqual(diagnostic(missing_abs)["code"], "INPUT_NOT_FOUND")

        # Real character-class globs continue to match non-literal directory patterns.
        for suffix, val in (("a", 11), ("b", 22)):
            shard = self.root / f"shard_{suffix}"
            shard.mkdir()
            self.assertEqual(invoke("run", "-o", str(shard / "part.parquet"), f"SELECT {val} AS v").returncode, 0)
        char_glob = invoke("run", "-t", f"s={self.root / 'shard_[ab]' / '*.parquet'}", "SELECT v FROM s ORDER BY v")
        self.assertEqual(char_glob.returncode, 0, char_glob.stderr)
        self.assertEqual([json.loads(line) for line in char_glob.stdout.splitlines()], [{"v": 11}, {"v": 22}])

    def test_bracketed_input_is_read_exactly_once(self) -> None:
        (self.root / "item[1].csv").write_text("id\n10\n", encoding="utf-8")
        (self.root / "item1.csv").write_text("id\n20\n", encoding="utf-8")

        for bindings, expected in (
            (("-t", "items=item[1].csv"), [{"id": 10}]),
            (("-t", "items=item[1].csv", "-t", "other=item1.csv"), [{"id": 10}]),
            (("-t", "items=item*.csv"), [{"id": 10}, {"id": 20}]),
        ):
            with self.subTest(bindings=bindings):
                result = invoke("run", *bindings, "SELECT id FROM items ORDER BY id", cwd=self.root)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], expected)

    @unittest.skipIf(os.name == "nt", "POSIX path traversal semantics")
    def test_parent_component_is_resolved_after_following_symlink(self) -> None:
        target = self.root / "actual" / "sub"
        target.mkdir(parents=True)
        (self.root / "link").symlink_to(target, target_is_directory=True)
        (target.parent / "data.csv").write_text("id\n10\n", encoding="utf-8")
        (self.root / "data.csv").write_text("id\n20\n", encoding="utf-8")

        for source in ("link/../data.csv", "link/../*.csv"):
            with self.subTest(source=source):
                result = invoke("run", "-t", f"data={source}", "SELECT id FROM data", cwd=self.root)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], [{"id": 10}])

        schema = invoke("schema", "link/../data.csv", cwd=self.root)
        self.assertEqual(schema.returncode, 0, schema.stderr)
        reported_file = Path(json.loads(schema.stdout)["file"])
        self.assertTrue(reported_file.is_absolute())
        self.assertTrue(reported_file.samefile(target.parent / "data.csv"))

        missing = invoke("schema", "missing/../data.csv", cwd=self.root)
        self.assertEqual(missing.returncode, 3, missing.stderr)
        self.assertEqual(diagnostic(missing)["code"], "INPUT_NOT_FOUND")

    def test_empty_literal_directory_does_not_hide_character_class_matches(self) -> None:
        (self.root / "shard_[ab]").mkdir()
        for name, value in (("shard_a", 10), ("shard_b", 20)):
            shard = self.root / name
            shard.mkdir()
            (shard / "part.csv").write_text(f"id\n{value}\n", encoding="utf-8")

        for source in ("shard_[ab]/part.csv", "shard_[ab]/*.csv"):
            with self.subTest(source=source):
                result = invoke("run", "-t", f"data={source}", "SELECT id FROM data ORDER BY id", cwd=self.root)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], [{"id": 10}, {"id": 20}])

    @unittest.skipIf(os.name == "nt", "literal wildcard characters are not valid in Windows directory names")
    def test_relative_glob_only_matches_under_the_current_directory(self) -> None:
        sibling = self.root / "job1"
        sibling.mkdir()
        (sibling / "other.csv").write_text("id\n20\n", encoding="utf-8")

        for name, escaped_name in (("job?", "job[?]"), ("job*", "job[*]")):
            with self.subTest(directory=name):
                cwd = self.root / name
                cwd.mkdir()
                (cwd / "part.csv").write_text("id\n10\n", encoding="utf-8")
                result = invoke("run", "-t", "data=*.csv", "SELECT id FROM data", cwd=cwd)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], [{"id": 10}])

                escaped_sibling = self.root / escaped_name
                escaped_sibling.mkdir()
                (escaped_sibling / "missing*.csv").write_text("id\n30\n", encoding="utf-8")
                missing = invoke("schema", "missing*.csv", cwd=cwd)
                self.assertEqual(missing.returncode, 3, missing.stderr)

    def test_early_output_validation_and_compressed_json_byte_limit(self) -> None:
        bad_ext = invoke("run", "-t", f"missing={self.root / 'missing.csv'}", "-o", str(self.root / "out.txt"), "SELECT 1")
        self.assertEqual(bad_ext.returncode, 5)
        self.assertEqual(diagnostic(bad_ext)["code"], "UNSUPPORTED_OUTPUT")

        bad_comp = invoke(
            "run", "-t", f"missing={self.root / 'missing.csv'}", "-o", str(self.root / "out.parquet.gz"), "SELECT 1"
        )
        self.assertEqual(bad_comp.returncode, 5)
        self.assertEqual(diagnostic(bad_comp)["code"], "UNSUPPORTED_COMPRESSION")

        gz_cap = self.root / "capped.jsonl.gz"
        over_gz = invoke(
            "run",
            "--timeout",
            "1h",
            "--max-output-bytes",
            "32B",
            "-o",
            str(gz_cap),
            "SELECT i, md5(i::VARCHAR) AS digest FROM range(5000) AS rows(i)",
        )
        self.assertEqual(over_gz.returncode, 5)
        self.assertEqual(diagnostic(over_gz)["code"], "OUTPUT_LIMIT")
        self.assertFalse(gz_cap.exists())
        self.assertEqual(list(self.root.glob("capped.jsonl.gz.sqrail-tmp-*")), [])


if __name__ == "__main__":
    unittest.main()
