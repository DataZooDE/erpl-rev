# Delta (incremental) extraction

erpl-rev can keep a DuckDB target in sync with a SAP source **incrementally** —
loading only what changed since the last cycle — on top of the existing full-load
path. All merge logic and all delta state live in the C++/DuckDB server; the ABAP
side stays a thin reader that selects changed rows with plain Open SQL — except
for the APE path (§"The sixth path" below), where ABAP itself drives SAP's
DHAPE engine in-session and the server handles packages, staging, and merge.
**No SAP interface restricted by the April-2026 API policy is used** (no ODP-RFC,
no SAPI/BW Service API, no `RFC_READ_TABLE`) — every source read is Open SQL /
CDS / native-SQL in a customer `Z` function module.

In short: **choose a method** (table below), **register** it with `sync create`,
**run once and verify** with `sync run` + `sync validate` — then automate via
the SAP periodic job or the daemon.

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

## The methods (all seven)

| Method | Use when | How it reads | Apply |
|--------|----------|--------------|-------|
| **WATERMARK** | the source has a monotonic change column (UTC timestamp / sequence) | `WHERE chg_col > wm` | keyed upsert (`MODE=MERGE`) |
| **INSERT_ONLY** | append-only, driven by change documents (e.g. `CDPOS`) | CDHDR feed → `CHANGENR` list → `CDPOS WHERE CHANGENR IN (…)` (2-step, portable across ECC cluster / S4 transparent) | keyed upsert (DDIC key dedups re-delivered rows) |
| **CHANGEDOC** | weak/absent change column (e.g. `MARA`, `MAKT`) | CDHDR `WHERE objectclas=… AND (udate>… OR (udate=… AND utime>…))` → business keys → **re-read** current rows from the source by key | keyed upsert |
| **SNAPSHOT** | physical deletes, or bounded column-less tables | full reload into `<target>__snap` (the normal full-load path) | server anti-join: upsert all of staging **and DELETE target keys absent from it** |
| **CDC** | physical deletes on a table too large to snapshot | database triggers on the source write a shadow log; the cycle drains it | delete-then-upsert per net operation — reached via `erpl-rev cdc`, **not** `--method CDC`: `sync create` refuses it at registration, see [`cdc.md`](cdc.md) |
| **APE_FULL** | one-off snapshot of a CDS view | SAP DHAPE engine, unique-per-scan subscription | staging → snapshot merge |
| **APE_DELTA** | ongoing replication of a CDS view incl. deletes | SAP DHAPE engine, named resumed subscription | keyed merge (`U` upsert, `D` delete by key) |

Rule of thumb: **CDS view with extraction enabled → APE_DELTA** (snapshot → APE_FULL);
**timestamp present → WATERMARK; append-only huge → INSERT_ONLY;
no/weak change column → CHANGEDOC for I/U + nightly SNAPSHOT for deletes; bounded &
column-less → just SNAPSHOT.** Physical deletes are reflected by SNAPSHOT, the
trigger-CDC tier (see [`cdc.md`](cdc.md)), or APE_DELTA — a plain change column
can never report a row that no longer exists.

No separate initial-load step exists: APE cycles seed their own target first,
SNAPSHOT reloads every cycle by definition, and the other methods start from
their watermark/position on the first cycle.

## APE path details: APE_FULL / APE_DELTA (SAP DHAPE extraction)

For **CDS views** with `@Analytics.dataExtraction.enabled: true`, erpl-rev can
extract through SAP's own DHAPE engine instead of Open SQL. Prerequisites
before registering: an APE-capable S/4 (DHAPE engine present), the extraction
annotations on the view (plus `...delta.changeDataCapture.automatic: true`
for APE_DELTA), working CDC background jobs (`S_DHCDC*` authorisations), a
C1-released view — or `--allow-unreleased` as a development/test escape hatch
(WARN-logged on the row) — and a non-`micro:*` cadence. Two methods, same
registry, same scheduler:

| Method | Use when | How it reads | Apply |
|--------|----------|--------------|-------|
| **APE_FULL** | snapshot of a CDS view | unique-per-scan subscription, ends on `lastBatch`; errors when the stream stalls 360 polls after first data | staging → snapshot merge; subscription erased afterwards |
| **APE_DELTA** | ongoing replication of a CDS view (`...delta.changeDataCapture.automatic: true` additionally required) | named subscription, resume-or-create; ends after 3 consecutive rowless polls once data has streamed (a quiet new subscription warns instead) | per-package spill → keyed merge (`U` upsert, `D` delete by key) |

Register from the CLI (not from the Delta tab):

```bash
erpl-rev sync create ape_flights \
    --method APE_DELTA --source ZERPL_APE_D --keys RID \
    --subscriber-process ERPLREV99 --chunk-size 20000 \
    --wireformat 'Required Conversions Plus Time Format and Currency' \
    --cadence hourly --allow-unreleased
```

`--subscriber-process` names the SAP-side subscription (unique-per-scan names are
minted for `APE_FULL`, the name is resumed for `APE_DELTA`); `--chunk-size`
sizes engine handovers (default 20000); `micro:*` cadences are refused.
Re-running `sync create` on an existing target is create-or-update, as with
the other methods.

Column subset (`--columns`): replicate only the named stream fields — the
target will contain exactly those columns. Key columns you omit are added
automatically. Unknown names fail at registration with a telling error. To
change the selection later, drop the target and re-register; `--columns` on
a non-APE method is refused rather than ignored.

A cycle runs lease → cleanup → seed → spill replay (crashed packages apply
automatically, a second replay changes nothing) → poll → release. The
seed-first snapshot means state converges even when a stream handover clips:
measured on A4H, one cycle window carries a commit's first two DMLs, so a
delete lands within 1–2 cycles while counts and keys stay exact. If a cycle
reports a stale batch error, the saved crash position and the incoming
package counter disagree — do not retry blindly, check for a second
server/database writing the same target.

