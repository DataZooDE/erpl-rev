# Running erpl-rev

The day-to-day runbook. Anything here that makes SAP *do* something goes through the
SAP command queue, so it needs no `S_DEVELOP` and no generated ABAP, and it is
recorded. The read-only ones — `sync ls`, `sync show`, `top` — read DuckDB directly
and do not contact SAP at all.

## Registering a target

```bash
erpl-rev sync create sales \
    --method WATERMARK --source VBAK --keys MANDT,VBELN \
    --chg-col AEDAT --wm-kind DATE --cadence hourly \
    --log
```

`sync create` is **create-or-update**: re-running it on an existing target
updates it, which is what makes registration scriptable. A field you do not name
on the command line is left as it was — so changing a cadence does not silently
turn off a target's change log.

Registration writes **intent**; it never writes engine state. The watermark, the
status, the failure count and the run history belong to the engine, and
registration cannot overwrite them. See [control-tables.md](control-tables.md)
for which column is which.

## Seeding and repairing

There are four load types — `D` delta, `I` adopt a position, `L` initial load then
delta, `F` repair — and `F`, `I` and `L` are **one-shot**: set as a target's default
they run once and the target reverts to delta. What each does to the watermark, which
is the part that catches people out, is in
[`delta.md`](delta.md#load-types).

```bash
erpl-rev sync run sales --load-type F        # repair now
erpl-rev sync set-wm sales --wm-value 20260101000000   # re-deliver a window
```

`set-wm` records a run of its own. A watermark that moved with no record of who
moved it is the hardest kind of replication question to answer later.

**A reload will not empty a target by accident**: an `F` whose read produced no rows
is refused rather than deleting a replica because a filter matched nothing. Set
`allow_empty_reload` when the source really has been emptied — the reasoning is in
[`delta.md`](delta.md).

## Checking the data

```bash
erpl-rev sync preview sales --rows 20     # what a subscriber would see
erpl-rev sync validate sales              # compare against SAP, cell by cell
erpl-rev sync validate sales --full
```

`validate` compares canonical text per column, not row counts: a replica that is
the right size and the wrong content passes every count check there is. A
differing row count is itself a mismatch.

## Publishing

```bash
erpl-rev sub create warehouse --target sales --sink "PARQUET:/data/sales.parquet:FULL"
erpl-rev sub advance warehouse
erpl-rev sub ls
erpl-rev retain --target sales --window-days 7
```

A subscription's publish and its offset advance are one transaction: a failed
publish leaves the offset where it was, so nothing is skipped. Retention prunes
the change log to behind the slowest subscriber, or to the window when nothing
is subscribed.

The log appears on a target's first successful cycle. Before that there is no
log table, and every reader treats absence as "nothing logged yet".

## Mass loads

```bash
erpl-rev mass run --target hist --source BSEG --part-col BELNR \
    --split records --limit-rows 100000
```

The server cuts the portions and persists them before any worker starts, and
ABAP supplies only facts — the partition column's bounds and a row count — so
one code path cuts every strategy.

**A mass load is parallel, not resumable.** If it dies part-way, re-run it: the
target is rebuilt from scratch. The persisted portion list records what was
planned, and nothing yet reads it back to resume — so a half-finished load is
not something to recover, it is something to repeat.

## Trigger targets

```bash
erpl-rev cdc provision --target sales --mode KEYS_IUD   # create the triggers
erpl-rev cdc status --target sales
erpl-rev cdc repair --target sales
```

Status is **derived from the database catalogue**, not read from the registry. A
trigger dropped out of band — a system copy, a transport, a DBA — leaves the
registry saying `ACTIVE` while nothing is captured, and nobody finds out until
rows are missing.

`repair` recreates only the objects the probe found missing. It does not re-run
the provisioning, which would recreate the shadow table and reset the position,
discarding every change captured since.

## Watching it

```bash
erpl-rev top                      # the monitor: worst target first, refreshed every 2s
erpl-rev top --once               # one frame, for a script, a ticket or a log
erpl-rev top --once --graph --refreshes 3   # …with the throughput graph
```

Keys: `q` quit, `r` refresh, **`g` throughput graph**, `n` run the selected target now,
`u` unpark it, `↑`/`↓` select.

### What the throughput graph measures, and what it does not

`g` opens a stacked graph of rows arriving per second, so several concurrent
replications read as contributions to one total. Two things are encoded at once:

| | |
|---|---|
| **Colour** | which target — one band colour per replication, keyed to its name |
| **Glyph** | which operation — `▲` insert, `◆` update, `▼` delete |

Colour cannot also carry the operation, and the operation cannot be a colour without
giving up the one thing the graph exists for, so the operation is a shape. The same
rule holds in the target table, where the `LAST CYCLE` column prints the three counts
the last completed cycle reported, in that target's own band colour.

It is **sampled, not instrumented**. erpl-rev has no internal throughput meter — a full
load writes one statistics row, at the end, so a graph fed from those would sit flat
and then jump. Instead the monitor counts each target's rows on every refresh and
differentiates. Consequences worth knowing before you read anything into it:

- **A rate needs two samples**, so a target draws nothing on its first one. A target
  that already holds a million rows is not replicating a million rows per second, and
  the graph deliberately refuses to say so.
- **The resolution is the refresh interval**, not the engine's real granularity. A
  burst finishing between two samples is spread across the interval.
- **Colour means which target, not how high.** That is a deliberate departure from the
  tool this borrows its look from, which spends colour on magnitude — one area cannot
  encode both, and showing concurrent targets is the point here. The height gradients
  are kept for the lag meter, where magnitude is the only thing being said.
- **Nothing is truncated, and a bucket that did anything is never drawn as nothing.**
  A bulk load and the change traffic after it differ by five orders of magnitude, so on
  an axis set by the load every ordinary change rounds to zero cells and the graph reads
  as idle while replication is working. Such a bucket is rounded **up** to a single cell
  at the baseline. That cell is an indicator — "some, below the resolution of this
  scale" — not a measured height; the legend carries the figure it stands for. The floor
  is on the bar, not on the bands inside it, so a trickle beside a bulk load *in the same
  bucket* still rounds to nothing.
- **The axis is the tallest bar in the window, and comes down when that bar leaves it.**
  There is no rule deciding when to rescale: the scale is the peak of the buckets
  actually drawn, so a load holds the axis up for as long as it is on screen and the axis
  drops on its own once it scrolls off the left. Everything drawn is honestly in
  proportion to the top of the axis.
- **Within one bucket the split is net.** Inserts and deletes come from the row count,
  which is the only signal that moves *during* a load; updates come from the cycle's
  own report, because an update changes no row count and counting cannot see it at
  all. So an interval that inserted three rows and deleted one draws two inserts. The
  `LAST CYCLE` column carries the exact figures — read that when the split matters.
- **Reported inserts and deletes are deliberately ignored.** Differentiating them as
  well would draw the same rows twice: once as they arrived, and again as one
  fabricated spike in whichever bucket the cycle happened to finish in.

The sampling costs one small count per target per refresh plus one aggregate over the
run statistics, and runs only while the graph is open. That is why it is a key rather
than always on.

`--refreshes N` runs N cycles at the real cadence **inside one process** and prints the
final frame. That is what makes the graph testable: a rate needs two samples, so a loop
of separate `--once` runs can never draw a band and would pass over a broken binary.

**`LAG` is not freshness.** It is the time since that target last applied something. On
an idle target it grows, correctly — nothing has changed. For how far behind the data
actually is, compare `_commit_ts` with `_applied_at` in the change log.

## Before a release

The release gate and the full test lanes are in [`testing.md`](testing.md).

## When something is wrong

| Symptom | Likely cause | What to do |
|---|---|---|
| Target never runs | `status = BLOCKED` | the registration cannot run; fix it and re-register |
| Target runs, no rows | cadence is `manual`, or nothing changed | `sync run <target>` to force one |
| `parked_until` set | repeated failures | read `last_error`, fix, then `sync unpark` |
| Reload refused | it staged no rows against a non-empty target | check the source filter; set `allow_empty_reload` if the source really is empty |
| Subscriber sees nothing | the target has no change log | register with `--log`; the log starts at the next cycle |
| `cdc status` INCONSISTENT | a trigger is missing or invalid | `cdc repair --target T` — the position survives |
| Validation FAILED | the replica diverged | the run names the first mismatching row; repair with `--load-type F` |
| Daemon not ticking | see [daemon.md](daemon.md) | |

Everything above is visible in `erpl_rev_run_stats` and
`_erpl_rev_delta_state`; [control-tables.md](control-tables.md) documents both.
