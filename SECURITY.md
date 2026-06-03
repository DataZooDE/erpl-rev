# Security Policy

## Supported versions

erpl-rev is a research prototype. Only the **latest release** receives security
fixes.

| Version | Supported |
|---------|-----------|
| latest release | ✅ |
| older releases | ❌ |

## Reporting a vulnerability

**Please do not open a public issue for security reports.**

Report vulnerabilities privately via GitHub's **private vulnerability reporting**:
on this repository, go to the **Security** tab → **Report a vulnerability**
(<https://github.com/DataZooDE/erpl-rev/security/advisories/new>).

Please include enough detail to reproduce — affected version/commit, platform,
and a minimal proof of concept. We aim to acknowledge a report within a few
business days and will coordinate a fix and disclosure with you.

## Scope

This policy covers erpl-rev's own code (the C++ RFC server, the launcher, and the
ABAP objects in this repository). Two bundled dependencies are out of scope here:

- the **SAP NW RFC SDK** — proprietary to SAP; report SDK issues to SAP.
- **DuckDB** — report upstream at <https://github.com/duckdb/duckdb>.

Note that the released single-file binaries are **self-extracting**: they unpack
the bundled libraries to a temporary directory on first run. Operators deploying
the RFC server should also follow the hardening guidance in
[`docs/security.md`](docs/security.md) (gateway `reginfo` ACL, SNC, least-
privilege RFC user).
