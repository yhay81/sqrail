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

## Verify a release archive

Every release publishes `SHA256SUMS`, which lists the SHA-256 digest of each
archive and SBOM as `<digest>  ./<asset name>`. Download it next to the archive
and compare the archive with its own entry before extracting it. Each command
prints `OK` only when the digest matches. A mismatch, or an archive name with
no entry, fails with a nonzero exit status; do not extract the archive then.

macOS, with the built-in `shasum`:

```sh
archive=sqrail-v0.4.0-macos-arm64.tar.gz
awk -v name="./$archive" '$2 == name' SHA256SUMS | shasum -a 256 -c -
```

Linux, with GNU coreutils `sha256sum`:

```sh
archive=sqrail-v0.4.0-linux-x86_64.tar.gz
awk -v name="./$archive" '$2 == name' SHA256SUMS | sha256sum -c -
```

Windows PowerShell:

```powershell
$archive = "sqrail-v0.4.0-windows-x86_64.zip"
$entry = Get-Content SHA256SUMS | Where-Object { $_.EndsWith("  ./$archive") }
$actual = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($entry -and $entry.Split(" ")[0] -eq $actual) { "OK $archive" } else { throw "SHA-256 mismatch: $archive" }
```

Each command selects only the entry whose name is exactly `./` followed by the
archive name. Replace the version and target with the archive you downloaded;
the asset names are listed on the
[release page](https://github.com/yhay81/sqrail/releases/latest).

A matching checksum shows that the download is intact, not who built it. Each
archive also has GitHub build-provenance and SPDX SBOM
[attestations](https://github.com/yhay81/sqrail/attestations), its SBOM is
published as `sqrail-<version>-<target>.spdx.json`, and `SHA256SUMS` has its
own provenance attestation. With GitHub CLI:

```sh
gh attestation verify sqrail-v0.4.0-linux-x86_64.tar.gz --repo yhay81/sqrail
gh attestation verify sqrail-v0.4.0-linux-x86_64.tar.gz --repo yhay81/sqrail \
  --predicate-type https://spdx.dev/Document/v2.3
```

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

## Shell completion

On Linux and macOS, `cmake --install` places a static Bash completion file at
`share/bash-completion/completions/sqrail` under the installation prefix. It
completes the three commands and their documented options, and paths after
`schema`, `-t NAME=`, `-o`, and `--spill`. It never runs sqrail, opens data
files, or makes network calls.

bash-completion 2 loads the file on demand when that directory is on its search
path, as it is for the `/usr` and `/usr/local` prefixes. Otherwise, activate it
in the current shell or from `~/.bashrc`:

```sh
source /path/to/prefix/share/bash-completion/completions/sqrail
```
