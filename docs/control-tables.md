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

Some things are deliberately *not* migrations, because a migration cannot be corrected
on a file that already has it: the **three views** and the transformation and currency
macros are `CREATE OR REPLACE`d on every open, so a fix to any of them reaches an
existing database with no version bump.

## The tables

| Table | One row per | Purpose |
|---|---|---|
| `_erpl_rev_schema_version` | migration | which binary applied what, and when |
| `_erpl_rev_delta_state` | target | method, source, keys, watermark, safety window, cadence, load type, backoff and parking, logging, transform, validation policy, lease and `active_run_id` |
| `_erpl_rev_run_stats` | run | status, counts, duration, watermarks, load type, validation status, lag |
| `_erpl_rev_cdc` | trigger target | platform, mode, log and trigger table names, position, status, shadow depth, tuning |
| `_erpl_rev_daemon` | server (one row) | instance, heartbeat, tick interval, worker budget, full-load share, stop flag, ticks |
| `_erpl_rev_cli_cmd` | queued command | the CLI's queue, drained by the ABAP driver |
| `_erpl_rev_log_<target>` | applied change | opt-in change log: `_seq`, `_op`, `_run_id`, `_commit_ts`, `_applied_at`, plus the target's own columns |

`_op` is `I`, `U` or `D`, and it is the **engine's** verdict on both tiers: `U`
when the key was already in the target, `I` when it was not — never the source's
own claim about what it did. That matters to a subscriber, which would otherwise
conflict on an insert for a key it already holds when a seeded target replays an
old trigger row.

A `D` arrives from a trigger's delete event, from the mirror race (a changed key
the re-read could not find, so it is removed), or from a reload noticing that a
key the target held is not in the new image. Only the first has a source
timestamp; the others carry `_commit_ts` NULL, because a deletion inferred by
comparing two images has no moment at the source.

A **reload** emits `I` and `D` only. It replaces the target, so every surviving
row is an insert and the keys that did not come back are deletes; `rows_upd` on
such a run is 0 for the same reason.

`_run_id` joins to `_erpl_rev_run_stats` on both tiers.

`_commit_ts` is when the **source** says the row changed; `_applied_at` is when
erpl-rev wrote it. Two columns, not one: the difference between them is the
replication latency, and a single column filled from whichever clock was nearest
measures nothing. `_commit_ts` is NULL where the method has no source clock — a
counter watermark, or a snapshot row — so latency for those targets is honestly
unmeasurable rather than reported as zero.

The log table appears on a target's **first successful cycle**, not at
registration — both replication tiers provision it inside the transaction that
first writes to it, so a target whose first cycle fails has none. Readers treat
absence as "nothing has been logged yet": `sub advance` publishes nothing, the
retention pass prunes nothing, and the latency view reports no samples. From
that first cycle on it exists even when a cycle changes nothing, so an idle
target has an empty log rather than a missing one.

### Who writes what

`_erpl_rev_delta_state` holds two kinds of column, and they have different
owners. Mixing them is where several defects came from.

**Intent** — what the operator asked for: `method`, `source_from`, `keys`,
`chg_col`, `time_col`, `wm_kind`, `safety_secs`, `safety_units`, `cadence`,
`extra`, `log_enabled`, `load_type_default`, `allow_empty_reload`. Written only
by registration. `sync create` is create-or-update, so re-running it on an
existing target updates it — and a field the command line does not mention is
left as it was, rather than reset to a default.

**Engine state** — what replication has since done: `wm_value`, `status`,
`last_run_ts`, `fail_count`, `parked_until`, `active_run_id`, `rows_applied`,
`one_shot_spent`, `last_error`. Each has exactly one writer, and registration is
not it.

`load_type_default` and `one_shot_spent` are the clearest case: the first is the
operator saying "seed this target" (`L`) or "repair it once" (`F`); the second is
the engine recording that it has done so. They were one column, and the engine
crossed out the operator's value — so a re-registration for an unrelated reason
cancelled a pending seed, and a manual `sync run --load-type F` consumed one.
The planner combines them: a one-shot type that has been spent plans as `D`.

**Three views are the reading surface**, recreated at every open so a fix reaches an
existing database without a migration:

| view | answers |
|---|---|
| `erpl_rev_run_stats` | what each run did — derived counts, rates and durations |
| `erpl_rev_targets` | per target: method, cadence, status, lag, last cycle's inserts/updates/deletes, health, and the trigger registry's own state |
| `erpl_rev_health` | one row: how many targets, how many healthy, worst lag, daemon heartbeat |

