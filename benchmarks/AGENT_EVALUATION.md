# Identity-concealed agent evaluation

This protocol tests sqrail's product claim: a coding agent that already writes
SQL can complete file tasks from a short, deterministic CLI contract. It does
not test natural-language-to-SQL quality or claim that sqrail has a faster query
engine than DuckDB.

## Experimental design

The runner builds a crossed design from the selected factors:

| Factor | Levels |
|---|---|
| Agent runtime | Codex; Claude Code; Antigravity CLI (`agy`); optional Codex-to-Ollama/LM Studio adapter |
| Data CLI | sqrail; DuckDB CLI built from the same DuckDB version |
| Context | clean; noisy workspace; superseded handoff; prior-error recovery |
| Task | eight deterministic file, query, resource, and failure tasks |

Every agent, tool, task, and repetition runs in a fresh session and working
directory. The runner randomizes execution order and file names from a recorded
seed. Model, reasoning effort, turn limits, task text, data, and wall-clock
limit remain fixed within a cohort. Run at least five repetitions before using
rates for a public comparison.

Each live agent works from a neutral temporary path outside the repository and
results tree, so CLI initialization metadata cannot reveal a product name from
the current directory. After deterministic scoring, the runner archives that
workspace and its private invocation log under the opaque run ID.

The machine-readable corpus in
[`agent-tasks.json`](agent-tasks.json) contains eight tasks: schema discovery,
streaming JSONL, a join and aggregation, two format conversions, bounded
out-of-core sorting, timeout recovery, and no-overwrite behavior.
It also defines four controlled context profiles that exercise realistic
workspace noise, stale conversation state, and recovery from a failed command
without changing the task oracle.

## Blinding controls

The agent sees only an execute-only launcher called `./rail`. The launcher:

- conceals the underlying executable path and reports a neutral version;
- replaces product names in the tool's real help output with `rail`;
- disables the DuckDB user's init file;
- records invocations in a private log for machine scoring and refuses to run
  the concealed tool if that audit log cannot be written.

The prompt never names either product or says that a comparison is in progress.
Opaque condition labels are randomized. Before execution, the runner publishes
only a SHA-256 commitment and stores the mapping in a private resume file,
separate from `raw.jsonl`. Scoring is deterministic and completes before the
runner reveals the mapping as `allocation.json`.

This is **identity concealment**, not a double-blind clinical design. The two
interfaces remain behaviorally distinguishable, the host operator can access
the allocation, and other host-installed executables are not physically
removed. Inspecting the launcher, searching for installed data tools, using the
network, or invoking another data-processing runtime is recorded as a protocol
violation. For stronger isolation, run the same harness on a disposable VM
whose image contains only the selected launcher target.

## Run

Build sqrail and its same-source DuckDB comparison shell:

```sh
cmake -S . -B build-agent-eval \
  -DCMAKE_BUILD_TYPE=Release \
  -DSQRAIL_BUILD_DUCKDB_SHELL=ON
cmake --build build-agent-eval --target sqrail shell --parallel 2
```

Generate the deterministic one-million-row cohort:

```sh
BENCH_ROWS=1000000 BENCH_DIM_ROWS=100000 \
  benchmarks/generate.sh \
  benchmark-data-agent \
  build-agent-eval/_deps/duckdb-build/duckdb
```

Before spending model budget, materialize and inspect the randomized 960-run
plan:

```sh
python3 benchmarks/agent-eval/run.py \
  --data-dir benchmark-data-agent \
  --results-dir benchmark-results-agent-plan \
  --sqrail-bin build-agent-eval/sqrail \
  --duckdb-bin build-agent-eval/_deps/duckdb-build/duckdb \
  --agents codex,claude,gemini \
  --codex-model gpt-5.6-luna \
  --codex-effort high \
  --claude-model claude-sonnet-5 \
  --claude-effort high \
  --agy-model 'Gemini 3.6 Flash (High)' \
  --contexts clean,noisy_workspace,superseded_handoff,prior_error \
  --repetitions 5 \
  --plan-only
```

Use a new results path for the real run:

```sh
python3 benchmarks/agent-eval/run.py \
  --data-dir benchmark-data-agent \
  --results-dir benchmark-results-agent \
  --sqrail-bin build-agent-eval/sqrail \
  --duckdb-bin build-agent-eval/_deps/duckdb-build/duckdb \
  --agents codex,claude,gemini \
  --codex-model gpt-5.6-luna \
  --codex-effort high \
  --claude-model claude-sonnet-5 \
  --claude-effort high \
  --claude-max-budget-usd 1.0 \
  --agy-model 'Gemini 3.6 Flash (High)' \
  --contexts clean,noisy_workspace,superseded_handoff,prior_error \
  --repetitions 5
```

If a long cohort is interrupted, rerun the exact command with `--resume`.
Completed run IDs are skipped, an incomplete run directory is safely rebuilt,
and any changed model, prompt, help, dataset, limit, or schedule causes a hard
failure instead of silently mixing cohorts.

The Claude CLI reports the resolved model identifier in each transcript. The
runner also extracts Agy's resolved model label rather than assuming its CLI
selector stayed fixed, and reports model-selection mismatches. This catches
aliases that silently resolve to a different price/performance tier. Agy shell
calls remain in its structured Antigravity transcript; the runner copies that
transcript for protocol auditing and removes the separate runtime log, which
can contain account metadata. All selected agent CLIs must already be
authenticated. The runner never reads or records credentials.

The default `--min-free-gib 2` preflight prevents a low-space run from silently
losing evidence. Provider authentication, unavailable-model, overload, and
audit-log failures are infrastructure outcomes; repair the environment and
resume rather than counting them as model failures.

