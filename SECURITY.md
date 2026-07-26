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
bound inputs and output temporaries, and locks DuckDB configuration. When
`--spill` is used, sqrail creates and allowlists only a unique, owner-only
workspace beneath the requested root. Existing siblings under that root remain
inaccessible and the workspace is removed after DuckDB shuts down.

These controls reduce accidental and agent-generated file access. They do not
replace an operating-system sandbox, resource cgroup, or trust review for
hostile SQL, native dependencies, or malformed data.
