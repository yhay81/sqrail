#!/usr/bin/env python3
"""Create a deterministic tar.gz or zip archive from one installed directory."""

from __future__ import annotations

import argparse
import gzip
import os
import stat
import sys
import tarfile
import zipfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--epoch",
        type=int,
        default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")),
    )
    return parser.parse_args()


def paths(root: Path) -> list[Path]:
    values = [root]
    values.extend(sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix()))
    for path in values:
        if path.is_symlink():
            raise ValueError(f"release tree contains a symlink: {path}")
    return values


def normalized_mode(path: Path) -> int:
    if path.is_dir():
        return 0o755
    return 0o755 if path.stat().st_mode & stat.S_IXUSR else 0o644


def create_tar(root: Path, output: Path, epoch: int) -> None:
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in paths(root):
                    relative = Path(root.name) if path == root else Path(root.name) / path.relative_to(root)
                    info = archive.gettarinfo(str(path), arcname=relative.as_posix())
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mtime = epoch
                    info.mode = normalized_mode(path)
                    info.pax_headers = {}
                    if path.is_file():
                        with path.open("rb") as handle:
                            archive.addfile(info, handle)
                    else:
                        archive.addfile(info)


def create_zip(root: Path, output: Path, epoch: int) -> None:
    import datetime as dt

    instant = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc)
    if instant.year < 1980:
        instant = dt.datetime(1980, 1, 1, tzinfo=dt.timezone.utc)
    date_time = (instant.year, instant.month, instant.day, instant.hour, instant.minute, instant.second)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in paths(root):
            relative = Path(root.name) if path == root else Path(root.name) / path.relative_to(root)
            name = relative.as_posix() + ("/" if path.is_dir() else "")
            info = zipfile.ZipInfo(name, date_time=date_time)
            info.create_system = 3
            mode = normalized_mode(path)
            if path.is_dir():
                mode |= stat.S_IFDIR
                info.external_attr = (mode << 16) | 0x10
                archive.writestr(info, b"")
            else:
                mode |= stat.S_IFREG
                info.external_attr = mode << 16
                with path.open("rb") as handle:
                    archive.writestr(info, handle.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main() -> int:
    arguments = parse_args()
    root = arguments.root.resolve()
    output = arguments.output.resolve()
    if not root.is_dir():
        raise ValueError(f"release root is not a directory: {root}")
    if arguments.epoch <= 0:
        raise ValueError("--epoch or SOURCE_DATE_EPOCH must be a positive Unix timestamp")
    if output.exists():
        raise ValueError(f"refusing to replace archive: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.name.endswith(".tar.gz"):
        create_tar(root, output, arguments.epoch)
    elif output.suffix.lower() == ".zip":
        create_zip(root, output, arguments.epoch)
    else:
        raise ValueError("output must end in .tar.gz or .zip")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"package-release: {error}", file=sys.stderr)
        sys.exit(2)
