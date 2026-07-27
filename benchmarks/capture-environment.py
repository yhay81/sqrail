#!/usr/bin/env python3
"""Capture non-secret benchmark provenance and host metadata as JSON."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command: list[str]) -> str | None:
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    value = result.stdout.strip()
    return value if result.returncode == 0 and value else None


def cpu_model() -> str | None:
    if sys.platform == "darwin":
        return command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith(("model name", "hardware")) and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or None


def memory_bytes() -> int | None:
    if sys.platform == "darwin":
        raw = command_output(["sysctl", "-n", "hw.memsize"])
        return int(raw) if raw and raw.isdecimal() else None
    if hasattr(os, "sysconf"):
        try:
            return int(os.sysconf("SC_PHYS_PAGES")) * int(os.sysconf("SC_PAGE_SIZE"))
        except (OSError, ValueError):
            return None
    return None


def executable_metadata(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError(f"benchmark executable does not exist: {path}")
    return {
        "path": str(resolved),
        "bytes": resolved.stat().st_size,
        "sha256": sha256(resolved),
        "version": command_output([str(resolved), "--version"]),
    }


def git_metadata(repository: Path) -> dict[str, Any]:
    commit = command_output(["git", "-C", str(repository), "rev-parse", "HEAD"])
    status = command_output(["git", "-C", str(repository), "status", "--porcelain"])
    return {
        "commit": commit,
        "dirty": status is not None and bool(status),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--sqrail", type=Path, required=True)
    parser.add_argument("--duckdb", type=Path)
    parser.add_argument("--data-manifest", type=Path)
    parser.add_argument(
        "--cache-state",
        choices=("warm", "cold-controlled", "first-run-uncontrolled"),
        required=True,
    )
    parser.add_argument("--cache-control-evidence", default="")
    parser.add_argument("--parameters-json", default="{}")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    if arguments.cache_state == "cold-controlled" and not arguments.cache_control_evidence:
        raise ValueError("cold-controlled measurements require --cache-control-evidence")
    try:
        parameters = json.loads(arguments.parameters_json)
    except json.JSONDecodeError as error:
        raise ValueError(f"--parameters-json is invalid: {error}") from error
    if not isinstance(parameters, dict):
        raise ValueError("--parameters-json must be a JSON object")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    storage = shutil.disk_usage(arguments.output.parent)
    compiler_command = shlex.split(os.environ.get("CXX", "c++")) or ["c++"]
    filesystem_command = (
        ["df", "-T", str(arguments.output.parent)]
        if sys.platform.startswith("linux")
        else ["df", "-P", str(arguments.output.parent)]
    )
    executables = {"sqrail": executable_metadata(arguments.sqrail)}
    if arguments.duckdb is not None:
        executables["duckdb"] = executable_metadata(arguments.duckdb)

    document: dict[str, Any] = {
        "schema_version": 1,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "cache": {
            "state": arguments.cache_state,
            "control_evidence": arguments.cache_control_evidence or None,
        },
        "parameters": parameters,
        "executables": executables,
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "cpu_model": cpu_model(),
            "logical_cpu_count": os.cpu_count(),
            "memory_bytes": memory_bytes(),
            "storage_total_bytes": storage.total,
            "storage_free_bytes": storage.free,
            "python": platform.python_version(),
            "compiler": command_output([*compiler_command, "--version"]),
            "filesystem": command_output(filesystem_command),
        },
        "repository": git_metadata(arguments.repository.resolve()),
        "locale": {
            key: os.environ[key]
            for key in ("LANG", "LC_ALL", "TZ")
            if key in os.environ
        },
    }
    if arguments.data_manifest is not None:
        manifest = arguments.data_manifest.resolve()
        if not manifest.is_file():
            raise ValueError(f"data manifest does not exist: {manifest}")
        document["data_manifest"] = {
            "path": str(manifest),
            "bytes": manifest.stat().st_size,
            "sha256": sha256(manifest),
        }

    arguments.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"capture-environment: {error}", file=sys.stderr)
        sys.exit(2)
