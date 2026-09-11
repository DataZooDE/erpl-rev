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

## Three modes

| Mode | Triggers | Log payload | Use when |
|------|----------|-------------|----------|
| **DELETE_ONLY** (default) | `AFTER DELETE` only | keys | inserts/updates already come from the watermark tier — the trigger only closes the physical-delete gap, for minimal write-path overhead |
| **KEYS_IUD** | `AFTER INSERT`/`UPDATE`/`DELETE` | keys only | the source has no usable change column, and it is wide or hot. The cycle coalesces the log to a net op per key and **re-reads the source** for the row values, so the write path carries a key and not a row image |
| **IMAGE_IUD** | `AFTER INSERT`/`UPDATE`/`DELETE` | full row image | the source has no usable change column and **cannot be re-read cheaply** — the log carries the row, so the server upserts I/U and deletes D without going back to SAP |

> **`FULL_IUD` was renamed `IMAGE_IUD`.** The old spelling names what the log holds
> rather than implying "everything", now that a keys-only I/U/D mode exists beside it.
> Stored values were rewritten by control-schema migration v3, and **`FULL_IUD` is
> still accepted on read, permanently** — a system provisioned before the rename keeps
> working, and an unrecognised mode is never silently downgraded to `DELETE_ONLY`,
> because that would quietly stop capturing inserts and updates.

**`DELETE_ONLY` remains the default**, and provisioning without a mode still yields it.
Choosing between the other two is a write-path question, not a correctness one: both
capture I/U/D. `KEYS_IUD` moves the cost from the source's write path (a narrow log
row) to the cycle (a re-read); `IMAGE_IUD` does the reverse.

Measured on a 5-key, ~400-column table at 2,000 changes ([`perf-results.md`](perf-results.md)):
`KEYS_IUD` costs **14x less on the write path** (43 ms vs 619 ms) and about **2.8x
more per cycle** (2659 ms vs 936 ms). So the question is whose time you are spending.
The write path sits inside the customer's business transaction; the cycle is the
replicator's own, and asynchronous. For a wide, hot table `KEYS_IUD` is the better
trade; where the table is not hot, `IMAGE_IUD` moves fewer bytes overall.

## Using it

From the CLI, which is where most of this belongs:

```bash
# 1. seed the DuckDB target (a normal full load)
erpl-rev replicate --table ZDELTA_WM --target cdc_wm

# 2. register it on the trigger tier
erpl-rev sync create cdc_wm --method CDC --source ZDELTA_WM --keys CLIENT,ID \
    --cadence micro:2 --log

# 3. provision the triggers (creates the ZCDC_* log, sequence and triggers in
#    the SAP database). Source and keys come from the registry -- they were
#    given once, at step 2.
erpl-rev cdc provision --target cdc_wm --mode KEYS_IUD

# 4. from here the daemon runs the cycles; or drive one by hand:
erpl-rev sync run cdc_wm
```

**Step 1 is not optional and its order matters.** A cycle applies a *delta*; with
no target table the first one errors, the registry leaves `SEEDED`/`ACTIVE`, and
the tick planner then skips the target silently on every tick — a trigger tier
that looks provisioned and does nothing. `cdc status` is what tells you.

The same from ABAP, which is still the only way to reach some of the tuning
parameters:

```abap
" 1. seed the DuckDB target (a normal full load)
zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'cdc_wm' ).

" 2. provision the triggers (creates ZCDC_* log/sequence/trigger on the SAP DB)
zcl_erpl_rev_cdc=>provision(
  iv_target = 'cdc_wm' iv_source = 'ZDELTA_WM' iv_keys = 'CLIENT,ID'
  iv_mode = 'DELETE_ONLY' ).        " or 'KEYS_IUD' / 'IMAGE_IUD'

" 3. each cycle: stage new log rows -> apply in the server -> prune the log
DATA(r) = zcl_erpl_rev_cdc=>run( 'cdc_wm' ).   " r-ins / r-upd / r-del / r-applied

" 4. when done: drop the triggers + log table + sequence
zcl_erpl_rev_cdc=>teardown( 'cdc_wm' ).
```

`run` is what a periodic job calls (like the watermark/snapshot tiers). Provisioning is
idempotent — it best-effort drops any leftover objects first, so it is safe to re-run.

## Letting the daemon drive it

Step 3 above is for a job you schedule yourself. Register the target with
`method = 'CDC'` and a micro cadence, and the daemon runs the cycles instead — nobody
calls `run` at all:

```abap
zcl_erpl_rev_delta=>register( VALUE #(
  target = 'cdc_wm'  method = 'CDC'  source_from = 'ZDELTA_WM'
  keys = 'CLIENT,ID'  cadence = 'micro:2'  log_enabled = 'true' ) ).
```

A trigger target is **not polled on a clock**: it is due the moment a row appears in the
shadow table, and the cadence is a floor on how often it is checked rather than a
polling interval. `micro:2` is a ceiling on latency.

**The order in the previous section is not a suggestion.** The planner only schedules a
trigger target whose `_erpl_rev_cdc.status` is `SEEDED` or `ACTIVE`. Provision before
the DuckDB target exists and the first cycle has nothing to apply to: it errors, the
status leaves that set, and every later tick skips the target **silently** — no
failure, no backoff, nothing on any operator surface. Seed first.

To confirm the daemon really is driving it, rather than assuming:

```sql
-- the entry point that actually ran; a trigger target should show CDC, not FULL
SELECT method, status, count(*) FROM erpl_rev_run_stats
WHERE target = 'cdc_wm' GROUP BY 1, 2;

-- and the operator views, which is what `top` and `sync ls` read
SELECT target, status, lag_seconds, last_ins, last_upd, last_del
FROM erpl_rev_targets WHERE target = 'cdc_wm';

-- if it is being skipped, this says why
SELECT status, error, shadow_rows FROM _erpl_rev_cdc WHERE target = 'cdc_wm';
```

