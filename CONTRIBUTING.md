# Contributing

sqrail welcomes focused bug reports, reproducible performance evidence, and
small changes that preserve its narrow agent-facing contract.

Every contribution matters: a minimal reproducer, a documentation correction,
an unfamiliar platform report, a benchmark, or a focused patch can all improve
the project. You do not need permission to open an issue or submit a small pull
request. Please follow the [Code of Conduct](CODE_OF_CONDUCT.md), and use the
[support guide](SUPPORT.md) to choose the right reporting path.

## Your first contribution

1. Search open issues and pull requests to avoid duplicate work.
2. For a non-trivial change, open a feature proposal before investing heavily
   so the public contract and scope can be discussed early.
3. Fork the repository, branch from the current `main`, and keep one logical
   change per branch.
4. Add tests or reproducible evidence, run the relevant checks below, and open a
   pull request using the repository template.

Issues labeled
[`good first issue`](https://github.com/yhay81/sqrail/labels/good%20first%20issue)
have bounded scope suitable for a first contribution. If an issue is unclear,
asking a focused question is useful work too.

### Claiming an issue

Comment `/take` on an unassigned issue if you want to work on it. A maintainer
will normally assign it for seven days; ask for more time whenever you need it.
If there is no update after that window, the issue may be opened to another
contributor. This is coordination, not permission: small pull requests are also
welcome without prior assignment.

## Before proposing a feature

The project deliberately has one engine, one SQL dialect, no natural-language
layer, no daemon, and no configuration file. A feature proposal should explain
why ordinary DuckDB SQL or an existing command option cannot solve the task and
how an agent can learn the addition without making `--help` ambiguous.

## Build and test

```sh
cmake --workflow --preset core
cmake --workflow --preset dev
cmake --workflow --preset strict
cmake --workflow --preset sanitize
cmake --workflow --preset thread
cmake --preset fuzz
cmake --build --preset fuzz
out/build/fuzz/sqrail-strict-json-fuzz -max_total_time=30
out/build/fuzz/sqrail-cli-fuzz -max_total_time=30
CMAKE_PREFIX_PATH=/path/to/duckdb cmake --workflow --preset system
clang-format-18 --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp fuzz/*.cpp \
  benchmarks/agent-eval/launcher.c
shellcheck tests/*.sh benchmarks/*.sh
actionlint
GH_TOKEN=$(gh auth token) zizmor --pedantic .
```

Format C++ with the repository `.clang-format`. CI enforces the same check with
`clang-format` 18, so a different major version can disagree; `uvx
clang-format@18.1.8` provides it when your platform packages another release.
Give Zizmor a `GH_TOKEN` so it runs the online audits CI performs, including the
action reference checks a local run otherwise skips. GitHub Actions additionally
run CodeQL and audit every workflow with Actionlint and Zizmor. The `windows-x64`
preset follows the Windows 2025 hosted image and uses Visual Studio 2026 with
a CMake version that provides the `Visual Studio 18 2026` generator; the Windows
11 Arm64 runner continues to use Visual Studio 2022 through the `windows-arm64`
preset. Both select the static MSVC runtime and the Release configuration.

The `core` workflow runs Catch2-based native unit tests without Python. The
other development workflows enable every optional suite. Behavioral changes
must keep the cross-platform Python end-to-end suite portable. POSIX-only
signal, mode, or filesystem assertions belong in the Bash smoke suite or behind
an explicit platform condition. The boundaries and CMake options are documented
in [docs/TESTING.md](docs/TESTING.md).

## Performance changes

Performance claims must follow [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
Include the dataset generator parameters, environment metadata, raw hyperfine
JSON, peak RSS, row counts, and logical checksums. A faster result with different
output is a correctness failure.

## Pull requests

- Keep unrelated changes separate.
- Add a regression test for behavioral fixes.
- Update `docs/CONTRACT.md` and `--help` together when the public CLI
  changes.
- Do not update the DuckDB revision without documenting and benchmarking the
  change.
- Confirm that `git diff --check`, `clang-format`, the smoke test, ShellCheck,
  Actionlint, and Zizmor pass.

Reviews focus on the documented contract, correctness, safety, portability,
performance evidence, and whether an agent can learn the interface reliably.
Feedback should explain the technical reason and, where possible, a concrete
path forward. Contributions remain credited in Git history; substantial
user-visible changes may also be acknowledged in release notes.
