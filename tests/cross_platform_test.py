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


def invoke(*arguments: str, stdin: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(SQRAIL), *arguments],
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def diagnostic(result: subprocess.CompletedProcess[bytes]) -> dict[str, object]:
    value = json.loads(result.stderr.decode("utf-8"))
    assert value["schema_version"] == 1
    assert value["sqrail_version"] == "0.3.3"
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

    def test_contract_formats_limits_and_atomic_output(self) -> None:
        schema = invoke("schema", str(self.csv))
        self.assertEqual(schema.returncode, 0, schema.stderr)
        schema_value = json.loads(schema.stdout)
        self.assertEqual(schema_value["schema_version"], 1)
        self.assertEqual(schema_value["sqrail_version"], "0.3.3")
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
        blocked.chmod(0o500)
        output = blocked / "result.parquet"
        try:
            result = invoke("run", "-o", str(output), "SELECT * FROM range(10000)")
        finally:
            blocked.chmod(0o700)
        self.assertNotEqual(result.returncode, 0)
        diagnostic(result)
        self.assertFalse(output.exists())
        self.assertEqual(list(blocked.glob("result.parquet.sqrail-tmp-*")), [])

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


if __name__ == "__main__":
    unittest.main()
