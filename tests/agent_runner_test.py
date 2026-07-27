#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "benchmarks" / "agent-run.py"
sys.path.insert(0, str(RUNNER_PATH.parent))
SPEC = importlib.util.spec_from_file_location("agent_runner", RUNNER_PATH)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class AgentRunnerTest(unittest.TestCase):
    def candidate(self, *argv: str) -> dict[str, object]:
        return {
            "schema_version": 1,
            "input_tokens": 1,
            "output_tokens": 1,
            "session_id": "session-1",
            "invocations": [{"argv": list(argv), "stdin": None}],
        }

    def test_rejects_paths_outside_workspace(self) -> None:
        invalid = (
            ("duckdb", "-c", "SELECT * FROM read_csv('/etc/passwd')"),
            ("duckdb", "-c", r"SELECT * FROM read_csv('C:\\secret.csv')"),
            ("duckdb", "-c", "SELECT * FROM read_csv('../secret.csv')"),
            ("duckdb", "-c", "SELECT * FROM read_csv('https://example.test/data.csv')"),
        )
        for argv in invalid:
            with self.subTest(argv=argv), self.assertRaises(RUNNER.RunnerError):
                RUNNER.validate_candidate(self.candidate(*argv), "duckdb")

    def test_allows_relative_paths_and_sql_division(self) -> None:
        invocations = RUNNER.validate_candidate(
            self.candidate(
                "duckdb",
                "-json",
                "-c",
                "SELECT value / 2 FROM read_parquet('token-fact.parquet')",
            ),
            "duckdb",
        )
        self.assertEqual(len(invocations), 1)

    def test_rejects_external_references_in_executable_and_stdin(self) -> None:
        with self.assertRaisesRegex(RUNNER.RunnerError, "absolute path"):
            RUNNER.validate_candidate(
                self.candidate("/tmp/duckdb", "-json", "-c", "SELECT 1"),
                "duckdb",
            )
        candidate = self.candidate("duckdb", "-json")
        candidate["invocations"][0]["stdin"] = "COPY (SELECT 1) TO '/tmp/out.parquet'"
        with self.assertRaisesRegex(
            RUNNER.RunnerError, "stdin contains an absolute path"
        ):
            RUNNER.validate_candidate(candidate, "duckdb")

    def test_timeout_wrapper_is_limited_to_duckdb_timeout_task(self) -> None:
        candidate = self.candidate(
            "timeout",
            "0.01s",
            "duckdb",
            "-c",
            "SELECT count(*) FROM range(1000000000)",
        )
        self.assertEqual(
            len(RUNNER.validate_candidate(candidate, "duckdb", "timeout_recovery")),
            1,
        )
        with self.assertRaises(RUNNER.RunnerError):
            RUNNER.validate_candidate(candidate, "duckdb", "schema_discovery")

    def test_input_copy_is_independent_and_read_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.csv"
            destination = root / "workspace" / "copy.csv"
            source.write_bytes(b"value\n1\n")
            RUNNER.copy_read_only(source, destination)
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            source.write_bytes(b"value\n2\n")
            self.assertNotEqual(destination.read_bytes(), source.read_bytes())
            if os.name != "nt":
                self.assertEqual(destination.stat().st_mode & 0o222, 0)

    def test_task_prompts_expose_oracle_specific_requirements(self) -> None:
        corpus = json.loads(
            (ROOT / "benchmarks" / "agent-tasks-v0.3.json").read_text(encoding="utf-8")
        )
        tasks = {task["id"]: task for task in corpus["tasks"]}
        requirements = {
            "join_aggregate": ("drug_class", "observation_count"),
            "timeout_recovery": ("cross join", "10 ms"),
            "check_metadata": ("drug_id", "observations"),
            "success_stats": ("column rows",),
        }
        for task_id, required_phrases in requirements.items():
            prompt = tasks[task_id]["prompt"].lower()
            with self.subTest(task=task_id):
                for phrase in required_phrases:
                    self.assertIn(phrase, prompt)
        self.assertEqual(
            tasks["timeout_recovery"]["oracle"]["required_arguments"],
            ["cross join"],
        )


if __name__ == "__main__":
    unittest.main()
