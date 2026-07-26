# Contributing

sqrail welcomes focused bug reports, reproducible performance evidence, and
small changes that preserve its narrow agent-facing contract.

## Before proposing a feature

The project deliberately has one engine, one SQL dialect, no natural-language
layer, no daemon, and no configuration file. A feature proposal should explain
why ordinary DuckDB SQL or an existing command option cannot solve the task and
how an agent can learn the addition without making `--agent-help` ambiguous.

## Build and test

```sh
cmake --workflow --preset dev
cmake --workflow --preset strict
cmake --workflow --preset sanitize
cmake --preset fuzz
cmake --build --preset fuzz
out/build/fuzz/sqrail-strict-json-fuzz -max_total_time=30
shellcheck tests/*.sh benchmarks/*.sh
actionlint
zizmor --pedantic .
```

Format C++ with the repository `.clang-format`. GitHub Actions additionally run
CodeQL and audit every workflow with Actionlint and Zizmor.

## Performance changes

Performance claims must follow [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
Include the dataset generator parameters, environment metadata, raw hyperfine
JSON, peak RSS, row counts, and logical checksums. A faster result with different
output is a correctness failure.

## Pull requests

- Keep unrelated changes separate.
- Add a regression test for behavioral fixes.
- Update `docs/CONTRACT.md` and `--agent-help` together when the public CLI
  changes.
- Do not update the DuckDB revision without documenting and benchmarking the
  change.
- Confirm that `git diff --check`, the smoke test, ShellCheck, Actionlint, and
  Zizmor pass.
