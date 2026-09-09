# The streaming daemon

`Z_ERPL_REV_DAEMON` is one background job that ticks every few seconds, asks the
server what is due, and runs it. It is what turns erpl-rev from "replicate on a
schedule" into second-scale replication, and it needs no new SAP interface: the
tick is an ordinary RFC call to a function module that was already there.

Latency is one tick plus one cycle. Nothing is pushed, nothing long-polls, and
no work process is held open waiting for a change.

Measured on the test system, at a two-second tick under a generated workload of
20 changes/second: **NUMTS p50 1.37 s / p95 2.37 s**, **DATETIME p50 1.70 s /
p95 3.34 s**, source commit to applied.

## Operating it

```bash
erpl-rev daemon start --tick 2 --workers 4
erpl-rev daemon status
erpl-rev daemon stop
```

`start` submits the report as a background job. `stop` sets a flag the daemon
reads at the top of every tick; it finishes the cycle in flight and then exits,
so a stop never interrupts a transaction. `status` reports the running instance,
its tick count, and how long ago it last beat.

All three go through the command queue like every other verb, so an operator
needs no `S_DEVELOP` and no generated ABAP.

## One daemon, not two

Two daemons would run every target against each other. The per-target lease
stops double *cycles*; the singleton row in `_erpl_rev_daemon` stops double
*daemons*, and a second start reports the running instance and exits.

The instance id is a UUID. It was host/user/timestamp at one-second resolution,
which meant two daemons launched in the same second — a scheduler retrying a
failed submit, two jobs released together, an operator starting twice from two
sessions — built the *same* id and both believed they had won.

A daemon that dies without releasing the row is detected by its heartbeat going
stale, at which point the next start takes over. The heartbeat is written
between cycles, not once per tick, so a tick with several slow cycles does not
look like a dead daemon.

## What runs on a tick

The server decides, in one pure function that the batch tick and the daemon both
go through. Per tick it picks the targets that are due, subject to:

| Rule | Effect |
|---|---|
| Cadence | `micro:<sec>`, `hourly`, `nightly`; `manual` is never due on its own |
| Backoff | after a failure the interval doubles, capped, so a broken target stops hammering SAP |
| Parking | after enough consecutive failures a target is parked until `sync unpark` |
| Worker budget | `max_workers` cycles per tick, most overdue first, ties broken by name |
| Full-load share | a mass load cannot take every slot and starve the micro-cadence targets |
| Trigger reservation | one slot is held for the trigger tier when it has work — its shadow rows only accumulate |
| `BLOCKED` | a target whose registration cannot run is skipped entirely |

A cycle the planner marks too large to run inline is submitted as its own
background job, so it cannot hold the tick thread and stall the heartbeat.

## When something is wrong

| Symptom | Where to look |
|---|---|
| No cycles at all | `daemon status` — is it ticking, and is `stop` set? |
| One target never runs | its `status`: `BLOCKED` means the registration cannot run; `parked_until` means backoff |
| A target retries constantly | `fail_count` and `last_error` on `_erpl_rev_delta_state` |
| Latency climbing | `erpl_rev_run_stats` for cycle duration, and whether one target dominates the budget |
| Two daemons suspected | `instance_id` on `_erpl_rev_daemon`; only one row exists, and only one id can hold it |

A target is `BLOCKED` when its registration is impossible — for example a load
type its replication method does not implement. Re-registering it clears the
block, because re-registering is the operator restating what the target should
be.

See [operations.md](operations.md) for the day-to-day runbook and
[control-tables.md](control-tables.md) for what each column means.
