# HLD — APE delta method for erpl-rev (arc42, focused)

Companion: [`ape-delta-brd.md`](ape-delta-brd.md) (requirements, normative for
*what*). This document is normative for *how*. Normative protocol detail lives in
erpl `ape/docs/protocol.md`; this HLD cites its sections instead of copying them.

## 1. Introduction and goals

erpl-rev gains two delta methods, `APE_FULL` (snapshot/seed) and `APE_DELTA`
(incremental with real deletes), for CDS entities on APE-capable S/4 systems
(BRD §2). Quality goals, in order:

1. **No silent wrongness.** Every known engine limitation (no v6 filter pushdown,
   currency shift, async preparation, unreleased entities) fails loudly or is
   documented — never silently approximated.
2. **Crash safety.** A killed cycle loses nothing the engine already handed over
   (spill-before-merge) and never wedges later cycles (stale-graph recovery).
3. **Zero regression.** The five existing methods, the ABAP thin reader, and the
   RFC-server direction are untouched; APE is a new branch in existing dispatch,
   plus one new outbound client component.
4. **Reuse, not re-derivation.** Graph/session/decode logic comes from `erpl_ape`
   (already verified live); no second protocol implementation.

## 2. Constraints

- C-1: erpl-rev is an RFC **server** today (`src/rfc_handlers.cpp`); there is no
  outbound RFC client path. One must be added (new capability, new credentials).
- C-2: v6 reader only (`com.sap.abap.cds.reader.v2`, protocol v6). No v7/gen2, no
  `Delta Load` action, CDS containers only (BRD BR-7–BR-9).
- C-3: Session affinity (protocol §9): one pinned RFC connection per graph
  lifetime. The graph must be created, polled and stopped on the same connection.
- C-4: Preparation is asynchronous (protocol §11, "RESOLVED — poll"): tens of
  seconds before first data, empty port + empty message while pending. The client
  polls `ROUNDTRIP`; it cannot query job status itself (`S_DHCDC*` needed).
- C-5: April-2026 API-policy clearance for `DHAPE_*`/`DHAMB_*` is a launch gate
  (BRD NFR-4). No implementation work starts before it.
- C-6: Control tables are a stable versioned interface
  ([`control-tables.md`](control-tables.md)): new state goes through one ordered
  migration in `src/control_schema.cpp`, idempotent, server-applies-only.

## 3. Context and scope

```
                ┌──────── SAP S/4 (APE 2.x) ────────┐
                │ DHAPE_GRAPH_*  DHAMB_SERVICE_*    │
                │ DHAPE_SUBSCR(V)  CDC jobs /1DH/*  │
                └──────▲─────────────────▲─────────┘
                       │ outbound RFC    │ Open SQL / replicate()
                       │ client (NEW)    │ (existing ABAP reader)
                ┌──────┴─────────────────┴─────────┐
                │        erpl-rev C++ server        │
                │  ApeClient │ cycle │ merge │      │
                │  _erpl_rev_delta_state (+migr.)   │
                └──────┬───────────────────────────┘
                       │ existing scheduler: run/run_due,
                         Z_ERPL_REV_DELTA, daemon
```

External actors: SAP S/4 system (APE engine), operator (CLI), existing ABAP
scheduler (`zcl_erpl_rev_delta=>run/run_due`, `Z_ERPL_REV_DELTA` job, daemon).
Out of scope: SAP-side changes beyond `zcl_erpl_rev_delta` dispatch/registration
parameters; any change to the five existing read paths.

## 4. Solution strategy

- **Reuse `erpl_ape`'s client logic** (graph spec → JSON, session create/poll/stop,
  CSV-package decode, subscription inventory/erase, spill). Recommended packaging:
  consume the private `erpl-ape` repo as a library/submodule inside the erpl-rev
  build (OQ-1 for the implementing session to confirm; fallback is a vendored
  port, which must stay behaviour-identical and is the worse option).
- **Server-driven cycles.** The ABAP scheduler keeps dispatching by method, but
  for APE targets it calls one new server endpoint (e.g. `Z_DUCKDB_APE_RUN`, in
  the style of `Z_DUCKDB_CDC_APPLY`): all graph I/O happens inside the C++
  server on a pinned connection, satisfying C-3. ABAP never holds graph state.
- **Position = subscription.** Unlike watermark methods there is no change-column
  window: the resume token is the named SAP-side subscription (`subscriber_process`
  per target). Cycle commit = merge commit + spill bookkeeping; nothing advances
  a watermark.
- **Full reuses the seed contract**: `APE_FULL` output feeds the same target
  creation + PK path as `zcl_erpl_rev_util=>replicate`, so a later `APE_DELTA`
  registration on the same target just works.

