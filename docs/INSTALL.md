# erpl-rev — Installation (SAP transport + server)

Two parts: (1) get the **ABAP objects** into the SAP system, (2) install the
**external server** + wire up the gateway. Read `docs/security.md` alongside this.

> **Try `erpl-rev setup` first.** If you installed via `uvx erpl-rev`, most of this
> document is automated:
>
> ```
> uvx erpl-rev doctor          # read-only: what is missing, and the fix for each
> uvx erpl-rev setup --dry-run # the exact change set, nothing written
> uvx erpl-rev setup           # deploy, then prove a round trip before claiming success
> ```
>
> **`setup` needs `S_DEVELOP`** (OBJTYPE=CLAS, ACTVT 01 and 02): it creates and
> activates ABAP objects. That is a developer authorisation and is normally
> absent on production, so on production import the transport below instead.
> `doctor` checks it and says so.
>
> The `sync` and `replicate` subcommands do **not** need it once setup (or the
> transport) has deployed `ZCL_ERPL_REV_CLIDRV`: they pass their parameters as
> data through a queue the driver reads. `--queue-only` goes further and does not
> contact SAP at all.
>
> `setup` deploys the production ABAP objects over ADT, creates the function group,
> the type-T destination and the eight `Z_DUCKDB_*` modules, and writes
> `erpl-rev-basis-handout.md` with the two things a client genuinely cannot do — the
> `reginfo` line and the RFC user. Re-running it is idempotent. The rest of this
> document is the manual path, and what the handout refers to.

## 0. Prerequisites
- SAP NetWeaver AS ABAP **7.40 SP05+** (tested on A4H / ABAP 7.5x).
- A host for the external server (Linux) with: the **SAP NW RFC SDK** (`libsapnwrfc.so`
  trio + ICU), the vendored **DuckDB 1.5.4** libs, network access to the SAP **gateway**
  (`sapgw<nr>`, default port 33<nr>).
- Transport import authority (Basis) and a dedicated RFC user (see security.md §4).

## 1. Import the ABAP transport (the package `ZERPL`)
The delivery is a standard transport request: a **cofile `K9xxxxx.<SID>`** + a
**data file `R9xxxxx.<SID>`** (in `transport/`). Production objects live in package
**`ZERPL_CORE`**; tests/demos/fixtures (`ZERPL_TEST`) are **not** part of the
production delivery.

### 1a. With filesystem access to the SAP transport directory (preferred)
```
cp K9xxxxx.<SID>  /usr/sap/trans/cofiles/
cp R9xxxxx.<SID>  /usr/sap/trans/data/
```
Then in SAP: **STMS → Import Overview → <your system> → Extras → Other Requests →
Add**, add the request, and **Import**. (Or `tp addtobuffer` + `tp import` at the OS
level with your domain profile.)

### 1b. Without filesystem access (upload via SAP)
Use **`ARCHIVFILE_CLIENT_TO_SERVER`** (SE37) to upload both files to the server's
`DIR_TRANS` subfolders (data + cofiles), then import via **STMS** as in 1a. (This is
the same path the Theobald transports document.)

## 2. Post-import setup (run once)
1. Run classrun **`ZCL_ERPL_REV_SETUP`**: creates the type-T **`ERPL_REV`**
   destination in **registration mode** (`method='R'`), pointing at your gateway.
   Then run **`ZCL_ERPL_REV_MKFM`**, which creates the eight `Z_DUCKDB_*` function
   modules in function group `ZERPL_REV` (create the group in SE80 first).
   For the `reginfo` line filled in for your host, run `erpl-rev setup
   --print-runbook`, or compose it from security.md §2.
2. Add the **`reginfo`** allow-list line (security.md §2) and reload the ACL in SMGW.
3. Create the **RFC user** + assign role **`ZERPL_REV_RFC`** (security.md §4).

## 3. Install + start the external server
```
# on the server host (as the service account)
export LD_LIBRARY_PATH=$NWRFC/lib:/path/to/duckdb-1.5.4
export ERPL_REV_GWHOST=<gateway-host> ERPL_REV_GWSERV=sapgw<nr>
export ERPL_REV_PROGRAM_ID=ERPL_REV ERPL_REV_DB_PATH=/var/lib/erpl/erpl.duckdb
# external publish targets (optional): ATTACH/secrets in an init file (chmod 600)
./erpl_rev_server --init-file /etc/erpl/init.sql
```
Production: run it as a **systemd service** — see `deploy/erpl-rev.service`.

## 4. Smoke test
- `ZCL_ERPL_REV_DIAG` (ping) → `PONG from erpl-rev`.
- Run `Z_ERPL_REV_REPLICATE` on a small table → DuckDB target; verify row parity.

## 5. Upgrade
Import the next transport (cumulative). Objects are `Z*` and non-modifying, so an
SAP system upgrade does not touch them. Restart the server on a new binary.

## 6. Uninstall
- Stop + disable the server service; remove `reginfo` line; delete destination
  `ERPL_REV` (SM59) and the RFC user/role.
- Remove the ABAP objects: delete package **`ZERPL`** (and subpackages) — e.g. via a
  transport of copies / object deletion. Non-modifying, so nothing else is affected.
