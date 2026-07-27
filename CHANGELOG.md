# Changelog

All notable changes follow [Keep a Changelog](https://keepachangelog.com/) and
versions follow semantic versioning.

## [Unreleased]

### Added

- an identity-concealed, randomized agent evaluation harness for crossed
  sqrail/DuckDB CLI and Codex/Claude task-completion experiments

## [0.2.2] - 2026-07-27

### Added

- a `SYSTEM` DuckDB provider and source-only distribution workflow suitable for
  package managers such as Homebrew Core
- Linux and macOS CI coverage for linking against the packaged DuckDB shared
  library

### Changed

- version output to report the DuckDB library actually linked at runtime
- third-party licence installation so system packages do not duplicate the
  notices owned by their DuckDB dependency

## [0.2.1] - 2026-07-27

### Added

- a 12-workload benchmark matrix across scans, aggregations, joins, sorting,
  distinct, windows, and streaming format conversion
- bounded multi-memory benchmark runs, generation capacity guards, and an
  eight-task agent evaluation protocol
- a CI developer build that verifies the final executable link step

### Changed

- strict JSON streaming to read DuckDB vectors directly, reducing 10M-row
  Parquet-to-JSONL elapsed time by 63% while retaining RFC 8259 normalization
- the development container to the current major C++ image definition
- GitHub Pages actions to their current major releases

### Fixed

- sanitizer ownership between sqrail and embedded DuckDB so debug and
  sanitizer builds instrument and link the complete process consistently

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

[Unreleased]: https://github.com/yhay81/sqrail/compare/v0.2.2...HEAD
[0.2.2]: https://github.com/yhay81/sqrail/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/yhay81/sqrail/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/yhay81/sqrail/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yhay81/sqrail/releases/tag/v0.1.0