Key design decisions (ADR shortlist):

- ADR-1 *No v6 filter/projection emission.* The reader ignores `/Config/filter`
  and `/Config/schema`; emitting them would return all rows under the appearance
  of selection. Refuse `filters`; project by name client-side (protocol §11).
- ADR-2 *Spill-before-merge + replay.* The engine commits on handover
  (protocol §13); the spill is the only copy after a crash. Persist raw packages
  first, merge second.
- ADR-3 *Termination asymmetry.* Initial load ends on `lastBatch`/port close;
  replication ends after K consecutive empty roundtrips post-data (default 3),
  with the preparation timeout applying only pre-first-data (protocol §13).
- ADR-4 *Release gate default-off.* Unreleased CDS refused unless an explicit,
  WARN-logged override is set (mirrors `erpl_ape_allow_unreleased`).
  Release state is read from the ARS catalog view `I_APISFORCLOUDDEVELOPMENT`
  (`releasedobjecttype='CDS_STOB'`, entity resolved via `DDLDEPENDENCY`
  `OBJECTTYPE='STOB'`); only `RELEASED` passes, `DEPRECATED` included in the
  refusal so no untested branch decides a registration. XCO `get_api_state`
  is not used (resolves STOB existence before ARS); `CDS_PUBLISHED` is not
  used (empty even for released SAP views). Override is set-semantics on
  `allow_unreleased` (no coalesce: unstated re-registration re-enforces) and
  each use writes `last_warning`, rewritten every `register()` call.

## 5. Building-block view

New code only; existing blocks (`cycle`, merge, lease, run-stats, daemon, ABAP
reader) are reused unchanged.

| Block | Responsibility | Notes |
|---|---|---|
| `ApeClient` (new, C++) | Owns one pinned RFC client connection; `Create(spec)` → `Roundtrip` loop → `Stop` (noexcept); capability probe (`DHAPE_GRAPH_VERSION` + `DHAMB_SERVICE_SYSTEM`) | Wraps/reuses `ApeSession`, `ApeGraphSpec`, `ApeDecodedPackage` from `erpl_ape` |
| `ApeCycle` (new, C++) | One cycle: GC stale graphs → create/resume subscription → poll → decode → spill → merge → stats; `recover` replay path that never touches SAP | Called from the new `Z_DUCKDB_APE_RUN` handler; single-threaded per target (graph session affinity) |
| `ApeMerge` (new, thin) | Maps `/1DH/OPERATION`: `U`/blank → keyed upsert, `D` → delete by key; keeps the engine-computed `I`/`U`/`D` verdict for `_erpl_rev_log_<target>` | Reuses the existing MERGE path (`Z_DUCKDB_INGEST MODE=MERGE` semantics) |
| State migration (new) | Extends `_erpl_rev_delta_state` with `subscriber_process`, `chunk_size`, `wireformat`, `last_batch_index`, `spill_batch`; method enum += `APE_FULL`, `APE_DELTA` | One idempotent migration per C-6; CLI/ABAP registration writes the new fields |
| CLI + ABAP registration (extended) | `sync create --method APE_*` flags; `register()` params; cadence-floor refusal for `micro:*` on APE methods | Telling errors per BRD FR-2/AC-4 |
| `cds_delta` (extended) | Recognise `@Analytics.dataExtraction.*` annotations; suggest/refuse `APE_DELTA` accordingly | Small, pure, unit-testable (existing style) |

## 6. Runtime view

`APE_FULL` cycle:

```
run(target) → lease → ApeClient::probe → Create(v6 graph, subscription New/unique)
  → poll ROUNDTRIP (2 s) until lastBatch/port-close [prep timeout guards]
  → per package: decode → spill? (no: full re-runs cheaply) → merge(upsert)
  → Stop(graph) → erase subscription (best-effort) → stats → release
```

`APE_DELTA` cycle (established or new subscription):

```
run(target) → lease → GC stale R-graphs (read_graph_ids_by_status+is_session_active)
  → lookup subscription by (cds, subscriber_process); New or Existing
  → Create → poll: empty pre-data ⇒ preparing (prep timeout); empty post-data ×K ⇒ done
  → per package: spill BEFORE merge → merge(U=upsert, D=delete-by-key)
  → Stop(graph), keep subscription → stats → release
```

`recover` cycle: read latest spilled batch → decode → merge → discard batch. No
SAP contact (a graph would advance the subscription past owed changes).

Concurrency: one active cycle per target (existing lease); one graph per cycle
(session affinity). No intra-cycle parallelism in v1 (chunkSize stays the
throughput knob; protocol §14: 100k×8 ≈ 40 s, wide tables are decode-bound).

