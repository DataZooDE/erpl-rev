# Delta (incremental) extraction

erpl-rev can keep a DuckDB target in sync with a SAP source **incrementally** —
loading only what changed since the last cycle — on top of the existing full-load
path. All merge logic and all delta state live in the C++/DuckDB server; the ABAP
side stays a thin reader that selects changed rows with plain Open SQL. **No SAP
interface restricted by the April-2026 API policy is used** (no ODP-RFC, no SAPI/BW
Service API, no `RFC_READ_TABLE`) — every source read is Open SQL / CDS / native-SQL
in a customer `Z` function module.

## Architecture

One ABAP reader, one server merge engine, one state table.

```
Z_ERPL_REV_DELTA (job loop) ─► zcl_erpl_rev_delta (lean reader)
   WATERMARK   : WHERE chg_col > wm                ─┐
   INSERT_ONLY : CDHDR → CHANGENR list → re-read    │ stream binary sXML
   CHANGEDOC   : CDHDR(objectclas) → keys → re-read │ → Z_DUCKDB_INGEST (MODE=MERGE)
   SNAPSHOT    : full reload → <target>__snap      ─┘ → Z_DUCKDB_SNAPSHOT_MERGE
        data + _erpl_rev_delta_state live in the same DuckDB store
```

Config **and** runtime state live in one DuckDB table, `_erpl_rev_delta_state`
(created at server boot), read/written through the existing `Z_DUCKDB_QUERY` — there
is **no new `Z` table in SAP**.

## The four methods

| Method | Use when | How it reads | Apply |
|--------|----------|--------------|-------|
| **WATERMARK** | the source has a monotonic change column (UTC timestamp / sequence) | `WHERE chg_col > wm` | keyed upsert (`MODE=MERGE`) |
| **INSERT_ONLY** | append-only, driven by change documents (e.g. `CDPOS`) | CDHDR feed → `CHANGENR` list → `CDPOS WHERE CHANGENR IN (…)` (2-step, portable across ECC cluster / S4 transparent) | keyed upsert (DDIC key dedups re-delivered rows) |
| **CHANGEDOC** | weak/absent change column (e.g. `MARA`, `MAKT`) | CDHDR `WHERE objectclas=… AND (udate>… OR (udate=… AND utime>…))` → business keys → **re-read** current rows from the source by key | keyed upsert |
| **SNAPSHOT** | physical deletes, or bounded column-less tables | full reload into `<target>__snap` (the normal full-load path) | server anti-join: upsert all of staging **and DELETE target keys absent from it** |

Rule of thumb: **timestamp present → WATERMARK; append-only huge → INSERT_ONLY;
no/weak change column → CHANGEDOC for I/U + nightly SNAPSHOT for deletes; bounded &
column-less → just SNAPSHOT.** Physical deletes are reflected **only** by SNAPSHOT
(no change column can report a row that no longer exists) — or, for a table too large
to snapshot, by the opt-in **trigger-CDC** tier (see [`cdc.md`](cdc.md)).

## Registering a target

A target is one row in `_erpl_rev_delta_state`. Use `zcl_erpl_rev_delta=>register( )`
(or one INSERT via `Z_DUCKDB_QUERY`):

```abap
zcl_erpl_rev_delta=>register( VALUE #(
  target      = 'mara'                         " DuckDB target table
  method      = 'CHANGEDOC'                     " WATERMARK | INSERT_ONLY | CHANGEDOC | SNAPSHOT
  source_from = 'MARA'                          " SAP entity to read / re-read
  keys        = 'MANDT,MATNR'                   " merge / anti-join key (DuckDB column names)
  chg_col     = 'CHANGED_AT'                    " watermark column (WATERMARK/INSERT_ONLY)
  wm_kind     = 'NUMTS'                          " NUMTS | TIMESTAMPL | DATETIME | DATE | INT
  time_col    = ''                              " DATETIME only: the TIMS half of the pair
  wm_value    = '20260101000000'                " last high-water (text); blank = first cycle reads all
  safety_secs = 120                             " seconds of overlap, clock-based kinds
  safety_units = 0                              " values of overlap, counter kinds (INT)
  cadence     = 'micro:120'                      " micro:<sec> | hourly | nightly | manual
  extra       = '{"objectclas":"MATERIAL"}' ) ).  " CHANGEDOC/INSERT_ONLY driver class
```

