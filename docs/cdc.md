# Trigger-based CDC (opt-in physical-delete tier)

The watermark/snapshot delta methods ([`delta.md`](delta.md)) cover most needs, but a
**watermark is structurally blind to physical `DELETE`s** and a **`SNAPSHOT` re-reads
the whole table**. For a table too large to snapshot that still needs low-latency
delete capture, erpl-rev offers an **opt-in trigger-CDC tier** (design study ADR-0004):
database **triggers + a customer-owned log table** capture changes, and the server
applies them incrementally.

This is **not** a SAP-proprietary CDC interface. The triggers and log table are
**customer-owned, in-namespace (`ZCDC_*`)** objects created with the customer's own
DDL on the customer's own table via the ADBC native-SQL path the BW/native source
already uses — keeping the April-2026 compliance posture (no `RODPS_REPL_*`, no SAPI,
no `/1DH/*`, no `RFC_READ_TABLE`).

> **v1 platform: SAP HANA.** A4H runs on HANA, so the whole tier is E2E-proven on the
> test system. The SQL generator is a pluggable dialect; AnyDB (Oracle/DB2/MSSQL/ASE)
> is a roadmap stub that refuses for now.

## Architecture — all the logic is in the C++ server; ABAP is a dumb executor

```
                         ┌──────────────── C++ server (all the logic) ────────────────┐
provision  ── ABAP ◄───  │ CdcDialect → {sequence, log table, trigger(s), read, prune, │
           (ADBC exec)    │              teardown}   ·   _erpl_rev_cdc state machine    │
seed       ── existing replicate() seeds the DuckDB target ───────► state SEEDED, pos=0 │
run cycle  ── ABAP stages new log rows (replicate_native, seq > pos) ──►                │
           │   Z_DUCKDB_CDC_APPLY: coalesce to net op/key → MERGE I/U/D → advance pos   │
           │   → return prune bound ──► ABAP prunes the SAP log (seq ≤ confirmed)       │
teardown   ── ABAP runs the server teardown DDL ─────────────────► state DISABLED       │
                         └────────────────────────────────────────────────────────────┘
```

The server (`zcl_erpl_rev_cdc` ABAP side, `cdc_dialect` + `_erpl_rev_cdc` +
`CdcApply` C++ side) makes **every** decision: which DDL to emit, the state machine,
log coalescing, the merge apply, the position advance and the prune bound. ABAP only
(a) ADBC-executes the opaque DDL on the SAP DB, (b) runs the opaque incremental read
and streams the rows, and (c) runs the opaque prune.

## Two modes

| Mode | Triggers | Log payload | Use when |
|------|----------|-------------|----------|
| **DELETE_ONLY** (default) | `AFTER DELETE` only | keys | inserts/updates already come from the watermark tier — the trigger only closes the physical-delete gap (the Fivetran pattern; minimal write-path overhead) |
| **FULL_IUD** | `AFTER INSERT`/`UPDATE`/`DELETE` | full row image | the source has no usable change column at all — the log carries the row so the server upserts I/U and deletes D, entirely server-side |

## Using it

```abap
" 1. seed the DuckDB target (a normal full load)
zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'cdc_wm' ).

" 2. provision the triggers (creates ZCDC_* log/sequence/trigger on the SAP DB)
zcl_erpl_rev_cdc=>provision(
  iv_target = 'cdc_wm' iv_source = 'ZDELTA_WM' iv_keys = 'CLIENT,ID'
  iv_mode = 'DELETE_ONLY' ).        " or 'FULL_IUD'

" 3. each cycle: stage new log rows -> apply in the server -> prune the log
DATA(r) = zcl_erpl_rev_cdc=>run( 'cdc_wm' ).   " r-ins / r-upd / r-del / r-applied

" 4. when done: drop the triggers + log table + sequence
zcl_erpl_rev_cdc=>teardown( 'cdc_wm' ).
```

`run` is what a periodic job calls (like the watermark/snapshot tiers). Provisioning is
idempotent — it best-effort drops any leftover objects first, so it is safe to re-run.

## Correctness contract

Every cycle is **at-least-once and idempotent**: the server stages the new log rows
(`seq > position`), **coalesces** them to one net op per key (latest by sequence — an
insert+update+delete of the same key in one batch applies once), applies a key-based
DELETE/upsert in **one transaction**, advances the position to the max consumed
sequence and marks the run `ACTIVE` — all atomically. An error rolls everything back
and leaves the position untouched, so the next cycle re-reads the same rows. The SAP
log is pruned **only up to the server-confirmed position** (watermark-driven, never
destructive-on-read), so a crash between apply and prune just re-delivers a few rows
the idempotent merge absorbs. The state machine guards transitions
(`PROVISIONED → SEEDED → ACTIVE → DISABLED`) and the position is monotonic.

## Safety & limitations (the gate)

- **Transparent tables only.** Pool/cluster tables, views and activation-request
  (ADSO) objects are not trigger-trackable; `provision` refuses them with guidance to
  use the `SNAPSHOT` method instead.
- **In-namespace by construction.** The log table, sequence and triggers are all named
  `ZCDC_*` — provisioning can never touch a SAP-owned object.
- **DB triggers carry real cost** (write-path overhead, transport/Basis sign-off,
  DB-platform-specific DDL). This is exactly why the tier is **opt-in, per table**, not
  a default — the watermark + snapshot tiers remain the default.
- **Key types:** keys are logged as SAP-raw text and the server casts each to the
  target column's type when matching — `CHAR`/string keys directly, `NUMC` via numeric
  cast (`'0017'` → `17`), `DATS`/`TIMS` via `strptime` (`'20991231'` → a `DATE`). So
  composite keys like SFLIGHT's `MANDT,CARRID,CONNID,FLDATE` (a NUMC + a date) work
  unchanged — the flight-booking demo is wired to CDC and proven E2E.

## Testing

- **Server engine** — Catch2 (`test/test_cdc_dialect.cpp` + `[cdc]` cases in
  `test/test_duckdb_bridge.cpp`): the dialect golden strings (delete-only + full-IUD,
  namespace guard, AnyDB refusal), the `_erpl_rev_cdc` state machine (transitions,
  monotonic position, restart-safe), log coalescing, and the apply (delete reflected,
  full-IUD I/U/D, idempotent, rollback-on-error).
- **E2E on A4H (real HANA triggers)** — `ZCL_ERPL_REV_CDCTEST` (run by `make e2e`):
  provisions real HANA triggers on `ZDELTA_WM` (delete-only + full-IUD) **and on
  SFLIGHT** (the flight-booking demo — composite DATE+NUMC keys), physically changes
  rows, and proves one CDC cycle reflects them in the DuckDB target; idempotent re-run;
  `run_due` heartbeat; teardown leaves no orphan objects. Prints `CDC RESULT pass=N fail=0`.

See ADR-0004 in the design study for the rationale and the industry corroboration
(Theobald Table CDC, Fivetran delete-only triggers, SLT/CDS-CDC key-only logging).
