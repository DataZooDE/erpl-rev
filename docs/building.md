# Building erpl-rev from source

**You almost certainly do not need this.** Released bundles and the PyPI package carry
everything; see the [README](../README.md) to install and run one. Build from source
only to develop erpl-rev itself.

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
make duckdb-dist                       # fetch prebuilt libduckdb 1.5.5 into vendor/
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
[`docs/INSTALL.md`](INSTALL.md). You need three things in the SAP system:
- a **type-T `ERPL_REV` destination** in registration mode (`method='R'`) — created by `ZCL_ERPL_REV_SETUP`;
- the **`ZERPL_REV` function group + FMs** (`Z_DUCKDB_*`) — created by `ZCL_ERPL_REV_MKFM`;
- gateway registration allowed for the server's host — [`docs/enable-rfc-registration.md`](enable-rfc-registration.md).

### 4. Run the server
Running the **downloaded release binary** (or the Docker image) needs no setup —
just `./erpl-rev-linux-amd64` with the `ERPL_REV_*` env below; the bundle
self-extracts and sets its own loader path. The `LD_LIBRARY_PATH` line is **only**
for the from-source `build/erpl_rev_server`, whose libs live elsewhere in the tree:
```bash
export LD_LIBRARY_PATH=$PWD/nwrfcsdk/linux/lib:$PWD/vendor/duckdb-1.5.5
ERPL_REV_GWHOST=<gateway-host> ERPL_REV_GWSERV=sapgw00 \
ERPL_REV_DB_PATH=erpl-rev.duckdb \
  ./build/erpl_rev_server            # add --quack for the network server
# convenience: `make run` (quack on), `make run-mem` (in-memory), or `make run-no-quack`
```
Easiest is **[`scripts/run-rfc-server.sh`](../scripts/run-rfc-server.sh)**: it sets
`LD_LIBRARY_PATH`, registers as `ERPL_REV`, and — opt-in via the environment —
attaches **MotherDuck** (`motherduck_token`) and/or **BigQuery**
(`ERPL_REV_BQ_PROJECT`). Pass `-r` to restart.

To publish to **external / cloud catalogs** (parquet, postgres, ducklake,
bigquery, motherduck), give DuckDB boot SQL that runs `INSTALL`/`LOAD`/`ATTACH`
(and `CREATE SECRET`) once on a global connection — via `--init-sql "<sql>"`,
`--init-file <path>`, or the `ERPL_REV_DUCKDB_INIT` env var.

For production, run it as a **systemd service** ([`deploy/erpl-rev.service`](../deploy/erpl-rev.service))
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
checks a pulled image loads with no gateway. See [`docs/docker.md`](docker.md).

### 5. Smoke test
- `./build/erpl_rev_server --smoke` (or the bundled binary) — loads the SAP NW RFC
  SDK + DuckDB and prints their versions; needs no gateway.
- `Z_ERPL_REV_SQL` (`SE38`) → run `SELECT 42` to confirm the ABAP → server → DuckDB
  round-trip (server must be running and registered).
- Run `Z_ERPL_REV_REPLICATE` (`SE38`) on a small table and check row parity.

---


## How it works, inside


```
ABAP ──CALL FUNCTION 'Z_DUCKDB_QUERY'/'Z_DUCKDB_INGEST' DESTINATION 'ERPL_REV'──►
   SAP gateway (registered-server routing, RFCOPTIONS H=RFCSERVER)
      └──► erpl_rev_server (C++) ──► DuckDbBridge ──► DuckDB (parquet / lakehouse)
```

A registered RFC server (`RfcCreateServer`/`RfcLaunchServer`) hosts a handful of
function modules whose payloads are **JSON / binary-sXML over scalar `STRING`
params** — schema-generic, so no custom DDIC structures.

**DuckDB 1.5.5** (parquet + json + quack built in) is linked **statically**, as are
libstdc++/libgcc and — in the released bundles — the `erpl-proto` RFC implementation.
That is what makes a bundle a single file with nothing beside it. A from-source build
differs: `make` defaults to `RFC_BACKEND=sdk` and then does need the SAP NW RFC SDK at
runtime; `make build RFC_BACKEND=proto` reproduces what ships.

<details>
<summary><b>Configuration (env vars & flags)</b></summary>

12-factor: config from the environment, logs to stderr, graceful `SIGINT`/`SIGTERM`.
CLI flags override env (**flag > env > default**); `--help` prints the full surface.

| Concern | Flag | Env var | Default |
|---|---|---|---|
| Gateway PROGRAM_ID | `--program-id` | `ERPL_REV_PROGRAM_ID` | `ERPL_REV` |
| Gateway host / service | `--gwhost` / `--gwserv` | `ERPL_REV_GWHOST` / `ERPL_REV_GWSERV` | `localhost` / `3300` |
| Parallel registrations | — | `ERPL_REV_REG_COUNT` | `5` |
| Reach the gateway through a tunnel | `--tunnel-secret <name>` | `ERPL_REV_TUNNEL_SECRET` | — (off; see [docs/tunnel.md](tunnel.md)) |
| Tunnel far end / near end | `--tunnel-target` / `--tunnel-local-port` | `ERPL_REV_TUNNEL_TARGET` / `ERPL_REV_TUNNEL_LOCAL_PORT` | the gateway / a free loopback port |
| Disable quack | `--no-quack` | `ERPL_REV_NO_QUACK` | quack is **on**, bound to loopback |
| Quack bind / token | `--quack-listen` / `--quack-token` | `ERPL_REV_QUACK_LISTEN` / `ERPL_REV_QUACK_TOKEN` | `quack:localhost` (port 9494) / random |
| DuckDB file | `--db <path>` | `ERPL_REV_DB_PATH` | `erpl-rev.duckdb` (`:memory:` for in-mem) |
| Boot init SQL | `--init-sql` / `--init-file` | `ERPL_REV_DUCKDB_INIT` | — (ATTACH/secrets for external/cloud targets) |
| Telemetry opt-out | `--no-telemetry` | `ERPL_REV_NO_TELEMETRY` / `DATAZOO_DISABLE_TELEMETRY` | on by default ([what is sent](../TELEMETRY.md)) |
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

