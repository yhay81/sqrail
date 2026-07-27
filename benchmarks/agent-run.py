#!/usr/bin/env python3
"""Prepare and execute reproducible agent-evaluation attempts without a shell."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from agent_oracle import OracleError, sha256, verify_attempt


class RunnerError(ValueError):
    """The requested attempt is unsafe or incomplete."""


ROLE_SOURCES = {
    "fact_csv": "fact.csv",
    "fact_parquet": "fact.parquet",
    "dim_parquet": "dim.parquet",
    "evolution_a_parquet": "schema-evolution/part-a.parquet",
    "evolution_b_parquet": "schema-evolution/part-b.parquet",
}

OUTPUT_FORMATS = {
    "join_aggregate": ".parquet",
    "csv_to_parquet": ".parquet",
    "parquet_to_jsonl": ".jsonl",
    "bounded_sort": ".parquet",
    "timeout_recovery": ".parquet",
    "no_overwrite": ".parquet",
    "result_limit": ".parquet",
}

PROMPT_PATH_ROLES = {
    "schema_discovery": ("fact_csv",),
    "selective_jsonl": ("fact_parquet",),
    "join_aggregate": ("fact_parquet", "dim_parquet", "output"),
    "csv_to_parquet": ("fact_csv", "output"),
    "parquet_to_jsonl": ("fact_parquet", "output"),
    "bounded_sort": ("fact_parquet", "output", "spill"),
    "timeout_recovery": ("fact_parquet",),
    "no_overwrite": ("fact_parquet", "output"),
    "check_metadata": ("fact_parquet",),
    "result_limit": ("fact_parquet", "output"),
    "schema_evolution": ("evolution_a_parquet", "evolution_b_parquet"),
    "success_stats": ("fact_parquet",),
}

EVALUATION_SOURCES = {
    "runner": Path(__file__).resolve(),
    "oracle": Path(__file__).with_name("agent_oracle.py").resolve(),
    "evaluator": Path(__file__).with_name("agent-eval.py").resolve(),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="create a fresh randomized session")
    prepare.add_argument("--artifact-root", type=Path, required=True)
    prepare.add_argument("--artifact-id", required=True)
    prepare.add_argument("--data", type=Path, required=True)
    prepare.add_argument("--tasks", type=Path, default=Path(__file__).with_name("agent-tasks-v0.3.json"))
    prepare.add_argument("--sqrail", type=Path, required=True)
    prepare.add_argument("--duckdb", type=Path, required=True)
    prepare.add_argument("--model", required=True)
    prepare.add_argument("--arm", choices=("sqrail", "duckdb"), required=True)
    prepare.add_argument("--task", required=True)
    prepare.add_argument("--run-id", required=True)
    prepare.add_argument("--attempt", type=int, choices=(1, 2), required=True)

    execute = subparsers.add_parser("execute", help="execute a model-produced command plan")
    execute.add_argument("--artifact-root", type=Path, required=True)
    execute.add_argument("--artifact-id", required=True)
    execute.add_argument("--candidate", type=Path, required=True)
    execute.add_argument("--data", type=Path, required=True)
    execute.add_argument("--tasks", type=Path, default=Path(__file__).with_name("agent-tasks-v0.3.json"))
    execute.add_argument("--sqrail", type=Path, required=True)
    execute.add_argument("--duckdb", type=Path, required=True)
    execute.add_argument("--command-timeout", type=float, default=120.0)
    return parser.parse_args()


def load_document(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_bytes())
    except (OSError, json.JSONDecodeError) as error:
        raise RunnerError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise RunnerError(f"JSON document must be an object: {path}")
    return value


def task_map(path: Path) -> dict[str, dict[str, Any]]:
    document = load_document(path)
    tasks = document.get("tasks")
    if not isinstance(tasks, list):
        raise RunnerError("task corpus has no tasks array")
    result: dict[str, dict[str, Any]] = {}
    for task in tasks:
        if not isinstance(task, dict) or not isinstance(task.get("id"), str):
            raise RunnerError("task corpus contains an invalid task")
        result[task["id"]] = task
    return result


def validate_artifact_id(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", value):
        raise RunnerError("artifact-id must be a portable 1-128 character slug")
    return value


def snapshot(root: Path) -> dict[str, dict[str, object]]:
    values: dict[str, dict[str, object]] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise RunnerError(f"workspace contains a symlink: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            values[relative] = {"sha256": sha256(path), "bytes": path.stat().st_size}
    return values


def copy_read_only(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)


def prepare_session(arguments: argparse.Namespace) -> int:
    artifact_id = validate_artifact_id(arguments.artifact_id)
    tasks = task_map(arguments.tasks)
    if arguments.task not in tasks:
        raise RunnerError(f"unknown task: {arguments.task}")
    if not arguments.sqrail.is_file() or not arguments.duckdb.is_file():
        raise RunnerError("sqrail and DuckDB executables must exist")
    data = arguments.data.resolve()
    manifest = data / "manifest.json"
    if not manifest.is_file():
        raise RunnerError("benchmark dataset has no manifest.json")

    artifact = arguments.artifact_root.resolve() / artifact_id
    if artifact.exists():
        raise RunnerError(f"refusing to replace artifact: {artifact}")
    workspace = artifact / "workspace"
    workspace.mkdir(parents=True)
    token = secrets.token_hex(6)

    paths: dict[str, str] = {}
    sources: dict[str, str] = {}
    input_digests: dict[str, str] = {}
    for role, source_relative in ROLE_SOURCES.items():
        source = data / source_relative
        if not source.is_file():
            raise RunnerError(f"dataset input is missing: {source_relative}")
        if role.startswith("evolution_"):
            destination_relative = f"{token}-evolution/{Path(source_relative).name}"
        else:
            destination_relative = f"{token}-{Path(source_relative).name}"
        destination = workspace / destination_relative
        copy_read_only(source, destination)
        paths[role] = destination_relative
        sources[role] = source_relative
        input_digests[role] = sha256(source)

    output_suffix = OUTPUT_FORMATS.get(arguments.task, ".jsonl")
    paths["output"] = f"{token}-output{output_suffix}"
    paths["spill"] = f"{token}-spill"
    if arguments.task == "no_overwrite":
        (workspace / paths["output"]).write_bytes(b"sqrail-agent-evaluation-sentinel\n")

    selected = arguments.sqrail if arguments.arm == "sqrail" else arguments.duckdb
    help_argument = "--agent-help" if arguments.arm == "sqrail" else "--help"
    help_result = subprocess.run(
        [str(selected.resolve()), help_argument],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if help_result.returncode != 0:
        raise RunnerError(f"cannot capture {arguments.arm} help")

    try:
        prompt_path_roles = PROMPT_PATH_ROLES[arguments.task]
    except KeyError as error:
        raise RunnerError(f"task has no prompt-path contract: {arguments.task}") from error
    path_lines = "\n".join(
        f"- {role}: {paths[role]}" for role in prompt_path_roles
    )
    prompt = (
        tasks[arguments.task]["prompt"]
        + "\n\nUse these attempt-specific relative paths:\n"
        + path_lines
        + "\nReturn a JSON command plan matching candidate-schema.json; do not use a shell."
    )
    session = {
        "schema_version": 1,
        "artifact_id": artifact_id,
        "run_id": arguments.run_id,
        "model": arguments.model,
        "arm": arguments.arm,
        "task": arguments.task,
        "attempt": arguments.attempt,
        "prompt": prompt,
        "prompt_path_roles": list(prompt_path_roles),
        "tool_help": help_result.stdout.decode("utf-8", errors="replace"),
        "tool_help_sha256": hashlib.sha256(help_result.stdout).hexdigest(),
        "sqrail_sha256": sha256(arguments.sqrail.resolve()),
        "duckdb_sha256": sha256(arguments.duckdb.resolve()),
        "task_corpus_sha256": sha256(arguments.tasks.resolve()),
        "evaluation_source_sha256": {
            name: sha256(path) for name, path in EVALUATION_SOURCES.items()
        },
        "dataset_manifest_sha256": sha256(manifest),
        "paths": paths,
        "source_paths": sources,
        "input_sha256": input_digests,
        "initial_snapshot": snapshot(workspace),
        "candidate_schema": {
            "schema_version": 1,
            "input_tokens": "non-negative integer",
            "output_tokens": "non-negative integer",
            "session_id": "non-empty provider session identifier",
            "invocations": [
                {
                    "argv": [arguments.arm, "arguments without shell syntax"],
                    "stdin": "optional UTF-8 string or null",
                }
            ],
        },
    }
    (artifact / "session.json").write_text(
        json.dumps(session, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (artifact / "candidate-schema.json").write_text(
        json.dumps(session["candidate_schema"], indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"artifact": artifact_id, "session": str(artifact / "session.json"), "prompt": prompt}))
    return 0


def validate_candidate(
    candidate: dict[str, Any],
    arm: str,
    task: str | None = None,
) -> list[dict[str, Any]]:
    def reject_external_reference(value: str, location: str) -> None:
        if re.search(
            r"(?:^|[\s'\"=(,])(?:/(?=[^\s/])|[A-Za-z]:[\\/]|\\\\)",
            value,
        ):
            raise RunnerError(f"{location} contains an absolute path")
        if re.search(r"(?:^|[\\/\s'\"=(,])\.\.(?:[\\/]|$)", value):
            raise RunnerError(f"{location} contains parent traversal")
        if re.search(r"(?i)\b(?:file|https?|s3|gs|azure)://", value):
            raise RunnerError(f"{location} contains an external URI")

    if candidate.get("schema_version") != 1:
        raise RunnerError("candidate schema_version must be 1")
    for field in ("input_tokens", "output_tokens"):
        value = candidate.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise RunnerError(f"candidate {field} must be a non-negative integer")
    if not isinstance(candidate.get("session_id"), str) or not candidate["session_id"]:
        raise RunnerError("candidate session_id must be non-empty")
    invocations = candidate.get("invocations")
    if not isinstance(invocations, list) or not 1 <= len(invocations) <= 4:
        raise RunnerError("candidate must contain one through four invocations")
    for index, invocation in enumerate(invocations):
        if not isinstance(invocation, dict):
            raise RunnerError(f"candidate invocation {index} is not an object")
        argv = invocation.get("argv")
        if not isinstance(argv, list) or not argv or len(argv) > 64:
            raise RunnerError(f"candidate invocation {index} argv is invalid")
        if not all(isinstance(value, str) and "\0" not in value for value in argv):
            raise RunnerError(f"candidate invocation {index} argv must contain plain strings")
        for value in argv:
            reject_external_reference(value, f"candidate invocation {index}")
        executable = Path(argv[0]).name.lower().removesuffix(".exe")
        allowed = {arm}
        if executable in {"timeout", "gtimeout"}:
            if arm != "duckdb" or task != "timeout_recovery":
                raise RunnerError(
                    f"timeout wrapper in invocation {index} is allowed only for "
                    "the DuckDB timeout-recovery arm"
                )
            nested = {
                Path(value).name.lower().removesuffix(".exe")
                for value in argv[1:4]
            }
            if arm not in nested:
                raise RunnerError(f"timeout wrapper in invocation {index} does not launch {arm}")
        elif executable not in allowed:
            raise RunnerError(f"invocation {index} may only launch the selected {arm} CLI")
        stdin = invocation.get("stdin")
        if stdin is not None and not isinstance(stdin, str):
            raise RunnerError(f"candidate invocation {index} stdin must be a string or null")
        if isinstance(stdin, str):
            reject_external_reference(stdin, f"candidate invocation {index} stdin")
    return invocations


def resolve_command(
    argv: list[str],
    sqrail: Path,
    duckdb: Path,
) -> list[str]:
    resolved = list(argv)
    for index, value in enumerate(resolved):
        executable = Path(value).name.lower().removesuffix(".exe")
        if executable == "sqrail":
            resolved[index] = str(sqrail.resolve())
        elif executable == "duckdb":
            resolved[index] = str(duckdb.resolve())
    return resolved


def execute_candidate(arguments: argparse.Namespace) -> int:
    artifact_id = validate_artifact_id(arguments.artifact_id)
    artifact = arguments.artifact_root.resolve() / artifact_id
    session = load_document(artifact / "session.json")
    candidate = load_document(arguments.candidate)
    workspace = artifact / "workspace"
    if (artifact / "record.json").exists():
        raise RunnerError("refusing to replace an executed attempt")
    if snapshot(workspace) != session.get("initial_snapshot"):
        raise RunnerError("workspace changed after prepare and before execute")
    if session.get("artifact_id") != artifact_id:
        raise RunnerError("session artifact id differs")
    if sha256(arguments.sqrail.resolve()) != session.get("sqrail_sha256"):
        raise RunnerError("sqrail binary changed after session preparation")
    if sha256(arguments.duckdb.resolve()) != session.get("duckdb_sha256"):
        raise RunnerError("DuckDB binary changed after session preparation")
    if sha256(arguments.tasks.resolve()) != session.get("task_corpus_sha256"):
        raise RunnerError("task corpus changed after session preparation")
    expected_sources = {
        name: sha256(path) for name, path in EVALUATION_SOURCES.items()
    }
    if session.get("evaluation_source_sha256") != expected_sources:
        raise RunnerError("evaluation source changed after session preparation")

    invocations = validate_candidate(candidate, session["arm"], session["task"])
    records: list[dict[str, Any]] = []
    allowed_environment = {
        "PATH",
        "SystemRoot",
        "WINDIR",
        "COMSPEC",
        "PATHEXT",
        "TEMP",
        "TMP",
        "TMPDIR",
        "LD_LIBRARY_PATH",
        "DYLD_LIBRARY_PATH",
    }
    environment = {
        key: value for key, value in os.environ.items() if key in allowed_environment
    }
    environment.update(
        {
            "HOME": str(workspace),
            "USERPROFILE": str(workspace),
            "XDG_CONFIG_HOME": str(workspace / ".config"),
            "LC_ALL": "C",
        }
    )
    for index, invocation in enumerate(invocations, start=1):
        command = resolve_command(invocation["argv"], arguments.sqrail, arguments.duckdb)
        started = time.monotonic()
        try:
            result = subprocess.run(
                command,
                cwd=workspace,
                env=environment,
                input=(invocation.get("stdin") or "").encode("utf-8"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=arguments.command_timeout,
            )
            exit_code = result.returncode
            stdout = result.stdout
            stderr = result.stderr
        except subprocess.TimeoutExpired as error:
            exit_code = 124
            stdout = error.stdout or b""
            stderr = (error.stderr or b"") + b"\nagent-run command timeout\n"
        elapsed = time.monotonic() - started
        stdout_name = f"invocation-{index}.stdout"
        stderr_name = f"invocation-{index}.stderr"
        (artifact / stdout_name).write_bytes(stdout)
        (artifact / stderr_name).write_bytes(stderr)
        records.append(
            {
                "argv": invocation["argv"],
                "exit_code": exit_code,
                "wall_seconds": round(elapsed, 6),
                "stdout": stdout_name,
                "stderr": stderr_name,
                "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
                "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
            }
        )

    record = {
        "schema_version": 1,
        "run_id": session["run_id"],
        "model": session["model"],
        "arm": session["arm"],
        "task": session["task"],
        "attempt": session["attempt"],
        "session_id": candidate["session_id"],
        "before": session["initial_snapshot"],
        "after": snapshot(workspace),
        "invocations": records,
    }
    (artifact / "record.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    tasks = task_map(arguments.tasks)
    indexed = {
        "run_id": session["run_id"],
        "model": session["model"],
        "arm": session["arm"],
        "task": session["task"],
        "attempt": session["attempt"],
        "artifact": artifact_id,
        "session_id": candidate["session_id"],
        "wall_seconds": round(sum(item["wall_seconds"] for item in records), 6),
        "input_tokens": candidate["input_tokens"],
        "output_tokens": candidate["output_tokens"],
    }
    verification = verify_attempt(
        indexed,
        tasks[session["task"]],
        arguments.tasks,
        arguments.artifact_root,
        arguments.data,
        arguments.sqrail,
        arguments.duckdb,
    )
    indexed.update(
        {
            "success": verification.success,
            "exit_code": verification.exit_code,
            "safety_violation": verification.safety_violation,
            "oracle_issues": list(verification.issues),
            "evidence_sha256": verification.evidence_sha256,
        }
    )
    print(json.dumps(indexed, separators=(",", ":"), sort_keys=True))
    return 0


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.command == "prepare":
            return prepare_session(arguments)
        return execute_candidate(arguments)
    except (RunnerError, OracleError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, separators=(",", ":")), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
