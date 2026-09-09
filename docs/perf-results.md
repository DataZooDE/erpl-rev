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

### P-KEYS — **not measured: the mode does not work**

`KEYS_IUD` versus `IMAGE_IUD` on `ZWIDE_BSEG` (5 key columns, ~400 payload columns)
could not be run, because `KEYS_IUD` fails before it applies anything. The
benchmark exists (`abap/zcl_erpl_rev_perftest.abap`, the `@perf` lane) and found
three defects in a mode **no test had ever exercised**:

1. the server built `netkeys_sql` against `<log_table>__cdclog` while the executor
   staged into `<target>__cdclog` — **fixed**, the staging name now travels as
   `%STG%` like `%POS%` and `%CONF%`;
2. the net-key parser's skip-guard required a single-column key, so a composite key
   produced a tuple of empty strings and a `client = '' AND …` predicate HANA
   refuses — **fixed**;
3. the re-read of the net keys fails with `try_strptime(DATE, STRING_LITERAL)` on a
   source carrying `DATS` columns — **open**.

So the premise behind making `KEYS_IUD` the trigger tier's design centre is not
merely unmeasured; the mode is unusable on a composite-key table today. Treat the
`KEYS_IUD` / `IMAGE_IUD` choice in [`cdc.md`](cdc.md) accordingly until (3) is
closed and this section carries real numbers.

### Not yet built

`P-FULL` (parallel full load), `P-LOG` (change-log append and publish as a share of
cycle time) and `P-VAL` (validation cost) have no harness. The README's
~167k rows/s at 5 workers is an earlier full-load measurement and is **not** a
P-FULL run against this build.
