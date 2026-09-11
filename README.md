# erpl-rev — query and replicate SAP through DuckDB

[![CI](https://github.com/DataZooDE/erpl-rev/actions/workflows/ci.yml/badge.svg)](https://github.com/DataZooDE/erpl-rev/actions/workflows/ci.yml)

**Run DuckDB SQL on SAP data, and bulk-replicate SAP into DuckDB — with nothing to install inside SAP.**

erpl-rev is a small C++ server that registers at the SAP gateway as an RFC
destination. Your ABAP calls it with `CALL FUNCTION '…' DESTINATION 'ERPL_REV'`
and **DuckDB runs behind it**. Two things, in equal measure: **query** SAP
tables, CDS views and BW calculation views with full SQL (and join them to cloud
data), and **replicate** them into DuckDB, parquet, DuckLake, Iceberg or any
DuckDB-attachable warehouse. No DuckDB extension in SAP, no DDIC structures, no
SLT/SDI/BODS — payloads travel as plain JSON over RFC.

It's the inverse of [`erpl`](https://github.com/DataZooDE/erpl) — where `erpl`
makes DuckDB call *into* SAP, **erpl-rev has SAP call out into DuckDB.**

> **Status:** research prototype, but both paths — **query** (SQL, federation,
> console, live serving) and **replication** (table / CDS / BW → DuckDB, parquet
> + attached catalogs) — are verified end-to-end against a live SAP ABAP (A4H)
> system in `scripts/e2e.sh`.

---

## Query — SQL on SAP data, from ABAP and beyond

- **Run DuckDB SQL from ABAP.** Send a query, get typed rows back — or an
  interactive **ALV grid** via the in-GUI SQL console (`Z_ERPL_REV_SQL`, `SE38`).
- **Do what Open SQL can't.** Joins across tables, window functions, aggregations
  and parquet scans over live SAP data, without staging anything.
- **Federate in one statement.** Join a SAP slice against a cloud **parquet /
  Iceberg / DuckLake** dataset or an attached **Postgres / BigQuery** table, and
  hand the result back to ABAP — cross-system queries SAP can't do alone.
- **Query live, over the network.** With `--quack`, remote DuckDB clients
  (notebooks, BI tools) query the same in-process data directly — no export step.
- **Big results, fixed memory.** A streamed `OPEN`/`FETCH`/`CLOSE` cursor decodes
  results page-by-page, so result size isn't bounded by ABAP memory.

## Replicate — bulk-load SAP into DuckDB & the lakehouse

- **Any SAP source.** Replicate **tables, CDS views (incl. `WITH PARAMETERS`) and
  BW / HANA calculation views** into typed DuckDB — full or filtered, with
  source-side `WHERE`, column selection and idempotent `UPSERT`. Built for
  **>100M-row** loads.
- **Delta (incremental) too.** Keep a target in sync loading only what changed —
  **watermark, insert-only, change-document (CDHDR/CDPOS), snapshot-diff and
  trigger-CDC (both catch deletes)** methods, all merging server-side, idempotent
  and re-runnable. Customer-owned Open SQL only
  (no ODP / SAPI / `RFC_READ_TABLE`). See [`docs/delta.md`](docs/delta.md).
- **Real time, when you want it.** A daemon at a two-second cadence, and a
  trigger tier that catches physical deletes a watermark cannot see. Three
  minutes, unedited: a million goods movements synced in ten seconds, then a
  material document posted, transferred and archived in SAP — each change in
  DuckDB a second or two later — and finally thirty seconds of continuous mixed
  traffic at a few dozen rows a second:

![A million goods movements sync in ten seconds, then a material document is posted, transferred between storage locations and archived in SAP, and finally thirty seconds of continuous mixed traffic; each change appears in DuckDB a second or two later while erpl-rev top graphs the throughput, one glyph per operation](demo/realtime.gif)

  The right-hand pane is `erpl-rev top`, started once and never touched again:
  **colour is the target, the glyph is the operation** — `▲` insert, `◆` update,
  `▼` delete — and the `LAST CYCLE` column carries the exact per-cycle split.
  The clip ends by measuring itself: SAP's change time against the apply time,
  per row, from two independent clocks.

  See [`docs/demo.md`](docs/demo.md) for what it proves and what it does not.
  Re-render it with `bash demo/setup.sh && vhs demo/realtime.tape`, or package it
  as a single shareable HTML file with `bash demo/package-html.sh`.
- **Land in the open lakehouse.** parquet / partitioned datasets, **DuckLake** or
  **Iceberg**, on local disk or **cloud object storage** (S3 / GCS / Azure).
- **Publish into a warehouse.** `ATTACH` **Postgres / MySQL / BigQuery / MotherDuck**
  and push a SAP slice in with one SQL statement — see
  [`docs/publishing.md`](docs/publishing.md).
- **Fast & parallel** — a live 10M-row run (50-of-400-column BSEG-shaped table,
  BELNR-partitioned, on the A4H trial / loopback):

![Parallel replication throughput: aggregate rises to ~167k rows/s at 5 workers (10M rows in 60s) while per-worker throughput tapers from 47k to 33k rows/s](docs/perf-scaling.png)

| Workers | Wall time | Aggregate | Per worker |
|:-------:|----------:|----------:|-----------:|
| 2 | 106 s | ~94,000 rows/s  | ~47,000 rows/s |
| 4 |  65 s | ~154,000 rows/s | ~38,000 rows/s |
| 5 |  60 s | ~167,000 rows/s | ~33,000 rows/s |

**Peak ~167k rows/s — 10M rows in a minute.** Each worker bulk-loads a disjoint
key range with a DuckDB `Appender` (~230× a naive per-row path); memory is bounded
by batch size, and loads are restartable and idempotent
([`test/bench_ingest.cpp`](test/bench_ingest.cpp)).

## Why it fits the SAP data stack

- **No SLT, no SDI, no Data Services, no add-on** — a transport (package `ZERPL_CORE`)
  plus a registered RFC server. No core modification, no HANA license, no BTP.
  Runs against any NetWeaver ABAP stack (ECC, S/4HANA, BW/4HANA).
- **Reads what you model** — tables, CDS views (keys auto-detected), BW/HANA calc
  views (`"_SYS_BIC"."pkg/CV"`) — semantics intact, not raw dumps.
- **SLT semantics you know (LTRS)** — field selection, source-side filter, key
  `UPSERT` — without standing up SLT.
- **DDIC-typed & provably faithful** — NUMC / DATS / CURR / DECIMAL / RAW map to
  real DuckDB types; a built-in diff harness checks the target against the source
  **cell-by-cell** (incl. a 400-column BSEG-shaped table).

**Security & authorizations.** Only `Z*` objects (no core mod), reached via a
type-T destination locked down with a gateway `reginfo` allow-list and (off-box)
SNC. FM calls run as a comm user scoped to `S_RFC` for function group `ZERPL_REV`
only; the reports run under the end user's auth (CDS DCL is enforced, raw-table
`SELECT` is **not** implicitly `S_TABU`-checked — gate program execution; the
native/ADBC BW path reads cross-client). Full guide: [`docs/security.md`](docs/security.md).

---

## Getting started

Five steps. The third one is a loop, and it is the only part that surprises people.

### 1. See it run — no SAP needed

`uvx` runs a published tool without installing it, and comes with
[uv](https://docs.astral.sh/uv/). erpl-rev's CLI shells out to it, so install uv
first — or `pip install erpl-rev` and drop the `uvx` prefix from everything below.

```bash
uvx erpl-rev --smoke
```

Loads the RFC stack and DuckDB and prints their versions. If that works, the binary
is fine and everything from here is about SAP. (`pip install erpl-rev` if you would
rather have it on `PATH`; a container image and standalone bundles are in
[Install options](#install-options) below.)

### 2. Collect what you need

erpl-rev makes **two different connections**, and mixing them up is the commonest
early confusion:

```
  your laptop ──── ADT, HTTP :50000 ────▶  SAP   (the CLI: doctor, setup, sync, replicate)
  the server  ◀─── RFC, gateway :3300 ──▶  SAP   (registers OUTBOUND; SAP never dials in)
  your laptop ──── quack, loopback ─────▶  the server   (sql, sync ls)
```

For the **CLI**, an ABAP Development Tools login:

| | flag | env | default |
|---|---|---|---|
| host | `--sap-host` | `SAP_HOST` | `localhost` |
| HTTP port | `--sap-port` | `SAP_PORT` | `50000` |
| client | `--sap-client` | `SAP_CLIENT` | `001` |
| user | `--sap-user` | `SAP_USER` | prompted |
| password | — | `SAP_PASSWORD` | prompted |

ADT must be reachable (`/sap/bc/adt`, ICF active). `uvx`/`uv` must be installed —
the CLI shells out to it.

For the **server**, the gateway: `--gwhost` and `--gwserv` (`sapgw<NN>`, port
`33<NN>`). Terms in that paragraph you do not recognise are in the
[glossary](docs/glossary.md).

And from your Basis team, eventually: a `reginfo` line allowing the program ID, an
RFC user, and — on a development system — `S_DEVELOP` for the account that runs
`setup`. Step 3 generates the exact request.

### 3. Deploy the ABAP, and get the gateway to accept you

This is a **loop**, not a sequence. `setup` finishes by making ABAP call back out
through the server, which cannot happen until the gateway lets the server register,
and the `reginfo` line that allows it is written by `setup`. So you go round once:

```bash
erpl-rev doctor                 # read-only: what is missing, and the fix for each
erpl-rev setup --dry-run        # the exact change set, nothing written
erpl-rev setup                  # deploy, then try the round trip
```

`setup` will deploy the ABAP and then say:

```
Deployed, but the round trip did NOT complete yet. That is expected if the
server is not running, or if the gateway has not been told to accept the
registration.
```

**That is not a failure.** It also writes `erpl-rev-basis-handout.md`. Give that to
Basis — it contains the least-privilege `reginfo` line already filled in.

> **If the server will run on a different machine from the one you just typed on,
> pass `--server-host`.** The `reginfo` line names the host allowed to register, and
> it defaults to *this* machine. Get it wrong and Basis allows your laptop while the
> real server is refused:
>
> ```bash
> erpl-rev setup --print-runbook --server-host sapbridge01.corp
> ```
(`erpl-rev setup --print-runbook` prints the same handout **without deploying
anything**, which is what you want on a system where you will never have
`S_DEVELOP`; there, the ABAP arrives by transport instead. The binary builds that
transport too — `setup --package ZERPL_CORE --transport <request>` on your own DEV
system — and `erpl-rev abap export <dir>` writes the fourteen sources out as files.
See [`INSTALL.md`](docs/INSTALL.md).)

Then start the server and go round again:

Leave the server running in one terminal:

```bash
erpl-rev --gwhost <gateway-host> --gwserv sapgw<NN>
```

**Watch for `to="running"`, not for "listening".**

```
INFO [server] registration state from="starting" to="running"     <- the gateway accepted
INFO [server] listening (Ctrl-C to stop) program_id="ERPL_REV" ...
```

"listening" is printed as soon as the process starts serving, whether or not the
gateway let it register; the state line is the one that tells you. If it says
`broken`, the `reginfo` does not allow this host and program ID — that is the loop
above not closed yet.

Then, in another terminal:

```bash
erpl-rev doctor                 # the round trip should now pass
```

`doctor` and `setup` take `--gwhost`/`--gwserv` too, and they are *not* the server's
defaults: `setup` assumes the gateway is on the SAP host. Pass them explicitly if your
gateway is somewhere else.

On a throwaway trial with `gw/acl_mode = 0` there is no ACL to satisfy and the two
halves can happen in either order — see the [A4H appendix](docs/enable-rfc-registration.md).

### 4. Get your first table out

```bash
erpl-rev replicate --table MARA --target mara     # full load, as a background job
erpl-rev sql "SELECT count(*) FROM mara"          # read it back
erpl-rev sql "SELECT * FROM mara LIMIT 10"
```

`sql` reads the live database through the server, so it works while the server is
running. Stop the server and the DuckDB file is an ordinary DuckDB file — open it
with anything.

### 5. Keep it in sync

A full load is a snapshot. To keep a target current, register how it should find
what changed:

```bash
erpl-rev sync create mara --method WATERMARK --source MARA \
    --keys MANDT,MATNR --chg-col AEDAT --wm-kind DATE --cadence hourly --log
erpl-rev sync run mara            # one cycle now
erpl-rev sync ls                  # what is registered, and how far behind
```

> `wm-kind DATE` reads **whole days, and never today** — so once the backfill is done,
> a run made the same day returns nothing. That is the complete-day rule doing its job,
> not a fault: a day still being written to cannot be safely marked as read. Sources
> with a timestamp column use `NUMTS` and do not wait. The table of kinds is in
> [`delta.md`](docs/delta.md).

Which method to choose, and how to catch **physical deletes** a change column cannot
see, is [`docs/delta.md`](docs/delta.md) and [`docs/cdc.md`](docs/cdc.md). To have it
run continuously rather than on demand, [`docs/daemon.md`](docs/daemon.md); to watch
it, `erpl-rev top` and [`docs/operations.md`](docs/operations.md).

## Install options

| | | |
|---|---|---|
| **PyPI** | `uvx erpl-rev` / `pip install erpl-rev` | wheels for Linux x86-64, macOS arm64, Windows x64 |
| **Bundle** | a single file per OS, from [Releases](https://github.com/DataZooDE/erpl-rev/releases) | verify with `SHA256SUMS.txt` |
| **Docker** | `ghcr.io/datazoode/erpl-rev:latest` | see [`docs/docker.md`](docs/docker.md) |

All of them carry **DuckDB and nothing else**: since `v2026.08.30` the RFC protocol
is [`erpl-proto`](https://erpl.io/blog/sap-rfc-protocol-byte-by-byte), our pure-Rust
implementation, linked statically — no SAP NW RFC SDK, no ICU, no `LD_LIBRARY_PATH`.
Building from source is different and is [`docs/building.md`](docs/building.md).

Everything the SAP GUI reports do is also a CLI command, and `--print-abap` on any of
them shows the ABAP instead of running it. Nothing writes to SAP or DuckDB without a
confirmation or an explicit `--yes`.

## Feedback

If `erpl-rev` misbehaves, please [open an issue](https://github.com/DataZooDE/erpl-rev/issues).
This is a registered RFC server talking to gateways and SAP releases we cannot reproduce
here, so a report with your setup is the fastest path to a fix.

If it saved you time, a star on the repo helps others find it.

Because this process usually runs under a service manager rather than on a terminal, the
feedback pointer is emitted as an INFO log line at startup — where operators actually read
it. On an interactive start you also get a small banner, at most once a day; silence both
with `DATAZOO_NO_BANNER=1`.

## Telemetry

The server sends **anonymous** usage telemetry to help us understand which bridge
operations are used and where they break. Three events: `server_started` once at
boot, **`rfc_call` on every bridge function-module invocation** (which module,
success or failure, how long — sampled), and `$exception` when one fails.

What never leaves the machine: **SAP data, SQL text, table or field names,
target names, connection strings and error messages.** Only bounded
enumerations and numbers.

It is **on by default** and disabled by any one of `--no-telemetry`,
`ERPL_REV_NO_TELEMETRY`, or `DATAZOO_DISABLE_TELEMETRY`;
`ERPL_REV_TELEMETRY_SAMPLE_RATE` thins `rfc_call` without silencing it.
Air-gapped hosts drop the request silently with zero impact.
Full detail, event by event and property by property:
[`TELEMETRY.md`](TELEMETRY.md).

## Docs

**Getting it working**

- [`docs/glossary.md`](docs/glossary.md) — the SAP words, if you do not use them daily
- [`docs/INSTALL.md`](docs/INSTALL.md) — the transport path, for a system where you will never have `S_DEVELOP`
- [`docs/security.md`](docs/security.md) — **what your Basis team will ask**: the trust boundary, `reginfo`, the RFC user, SNC
- [`docs/docker.md`](docs/docker.md) — running the container image
- [`docs/tunnel.md`](docs/tunnel.md) — only if this host has no route to the gateway

**Keeping data in sync**

- [`docs/delta.md`](docs/delta.md) — the five incremental methods, and how to choose
- [`docs/cdc.md`](docs/cdc.md) — the trigger tier, for physical deletes
- [`docs/daemon.md`](docs/daemon.md) — continuous replication in one background job
- [`docs/operations.md`](docs/operations.md) — the operator's runbook and the monitor
- [`docs/upgrading.md`](docs/upgrading.md) — **what changes for a system already replicating**

**Reading and publishing the data**

- [`docs/sql-console.md`](docs/sql-console.md) — the DuckDB SQL console inside SAP GUI
- [`docs/publishing.md`](docs/publishing.md) — parquet, DuckLake, Iceberg, or an attached warehouse
- [`docs/control-tables.md`](docs/control-tables.md) — the control schema as a versioned interface, and what each run records

**How it behaves, and how fast**

- [`docs/demo.md`](docs/demo.md) — the recorded session: what it proves and what it does not
- [`docs/perf-results.md`](docs/perf-results.md) — measured numbers, dated, with the box they came from
- [`TELEMETRY.md`](TELEMETRY.md) — every event and property that leaves the machine, and the opt-outs

**Working on erpl-rev**

- [`docs/building.md`](docs/building.md) — building from source
- [`docs/testing.md`](docs/testing.md) — the test lanes and the release gate
- [`docs/enable-rfc-registration.md`](docs/enable-rfc-registration.md) — appendix: RFC registration on an A4H trial

## License

[Business Source License 1.1](LICENSE) (BSL), Licensor **DataZoo GmbH**, Change
License MPL 2.0 — same terms as [`erpl`](https://github.com/DataZooDE/erpl).
