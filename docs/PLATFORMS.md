# Platform support

sqrail's portable contract is the same on supported operating systems. A
platform is called supported only after the bundled build, unit tests,
Unicode-path end-to-end tests, interruption cleanup, no-overwrite race, install
layout, and release packaging all pass on that target.

## Tiers

| Tier      | Target                                            | CI runner                              | Release archive    |
| --------- | ------------------------------------------------- | -------------------------------------- | ------------------ |
| 1         | Linux x86-64                                      | Ubuntu 24.04 x64                       | `.tar.gz`          |
| 1         | Linux Arm64                                       | Ubuntu 24.04 Arm64                     | `.tar.gz`          |
| 1         | macOS x86-64                                      | macOS 15 Intel                         | `.tar.gz`          |
| 1         | macOS Arm64                                       | macOS 15 Arm64                         | `.tar.gz`          |
| 1         | Windows x86-64                                    | Windows 2025 x64                       | `.zip`             |
| 1 preview | Windows Arm64                                     | Windows 11 Arm64 public-preview runner | `.zip`             |
| 2         | Linux/macOS with a packaged DuckDB shared library | Ubuntu 24.04 and macOS 15              | distribution-owned |

Windows Arm64 remains preview until GitHub's hosted runner is generally
available and repeated release builds establish equivalent reliability. Other
POSIX systems and other C++20 toolchains may build from source, but are
best-effort until they have a maintained CI lane. No FreeBSD, musl, 32-bit, or
big-endian release artifact is currently claimed.

## Filesystem and process semantics

- POSIX output files use owner read/write permissions and spill workspaces use
  owner-only directory permissions.
- Windows output files and spill workspaces receive a protected DACL granting
  the current user full access without inherited access rules.
- POSIX commits use a same-directory hard link followed by removal of the
  private temporary name. Filesystems without hard-link support fail closed.
- Windows commits use a no-replace, write-through move.
- Windows binaries opt into UTF-8 process paths and long-path awareness.
- `SIGINT`/`SIGTERM` are supported on POSIX. Windows console Ctrl+C and
  Ctrl+Break are translated to the same structured interruption path.

Network filesystems can weaken durability or atomicity guarantees despite
successful system calls. Release tests cover the hosted runner's local
filesystem; applications requiring crash-consistent remote storage should
stage to local disk and copy the completed artifact separately.

## What “tested” means

The repository defines the matrix, but a local checkout alone cannot prove
every target. Before a release tag, all six bundled lanes, the system-DuckDB
lanes, Clang static analysis, ASan/UBSan, ThreadSanitizer, both fuzz targets,
and package reproducibility checks must be green for the exact commit. A
signed annotated tag then rebuilds and attests each archive.
