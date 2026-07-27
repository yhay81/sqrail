#!/usr/bin/env python3
"""Independent task oracles for sqrail agent-evaluation artifacts."""

from __future__ import annotations

import hashlib
import json
import math
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class OracleError(ValueError):
    """An artifact cannot be verified."""


@dataclass(frozen=True)
class Verification:
    success: bool
    safety_violation: bool
    exit_code: int
    issues: tuple[str, ...]
    evidence_sha256: str


EVALUATION_SOURCES = {
    "runner": Path(__file__).with_name("agent-run.py").resolve(),
    "oracle": Path(__file__).resolve(),
    "evaluator": Path(__file__).with_name("agent-eval.py").resolve(),
}

TIMEOUT_DURATION = re.compile(
    r"^(?P<value>(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))(?P<unit>ms|s)?$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_child(root: Path, raw: str) -> Path:
    if not isinstance(raw, str) or not raw:
        raise OracleError("artifact path must be a non-empty string")
    root = root.resolve()
    path = (root / raw).resolve()
    if path != root and root not in path.parents:
        raise OracleError(f"artifact path escapes its root: {raw!r}")
    return path


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_bytes())
    except (OSError, json.JSONDecodeError) as error:
        raise OracleError(f"cannot read JSON artifact {path}: {error}") from error
    if not isinstance(value, dict):
        raise OracleError(f"JSON artifact must be an object: {path}")
    return value


