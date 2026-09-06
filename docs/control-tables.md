# Control tables — the operations API

Every piece of replication state erpl-rev owns lives in DuckDB, in tables prefixed
`_erpl_rev_`. Nothing lives in SAP. That is what makes uninstalling a matter of
deleting a package, and what lets any DuckDB client — the CLI, quack, a dashboard —
read the state of a replication estate without an RFC call.

These tables are a **stable, versioned interface**. They are created and evolved by
one ordered migration list (`src/control_schema.cpp`), applied at server boot.

## Versioning

`_erpl_rev_schema_version` is a history table, one row per applied migration:
`version`, `name`, `applied_ts`, `binary_version`. After an upgrade incident the
question actually asked is *which binary applied v7, and when* — so that is what it
records.

Rules the migration list keeps:

- **v1 is the schema as it stood before versioning existed**, byte for byte. Every
  statement is `CREATE … IF NOT EXISTS`, so a file created by an older binary counts
  as version 0, takes v1 as a no-op, and then receives v2 and later. That is what
  makes an upgrade a migration rather than an export/import. It must never be tidied.
- **Every migration is idempotent**, so re-running the list changes nothing.
- **A file newer than the binary is refused**, naming both versions. Opening it and
  failing later, somewhere unrelated, reads as data corruption.
- **Only the server migrates.** The CLI reaches DuckDB over quack while the server
  holds the file lock.

Two things are deliberately *not* migrations, because a migration cannot be corrected
on a file that already has it: the `erpl_rev_run_stats` view and the transformation
macros are `CREATE OR REPLACE`d on every open.

## The tables

| Table | One row per | Purpose |
|---|---|---|
| `_erpl_rev_schema_version` | migration | which binary applied what, and when |
| `_erpl_rev_delta_state` | target | method, source, keys, watermark, safety window, cadence, load type, backoff and parking, logging, transform, validation policy, lease and `active_run_id` |
| `_erpl_rev_run_stats` | run | status, counts, duration, watermarks, load type, validation status, lag |
| `_erpl_rev_cdc` | trigger target | dialect, mode, log/sequence names, position, status, shadow depth, tuning |
| `_erpl_rev_daemon` | server (one row) | instance, heartbeat, tick interval, worker budget, full-load share, stop flag, ticks |
| `_erpl_rev_cli_cmd` | queued command | the CLI's queue, drained by the ABAP driver |
| `_erpl_rev_log_<target>` | applied change | opt-in change log: `_seq`, `_op`, `_run_id`, `_commit_ts`, `_applied_at`, plus the target's own columns |

`_op` is `I`, `U` or `D` on both tiers. A `D` arrives from a trigger's own
delete event, or from a reload noticing that a key the target held is not in the
new image -- the second has no source timestamp, so its `_commit_ts` is NULL.

`_commit_ts` is when the **source** says the row changed; `_applied_at` is when
erpl-rev wrote it. Two columns, not one: the difference between them is the
replication latency, and a single column filled from whichever clock was nearest
measures nothing. `_commit_ts` is NULL where the method has no source clock — a
counter watermark, or a snapshot row — so latency for those targets is honestly
unmeasurable rather than reported as zero.

The log table exists as soon as a log-enabled target does. A target that has
never had a change has an **empty** log, not a missing one, so readers and the
retention pass never have to special-case it.

Two views are the reading surface: `erpl_rev_run_stats` (derived counts, rates and
durations) and the per-target change logs.

## Naming

A change-log table is named from the target through a collision-safe token, because
the input is a customer-chosen name: `MY-TAB` and `MY_TAB` must not land on one
table. Staging tables are named `<target>__stg_<run_id>`, so an orphan left by a
crashed cycle identifies itself and cleanup is a `DROP` over names that do not match
an in-flight run.

## Reading it

```sql
-- how far behind is each target?
SELECT target, status, wm_value, last_run_ts FROM _erpl_rev_delta_state ORDER BY 1;

-- what did the last runs do?
SELECT target, run_type, status, rows_read, rows_applied, rows_per_sec
FROM erpl_rev_run_stats ORDER BY finished_at DESC LIMIT 20;

-- is the daemon alive?
SELECT instance_id, status, ticks, epoch(now()) - epoch(heartbeat_ts) AS age_s
FROM _erpl_rev_daemon;
```