## 7. Deployment view

No new process. The server binary gains an outbound RFC client (same `nwrfcsdk`,
new `RfcOpenConnection` client handle + stored `sap_rfc`-style credentials —
secret plumbing to be designed in implementation, reusing ADT credential patterns
where possible). Network: server → SAP gateway/message server must allow
inbound RFC (this is the genuinely new operational requirement vs today's
register-outbound + ADT-HTTP topology). SAP authorisations: RFC rights for
`DHAPE_*`/`DHAMB_*` + `S_DHCDC*` for the CDC jobs + background-job capacity for
preparation.

## 8. Crosscutting concepts

- **Typing.** Package `Attributes.ABAP.Fields` (kind/length/decimals) drives
  decode via the shared DDIC mapper; bind-time catalogue types come from
  `DHAMB_SERVICE_DSET_DEFINITION`. Pack `P` length is bytes → digits×2−1.
  Document the `CURR` currency shift (BRD FR-8) in `delta.md` + F1 help.
- **Errors.** Engine `et_msg` errors throw with the failing phase named
  (create/start/roundtrip); truncated-package diagnostics show head/tail;
  row-width mismatches name package + row. Cleanup (graph stop, full-subscription
  erase) is best-effort and never throws.
- **Observability.** Per-cycle stats into `_erpl_rev_run_stats` (packages seen,
  rows merged, U/D split, polls, preparation wait); reuse the `top`/metrics path.
  Turning the unreleased override on logs WARN (grep-able, as `erpl_ape` does).
- **Security.** Least-privilege comm user for `DHAPE/DHAMB` only; spill tables
  hold raw SAP payload — same access class as targets; subscription names are
  operator-chosen (≤30 chars, no control chars — validate at registration).

## 9. Risks (technical)

- T-1 Stale `R`unning graphs wedge later cycles → GC procedure runs first in
  every delta cycle (BRD R-4).
- T-2 Wide-table decode cost (40 M cells ≈ 28 min under ASAN, protocol §14) →
  document; `chunkSize` tuning; no optimisation in v1 beyond package-bounded
  streaming.
- T-3 `PORT_DATA` NULL vs empty; control envelopes without schema/body → skip
  after reading `lastBatch` (protocol §12). Covered by decode unit tests.
- T-4 Reuse-packaging failure (OQ-1) → fallback is a behaviour-identical port;
  protocol sections + fixture JSONs (`graph_*_initial_load.json`) pin behaviour.

## 10. Implementation plan (for the coding-agent session)

Phase 0 — gates: policy clearance (C-5); OQ-1 packaging decision; credential +
network path for outbound RFC proven with `DHAPE_GRAPH_VERSION` from the server
host. Do not proceed without all three.

Phase 1 — read-only: probe + `DHAMB` browse/definition wired to registration
validation (FR-2) + `cds_delta` annotation recognition. Tests: unit + live probe
against A4H. No graphs yet.

Phase 2 — `APE_FULL` end-to-end (AC-1 first half, AC-4, AC-5). Includes
building-block skeleton (`ApeClient`, `ApeCycle`, handler, migration).

Phase 3 — `APE_DELTA` + spill/recover + subscription lifecycle + stale-graph GC
(AC-1 second half, AC-2, AC-3).

Phase 4 — hardening + docs: cadence floor, F1 help, `delta.md`/`operations.md`
updates, 100k volume proof, full regression (AC-5).

Test strategy: unit (graph JSON golden vs fixtures, decode incl. hostile CSV
values, mapping/validation); integration (recorded packages); E2E on A4H
(`scripts/e2e.sh` style: seed → full → I/U/D → kill-cycle → stale-graph);
negative matrix (AC-4). Definition of done = BRD §6 AC-1–AC-5 all green.

## 11. Phase-2 implementation amendments (supersede §4–§6 where they differ)

ABAP drives DHAPE; there is no C++ outbound RFC client. The shim exposes
`RfcOpenConnection`/`RfcPing` but no invoke surface, and the SDK is gone by
policy, so graph I/O lives in `zcl_erpl_rev_delta=>run_ape_full` (create →
poll `ROUNDTRIP` → stop in one SAP session: session affinity by
construction, protocol §9). Consequences:

- `ApeClient` (§5) is the ABAP poll loop, not a C++ component. `ApeCycle` is
  the server side: per-package decode → stage (`src/ape_cycle.*`) → reconcile
  on `lastBatch`, reached through the new `Z_DUCKDB_APE_RUN` FM (one package
  per call, memory bounded by package size, NFR-1). The APE_GRAPH
  `Z_DUCKDB_PLAN` action returns the graph JSON **raw**: wrapping it in
  `{"graph":…}` hands the flat extractors on both sides one backslash
  instead of a graph.
