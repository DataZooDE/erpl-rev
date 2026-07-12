# erpl-rev Telemetry

erpl-rev collects **anonymous, aggregate usage telemetry** to help us understand
which bridge operations are used and where things break. It is privacy-
preserving by construction: only bounded enumerations and numbers ever leave the
machine — **never** SAP data, SQL, table/field names, targets, cursor handles,
connection strings, or error messages.

Telemetry is emitted against the shared DataZoo telemetry schema
(`telemetry_schema: 2`) via the [`posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library, into PostHog's **EU** ingestion endpoint (`eu.i.posthog.com`). erpl-rev
is a long-running RFC server, so events carry `install_kind = "server"` and a
single session id per server uptime.

## How to turn it off

Any **one** of the following disables all telemetry — a single guard
short-circuits every event, and nothing leaves the machine:

- **CLI flag:** `erpl_rev_server --no-telemetry`.
- **Environment:** `ERPL_REV_NO_TELEMETRY=1` (also `true`/`yes`), or
  `DATAZOO_DISABLE_TELEMETRY=1` (disables telemetry across all DataZoo tools).

For very high-throughput servers you can down-sample the per-call event
(lifecycle events are always sent in full):

```
ERPL_REV_TELEMETRY_SAMPLE_RATE=0.1   # emit 10% of rfc_call; events carry sample_rate
```

## What is collected

Every event carries a common envelope from the library: `product` (`erpl_rev`),
`product_version`, `product_edition` (`oss`/`enterprise`), `telemetry_schema`,
`duckdb_version`, `os`, `arch`, `platform`, `is_ci`, `is_container`, a per-uptime
`$session_id`, a pseudonymous per-machine `distinct_id` (salted SHA-256 of the OS
machine id — identifies a *machine/install*, not a person), and `$groups` once
associated. erpl-rev additionally stamps `install_kind = "server"` on every
event.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `server_started` | server boot | `quack_enabled` (bool), `reg_count` (number) |
| `rfc_call` | a bridge FM is invoked | `fm` ∈ `ping`\|`query`\|`ingest`\|`snapshot_merge`\|`cursor_open`\|`cursor_fetch`\|`cursor_close`\|`cdc_plan`\|`cdc_apply`; `status_class` ∈ `2xx`\|`5xx`; `duration_ms` (number) |
| `$exception` | a bridge FM fails | `error_class` ∈ `rfc_error`\|`sql_error`\|`ingest_error`\|`cdc_error`\|`cursor_error`; `feature` (the `fm`) |

Sampled `rfc_call` events additionally carry `sample_rate` (number) so aggregate
counts scale back up.

### Groups

- **`deployment`** — always associated at boot; key is the pseudonymous
  per-machine `distinct_id`.
- **`account`** — associated only when `ERPL_REV_LICENSE_ID` is set; key is
  `sha256(license_id)`. The raw license id is never sent.

## What is **never** collected

By design, the following never appear in any property — call sites only pass
enums/numbers, and the library additionally clamps every string property to 512
bytes as a backstop:

- SQL text (`IV_SQL`), or any RFC parameter values.
- Ingest/merge targets, staging names, key lists, or DDL.
- Table names, field names (`MANDT`, …), or row data.
- Cursor handles.
- Gateway host/service, DuckDB file path, or the quack token.
- Free-form error messages (only an enumerated `error_class`).

## Where this lives in the code

- Facade + gating: `src/erpl_rev_telemetry.{hpp,cpp}` (`erpl_rev::GlobalTelemetry()`).
- Boot / shutdown (server_started, deployment group, `flush()` on stop):
  `src/main.cpp`.
- `rfc_call` + errors: `src/rfc_handlers.cpp` (`RfcCallScope` around each bridge FM).
- Tests (incl. a no-leak assertion against the real transport):
  `test/test_telemetry.cpp`.
