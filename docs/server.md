# Running the server

The server is the process that registers at the SAP gateway and holds the DuckDB
file open. Everything else — the CLI, the ABAP reports — talks to it or to the file.

## Where your data is

```
./erpl-rev.duckdb
```

A plain DuckDB file, in the working directory unless you say otherwise with
`--db` (or `ERPL_REV_DB_PATH`). `--db :memory:` throws it away on exit.

Three ways to read it:

```bash
erpl-rev sql "SELECT * FROM mara LIMIT 10"   # while the server runs, through quack
duckdb erpl-rev.duckdb                       # once the server has stopped
```

…and from anything that speaks DuckDB — a notebook, a BI tool — by attaching over
quack while the server is up. Only one process may have a DuckDB file open for
writing, which is why `erpl-rev sql` goes through the server rather than around it.

## The quack listener, and its token

`quack` is a DuckDB network server the process runs **on by default, bound to
loopback** (`quack:localhost`, port 9494). The CLI uses it to read the live database.

It generates a random auth token at boot and writes the connection details to:

```
$XDG_RUNTIME_DIR/erpl-rev/server.json      # falls back to a temp dir
```

`erpl-rev sql` and `sync ls|show` find it there themselves. You only need the token
to connect from something else:

```sql
-- from another DuckDB
SELECT * FROM quack_query('quack:localhost', 'SELECT 42', token => '<token>');
```

Pin it with `--quack-token` if you would rather it not change on restart, widen the
bind with `--quack-listen quack:0.0.0.0:9494` (see [`security.md`](security.md) before
you do), or turn it off entirely with `--no-quack` — after which `erpl-rev sql` needs
the server stopped so it can open the file directly.

## Connecting to SAP

| flag | env | default | |
|---|---|---|---|
| `--gwhost` | `ERPL_REV_GWHOST` | `localhost` | gateway host |
| `--gwserv` | `ERPL_REV_GWSERV` | `3300` | gateway service, i.e. `sapgw<NN>` |
| `--program-id` | `ERPL_REV_PROGRAM_ID` | `ERPL_REV` | must match the `reginfo` line |

Registration is **outbound**: the server dials the gateway. Nothing needs to reach
*it*, and it supplies no SAP credentials — the gateway's `reginfo` decides whether the
program ID may register from this host.

Watch for the registration state, not just the listening line:

```
INFO [server] registration state from="starting" to="running"
```

`broken` means the gateway refused it, almost always a `reginfo` that does not name
this host and program ID.

If this host has no route to the gateway at all, there is a tunnel —
[`tunnel.md`](tunnel.md), which opens by telling you that you probably do not need it.

## Boot SQL: publishing targets and secrets

`--init-sql` / `--init-file` run once at boot on a global connection. This is where
`INSTALL`/`LOAD`, `CREATE SECRET` and `ATTACH` go, so replication can write to
parquet, Postgres, DuckLake, Iceberg or BigQuery — see
[`publishing.md`](publishing.md).

Keep the file `0600`: it holds credentials.

## Logging

Environment only — there are no flags for these:

| env | |
|---|---|
| `ERPL_REV_LOG_LEVEL` | `info` by default; `debug` is very verbose |
| `ERPL_REV_LOG_FORMAT` | `text` or `json` |

## Running it as a service

`deploy/erpl-rev.service` is a systemd unit to start from. The two things it needs
that a shell does not give you: a working directory the service account can write the
DuckDB file into, and the gateway settings in the environment.

Container users: [`docker.md`](docker.md). Everything above is the same, configured
through `ERPL_REV_*` variables, with `/data` as the volume.

## Full flag list

```bash
erpl-rev --help
```

This page covers what an operator needs. `--help` is the complete surface, including
the tunnel flags and the metrics port.
