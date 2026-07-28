# Testing architecture

sqrail separates native product tests from Python-based integration and
research tooling. A contributor can build and test the C++ product without a
Python interpreter, while repository CI still exercises every layer.

## Test layers

| Layer            | CMake option                      | Runtime                | Responsibility                                                   |
| ---------------- | --------------------------------- | ---------------------- | ---------------------------------------------------------------- |
| native unit      | `SQRAIL_BUILD_TESTS`              | C++20 + Catch2         | CLI parsing, limits, query normalization, strict JSON            |
| CLI integration  | `SQRAIL_BUILD_INTEGRATION_TESTS`  | Python 3; Bash on Unix | real processes, files, signals, races, limits, platform behavior |
| agent evaluation | `SQRAIL_BUILD_AGENT_TESTS`        | Python 3               | blinded experiment runner, oracle, reports, environment capture  |
| release tools    | `SQRAIL_BUILD_RELEASE_TOOL_TESTS` | Python 3               | deterministic archives and SPDX SBOM generation                  |

`SQRAIL_BUILD_TESTS` defaults to `ON`. The three Python-backed layers default
to `OFF`, so an ordinary source configure does not require a Python
interpreter.

Catch2 v3.15.3 is fetched at an immutable commit when native tests are enabled.
It is a build-only dependency: neither the sqrail executable nor release
archives contain the test framework.

## Workflows

Run only native product tests:

```sh
cmake --workflow --preset core
```

Run the full developer suite:

```sh
cmake --workflow --preset dev
```

The `dev`, `release`, `strict`, `sanitize`, `thread`, `system`, and Windows
presets inherit the full test configuration used by CI. The `fuzz` preset turns
off every other test layer and builds only the two libFuzzer targets.

For a production-only build, disable native tests too:

```sh
cmake -S . -B out/build/product -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSQRAIL_BUILD_TESTS=OFF
cmake --build out/build/product --target sqrail
```

## Placement rules

- Put deterministic functions and parser behavior in a Catch2 test under
  `tests/*.cpp`.
- Put portable executable-level behavior in `cross_platform_test.py`.
- Put POSIX signal, permission, pipe, and spill cleanup behavior in `smoke.sh`.
- Put tests for `benchmarks/*.py` beside the existing agent-evaluation test
  group.
- Put release archive and SBOM checks in `package_tools_test.py`.

CTest remains the single entry point for all configured layers. Test names use
stable `sqrail-` prefixes so a preset or CI job can select a responsibility
without depending on source-file layout.
