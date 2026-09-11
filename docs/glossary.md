# Glossary

Written for a data engineer who does not work in SAP every day. The first group is
what you need to follow the setup; the second is what you will *hear* from your Basis
team without needing to act on it.

## You need these

**ABAP** — SAP's application language. Everything that runs inside the SAP system is
written in it. You do not need to write any to use erpl-rev, but you will run a
couple of pre-built programs by name.

**SE38 / SA38** — the two transaction codes for "run a program by name". `SA38` is the
one an ordinary user has; `SE38` is the developer's editor which can also run things.
Where a document says either, type the code into the command box at the top left of
SAP GUI, press Enter, type the program name, press **F8** to execute.

**SAP client** — a tenant inside one SAP system, identified by a three-digit number
(`001`, `100`). Data is separated by client, and almost every table's first key field
is the client. You log in to a specific one, and erpl-rev replicates from the one it
logs in to.

**Gateway** — the SAP process that external programs connect to, usually
`sapgw<NN>` on port `33<NN>` (so instance 00 is port 3300). erpl-rev connects
**outbound** to it and registers itself; SAP never dials in to erpl-rev.

**Program ID** — the name erpl-rev registers at the gateway under, default
`ERPL_REV`. The gateway's allow-list and the RFC destination both refer to it.

**RFC destination** — SAP's name for "somewhere ABAP can call". erpl-rev is a
*type-T* destination (T for TCP/IP), set to *registered server* mode, which means it
waits for erpl-rev to connect rather than trying to start anything.

**reginfo** — the gateway's allow-list: which program IDs may register, from which
hosts. Changing it is a Basis job. A permissive one lets anything register; the one
you want names your host and denies the rest.

**ADT** — *ABAP Development Tools*, SAP's HTTP API for source code and program
execution (`/sap/bc/adt`). The erpl-rev **CLI** talks to SAP over this, on the ABAP
server's HTTP port (often 50000) — a completely different connection from the
gateway one the server uses.

**Transport** — how ABAP code moves between SAP systems: a request containing objects,
exported to two files, imported elsewhere. This is how you install erpl-rev's ABAP on
a production system, because production usually does not allow creating code directly.

**Package** — a namespace for ABAP objects. erpl-rev delivers `ZERPL_CORE`; its tests
and fixtures live in `ZERPL_TEST` and are not part of a delivery.

**`S_DEVELOP`** — the authorisation to create or change ABAP objects. Development
systems grant it; production does not. `erpl-rev setup` needs it, which is why
production is installed from a transport instead.

**`S_RFC`** — the authorisation to call a function module remotely. erpl-rev's RFC
user needs it for one function group and nothing else.

**Function module / function group** — an ABAP callable, and the container it lives
in. erpl-rev creates nine function modules in the group `ZERPL_REV`; they are the only
things ABAP calls to reach DuckDB.

**Background job** — SAP's batch scheduler (transaction `SM37` to watch one). Long
loads run as jobs so they are not bounded by a dialog timeout.

**ALV** — SAP's standard interactive result grid. erpl-rev's reports display in one.

**CDHDR / CDPOS** — SAP's change-document tables: a header and its item rows,
recording that a business object changed. One of the delta methods reads them to find
out what to re-read.

**DDIC** — the Data Dictionary, SAP's type system. It is why a `NUMC(4)` keeps its
leading zeros and a `DATS` is a date rather than an eight-character string; erpl-rev
maps DDIC types to real DuckDB types rather than dumping everything as text.

## erpl-rev's own words

**Target** — a DuckDB table erpl-rev keeps filled from a SAP source, plus the
registered settings that say how.

**Cycle** — one run of a delta target: read what changed, apply it, record it.

**Cadence** — how often a target is due. `hourly`, or `micro:2` for every two seconds.

**Method** — how a target finds what changed: `WATERMARK`, `INSERT_ONLY`,
`CHANGEDOC`, `SNAPSHOT` or `CDC`. See [`delta.md`](delta.md).

**Trigger tier / CDC** — database triggers on the source table that log every change,
including physical deletes. The only method that sees a row *leave* without a full
reload. See [`cdc.md`](cdc.md).

**Watermark** — the "everything up to here is replicated" marker a delta target
stores, usually a timestamp from the source's change column.

**Driver (`ZCL_ERPL_REV_CLIDRV`)** — the pre-deployed ABAP program that executes
commands the CLI puts in a queue. It is why CLI commands need no `S_DEVELOP`.

**quack** — the DuckDB network listener the server runs on loopback, so the CLI can
read the live database while the server holds it open.

**A4H** — SAP's free *ABAP Platform trial* appliance. Several documents use it as the
example system.

## Your Basis team's words

You will see these in `security.md` and in the handout `erpl-rev setup` writes. You
do not have to act on them — they are the things Basis decides.

**SNC** — encrypted, authenticated SAP network communication.
**UCON** — a way of restricting which function modules are callable remotely at all.
**`gw/acl_mode`** — the gateway parameter that switches the allow-list on.
**STMS** — the transport management transaction; how Basis imports a transport.
**PFCG** — the role editor; how Basis grants the authorisations above.
**SMGW** — the gateway monitor; where Basis sees registrations and rereads `reginfo`.
**`S_TABU`** — table-level display authorisation.
**DCL** — the access-control layer on CDS views; it is enforced for CDS reads.
**ADBC** — ABAP's native database connection, used for the BW/calculation-view path.
**SLT / LTRS** — SAP's own replication product and its configuration transaction.
erpl-rev is not SLT and does not need it; the comparison comes up because the
field-selection and filtering ideas are the same.
**ODP / SAPI** — SAP's extraction frameworks. erpl-rev deliberately uses neither.