Retire a target with the explicit drop (FR-9):

```bash
erpl-rev sync drop ape_flights
```

This erases the SAP-side subscription and deletes the state row plus its spill,
and refuses while a cycle holds a fresh lease (a cycle starting mid-drop loses
atomically: the drop is refused and the row survives). The drop reports `erase
confirmed` only when SAP raised no error and the subscription is gone on
re-lookup, otherwise `erase unverified` — cleanup stays best-effort either
way, and the local registration is removed in both cases: after an
`erase unverified`, check the subscription in SAP (`DHAPE_SUBSCR`) and erase
it there if it survives. A stale (orphaned) lease does not block the drop.
Dropping an unknown target reports instead of failing. The DuckDB table
itself is kept.

One operator note: amounts under `...Plus Currency` wire formats are
**currency-shifted by design** — compare against APE semantics, not Open-SQL
values (cell-for-cell parity excludes `AMOUNT`).

Live proof on A4H is the `ZCL_ERPL_REV_APETEST` milestones, split across
classruns — see [`testing.md`](testing.md#the-ape-path).

## Registering a target

A target is one row in `_erpl_rev_delta_state`. From the CLI:

```bash
erpl-rev sync create sales \
    --method WATERMARK --source VBAK --keys MANDT,VBELN \
    --chg-col AEDAT --wm-kind DATE --cadence hourly --log
```

`sync create` is **create-or-update**: re-running it on an existing target changes
only the fields you pass. The flags map one-to-one onto the registry:

| flag | field | |
|---|---|---|
| *(positional)* | `target` | the DuckDB table to fill |
| `--method` | `method` | `WATERMARK` \| `INSERT_ONLY` \| `CHANGEDOC` \| `SNAPSHOT` \| `CDC` \| `APE_FULL` \| `APE_DELTA` (APE needs `--subscriber-process`; see "The sixth path" above) |
| `--source` | `source_from` | the SAP table, CDS view or calc view |
| `--keys` | `keys` | the key columns, comma-separated |
| `--chg-col` | `chg_col` | the column a watermark advances on |
| `--wm-kind` | `wm_kind` | how that column is read — see the table below |
| `--wm-value` | `wm_value` | start from here instead of the beginning |
| `--cadence` | `cadence` | `hourly`, `micro:2`, `manual` |
| `--safety-secs` | `safety_secs` | overlap re-read, against late commits |
| `--log` / `--no-log` | `log_enabled` | keep a per-target change log |
| `--load-type-default` | `load_type_default` | `D`, `I`, `L` or `F` |
| `--allow-empty-reload` | `allow_empty_reload` | permit an `F` that reads nothing |
| `--subscriber-process` | `subscriber_process` | APE only: SAP-side subscription name |
| `--chunk-size` | `chunk_size` | APE only: engine handover size (default 20000) |
| `--wireformat` | `wireformat` | APE only: engine wire-format string |
| `--allow-unreleased` | `allow_unreleased` | APE only: accept unreleased CDS (WARN-logged) |
| `--columns` | `columns` | APE only: replicate this subset by name (empty = all) |

Trigger-CDC targets need one more step after this — `erpl-rev cdc provision` — see
[`cdc.md`](cdc.md).

### The same from ABAP

`zcl_erpl_rev_delta=>register( )` (or one INSERT via `Z_DUCKDB_QUERY`) is what the
CLI ends up calling, and is still the way to reach anything the flags do not cover:

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

```bash
erpl-rev sync run sales            # one cycle, now
erpl-rev sync ls                   # what is registered, and how far behind
erpl-rev sync show sales           # one target in detail
```

To have cycles run without being asked: run once manually to verify, then
choose the SAP periodic job for cadences of a minute or more, or the daemon
for sub-minute targets (APE targets use hourly or slower cadences).

### The same from ABAP

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

**`F` replaces the target; it does not merge into it.** That is the point of a
repair: the drift an operator runs `F` to fix is usually a row the source no
longer has, and an upsert can never remove one. The replacement happens inside
the commit transaction, so a reload that fails leaves the previous contents
intact rather than an emptied table.

**A reload will not empty a target by accident.** The reader creates its staging
table before it selects anything, so an empty stage is what BOTH a genuinely
empty source and a mistyped filter, a wrong client or a bad `extra` predicate
look like. The rule is therefore three-way:

| staged | target | what happens |
|---|---|---|
| rows | anything | the target is replaced by the staged rows |
| no stage at all | anything | no truncate: the read produced nothing, which is also what a short read looks like |
| empty stage | empty | truncate — nothing to lose |
| empty stage | not empty | **refused**, naming the target and its row count |

The refusal is deliberate: deleting a replica because a filter matched nothing
is not recoverable, and re-running a reload is. When the source really has been
emptied, set `allow_empty_reload` on the target and run it again:

```bash
erpl-rev sync create <target> --allow-empty-reload ...   # at registration
```

### The quiet cycle

A cycle that reads nothing still advances the watermark to its ceiling, and this
is not an optimisation: a target that does not move its floor when nothing
changed re-reads an ever-widening range, until a micro-cadence target is
scanning the whole table every tick.

Its change log is provisioned all the same, from the first cycle that reaches
the commit: a log-enabled target whose first cycle read nothing still ends with
an **empty** log rather than no log. Before that first cycle there is no log
table, and every reader treats its absence as "nothing has been logged yet"
rather than as an error.

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

The suites that cover the delta methods are in
[`testing.md`](testing.md#delta-methods).
