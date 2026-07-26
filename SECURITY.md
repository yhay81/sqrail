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

sqrail rejects write statements and protects named output files from accidental
replacement. It does not sandbox DuckDB expressions or file-reading functions.
Only run SQL from an agent or user that is trusted with the current process's
filesystem permissions.
