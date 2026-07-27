#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVALUATOR = ROOT / "benchmarks" / "agent-eval.py"
TASKS = ROOT / "benchmarks" / "agent-tasks-v0.3.json"
TASK_IDS = [task["id"] for task in json.loads(TASKS.read_text(encoding="utf-8"))["tasks"]]


def attempt(
    *,
    arm: str,
    task: str,
    repetition: int,
    number: int = 1,
    success: bool = True,
    safety_violation: bool = False,
) -> dict[str, object]:
    return {
        "run_id": f"{task}-{repetition}",
        "model": "test/model-v1",
        "arm": arm,
        "task": task,
        "attempt": number,
        "artifact": f"{arm}-{task}-{repetition}",
        "session_id": f"session-{arm}-{task}-{repetition}",
        "success": success,
        "exit_code": 0 if success else 4,
        "wall_seconds": 0.25,
        "input_tokens": 100,
        "output_tokens": 20,
        "safety_violation": safety_violation,
    }


def complete_results() -> list[dict[str, object]]:
    return [
        attempt(arm=arm, task=task, repetition=repetition)
        for arm in ("sqrail", "duckdb")
        for task in TASK_IDS
        for repetition in range(5)
    ]


class AgentEvaluationTest(unittest.TestCase):
    def evaluate(
        self,
        values: list[dict[str, object]],
        *extra: str,
        trusted: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.jsonl"
            path.write_text(
                "".join(json.dumps(value, separators=(",", ":")) + "\n" for value in values),
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    sys.executable,
                    str(EVALUATOR),
                    str(path),
                    "--tasks",
                    str(TASKS),
                    *(("--trust-recorded", "--report-only") if trusted else ()),
                    *extra,
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

    def test_complete_passing_report(self) -> None:
        result = self.evaluate(complete_results())
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["passed"])
        self.assertFalse(report["evidence_verified"])
        self.assertFalse(
            report["models"]["test/model-v1"]["gate"]["evidence_verified"]
        )
        self.assertEqual(
            report["models"]["test/model-v1"]["arms"]["sqrail"]["overall"]["trials"],
            len(TASK_IDS) * 5,
        )

    def test_failed_first_attempt_can_recover(self) -> None:
        values = complete_results()
        target = next(
            value
            for value in values
            if value["arm"] == "sqrail"
            and value["task"] == TASK_IDS[0]
            and value["run_id"].endswith("-0")
        )
        target["success"] = False
        target["exit_code"] = 4
        values.append(
            attempt(
                arm="sqrail",
                task=TASK_IDS[0],
                repetition=0,
                number=2,
            )
        )
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        sqrail = report["models"]["test/model-v1"]["arms"]["sqrail"]["overall"]
        self.assertEqual(
            sqrail["first_attempt_success_rate"],
            round((len(TASK_IDS) * 5 - 1) / (len(TASK_IDS) * 5), 6),
        )
        self.assertEqual(sqrail["eventual_success_rate"], 1.0)

    def test_safety_violation_fails_gate(self) -> None:
        values = complete_results()
        values[0]["safety_violation"] = True
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 0)
        report = json.loads(result.stdout)
        self.assertFalse(report["passed"])
        self.assertFalse(
            report["models"]["test/model-v1"]["gate"]["zero_safety_violations"]
        )

    def test_duplicate_attempt_is_invalid(self) -> None:
        values = complete_results()
        values.append(dict(values[0]))
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 2)
        error = json.loads(result.stderr)
        self.assertIn("duplicate attempt", error["error"])

    def test_incomplete_report_only_mode(self) -> None:
        result = self.evaluate(complete_results()[:1])
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["passed"])
        self.assertFalse(report["models"]["test/model-v1"]["gate"]["complete"])

    def test_boolean_numeric_field_is_invalid(self) -> None:
        values = complete_results()
        values[0]["wall_seconds"] = True
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 2)
        error = json.loads(result.stderr)
        self.assertIn("wall_seconds has the wrong type", error["error"])

    def test_task_inferiority_fails_gate(self) -> None:
        values = complete_results()
        target = next(
            value
            for value in values
            if value["arm"] == "sqrail"
            and value["task"] == TASK_IDS[0]
            and value["run_id"].endswith("-0")
        )
        target["success"] = False
        target["exit_code"] = 4
        values.append(
            attempt(
                arm="sqrail",
                task=TASK_IDS[0],
                repetition=0,
                number=2,
                success=False,
            )
        )
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 0)
        gate = json.loads(result.stdout)["models"]["test/model-v1"]["gate"]
        self.assertEqual(gate["inferior_tasks"], [TASK_IDS[0]])

    def test_arms_require_paired_run_ids(self) -> None:
        values = complete_results()
        target = next(
            value
            for value in values
            if value["arm"] == "duckdb"
            and value["task"] == TASK_IDS[0]
            and value["run_id"].endswith("-0")
        )
        target["run_id"] = "different-run-id"
        result = self.evaluate(values)
        self.assertEqual(result.returncode, 0)
        gate = json.loads(result.stdout)["models"]["test/model-v1"]["gate"]
        self.assertFalse(gate["complete"])
        self.assertTrue(any("unpaired run ids" in issue for issue in gate["issues"]))

    def test_release_mode_requires_artifacts_data_and_duckdb(self) -> None:
        result = self.evaluate(complete_results(), trusted=False)
        self.assertEqual(result.returncode, 2)
        error = json.loads(result.stderr)
        self.assertIn("--artifacts, --data, --sqrail, and --duckdb", error["error"])

    def test_trusted_recording_cannot_be_used_as_a_gate(self) -> None:
        result = self.evaluate(complete_results())
        report = json.loads(result.stdout)
        gate = report["models"]["test/model-v1"]["gate"]
        self.assertTrue(gate["complete"])
        self.assertTrue(gate["first_attempt_at_least_90_percent"])
        self.assertFalse(gate["evidence_verified"])
        self.assertFalse(gate["passed"])


if __name__ == "__main__":
    unittest.main()
