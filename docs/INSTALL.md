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
> the type-T destination and the nine `Z_DUCKDB_*` modules, and writes
> `erpl-rev-basis-handout.md` with the two things a client genuinely cannot do — the
> `reginfo` line and the RFC user. Re-running it is idempotent. The rest of this
> document is the manual path, and what the handout refers to.

## 0. Prerequisites
- SAP NetWeaver AS ABAP **7.40 SP05+** (tested on A4H / ABAP 7.5x).
- A host for the external server (Linux) with network access to the SAP **gateway**
  (`sapgw<nr>`, default port 33<nr>).
  **A released bundle needs nothing else** — since `v2026.08.30` the RFC protocol is
  `erpl-proto`, linked statically, and DuckDB is linked statically too: one file, no
  SAP NW RFC SDK, no ICU, no `LD_LIBRARY_PATH`. Only a **from-source** build needs the
  SDK, because `make` still defaults to `RFC_BACKEND=sdk`
  (see [building.md](building.md#1-provide-the-sdk--duckdb) — or build
  with `RFC_BACKEND=proto`, which reproduces what ships).
- Transport import authority (Basis) and a dedicated RFC user (see security.md §4).

## 1. Import the ABAP transport (the package `ZERPL_CORE`)

> **There is no prebuilt transport to download**, and there cannot be: a transport
> carries the originating system's object directory, so a generic one is not
> something we can publish. You build it on a system you control — which the binary
> can do for you.

### 1.0 Producing the request on your own DEV system

The fourteen ABAP sources are compiled into the `erpl-rev` binary, so the machine
that builds the transport needs no git checkout — only ADT access to a DEV system
with STMS routes.

An ABAP developer creates the package and an empty workbench request first
(SE80 / SE09 — `setup` will not create either, and says so if they are missing).
Then:

```bash
erpl-rev setup --package ZERPL_CORE --transport A4HK900123
```

Every object `setup` creates is recorded on that request. Release it in SE09 and
STMS moves it to QA and production in the normal way.

**`--transport` is required whenever `--package` is transportable**, and refused
for `$TMP`. This is not pedantry: SAP answers a transportable create with no
request by half-creating the object, recording it on a request it invents, and
returning an HTTP 500 that names neither cause — after which the name is locked
and the retry fails too. `setup` refuses before touching the system instead.

The nine `Z_DUCKDB_*` modules need no separate handling. They are sub-objects of
the function group `ZERPL_REV`, which `setup` puts on the request; the group's
entry carries the whole pool.

`scripts/package-transport.sh` does the same from a checkout and additionally
deploys the `ZERPL_TEST` set, which is what a development system wants.

### 1.0a Taking the sources out instead

For abapGit, a code review, or an import by hand:

```bash
erpl-rev abap export ./abap-out
```

Fourteen files plus a `MANIFEST.txt` giving the **deployment order** — which is not
alphabetical and matters: an interface has to exist before the class whose
signature names it.

Once built, the delivery is a standard transport request: a **cofile
`K9xxxxx.<SID>`** + a **data file `R9xxxxx.<SID>`**. Production objects live in
package **`ZERPL_CORE`**; tests, demos and fixtures (`ZERPL_TEST`) are **not** part
of it.

The transport carries the same **fourteen** objects `erpl-rev setup` deploys — the
list is checked against `ZCL_ERPL_REV_FOOTPRINT`'s by
`scripts/check-transport-complete.sh`, which CI runs, so the two cannot drift. That
includes `ZCL_ERPL_REV_CLIDRV`, which is what lets the CLI work without
`S_DEVELOP`, and `ZCL_ERPL_REV_DIAG`, the smoke test in step 4.

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
the standard path for importing a transport onto a system you have no shell access
to, and the one other SAP add-ons document.)

## 2. Post-import setup (run once)
1. Run classrun **`ZCL_ERPL_REV_SETUP`**: creates the type-T **`ERPL_REV`**
   destination in **registration mode** (`method='R'`), pointing at your gateway.
   Then run **`ZCL_ERPL_REV_MKFM`**, which creates the nine `Z_DUCKDB_*` function
   modules in function group `ZERPL_REV` (create the group in SE80 first).
   For the `reginfo` line filled in for your host, run `erpl-rev setup
   --print-runbook`, or compose it from security.md §2.
2. Add the **`reginfo`** allow-list line (security.md §2) and reload the ACL in SMGW.
3. Create the **RFC user** + assign role **`ZERPL_REV_RFC`** (security.md §4).

## 3. Install + start the external server
```
# on the server host (as the service account)
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
- Remove the ABAP objects: delete packages **`ZERPL_CORE`** (and `ZERPL_TEST` on a dev system) — e.g. via a
  transport of copies / object deletion. Non-modifying, so nothing else is affected.