- FULL scans use unique-per-scan subscription names
  (`subscriber_process` + timestamp, head-truncated to 30 chars) because the
  engine refuses create on a reused name; the scan erases its subscription
  afterwards (verified live: no residue). Staging is `<target>__apesnap`
  (distinct from the SNAPSHOT method's `__snap`); no spill for FULL (a failed
  scan re-runs). The ABAP loop carries a lease heartbeat: a healthy scan
  outlasts the 600 s reclaim TTL.
- Live engine behaviour on A4H, now pinned by tests: create reports the new
  uuid in the message text (`Graph UUID is <32 hex>.`, harvested with
  `FIND REGEX` — `FIND PCRE` fails on this kernel) while `ev_graph_uuid`
  stays empty; the `lastBatch` terminator arrives with `ABAP.Kind =
  "Element"` (scalar descriptor, still carrying a `Body`), so only
  `Kind = "Table"` stages rows and every other kind is a control envelope
  (`test_ape_envelope.cpp`, HLD T-3).
- DELTA termination counts ROWLESS polls, not packages: an idle stream's
  Element `lastBatch` terminator is one such poll, never an exit — exiting
  on it ends the cycle on the first poll, before lagging CDC changes arrive
  (measured: zero packages, clean exit, changes missed). K = 3 rowless polls
  ends idle and drained cycles alike; a 360-poll backstop bounds runaway
  streams (lossless: per-package commits + subscription position). Poll pause
  is adaptive (2 s flowing, 10 s idle) so the 100k volume run stays sane.
- Cycle order in `run_ape_delta` is GC → seed → APE_RECOVER → resume-or-
  create → poll: the seed must precede the replay (a never-cycled target
  has no table for the replay to read), and the replay must precede any SAP
  contact (a graph would advance the subscription past owed changes).
- Drop (FR-9) is `sync drop <target>` → queued `sync_drop` verb → CLIDRV →
  `zcl_erpl_rev_delta=>drop`: refuse on a fresh RUNNING lease, erase the
  SAP-side subscription by id when the target has one (best-effort, like all
  cleanup), then delete the state row plus its spill (an orphaned spill can
  never replay: RECOVER refuses unregistered targets). Unknown targets report,
  they do not fail. The DuckDB target table stays: retention is the operator's
  call. Proven by m8 (refuse, erase-message witness, gone, idempotent,
  re-register, converge, final drop).
- Two-row handovers: a 3-DML commit streams its FIRST TWO DMLs per cycle
  window (measured twice: carry `[I,U]`, restore `[D,U]`; two drained top-up
  cycles found no tails). The third DML's image never streams -- clipped, not
  late. State still converges because the seed-first snapshot covers the tail,
  but the seed MASKS the loss: a seedless consumer would diverge. M4B therefore
  witnesses the streamed delete window-wide (carry-del + restore-del >= 1,
  the restore commit opens with its D so this is structural, not luck) instead
  of demanding single-cycle D timeliness. K stays 3: longer windows would not
  summon a handover the engine never forms, they would only burn classrun
  budget.
- Loop-local `DATA ... VALUE` is a trap: ABAP creates the object once, so a
  per-poll counter declared inside the `WHILE` keeps its first value forever.
  `lv_poll` declared in the DELTA poll loop pinned `lv_empty` at 0 after the
  first posted package -- K = 3 could never fire and the M4B restore looped 92
  rowless polls into TIME_OUT. Per-poll counters live outside the loop with an
  explicit reset at the top (`lv_poll = 0`, same for the FULL loop's `lv_fwd`).
- A first FULL over a stale target legitimately deletes (snapshot-merge
  repair of legacy keys): M3's first-scan `del` caught exactly that, the
  in-method re-run was clean, and the next classrun was fully green. `del = 0`
  on first scan is a property of a converged target, not of the scan.
- Classrun budget: one m1-m6 classrun exceeds the dialog TIME_OUT
  (~10 min, HTTP 500, results lost). The DoD suite is split — APETEST
  (m1-m3), APEDLTA (m4a converge), APEDLTB (m4b surgery/carry/restore),
  APEDLTC (m5 recover, m6 stale graph). A `run()` skipped on a held lease
  returns error-initial with `rs-skipped = X`: tests assert
  `skipped IS INITIAL`, turning vacuous passes into loud skips. An orphaned
  dialog session (HTTP dead, work process alive) keeps heartbeating until
  TIME_OUT; the 600 s lease TTL plus reclaim is what unblocks the next
  cycle.
