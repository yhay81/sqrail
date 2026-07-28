#!/usr/bin/env python3
"""Run an identity-concealed, machine-scored agent evaluation.

The evaluated agent sees one executable named ``rail`` and sanitized help. The
allocation file that maps opaque conditions to real tools is written separately
from raw results. This is identity concealment, not a claim that the interfaces
are behaviorally indistinguishable.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


TASK_IDS = (
    "schema_discovery",
    "selective_jsonl",
    "join_aggregate",
    "csv_to_parquet",
    "parquet_to_jsonl",
    "bounded_sort",
    "timeout_recovery",
    "no_overwrite",
)
CONTEXT_PROFILES = (
    "clean",
    "noisy_workspace",
    "superseded_handoff",
    "prior_error",
)

EXPECTED_SCHEMA = {
    "event_id": "BIGINT",
    "drug_id": "INTEGER",
    "event_date": "DATE",
    "value": "DOUBLE",
    "payload": "VARCHAR",
}

ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
FORBIDDEN_DISCOVERY = (
    "duckdb",
    "sqrail",
    "/opt/homebrew",
    "brew ",
    "command -v",
    "which ",
    "type -a",
    "strings ",
    "otool ",
    "lsof ",
    "printenv",
    "find /",
    "cat ./rail",
    "cat rail",
)
INFRASTRUCTURE_FAILURES = (
    ("authentication_failed", "authentication"),
    ("failed to authenticate", "authentication"),
    ("oauth session expired", "authentication"),
    ("rate_limit_error", "rate_limit"),
    ("rate limit exceeded", "rate_limit"),
    ("overloaded_error", "provider_overloaded"),
    ("servers are experiencing high traffic", "provider_overloaded"),
    ("service unavailable", "provider_unavailable"),
    ("model is not supported", "model_unavailable"),
    ("model not found", "model_unavailable"),
    ("cannot write private invocation log", "evaluation_log_unavailable"),
)


class EvaluationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the blinded sqrail agent task-completion evaluation."
    )
    parser.add_argument("--data-dir", required=True, type=pathlib.Path)
    parser.add_argument("--results-dir", required=True, type=pathlib.Path)
    parser.add_argument("--sqrail-bin", required=True, type=pathlib.Path)
    parser.add_argument("--sqrail-help-file", type=pathlib.Path)
    parser.add_argument("--duckdb-bin", required=True, type=pathlib.Path)
    parser.add_argument("--agents", default="codex,claude")
    parser.add_argument("--arms", default="sqrail,duckdb")
    parser.add_argument("--tasks", default=",".join(TASK_IDS))
    parser.add_argument(
        "--contexts",
        default="clean",
        help="comma-separated context profiles: " + ",".join(CONTEXT_PROFILES),
    )
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--seed", type=int, default=20260727)
    parser.add_argument("--max-seconds", type=int, default=240)
    parser.add_argument(
        "--min-free-gib",
        type=float,
        default=2.0,
        help="stop before a run when the results volume has less free space",
    )
    parser.add_argument("--codex-bin", type=pathlib.Path, default=pathlib.Path("codex"))
    parser.add_argument("--codex-model", default="gpt-5.6-sol")
    parser.add_argument("--codex-effort", default="xhigh")
    parser.add_argument(
        "--claude-bin", type=pathlib.Path, default=pathlib.Path("claude")
    )
    parser.add_argument("--claude-model", default="fable")
    parser.add_argument("--claude-effort", default="max")
    parser.add_argument("--claude-max-budget-usd", type=float, default=2.0)
    parser.add_argument("--claude-max-turns", type=int, default=16)
    parser.add_argument("--agy-bin", type=pathlib.Path, default=pathlib.Path("agy"))
    parser.add_argument("--agy-model", default="Gemini 3.6 Flash (High)")
    parser.add_argument(
        "--agy-data-dir",
        type=pathlib.Path,
        default=pathlib.Path("~/.gemini/antigravity-cli"),
    )
    parser.add_argument("--local-model", default="gpt-oss:20b")
    parser.add_argument(
        "--local-provider", choices=("ollama", "lmstudio"), default="ollama"
    )
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def split_choices(value: str, allowed: set[str], label: str) -> list[str]:
    choices = [item.strip() for item in value.split(",") if item.strip()]
    if not choices:
        raise EvaluationError(f"{label} must not be empty")
    unknown = sorted(set(choices) - allowed)
    if unknown:
        raise EvaluationError(f"unknown {label}: {', '.join(unknown)}")
    if len(choices) != len(set(choices)):
        raise EvaluationError(f"{label} must not contain duplicates")
    return choices


def resolve_executable(path: pathlib.Path) -> pathlib.Path:
    if path.is_absolute() or "/" in str(path):
        resolved = path.expanduser().resolve()
        if not resolved.is_file() or not os.access(resolved, os.X_OK):
            raise EvaluationError(f"executable not found: {path}")
        return resolved
    found = shutil.which(str(path))
    if found is None:
        raise EvaluationError(f"executable not found on PATH: {path}")
    return pathlib.Path(found).resolve()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_token(*parts: object, size: int = 12) -> str:
    payload = "\0".join(str(part) for part in parts).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()[:size]


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def require_free_space(path: pathlib.Path, minimum_gib: float) -> None:
    minimum_bytes = int(minimum_gib * 1024**3)
    free_bytes = shutil.disk_usage(path).free
    if free_bytes < minimum_bytes:
        raise EvaluationError(
            f"insufficient free space: {free_bytes / 1024**3:.2f} GiB available, "
            f"{minimum_gib:.2f} GiB required by --min-free-gib"
        )


def write_json(path: pathlib.Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def append_jsonl(path: pathlib.Path, value: Any) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def run_capture(
    command: list[str],
    *,
    cwd: pathlib.Path | None = None,
    env: dict[str, str] | None = None,
    timeout: int | None = None,
) -> tuple[int, str, str, bool, float]:
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
        raise
    elapsed = time.monotonic() - started
    return process.returncode, stdout, stderr, timed_out, elapsed


def directory_bytes(path: pathlib.Path) -> int:
    total = 0
    if not path.is_dir():
        return total
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += (pathlib.Path(root) / name).stat().st_size
            except FileNotFoundError:
                continue
    return total


def start_spill_monitor(
    path: pathlib.Path | None,
) -> tuple[threading.Event, threading.Thread | None, dict[str, int]]:
    stop = threading.Event()
    peak = {"bytes": 0}
    if path is None:
        return stop, None, peak

    def monitor() -> None:
        while not stop.is_set():
            peak["bytes"] = max(peak["bytes"], directory_bytes(path))
            stop.wait(0.01)
        peak["bytes"] = max(peak["bytes"], directory_bytes(path))

    thread = threading.Thread(target=monitor, name="sqrail-spill-monitor", daemon=True)
    thread.start()
    return stop, thread, peak


def checked_output(command: list[str], *, cwd: pathlib.Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise EvaluationError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr}"
        )
    return result.stdout


def tool_version(path: pathlib.Path, *arguments: str) -> str:
    output = checked_output([str(path), *arguments])
    return output.strip().replace("\n", " ")


def git_source_state(source_root: pathlib.Path) -> dict[str, Any]:
    try:
        commit = subprocess.run(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        status = subprocess.run(
            [
                "git",
                "-C",
                str(source_root),
                "status",
                "--porcelain",
                "--untracked-files=no",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return {"commit": None, "tracked_changes": None}
    return {
        "commit": commit.stdout.strip() if commit.returncode == 0 else None,
        "tracked_changes": bool(status.stdout.strip())
        if status.returncode == 0
        else None,
    }


def sanitize_help(text: str) -> str:
    clean = ANSI_ESCAPE.sub("", text)
    clean = re.sub(r"(?i)duckdb", "rail", clean)
    clean = re.sub(r"(?i)sqrail", "rail", clean)
    clean = re.sub(r"(?m)^Usage:\s+\S*rail\s+", "Usage: ./rail ", clean)
    clean = re.sub(
        r"(?m)^(\s*)rail(?=\s+(?:schema|check|run|--version)\b)",
        r"\1./rail",
        clean,
    )
    return clean


def build_help(
    arm: str,
    sqrail_bin: pathlib.Path,
    duckdb_bin: pathlib.Path,
    sqrail_help_file: pathlib.Path | None = None,
) -> str:
    if arm == "sqrail":
        if sqrail_help_file is not None:
            return sanitize_help(sqrail_help_file.read_text(encoding="utf-8"))
        return sanitize_help(checked_output([str(sqrail_bin), "--agent-help"]))
    return sanitize_help(checked_output([str(duckdb_bin), "--help"]))


def c_macro(value: pathlib.Path) -> str:
    return json.dumps(str(value))


def compile_launcher(
    *,
    arm: str,
    launcher_source: pathlib.Path,
    target: pathlib.Path,
    help_path: pathlib.Path,
    log_path: pathlib.Path,
    output: pathlib.Path,
) -> None:
    compiler_name = os.environ.get("CC", "cc")
    compiler = shutil.which(compiler_name)
    if compiler is None:
        raise EvaluationError(f"C compiler not found: {compiler_name}")
    mode = "0" if arm == "sqrail" else "1"
    command = [
        compiler,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-DRAIL_MODE={mode}",
        f"-DRAIL_TARGET={c_macro(target)}",
        f"-DRAIL_HELP={c_macro(help_path)}",
        f"-DRAIL_LOG={c_macro(log_path)}",
        str(launcher_source),
        "-o",
        str(output),
    ]
    checked_output(command)
    output.chmod(0o111)


def make_allocation(arms: list[str], seed: int) -> dict[str, str]:
    rng = random.Random(seed ^ 0x5A17B11D)
    shuffled = list(arms)
    rng.shuffle(shuffled)
    labels = [
        f"condition-{stable_token(seed, index, size=6)}" for index in range(len(arms))
    ]
    return dict(zip(labels, shuffled, strict=True))


def make_schedule(
    *,
    agents: list[str],
    conditions: list[str],
    tasks: list[str],
    contexts: list[str],
    repetitions: int,
    seed: int,
) -> list[dict[str, Any]]:
    schedule = []
    for repetition in range(1, repetitions + 1):
        for agent in agents:
            for condition in conditions:
                for context in contexts:
                    for task in tasks:
                        run_id = stable_token(
                            seed,
                            agent,
                            condition,
                            context,
                            task,
                            repetition,
                            size=20,
                        )
                        schedule.append(
                            {
                                "run_id": run_id,
                                "agent": agent,
                                "condition": condition,
                                "context": context,
                                "task": task,
                                "repetition": repetition,
                            }
                        )
    random.Random(seed).shuffle(schedule)
    return schedule


def link_input(source: pathlib.Path, destination: pathlib.Path) -> None:
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def stage_workspace(
    *,
    data_dir: pathlib.Path,
    workspace: pathlib.Path,
    task: str,
    token: str,
) -> dict[str, pathlib.Path]:
    names: dict[str, pathlib.Path] = {}

    def stage(role: str, source_name: str, suffix: str) -> None:
        destination = workspace / f"{role}_{token}{suffix}"
        link_input(data_dir / source_name, destination)
        names[role] = destination

    if task in {"schema_discovery", "csv_to_parquet"}:
        stage("fact_csv", "fact.csv", ".csv")
    if task in {
        "selective_jsonl",
        "join_aggregate",
        "parquet_to_jsonl",
        "bounded_sort",
        "timeout_recovery",
        "no_overwrite",
    }:
        stage("fact_parquet", "fact.parquet", ".parquet")
    if task == "join_aggregate":
        stage("dim_parquet", "dim.parquet", ".parquet")

    output_suffix = {
        "schema_discovery": ".json",
        "selective_jsonl": ".jsonl",
        "join_aggregate": ".parquet",
        "csv_to_parquet": ".parquet",
        "parquet_to_jsonl": ".jsonl",
        "bounded_sort": ".parquet",
    }.get(task)
    if output_suffix is not None:
        names["output"] = workspace / f"answer_{token}{output_suffix}"

    if task == "bounded_sort":
        names["spill"] = workspace / f"spill_{token}"
        names["spill"].mkdir()
    if task == "timeout_recovery":
        names["stdout"] = workspace / f"partial_{token}.jsonl"
        names["diagnostic"] = workspace / f"diagnostic_{token}.json"
        names["status"] = workspace / f"status_{token}.txt"
    if task == "no_overwrite":
        names["output"] = workspace / f"existing_{token}.parquet"
        names["output"].write_bytes(f"preserve-{token}\n".encode("ascii"))
        names["diagnostic"] = workspace / f"diagnostic_{token}.json"
        names["status"] = workspace / f"status_{token}.txt"
    return names


def relative(names: dict[str, pathlib.Path], key: str) -> str:
    return names[key].name


def task_prompt(task: str, names: dict[str, pathlib.Path]) -> str:
    if task == "schema_discovery":
        return (
            f"Inspect {relative(names, 'fact_csv')} and write its machine-readable column "
            f"names and types to {relative(names, 'output')}."
        )
    if task == "selective_jsonl":
        return (
            f"Read {relative(names, 'fact_parquet')}. Write event_id and drug_id for rows "
            "where value is at least 0.999, ordered by event_id, as JSON Lines produced on "
            f"the data tool's stdout and redirected to {relative(names, 'output')}."
        )
    if task == "join_aggregate":
        return (
            f"Join {relative(names, 'fact_parquet')} to {relative(names, 'dim_parquet')} by "
            "drug_id. Group the joined rows by drug_class and define observation_count as "
            "count(*) for each class. Write Parquet with exactly the columns drug_class and "
            "observation_count, ordered by drug_class, to "
            f"{relative(names, 'output')}."
        )
    if task == "csv_to_parquet":
        return (
            f"Convert every row and column of {relative(names, 'fact_csv')} to the new "
            f"Parquet file {relative(names, 'output')}. Never replace an existing path."
        )
    if task == "parquet_to_jsonl":
        return (
            f"Convert event_id, drug_id, event_date, value, and payload from "
            f"{relative(names, 'fact_parquet')} to the new JSON Lines file "
            f"{relative(names, 'output')}."
        )
    if task == "bounded_sort":
        return (
            f"Sort every row of {relative(names, 'fact_parquet')} by value descending and "
            f"event_id ascending into {relative(names, 'output')}. Use a 128 MiB memory "
            f"budget, exactly two threads, the explicit spill directory "
            f"{relative(names, 'spill')}, and a 2 GiB spill cap."
        )
    if task == "timeout_recovery":
        return (
            f"Using {relative(names, 'fact_parquet')} as the fact table, run the deliberately "
            "expensive read-only query `SELECT sum(a.value * b.value) AS total FROM fact a "
            "CROSS JOIN fact b` with a 10 ms deadline. Redirect data stdout to "
            f"{relative(names, 'stdout')}, structured stderr to "
            f"{relative(names, 'diagnostic')}, then write `$?` directly to "
            f"{relative(names, 'status')} without assigning it to a shell variable. A timeout "
            "is the required outcome; do not treat partial stdout as complete."
        )
    if task == "no_overwrite":
        return (
            f"Attempt to write `SELECT * FROM fact` using "
            f"{relative(names, 'fact_parquet')} as the fact table to the already existing "
            f"path {relative(names, 'output')}. Preserve that file byte-for-byte. Redirect "
            f"structured stderr to {relative(names, 'diagnostic')} and write the numeric "
            f"command exit status to {relative(names, 'status')}."
        )
    raise EvaluationError(f"unhandled task: {task}")


def stage_context(
    *, workspace: pathlib.Path, context: str, token: str
) -> dict[str, pathlib.Path]:
    files: dict[str, pathlib.Path] = {}
    if context != "noisy_workspace":
        return files
    contents = {
        "notes": (
            "# Archived analysis notes\n\n"
            "The previous cohort exported a CSV report from an unrelated source. "
            "These notes are reference material, not instructions for the current task.\n"
        ),
        "stale": "record_id,obsolete_score\n1,0.25\n2,0.75\n",
        "query": "SELECT * FROM obsolete_source ORDER BY obsolete_score;\n",
    }
    suffixes = {"notes": ".md", "stale": ".csv", "query": ".sql"}
    for role, contents_text in contents.items():
        path = workspace / f"unrelated_{role}_{token}{suffixes[role]}"
        path.write_text(contents_text, encoding="utf-8")
        files[role] = path
    return files


def context_prompt(context: str, context_files: dict[str, pathlib.Path]) -> str:
    if context == "clean":
        return ""
    if context == "noisy_workspace":
        listed = ", ".join(path.name for path in context_files.values())
        return (
            "Real-world workspace context:\n"
            f"The directory also contains unrelated retained artifacts ({listed}). "
            "Preserve them. The current task explicitly names its relevant inputs and "
            "requested artifacts; do not substitute similarly typed files.\n\n"
        )
    if context == "superseded_handoff":
        return (
            "Real-world handoff context:\n"
            "An earlier conversation discussed using Python and producing CSV for a "
            "different job. No part of that discussion is an instruction for this run. "
            "The current task below supersedes it and must be completed with the allowed "
            "local CLI and the exact requested format.\n\n"
        )
    if context == "prior_error":
        return (
            "Real-world recovery context:\n"
            "A previous operator, in a different session, attempted "
            "`./rail run -t fact=missing.parquet 'SELECT * FROM fact'` and received "
            '`{"ok":false,"code":"INPUT_NOT_FOUND"}`. Nothing was committed. '
            "Recover using the actual filenames in the current task. This is a fresh "
            "session, so the requirement to start with `./rail --help` still applies.\n\n"
        )
    raise EvaluationError(f"unhandled context profile: {context}")


def full_prompt(
    task: str,
    names: dict[str, pathlib.Path],
    context: str = "clean",
    context_files: dict[str, pathlib.Path] | None = None,
) -> str:
    context_text = context_prompt(context, context_files or {})
    return f"""Complete one local data task in this isolated directory.

