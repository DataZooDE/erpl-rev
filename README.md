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
  **watermark, change-document (CDHDR/CDPOS), and snapshot-diff (deletes)** methods,
  all merging server-side, idempotent and re-runnable. Customer-owned Open SQL only
  (no ODP / SAPI / `RFC_READ_TABLE`). See [`docs/delta.md`](docs/delta.md).
- **Land in the open lakehouse.** parquet / partitioned datasets, **DuckLake** or
  **Iceberg**, on local disk or **cloud object storage** (S3 / GCS / Azure).
- **Publish into a warehouse.** `ATTACH` **Postgres / MySQL / BigQuery / MotherDuck**
  and push a SAP slice in with one SQL statement — see
  [Push to MotherDuck](#push-to-motherduck-duckdbs-cloud) below.
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

### Push to MotherDuck (DuckDB's cloud)

[MotherDuck](https://motherduck.com) is just another DuckDB-attachable catalog, so
the embedded engine reaches it exactly like Postgres / BigQuery / Iceberg — only the
`ATTACH` and credentials differ. Point the server's boot init at MotherDuck once,
then replicate or publish SAP slices straight into the cloud.

**1. Boot the server attached to MotherDuck.** Supply the token via the
`motherduck_token` env var (or a `CREATE SECRET` in an `--init-file`) — never commit it:
```bash
export motherduck_token='<your-md-token>'
ERPL_REV_GWHOST=<gw> ERPL_REV_GWSERV=sapgw00 \
  ./build/erpl_rev_server --db erpl-rev.duckdb \
  --init-sql "INSTALL motherduck; LOAD motherduck; ATTACH 'md:';"
```
Your MotherDuck databases now appear as catalogs (e.g. `my_db.main.<table>`).

**2. Push a SAP slice from ABAP** — stage locally, then publish to the cloud:
```abap
zcl_erpl_rev_util=>replicate( iv_tab = 'MARA' iv_target = 'mara' ).
zcl_erpl_rev_util=>publish(                       " FULL = overwrite, APPEND = insert
  iv_source = 'mara' iv_kind = 'TABLE'
  iv_dest   = 'my_db.main.mara' iv_mode = 'FULL' ).
```
The *publish* field of `Z_ERPL_REV_REPLICATE` does the same from the GUI.

**3. Query MotherDuck from the SQL console.** `Z_ERPL_REV_SQL` ships an **example
dropdown** with ready-to-run queries: the classic NYC-taxi public-Parquet aggregate,
MotherDuck's shared `sample_data` (taxi + Hacker News), a `SUMMARIZE`, and a
push-a-table round-trip — pick one and hit *Execute*.

> The released bundle ships DuckDB with `parquet` / `json` built in; `motherduck`
> (and `httpfs`) auto-install from `extensions.duckdb.org` on first use, so the host
> needs outbound HTTPS — or pre-stage the extension for air-gapped systems.

---

## Why it fits the SAP data stack

- **No SLT, no SDI, no Data Services, no add-on** — a transport (package `ZERPL`)
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

## Install & setup

**Just want to run it?** This is the recommended way — grab the **single
self-extracting binary** for your OS from the
[latest release](https://github.com/DataZooDE/erpl-rev/releases)
(`erpl-rev-linux-amd64`, `erpl-rev-macos-arm64`, `erpl-rev-windows-amd64.exe`) and
run it. It bundles the SAP NW RFC SDK + ICU + DuckDB, self-extracts on first
launch, and needs **no `LD_LIBRARY_PATH`** and nothing else installed. Or use
**Docker** — `docker pull ghcr.io/datazoode/erpl-rev:latest` — the image bakes the
**same** bundle (see [Run with Docker](#run-with-docker)). Either way you still do
the one-time SAP-side wiring (**step 3**) and then run it (**step 4**).

> The numbered steps below **build from source** — only needed to develop erpl-rev
> or to produce the bundle yourself (`make bundle` → `dist/erpl-rev`).

### Prerequisites
- Linux host with **CMake ≥ 3.16**, a **C++17** compiler, **Ninja**, and **vcpkg**
  (supplies Catch2 for the tests).
- The proprietary **SAP NW RFC SDK** (not redistributed — see below).
- A reachable SAP **gateway** (any NetWeaver ABAP; a local A4H docker trial works).

### 1. Provide the SDK + DuckDB
The NW RFC SDK lives in a repo-local, gitignored `nwrfcsdk/linux/` (same convention
as `erpl`). Download it from the SAP Software Center, or copy it from an `erpl`
checkout. DuckDB is fetched as an official prebuilt:
```bash
cp -a /path/to/nwrfcsdk ./nwrfcsdk     # provides nwrfcsdk/linux/{include,lib}
make duckdb-dist                       # fetch prebuilt libduckdb 1.5.4 into vendor/
```

### 2. Build & test
```bash
make build      # -> build/erpl_rev_server + build/erpl_rev_tests
make test       # the Catch2 suite against real DuckDB (no mocks)
```
`make build` also initialises the `third_party/posthog-telemetry` submodule, so a
fresh clone needs no extra `git submodule` step.

### 3. Wire up the SAP side (one-time)
Production = import the ABAP transport and run the setup classrun — full guide in
[`docs/INSTALL.md`](docs/INSTALL.md). You need three things in the SAP system:
- a **type-T `ERPL_REV` destination** in registration mode (`method='R'`) — created by `ZCL_ERPL_REV_SETUP`;
- the **`ZERPL_REV` function group + FMs** (`Z_DUCKDB_*`) — created by `ZCL_ERPL_REV_MKFM`;
- gateway registration allowed for the server's host — [`docs/enable-rfc-registration.md`](docs/enable-rfc-registration.md).

### 4. Run the server
Running the **downloaded release binary** (or the Docker image) needs no setup —
just `./erpl-rev-linux-amd64` with the `ERPL_REV_*` env below; the bundle
self-extracts and sets its own loader path. The `LD_LIBRARY_PATH` line is **only**
for the from-source `build/erpl_rev_server`, whose libs live elsewhere in the tree:
```bash
export LD_LIBRARY_PATH=$PWD/nwrfcsdk/linux/lib:$PWD/vendor/duckdb-1.5.4
ERPL_REV_GWHOST=<gateway-host> ERPL_REV_GWSERV=sapgw00 \
ERPL_REV_DB_PATH=erpl-rev.duckdb \
  ./build/erpl_rev_server            # add --quack for the network server
# convenience: `make run` (quack on), `make run-mem` (in-memory), or `make run-no-quack`
```
Easiest is **[`scripts/run-rfc-server.sh`](scripts/run-rfc-server.sh)**: it sets
`LD_LIBRARY_PATH`, registers as `ERPL_REV`, and — opt-in via the environment —
attaches **MotherDuck** (`motherduck_token`) and/or **BigQuery**
(`ERPL_REV_BQ_PROJECT`). Pass `-r` to restart.

To publish to **external / cloud catalogs** (parquet, postgres, ducklake,
bigquery, motherduck), give DuckDB boot SQL that runs `INSTALL`/`LOAD`/`ATTACH`
(and `CREATE SECRET`) once on a global connection — via `--init-sql "<sql>"`,
`--init-file <path>`, or the `ERPL_REV_DUCKDB_INIT` env var.

For production, run it as a **systemd service** ([`deploy/erpl-rev.service`](deploy/erpl-rev.service))
or via **Docker** (image below).

#### Run with Docker

Prebuilt `linux/amd64` images are published to GitHub Container Registry:

```bash
docker run -d --name erpl-rev \
  -e ERPL_REV_GWHOST=<gateway-host> -e ERPL_REV_GWSERV=sapgw00 \
  -e ERPL_REV_PROGRAM_ID=ERPL_REV \
  -v erpl-data:/data \
  ghcr.io/datazoode/erpl-rev:latest
# add `--quack` (and `-p 9494:9494`) for the DuckDB network server
```

Config is entirely via `ERPL_REV_*` env vars; the DuckDB file lives on the
`/data` volume. RFC registration is **outbound** to the gateway, so no inbound
port is needed — the gateway's `reginfo` ACL must allow `ERPL_REV_PROGRAM_ID`
from the container's host. Add `--quack` and publish `-p 9494:9494` for the
network server; `docker run --rm ghcr.io/datazoode/erpl-rev:latest --smoke`
checks a pulled image loads with no gateway. See [`docs/docker.md`](docs/docker.md).

### 5. Smoke test
- `./build/erpl_rev_server --smoke` (or the bundled binary) — loads the SAP NW RFC
  SDK + DuckDB and prints their versions; needs no gateway.
- `Z_ERPL_REV_SQL` (`SE38`) → run `SELECT 42` to confirm the ABAP → server → DuckDB
  round-trip (server must be running and registered).
- Run `Z_ERPL_REV_REPLICATE` (`SE38`) on a small table and check row parity.

---

## Quick start

**Query SAP data with SQL** — `Z_ERPL_REV_SQL` (`SE38`) opens a DuckDB SQL console
in the SAP GUI: type any query (over replicated SAP data, cloud parquet, or
attached catalogs) and get an ALV grid back; or call the query FM from ABAP and
receive typed rows. From an external DuckDB client (with `--quack`):
```sql
ATTACH 'quack:host:9494' AS r (TOKEN '<token>');
SELECT * FROM r.<table>;            -- query the live in-process data
```

**Replicate a table** — `Z_ERPL_REV_REPLICATE` (`SE38`): pick the source (F4 to
search the DDIC), optionally pick columns (F4) and a `WHERE`, choose a target; keys
are auto-kept so re-runs dedup. For >100M rows, tick *parallel* and run in
background. Mirrors SLT's `LTRS` knobs — details below.

<details>
<summary><b>Replicating SAP tables — the SLT-style detail</b></summary>

`Z_ERPL_REV_REPLICATE` maps to the three per-table controls of SAP SLT (`LTRS`):

| SLT concept | Parameter | Behaviour |
|---|---|---|
| Table selection | `p_tab` | source SAP table / CDS view (F4 search). |
| Field selection | `p_cols` | columns to replicate (blank = all); keys always kept. |
| Filter (at source) | `p_where` | OpenSQL `WHERE`, applied in the SAP `SELECT` so non-matching rows never transfer. |
| target / init / mode | `p_target` `p_init` `p_mode` `p_maxrow` `p_verify` | DuckDB table name; pre-SQL; `UPSERT`/`INSERT`; row cap; count-parity check. |

Reads are **package-wise** (keyset pagination, 50k/batch) so memory is bounded;
full-load-replace makes a crashed run safely re-runnable. The data-identity test
(`zcl_erpl_rev_difftest`) compares target vs source cell-by-cell (SFLIGHT,
ZWIDE_BSEG, REPOSRC + a negative control), **byte for byte including trailing
zeros** — `RAW` columns replicate faithfully.
</details>

---

## How it works

```
ABAP ──CALL FUNCTION 'Z_DUCKDB_QUERY'/'Z_DUCKDB_INGEST' DESTINATION 'ERPL_REV'──►
   SAP gateway (registered-server routing, RFCOPTIONS H=RFCSERVER)
      └──► erpl_rev_server (C++) ──► DuckDbBridge ──► DuckDB (parquet / lakehouse)
```

A registered RFC server (`RfcCreateServer`/`RfcLaunchServer`) hosts a handful of
function modules whose payloads are **JSON / binary-sXML over scalar `STRING`
params** — schema-generic, so no custom DDIC structures. It links the official
prebuilt **DuckDB 1.5.4** (`libduckdb.so`, parquet+json+quack built in); our code
plus libstdc++/libgcc are static, leaving only `libduckdb.so` and the SAP `.so`
trio dynamic.

<details>
<summary><b>Configuration (env vars & flags)</b></summary>

12-factor: config from the environment, logs to stderr, graceful `SIGINT`/`SIGTERM`.
CLI flags override env (**flag > env > default**); `--help` prints the full surface.

| Concern | Flag | Env var | Default |
|---|---|---|---|
| Gateway PROGRAM_ID | — | `ERPL_REV_PROGRAM_ID` | `ERPL_REV` |
| Gateway host / service | — | `ERPL_REV_GWHOST` / `ERPL_REV_GWSERV` | `localhost` / `3300` |
| Parallel registrations | — | `ERPL_REV_REG_COUNT` | `5` |
| Enable quack | `--quack[=<listen>]` | `ERPL_REV_QUACK` | off |
| Quack bind / token | `--quack-listen` / `--quack-token` | `ERPL_REV_QUACK_LISTEN` / `ERPL_REV_QUACK_TOKEN` | `quack:localhost` (port 9494) / random |
| DuckDB file | `--db <path>` | `ERPL_REV_DB_PATH` | `erpl-rev.duckdb` (`:memory:` for in-mem) |
| Boot init SQL | `--init-sql` / `--init-file` | `ERPL_REV_DUCKDB_INIT` | — (ATTACH/secrets for external/cloud targets) |
| Telemetry opt-out | `--no-telemetry` | `ERPL_REV_NO_TELEMETRY` / `DATAZOO_DISABLE_TELEMETRY` | on by default ([docs](docs/telemetry.md)) |
| Self-check & exit | `--smoke` | — | — |
| Logging | — | `ERPL_REV_LOG_{LEVEL,FORMAT,COLOR}` | `info` / `console` / `auto` |

A file-backed `--db` makes ingested (and quack-served) data durable across
restarts. The quack token is a bearer credential — pin a high-entropy value via
`--quack-token` (it's redacted from the log) and keep the listener on loopback
unless you intend remote access.
</details>

<details>
<summary><b>Build internals & troubleshooting</b></summary>

- The build resolves the SDK from `nwrfcsdk/linux` (override `-DSAPNWRFC_HOME=…` /
  `make build NWRFC_HOME=…`); Catch2 via **vcpkg** manifest mode (`VCPKG_ROOT`).
- CI builds the server + runs tests on every push; it pulls the SDK from S3 via the
  same GitHub-OIDC→AWS role as `erpl` (`scripts/download_and_extract_nwrfc.sh`).
- **Registered destination must be `method='R'`** (`H=RFCSERVER`) — "start" mode
  makes the gateway try to launch an executable and the call never reaches us.
- **The FM interface must exist in the backend** or ABAP marshalling returns
  `SYSTEM_FAILURE` — `ZCL_ERPL_REV_MKFM` creates them.
- **Run with `LD_LIBRARY_PATH=$NWRFC_HOME/lib`** — `libsapnwrfc.so` `dlopen`s ICU by
  name, so rpath alone is insufficient.
</details>

---

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

The server sends **anonymous** usage telemetry (`application_start` /
`application_stop` with app/version/platform/DuckDB-version only — **no SAP
data, query text, or table/field names**) to help us understand adoption. It is
**on by default** and disabled by any one of `--no-telemetry`,
`ERPL_REV_NO_TELEMETRY`, or `DATAZOO_DISABLE_TELEMETRY`. Air-gapped SAP hosts
drop the request silently with zero impact. Details: [`docs/telemetry.md`](docs/telemetry.md).

## Docs

- [`docs/delta.md`](docs/delta.md) — incremental extraction (watermark / change-doc / snapshot)
- [`docs/INSTALL.md`](docs/INSTALL.md) — SAP transport import + server install + upgrade/uninstall
- [`docs/enable-rfc-registration.md`](docs/enable-rfc-registration.md) — gateway registration / `reginfo`
- [`docs/security.md`](docs/security.md) — Basis hardening, RFC user, SNC, ACLs
- [`docs/sql-console.md`](docs/sql-console.md) — the in-GUI DuckDB SQL console
- [`docs/telemetry.md`](docs/telemetry.md) — what's collected, where, and the three opt-outs
- [`docs/docker.md`](docs/docker.md) — running the container image from ghcr.io

## License

[Business Source License 1.1](LICENSE) (BSL), Licensor **DataZoo GmbH**, Change
License MPL 2.0 — same terms as [`erpl`](https://github.com/DataZooDE/erpl).
