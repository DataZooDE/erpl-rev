# BRD — APE as a delta method for erpl-rev

| | |
|---|---|
| Status | Implemented; DoD green on A4H (AC-1–AC-5), plus Codex-findings rework (see HLD §12) |
| Companion | [`ape-delta-hld.md`](ape-delta-hld.md) (arc42, normative for design) |
| Sources of truth | erpl `ape/docs/protocol.md` §§11–14; erpl-rev [`delta.md`](delta.md), [`cdc.md`](cdc.md), [`control-tables.md`](control-tables.md) |

## 1. Problem

erpl-rev replicates SAP tables, CDS views and BW/calc views into DuckDB with five
delta methods (`WATERMARK`, `INSERT_ONLY`, `CHANGEDOC`, `SNAPSHOT`, trigger-`CDC` —
see [`delta.md`](delta.md)). Four of them are emulations built from Open SQL reads:

- A watermark is structurally blind to physical `DELETE`s.
- `SNAPSHOT` is the only emulation that reflects deletes, and it re-reads the whole
  source — infeasible for large targets.
- Trigger-`CDC` catches deletes incrementally but needs customer-owned triggers and a
  log table on the source (see [`cdc.md`](cdc.md)), which many customers will not
  approve.

SAP's **ABAP Pipeline Engine (APE)** already solves exactly this: its CDS reader
(`com.sap.abap.cds.reader.v2`, protocol v6) streams initial loads **and**
replication with a per-row change indicator (`/1DH/OPERATION`: `U` after-image,
`D` keys-only delete). The erpl project has a working client (`erpl_ape`:
`ape/docs/protocol.md`, verified live against the A4H trial, APE 2.7.0). The
shipped design does **not** port it: erpl-rev stays an RFC **server** (ABAP
calls in) and ABAP itself drives DHAPE inside its own session
(`DHAPE_GRAPH_MANAGER` / `DHAPE_GRAPH_ROUNDTRIP`); the C++ server receives
packages and performs staging/merge/recovery (see HLD §11–§12 for the
inversion of the original client-port plan).

## 2. Objective

Add APE as an opt-in, sixth delta path for **CDS entities on APE-capable S/4
systems**, reusing the proven `erpl_ape` protocol work instead of a third delete
emulation:

- `APE_FULL` — one-shot snapshot of a CDS entity (seed path, replaces `replicate()`
  for APE sources).
- `APE_DELTA` — incremental replication keyed by a stable `subscriber_process`,
  resumable across cycles, with **real deletes**.

## 3. Scope

### In scope

- BR-1: Register APE targets from the CLI (`sync create --method APE_DELTA ...`)
  and from ABAP (`zcl_erpl_rev_delta=>register`), same registry
  (`_erpl_rev_delta_state`) and same per-target lease/cadence/run-stats machinery
  as the five existing methods.
- BR-2: Drive the APE graph lifecycle ABAP-side (create → poll `ROUNDTRIP` →
  stop inside the existing session; **no** outbound RFC client path — this
  inverts the original port plan, see HLD §11). The C++ server handles
  package decode, spill, staging/merge, and recovery.
- BR-3: Merge semantics: `U` → keyed upsert, `D` → delete by key, blank operation
  (initial load) → upsert. The engine's `I`/`U`/`D` verdict stays server-computed,
  consistent with [`control-tables.md`](control-tables.md).
- BR-4: Crash recovery for delta: an interrupted cycle's already-handed-over
  packages must be replayable (the engine advances its pointer on handover and
  never re-sends — protocol §13). Reuse the spill concept (`erpl_ape.delta_spill`
  or an equivalent under erpl-rev ownership).
- BR-5: Subscription lifecycle: full-load subscriptions are erased after the scan;
  delta subscriptions persist under `subscriber_process` and are dropped only by an
  explicit command (mirror of `PRAGMA sap_ape_drop`).
