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
(no change column can report a row that no longer exists).

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
  wm_kind     = 'NUMTS'                          " NUMTS | DATETIME | CHANGENR | INT | DATE
  wm_value    = '20260101000000'                " last high-water (text); blank = first cycle reads all
  safety_secs = 120                             " safety-interval overlap (absorbed by the idempotent merge)
  cadence     = 'micro:120'                      " micro:<sec> | hourly | nightly | manual
  extra       = '{"objectclas":"MATERIAL"}' ) ).  " CHANGEDOC/INSERT_ONLY driver class
```

> **Granularity gate:** registering `cadence='micro:*'` with `wm_kind='DATE'`
> (a date-only column can't be sub-hourly) is rejected.

Seed the target with an initial full load first (`zcl_erpl_rev_util=>replicate`),
then register; a WATERMARK/CHANGEDOC/INSERT_ONLY target is self-creating with its PK
on the first cycle, and SNAPSHOT self-seeds the target from its staging structure.

## Running cycles

- `zcl_erpl_rev_delta=>run( iv_target )` runs one cycle for one target
  (lease → dispatch by method → commit watermark → release).
- `zcl_erpl_rev_delta=>run_due( )` runs every **due** target (cadence elapsed since
  `last_run_ts`, lease free).
- **`Z_ERPL_REV_DELTA`** is the orchestration report: one tick (`p_once`, the default,
  for an external scheduler / self-rescheduling job) or a `p_loop` watch loop
  (`p_secs` interval, `p_dur` duration) for sub-minute micro-batch during a demo.

## Correctness contract

Every cycle is **idempotent** (key-based merge / set-based snapshot diff) and
re-runnable. A `safety_secs` overlap on the read guarantees no lost rows
(at-least-once); the idempotent merge absorbs the re-delivered overlap. State is
advanced in the server (one connection-local transaction per apply), so a failed
cast or any error rolls the whole package back. A per-target **lease** prevents two
cycles from overlapping (`status='RUNNING'` + a fresh `lease_ts`; a stale lease is
reclaimed). There is no cross-system 2-phase commit.

`CHANGENR` is buffered and **not strictly monotonic** in commit order, so CDHDR-driven
methods watermark on `UDATE`+`UTIME` with a safety lag — never a bare `CHANGENR > wm`.

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

The **log pane** (top) records every action plus each cycle's `ins/upd/del` counts
and a SAP-source-vs-DuckDB row-count check; the **ALV pane** (bottom) shows the live
DuckDB `sflight` contents — so you can watch a real SAP change flow into DuckDB and
debug exactly what was loaded. (This is the scenario the M5 E2E section verifies.)

`Z_ERPL_REV_DELTA_SIM` is the lower-level simulator: pick a scenario (seed / update /
insert / delete a `ZDELTA_WM` row, or change a material), inject a committed change,
run one cycle, and see the applied counts plus the target row count before/after.

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

See the design study (HLD + ADRs) for the rationale behind each decision.
