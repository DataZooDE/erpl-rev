# Telemetry

`erpl_rev_server` sends **anonymous, privacy-conscious usage telemetry** so we
can see how many installations run the server, on which platforms, and on which
versions. It uses the shared [`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library — the same one [flapi](https://github.com/DataZooDE/flapi) uses — and
reports into the same PostHog project, so erpl-rev shows up as an `erpl-rev` row
next to `flapi` in the existing dashboards.

Telemetry is **on by default** and **trivial to turn off** (see below).

## What is sent

Exactly two lifecycle events:

| Event | When |
|-------|------|
| `application_start` | once, when the server starts listening |
| `application_stop`  | on graceful shutdown (Ctrl-C / SIGTERM / normal exit) |

Each event carries only these properties:

| Property | Example | Meaning |
|----------|---------|---------|
| `app_name` | `erpl-rev` | constant, identifies the product |
| `app_version` | `2026.06.13` | release version (git tag, without the leading `v`) |
| `platform` | `linux_amd64` | OS/arch, detected at compile time |
| `duckdb_version` | `1.5.3` | bundled DuckDB engine version |
| `distinct_id` | *(hash)* | anonymous, stable SHA-256 of the machine id |

**Nothing else.** Explicitly **never** sent: SAP system data, connection
parameters, query text (SQL), table or field names, row data, credentials,
hostnames, file paths, or any personally identifiable information.

## Where it goes

To PostHog in the EU region: `https://eu.posthog.com`. The ingestion key is a
public project key baked into the shared library.

## How to opt out

Any **one** of these disables all telemetry — no event is ever sent:

- `--no-telemetry` — CLI flag on `erpl_rev_server`
- `ERPL_REV_NO_TELEMETRY=1` — environment variable (`1` / `true` / `yes`)
- `DATAZOO_DISABLE_TELEMETRY=1` — shared DataZoo opt-out across all our tools

The `--smoke` self-check and `--help` exit before telemetry is even initialized,
so they never emit.

## SAP / air-gapped hosts

The POST runs on a background thread and the library swallows all errors. On a
host without outbound internet access (typical for SAP application servers) the
request simply fails silently — **zero functional impact** on the RFC server.
You may still set an opt-out env var to skip the attempt entirely.