`erpl_rev_targets` and `erpl_rev_health` are what `erpl-rev top`, `sync ls`, the
Prometheus endpoint and the ABAP ALV report all read — so four surfaces cannot
disagree about whether a target is healthy. The per-target change logs
(`_erpl_rev_log_<target>`) are the fourth reading surface, for subscribers.

## Naming

A change-log table is named from the target through a collision-safe token, because
the input is a customer-chosen name: `MY-TAB` and `MY_TAB` must not land on one
table. Staging tables are named `<target>__stg_<run_id>`, so an orphan left by a
crashed cycle identifies itself and cleanup is a `DROP` over names that do not match
an in-flight run.

## What each run records

`_erpl_rev_run_stats` is one row per run, and `erpl_rev_run_stats` is the view a
dashboard reads.

| column | meaning |
|--------|---------|
| `run_id` | sequence-assigned, monotonic |
| `ts` | when the run was recorded (server clock — same source as the delta state) |
| `target` / `source` | DuckDB target table / SAP source entity |
| `run_type` | `FULL` \| `DELTA` |
| `method` | `FULL` \| `WATERMARK` \| `SNAPSHOT` \| `CHANGEDOC` \| `INSERT_ONLY` \| `CDC` |
| `status` | `SUCCESS` \| `ERROR` |
| `duration_ms` | wall-clock of the run |
| `rows_read` | rows pulled from SAP |
| `rows_ins` / `rows_upd` / `rows_del` | applied to the target (SNAPSHOT splits I/U/D; the watermark/change-doc methods report the merged total under `rows_ins`) |
| `wm_from` / `wm_to` | watermark / position before and after (delta) |
| `jobs` | parallel workers used (parallel full load / parallel snapshot) |
| `error_text` | on failure |
| `load_type` | the load type this run used — see [`delta.md`](delta.md) |
| `portion_count` | portions a mass/split load was cut into |
| `validation_status` | result of a post-run `sync validate`, when one ran |
| `lag_ms` | source change time to apply time, where the method can know it |
| `clock_skew_secs` | SAP's clock minus the server's, recorded per run, because a wall-clock change value cannot be read without it |

One row per run: a full load is recorded once by `replicate` / `replicate_parallel`
(the internal delta sub-step reloads and the parallel workers pass `iv_record=false`),
and a delta cycle is recorded once by `zcl_erpl_rev_delta=>run` — so counts never
double-count.

The view adds the derived columns a dashboard wants:

- `finished_at` — the raw table calls this `ts`; the view renames it, and the
  example queries below use the new name
- `started_at` = `finished_at − duration_ms`
- `rows_applied` = `rows_ins + rows_upd + rows_del`
- `rows_per_sec`
- `is_success` (boolean)

## Example dashboard queries

```sql
-- Throughput per target over the last day
SELECT target, run_type,
       count(*)               AS runs,
       sum(rows_applied)      AS rows_applied,
       round(avg(rows_per_sec)) AS avg_rows_per_sec
FROM erpl_rev_run_stats
WHERE finished_at > now() - INTERVAL 1 DAY
GROUP BY target, run_type
ORDER BY rows_applied DESC;

-- Last run per target (freshness) + outcome
SELECT target, max(finished_at) AS last_run,
       arg_max(status, finished_at) AS last_status,
       arg_max(duration_ms, finished_at) AS last_ms
FROM erpl_rev_run_stats
GROUP BY target;

-- Success rate per method
SELECT method,
       count(*) AS runs,
       round(100.0 * count(*) FILTER (WHERE is_success) / count(*), 1) AS pct_ok
FROM erpl_rev_run_stats
GROUP BY method
ORDER BY runs DESC;

-- Delete volume captured by snapshot reconciliation
SELECT target, sum(rows_del) AS deletes_reconciled
FROM erpl_rev_run_stats
WHERE method = 'SNAPSHOT'
GROUP BY target
HAVING sum(rows_del) > 0;

-- Runs that failed, newest first
SELECT finished_at, target, method, error_text
FROM erpl_rev_run_stats
WHERE NOT is_success
ORDER BY finished_at DESC;
```

## Retention

The table grows by one row per run. Prune it on whatever horizon you keep dashboard
history for, e.g. a periodic `DELETE FROM _erpl_rev_run_stats WHERE ts < now() - INTERVAL 90 DAY`.

See [`delta.md`](delta.md) for the delta methods that produce the `DELTA` rows.

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
