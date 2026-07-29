# Changelog

All notable changes follow [Keep a Changelog](https://keepachangelog.com/) and
versions follow semantic versioning.

## [Unreleased]

## [0.3.2] - 2026-07-30

### Added

- a public Agent Skill discoverable by GitHub CLI, Cursor, Codex, Claude Code,
  AGY/Antigravity, and other Agent Skills-compatible hosts, with a compatibility
  target for enterprise and API-key Gemini CLI deployments
- a native AGY/Antigravity plugin manifest for reliable Skill discovery
- a stable, review-first web bootstrap for agents to install both the sqrail
  executable and its Skill
- release-archive and Homebrew source-build installation of the canonical Skill

### Fixed

- the Antigravity evaluation runner now uses AGY 1.1.8's `--prompt` form and
  explicitly shares its isolated workspace instead of silently running in the
  CLI's private scratch directory

## [0.3.1] - 2026-07-29

### Added

- automated link checking for Markdown, the documentation site, discovery
  files, and GitHub community forms

### Changed

- canonical project and community links from the legacy GitHub Pages deployment
  to `sqrails.yhay81.com`
- GitHub language statistics to identify C++ as the shipped implementation

## [0.3.0] - 2026-07-28

### Added

- an identity-concealed, randomized agent evaluation harness for crossed
  sqrail/DuckDB CLI and Codex/Claude task-completion experiments
- `--max-rows` result guards for execution and validation, with atomic
  destination cleanup on limit failures
- `--stats` success telemetry on stderr with result rows, bytes, elapsed time,
  resolved input count, and destination
- result-column and input-count metadata in `check` JSON
- resource limits and strict multi-file validation for `schema`
- SQL-byte, input-file, and output-byte caps with structured fail-closed
  diagnostics
- an artifact-backed agent-evaluation runner, independent oracle, validator,
  and gate reporter, backed by a twelve-task v0.3 corpus
- a reproducible frontend startup benchmark for version-only and trivial-query
  process paths
- isolated CLI parser unit tests in addition to end-to-end smoke coverage
- Linux, macOS, and Windows x86-64/Arm64 CI and release targets, with Windows
  UTF-8/long-path support and protected output ACLs
- ThreadSanitizer and CLI-parser fuzz lanes
- deterministic archives, SPDX SBOMs, signed-tag enforcement, and provenance
  and SBOM attestations

### Changed

- multi-file CSV, JSON, and Parquet bindings to union evolving columns by name
  unless `--strict-schema` is requested
- file outputs to use owner-only permissions on POSIX systems
- query deadlines to cover input resolution, schema inference, planning,
  execution, and output finalization
- every machine-readable object to carry schema and sqrail versions
- benchmark evidence to record CPU time, output bytes, cache classification,
  binary/dataset digests, host resources, compiler, and git state
- the compact agent contract to document result guards, statistics, schema
  evolution, and option termination
- DuckDB resource limits to accept common `K`, `M`, `G`, and `T` size suffixes
  in addition to the documented SI and IEC forms

### Fixed

- `SIGINT` and `SIGTERM` now interrupt DuckDB work and remove private output and
  spill artifacts
- closed stdout pipes now produce a structured `STDOUT_WRITE` failure instead
  of bypassing cleanup through `SIGPIPE`
- artifact-gate prompts now expose every oracle-specific requirement without
  advertising irrelevant output paths, and bind the runner, oracle, and
  evaluator digests for later verification
- the DuckDB timeout-recovery comparison now applies and verifies its exact
  10 ms deadline without requiring a platform `timeout` executable
- artifact sessions now copy only the independent read-only inputs required by
  their task instead of duplicating the full benchmark dataset

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

[Unreleased]: https://github.com/yhay81/sqrail/compare/v0.3.2...HEAD
[0.3.2]: https://github.com/yhay81/sqrail/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/yhay81/sqrail/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/yhay81/sqrail/compare/v0.2.2...v0.3.0
[0.2.2]: https://github.com/yhay81/sqrail/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/yhay81/sqrail/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/yhay81/sqrail/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/yhay81/sqrail/releases/tag/v0.1.0
