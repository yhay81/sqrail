#!/usr/bin/env python3
"""Generate a deterministic SPDX 2.3 SBOM for an installed sqrail tree."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--duckdb-version", required=True)
    parser.add_argument("--duckdb-revision", required=True)
    parser.add_argument(
        "--epoch",
        type=int,
        default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")),
        help="UTC creation timestamp; defaults to SOURCE_DATE_EPOCH",
    )
    return parser.parse_args()


def digest(path: Path, algorithm: str) -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main() -> int:
    arguments = parse_args()
    root = arguments.root.resolve()
    output = arguments.output.resolve()
    if not root.is_dir():
        raise ValueError(f"installed root is not a directory: {root}")
    if arguments.epoch <= 0:
        raise ValueError("--epoch or SOURCE_DATE_EPOCH must be a positive Unix timestamp")
    if output != root and root not in output.parents:
        raise ValueError("SBOM output must be inside the installed root")

    files: list[tuple[str, Path, str, str]] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"installed tree contains a symlink: {path}")
        if not path.is_file() or path == output:
            continue
        relative = "./" + path.relative_to(root).as_posix()
        files.append((relative, path, digest(path, "sha256"), digest(path, "sha1")))
    if not files:
        raise ValueError("installed tree contains no files")

    verification = hashlib.sha1(
        "".join(sorted(item[3] for item in files)).encode("ascii")
    ).hexdigest()
    identity = hashlib.sha256()
    for relative, _, sha256_value, _ in files:
        identity.update(relative.encode("utf-8"))
        identity.update(bytes.fromhex(sha256_value))
    identity.update(arguments.version.encode())
    identity.update(arguments.target.encode())
    namespace_digest = identity.hexdigest()
    created = dt.datetime.fromtimestamp(arguments.epoch, tz=dt.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )

    file_entries = []
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": "SPDXRef-Package-sqrail",
        },
        {
            "spdxElementId": "SPDXRef-Package-sqrail",
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": "SPDXRef-Package-duckdb",
        },
    ]
    for index, (relative, _, sha256_value, _) in enumerate(files):
        identifier = f"SPDXRef-File-{index + 1}-{sha256_value[:12]}"
        file_entries.append(
            {
                "SPDXID": identifier,
                "fileName": relative,
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": sha256_value}
                ],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-Package-sqrail",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": identifier,
            }
        )

    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"sqrail-{arguments.version}-{arguments.target}",
        "documentNamespace": (
            "https://github.com/yhay81/sqrail/sbom/"
            f"{arguments.version}/{arguments.target}/{namespace_digest}"
        ),
        "creationInfo": {
            "created": created,
            "creators": ["Tool: sqrail-generate-sbom/1"],
        },
        "packages": [
            {
                "name": "sqrail",
                "SPDXID": "SPDXRef-Package-sqrail",
                "versionInfo": arguments.version,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "packageVerificationCode": {
                    "packageVerificationCodeValue": verification
                },
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": (
                            f"pkg:github/yhay81/sqrail@{arguments.version}"
                        ),
                    }
                ],
            },
            {
                "name": "DuckDB",
                "SPDXID": "SPDXRef-Package-duckdb",
                "versionInfo": arguments.duckdb_version,
                "downloadLocation": (
                    "git+https://github.com/duckdb/duckdb.git@"
                    + arguments.duckdb_revision
                ),
                "filesAnalyzed": False,
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": (
                            "pkg:github/duckdb/duckdb@"
                            + arguments.duckdb_revision
                        ),
                    }
                ],
            },
        ],
        "files": file_entries,
        "relationships": relationships,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"generate-sbom: {error}", file=sys.stderr)
        sys.exit(2)
