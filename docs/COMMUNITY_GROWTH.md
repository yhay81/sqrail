# Community growth plan

sqrail should grow by making a narrow promise, proving it, and helping new
contributors ship useful work quickly. This is a public working plan: results
and community feedback should change it.

## Positioning

**sqrail is the bounded SQL-on-files execution layer for coding agents that
already know how to write SQL.**

It is not a natural-language database, a new query engine, or a claim to
outperform DuckDB. It packages DuckDB-class execution behind a small,
machine-readable contract with explicit inputs, read-only SQL, resource limits,
structured errors, atomic outputs, and no hidden configuration.

The first audiences are:

1. coding-agent and automation builders who need a predictable local data tool;
2. data engineers who want reproducible CSV, JSON, and Parquet transformations;
3. CLI users who value one portable binary and stdout/stderr discipline; and
4. security and platform engineers evaluating bounded agent capabilities.

## The contribution funnel

| Stage      | What the project must provide                                           | Primary signal                      |
| ---------- | ----------------------------------------------------------------------- | ----------------------------------- |
| Discover   | an honest one-sentence promise and a memorable live demo                | qualified visits                    |
| Evaluate   | three-minute install, contract, safety model, and reproducible evidence | installs and release downloads      |
| Activate   | one successful real-file query with no special SDK                      | successful first queries            |
| Contribute | bounded issues, validation commands, kind review, and fast feedback     | first-time contributors             |
| Retain     | predictable releases, public decisions, and visible contributor credit  | repeat contributors and discussions |

The website, README, `--help`, Agent Skill, packages, and release artifacts
must tell the same story. Unsupported “faster than DuckDB/Polars” claims would
damage trust; performance statements require equivalent-work checks and raw
evidence under the benchmark policy.

## 90-day program

### Days 0–14: open the doors

- Enable GitHub Discussions with a Q&A route and a concrete welcome prompt.
- Maintain eight small `good first issue` tasks with acceptance criteria and
  validation commands.
- Publish one 60-second terminal demo: inspect a schema, check a plan, run a
  bounded transformation, and show structured statistics.
- Launch to developer communities with audience-specific copy instead of one
  generic announcement.
- Reply to questions and first-time pull requests within two working days.

### Days 15–45: prove real use

- Publish three reproducible case studies: agent data preparation, large
  Parquet filtering, and cross-format validation.
- Record installation friction by platform and remove the most common failure.
- Turn repeated Q&A answers into recipes or documentation.
- Complete Homebrew and WinGet distribution work and keep release provenance
  visible.
- Invite early users to add one sentence about the task they completed—not a
  generic testimonial.

### Days 46–90: build a durable loop

- Refresh benchmark evidence on a fixed schedule and archive raw results.
- Publish a monthly roadmap discussion with shipped, next, and declined items.
- Highlight contributors in release notes and a website community section.
- Seek one integration with an agent framework or developer-tool workflow.
- Keep the first-contribution queue stocked without manufacturing low-value
  chores.

## Launch channels

Every post should lead with a reproducible task and disclose that DuckDB is the
execution engine.

- **GitHub Discussions:** home for welcome, Q&A, roadmap, and user recipes.
- **Show HN:** use the required `Show HN:` title format and link to something
  people can try immediately. Proposed title:
  `Show HN: sqrail – Bounded SQL over local files for coding agents`.
- **r/commandline:** emphasize one binary, stdin/stdout behavior, and portable
  installation.
- **r/dataengineering:** emphasize explicit bindings, Parquet, resource limits,
  and equivalent-work benchmarks.
- **Agent-builder communities:** publish the complete 170-word interface and a
  blind low-cost-model evaluation, including failures.
- **Developer-tool curators:** submit only after the quick start and platform
  packages are verified. Console's published criteria favor self-service,
  documented, high-quality, multiplatform tools.

Useful references:

- [GitHub Discussions documentation](https://docs.github.com/en/discussions/managing-discussions-for-your-community/managing-discussions)
- [GitHub repository best practices](https://docs.github.com/en/repositories/creating-and-managing-repositories/best-practices-for-repositories)
- [Show HN guidelines](https://news.ycombinator.com/showhn.html)
- [Console selection criteria](https://console.dev/selection-criteria)
- [Terminal Trove](https://terminaltrove.com/about/)

## Ready-to-adapt launch copy

> Coding agents are good at writing SQL, but a general database CLI exposes
> more surface area than many file-transformation jobs need. sqrail binds named
> CSV, JSON, or Parquet inputs, accepts exactly one read-only query, and returns
> JSONL or an atomic output file. It adds explicit memory, thread, timeout, row,
> byte, file, SQL-size, and spill limits around DuckDB. The interface fits in
> 170 words and ships as one C++20 binary for macOS, Linux, and Windows. I would
> especially value reports from real agent workflows where the contract is
> unclear or a limit is missing.

Change the opening example and level of detail for each community; do not
cross-post identical text or ask for stars.

## Measures

Review monthly, using trends rather than vanity targets:

- qualified website visits and documentation completion;
- release downloads and package installs where counters are available;
- successful first-query reports and installation failures by platform;
- stars and forks as weak awareness signals, not product success;
- first-time and repeat contributors;
- median time to first maintainer response and first review;
- Q&A resolution rate and recurring documentation gaps; and
- reproducible external workflows, case studies, or integrations.

## Community operating rules

- Be precise about the security boundary and benchmark scope.
- Prefer a small reviewed contribution over an ambitious unreviewable one.
- Give a reason when declining a feature and suggest an external composition
  when possible.
- Never use fake activity, unsolicited mass outreach, or empty issues to inflate
  metrics.
- Credit contributors visibly and make decisions in public unless security or
  privacy requires otherwise.
