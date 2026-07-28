# sqrail documentation

> Bind a file. Check the plan. Run one read-only query.

[Website](https://sqrails.yhay81.com) ·
[3-minute guide](https://sqrails.yhay81.com/docs/) ·
[Source](https://github.com/yhay81/sqrail) ·
[Releases](https://github.com/yhay81/sqrail/releases)

## Start with the question

| I need to…                        | Read                                               |
| --------------------------------- | -------------------------------------------------- |
| install and run a first query     | [3-minute guide](https://sqrails.yhay81.com/docs/) |
| integrate sqrail with an agent    | [Agent integration](AGENT_INTEGRATION.md)          |
| depend on exact CLI behavior      | [CLI contract](CONTRACT.md)                        |
| understand supported systems      | [Platform support](PLATFORMS.md)                   |
| evaluate performance claims       | [Benchmark policy](BENCHMARKS.md)                  |
| reproduce historical measurements | [Performance baselines](BASELINE.md)               |
| review the v0.3 release criteria  | [v0.3 release gate](V0.3_RELEASE.md)               |
| validate or deploy the website    | [Documentation deployment](DEPLOYMENT.md)          |
| understand or run the test suites | [Testing architecture](TESTING.md)                 |
| understand the original design    | [Concept](CONCEPT.md) / [日本語](CONCEPT.ja.md)    |
| report a vulnerability            | [Security policy](../SECURITY.md)                  |
| contribute a change               | [Contributing guide](../CONTRIBUTING.md)           |

## Contract hierarchy

The executable's `--agent-help` text is the short operational interface.
[CONTRACT.md](CONTRACT.md) is normative when the short help omits detail.
Release artifacts and observed behavior must agree with that contract.

Performance documents make evidence-scoped claims. A baseline measured on one
machine is not a promise for another machine; follow the benchmark policy and
committed harness before comparing tools.

## For coding agents

The smallest stable context is:

```text
Run `sqrail --agent-help`.
If names and types are unknown, use `sqrail schema` once.
Otherwise trust the stated schema, run once, and stop after success.
```

The website also publishes the same interface as
[`agent-help.txt`](https://sqrails.yhay81.com/agent-help.txt) and publishes an
LLM discovery index at
[`llms.txt`](https://sqrails.yhay81.com/llms.txt).
