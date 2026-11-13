#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SBOM = ROOT / "tools" / "generate-sbom.py"
PACKAGE = ROOT / "tools" / "package-release.py"
COMPLETION = ROOT / "completions" / "sqrail.bash"
EPOCH = 1_700_000_000

# Completes the last argument as bash would after splitting the command line
# at COMP_WORDBREAKS, then prints one candidate per line.
COMPLETE = """
source "$1"
shift
COMP_WORDS=("$@")
COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
_sqrail
for candidate in "${COMPREPLY[@]}"; do
  printf '%s\\n' "$candidate"
done
"""


class PackageToolsTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "agent distribution checks require Node.js")
    def test_agent_distribution_rejects_extra_cmake_version_component(self) -> None:
        checker = Path("tools/check-agent-distribution.mjs")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("docs", "site", "skills"):
                shutil.copytree(ROOT / name, root / name)
            (root / "tools").mkdir()
            for path in ("README.md", "plugin.json", "package.json", "CMakeLists.txt", checker):
                shutil.copyfile(ROOT / path, root / path)
            command = ["node", str(root / checker)]
            # Exercise a Windows checkout even when the test runs on Unix.
            for relative in ("skills/sqrail/SKILL.md", "site/help.txt"):
                path = root / relative
                path.write_bytes(path.read_text(encoding="utf-8").replace("\n", "\r\n").encode())
            valid = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(valid.returncode, 0, valid.stderr)

            cmake = root / "CMakeLists.txt"
            original = cmake.read_text(encoding="utf-8")
            version = json.loads((root / "plugin.json").read_text(encoding="utf-8"))["version"]
            changed = original.replace(f"VERSION {version}", f"VERSION {version}.1", 1)
            self.assertNotEqual(original, changed)
            cmake.write_text(changed, encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("CMake project version and AGY plugin version must match", result.stderr)

    def test_sbom_and_archives_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            installed = temporary / "sqrail-v0.3.2-test"
            binary = installed / "bin" / "sqrail"
            license_file = installed / "share" / "doc" / "sqrail" / "LICENSE"
            binary.parent.mkdir(parents=True)
            license_file.parent.mkdir(parents=True)
            binary.write_bytes(b"binary fixture\n")
            binary.chmod(0o755)
            license_file.write_text("MIT\n", encoding="utf-8")
            sbom = installed / "share" / "doc" / "sqrail" / "SBOM.spdx.json"
            subprocess.run(
                [
                    sys.executable,
                    str(SBOM),
                    "--root",
                    str(installed),
                    "--output",
                    str(sbom),
                    "--version",
                    "0.3.2",
                    "--target",
                    "test",
                    "--duckdb-version",
                    "v1.5.5",
                    "--duckdb-revision",
                    "d8cdaa33fda8df955cc76ef58a280f68f4cd43fa",
                    "--epoch",
                    str(EPOCH),
                ],
                check=True,
            )
            document = json.loads(sbom.read_text(encoding="utf-8"))
            self.assertEqual(document["spdxVersion"], "SPDX-2.3")
            self.assertEqual(
                [package["name"] for package in document["packages"]],
                ["sqrail", "DuckDB"],
            )
            self.assertEqual(len(document["files"]), 2)

            for suffix in (".tar.gz", ".zip"):
                digests = []
                for index in range(2):
                    archive = temporary / f"archive-{index}{suffix}"
                    subprocess.run(
                        [
                            sys.executable,
                            str(PACKAGE),
                            "--root",
                            str(installed),
                            "--output",
                            str(archive),
                            "--epoch",
                            str(EPOCH),
                        ],
                        check=True,
                    )
                    digests.append(hashlib.sha256(archive.read_bytes()).hexdigest())
                self.assertEqual(digests[0], digests[1])

            with tarfile.open(temporary / "archive-0.tar.gz") as archive:
                names = archive.getnames()
                self.assertTrue(all(not name.startswith("/") for name in names))
                self.assertTrue(all(member.mtime == EPOCH for member in archive.getmembers()))
            with zipfile.ZipFile(temporary / "archive-0.zip") as archive:
                self.assertTrue(
                    all(not name.startswith("/") for name in archive.namelist())
                )

    @unittest.skipUnless(os.environ.get("SQRAIL_BUILD_DIR"), "requires a built CMake tree")
    def test_install_layout(self) -> None:
        datadir = os.environ.get("SQRAIL_INSTALL_DATADIR", "share")
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            subprocess.run(
                [
                    os.environ.get("SQRAIL_CMAKE", "cmake"),
                    "--install",
                    os.environ["SQRAIL_BUILD_DIR"],
                    "--prefix",
                    str(prefix),
                    "--component",
                    "sqrail",
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            completion = prefix / datadir / "bash-completion" / "completions" / "sqrail"
            self.assertEqual(completion.read_bytes(), COMPLETION.read_bytes())
            docdir = prefix / datadir / "doc" / "sqrail"
            for relative in (
                "README.md",
                "CONTRIBUTING.md",
                "SUPPORT.md",
                "CODE_OF_CONDUCT.md",
                "docs/README.md",
                "docs/RECIPES.md",
                "docs/CONTRACT.md",
            ):
                self.assertTrue((docdir / relative).is_file(), relative)
            self.assertTrue(
                (prefix / datadir / "sqrail" / "skills" / "sqrail" / "SKILL.md").is_file()
            )
            link_pattern = re.compile(r"\[[^\]]*\]\(([^)#\s]+\.md)(?:#[^)]*)?\)")
            for markdown in sorted(docdir.rglob("*.md")):
                stripped = re.sub(
                    r"```.*?```", "", markdown.read_text(encoding="utf-8"), flags=re.DOTALL
                )
                for match in link_pattern.finditer(stripped):
                    target = match.group(1)
                    if "://" in target or target.endswith("skills/sqrail/SKILL.md"):
                        continue
                    self.assertTrue(
                        (markdown.parent / target).resolve().is_file(),
                        f"broken installed doc link in {markdown.relative_to(docdir)}: {target}",
                    )


@unittest.skipIf(os.name == "nt" or shutil.which("bash") is None, "Bash completion is POSIX-only")
class BashCompletionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.work = root / "work"
        (self.work / "data").mkdir(parents=True)
        (self.work / "spill dir").mkdir()
        for name in ("data.csv", "other.parquet", "-odd.csv"):
            (self.work / name).touch()
        # A sqrail stub on PATH records any attempt to run the CLI.
        self.marker = root / "sqrail-ran"
        self.bin = root / "bin"
        self.bin.mkdir()
        stub = self.bin / "sqrail"
        stub.write_text(f"#!/bin/sh\ntouch '{self.marker}'\n", encoding="utf-8")
        stub.chmod(0o755)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def complete(self, *words: str) -> list[str]:
        environment = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ.get('PATH', '')}")
        result = subprocess.run(
            ["bash", "-c", COMPLETE, "bash", str(COMPLETION), *words],
            cwd=self.work,
            env=environment,
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(result.stderr, "")
        self.assertFalse(self.marker.exists(), "completion must not run sqrail")
        return result.stdout.splitlines()

    def test_syntax(self) -> None:
        subprocess.run(["bash", "-n", str(COMPLETION)], check=True)

    def test_commands_and_options(self) -> None:
        self.assertEqual(self.complete("sqrail", ""), ["schema", "check", "run", "--help", "--version"])
        self.assertEqual(self.complete("sqrail", "r"), ["run"])
        self.assertEqual(
            self.complete("sqrail", "schema", "-"),
            [
                "--memory",
                "--threads",
                "--timeout",
                "--max-input-files",
                "--strict-schema",
                "--all-varchar",
                "--",
            ],
        )
        self.assertEqual(
            self.complete("sqrail", "check", "-"),
            [
                "-t",
                "--memory",
                "--threads",
                "--timeout",
                "--max-rows",
                "--max-input-files",
                "--max-sql-bytes",
                "--strict-schema",
                "--all-varchar",
                "--",
            ],
        )
        self.assertEqual(
            self.complete("sqrail", "run", "--all-"),
            ["--all-varchar"],
        )
        self.assertEqual(
            self.complete("sqrail", "run", "--max-"),
            ["--max-spill", "--max-rows", "--max-output-bytes", "--max-input-files", "--max-sql-bytes"],
        )
        self.assertEqual(self.complete("sqrail", "run", "--st"), ["--stats", "--strict-schema"])
        for option in (
            "-t",
            "--table",
            "--memory",
            "--threads",
            "--max-spill",
            "--timeout",
            "--max-rows",
        ):
            with self.subTest(option=option):
                self.assertEqual(self.complete("sqrail", "run", option, ""), [])
        self.assertEqual(self.complete("sqrail", "run", "SELECT"), [])
        self.assertEqual(self.complete("sqrail", "--version", ""), [])

    def test_paths(self) -> None:
        self.assertEqual(sorted(self.complete("sqrail", "schema", "da")), ["data", "data.csv"])
        self.assertEqual(sorted(self.complete("sqrail", "schema", "--", "-")), ["-odd.csv"])
        self.assertEqual(sorted(self.complete("sqrail", "schema", "--", "--timeout", "da")), ["data", "data.csv"])
        self.assertEqual(sorted(self.complete("sqrail", "schema", "--", "-t", "da")), ["data", "data.csv"])
        self.assertEqual(self.complete("sqrail", "schema", "--", "-o", "o"), ["other.parquet"])
        self.assertEqual(self.complete("sqrail", "run", "--", "SELECT 1", "-o", "o"), [])
        self.assertEqual(self.complete("sqrail", "run", "--", "SELECT 1", "--spill", ""), [])
        self.assertEqual(sorted(self.complete("sqrail", "run", "--", "-", "<", "da")), ["data", "data.csv"])
        self.assertEqual(
            sorted(self.complete("sqrail", "run", "-t", "sales", "=")),
            ["-odd.csv", "data", "data.csv", "other.parquet", "spill dir"],
        )
        self.assertEqual(
            sorted(self.complete("sqrail", "run", "--table", "sales", "=")),
            ["-odd.csv", "data", "data.csv", "other.parquet", "spill dir"],
        )
        self.assertEqual(self.complete("sqrail", "check", "-t", "sales", "=", "o"), ["other.parquet"])
        self.assertEqual(
            self.complete("sqrail", "check", "--table", "sales", "=", "o"), ["other.parquet"]
        )
        self.assertEqual(self.complete("sqrail", "run", "-o", "o"), ["other.parquet"])
        self.assertEqual(self.complete("sqrail", "run", "--output", "o"), ["other.parquet"])
        self.assertEqual(sorted(self.complete("sqrail", "run", "--spill", "")), ["data", "spill dir"])
        self.assertEqual(sorted(self.complete("sqrail", "run", "-", "<", "da")), ["data", "data.csv"])

    def test_only_a_standalone_option_terminator_ends_completion(self) -> None:
        for prior in (("--spill", "--"), ("-t", "data", "=", "--"), ("<", "--")):
            with self.subTest(prior=prior):
                self.assertEqual(self.complete("sqrail", "run", *prior, "-o", "o"), ["other.parquet"])
                self.assertEqual(self.complete("sqrail", "run", *prior, "--", "-o", "o"), [])


if __name__ == "__main__":
    unittest.main()
