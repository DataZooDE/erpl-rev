# erpl-rev — Security & Basis hardening

erpl-rev is a **registered external RFC server**: a C++ process registers a
PROGRAM_ID at the SAP gateway, and ABAP calls it via a type-T RFC destination.
This is the part SAP Basis scrutinises most. The guidance below is what makes the
solution **safe to run in production** (and not the dev shortcut we used while
building it).

> ⚠️ The dev/test setup used `gw/acl_mode=0` (open gateway). **Never do that in
> production.** Use an explicit `reginfo` allow-list as below.

## 1. Components & trust boundary
| Piece | What it is |
|---|---|
| `erpl_rev_server` | external C++ process, registers `PROGRAM_ID=ERPL_REV` at the gateway |
| RFC destination `ERPL_REV` | type **T**, **registration mode** (`method='R'`), points at the gateway |
| Function group `ZERPL_REV` | the 5 RFC FMs ABAP calls (`Z_DUCKDB_QUERY/INGEST/OPEN/FETCH/CLOSE`) |
| RFC user | the user under which ABAP→server calls run / the server is reached |

ABAP→server traffic carries **table data** (SAP business data leaving the system),
so it must be access-controlled and, off-box, encrypted (SNC).

## 2. Gateway registration ACL — `reginfo` (mandatory)
Allow **only** this program to register, **only** from the server's host. Maintain
via **SMGW → Goto → Expert Functions → External Security → Maintain ACL Files**
(or the files referenced by `gw/reg_info`).

```
# reginfo — one line, no wildcards on TP/HOST
P TP=ERPL_REV HOST=<server-host-or-ip> ACCESS=<as-host> CANCEL=<as-host>
# deny everything else
P TP=* HOST=*local* ACCESS=*local* CANCEL=*local*
D TP=*
```
- `TP` = the PROGRAM_ID (exact, no `*`).
- `HOST` = the host(s) the server runs on (FQDN/IP; comma-list if HA). No `*`.
- `CANCEL` = who may cancel the registration (keep tight).
- Keep `gw/acl_mode=1` (default) so the ACL is enforced.

`secinfo` is for *started* programs; our server is *registered*, so `secinfo`
needs no entry for it (leave it restrictive for everything else).

## 3. Transport-layer encryption — SNC (recommended; required off-box)
If the server runs on a different host than the gateway, protect the channel with
**SNC** (SAP CommonCryptoLib on both ends, X.509 key pairs/PSE):
- Server: NW RFC SDK SNC params (`SNC_LIB`, `SNC_MYNAME`, `SNC_PARTNERNAME`).
- Gateway: `snc/enable=1`, and require SNC for the registration.
- Same-host loopback only → SNC optional but still recommended.

## 4. RFC user — least privilege
Create a dedicated **Communications**-type user (no dialog logon) for the FM calls
and give it only the delivered role:
- Role **`ZERPL_REV_RFC`** → `S_RFC` for **function group `ZERPL_REV` only**
  (`RFC_TYPE=FUGR`, `RFC_NAME=ZERPL_REV`, `ACTVT=16`). Nothing else.
- The replicator/console reports run under the *end user's* own authorizations
  (so source-table reads obey existing `S_TABU_*`/CDS DCL — no privilege bypass).
- Do **not** grant `S_RFC = *`. Do **not** reuse `DDIC`/`SAP*`.

Minimal `S_RFC` authorization (PFCG):
```
S_RFC: ACTVT=16, RFC_TYPE=FUGR, RFC_NAME=ZERPL_REV
```

## 5. UCON & monitoring
- Put the FMs on a **UCON** communication assembly (enforcement mode) so only
  explicitly exposed FMs are RFC-callable.
- Enable **gateway security logging** (`gw/logging`) and alert on denied
  registrations/calls. Review `SMGW` regularly.

## 6. Server host hardening
- Run as a non-root service account; see `deploy/erpl-rev.service`.
- Credentials for external publish targets (Postgres/DuckLake/object store) go in
  the server's **`--init-file`** (CREATE SECRET / ATTACH), never on the RFC wire,
  never in ABAP. Restrict that file to the service account (`chmod 600`).
- The DuckDB file + quack token: keep the quack listener on loopback unless needed;
  pin a token (`--quack-token`) and treat it as a secret.

## 7. What erpl-rev does NOT do (assurances for Basis)
- **Non-modifying**: ships only `Z*` objects in package `ZERPL`; modifies **no** SAP
  standard repository/customizing objects; no kernel/core changes.
- No background daemons inside SAP; the only persistent process is the external
  server you control.
- Uninstall = delete the `ZERPL` package (transport of copies / object deletion),
  remove the destination + reginfo line, stop the server.