A one-repetition, smaller-data run is useful only as a harness pilot:

```sh
BENCH_ROWS=100000 BENCH_DIM_ROWS=10000 \
  benchmarks/generate.sh \
  benchmark-data-agent-pilot \
  build-agent-eval/_deps/duckdb-build/duckdb

python3 benchmarks/agent-eval/run.py \
  --data-dir benchmark-data-agent-pilot \
  --results-dir benchmark-results-agent-pilot \
  --sqrail-bin build-agent-eval/sqrail \
  --duckdb-bin build-agent-eval/_deps/duckdb-build/duckdb \
  --agents codex,claude,gemini \
  --codex-model gpt-5.6-luna \
  --codex-effort high \
  --claude-model claude-sonnet-5 \
  --claude-effort high \
  --agy-model 'Gemini 3.6 Flash (High)' \
  --contexts clean,noisy_workspace,superseded_handoff,prior_error \
  --repetitions 1
```

Never merge pilot and full-cohort results or mix runs after a CLI, model alias,
model version, prompt, help text, dataset, or runner change.

## Recorded evidence

Each run retains:

- the exact prompt, context profile, opaque condition, randomized schedule,
  and seed;
- agent and data-tool CLI versions, configured and resolved model IDs,
  model-selection match, and reasoning effort;
- raw agent event transcript (including Agy's structured Antigravity
  transcript) and runner stderr;
- input/output token counts when the provider reports them;
- wall time, agent exit, timeout state, help calls, and data-tool calls;
- artifact byte counts and SHA-256 digests;
- task-oracle details, safety violations, and exact protocol-violation reasons.

Large task artifacts are deleted after hashing and logical scoring. Inputs are
hard-linked when possible and checked byte-for-byte after every run.
`raw.jsonl`, `allocation.json`, `environment.json`, `summary.json`, and
`SUMMARY.md` are sufficient to audit the aggregate result. Preserve the
generated dataset manifest and the exact source commit. `environment.json`
binds resume compatibility to SHA-256 digests of the runner, launcher, task
corpus, all dataset files, both data CLIs, and every selected agent CLI. The
generated summary reports overall and per-task rates, 95% Wilson intervals,
per-context rates, model-selection mismatches, paired outcome counts, and the
exact McNemar test for discordant sqrail/DuckDB outcomes. Provider cost is
reported only when the CLI supplies it.

An attempt succeeds only when the final artifact passes the hidden row-count,
logical-checksum, ordering, resource, exit-code, diagnostic, and preservation
checks relevant to that task. Agent-process success alone never counts.

## Decision rule

Before expanding the CLI, the full sqrail cohort should achieve:

- at least 90% machine-scored success;
- zero input mutation, partial-output acceptance, overwrite, timeout, or spill
  safety violations;
- no task with lower success than the same model's DuckDB CLI arm.

Treat lower token use, wall time, or fewer calls as a product advantage only
when task success and safety are non-inferior. Five repetitions are an
engineering gate, not enough on their own for a broad statistical claim.

## Complementary v0.3 artifact gate

The identity-concealed harness above remains the comparative experiment and
uses [`agent-tasks.json`](agent-tasks.json). The v0.3 release gate adds four
contract-specific cases—check metadata, result limits, schema evolution, and
structured statistics—in a separate twelve-task corpus:
[`agent-tasks-v0.3.json`](agent-tasks-v0.3.json).

`agent-run.py` prepares randomized workspaces, copies inputs read-only, binds
the task corpus, runner, oracle, evaluator, and exact sqrail/DuckDB hashes,
filters inherited environment secrets, rejects external paths and URIs, and
executes model-produced argument arrays without a shell. The independent
oracle recomputes task results from the retained stdout, stderr, files,
snapshots, dataset, and pinned binaries.

For the DuckDB timeout-recovery arm, the runner interprets only the documented
`["timeout", "10ms", "duckdb", ...]` command-plan form itself. This keeps the
10 ms deadline portable and reproducible without depending on an unpinned
platform `timeout` executable. The oracle independently checks the requested
duration and the non-zero result.

Prepare and execute each attempt with:

```sh
python3 benchmarks/agent-run.py prepare \
  --artifact-root agent-artifacts \
  --artifact-id run-001-sqrail-join-1 \
  --data benchmark-data \
  --sqrail build-agent-eval/sqrail \
  --duckdb build-agent-eval/_deps/duckdb-build/duckdb \
  --model provider/model-version \
  --arm sqrail \
  --task join_aggregate \
  --run-id run-001 \
  --attempt 1

python3 benchmarks/agent-run.py execute \
  --artifact-root agent-artifacts \
  --artifact-id run-001-sqrail-join-1 \
  --candidate candidate.json \
  --data benchmark-data \
  --sqrail build-agent-eval/sqrail \
  --duckdb build-agent-eval/_deps/duckdb-build/duckdb \
  >> agent-results.jsonl
```

After at least five paired, independent repetitions per model, arm, and task,
recompute all evidence and apply the release gate:

```sh
python3 benchmarks/agent-eval.py agent-results.jsonl \
  --artifacts agent-artifacts \
  --data benchmark-data \
  --sqrail build-agent-eval/sqrail \
  --duckdb build-agent-eval/_deps/duckdb-build/duckdb \
  > agent-evaluation-report.json
```

Synthetic or replayed successes are not release evidence.

Historical contract-refinement pilots and rejected help candidates are kept in
the [experiment log](agent-eval/EXPERIMENTS.md). Those v0.2.2 results motivate
the v0.3 decision rule but do not replace a fresh v0.3 release cohort.
