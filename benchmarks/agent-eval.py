#!/usr/bin/env python3
"""Validate and summarize sqrail agent-evaluation JSONL results."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

from agent_oracle import OracleError, verify_attempt


class EvaluationError(ValueError):
    """The input cannot support a reproducible evaluation."""


REQUIRED_FIELDS = {
    "run_id": str,
    "model": str,
    "arm": str,
    "task": str,
    "attempt": int,
    "artifact": str,
    "session_id": str,
    "wall_seconds": (int, float),
    "input_tokens": int,
    "output_tokens": int,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate JSONL attempts, calculate task-completion metrics, and "
            "enforce the acceptance gate in AGENT_EVALUATION.md."
        )
    )
    parser.add_argument("results", type=Path, help="one JSON object per attempt")
    parser.add_argument(
        "--tasks",
        type=Path,
        default=Path(__file__).with_name("agent-tasks-v0.3.json"),
        help="task corpus (default: adjacent agent-tasks-v0.3.json)",
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="artifact root produced by agent-run.py (required for a release gate)",
    )
    parser.add_argument(
        "--data",
        type=Path,
        help="deterministic benchmark dataset used by the attempts",
    )
    parser.add_argument(
        "--sqrail",
        type=Path,
        help="exact sqrail executable used by the attempts",
    )
    parser.add_argument(
        "--duckdb",
        type=Path,
        help="pinned DuckDB CLI used to recompute reference results",
    )
    parser.add_argument(
        "--trust-recorded",
        action="store_true",
        help="unit/exploratory mode: accept recorded booleans; requires --report-only and can never pass",
    )
    parser.add_argument(
        "--min-repetitions",
        type=int,
        default=5,
        help="required independent trials per model, arm, and task (default: 5)",
    )
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="return success after valid input even when the acceptance gate fails",
    )
    return parser.parse_args()


def load_tasks(path: Path) -> tuple[list[str], str, dict[str, dict[str, Any]]]:
    try:
        raw = path.read_bytes()
        document = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise EvaluationError(f"cannot read task corpus {path}: {error}") from error
    tasks = document.get("tasks")
    if not isinstance(tasks, list) or not tasks:
        raise EvaluationError("task corpus must contain a non-empty tasks array")
    task_ids: list[str] = []
    task_map: dict[str, dict[str, Any]] = {}
    for index, task in enumerate(tasks):
        if not isinstance(task, dict) or not isinstance(task.get("id"), str) or not task["id"]:
            raise EvaluationError(f"task corpus entry {index} has no valid id")
        task_ids.append(task["id"])
        oracle = task.get("oracle")
        if not isinstance(oracle, dict) or not isinstance(oracle.get("kind"), str):
            raise EvaluationError(f"task corpus entry {index} has no structured oracle")
        task_map[task["id"]] = task
    if len(set(task_ids)) != len(task_ids):
        raise EvaluationError("task corpus contains duplicate task ids")
    return task_ids, hashlib.sha256(raw).hexdigest(), task_map


def validate_attempt(value: Any, line_number: int, task_ids: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvaluationError(f"line {line_number}: attempt must be a JSON object")
    for field, expected_type in REQUIRED_FIELDS.items():
        if field not in value:
            raise EvaluationError(f"line {line_number}: missing {field}")
        if not isinstance(value[field], expected_type) or (
            field in {"attempt", "exit_code", "wall_seconds", "input_tokens", "output_tokens"}
            and isinstance(value[field], bool)
        ):
            raise EvaluationError(f"line {line_number}: {field} has the wrong type")

    if not value["run_id"] or not value["model"] or not value["artifact"] or not value["session_id"]:
        raise EvaluationError(
            f"line {line_number}: run_id, model, artifact, and session_id must be non-empty"
        )
    if value["arm"] not in {"sqrail", "duckdb"}:
        raise EvaluationError(f"line {line_number}: arm must be sqrail or duckdb")
    if value["task"] not in task_ids:
        raise EvaluationError(f"line {line_number}: unknown task {value['task']!r}")
    if value["attempt"] < 1 or value["attempt"] > 2:
        raise EvaluationError(f"line {line_number}: attempt must be 1 or 2")
    if (
        not math.isfinite(float(value["wall_seconds"]))
        or value["wall_seconds"] < 0
        or value["input_tokens"] < 0
        or value["output_tokens"] < 0
    ):
        raise EvaluationError(f"line {line_number}: time and token counts must be finite and non-negative")
    return value


def verify_recorded_attempt(value: dict[str, Any], line_number: int) -> dict[str, Any]:
    for field, expected in (
        ("success", bool),
        ("exit_code", int),
        ("safety_violation", bool),
    ):
        if field not in value or not isinstance(value[field], expected) or (
            field == "exit_code" and isinstance(value[field], bool)
        ):
            raise EvaluationError(f"line {line_number}: {field} is required in --trust-recorded mode")
    if value["exit_code"] < 0:
        raise EvaluationError(f"line {line_number}: exit_code must be non-negative")
    return value


def load_attempts(path: Path, task_ids: set[str]) -> tuple[list[dict[str, Any]], str]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvaluationError(f"cannot read results {path}: {error}") from error
    attempts: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(raw.splitlines(), start=1):
        if not raw_line.strip():
            continue
        try:
            value = json.loads(raw_line)
        except json.JSONDecodeError as error:
            raise EvaluationError(f"line {line_number}: invalid JSON: {error.msg}") from error
        attempts.append(validate_attempt(value, line_number, task_ids))
    if not attempts:
        raise EvaluationError("results contain no attempts")
    return attempts, hashlib.sha256(raw).hexdigest()


def rounded(value: float) -> float:
    return round(value, 6)


def calculate_metrics(trials: list[list[dict[str, Any]]]) -> dict[str, Any]:
    first_successes = 0
    eventual_successes = 0
    attempts_for_successes = 0
    all_attempts: list[dict[str, Any]] = []
    for trial in trials:
        all_attempts.extend(trial)
        first_success = trial[0]["success"]
        eventual = next((attempt for attempt in trial if attempt["success"]), None)
        first_successes += int(first_success)
        eventual_successes += int(eventual is not None)
        if eventual is not None:
            attempts_for_successes += eventual["attempt"]

    trial_count = len(trials)
    attempt_count = len(all_attempts)
    return {
        "trials": trial_count,
        "attempts": attempt_count,
        "first_attempt_successes": first_successes,
        "first_attempt_success_rate": rounded(first_successes / trial_count),
        "eventual_successes": eventual_successes,
        "eventual_success_rate": rounded(eventual_successes / trial_count),
        "mean_attempts_per_success": (
            rounded(attempts_for_successes / eventual_successes) if eventual_successes else None
        ),
        "input_tokens": sum(attempt["input_tokens"] for attempt in all_attempts),
        "output_tokens": sum(attempt["output_tokens"] for attempt in all_attempts),
        "wall_seconds": rounded(sum(float(attempt["wall_seconds"]) for attempt in all_attempts)),
        "safety_violations": sum(int(attempt["safety_violation"]) for attempt in all_attempts),
    }


def build_report(
    task_ids: list[str],
    task_sha256: str,
    result_sha256: str,
    attempts: list[dict[str, Any]],
    min_repetitions: int,
    evidence_verified: bool,
) -> dict[str, Any]:
    if min_repetitions < 1:
        raise EvaluationError("--min-repetitions must be at least 1")

    by_trial: dict[tuple[str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    seen_attempts: set[tuple[str, str, str, str, int]] = set()
    for attempt in attempts:
        trial_key = (attempt["model"], attempt["arm"], attempt["task"], attempt["run_id"])
        attempt_key = (*trial_key, attempt["attempt"])
        if attempt_key in seen_attempts:
            raise EvaluationError(
                "duplicate attempt for "
                f"model={attempt['model']!r}, arm={attempt['arm']}, "
                f"task={attempt['task']}, run_id={attempt['run_id']!r}, "
                f"attempt={attempt['attempt']}"
            )
        seen_attempts.add(attempt_key)
        by_trial[trial_key].append(attempt)

    trial_groups: dict[tuple[str, str, str], list[list[dict[str, Any]]]] = defaultdict(list)
    incomplete_trials: list[str] = []
    session_trials: dict[tuple[str, str, str], str] = {}
    for (model, arm, task, run_id), trial in sorted(by_trial.items()):
        trial.sort(key=lambda value: value["attempt"])
        if trial[0]["attempt"] != 1:
            raise EvaluationError(
                f"trial {model}/{arm}/{task}/{run_id} does not start at attempt 1"
            )
        if trial[0]["success"] and len(trial) > 1:
            raise EvaluationError(
                f"trial {model}/{arm}/{task}/{run_id} continues after success"
            )
        if not trial[0]["success"] and len(trial) == 1:
            incomplete_trials.append(f"{model}/{arm}/{task}/{run_id}")
        session_ids = {attempt["session_id"] for attempt in trial}
        if len(session_ids) != 1:
            raise EvaluationError(
                f"trial {model}/{arm}/{task}/{run_id} uses multiple model sessions"
            )
        session_id = next(iter(session_ids))
        session_key = (model, arm, session_id)
        previous_run = session_trials.get(session_key)
        if previous_run is not None and previous_run != run_id:
            raise EvaluationError(
                f"model session {model}/{arm}/{session_id} is reused by multiple trials"
            )
        session_trials[session_key] = run_id
        trial_groups[(model, arm, task)].append(trial)

    models = sorted({attempt["model"] for attempt in attempts})
    model_reports: dict[str, Any] = {}
    for model in models:
        arm_reports: dict[str, Any] = {}
        completeness_issues: list[str] = []
        for arm in ("sqrail", "duckdb"):
            task_reports: dict[str, Any] = {}
            arm_trials: list[list[dict[str, Any]]] = []
            for task in task_ids:
                trials = trial_groups.get((model, arm, task), [])
                task_reports[task] = calculate_metrics(trials) if trials else None
                arm_trials.extend(trials)
                if len(trials) < min_repetitions:
                    completeness_issues.append(
                        f"{arm}/{task}: {len(trials)} of {min_repetitions} repetitions"
                    )
            arm_reports[arm] = {
                "overall": calculate_metrics(arm_trials) if arm_trials else None,
                "tasks": task_reports,
            }

        for task in task_ids:
            sqrail_trials = trial_groups.get((model, "sqrail", task), [])
            duckdb_trials = trial_groups.get((model, "duckdb", task), [])
            sqrail_count = len(sqrail_trials)
            duckdb_count = len(duckdb_trials)
            if sqrail_count != duckdb_count:
                completeness_issues.append(
                    f"unbalanced {task}: sqrail={sqrail_count}, duckdb={duckdb_count}"
                )
            sqrail_runs = {trial[0]["run_id"] for trial in sqrail_trials}
            duckdb_runs = {trial[0]["run_id"] for trial in duckdb_trials}
            if sqrail_runs != duckdb_runs:
                completeness_issues.append(
                    f"unpaired run ids for {task}: "
                    f"sqrail-only={sorted(sqrail_runs - duckdb_runs)}, "
                    f"duckdb-only={sorted(duckdb_runs - sqrail_runs)}"
                )

        model_incomplete = [
            trial for trial in incomplete_trials if trial.startswith(f"{model}/")
        ]
        completeness_issues.extend(
            f"missing attempt 2 after failure: {trial}" for trial in model_incomplete
        )
        complete = not completeness_issues
        sqrail_overall = arm_reports["sqrail"]["overall"]
        first_gate = bool(
            sqrail_overall and sqrail_overall["first_attempt_success_rate"] >= 0.9
        )
        eventual_gate = bool(
            sqrail_overall and sqrail_overall["eventual_success_rate"] >= 0.98
        )
        safety_gate = bool(sqrail_overall and sqrail_overall["safety_violations"] == 0)
        inferior_tasks: list[str] = []
        for task in task_ids:
            sqrail_task = arm_reports["sqrail"]["tasks"][task]
            duckdb_task = arm_reports["duckdb"]["tasks"][task]
            if sqrail_task and duckdb_task and (
                sqrail_task["eventual_successes"] * duckdb_task["trials"]
                < duckdb_task["eventual_successes"] * sqrail_task["trials"]
            ):
                inferior_tasks.append(task)
        noninferior_gate = not inferior_tasks and all(
            arm_reports[arm]["tasks"][task] is not None
            for arm in ("sqrail", "duckdb")
            for task in task_ids
        )
        gate = {
            "evidence_verified": evidence_verified,
            "complete": complete,
            "first_attempt_at_least_90_percent": first_gate,
            "eventual_at_least_98_percent": eventual_gate,
            "zero_safety_violations": safety_gate,
            "no_task_inferior_to_duckdb": noninferior_gate,
            "inferior_tasks": inferior_tasks,
            "issues": completeness_issues,
        }
        gate["passed"] = all(
            gate[name]
            for name in (
                "complete",
                "evidence_verified",
                "first_attempt_at_least_90_percent",
                "eventual_at_least_98_percent",
                "zero_safety_violations",
                "no_task_inferior_to_duckdb",
            )
        )
        model_reports[model] = {"arms": arm_reports, "gate": gate}

    return {
        "schema_version": 1,
        "task_corpus_sha256": task_sha256,
        "results_sha256": result_sha256,
        "minimum_repetitions": min_repetitions,
        "evidence_verified": evidence_verified,
        "models": model_reports,
        "passed": all(report["gate"]["passed"] for report in model_reports.values()),
    }


def main() -> int:
    arguments = parse_args()
    try:
        task_ids, task_sha256, tasks = load_tasks(arguments.tasks)
        attempts, result_sha256 = load_attempts(arguments.results, set(task_ids))
        evidence_verified = not arguments.trust_recorded
        if arguments.trust_recorded:
            if not arguments.report_only:
                raise EvaluationError("--trust-recorded requires --report-only")
            attempts = [
                verify_recorded_attempt(attempt, index)
                for index, attempt in enumerate(attempts, start=1)
            ]
        else:
            if (
                arguments.artifacts is None
                or arguments.data is None
                or arguments.sqrail is None
                or arguments.duckdb is None
            ):
                raise EvaluationError(
                    "--artifacts, --data, --sqrail, and --duckdb are required for oracle verification"
                )
            if not arguments.sqrail.is_file():
                raise EvaluationError(f"sqrail executable does not exist: {arguments.sqrail}")
            if not arguments.duckdb.is_file():
                raise EvaluationError(f"DuckDB CLI does not exist: {arguments.duckdb}")
            verified_attempts: list[dict[str, Any]] = []
            for index, attempt in enumerate(attempts, start=1):
                try:
                    verification = verify_attempt(
                        attempt,
                        tasks[attempt["task"]],
                        arguments.tasks,
                        arguments.artifacts,
                        arguments.data,
                        arguments.sqrail,
                        arguments.duckdb,
                    )
                except (OracleError, OSError, subprocess.SubprocessError) as error:
                    raise EvaluationError(f"line {index}: oracle verification failed: {error}") from error
                verified = dict(attempt)
                verified.update(
                    {
                        "success": verification.success,
                        "exit_code": verification.exit_code,
                        "safety_violation": verification.safety_violation,
                        "oracle_issues": list(verification.issues),
                        "evidence_sha256": verification.evidence_sha256,
                    }
                )
                for field in ("success", "exit_code", "safety_violation", "evidence_sha256"):
                    if field in attempt and attempt[field] != verified[field]:
                        raise EvaluationError(
                            f"line {index}: recorded {field} differs from the independent oracle"
                        )
                verified_attempts.append(verified)
            attempts = verified_attempts
        report = build_report(
            task_ids,
            task_sha256,
            result_sha256,
            attempts,
            arguments.min_repetitions,
            evidence_verified,
        )
    except EvaluationError as error:
        print(json.dumps({"ok": False, "error": str(error)}, separators=(",", ":")), file=sys.stderr)
        return 2

    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["passed"] or arguments.report_only else 1


if __name__ == "__main__":
    sys.exit(main())
