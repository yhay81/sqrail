#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
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
EPOCH = 1_700_000_000


class PackageToolsTest(unittest.TestCase):
    def test_sbom_and_archives_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            installed = temporary / "sqrail-v0.3.1-test"
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
                    "0.3.1",
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


if __name__ == "__main__":
    unittest.main()
