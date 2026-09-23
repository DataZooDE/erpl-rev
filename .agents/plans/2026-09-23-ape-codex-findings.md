## Goal

Fix the 8 Codex read-only review findings against the APE sixth delta path
(`APE_FULL` + `APE_DELTA`, commits `770420c` + `99f9bf5`) and re-earn the
"lossless" verdict by re-running the full DoD (BRD §6 AC-1–AC-5) on A4H.
Red/green TDD throughout; one commit at the end.

## Success Criteria

- The cross-cycle spill loss (W1) is fixed and proven by a kill-cycle test
  that fails before / passes after.
- `drop` reports erasure only when SAP confirms it (W2); every other cleanup
  keeps its best-effort contract.
- `drop` vs in-flight cycle is fenced atomically (W3); no check-then-act
  across RFC calls.
- DELTA pre-data polls never count toward K (W4); K = 3 applies post-data
  only, per FR-5; a quiet NEW subscription warns observably, resumed quiet
  streams exit fast and clean (AC-3).
- A FULL scan whose stream stalls after first data exits boundedly and still
  stops its graph (W5).
- `columns`-subset projection works end to end (W6, per Q1 decision):
  FULL seeds and DELTA replicates exactly the registered columns by name,
  unknown names fail loudly at bind/plan (AC-4), keys outside the subset
  are refused; filter/micro refusals stay green.
- AC-4 live negatives and AC-5 full regression are green on A4H (W7–W8),
  including a re-run of AC-1/AC-2/AC-3 after the fixes.

## Context And Current Facts

- Design: `docs/ape-delta-brd.md` (FR-5 prep timeout pre-data, FR-6
  spill-before-merge, FR-9 drop+eraser, BR-8 columns, AC-4/AC-5),
  `docs/ape-delta-hld.md` §11 amendments (ABAP drives DHAPE; DELTA batching,
  GC → seed → RECOVER → resume/create → poll; K = 3 rowless; drop flow).
- W1 verified: `run_ape_delta` restarts `lv_post` at 0 every cycle
  (`abap/zcl_erpl_rev_delta.abap:1467`) while `spill_batch` persists across
  cycles (`src/ape_cycle.cpp:292,325`); spill upserts on
  `(target, batch_index)` (`src/ape_cycle.cpp:282-285`); RECOVER replays only
  `batch_index > spill_batch` (`src/ape_cycle.cpp:301-303`). Kill between
  spill and merge in any cycle whose indexes sit at/below the stale
  `spill_batch` replays nothing: silent loss. `spill_batch` also regresses
  (k → 0), widening the hole.
- W2 verified: `ape_erase` returns void, swallows everything
  (`TRY/CATCH cx_root`, `:1596-1632`), never inspects `et_msg`; `drop`
  reports "subscription erased" on subid-found, not SAP-confirmed
  (`:1716-1723`).
- W3 verified: `drop` checks fresh-RUNNING-lease, then erases + deletes state
  in later calls (`:1698-1728`); a cycle can start/heartbeat between the
  check and the delete.
- W4 verified: the DELTA loop counts pre-data rowless polls toward the same
  K = 3 exit (`:1505-1512`); there is no preparation timeout on the DELTA
  side (the 60-poll one at `:1273` is FULL-only). A slow-preparing graph
  exits clean with zero rows: vacuous green.
- W5 verified: the FULL loop exits on error / `lastBatch` / pre-data
  prep-timeout only (`:1237-1290`); once `lv_got = true`, a stalled stream
  loops forever on the lease heartbeat and `ape_stop` never runs: wedged
  target + leaked graph.
- W6 verified: `ValidateRegistration` refuses `filter` but never looks at
  `columns` (`src/ape_validate.cpp:24-42`); no unknown-column bind check
  exists anywhere on the APE path, so the AC-4 probe has nothing to hit.
- W7/W8 gap: unit refusals exist (`test/test_ape_register.cpp`: filter,
  micro, subscriber names) and merge/recover units exist
  (`test/test_ape_cycle.cpp`, incl. "recover replays a spilled batch");
  no test pins cross-cycle batch monotonicity; live A4H negatives
  (unreleased, non-CDS, unknown column) and a post-fix full regression were
  never run.
- Standing constraints: red/green TDD, repo artefacts only (`.agents/plans/`
  convention kept), single final commit, scratch tables cleaned, hermetic
  quack ports kept (`99f9bf5`).

## Constraints And Non-goals

- No change to the five existing methods, the ABAP thin reader, or the
  RFC-server direction (BR-10).
