# Install sqrail for an AI agent

Install the sqrail executable first, then install its optional Agent Skill for
the agent that is currently running. Review each command before executing it.
Do not pipe downloaded code into a shell.

## 1. Install the executable

Prefer an existing package manager:

- macOS or Linux with Homebrew:
  `brew install yhay81/tap/sqrail`
- Windows: first run `winget show --id yhay81.sqrail --exact`. If it succeeds,
  run `winget install --id yhay81.sqrail --exact`. If the package is not yet
  available, use the signed release below.
- Other supported systems: select the matching Arm64 or x86-64 archive from
  <https://github.com/yhay81/sqrail/releases/latest>. Verify it with the
  published `SHA256SUMS` or GitHub attestation, extract it, and place
  `bin/sqrail` on `PATH`.

Do not replace an existing installation without the user's permission.

Verify:

```sh
sqrail --version
sqrail run 'SELECT 42 AS answer'
```

The second command must print `{"answer":42}` and exit successfully.

## 2. Install the Agent Skill

For AGY or Antigravity CLI, use its native plugin manager. This route is
verified with AGY 1.1.8:

```sh
agy plugin install https://github.com/yhay81/sqrail
```

For other hosts, GitHub CLI 2.90 or newer is preferred because it records
provenance and can update the Skill later:

```sh
gh skill preview yhay81/sqrail sqrail
gh skill install yhay81/sqrail sqrail --agent AGENT --scope user
```

Replace `AGENT` with the current host. Common values are `codex`,
`claude-code`, `cursor`, `github-copilot`, `opencode`, `cline`, `kiro-cli`, and
`windsurf`. Use `universal` only when the host is unknown.

If `gh skill` is unavailable but Node.js is already installed, use:

```sh
npx skills add yhay81/sqrail --skill sqrail --agent AGENT --global --yes
```

Do not install Node.js solely for this fallback. The canonical, reviewable
Skill source is
<https://github.com/yhay81/sqrail/tree/main/skills/sqrail>.
Follow any host-specific post-install notice printed by the installer.

## 3. Confirm discovery

Confirm that the host lists a Skill named `sqrail`. Restart the agent if it
only discovers Skills at startup. Then use a fresh task to inspect a local CSV
or Parquet file; the agent should choose `sqrail`, read
`sqrail --help`, run the required query once, and stop after success.
