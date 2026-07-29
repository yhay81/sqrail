#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"
sys.path.insert(0, str(BENCHMARKS))

from agent_oracle import (  # noqa: E402
    OracleError,
    duration_seconds,
    requested_timeout,
    verify_attempt,
)


TASKS_PATH = BENCHMARKS / "agent-tasks-v0.3.json"
EVALUATOR = BENCHMARKS / "agent-eval.py"
TASK = next(
    task
    for task in json.loads(TASKS_PATH.read_text(encoding="utf-8"))["tasks"]
    if task["id"] == "schema_discovery"
)


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(root: Path) -> dict[str, dict[str, object]]:
    return {
        path.relative_to(root).as_posix(): {
            "sha256": file_sha256(path),
            "bytes": path.stat().st_size,
        }
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


class AgentOracleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.data = self.root / "data"
        self.artifacts = self.root / "artifacts"
        self.artifact = self.artifacts / "attempt-1"
        self.workspace = self.artifact / "workspace"
        self.data.mkdir()
        (self.data / "schema-evolution").mkdir()
        self.workspace.mkdir(parents=True)

        sources = {
            "fact_csv": "fact.csv",
            "fact_parquet": "fact.parquet",
            "dim_parquet": "dim.parquet",
            "evolution_a_parquet": "schema-evolution/part-a.parquet",
            "evolution_b_parquet": "schema-evolution/part-b.parquet",
        }
        paths: dict[str, str] = {}
        digests: dict[str, str] = {}
        for role, relative in sources.items():
            source = self.data / relative
            source.write_bytes(f"fixture:{role}\n".encode())
            workspace_relative = role + Path(relative).suffix
            destination = self.workspace / workspace_relative
            destination.write_bytes(source.read_bytes())
            paths[role] = workspace_relative
            digests[role] = file_sha256(source)
        paths["output"] = "output.jsonl"
        paths["spill"] = "spill"

        (self.data / "manifest.json").write_text('{"fixture":true}\n', encoding="utf-8")
        schema_stdout = {
            "schema_version": 1,
            "sqrail_version": "0.3.2",
            "file": paths["fact_csv"],
            "files": 1,
            "columns": [
                {"name": name, "type": "VARCHAR", "nullable": True}
                for name in TASK["oracle"]["columns"]
            ],
        }
        (self.artifact / "invocation-1.stdout").write_text(
            json.dumps(schema_stdout) + "\n",
            encoding="utf-8",
        )
        (self.artifact / "invocation-1.stderr").write_bytes(b"")

        self.session = {
            "schema_version": 1,
            "artifact_id": "attempt-1",
            "run_id": "run-1",
            "model": "test/model",
            "arm": "sqrail",
            "task": "schema_discovery",
            "attempt": 1,
            "dataset_manifest_sha256": file_sha256(self.data / "manifest.json"),
            "sqrail_sha256": file_sha256(Path(sys.executable)),
            "duckdb_sha256": file_sha256(Path(sys.executable)),
            "task_corpus_sha256": file_sha256(TASKS_PATH),
            "evaluation_source_sha256": {
                name: file_sha256(path)
                for name, path in {
                    "runner": BENCHMARKS / "agent-run.py",
                    "oracle": BENCHMARKS / "agent_oracle.py",
                    "evaluator": BENCHMARKS / "agent-eval.py",
                }.items()
            },
            "paths": paths,
            "source_paths": sources,
            "input_sha256": digests,
        }
        initial = snapshot(self.workspace)
        self.session["initial_snapshot"] = initial
        self.record = {
            "schema_version": 1,
            "run_id": "run-1",
            "model": "test/model",
            "arm": "sqrail",
            "task": "schema_discovery",
            "attempt": 1,
            "session_id": "session-1",
            "before": initial,
            "after": initial,
            "invocations": [
                {
                    "argv": ["sqrail", "schema", paths["fact_csv"]],
                    "exit_code": 0,
                    "wall_seconds": 0.1,
                    "stdout": "invocation-1.stdout",
                    "stderr": "invocation-1.stderr",
                }
            ],
        }
        self.attempt = {
            "run_id": "run-1",
            "model": "test/model",
            "arm": "sqrail",
            "task": "schema_discovery",
            "attempt": 1,
            "artifact": "attempt-1",
            "session_id": "session-1",
            "wall_seconds": 0.1,
            "input_tokens": 10,
            "output_tokens": 10,
        }
        self.write_metadata()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_metadata(self) -> None:
        (self.artifact / "session.json").write_text(
            json.dumps(self.session, sort_keys=True),
            encoding="utf-8",
        )
        (self.artifact / "record.json").write_text(
            json.dumps(self.record, sort_keys=True),
            encoding="utf-8",
        )

    def test_artifact_oracle_derives_success(self) -> None:
        result = verify_attempt(
            self.attempt,
            TASK,
            TASKS_PATH,
            self.artifacts,
            self.data,
            Path(sys.executable),
            Path(sys.executable),
        )
        self.assertTrue(result.success)
        self.assertFalse(result.safety_violation)
        self.assertEqual(len(result.evidence_sha256), 64)

    def test_tampered_success_flag_is_rejected(self) -> None:
        (self.artifact / "invocation-1.stdout").write_text(
            '{"columns":[{"name":"wrong"}]}\n',
            encoding="utf-8",
        )
        indexed = dict(self.attempt, success=True, exit_code=0, safety_violation=False)
        results = self.root / "results.jsonl"
        results.write_text(json.dumps(indexed) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [
                sys.executable,
                str(EVALUATOR),
                str(results),
                "--tasks",
                str(TASKS_PATH),
                "--artifacts",
                str(self.artifacts),
                "--data",
                str(self.data),
                "--sqrail",
                sys.executable,
                "--duckdb",
                sys.executable,
                "--min-repetitions",
                "1",
                "--report-only",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("recorded success differs", completed.stderr)

    def test_changed_dataset_is_rejected(self) -> None:
        (self.workspace / self.session["paths"]["fact_csv"]).write_bytes(b"changed")
        with self.assertRaisesRegex(OracleError, "dataset role digest differs"):
            verify_attempt(
                self.attempt,
                TASK,
                TASKS_PATH,
                self.artifacts,
                self.data,
                Path(sys.executable),
                Path(sys.executable),
            )

    def test_changed_evaluation_source_digest_is_rejected(self) -> None:
        self.session["evaluation_source_sha256"]["runner"] = "0" * 64
        self.write_metadata()
        with self.assertRaisesRegex(OracleError, "evaluation source digests"):
            verify_attempt(
                self.attempt,
                TASK,
                TASKS_PATH,
                self.artifacts,
                self.data,
                Path(sys.executable),
                Path(sys.executable),
            )

    def test_timeout_duration_is_recomputed_from_recorded_arguments(self) -> None:
        self.assertEqual(duration_seconds("10ms"), 0.01)
        self.assertEqual(duration_seconds("0.01s"), 0.01)
        self.assertIsNone(duration_seconds("--signal=TERM"))
        duckdb_record = {
            "invocations": [{"argv": ["timeout", "10ms", "duckdb", "-c", "SELECT 1"]}]
        }
        sqrail_record = {
            "invocations": [{"argv": ["sqrail", "run", "--timeout=10ms", "SELECT 1"]}]
        }
        self.assertEqual(requested_timeout(duckdb_record, "duckdb"), 0.01)
        self.assertEqual(requested_timeout(sqrail_record, "sqrail"), 0.01)


if __name__ == "__main__":
    unittest.main()
