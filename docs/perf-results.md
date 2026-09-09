# Performance results

Measured numbers, dated, with the machine they came from. A number without a date
and a box is an anecdote; append, never overwrite, so a regression is visible as a
change rather than discovered as a surprise.

Run them with:

```bash
./build/erpl_rev_tests "[bench]" --success     # the local benchmarks
ERPL_REV_E2E_ONLY='perf' ./scripts/e2e.sh      # the A4H lane
```

---

## 2026-09-09 — Linux 6.18, local dev box; SAP A4H trial (single HANA, few work processes)

### P-CYCLE — what unconditional staging costs the delta cycle

Both arms get N changed rows into a 1,000,000-row target; they differ only in how.
The staged arm also advances the watermark, updates run statistics and drops its
staging table — i.e. it does the whole cycle, not just the merge.

| rows | direct (ms) | staged (ms) | staged+log (ms) | overhead |
|---:|---:|---:|---:|---:|
| 10 | 1.5 | 9.5 | 12.0 | +547% |
| 100 | 1.2 | 9.3 | 14.5 | +649% |
| 1000 | 1.5 | 7.0 | 16.7 | +377% |
| 10000 | 4.4 | 13.1 | 27.6 | +198% |
| 100000 | 32.1 | 47.8 | 119.1 | +49% |

**Read this as a fixed cost, not a rate.** Staging costs roughly 6–10 ms whatever
the row count, so the percentage is worst exactly where the daemon lives — micro
cycles of a few rows — and best where the volume is. In absolute terms a 10-row
micro cycle went from 1.5 ms to 12 ms, against a 2-second tick and an RFC round
trip: the percentage is alarming and the number is not.

What it buys is in [`delta.md`](delta.md): merge, change-log append and watermark
advance in one transaction for *every* target, so a cycle that dies is replayable
from an unmoved watermark. **This is the re-baseline. The pre-staging per-cycle
figure no longer describes the shipped build.**

### P-STAGE-PK — a primary key on the delta stage

| shape | rows | no stage PK (ms) | stage PK (ms) | of which build |
|---|---:|---:|---:|---:|
| update-only | 100000 | 50.2 | 62.1 | 14.7 |
| insert-heavy | 100000 | 99.0 | 113.3 | 15.8 |
| mixed | 100000 | 71.0 | 88.0 | 15.1 |

Building the key costs about as much as it saves. Not worth it at these shapes.

### Ingest

- **defer-PK**, 2,000,000 rows in 20 packages: per-package PK 1.257 s vs heap then
  one `ADD PRIMARY KEY` 0.381 s — **3.3x**.
- **phase split**, 50,000 rows x 420 columns (310 MiB BXML): decode 660 ms, total
  1527 ms (**32,743 rows/s**); decode is 43% of ingest.
- **query cap** 10k over 5M rows: streaming 0.011 s vs drain-all 0.381 s — 33x.

### P-KEYS — KEYS_IUD versus IMAGE_IUD

`ZWIDE_BSEG` (5 key columns, ~400 payload columns), 2,000 changes, both arms
converging the same target to what SAP holds.

| mode | write ms | cycle ms | total ms | rows applied |
|---|---:|---:|---:|---:|
| `KEYS_IUD` | **43** | 2659 | 2702 | 2000 |
| `IMAGE_IUD` | 619 | **936** | **1555** | 2000 |

**The trade-off is real and it is not a wash.** `KEYS_IUD` costs **14x less on the
source's write path** — 43 ms against 619 ms — and pays for it with a cycle roughly
2.8x more expensive, because it re-reads ~400 columns for every changed key.
`IMAGE_IUD` wins on total wall-clock by a wide margin.

Which one is right depends on whose time is being spent. The write path is a
customer's *business transaction*: a trigger writing a 400-column image sits inside
their posting. The cycle is the replicator's own time, and it is asynchronous. On a
wide, hot table `KEYS_IUD` is the right default for exactly that reason — and on a
table that is not hot, `IMAGE_IUD` moves fewer bytes in total and is simpler.

Neither is the provisioning default; that remains `DELETE_ONLY`. See
[`cdc.md`](cdc.md).

> These numbers came from a trial box with few work processes, and the arms report
> rather than assert their timings — a threshold that moves with someone else's
> background job fails for reasons nobody can act on. What the lane *asserts* is
> that both modes converge the target to what SAP holds.

**Getting here found five defects in a mode no test had ever exercised**, three of
them silent data loss. They are listed in the commit that added this lane; the
short version is that `KEYS_IUD` had never completed a single cycle on any system,
and every one of its unit tests was structurally incapable of noticing.

### Not yet built

`P-FULL` (parallel full load), `P-LOG` (change-log append and publish as a share of
cycle time) and `P-VAL` (validation cost) have no harness. The README's
~167k rows/s at 5 workers is an earlier full-load measurement and is **not** a
P-FULL run against this build.