- Cleanup stays best-effort and never throws out of a cycle (FR-4 style);
  only the *report* becomes honest (confirmed vs unverified).
- No new scheduler, no v7, no pushdown (BR-7/BR-8).
- Column-set changes on re-registration require a FULL re-seed (the
  target carries exactly the registered columns); changing `columns` on an
  APE_DELTA target without re-seeding is refused with a telling error.
- The quack port flake is already fixed hermetically; no work unit here.

## Key Decisions

- D1 Monotonic batches, not spill epochs: seed each DELTA cycle's
  `lv_post` from `spill_batch + 1` (ABAP reads state anyway). Keeps the
  `(target, batch_index)` PK and the C++ side nearly untouched; gaps after
  purge are harmless (`>` comparison, purge deletes all). Rejected:
  per-cycle epoch in the spill PK (heavier migration, same guarantee).
- D2 Witness, don't harden, the eraser: scan `et_msg` for E/F/A (the
  erpl_ape rule already used for poll loops) and re-run
  `ape_find_subscription`; `drop` reports "erase confirmed" vs "erase
  unverified". Rejected: raising out of cleanup (breaks FR-4-equivalent
  best-effort) and silent best-effort as today (dishonest report).
- D3 Atomic lease-take in `drop`: one conditional `UPDATE` (take iff no
  fresh RUNNING lease) with affected-rows check, plus `run()` re-validating
  the state row after lease acquire; `due()` skips `DROPPING`. Rejected:
  generation counters (no column, new migration, same guarantee for one
  cycle per target).
- D4 Shared pure termination predicate (`ape_poll_state`, public for
  headless classrun probes -- the repo has no testclasses deploy/run
  plumbing). DELTA: K = 3 post-data with pre-data excluded; a NEW
  subscription warns (never errors) after 36 quiet polls, resumed ones exit
  fast -- erroring quiet cycles would fail every idle drain (AC-3), and a
  drained stream is indistinguishable from slow prep at poll level. FULL:
  60-poll prep error (unchanged) + 360-poll post-data stall error.
  Rejected: unifying the two loops (different ends: lastBatch vs K);
  erroring DELTA pre-data quiet (breaks AC-3, found live via m6).
- D5 DoD rerun is part of done: AC-1/AC-2/AC-3 re-proven after W1–W6, then
  AC-4/AC-5; new live negatives ride the existing classrun split
  (APETEST/APEDLTA/B/C/V + new APEDLTN) to respect the ~10 min classrun
  budget.
- D6 Projection by name at stage/merge time (Q1: full subset, not
  refusal-only). `columns` is stored on the registration (reuse `extra` if
  red-phase discovery confirms it is the generic params carrier, else one
  ordered `AddColumnIfMissing` migration per C-6, following the
  `subscriber_process` precedent). FULL stages only requested columns and
  the seed creates the target with exactly that set; DELTA merge projects
  to the same set; every name resolves case-insensitively against the
  package fields, unknown names fail at bind/plan (AC-4), keys must be a
  subset of `columns`. Absent/empty `columns` means all columns (today's
  behavior, untouched). Rejected: refusal-only (against the Q1 call) and
  positional projection (FR-7 is by-name).

## Recommended Approach

Phase P1 (losslessness first): W1 → W2. Phase P2 (races/timeouts): W3 →
W4 → W5. Phase P3 (contract): W6. Phase P4 (DoD): W7 → W8. Each unit:
red test (C++ `test/test_ape_*` or ABAP Unit / live class) → minimal fix
→ green → no unrelated cleanup. ABAP goes through `scripts/deploy-abap.sh`;
C++ through `cmake --build build-sandbox` + `./build-sandbox/erpl_rev_tests`.

## Work Plan

- W1 Spill monotonicity (P1, critical). Red: C++ test pinning
  cross-cycle crash (spill 0..2, pos = 2, purge, new-cycle batches 3..4,
  kill between spill/merge of 4 → RECOVER must replay 4; today replays
  nothing). Green: seed `lv_post` from `spill_batch + 1` at DELTA cycle
  start (`-1` → 0 start); document ABAP-`i` headroom. Live: M5-style
  kill-cycle asserting zero loss/duplication. Files:
  `abap/zcl_erpl_rev_delta.abap`, `test/test_ape_cycle.cpp`,
  `abap/zcl_erpl_rev_apedltc.abap`.
- W2 Eraser witness (P1). Red: live `drop` on a scratch target asserting
  the message says "erase confirmed". Green: `ape_erase` returns a flag
  (E/F/A scan + post re-lookup via `ape_find_subscription`);
  `drop`/`ape_erase_for_graph` report confirmed vs unverified, still never
  raise. Files: `abap/zcl_erpl_rev_delta.abap`, APEDLTV/drop live test.