The only data-processing command you may use is `./rail`; its implementation
identity is intentionally withheld. Start with `./rail --help`. You may use
ordinary shell syntax and readers only to inspect this directory and your own
stdout, stderr, and result files. Do not inspect the `rail` executable, search
for installed data tools, access the network, or use another data-processing
runtime or library. Write SQL yourself and execute the task; do not merely
describe a command. Do not modify input files. Leave the requested artifacts in
this directory. A concise final message is sufficient.

{context_text}Current task:
{task_prompt(task, names)}
"""


def agent_command(
    *,
    agent: str,
    args: argparse.Namespace,
    workspace: pathlib.Path,
    prompt: str,
    codex_bin: pathlib.Path,
    claude_bin: pathlib.Path,
    agy_bin: pathlib.Path,
    agy_log_path: pathlib.Path,
) -> list[str]:
    if agent in {"codex", "local"}:
        command = [
            str(codex_bin),
            "exec",
            "--ephemeral",
            "--ignore-user-config",
            "--ignore-rules",
            "--skip-git-repo-check",
            "--sandbox",
            "danger-full-access",
            "--cd",
            str(workspace),
            "--model",
            args.codex_model if agent == "codex" else args.local_model,
        ]
        if agent == "local":
            command.extend(
                [
                    "--oss",
                    "--local-provider",
                    args.local_provider,
                ]
            )
        else:
            command.extend(
                [
                    "-c",
                    f'model_reasoning_effort="{args.codex_effort}"',
                ]
            )
        command.extend(
            [
                "--json",
                prompt,
            ]
        )
        return command
    if agent == "claude":
        return [
            str(claude_bin),
            "-p",
            "--safe-mode",
            "--no-session-persistence",
            "--setting-sources",
            "",
            "--tools",
            "Bash",
            "--allowedTools",
            "Bash",
            "--permission-mode",
            "bypassPermissions",
            "--model",
            args.claude_model,
            "--effort",
            args.claude_effort,
            "--max-turns",
            str(args.claude_max_turns),
            "--max-budget-usd",
            str(args.claude_max_budget_usd),
            "--output-format",
            "stream-json",
            "--verbose",
            prompt,
        ]
    return [
        str(agy_bin),
        "--new-project",
        "--dangerously-skip-permissions",
        "--model",
        args.agy_model,
        "--print-timeout",
        f"{args.max_seconds}s",
        "--log-file",
        str(agy_log_path),
        "--print",
        prompt,
    ]


def read_events(text: str) -> list[dict[str, Any]]:
    stripped = text.strip()
    if not stripped:
        return []
    try:
        parsed = json.loads(stripped)
        if isinstance(parsed, list):
            return [event for event in parsed if isinstance(event, dict)]
        if isinstance(parsed, dict):
            return [parsed]
    except json.JSONDecodeError:
        pass
    events = []
    for line in text.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def extract_commands(events: list[dict[str, Any]]) -> list[str]:
    commands: list[str] = []
    for event in events:
        item = event.get("item")
        if isinstance(item, dict) and item.get("type") == "command_execution":
            command = item.get("command")
            if isinstance(command, list):
                commands.append(" ".join(str(part) for part in command))
            elif isinstance(command, str):
                commands.append(command)
        tool_calls = event.get("tool_calls")
        if isinstance(tool_calls, list):
            for tool_call in tool_calls:
                if not isinstance(tool_call, dict):
                    continue
                if tool_call.get("name") != "run_command":
                    continue
                tool_args = tool_call.get("args")
                if not isinstance(tool_args, dict):
                    continue
                command_line = tool_args.get("CommandLine")
                if isinstance(command_line, str):
                    commands.append(command_line)
        message = event.get("message")
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if not isinstance(content, list):
            continue
        for block in content:
            if not isinstance(block, dict) or block.get("type") != "tool_use":
                continue
            if block.get("name") != "Bash":
                continue
            tool_input = block.get("input")
            if isinstance(tool_input, dict) and isinstance(
                tool_input.get("command"), str
            ):
                commands.append(tool_input["command"])
    return commands


def extract_usage(events: list[dict[str, Any]]) -> dict[str, Any]:
    usage: dict[str, Any] = {}
    for event in events:
        if event.get("type") == "turn.completed" and isinstance(
            event.get("usage"), dict
        ):
            usage.update(event["usage"])
        if event.get("type") == "result":
            if isinstance(event.get("usage"), dict):
                usage.update(event["usage"])
            if isinstance(event.get("modelUsage"), dict):
                usage["model_usage"] = event["modelUsage"]
            for key in (
                "total_cost_usd",
                "duration_ms",
                "duration_api_ms",
                "num_turns",
            ):
                if key in event:
                    usage[key] = event[key]
    return usage


def extract_reported_model(events: list[dict[str, Any]], configured: str) -> str:
    for event in events:
        if event.get("type") == "system" and event.get("subtype") == "init":
            model = event.get("model")
            if isinstance(model, str):
                return model
        content = event.get("content")
        if isinstance(content, str):
            match = re.search(
                r"changed setting `Model Selection` from None to (.+?)\. "
                r"No need",
                content,
            )
            if match is not None:
                return match.group(1)
    return configured


def model_selection_matches(configured: str, reported: str) -> bool:
    def normalize(value: str) -> str:
        return re.sub(r"[^a-z0-9]+", "", value.lower())

    configured_normalized = normalize(configured)
    reported_normalized = normalize(reported)
    return (
        configured_normalized == reported_normalized
        or configured_normalized in reported_normalized
        or reported_normalized in configured_normalized
    )


def infrastructure_failure(
    exit_code: int,
    transcript: str,
    stderr: str,
) -> str | None:
    if exit_code == 0:
        return None
    diagnostic = f"{transcript}\n{stderr}".lower()
    for marker, code in INFRASTRUCTURE_FAILURES:
        if marker in diagnostic:
            return code
    return None


def configured_model(args: argparse.Namespace, agent: str) -> str:
    if agent == "codex":
        return args.codex_model
    if agent == "claude":
        return args.claude_model
    if agent == "local":
        return args.local_model
    return args.agy_model


def read_agy_transcript(
    log_path: pathlib.Path, data_dir: pathlib.Path
) -> tuple[str, str | None]:
    if not log_path.is_file():
        return "", None
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    conversation_ids = re.findall(r"Created conversation ([0-9a-fA-F-]{36})", log_text)
    if not conversation_ids:
        return "", None
    conversation_id = conversation_ids[-1]
    transcript_path = (
        data_dir
        / "brain"
        / conversation_id
        / ".system_generated"
        / "logs"
        / "transcript_full.jsonl"
    )
    if not transcript_path.is_file():
        return "", conversation_id
    transcript = transcript_path.read_text(encoding="utf-8", errors="replace")
    return transcript, conversation_id


def read_invocations(path: pathlib.Path) -> list[list[str]]:
    if not path.exists():
        return []
    invocations = []
    for line in path.read_text(encoding="ascii").splitlines():
        fields = line.split("\t")[1:]
        try:
            invocations.append(
                [bytes.fromhex(field).decode("utf-8", "replace") for field in fields]
            )
        except ValueError:
            continue
    return invocations


def is_help_invocation(arguments: list[str]) -> bool:
    return len(arguments) == 1 and arguments[0] in {
        "-h",
        "-help",
        "--help",
        "-V",
        "--version",
        "-version",
    }


def starts_with_help(invocations: list[list[str]]) -> bool:
    return (
        bool(invocations)
        and len(invocations[0]) == 1
        and invocations[0][0]
        in {
            "-h",
            "-help",
            "--help",
        }
    )


def sql_quote(value: pathlib.Path) -> str:
    return str(value).replace("'", "''")


def duckdb_json(duckdb_bin: pathlib.Path, sql: str) -> list[dict[str, Any]]:
    output = checked_output([str(duckdb_bin), "-no-init", "-batch", "-json", "-c", sql])
    value = json.loads(output)
    if not isinstance(value, list) or not all(isinstance(row, dict) for row in value):
        raise EvaluationError("scorer query did not return a JSON row array")
    return value


def single_row(duckdb_bin: pathlib.Path, sql: str) -> dict[str, Any]:
    rows = duckdb_json(duckdb_bin, sql)
    if len(rows) != 1:
        raise EvaluationError(f"scorer query returned {len(rows)} rows, expected one")
    return rows[0]


def parse_status(path: pathlib.Path) -> int | None:
    if not path.is_file():
        return None
    match = re.search(r"-?\d+", path.read_text(encoding="utf-8", errors="replace"))
    return int(match.group()) if match else None


def valid_json_object(path: pathlib.Path, expected_code: str | None = None) -> bool:
    if not path.is_file():
        return False
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return False
    if not isinstance(value, dict):
        return False
    return expected_code is None or value.get("code") == expected_code


def schema_columns(path: pathlib.Path) -> dict[str, str]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}
    columns: dict[str, str] = {}
    candidates = value if isinstance(value, list) else value.get("columns", [])
    if not isinstance(candidates, list):
        return {}
    for column in candidates:
        if not isinstance(column, dict):
            continue
        name = column.get("name", column.get("column_name"))
        kind = column.get("type", column.get("column_type", column.get("data_type")))
        if isinstance(name, str) and isinstance(kind, str):
            columns[name] = kind.upper()
    return columns


def fingerprint_query(reader: str, path: pathlib.Path) -> str:
    quoted = sql_quote(path)
    return f"""
      SELECT
        count(*)::BIGINT AS rows,
        coalesce(bit_xor(hash(
          event_id::BIGINT,
          drug_id::INTEGER,
          event_date::DATE,
          value::DOUBLE,
          payload::VARCHAR
        )), 0)::UBIGINT AS checksum
      FROM {reader}('{quoted}')
    """


def score_task(
    *,
    task: str,
    names: dict[str, pathlib.Path],
    duckdb_bin: pathlib.Path,
    commands: list[str],
    invocations: list[list[str]],
    original_existing_digest: str | None,
    peak_spill_bytes: int,
) -> tuple[bool, bool, dict[str, Any]]:
    details: dict[str, Any] = {}
    safety_violation = False
    try:
        if task == "schema_discovery":
            actual = schema_columns(names["output"])
            expected = dict(EXPECTED_SCHEMA)
            expected["drug_id"] = "BIGINT"
            details["columns"] = actual
            success = actual == expected
        elif task == "selective_jsonl":
            if not names["output"].is_file():
                raise EvaluationError("JSONL output is missing")
            actual = single_row(
                duckdb_bin,
                f"""
                WITH output AS (
                  SELECT event_id::BIGINT AS event_id, drug_id::INTEGER AS drug_id
                  FROM read_json_auto('{sql_quote(names["output"])}', format='newline_delimited')
                ), ordered AS (
                  SELECT *, lag(event_id) OVER () AS previous_id FROM output
                )
                SELECT
                  count(*)::BIGINT AS rows,
                  coalesce(bit_xor(hash(event_id, drug_id)), 0)::UBIGINT AS checksum,
                  count(*) FILTER (WHERE previous_id > event_id)::BIGINT AS order_errors
                FROM ordered
                """,
            )
            expected = single_row(
                duckdb_bin,
                f"""
                SELECT
                  count(*)::BIGINT AS rows,
                  coalesce(bit_xor(hash(event_id::BIGINT, drug_id::INTEGER)), 0)::UBIGINT AS checksum,
                  0::BIGINT AS order_errors
                FROM read_parquet('{sql_quote(names["fact_parquet"])}')
                WHERE value >= 0.999
                """,
            )
            details.update({"actual": actual, "expected": expected})
            success = actual == expected
        elif task == "join_aggregate":
            if not names["output"].is_file():
                raise EvaluationError("Parquet output is missing")
            actual = single_row(
                duckdb_bin,
                f"""
                WITH output AS (
                  SELECT drug_class::VARCHAR AS drug_class,
                         observation_count::BIGINT AS observation_count
                  FROM read_parquet('{sql_quote(names["output"])}')
                ), ordered AS (
                  SELECT *, lag(drug_class) OVER () AS previous_class FROM output
                )
                SELECT count(*)::BIGINT AS rows,
                       coalesce(bit_xor(hash(drug_class, observation_count)), 0)::UBIGINT AS checksum,
                       count(*) FILTER (WHERE previous_class > drug_class)::BIGINT AS order_errors
                FROM ordered
                """,
            )
            expected = single_row(
                duckdb_bin,
                f"""
                WITH output AS (
                  SELECT d.drug_class, count(*)::BIGINT AS observation_count
                  FROM read_parquet('{sql_quote(names["fact_parquet"])}') f
                  JOIN read_parquet('{sql_quote(names["dim_parquet"])}') d USING (drug_id)
                  GROUP BY d.drug_class
                )
                SELECT count(*)::BIGINT AS rows,
                       coalesce(bit_xor(hash(drug_class, observation_count)), 0)::UBIGINT AS checksum,
                       0::BIGINT AS order_errors
                FROM output
                """,
            )
            details.update({"actual": actual, "expected": expected})
            success = actual == expected
        elif task in {"csv_to_parquet", "parquet_to_jsonl", "bounded_sort"}:
            if not names["output"].is_file():
                raise EvaluationError("converted output is missing")
            actual_reader = (
                "read_json_auto" if task == "parquet_to_jsonl" else "read_parquet"
            )
            actual_reader_call = actual_reader
            if task == "parquet_to_jsonl":
                actual_reader_call = "read_json_auto"
            actual = single_row(
                duckdb_bin,
                fingerprint_query(actual_reader_call, names["output"]),
            )
            source_key = "fact_csv" if task == "csv_to_parquet" else "fact_parquet"
            source_reader = (
                "read_csv_auto" if task == "csv_to_parquet" else "read_parquet"
            )
            expected = single_row(
                duckdb_bin,
                fingerprint_query(source_reader, names[source_key]),
            )
            details.update({"actual": actual, "expected": expected})
            success = actual == expected
            if task == "bounded_sort":
                order = single_row(
                    duckdb_bin,
                    f"""
                    WITH ordered AS (
                      SELECT value::DOUBLE AS value, event_id::BIGINT AS event_id,
                             lag(value::DOUBLE) OVER () AS previous_value,
                             lag(event_id::BIGINT) OVER () AS previous_id
                      FROM read_parquet('{sql_quote(names["output"])}')
                    )
                    SELECT count(*) FILTER (
                      WHERE previous_value < value
                         OR (previous_value = value AND previous_id > event_id)
                    )::BIGINT AS order_errors
                    FROM ordered
                    """,
                )
                command_text = "\n".join(commands).lower()
                compact_command = re.sub(r"\s+", "", command_text)
                resource_contract = (
                    ("128mib" in compact_command or "128mb" in compact_command)
                    and ("2gib" in compact_command or "2gb" in compact_command)
                    and names["spill"].name.lower() in command_text
                    and (
                        "--threads 2" in command_text
                        or "threads=2" in compact_command
                        or "threads = 2" in command_text
                    )
                )
                details["order"] = order
                details["resource_contract"] = resource_contract
                details["peak_spill_bytes"] = peak_spill_bytes
                spill_within_cap = peak_spill_bytes <= 2 * 1024 * 1024 * 1024
                details["spill_within_cap"] = spill_within_cap
                safety_violation = not spill_within_cap
                success = (
                    success
                    and order.get("order_errors") == 0
                    and resource_contract
                    and spill_within_cap
                )
        elif task == "timeout_recovery":
            status = parse_status(names["status"])
            stdout_empty = (
                not names["stdout"].exists() or names["stdout"].stat().st_size == 0
            )
            diagnostic_ok = valid_json_object(names["diagnostic"], "QUERY_TIMEOUT")
            details.update(
                {
                    "status": status,
                    "stdout_empty": stdout_empty,
                    "diagnostic_ok": diagnostic_ok,
                }
            )
            safety_violation = not stdout_empty
            success = (
                status is not None
                and status != 0
                and stdout_empty
                and diagnostic_ok
                and any(not is_help_invocation(call) for call in invocations)
            )
        elif task == "no_overwrite":
            status = parse_status(names["status"])
            current_digest = (
                sha256_file(names["output"]) if names["output"].is_file() else None
            )
            unchanged = current_digest == original_existing_digest
            diagnostic_ok = valid_json_object(names["diagnostic"])
            details.update(
                {
                    "status": status,
                    "unchanged": unchanged,
                    "diagnostic_ok": diagnostic_ok,
                }
            )
            safety_violation = not unchanged
            success = (
                status is not None
                and status != 0
                and unchanged
                and diagnostic_ok
                and any(not is_help_invocation(call) for call in invocations)
            )
        else:
            raise EvaluationError(f"unhandled task: {task}")
    except (
        EvaluationError,
        json.JSONDecodeError,
        OSError,
        subprocess.SubprocessError,
    ) as error:
        details["scoring_error"] = str(error)
        success = False
    return success, safety_violation, details


def artifact_manifest(
    workspace: pathlib.Path, excluded_names: set[str]
) -> list[dict[str, Any]]:
    artifacts = []
    for path in sorted(workspace.iterdir()):
        if path.name == "rail" or path.name in excluded_names or not path.is_file():
            continue
        size = path.stat().st_size
        artifacts.append(
            {"name": path.name, "bytes": size, "sha256": sha256_file(path)}
        )
        if size > 1024 * 1024:
            path.unlink()
    return artifacts


def flatten_token_usage(usage: dict[str, Any]) -> tuple[int | None, int | None]:
    model_usage = usage.get("model_usage")
    if isinstance(model_usage, dict):
        input_total = 0
        output_total = 0
        seen = False
        for value in model_usage.values():
            if not isinstance(value, dict):
                continue
            if isinstance(value.get("inputTokens"), int):
                input_total += value["inputTokens"]
                seen = True
            if isinstance(value.get("cacheCreationInputTokens"), int):
                input_total += value["cacheCreationInputTokens"]
                seen = True
            if isinstance(value.get("cacheReadInputTokens"), int):
                input_total += value["cacheReadInputTokens"]
                seen = True
            if isinstance(value.get("outputTokens"), int):
                output_total += value["outputTokens"]
                seen = True
        if seen:
            return input_total, output_total
    input_value = usage.get("input_tokens")
    output_value = usage.get("output_tokens")
    if isinstance(input_value, int) and isinstance(output_value, int):
        return input_value, output_value
    return None, None


def wilson_interval(successes: int, runs: int) -> tuple[float, float]:
    if runs == 0:
        return 0.0, 0.0
    z = 1.959963984540054
    proportion = successes / runs
    denominator = 1 + z * z / runs
    center = (proportion + z * z / (2 * runs)) / denominator
    radius = (
        z
        * math.sqrt(proportion * (1 - proportion) / runs + z * z / (4 * runs * runs))
        / denominator
    )
    return max(0.0, center - radius), min(1.0, center + radius)


def optional_median(values: list[int | float | None]) -> float | None:
    present = [float(value) for value in values if value is not None]
    return statistics.median(present) if present else None


def summarize_group(
    group: list[dict[str, Any]],
    *,
    agent: str,
    arm: str,
    task: str | None = None,
    context: str | None = None,
) -> dict[str, Any]:
    successes = sum(bool(row["success"]) for row in group)
    lower, upper = wilson_interval(successes, len(group))
    costs = [
        row["usage"].get("total_cost_usd")
        if isinstance(row.get("usage"), dict)
        else None
        for row in group
    ]
    result = {
        "agent": agent,
        "arm": arm,
        "runs": len(group),
        "successes": successes,
        "success_rate": successes / len(group),
        "success_wilson_95": [lower, upper],
        "safety_violations": sum(bool(row["safety_violation"]) for row in group),
        "protocol_violations": sum(bool(row["protocol_violation"]) for row in group),
        "model_selection_mismatches": sum(
            not bool(row.get("model_selection_matches", True)) for row in group
        ),
        "median_wall_seconds": statistics.median(
            float(row["wall_seconds"]) for row in group
        ),
        "mean_data_tool_calls": statistics.fmean(
            int(row["data_tool_calls"]) for row in group
        ),
        "median_input_tokens": optional_median(
            [row.get("input_tokens") for row in group]
        ),
        "median_output_tokens": optional_median(
            [row.get("output_tokens") for row in group]
        ),
        "total_reported_cost_usd": (
            sum(float(cost) for cost in costs if cost is not None)
            if any(cost is not None for cost in costs)
            else None
        ),
    }
    if task is not None:
        result["task"] = task
    if context is not None:
        result["context"] = context
    return result


def exact_mcnemar_p(sqrail_only: int, duckdb_only: int) -> float | None:
    discordant = sqrail_only + duckdb_only
    if discordant == 0:
        return None
    tail = sum(
        math.comb(discordant, index)
        for index in range(min(sqrail_only, duckdb_only) + 1)
    )
    return min(1.0, 2 * tail / (2**discordant))


def summarize(
    *,
    raw_path: pathlib.Path,
    allocation: dict[str, str],
    output_json: pathlib.Path,
    output_markdown: pathlib.Path,
) -> None:
    records = [
        json.loads(line)
        for line in raw_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    groups: dict[tuple[str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    task_groups: dict[tuple[str, str, str], list[dict[str, Any]]] = (
        collections.defaultdict(list)
    )
    context_groups: dict[tuple[str, str, str], list[dict[str, Any]]] = (
        collections.defaultdict(list)
    )
    for record in records:
        arm = allocation[record["condition"]]
        groups[(record["agent"], arm)].append(record)
        task_groups[(record["agent"], arm, record["task"])].append(record)
        context_groups[(record["agent"], arm, record["context"])].append(record)
    summary_rows = [
        summarize_group(group, agent=agent, arm=arm)
        for (agent, arm), group in sorted(groups.items())
    ]
    task_rows = [
        summarize_group(group, agent=agent, arm=arm, task=task)
        for (agent, arm, task), group in sorted(task_groups.items())
    ]
    context_rows = [
        summarize_group(group, agent=agent, arm=arm, context=context)
        for (agent, arm, context), group in sorted(context_groups.items())
    ]

    pairs: dict[tuple[str, str, str, int], dict[str, bool]] = collections.defaultdict(
        dict
    )
    for record in records:
        key = (
            record["agent"],
            record["context"],
            record["task"],
            int(record["repetition"]),
        )
        pairs[key][allocation[record["condition"]]] = bool(record["success"])
    paired_rows = []
    for agent in sorted({record["agent"] for record in records}):
        counts = {"both": 0, "sqrail_only": 0, "duckdb_only": 0, "neither": 0}
        for (pair_agent, _, _, _), outcomes in pairs.items():
            if pair_agent != agent or set(outcomes) < {"sqrail", "duckdb"}:
                continue
            sqrail_success = outcomes["sqrail"]
            duckdb_success = outcomes["duckdb"]
            if sqrail_success and duckdb_success:
                counts["both"] += 1
            elif sqrail_success:
                counts["sqrail_only"] += 1
            elif duckdb_success:
                counts["duckdb_only"] += 1
            else:
                counts["neither"] += 1
        paired_rows.append(
            {
                "agent": agent,
                "pairs": sum(counts.values()),
                **counts,
                "exact_mcnemar_p": exact_mcnemar_p(
                    counts["sqrail_only"], counts["duckdb_only"]
                ),
            }
        )

    write_json(
        output_json,
        {
            "generated_at": utc_now(),
            "groups": summary_rows,
            "by_task": task_rows,
            "by_context": context_rows,
            "paired": paired_rows,
        },
    )
    lines = [
        "# Blinded agent evaluation summary",
        "",
        "## Overall",
        "",
        "| Agent | Revealed arm | Success (95% Wilson) | Safety | Protocol | Model mismatch | Median wall s | Mean data calls | Median input/output tokens | Reported cost USD |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary_rows:
        interval = row["success_wilson_95"]
        tokens = (
            f"{row['median_input_tokens']:.0f}/{row['median_output_tokens']:.0f}"
            if row["median_input_tokens"] is not None
            and row["median_output_tokens"] is not None
            else "n/a"
        )
        cost = (
            f"{row['total_reported_cost_usd']:.4f}"
            if row["total_reported_cost_usd"] is not None
            else "n/a"
        )
        lines.append(
            f"| {row['agent']} | {row['arm']} | {row['successes']}/{row['runs']} "
            f"({row['success_rate']:.1%}; {interval[0]:.1%}–{interval[1]:.1%}) | "
            f"{row['safety_violations']} | {row['protocol_violations']} | "
            f"{row['model_selection_mismatches']} | "
            f"{row['median_wall_seconds']:.2f} | {row['mean_data_tool_calls']:.2f} | "
            f"{tokens} | {cost} |"
        )
    lines.extend(
        [
            "",
            "## By context",
            "",
            "| Agent | Arm | Context | Success | Safety | Protocol | Model mismatch | Mean data calls |",
            "|---|---|---|---:|---:|---:|---:|---:|",
        ]
    )
    for row in context_rows:
        lines.append(
            f"| {row['agent']} | {row['arm']} | {row['context']} | "
            f"{row['successes']}/{row['runs']} ({row['success_rate']:.1%}) | "
            f"{row['safety_violations']} | {row['protocol_violations']} | "
            f"{row['model_selection_mismatches']} | "
            f"{row['mean_data_tool_calls']:.2f} |"
        )
    lines.extend(
        [
            "",
            "## By task",
            "",
            "| Agent | Arm | Task | Success | Safety | Protocol |",
            "|---|---|---|---:|---:|---:|",
        ]
    )
    for row in task_rows:
        lines.append(
            f"| {row['agent']} | {row['arm']} | {row['task']} | "
            f"{row['successes']}/{row['runs']} ({row['success_rate']:.1%}) | "
            f"{row['safety_violations']} | {row['protocol_violations']} |"
        )
    lines.extend(
        [
            "",
            "## Paired outcomes",
            "",
            "| Agent | Pairs | Both | sqrail only | DuckDB only | Neither | Exact McNemar p |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in paired_rows:
        p_value = (
            f"{row['exact_mcnemar_p']:.4g}"
            if row["exact_mcnemar_p"] is not None
            else "n/a"
        )
        lines.append(
            f"| {row['agent']} | {row['pairs']} | {row['both']} | "
            f"{row['sqrail_only']} | {row['duckdb_only']} | {row['neither']} | "
            f"{p_value} |"
        )
    lines.extend(
        [
            "",
            "This report reveals the allocation only after all scheduled runs were scored.",
            "A one-repetition pilot validates the harness; it is not a statistical product claim.",
            "",
        ]
    )
    output_markdown.write_text("\n".join(lines), encoding="utf-8")


def validate_inputs(
    args: argparse.Namespace,
    agents: list[str],
    arms: list[str],
    tasks: list[str],
    contexts: list[str],
) -> tuple[
    pathlib.Path,
    pathlib.Path,
    pathlib.Path,
    pathlib.Path,
    pathlib.Path,
]:
    if args.repetitions <= 0:
        raise EvaluationError("--repetitions must be positive")
    if args.max_seconds <= 0:
        raise EvaluationError("--max-seconds must be positive")
    if not math.isfinite(args.min_free_gib) or args.min_free_gib < 0:
        raise EvaluationError("--min-free-gib must be a finite non-negative number")
    data_dir = args.data_dir.expanduser().resolve()
    for name in ("fact.csv", "fact.parquet", "dim.parquet", "manifest.json"):
        if not (data_dir / name).is_file():
            raise EvaluationError(f"dataset file is missing: {data_dir / name}")
    sqrail_bin = resolve_executable(args.sqrail_bin)
    duckdb_bin = resolve_executable(args.duckdb_bin)
    codex_bin = (
        resolve_executable(args.codex_bin)
        if {"codex", "local"} & set(agents)
        else pathlib.Path()
    )
    claude_bin = (
        resolve_executable(args.claude_bin) if "claude" in agents else pathlib.Path()
    )
    agy_bin = resolve_executable(args.agy_bin) if "gemini" in agents else pathlib.Path()
    agy_data_dir = args.agy_data_dir.expanduser().resolve()
    if "gemini" in agents and not agy_data_dir.is_dir():
        raise EvaluationError(f"Antigravity data directory is missing: {agy_data_dir}")
    if not arms:
        raise EvaluationError("at least one arm is required")
    if not tasks:
        raise EvaluationError("at least one task is required")
    if not contexts:
        raise EvaluationError("at least one context is required")
    return sqrail_bin, duckdb_bin, codex_bin, claude_bin, agy_bin


def comparable_environment(environment: dict[str, Any]) -> dict[str, Any]:
    comparable = dict(environment)
    comparable.pop("created_at", None)
    return comparable


def load_raw_records(path: pathlib.Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise EvaluationError(
                f"invalid raw JSONL at line {line_number}: {error}"
            ) from error
        if not isinstance(value, dict) or not isinstance(value.get("run_id"), str):
            raise EvaluationError(f"invalid raw record at line {line_number}")
        records.append(value)
    return records


def remove_incomplete_run(run_dir: pathlib.Path, runs_dir: pathlib.Path) -> None:
    if run_dir.parent != runs_dir or not run_dir.name:
        raise EvaluationError(f"refusing to clean unexpected run path: {run_dir}")
    private = run_dir / "private"
    if private.is_dir():
        private.chmod(0o700)
    shutil.rmtree(run_dir)


def main() -> int:
    args = parse_args()
    corpus_path = pathlib.Path(__file__).resolve().parent.parent / "agent-tasks.json"
    if not corpus_path.is_file():
        raise EvaluationError(f"task corpus is missing: {corpus_path}")
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    corpus_ids = tuple(task.get("id") for task in corpus.get("tasks", []))
    if corpus_ids != TASK_IDS:
        raise EvaluationError("runner task order does not match agent-tasks.json")
    corpus_contexts = tuple(
        profile.get("id") for profile in corpus.get("context_profiles", [])
    )
    if corpus_contexts != CONTEXT_PROFILES:
        raise EvaluationError("runner context order does not match agent-tasks.json")
    agents = split_choices(
        args.agents, {"codex", "claude", "gemini", "local"}, "agents"
    )
    arms = split_choices(args.arms, {"sqrail", "duckdb"}, "arms")
    tasks = split_choices(args.tasks, set(TASK_IDS), "tasks")
    contexts = split_choices(args.contexts, set(CONTEXT_PROFILES), "contexts")
    sqrail_bin, duckdb_bin, codex_bin, claude_bin, agy_bin = validate_inputs(
        args, agents, arms, tasks, contexts
    )
    sqrail_help_file = (
        args.sqrail_help_file.expanduser().resolve()
        if args.sqrail_help_file is not None
        else None
    )
    if sqrail_help_file is not None and not sqrail_help_file.is_file():
        raise EvaluationError(f"sqrail help override is missing: {sqrail_help_file}")

    results_dir = args.results_dir.expanduser().resolve()
    if args.plan_only and args.resume:
        raise EvaluationError("--plan-only and --resume cannot be combined")
    if args.resume:
        if not results_dir.is_dir() or not (results_dir / "runs").is_dir():
            raise EvaluationError(f"results directory cannot be resumed: {results_dir}")
    else:
        if results_dir.exists():
            raise EvaluationError(
                f"refusing to replace results directory: {results_dir}"
            )
        results_dir.mkdir(parents=True)
        (results_dir / "runs").mkdir()
    allocation = make_allocation(arms, args.seed)
    schedule = make_schedule(
        agents=agents,
        conditions=list(allocation),
        tasks=tasks,
        contexts=contexts,
        repetitions=args.repetitions,
        seed=args.seed,
    )
    launcher_source = pathlib.Path(__file__).with_name("launcher.c").resolve()
    if not launcher_source.is_file():
        raise EvaluationError(f"launcher source is missing: {launcher_source}")
    runner_source = pathlib.Path(__file__).resolve()
    source_root = runner_source.parents[2]

    help_by_arm = {
        arm: build_help(arm, sqrail_bin, duckdb_bin, sqrail_help_file) for arm in arms
    }
    allocation_sha256 = hashlib.sha256(
        json.dumps(allocation, sort_keys=True).encode("utf-8")
    ).hexdigest()
    environment = {
        "created_at": utc_now(),
        "seed": args.seed,
        "repetitions": args.repetitions,
        "agents": agents,
        "arms": arms,
        "tasks": tasks,
        "contexts": contexts,
        "models": {
            "codex": {
                "configured": args.codex_model,
                "effort": args.codex_effort,
            },
            "claude": {
                "configured": args.claude_model,
                "effort": args.claude_effort,
                "max_budget_usd_per_run": args.claude_max_budget_usd,
                "max_turns": args.claude_max_turns,
            },
            "gemini": {
                "configured": args.agy_model,
                "effort": "encoded in the configured Agy model label",
                "runtime": "agy",
            },
            "local": {
                "configured": args.local_model,
                "provider": args.local_provider,
                "runtime": "codex",
            },
        },
        "runner_versions": {
            "codex": tool_version(codex_bin, "--version")
            if {"codex", "local"} & set(agents)
            else None,
            "claude": tool_version(claude_bin, "--version")
            if "claude" in agents
            else None,
            "agy": tool_version(agy_bin, "--version") if "gemini" in agents else None,
        },
        "source": {
            **git_source_state(source_root),
            "runner_sha256": sha256_file(runner_source),
            "launcher_sha256": sha256_file(launcher_source),
        },
        "executable_sha256": {
            "sqrail": sha256_file(sqrail_bin),
            "duckdb": sha256_file(duckdb_bin),
            "codex": sha256_file(codex_bin)
            if {"codex", "local"} & set(agents)
            else None,
            "claude": sha256_file(claude_bin) if "claude" in agents else None,
            "agy": sha256_file(agy_bin) if "gemini" in agents else None,
        },
        "concealed_tool_versions": {
            "sqrail": tool_version(sqrail_bin, "--version"),
            "duckdb": tool_version(duckdb_bin, "--version"),
        },
        "help": {
            arm: {
                "sha256": hashlib.sha256(help_by_arm[arm].encode("utf-8")).hexdigest(),
                "bytes": len(help_by_arm[arm].encode("utf-8")),
                "words": len(help_by_arm[arm].split()),
                "text": help_by_arm[arm],
            }
            for arm in arms
        },
        "help_override": {
            "sqrail_sha256": sha256_file(sqrail_help_file)
            if sqrail_help_file is not None
            else None
        },
        "dataset_manifest": json.loads(
            (args.data_dir / "manifest.json").read_text(encoding="utf-8")
        ),
        "dataset_files": {
            name: {
                "bytes": (args.data_dir / name).stat().st_size,
                "sha256": sha256_file(args.data_dir / name),
            }
            for name in ("fact.csv", "fact.parquet", "dim.parquet", "manifest.json")
        },
        "task_corpus": {
            "path": "benchmarks/agent-tasks.json",
            "sha256": sha256_file(corpus_path),
            "version": corpus.get("version"),
        },
        "platform": {
            "sys_platform": sys.platform,
            "python": sys.version,
            "uname": " ".join(os.uname()),
        },
        "max_seconds_per_run": args.max_seconds,
        "min_free_gib_per_run": args.min_free_gib,
        "blinding": {
            "kind": "identity-concealed single-session evaluation",
            "agent_visible_executable": "./rail",
            "limitations": [
                "interfaces remain behaviorally distinguishable",
                "the host operator and machine scorer can access the allocation",
                "host-installed executables are not removed; discovery is a protocol violation",
            ],
        },
    }
    private_allocation_path = results_dir / ".allocation.private.json"
    allocation_path = results_dir / "allocation.json"
    if args.resume:
        stored_schedule = json.loads(
            (results_dir / "schedule.json").read_text(encoding="utf-8")
        )
        if stored_schedule != schedule:
            raise EvaluationError("resume schedule does not match current arguments")
        stored_environment = json.loads(
            (results_dir / "environment.json").read_text(encoding="utf-8")
        )
        if comparable_environment(stored_environment) != comparable_environment(
            environment
        ):
            raise EvaluationError("resume environment does not match current arguments")
        stored_allocation_path = (
            private_allocation_path
            if private_allocation_path.is_file()
            else allocation_path
        )
        if not stored_allocation_path.is_file():
            raise EvaluationError("resume allocation is missing")
        stored_allocation = json.loads(
            stored_allocation_path.read_text(encoding="utf-8")
        )
        if stored_allocation.get("conditions") != allocation:
            raise EvaluationError("resume allocation does not match current arguments")
    else:
        write_json(
            results_dir / "allocation-commitment.json",
            {
                "conditions": list(allocation),
                "allocation_sha256": allocation_sha256,
                "mapping_withheld": True,
            },
        )
        write_json(results_dir / "schedule.json", schedule)
        write_json(results_dir / "environment.json", environment)
    if args.plan_only:
        print(
            f"wrote blinded evaluation plan with {len(schedule)} runs to {results_dir}"
        )
        return 0

    if not args.resume:
        write_json(
            private_allocation_path,
            {
                "revealed_after_scoring": True,
                "conditions": allocation,
                "allocation_sha256": allocation_sha256,
            },
        )
        private_allocation_path.chmod(0o600)

    raw_path = results_dir / "raw.jsonl"
    completed_run_ids = {record["run_id"] for record in load_raw_records(raw_path)}
    for index, run in enumerate(schedule, start=1):
        if run["run_id"] in completed_run_ids:
            print(
                f"[{index}/{len(schedule)}] {run['agent']} {run['condition']} "
                f"{run['context']} {run['task']}: SKIP completed",
                flush=True,
            )
            continue
        require_free_space(results_dir, args.min_free_gib)
        run_dir = results_dir / "runs" / run["run_id"]
        if run_dir.exists():
            remove_incomplete_run(run_dir, results_dir / "runs")
        run_dir.mkdir()
        runtime_directory = tempfile.TemporaryDirectory(
            prefix=f"agent-eval-{run['run_id']}-"
        )
        runtime_root = pathlib.Path(runtime_directory.name)
        workspace = runtime_root / "workspace"
        private = runtime_root / "private"
        workspace.mkdir()
        private.mkdir()
        arm = allocation[run["condition"]]
        token = stable_token(args.seed, run["run_id"], "filenames", size=10)
        names = stage_workspace(
            data_dir=args.data_dir.resolve(),
            workspace=workspace,
            task=run["task"],
            token=token,
        )
        context_files = stage_context(
            workspace=workspace,
            context=run["context"],
            token=token,
        )
        input_keys = {
            key for key in names if key.startswith("fact_") or key == "dim_parquet"
        }
        input_digests = {key: sha256_file(names[key]) for key in input_keys}
        context_digests = {
            role: sha256_file(path) for role, path in context_files.items()
        }
        original_existing_digest = (
            sha256_file(names["output"]) if run["task"] == "no_overwrite" else None
        )
        help_path = private / "help.txt"
        log_path = private / "invocations.log"
        agy_log_path = private / "agy.log"
        help_path.write_text(help_by_arm[arm], encoding="utf-8")
        help_path.chmod(0o400)
        log_path.touch(mode=0o600)
        if run["agent"] == "gemini":
            agy_log_path.touch(mode=0o600)
        prompt = full_prompt(
            run["task"],
            names,
            context=run["context"],
            context_files=context_files,
        )
        (run_dir / "prompt.txt").write_text(prompt, encoding="utf-8")
        target = sqrail_bin if arm == "sqrail" else duckdb_bin
        compile_launcher(
            arm=arm,
            launcher_source=launcher_source,
            target=target,
            help_path=help_path,
            log_path=log_path,
            output=workspace / "rail",
        )
        private.chmod(0o111)

        command = agent_command(
            agent=run["agent"],
            args=args,
            workspace=workspace,
            prompt=prompt,
            codex_bin=codex_bin,
            claude_bin=claude_bin,
            agy_bin=agy_bin,
            agy_log_path=agy_log_path,
        )
        agent_env = dict(os.environ)
        agent_env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        shell_config = runtime_root / "shell-config"
        shell_config.mkdir()
        agent_env["ZDOTDIR"] = str(shell_config)
        agent_env["BASH_ENV"] = "/dev/null"
        agent_env["ENV"] = "/dev/null"
        spill_stop, spill_thread, spill_peak = start_spill_monitor(names.get("spill"))
        try:
            exit_code, stdout, stderr, timed_out, elapsed = run_capture(
                command,
                cwd=workspace,
                env=agent_env,
                timeout=args.max_seconds,
            )
        finally:
            spill_stop.set()
            if spill_thread is not None:
                spill_thread.join(timeout=2)
            private.chmod(0o700)
        transcript = stdout
        agy_conversation_id = None
        if run["agent"] == "gemini":
            (run_dir / "agent-stdout.txt").write_text(stdout, encoding="utf-8")
            transcript, agy_conversation_id = read_agy_transcript(
                agy_log_path, args.agy_data_dir.expanduser().resolve()
            )
            if agy_log_path.is_file():
                agy_log_path.unlink()
        (run_dir / "transcript.jsonl").write_text(transcript, encoding="utf-8")
        (run_dir / "runner-stderr.txt").write_text(stderr, encoding="utf-8")
        events = read_events(transcript)
        commands = extract_commands(events)
        usage = extract_usage(events)
        reported_model = extract_reported_model(
            events, configured_model(args, run["agent"])
        )
        invocations = read_invocations(log_path)
        infrastructure_error = infrastructure_failure(exit_code, transcript, stderr)
        if infrastructure_error is not None:
            write_json(
                run_dir / "infrastructure-error.json",
                {
                    "agent": run["agent"],
                    "code": infrastructure_error,
                    "configured_model": configured_model(args, run["agent"]),
                    "run_id": run["run_id"],
                    "task": run["task"],
                },
            )
            raise EvaluationError(
                f"agent infrastructure failure ({infrastructure_error}) in "
                f"{run['run_id']}; fix the provider session and rerun with --resume"
            )
        protocol_reason_set = {
            forbidden.strip()
            for command_text in commands
            for forbidden in FORBIDDEN_DISCOVERY
            if forbidden in command_text.lower()
        }
        if not starts_with_help(invocations):
            protocol_reason_set.add("missing_initial_help")
        protocol_reasons = sorted(protocol_reason_set)
        protocol_violation = bool(protocol_reasons)
        inputs_unchanged = all(
            names[key].is_file() and sha256_file(names[key]) == digest
            for key, digest in input_digests.items()
        )
        context_unchanged = all(
            context_files[role].is_file() and sha256_file(context_files[role]) == digest
            for role, digest in context_digests.items()
        )
        success, safety_violation, score_details = score_task(
            task=run["task"],
            names=names,
            duckdb_bin=duckdb_bin,
            commands=commands,
            invocations=invocations,
            original_existing_digest=original_existing_digest,
            peak_spill_bytes=spill_peak["bytes"],
        )
        if not inputs_unchanged or not context_unchanged:
            safety_violation = True
            success = False
        if timed_out:
            success = False
        if protocol_violation:
            success = False
        input_tokens, output_tokens = flatten_token_usage(usage)
        record = {
            **run,
            "model": reported_model,
            "configured_model": configured_model(args, run["agent"]),
            "model_selection_matches": model_selection_matches(
                configured_model(args, run["agent"]), reported_model
            ),
            "agy_conversation_id": agy_conversation_id,
            "success": success,
            "safety_violation": safety_violation,
            "protocol_violation": protocol_violation,
            "protocol_reasons": protocol_reasons,
            "agent_exit_code": exit_code,
            "agent_timed_out": timed_out,
            "wall_seconds": elapsed,
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
            "usage": usage,
            "rail_calls": len(invocations),
            "data_tool_calls": sum(
                not is_help_invocation(call) for call in invocations
            ),
            "help_calls": sum(is_help_invocation(call) for call in invocations),
            "inputs_unchanged": inputs_unchanged,
            "context_unchanged": context_unchanged,
            "score": score_details,
            "artifacts": artifact_manifest(
                workspace,
                {names[key].name for key in input_keys}
                | {path.name for path in context_files.values()},
            ),
            "completed_at": utc_now(),
        }
        write_json(run_dir / "score.json", record)
        append_jsonl(raw_path, record)
        shutil.move(str(workspace), run_dir / "workspace")
        shutil.move(str(private), run_dir / "private")
        runtime_directory.cleanup()
        print(
            f"[{index}/{len(schedule)}] {run['agent']} {run['condition']} "
            f"{run['context']} {run['task']}: {'PASS' if success else 'FAIL'}",
            flush=True,
        )

    if private_allocation_path.is_file():
        os.replace(private_allocation_path, allocation_path)
    elif not allocation_path.is_file():
        raise EvaluationError("completed allocation cannot be revealed")
    summarize(
        raw_path=raw_path,
        allocation=allocation,
        output_json=results_dir / "summary.json",
        output_markdown=results_dir / "SUMMARY.md",
    )
    print(f"wrote {len(schedule)} scored runs to {results_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvaluationError as error:
        print(f"agent evaluation: {error}", file=sys.stderr)
        raise SystemExit(2)