> **Granularity gate:** registering `cadence='micro:*'` with `wm_kind='DATE'`
> (a date-only column can't be sub-hourly) is rejected.

> **`CHANGENR` is not a watermark kind.** The change number comes from a buffered
> number range and is not monotonic in commit order, so an overlap counted in
> change numbers bounds nothing. Registering it is refused, naming `CHANGEDOC` --
> which positions on `UDATE`+`UTIME` -- as the alternative.

### What each kind means

| `wm_kind` | Column | Ceiling | Overlap |
|---|---|---|---|
| `NUMTS` | `YYYYMMDDHHMMSS` | read start − `safety_secs` | `safety_secs` seconds |
| `TIMESTAMPL` | `…HHMMSS.fffffff` | as above, fraction preserved | `safety_secs` seconds |
| `DATETIME` | a `DATS` + a `TIMS` column | as above, compared as one 14-char value | `safety_secs` seconds |
| `DATE` | `DATS` | **yesterday** — today is never read | whole days, ≥ 1 when `safety_secs` > 0 |
| `INT` | a monotonic counter | max of the staged rows − `safety_units` | `safety_units` values |

Seed the target with an initial full load first (`zcl_erpl_rev_util=>replicate`),
then register; a WATERMARK/CHANGEDOC/INSERT_ONLY target is self-creating with its PK
on the first cycle, and SNAPSHOT self-seeds the target from its staging structure.

### Parallel SNAPSHOT reload

A SNAPSHOT cycle re-reads the whole source, so for large tables it can fan the read
out across several background jobs — the same coordinator/worker engine the full load
uses (`replicate_parallel`: split a numeric key into *N* ranges, one worker each).
Register with `extra='{"jobs":4}'` (and optionally `"part_col":"BELNR"` to pin the
partition column; otherwise the widest numeric key is auto-picked). On
`Z_ERPL_REV_REPLICATE`'s Delta tab the **Parallel jobs** field (shown only for
SNAPSHOT) does the same.

It is a pure throughput optimisation: the merge/anti-join still runs once in the server
after staging is loaded, so results are identical to a serial reload. If no suitable
numeric partition column is available (or no free batch work processes), the cycle
**falls back to a serial reload** — never an error. WATERMARK/CHANGEDOC/INSERT_ONLY
read only the changed slice and don't use this.

## Running cycles

- `zcl_erpl_rev_delta=>run( iv_target )` runs one cycle for one target
  (lease → dispatch by method → commit watermark → release).
- `zcl_erpl_rev_delta=>run_due( )` runs every **due** target (cadence elapsed since
  `last_run_ts`, lease free).
- **`Z_ERPL_REV_DELTA`** is the orchestration report: one tick (`p_once`, the default —
  the job step) or a `p_loop` watch loop (`p_secs` interval, `p_dur` duration) for
  sub-minute micro-batch during a demo.

## Running it periodically (the cron)

The supported way to run delta on a schedule is **one periodic SAP background job**
running `Z_ERPL_REV_DELTA` (one tick) at the *finest* period you need. Each tick calls
`run_due()`, which runs only the targets whose per-target `cadence` has elapsed — so a
single 1-minute job drives mixed cadences (a `micro:120` target every ~2 min, a
`nightly` one once a day). It's all SM37-monitorable; no third-party scheduler.