`erpl-rev top` shows the same thing per cycle in its `LAST CYCLE` column, and the
throughput graph draws one glyph per operation.

### When a trigger target goes quiet

A trigger set can break without any cycle failing: a trigger dropped by a transport,
a log table removed, a provisioning that never finished. That leaves
`_erpl_rev_cdc.status` outside `ACTIVE`/`SEEDED`, which **silently stops the planner
scheduling the target** — while `_erpl_rev_delta_state` still holds whatever the last
successful cycle wrote. The target stops replicating, its lag ages, and its status
still reads `IDLE`.

`erpl_rev_targets` therefore carries the trigger registry's own state as `cdc_status`
and `cdc_error`, and **a trigger target whose registry is not `ACTIVE` or `SEEDED` is
not healthy**, whatever the last good cycle left behind. Every surface that reads the
view inherits that: `top` colours the row and prints the reason, `sync ls` and the
Prometheus gauges report it unhealthy, and the ALV report shows it too. For a target
that was never on the trigger tier both columns are `NULL`, so a watermark target is
not judged by a registry it does not have.

A failed *cycle* is recorded in the same place. `CdcApply` records every failure —
including the ones it refuses before opening a transaction, which previously went
unrecorded anywhere and so re-refused on every cycle forever — into both the registry
and `_erpl_rev_delta_state`, with the reason and an incremented `fail_count`.

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
  `test/test_duckdb_bridge.cpp`): the dialect golden strings (delete-only + IMAGE_IUD,
  namespace guard, AnyDB refusal), the `_erpl_rev_cdc` state machine (transitions,
  monotonic position, restart-safe), log coalescing, and the apply (delete reflected,
  IMAGE_IUD I/U/D, idempotent, rollback-on-error).
- **E2E on A4H (real HANA triggers)** — `ZCL_ERPL_REV_CDCTEST` (run by `make e2e`):
  provisions real HANA triggers on `ZDELTA_WM` (delete-only + IMAGE_IUD) **and on
  SFLIGHT** (the flight-booking demo — composite DATE+NUMC keys), physically changes
  rows, and proves one CDC cycle reflects them in the DuckDB target; idempotent re-run;
  `run_due` heartbeat; teardown leaves no orphan objects. Prints `CDC RESULT pass=N fail=0`.
- **E2E, driven by the daemon** — the `DAEMON-CDC` stage of `ZCL_ERPL_REV_DAEMONTEST`.
  This one never calls `run`. It registers a trigger target beside a watermark target on
  the *same source*, starts the real background daemon, and asserts that rows arrive,
  that the run statistics name `CDC` as the entry point that ran, that
  `erpl_rev_targets` reports the target as run rather than as never run, and that a
  physical delete leaves the trigger target while the watermark target keeps it.

- **Parity against an independent full load** — `ZCL_ERPL_REV_PARITYTEST`. Two paths
  to the same data must agree cell by cell: path A is the incremental method under
  test, over a type-spanning source with an edge-value corpus (negative decimals,
  NUMC leading zeros, empty rather than null, unicode, DATS/TIMS boundaries); path B
  is a plain full load at the same moment. `diff_joindiff` from the
  [anofox-tabular](https://github.com/DataZooDE/anofox-tabular) DuckDB extension does
  the comparison.

  **Each method is held to its own claim, because not all of them make the same one:**

  | Method | Workload | Expected |
  |---|---|---|
  | CDC `KEYS_IUD` | insert, update, physical delete | exact parity |
  | CDC `IMAGE_IUD` | insert, update, physical delete | exact parity |
  | `SNAPSHOT` | insert, update, physical delete | exact parity |
  | `WATERMARK` | insert, update | exact parity |
  | `WATERMARK` | …then a physical delete | **must diverge**, by exactly the deleted rows |

  That last row is the reason this tier exists, written as a test instead of as a
  sentence: a watermark reads rows whose change column moved, and a deleted row has
  no change column left to read. If it ever passes, either the workload stopped
  deleting or the diff stopped comparing — and both have happened here before.

  `IMAGE_IUD` is not redundant with `KEYS_IUD`: one takes its values from the logged
  row image and the other from a re-read of the source, so a coercion bug in one path
  and not the other is invisible to a single-mode test.

  Not covered yet, and named rather than left silent: CDC `DELETE_ONLY`, whose claim
  is only meaningful paired with a watermark tier, and `CHANGEDOC`, which would need
  synthetic change documents built for the fixture's five-part key first.

  Every defect found in `KEYS_IUD` was type-specific or key-specific **while the row
  counts matched** — rows deleted and re-inserted empty, values coerced wrong, keys
  joined on the wrong column. A count is blind to all of it; a diff names the key and
  the column. The suite carries its own negative controls: a tampered cell and a
  tampered key must both be caught, or a diff that returns zero has proved nothing.

  Both paths are ours, so a shared coercion bug would agree with itself. The
  comparison that crosses the boundary to SAP is `sync validate --full`, and that is
  the anchor.

  It exists because three defects reached `main` together — the planner gating trigger
  targets on a column nothing wrote, the daemon running every planned cycle through the
  watermark entry point, and the trigger apply never writing `_erpl_rev_delta_state` —
  and **all three were invisible to every test above**, because every one of them drove
  the tier by calling `run` itself. A recorded demo found them instead. Verified
  load-bearing by reintroducing the planner defect: the stage goes red.

See ADR-0004 in the design study for the rationale, and for how the three
established approaches compare: table-level trigger CDC, delete-only triggers
alongside a watermark tier, and SLT/CDS-CDC key-only logging.
