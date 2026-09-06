# Running erpl-rev

The day-to-day runbook. Every command here goes through the SAP command queue,
so none of them needs `S_DEVELOP` or generated ABAP, and every one of them is
recorded.

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

| Load type | Meaning | Watermark |
|---|---|---|
| `D` | delta (the default) | advances to the ceiling |
| `I` | adopt a position, transfer nothing | seeded from the source |
| `L` | initial load, then delta | advances |
| `F` | repair: replace the target | **untouched** — a repair fixes data, it does not re-seed |

`F`, `I` and `L` are **one-shot**. Set as a target's default they run once and
the target reverts to delta; they are not a schedule.

```bash
erpl-rev sync run sales --load-type F        # repair now
erpl-rev sync set-wm sales --wm-value 20260101000000   # re-deliver a window
```

`set-wm` records a run of its own. A watermark that moved with no record of who
moved it is the hardest kind of replication question to answer later.

**A reload will not empty a target by accident.** If the read produced no rows
and the target is not empty, `F` is refused rather than deleting a replica
because a filter matched nothing. When the source really has been emptied, set
`allow_empty_reload` on the target.

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

The server cuts the portions and persists them **before any worker starts**,
which is what makes a mass load restartable rather than merely parallel. ABAP
supplies only facts — the partition column's bounds and a row count — so one
code path cuts every strategy.

## Trigger targets

```bash
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