- BR-6: CDS-annotation alignment: APE needs
  `@Analytics.dataExtraction.enabled: true` (+ `.delta.changeDataCapture.automatic:
  true` for replication). erpl-rev's `cds_delta` annotation derivation
  (`src/cds_delta.hpp`) should recognise these and guide registration (refuse with
  a telling error, or auto-suggest `APE_DELTA`).

### Out of scope (explicit non-goals)

- BR-7: The gen2 reader (`com.sap.abap.reader`, protocol v7) stays parked —
  `STATE_UUID` is never populated on observed releases, so it cannot extract
  (protocol §13 "Still open"). No v7 code paths.
- BR-8: No server-side filter/projection pushdown on the v6 reader. The v6 reader
  reads seven config paths, none of them a filter or schema (protocol §11); the
  build must **refuse** a filter parameter rather than silently return all rows,
  and apply `columns` client-side by name. SQL `WHERE` applies on the stream.
- BR-9: No non-CDS containers (SLT, ODP), no `Delta Load` action, no unreleased
  entities by default (release gate, overridable with an explicit, logged opt-in).
- BR-10: No change to the five existing methods, to the ABAP thin-reader
  architecture, or to the RFC-server direction. Non-APE systems and plain tables
  keep working exactly as today.

## 4. Functional requirements

| ID | Requirement |
|---|---|
| FR-1 | `sync create --method APE_FULL\|APE_DELTA --source <CDS> --keys <k> --subscriber-process <NAME> [--chunk-size N] [--wireformat W] [--columns C,...] [--allow-unreleased]` registers a target; same create-or-update semantics as existing methods. |
| FR-2 | Registration probes the source and fails fast with a telling error when: the system has no APE/DHAPE (`DHAPE_GRAPH_VERSION`), the entity is not a CDS view (`DHAMB_SERVICE_DSET_DEFINITION` `OBJECTTYPE`), the entity lacks the `dataExtraction` annotation (DDIC annotation service; `APE_DELTA` additionally needs `changeDataCapture.automatic`), or it is unreleased and the override is off. Release state comes from the ARS catalog (`I_APISFORCLOUDDEVELOPMENT`, `CDS_STOB`/`RELEASED`) — never XCO (its STOB existence check fails before ARS) and never `CDS_PUBLISHED` (empty even for SAP views). The override (`--allow-unreleased`) is present-tense: a re-registration that does not restate it re-enforces the gate; each use is WARN-logged on the target row (`last_warning`, observable via `state()`). |
| FR-3 | A cycle for an APE target runs on the existing scheduler (`run`, `run_due`, `Z_ERPL_REV_DELTA`, daemon): lease → extract → keyed merge → commit position → release. No new scheduler. |
| FR-4 | `APE_FULL` terminates on `message.lastBatch`; any exit without it is an error and leaves the target untouched (the server merges only on `lastBatch`). Pre-data polls are bounded by a 60-poll preparation error; 360 rowless polls after first data end a stalled scan as an error. The scan owns its subscription and erases it afterwards (best-effort, never throwing out of cleanup). |
| FR-5 | `APE_DELTA` resumes the named subscription when it exists (no "already exists" failure) and creates it otherwise; post-data it ends after 3 consecutive empty roundtrips (replication graphs never send `lastBatch`). Pre-data quiet never counts toward K: a quiet newly created subscription warns observably (`last_warning`) and exits clean, a quiet resumed one exits clean after 3 polls; a 360-poll backstop (absolute for DELTA, consecutive-rowless for FULL) bounds every scan. |
| FR-6 | Every delta package is persisted **before** its rows are merged, so a crash between handover and commit loses nothing; a `recover`-style replay re-applies every spilled batch above the merged position, in order, without touching SAP. Batch counters continue past the position across cycles (a clean purge starts a new generation); a restarted counter is refused loudly. |
| FR-7 | Row mapping is by package-field **name** (case-insensitive fallback), not position; a row whose cell count disagrees with the declared field count is refused with package/row identity, never silently shifted. |
| FR-8 | Currency-shifted `CURR` amounts under `...Plus Currency` wire formats are documented and surfaced consistently (they differ from `RFC_READ_TABLE` values by design — protocol §12); the parity/diff harness must compare against APE semantics, not Open-SQL semantics, for APE targets. |
| FR-9 | An explicit drop command removes the SAP-side subscription (`subscr.eraser.v1` path) and the local state row; crashed-cycle recovery (stale `R`unning graphs blocking later cycles) follows the documented `read_graph_ids_by_status` + `is_session_active` procedure. The drop reports `erase confirmed` only when SAP raised no error and a re-lookup proves the subscription gone, otherwise `erase unverified` (cleanup stays best-effort); it refuses a fresh cycle lease, lets a stale one through, and fences the final state-row delete against a cycle starting mid-drop. |