Install/remove the job from the report (or `Z_ERPL_REV_REPLICATE`'s Delta tab):

- `Z_ERPL_REV_DELTA` with **`p_sched`** + **`p_min`** → installs a periodic job
  `ERPL_REV_DELTA` that starts now and repeats every `p_min` minutes (`1` = every
  minute, `30` = every 30 min). Re-running it just re-times the job.
- **`p_unsch`** → removes the job.
- Programmatically: `zcl_erpl_rev_delta=>schedule( iv_minutes = 1 )` /
  `schedule( iv_remove = abap_true )` (uses `JOB_OPEN`/`JOB_SUBMIT`/`JOB_CLOSE`).

A background-job period is **≥ 1 minute**. For genuine **sub-minute** replication run
**`Z_ERPL_REV_DAEMON`**: one background job that ticks every `p_secs`, asks the server
what is due and runs it. It is a singleton (a second start reports the running instance
and exits), and the periodic `Z_ERPL_REV_DELTA` job re-submits it if its heartbeat goes
stale, so it survives a system restart. For most cases a 1-minute job is plenty.

### One screen: load + register + schedule

`Z_ERPL_REV_REPLICATE` has a **Delta & schedule** tab. Tick *Register as delta target*,
pick a **Method** from the dropdown (only the fields that method needs are shown), pick
a **Refresh interval**, and tick *Run it automatically* — that's a full incremental,
scheduled load in one screen. The full load is the seed; WATERMARK/INSERT_ONLY auto-seed
the high-water from the current source max. Press **F1 on any field** for a plain-language
explanation.

The **Refresh interval** is one setting that means two things: how fresh this target is
kept (its `cadence`), and — when *Run it automatically* is ticked — the period of the
background job that drives it. So "every 30 minutes" sets both; no separate numbers.

## Correctness contract

Every cycle reads a half-open window `(floor, ceiling]` of the change column. Both
ends carry weight:

- The **floor** is the stored watermark pulled *back* by the safety window, so rows
  that committed late are re-read. The merge is keyed, so re-delivery is free — it
  shows up as `rows_read` > `rows_applied` in the run statistics and nothing else.
- The **ceiling** is a value the cycle is confident everything below has committed
  by: the cycle's read start, minus the safety window. **The watermark advances to
  the ceiling, never to the maximum of the rows that happened to be delivered.**

That second point is the whole guarantee. Subtracting the safety window from the
floor alone is *not* sufficient: if a cycle reads for longer than the window, a row
committing during the read below the delivered maximum is skipped, and once the
watermark reaches that maximum it is below the next floor forever. Advancing to the
read-start ceiling instead is what makes the overlap actually bound the loss.

The contract this buys is **at-least-once with a bounded lag**: no row is lost
provided its commit is visible to a read starting more than `safety_secs` after the
value it carries. That is a real assumption, and `safety_secs` is the dial for it —
raise it on a system where transactions stay open a long time.

The apply is atomic: merge, change-log append and watermark advance happen in **one
transaction**, and `wm_value` moves only inside it, after the merge. A cycle that
dies at any earlier point therefore leaves the watermark where it was, so the read
is simply replayed and the orphaned staging table is free to discard.

A cycle is fenced by `active_run_id`, not by the lease. A healthy cycle can block
for longer than any lease TTL (the ingest pipe waits up to an hour), so the lease is
advisory; the commit compare-and-swaps on the run id and refuses if the target was
reclaimed in the meantime. There is no cross-system 2-phase commit.

`CHANGENR` is buffered and **not strictly monotonic** in commit order, so CDHDR-driven
methods watermark on `UDATE`+`UTIME` with the same safety offset applied — never a bare
`CHANGENR > wm`.

> **A DATS+TIMS pair and daylight saving.** `DATETIME` compares a wall-clock
> value in SAP's own timezone. On the autumn transition that clock repeats an
> hour, and the two passes through it are genuinely indistinguishable — no
> watermark can order them. Rows committed in the *second* pass carry values the
> cycle has already gone past, so they are read only if the safety window
> reaches back that far. The watermark is clamped forward-only, so the target
> cannot rewind and no later data is at risk; but if you need sub-daily delta
> across a DST boundary, use `NUMTS`/`TIMESTAMPL`, which are UTC and have no
> repeated hour. This is a property of wall-clock columns, not of erpl-rev.

### Load types

Every run is one of four, selectable with `sync run --load-type`:

| Code | Meaning | Watermark |
|---|---|---|
| `D` | delta (default) | advances to the ceiling |
| `F` | full reload — a data **repair** | **untouched**: a repair fixes data, it does not re-seed the delta |
| `I` | init without data: adopt a position, transfer nothing | seeded from the source |
| `L` | init + full load | advances to the ceiling |

`I` is for a target already populated from somewhere else — a restore, a migration,
a parquet drop.

> **Upgrade note.** Before this, `safety_secs` was stored, exposed on the CLI and on
> the Delta tab, and read by nothing: the read was `chg_col > wm` with no overlap at
> all. Existing targets will now re-deliver more rows on their first cycles. That is
> harmless — the merge is idempotent — and visible as `rows_read` > `rows_applied`.

## Demo & inspection (SAP GUI)

**`Z_ERPL_REV_DELTA_SFLIGHT`** — the recommended hands-on demo, on the familiar
flight-booking model. Run it in SAP GUI (SA38 → F8) and use the buttons:

- **Setup** — full-load `SFLIGHT` into the DuckDB table `sflight` and register it as
  a **SNAPSHOT** delta target (SFLIGHT has no change column; the snapshot anti-join
  reflects inserts, updates **and physical deletes**).
- **Update / Insert / Delete flight** — make a real, committed change to `SFLIGHT`
  (the key is the screen's carrid/connid/fldate).
- **Run delta cycle** — one cycle; the change is merged into `sflight`.
- **Refresh** — re-read the target.

For larger change sets it also has a **Mass insert / update / delete** trio (batch
size `p_mass`, default 1000) operating on far-future "demo flights" so they never
collide with real data and are trivially cleaned up.

The **log pane** (top) records every action plus each cycle's `ins/upd/del` counts
and a SAP-source-vs-DuckDB row-count check; the **ALV pane** (bottom) shows the live
DuckDB `sflight` contents — so you can watch a real SAP change flow into DuckDB and
debug exactly what was loaded. (This is the scenario the M5 E2E section verifies.)

## Testing

- **Server merge engine** — Catch2 (`test/test_duckdb_bridge.cpp`, run by `make test`):
  the `MODE=MERGE` I/U/D apply, the snapshot diff, the state table at boot, atomic
  rollback, and the composite-key + cast-column upsert.
- **E2E on A4H** — `ZCL_ERPL_REV_DELTATEST` (run by `make e2e`) proves, against real
  SAP transactions: WATERMARK merge + idempotent re-run, SNAPSHOT physical-delete
  reconciliation, the orchestration lease / granularity-gate / catch-up, and the
  **SFLIGHT** insert/update/delete demo scenario end-to-end. It prints
  `DELTA RESULT pass=N fail=0`. (CHANGEDOC/INSERT_ONLY are exercised against a real
  `BAPI_MATERIAL_SAVEDATA` change document on an MM-equipped system; on a bare ABAP
  Platform trial without Materials Management that section skips.)

Every cycle (and every full load) is recorded in `_erpl_rev_run_stats` for a
replication dashboard — see [`stats.md`](stats.md).

See the design study (HLD + ADRs) for the rationale behind each decision.
