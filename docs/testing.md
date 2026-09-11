# Testing erpl-rev

For contributors. Nothing here is needed to *run* erpl-rev — the operator's runbook is
[`operations.md`](operations.md).

## Before a release

`make e2e` skips two lanes, and they are the two that matter most:

```bash
make e2e         # 13 suites against a live ABAP system, minutes
make e2e-full    # …plus the daemon running for real: soak, daemon, stress
make e2e-perf    # the measured numbers behind docs/perf-results.md
```

CI does **not** run a sanitized build. When a symptom looks like memory rather than
logic — a hang with no query behind it, a value that is wrong in a way no branch
explains, a crash that moves when you add a print — build the tests with
AddressSanitizer by hand and run them:

```bash
cmake -S . -B build-asan -G Ninja -DERPL_REV_SANITIZE=address,undefined
cmake --build build-asan --target erpl_rev_tests
ASAN_OPTIONS=detect_leaks=0 ./build-asan/erpl_rev_tests
```

It only sees code that actually runs, so pair it with a test that drives the suspect
path — `top --once --graph --refreshes 3` exists for exactly that reason.

**`make e2e-full` is the release gate.** It is where the product is driven the way
a customer drives it — a background job that stays up and replicates things nobody
asked it to replicate, including a trigger target end to end. Every defect that has
reached `main` from this tree so far was invisible to the other lanes. It is opt-in
because it takes minutes, not because it is optional.

Run it as one pass rather than filtering to the slow suites with
`ERPL_REV_E2E_ONLY='@soak'`. Two lanes that each pass on their own say nothing
about the order they run in, and this suite has a history of one suite's leftovers
deciding the next one's verdict.

**Expect timing assertions to be load-dependent on a laptop.** `SOAK` fails the
daemon if its heartbeat ever stalls for more than five ticks; on a box that has
been running SAP, the engine and a full e2e for an hour, a seventeen-second gap
at a two-second tick is the machine, not the daemon. Re-run it on a quiet system
before treating it as a defect — and if it reproduces there, it is one.

## What each suite covers

### Delta methods

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
replication dashboard — see [`control-tables.md`](control-tables.md#what-each-run-records).

See the design study (HLD + ADRs) for the rationale behind each decision.

### The trigger tier

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

## Defects this demo found

Building it turned up five things that made trigger CDC unusable in production, all
invisible for the same reason: every automated test drove the tier by calling
`zcl_erpl_rev_cdc=>run()` itself, and not one of these defects is on that path.

1. **The planner gated trigger targets on a column nothing writes.**
   `_erpl_rev_cdc.shadow_rows` was created by migration v3, read as the "work is
   waiting" signal, and assigned nowhere — so a trigger target was never due and the
   daemon could not drive the tier at all.
2. **The daemon ran every planned cycle through the watermark entry point.** The tick
   plan has always said which method each cycle is; nothing read it.
3. **Trigger cycles were invisible to every operator surface** — the apply wrote run
   statistics and the CDC registry but never `_erpl_rev_delta_state`, which is what
   `erpl_rev_targets` is built from, so `top`, `sync ls`, the Prometheus gauges and the
   ALV report all reported a busy target as *IDLE, never run, 0 rows*.
4. **`KEYS_IUD` itself had five defects** and had never completed a cycle anywhere.
5. **A failed cycle was recorded in the registry and nowhere else.** Two daemons
   driving one target run the same cycle concurrently; DuckDB rejects the second with
   `TransactionContext Error: Conflict on update` and `_erpl_rev_cdc.status` goes to
   `ERROR` with the reason stored. But `erpl_rev_targets` is built from
   `_erpl_rev_delta_state`, which never ran — so `top` reported the target as *healthy
   0, never run*, with no error anywhere. The registry knew; the operator's screen did
   not.

Each is fixed, and each now has a test that would have caught it — which is the part
that matters, because the demo found them only by accident:

- **`ZCL_ERPL_REV_DAEMONTEST`, the `DAEMON-CDC` stage** drives a trigger target through
  the real background daemon and never calls `run()`. It covers the first three: rows
  must arrive, the run statistics must name `CDC` as the entry point that ran, and
  `erpl_rev_targets` must report the target as run rather than as never run. Verified
  load-bearing by reintroducing the planner defect.
- **`ZCL_ERPL_REV_PARITYTEST`** diffs every incremental method against an independent
  full load, cell by cell, which is what the fourth needed: every `KEYS_IUD` defect was
  type- or key-specific while the row counts matched.
- **The fifth is fixed in both directions.** A failed apply now writes
  `_erpl_rev_delta_state` as well as the registry — including the refusals thrown
  before the transaction opens, which reached neither record and so re-refused on every
  cycle for ever — and `erpl_rev_targets` carries `cdc_status`/`cdc_error`, so a
  trigger fault is visible even when no cycle has failed. `demo/setup.sh` still refuses
  to record unless exactly one daemon is ticking.

A demo that runs the product the way a customer would is a test nobody thought to
write. The lesson was not to record more demos; it was to write those tests.
