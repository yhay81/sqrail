#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CAPTURE = ROOT / "benchmarks" / "capture-environment.py"


class BenchmarkEnvironmentTest(unittest.TestCase):
    def test_records_hashes_host_and_parameters(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text('{"rows":1}\n', encoding="utf-8")
            output = root / "environment.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(CAPTURE),
                    "--output",
                    str(output),
                    "--repository",
                    str(ROOT),
                    "--sqrail",
                    sys.executable,
                    "--duckdb",
                    sys.executable,
                    "--data-manifest",
                    str(manifest),
                    "--cache-state",
                    "warm",
                    "--parameters-json",
                    '{"runs":5,"warmup":1}',
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            value = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(value["schema_version"], 1)
            self.assertEqual(value["cache"]["state"], "warm")
            self.assertEqual(value["parameters"]["runs"], 5)
            self.assertEqual(len(value["executables"]["sqrail"]["sha256"]), 64)
            self.assertGreater(value["host"]["logical_cpu_count"], 0)
            self.assertEqual(len(value["data_manifest"]["sha256"]), 64)

    def test_controlled_cold_requires_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "environment.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(CAPTURE),
                    "--output",
                    str(output),
                    "--repository",
                    str(ROOT),
                    "--sqrail",
                    sys.executable,
                    "--cache-state",
                    "cold-controlled",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("require --cache-control-evidence", result.stderr)


if __name__ == "__main__":
    unittest.main()