## 5. Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-1 | Memory per cycle bounded by package size, not result size (decode one package → merge → discard; protocol §14 measured flat RSS). |
| NFR-2 | Cadence floor: APE targets must not be scheduled sub-minute. Preparation alone takes ~tens of seconds on first run; the daemon's 2 s tick is for Open-SQL methods. Registration rejects `micro:*` cadences for APE methods with a telling error. |
| NFR-3 | SAP prerequisites from protocol §§6/9/11 hold: `S_DHCDC*` authorisations for the CDC jobs, one pinned connection per graph lifetime (session affinity), background-job infrastructure for preparation. Missing prerequisites surface as the engine's own error text, not a generic failure. |
| NFR-4 | April-2026 API-policy clearance for `DHAPE_*`/`DHAMB_*` is obtained **before** implementation (delta.md promises no policy-restricted interfaces; APE must clear the same bar). This is a launch gate, not a footnote. |
| NFR-5 | Zero new SAP-side footprint is the goal: no new `Z` table, no trigger, no DDIC structure. Any ABAP addition is limited to dispatch/registration parameters in the existing `zcl_erpl_rev_delta` path. |

## 6. Acceptance criteria

- AC-1: E2E on A4H: seed `ZERPL_APE_D`-class fixture → `APE_FULL` matches the Open-SQL read cell-for-cell (modulo documented currency shift) → insert/update/delete via `APE_DELTA` with correct `/1DH/OPERATION` handling, including a 100k-row volume run with key-count == row-count.
- AC-2: Kill-the-cycle test: interrupt a delta mid-merge; the replay path restores every handed-over package and the next ordinary cycle resumes without loss or duplication (keyed merge absorbs re-delivery).
- AC-3: Stale-graph test: kill without stopping; the next cycle recovers (GC procedure) instead of wedging on "subscription still in use".
- AC-4: Negative tests: unreleased entity refused by default; filter parameter refused (not ignored); non-CDS source refused; `micro:5` cadence refused for APE methods; unknown column in `columns` fails at bind/plan time.
- AC-5: Existing five methods' test suites pass unmodified (no regression on non-APE paths).

## 7. Risks

- R-1 (policy): `DHAPE`/`DHAMB` fall under a restriction erpl-rev promised to avoid → mitigation: NFR-4 gate before any code.
- R-2 (stack coverage): APE needs S/4 + DHAPE/DHAMB + annotated CDS; ECC and unannotated estates stay on existing methods → mitigation: capability probe + telling errors (FR-2), never a silent fallback.
- R-3 (latency expectations): users map "real-time daemon" onto APE → mitigation: NFR-2 cadence floor + docs; the 2 s story stays Open-SQL-only.
- R-4 (stale graphs wedging subscriptions): a crashed cycle blocks everything after → mitigation: FR-9 recovery procedure runs before each delta cycle (as the erpl_ape harness does).

## 8. Open questions for the implementing session

- OQ-1: Reuse packaging — submodule/shared-lib/port of `erpl_ape` (HLD §4 recommends reuse; confirm licensing of the private repo inside erpl-rev's build).
- OQ-2: Spill ownership — reuse `erpl_ape.delta_spill` in the same DuckDB file or a `_erpl_rev_ape_spill` table under migration control (HLD §6).
- OQ-3: Exact `subscriber_process` naming rules (≤30 chars, no control chars — inherited from `erpl_ape`) vs erpl-rev target-name conventions.
