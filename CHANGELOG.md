# Changelog

All notable changes follow [Keep a Changelog](https://keepachangelog.com/) and
versions follow semantic versioning.

## [Unreleased]

## [0.2.0] - 2026-07-26

### Added

- file globs and recursively partitioned Parquet datasets with Hive columns
- non-executing JSON physical plans through `sqrail check`
- query deadlines and bounded explicit spill storage
- strict RFC 8259 JSON for non-finite values and streaming JSON file output
- CMake workflows for static analysis, sanitizers, fuzzing, native builds, IPO,
  and instrumentation PGO
- pinned LLVM 18 development container and CodeQL v4 scanning

### Security

- restrict DuckDB file access to canonical bound inputs, private output
  temporaries, and a per-process private spill workspace
- disable extension auto-install/autoload and lock engine configuration
- preserve valid diagnostic JSON when operating-system arguments contain
  malformed UTF-8

## [0.1.0] - 2026-07-26

### Added

- one-command schema inspection for CSV, TSV, JSON, and Parquet
- named file bindings for one read-only DuckDB query
- streaming JSONL stdout and atomic file output
- gzip and zstd support for text input and output
- explicit memory, thread, and spill controls
- stable JSON diagnostics and exit codes
- compact normative `--agent-help`
- deterministic smoke tests and same-engine comparison benchmarks
- Linux and macOS release archives for x86-64 and Arm64
- SHA-256 checksums and build-provenance attestations

[0.2.0]: https://github.com/yhay81/sqrail/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yhay81/sqrail/releases/tag/v0.1.0
