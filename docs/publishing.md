# Publishing to a lakehouse or a warehouse

erpl-rev lands SAP data in DuckDB. Everything DuckDB can write to, you can push it on
to with one SQL statement — parquet on object storage, DuckLake, Iceberg, or an
`ATTACH`ed Postgres / MySQL / BigQuery / MotherDuck.

The target is a normal DuckDB table, so none of this is erpl-rev-specific: if you know
how to do it in DuckDB, it works here.

## Push to MotherDuck (DuckDB's cloud)

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

