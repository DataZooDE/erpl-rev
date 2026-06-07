# Replication run statistics (dashboard-ready)

Every replication run — **full load** or **incremental (delta) cycle** — records one
durable row in the DuckDB control table **`_erpl_rev_run_stats`**, created at server
boot alongside `_erpl_rev_delta_state`. There is enough per-run dimension and measure
to build a replication dashboard straight from DuckDB; no external metrics system.

The stats live in the same DuckDB store as the data, written by the ABAP apply path
(`zcl_erpl_rev_util=>record_run` via the existing `Z_DUCKDB_QUERY`) — **no new SAP-side
table**. `run_id` and `ts` default in the server, so a run records itself with just the
summary it already holds.

## What is recorded

| column | meaning |
|--------|---------|
| `run_id` | sequence-assigned, monotonic |
| `ts` | when the run was recorded (server clock — same source as the delta state) |
| `target` / `source` | DuckDB target table / SAP source entity |
| `run_type` | `FULL` \| `DELTA` |
| `method` | `FULL` \| `WATERMARK` \| `SNAPSHOT` \| `CHANGEDOC` \| `INSERT_ONLY` |
| `status` | `SUCCESS` \| `ERROR` |
| `duration_ms` | wall-clock of the run |
| `rows_read` | rows pulled from SAP |
| `rows_ins` / `rows_upd` / `rows_del` | applied to the target (SNAPSHOT splits I/U/D; the watermark/change-doc methods report the merged total under `rows_ins`) |
| `wm_from` / `wm_to` | watermark / position before and after (delta) |
| `jobs` | parallel workers used (parallel full load / parallel snapshot) |
| `error_text` | on failure |

One row per run: a full load is recorded once by `replicate` / `replicate_parallel`
(the internal delta sub-step reloads and the parallel workers pass `iv_record=false`),
and a delta cycle is recorded once by `zcl_erpl_rev_delta=>run` — so counts never
double-count.

## The dashboard view

`erpl_rev_run_stats` (created at boot) adds the derived columns a dashboard wants:

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