- W3 Drop fencing (P2). Red: deterministic interleave test (fresh RUNNING
  lease → drop refused; stale lease → drop proceeds) extended with a
  take-lease assertion (second concurrent take fails). Green: atomic
  conditional lease-take UPDATE in `drop`; `run()` re-validates state row
  post-acquire; `due()` skips `DROPPING`. Files:
  `abap/zcl_erpl_rev_delta.abap` (`drop`, `run`, `due`).
- W4 DELTA prep bound (P2). Red: headless probes on the extracted
  predicate (pre-data empties do NOT count toward K; new-sub quiet →
  warn; resumed quiet → nothing; 3 post-data empties → done). Green:
  extract predicate, gate K on stream-rows `lv_got`, warn (last_warning)
  + clean exit on new-sub quiet. Files: `abap/zcl_erpl_rev_delta.abap`,
  scratch probes (durable: m6 exercises resumed-quiet live).
- W5 FULL stall cap (P2). Red: ABAP Unit test (post-data stall of 360 polls
  → bounded exit). Green: cap post-data polls at 360 → error exit with
  `ape_stop` still running (it already does on every loop exit). Happy-path
  FULL unchanged. Files: same as W4.
- W6 Columns-subset projection (P3, Q1 scope). Red: (i) unit —
  registration with an unknown `columns` entry fails, keys outside
  `columns` fail, projection maps package fields by name case-insensitively
  onto a subset target incl. D-by-key on subset keys; (ii) live — subset
  FULL seed creates a subset target, subset DELTA replicates I/U/D
  correctly. Green: add `columns` to `ApeRegistration` + CLI (`--columns`
  exists generically at `cmd_sync.cpp:855`) + ABAP `register()` + state
  storage (D6); validate names against the DHAMB definition probe (reuse
  the FR-2 path) with telling errors; FULL stages/finalises the subset
  (`ApeApplyFullPackage`/`ApeFinalizeFull` project by name); DELTA merge
  projects to the subset (`MergeDeltaEnvelope`); re-registration changing
  `columns` without a FULL re-seed is refused. Files:
  `src/ape_validate.*`, `src/ape_cycle.*`, `src/cmd_sync.cpp`,
  `src/control_schema.cpp` (only if `extra` is not reusable),
  `test/test_ape_register.cpp`, `test/test_ape_cycle.cpp`,
  `abap/zcl_erpl_rev_delta.abap`, AC-4/APEDLTN class.
- W7 AC-4 live negatives (P4). New APEDLTN class: unreleased refused by
  default, filter refused, non-CDS refused, `micro:*` refused, unknown
  column fails; each asserts the telling error text. A4H classrun.
- W8 Full DoD re-green (P4). Re-run AC-1 (incl. 100k volume), AC-2, AC-3
  after W1–W6, then AC-5 (five-method suites unmodified) plus C++
  `19757`-scale suite; single commit + push.

## Validation Plan

- C++: `cmake --build build-sandbox --target erpl_rev_tests erpl_rev_server`
  then `./build-sandbox/erpl_rev_tests` (focused `[ape]` tags red-first,
  full suite before commit).
- ABAP Unit: new predicate tests run green; deploy via
  `scripts/deploy-abap.sh`.
- A4H live: existing split APETEST / APEDLTA / APEDLTB / APEDLTC / APEDLTV
  all green, plus new APEDLTN; kill-cycle (W1/AC-2), stale-graph (AC-3),
  100k volume (AC-1) re-proven post-fix; subset seed + I/U/D replication
  green (W6); `skipped IS INITIAL` assertions stay (no vacuous passes);
  scratch targets dropped, spill tables clean.
- Highest-risk step: W1 live kill-cycle proof — it is the experiment that
  re-earns "lossless".

## Risks / Rollback

- A4H classrun TIME_OUT (~10 min): mitigated by the existing class split +
  APEDLTN; a red that needs long streams goes to a dedicated classrun.
- Orphaned dialog sessions holding leases: 600 s TTL + reclaim already
  covers; W3 does not change TTL semantics.
- Residual W3 window (engine-side create/resume vs drop): documented, loud
  on collision (engine error, GC recovers); no silent divergence path
  remains after W1.
- Rollback: single commit; revert it. No migration change is planned
  (spill PK untouched by D1); if W6 needs one, it goes through the ordered
  `control_schema.cpp` path per C-6.

## Open Questions

None. Q1 decided: full columns-subset client-side projection (D6/W6).