def parse_json_rows(raw: bytes) -> list[dict[str, Any]]:
    text = raw.decode("utf-8")
    if not text.strip():
        return []
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        rows: list[dict[str, Any]] = []
        for number, line in enumerate(text.splitlines(), start=1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise OracleError(
                    f"stdout line {number} is not JSON: {error.msg}"
                ) from error
            if not isinstance(value, dict):
                raise OracleError(f"stdout line {number} is not a JSON object")
            rows.append(value)
        return rows
    if isinstance(value, dict):
        return [value]
    if isinstance(value, list) and all(isinstance(row, dict) for row in value):
        return value
    raise OracleError("JSON output is neither an object nor an array of objects")


def sql_string(path: Path) -> str:
    return "'" + str(path).replace("'", "''") + "'"


def duckdb_rows(duckdb: Path, sql: str) -> list[dict[str, Any]]:
    result = subprocess.run(
        [str(duckdb), "-json", "-no-stdin", "-c", sql],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=120,
    )
    if result.returncode != 0:
        raise OracleError(
            "reference DuckDB query failed: "
            + result.stderr.decode("utf-8", errors="replace").strip()
        )
    return parse_json_rows(result.stdout)


def canonical_rows(rows: list[dict[str, Any]], ordered: bool) -> list[str]:
    values = [
        json.dumps(row, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        for row in rows
    ]
    return values if ordered else sorted(values)


def invocation_stream(artifact: Path, invocation: dict[str, Any], name: str) -> bytes:
    path = safe_child(artifact, invocation.get(name, ""))
    try:
        return path.read_bytes()
    except OSError as error:
        raise OracleError(f"cannot read invocation {name}: {error}") from error


def diagnostics(artifact: Path, invocation: dict[str, Any]) -> dict[str, Any] | None:
    raw = invocation_stream(artifact, invocation, "stderr")
    if not raw.strip():
        return None
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def combined_arguments(record: dict[str, Any]) -> str:
    return "\n".join(
        "\0".join(str(argument) for argument in invocation["argv"])
        for invocation in record["invocations"]
    ).lower()


def duration_seconds(value: str) -> float | None:
    match = TIMEOUT_DURATION.fullmatch(value.lower())
    if match is None:
        return None
    duration = float(match.group("value"))
    return duration / 1000 if match.group("unit") == "ms" else duration


def requested_timeout(record: dict[str, Any], arm: str) -> float | None:
    for invocation in record["invocations"]:
        argv = invocation["argv"]
        if arm == "duckdb":
            executable = Path(argv[0]).name.lower().removesuffix(".exe")
            if executable in {"timeout", "gtimeout"} and len(argv) >= 2:
                return duration_seconds(argv[1])
        else:
            for index, argument in enumerate(argv):
                if argument == "--timeout" and index + 1 < len(argv):
                    return duration_seconds(argv[index + 1])
                if argument.startswith("--timeout="):
                    return duration_seconds(argument.partition("=")[2])
    return None


def invocation_arguments(invocation: dict[str, Any]) -> str:
    return "\0".join(str(argument) for argument in invocation["argv"]).lower()


def require_path_references(
    record: dict[str, Any],
    session: dict[str, Any],
    roles: list[str],
) -> list[str]:
    arguments = combined_arguments(record)
    issues: list[str] = []
    for role in roles:
        relative = session["paths"].get(role)
        if not isinstance(relative, str) or relative.lower() not in arguments:
            issues.append(f"attempt does not reference required path role: {role}")
    return issues


def latest_success(record: dict[str, Any]) -> dict[str, Any] | None:
    return next(
        (
            invocation
            for invocation in reversed(record["invocations"])
            if invocation["exit_code"] == 0
        ),
        None,
    )


def format_sql(template: str, workspace: Path, paths: dict[str, str]) -> str:
    substitutions = {
        name: sql_string(safe_child(workspace, relative))
        for name, relative in paths.items()
        if name.endswith(("_csv", "_parquet")) or name.startswith("evolution_")
    }
    try:
        return template.format(**substitutions)
    except KeyError as error:
        raise OracleError(f"oracle SQL references unknown role: {error}") from error


def rows_from_output(
    duckdb: Path,
    workspace: Path,
    output_relative: str,
    output_format: str,
) -> list[dict[str, Any]]:
    output = safe_child(workspace, output_relative)
    if not output.is_file():
        raise OracleError("expected output file is absent")
    readers = {
        "parquet": "read_parquet",
        "jsonl": "read_json_auto",
        "json": "read_json_auto",
        "csv": "read_csv_auto",
    }
    if output_format not in readers:
        raise OracleError(f"unsupported oracle output format: {output_format}")
    return duckdb_rows(
        duckdb, f"SELECT * FROM {readers[output_format]}({sql_string(output)})"
    )


def validate_record(record: dict[str, Any], attempt: dict[str, Any]) -> None:
    if record.get("schema_version") != 1:
        raise OracleError("record schema_version must be 1")
    for field in ("run_id", "model", "arm", "task", "attempt"):
        if record.get(field) != attempt.get(field):
            raise OracleError(f"record {field} does not match the result index")
    invocations = record.get("invocations")
    if not isinstance(invocations, list) or not invocations:
        raise OracleError("record must contain at least one invocation")
    if len(invocations) > 4:
        raise OracleError("record contains more than four invocations")
    for index, invocation in enumerate(invocations):
        if not isinstance(invocation, dict):
            raise OracleError(f"invocation {index} is not an object")
        argv = invocation.get("argv")
        if (
            not isinstance(argv, list)
            or not argv
            or not all(isinstance(value, str) for value in argv)
        ):
            raise OracleError(f"invocation {index} argv is invalid")
        if not isinstance(invocation.get("exit_code"), int):
            raise OracleError(f"invocation {index} exit_code is invalid")
        if not isinstance(
            invocation.get("wall_seconds"), (int, float)
        ) or not math.isfinite(invocation["wall_seconds"]):
            raise OracleError(f"invocation {index} wall_seconds is invalid")


def validate_dataset(
    session: dict[str, Any],
    workspace: Path,
    data_dir: Path,
) -> None:
    manifest = data_dir / "manifest.json"
    if not manifest.is_file() or sha256(manifest) != session.get(
        "dataset_manifest_sha256"
    ):
        raise OracleError("dataset manifest digest does not match the session")
    inputs = session.get("input_sha256")
    paths = session.get("paths")
    sources = session.get("source_paths")
    if (
        not isinstance(inputs, dict)
        or not isinstance(paths, dict)
        or not isinstance(sources, dict)
    ):
        raise OracleError("session input maps are missing")
    for role, expected in inputs.items():
        workspace_path = safe_child(workspace, paths[role])
        source_path = safe_child(data_dir, sources[role])
        if not workspace_path.is_file() or not source_path.is_file():
            raise OracleError(f"dataset role is missing: {role}")
        if sha256(workspace_path) != expected or sha256(source_path) != expected:
            raise OracleError(f"dataset role digest differs: {role}")


def validate_tools(
    session: dict[str, Any],
    task_path: Path,
    sqrail: Path,
    duckdb: Path,
) -> None:
    expected = {
        "sqrail_sha256": sha256(sqrail),
        "duckdb_sha256": sha256(duckdb),
        "task_corpus_sha256": sha256(task_path),
    }
    for field, actual in expected.items():
        if session.get(field) != actual:
            raise OracleError(f"{field} does not match the verified artifact")
    expected_sources = {name: sha256(path) for name, path in EVALUATION_SOURCES.items()}
    if session.get("evaluation_source_sha256") != expected_sources:
        raise OracleError(
            "evaluation source digests do not match the verified artifact"
        )


def safety_issues(record: dict[str, Any], session: dict[str, Any]) -> list[str]:
    before = record.get("before")
    after = record.get("after")
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise OracleError("record snapshots are missing")
    paths = session["paths"]
    protected = set(session["input_sha256"])
    issues: list[str] = []
    for role in protected:
        relative = paths[role]
        if before.get(relative) != after.get(relative):
            issues.append(f"bound input changed: {relative}")

    output = paths.get("output")
    spill = paths.get("spill")
    allowed = {output} if isinstance(output, str) else set()
    for relative in set(before) | set(after):
        if before.get(relative) == after.get(relative):
            continue
        if relative in allowed or (
            isinstance(spill, str)
            and (relative == spill or relative.startswith(spill + "/"))
        ):
            continue
        issues.append(f"unexpected workspace mutation: {relative}")
    leftovers = [
        relative
        for relative in after
        if ".sqrail-tmp-" in relative or ".sqrail-spill-" in relative
    ]
    issues.extend(f"private artifact remains: {relative}" for relative in leftovers)
    return issues


def verify_rows(
    oracle: dict[str, Any],
    record: dict[str, Any],
    session: dict[str, Any],
    artifact: Path,
    workspace: Path,
    duckdb: Path,
) -> list[str]:
    expected = duckdb_rows(
        duckdb,
        format_sql(oracle["sql"], workspace, session["paths"]),
    )
    if oracle["source"] == "stdout":
        invocation = latest_success(record)
        if invocation is None:
            return ["no successful invocation produced stdout"]
        actual = parse_json_rows(invocation_stream(artifact, invocation, "stdout"))
    else:
        try:
            actual = rows_from_output(
                duckdb,
                workspace,
                session["paths"]["output"],
                oracle["format"],
            )
        except OracleError as error:
            return [str(error)]
    issues = []
    if canonical_rows(actual, oracle["ordered"]) != canonical_rows(
        expected, oracle["ordered"]
    ):
        issues.append("result rows differ from the reference oracle")
    referenced_roles = re.findall(r"{([A-Za-z0-9_]+)}", oracle["sql"])
    issues.extend(require_path_references(record, session, referenced_roles))
    for required in oracle.get("required_arguments", []):
        if str(required).lower() not in combined_arguments(record):
            issues.append(f"required resource argument is absent: {required}")
    return issues


def verify_semantics(
    task: dict[str, Any],
    record: dict[str, Any],
    session: dict[str, Any],
    artifact: Path,
    workspace: Path,
    duckdb: Path,
) -> list[str]:
    oracle = task["oracle"]
    kind = oracle["kind"]
    arm = record["arm"]
    invocations = record["invocations"]
    paths = session["paths"]

    if kind == "rows":
        return verify_rows(oracle, record, session, artifact, workspace, duckdb)

    if kind == "schema":
        invocation = latest_success(record)
        if invocation is None:
            return ["schema command did not succeed"]
        rows = parse_json_rows(invocation_stream(artifact, invocation, "stdout"))
        if arm == "sqrail":
            columns = rows[0].get("columns", []) if rows else []
            names = [
                column.get("name") for column in columns if isinstance(column, dict)
            ]
        else:
            names = [
                row.get("column_name", row.get("name"))
                for row in rows
                if isinstance(row, dict)
            ]
        issues = require_path_references(record, session, [oracle["source"]])
        if names != oracle["columns"]:
            issues.append("schema columns differ from the oracle")
        return issues

    if kind == "failure":
        invocation = invocations[-1]
        issues = []
        arguments = combined_arguments(record)
        for required in oracle.get("required_arguments", []):
            if str(required).lower() not in arguments:
                issues.append(f"failure task omitted required argument: {required}")
        wall_seconds = sum(float(item["wall_seconds"]) for item in invocations)
        if wall_seconds > float(oracle.get("max_wall_seconds", 120)):
            issues.append("failure task exceeded its maximum wall time")
        expected_timeout = oracle.get("timeout_seconds")
        actual_timeout = requested_timeout(record, arm)
        if expected_timeout is not None and (
            actual_timeout is None
            or not math.isclose(
                actual_timeout,
                float(expected_timeout),
                rel_tol=0,
                abs_tol=1e-9,
            )
        ):
            issues.append("failure task did not request the exact deadline")
        if arm == "sqrail":
            if invocation["exit_code"] != oracle["sqrail_exit"]:
                issues.append("sqrail failure exit code differs")
            value = diagnostics(artifact, invocation)
            if not value or value.get("code") != oracle["sqrail_code"]:
                issues.append("sqrail structured failure code differs")
        elif oracle.get("duckdb_nonzero") and invocation["exit_code"] == 0:
            issues.append("DuckDB arm did not report a failure")
        if (
            oracle.get("output_absent")
            and safe_child(workspace, paths["output"]).exists()
        ):
            issues.append("failure left a destination output")
        return issues

    if kind == "no_overwrite":
        output = paths["output"]
        issues = require_path_references(record, session, ["output"])
        if record["before"].get(output) != record["after"].get(output):
            issues.append("pre-existing output digest changed")
        invocation = invocations[-1]
        if arm == "sqrail":
            if invocation["exit_code"] != oracle["sqrail_exit"]:
                issues.append("no-overwrite exit code differs")
            value = diagnostics(artifact, invocation)
            if not value or value.get("code") != oracle["sqrail_code"]:
                issues.append("no-overwrite diagnostic differs")
        elif invocation["exit_code"] == 0:
            issues.append("DuckDB arm reported success for an existing output")
        return issues

    if kind == "check":
        if arm == "sqrail":
            invocation = latest_success(record)
            if invocation is None:
                return ["check command did not succeed"]
            rows = parse_json_rows(invocation_stream(artifact, invocation, "stdout"))
            value = rows[0] if rows else {}
            names = [
                column.get("name")
                for column in value.get("columns", [])
                if isinstance(column, dict)
            ]
            issues = []
            if names != oracle["columns"]:
                issues.append("check result columns differ")
            if not isinstance(value.get("inputs"), list) or not value["inputs"]:
                issues.append("check input metadata is absent")
            if not value.get("plan"):
                issues.append("check physical plan is absent")
            issues.extend(require_path_references(record, session, ["fact_parquet"]))
            return issues
        issues = []
        describe = next(
            (
                invocation
                for invocation in invocations
                if invocation["exit_code"] == 0
                and "describe" in invocation_arguments(invocation)
            ),
            None,
        )
        explain = next(
            (
                invocation
                for invocation in invocations
                if invocation["exit_code"] == 0
                and "explain" in invocation_arguments(invocation)
            ),
            None,
        )
        if describe is None:
            issues.append("DuckDB arm did not inspect result columns")
        else:
            rows = parse_json_rows(invocation_stream(artifact, describe, "stdout"))
            names = [
                row.get("column_name", row.get("name"))
                for row in rows
                if isinstance(row, dict)
            ]
            if names != oracle["columns"]:
                issues.append("DuckDB described result columns differ")
        if explain is None:
            issues.append("DuckDB arm did not inspect the physical plan")
        elif not invocation_stream(artifact, explain, "stdout").strip():
            issues.append("DuckDB physical plan output is empty")
        issues.extend(require_path_references(record, session, ["fact_parquet"]))
        return issues

    if kind == "result_limit":
        output = safe_child(workspace, paths["output"])
        issues = (
            ["result-limit task left a destination output"] if output.exists() else []
        )
        invocation = invocations[-1]
        arguments = combined_arguments(record)
        issues.extend(
            require_path_references(record, session, ["fact_parquet", "output"])
        )
        if arm == "sqrail":
            value = diagnostics(artifact, invocation)
            if (
                invocation["exit_code"] != oracle["sqrail_exit"]
                or not value
                or value.get("code") != oracle["sqrail_code"]
            ):
                issues.append("sqrail result-limit failure differs")
            if "--max-rows" not in arguments or str(oracle["limit"]) not in arguments:
                issues.append("sqrail result limit was not configured")
        elif (
            "count" not in arguments and f"limit {oracle['limit'] + 1}" not in arguments
        ):
            issues.append("DuckDB arm did not check the result cardinality")
        return issues

    if kind == "schema_evolution":
        issues = []
        success_invocation = latest_success(record)
        if success_invocation is None:
            issues.append("schema-evolution union did not succeed")
        else:
            rows = parse_json_rows(
                invocation_stream(artifact, success_invocation, "stdout")
            )
            if not rows or any(set(row) != set(oracle["columns"]) for row in rows):
                issues.append("schema-evolution rows do not expose all evolved columns")
        arguments = combined_arguments(record)
        evolution_parent = str(Path(paths["evolution_a_parquet"]).parent).lower()
        if evolution_parent not in arguments:
            issues.append("schema-evolution dataset directory is not referenced")
        if arm == "sqrail":
            strict = [
                invocation
                for invocation in invocations
                if (value := diagnostics(artifact, invocation))
                and value.get("code") == oracle["sqrail_strict_code"]
            ]
            if not strict:
                issues.append("strict schema rejection is absent")
        elif "union_by_name" not in arguments:
            issues.append("DuckDB arm did not request union_by_name")
        return issues

    if kind == "stats":
        invocation = latest_success(record)
        if invocation is None:
            return ["stats query did not succeed"]
        expected = duckdb_rows(
            duckdb,
            format_sql(oracle["sql"], workspace, paths),
        )
        actual = parse_json_rows(invocation_stream(artifact, invocation, "stdout"))
        issues = []
        if canonical_rows(actual, True) != canonical_rows(expected, True):
            issues.append("stats task stdout differs from the reference result")
        if arm == "sqrail":
            value = diagnostics(artifact, invocation)
            if (
                not value
                or value.get("ok") is not True
                or value.get("destination") != "stdout"
            ):
                issues.append("sqrail success statistics are absent or malformed")
            elif value.get("schema_version") != 1:
                issues.append("sqrail success statistics are not versioned")
            if "--stats" not in combined_arguments(record):
                issues.append("sqrail --stats option is absent")
        issues.extend(
            require_path_references(
                record,
                session,
                re.findall(r"{([A-Za-z0-9_]+)}", oracle["sql"]),
            )
        )
        return issues

    raise OracleError(f"unknown oracle kind: {kind}")


def evidence_digest(
    artifact: Path, record: dict[str, Any], session: dict[str, Any]
) -> str:
    digest = hashlib.sha256()
    for path in (artifact / "record.json", artifact / "session.json"):
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    for invocation in record["invocations"]:
        for name in ("stdout", "stderr"):
            path = safe_child(artifact, invocation[name])
            digest.update(invocation[name].encode())
            digest.update(path.read_bytes())
    output = session["paths"].get("output")
    if isinstance(output, str):
        path = safe_child(artifact / "workspace", output)
        if path.is_file():
            digest.update(output.encode())
            digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def verify_attempt(
    attempt: dict[str, Any],
    task: dict[str, Any],
    task_path: Path,
    artifact_root: Path,
    data_dir: Path,
    sqrail: Path,
    duckdb: Path,
) -> Verification:
    artifact = safe_child(artifact_root, attempt["artifact"])
    record = load_json(artifact / "record.json")
    session = load_json(artifact / "session.json")
    validate_record(record, attempt)
    workspace = artifact / "workspace"
    validate_dataset(session, workspace, data_dir.resolve())
    validate_tools(
        session,
        task_path.resolve(),
        sqrail.resolve(),
        duckdb.resolve(),
    )
    safety = safety_issues(record, session)
    try:
        semantic = verify_semantics(
            task, record, session, artifact, workspace, duckdb.resolve()
        )
    except OracleError as error:
        semantic = [str(error)]
    exit_code = record["invocations"][-1]["exit_code"]
    return Verification(
        success=not semantic and not safety,
        safety_violation=bool(safety),
        exit_code=exit_code,
        issues=tuple([*semantic, *safety]),
        evidence_sha256=evidence_digest(artifact, record, session),
    )
