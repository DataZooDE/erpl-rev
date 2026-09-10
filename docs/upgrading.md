# Upgrading

What changes for a system that is **already replicating**, in the order it will hit
you. If you are installing for the first time, read [`INSTALL.md`](INSTALL.md)
instead — none of this applies to an empty system.

## Do this first: upgrade the ABAP side before the binary

The server and the ABAP objects are one interface split across two release
vehicles. This release **adds a function module and a report**:

| Object | What it is |
|---|---|
| `Z_DUCKDB_PLAN` | one generic FM the server calls for every new action (`BEGIN_CYCLE`, `CYCLE_COMMIT`, `TICK`, `VALIDATE`, `SPLIT`, `CDC_STATUS`, …) |
| `Z_ERPL_REV_DAEMON` | the polling report that runs replication continuously in one background work process |

and **adds optional parameters** to two existing interfaces (`IV_IMAGES` on
`Z_DUCKDB_CDC_APPLY`, `p_portid` on `Z_ERPL_REV_REPL_WORKER`). They are optional
precisely so that the order below is recoverable rather than fatal.

```bash
erpl-rev setup      # deploys the ABAP objects and regenerates the function modules
erpl-rev doctor     # verifies the objects the server expects are present
```

> **`doctor` checks that the function modules EXIST. It does not check their
> SIGNATURES.** A system carrying an older `Z_DUCKDB_PLAN` — the name present, the
> parameters not — passes `doctor` and then fails at run time with an RFC parameter
> error that names the parameter but not the cause. If you see one, re-run
> `erpl-rev setup` before investigating anything else. Automating this check is
> known work that is not in this release.

## The control database migrates itself, and only forwards

On first open the binary applies migrations **v2 through v8** to your existing
`.duckdb` file. It is in place and automatic; there is no export/import step.

- **A file is never downgraded.** An *older* binary opening a *newer* file refuses
  to open it and names both versions, rather than failing deep inside a query for a
  column it does not know about. So roll the binary forward on every machine that
  shares a control database, and **back up the file before the first upgraded
  start** if you want the option to roll back.
- `_erpl_rev_schema_version` is a history table, one row per migration, recording
  which binary applied what and when. After an upgrade incident that is the first
  thing to read. See [`control-tables.md`](control-tables.md).

## Your first cycle after upgrading will read more rows than usual

This is expected, it is not a fault, and it is the whole point of the release.

Previously the watermark advanced to a `max(chg_col)` taken *after* the read
finished. Any row committing during the read carried a value below that maximum and
was therefore below the next cycle's floor — **lost, permanently**. `safety_secs`
was a column, a parameter, a CLI flag and a screen field that nothing read.

Now every cycle reads a half-open window `(floor, ceiling]` where the ceiling is
`read_start − safety_secs`, and **the watermark advances to that ceiling, never to
the maximum of the rows that happened to be delivered**. Two consequences:

- The first cycle re-reads roughly `safety_secs` worth of already-delivered rows,
  and every cycle after it keeps re-reading that overlap. Re-delivery is harmless —
  the merge is a keyed upsert, so a row that arrives twice lands once. It shows up in
  `_erpl_rev_run_stats` as a `rows_read` that covers more than the interval's real
  changes, with `wm_from`/`wm_to` recording the window each cycle actually read. It
  is not a bug report.
- If you had a target running with a large `safety_secs`, its first upgraded cycle
  is correspondingly large. Nothing breaks; it may just take a while.

**Rows lost before the upgrade are not recovered by it.** The fix stops the bleeding;
it cannot know what already went missing. If a target's completeness matters, run
`sync validate` against it, or re-seed it with load type `L`. See
[`delta.md`](delta.md) for both.

## Throughput has moved, deliberately

Every cycle now **stages unconditionally**, so the merge, the change-log append and
the watermark advance are one transaction for every target rather than only some.
That costs a full write-and-read of the delta per cycle, and the previously published
per-cycle throughput number no longer describes this build.

The guarantee bought with it is the one worth having: a cycle that dies leaves the
watermark unmoved and an orphaned staging table that is free to discard, so it is
simply replayed. There is no half-applied state to reason about.

Re-measure on your own hardware before quoting a number.

## Trigger CDC: a rename, and a new mode you have to ask for

- **`FULL_IUD` is now `IMAGE_IUD`.** Migration v3 rewrites stored values, and the old
  spelling is accepted on read permanently. Nothing to do.
- **`KEYS_IUD` is new** — `AFTER INSERT/UPDATE/DELETE` logging keys only, with the
  cycle re-reading the source for the values.
- **Existing registrations keep the mode they have, and are never silently
  re-provisioned.** Changing a target's mode re-creates its triggers, so it is
  something you ask for, not something an upgrade does to you.
- The default for a *new* provision is still `DELETE_ONLY`.

See [`cdc.md`](cdc.md).

## Things that are new but optional

None of these run unless you start them.

| | |
|---|---|
| **The daemon** | `erpl-rev daemon start` — continuous replication at a per-target cadence, in one background work process. Until you start it, the existing periodic report drives replication exactly as before. [`daemon.md`](daemon.md) |
| **Monitoring** | `erpl-rev top` (a terminal monitor), a Prometheus text endpoint (`erpl-rev serve --metrics-port`), and an ALV report — all three read the same two views, so they cannot disagree. [`operations.md`](operations.md) |
| **Operator verbs** | `sync set-wm`, `sync preview`, `sync validate`, `sync unpark`, `cdc status`, `cdc repair`, `mass run`, `sub` — all queued through the command table, so none of them need `S_DEVELOP`. |

## Known limits, stated plainly

- **Customer-written conversion exits are not supported by the SQL path.** The
  built-in transformations (`erpl_rev_alpha_in/out`, `erpl_rev_curr_amount`,
  `erpl_rev_xfeld`) are DuckDB macros; an exit written in ABAP has no equivalent and
  is not called. If a field depends on one, transform it downstream.
- **`doctor` does not verify function-module signatures** (above).
- **`KEYS_IUD` was unusable before this release** — five defects, three of them
  silent data loss, in a mode no test had ever exercised. It is now covered by a
  live end-to-end arm and measured; see [`perf-results.md`](perf-results.md). If you
  provisioned a `KEYS_IUD` target on an earlier build, re-seed it: its cycles
  deleted rows they should have updated.
- **Trigger CDC is HANA only.** The dialect seam exists; Oracle/Db2/MSSQL/ASE refuse.
