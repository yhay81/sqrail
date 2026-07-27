# Security policy

## Supported versions

The latest published sqrail release receives security fixes.

## Reporting

Do not open a public issue for a suspected vulnerability. Use
[GitHub private vulnerability reporting](https://github.com/yhay81/sqrail/security/advisories/new)
with:

- affected sqrail version and platform
- a minimal reproduction
- expected and observed impact
- whether untrusted SQL, filenames, or data are required

The project will acknowledge a complete report, investigate it, and coordinate
disclosure through a GitHub Security Advisory.

## Security boundary

sqrail rejects write statements, protects named output files from replacement,
disables extension installation/loading and external access, allowlists exact
bound inputs and output temporaries, creates POSIX outputs without group or
other permissions, protects Windows outputs with a non-inheriting current-user
DACL, and locks DuckDB configuration. When
`--spill` is used, sqrail creates and allowlists only a unique, owner-only
workspace beneath the requested root. Existing siblings under that root remain
inaccessible and the workspace is removed after DuckDB shuts down. Handled
interrupts and closed stdout pipes pass through the same private-artifact
cleanup path.

The SQL-size, input-count, result-row, output-byte, memory, spill, thread, and
end-to-end deadline controls bound distinct resource dimensions. `--memory`
remains a DuckDB buffer-manager limit rather than a process RSS or address-space
limit.

These controls reduce accidental and agent-generated file access. They do not
replace an operating-system sandbox, resource cgroup, or trust review for
hostile SQL, native dependencies, malformed data, or a filesystem whose remote
server does not preserve local atomicity and durability semantics. The
agent-evaluation runner also filters inherited environment secrets and rejects
absolute paths, parent traversal, and external URIs, but it is evidence
collection rather than a general-purpose hostile-code sandbox.
