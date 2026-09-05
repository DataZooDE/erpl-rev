# Reaching SAP when there is no route

**You almost certainly do not need this page.** erpl-rev registers *outbound* at
the SAP gateway — it opens the connections, SAP never dials in — so a server on
the SAP network needs no inbound firewall rule and no tunnel. Start there.

This page is for the case where the server cannot sit in the SAP network zone and
has no route to `gwhost:gwserv` from where it does sit.

## First: confirm that is actually your problem

```bash
erpl-rev doctor --gwhost sapgw.internal --gwserv 3300
```

```
[ fail ] gateway reachable from this machine
         sapgw.internal:3300
         → Nothing can register until this host can open a TCP connection to the
           gateway. Check the host/port (--gwhost/--gwserv), firewalls and routing.
           If this host has no route to the gateway at all, the server can reach
           it through a tunnel instead -- see --tunnel-secret.
```

If that check passes, stop reading. If it fails, the cheapest fix is usually a
firewall rule for outbound TCP to that one host and port — ask for it first. A
tunnel is what you reach for when that rule is not going to happen.

## What a tunnel changes, and what it does not

erpl-rev can reach the gateway through an [erpl-tunnel](https://github.com/DataZooDE/erpl-tunnel)
forward — SSH, Tailscale or NetBird. Two things to be clear about before you
adopt it.

**It encrypts the leg, which nothing else here does.** `docs/security.md` §3 says
SNC is required off-box, and erpl-rev has no way to configure it today: the RFC
connection parameters are fixed. So an off-box deployment without a tunnel sends
SAP business data across your network in cleartext. A WireGuard mesh fixes that.

**It is not SNC.** The mesh protects the pipe; SNC proves who is at each end.
Never present one as the other to a Basis team, and expect to revisit this when
SNC lands.

**The gateway sees the exit node.** `reginfo`'s `HOST=` will be the address the
forward egresses from, not the erpl-rev host. Read it out of SMGW rather than
inferring it, and pin the ACL to that. If the server is in a cloud VPC this is
usually an improvement — behind NAT or an autoscaling SNAT range there was no
stable address to pin in the first place.

## Setup

**1. Define the secret.** This is erpl-tunnel's own SQL and its own credential;
erpl-rev never sees the key. Put it in the boot init file, mode 0600:

```sql
-- /etc/erpl/init.sql
INSTALL erpl_tunnel FROM community;
LOAD erpl_tunnel;
CREATE SECRET sap (
    TYPE tunnel,
    backend 'tailscale',            -- or 'netbird', or TYPE ssh_tunnel
    auth_key 'tskey-auth-…',
    hostname 'erpl-rev-prod',
    state_dir '/var/lib/erpl/mesh'
);
```

Every backend-specific option — tags, `control_url`, `ephemeral`, groups — is
documented by erpl-tunnel and works unchanged here. Prefer a self-hosted control
plane (headscale, self-hosted NetBird) so the enrollment audit log is yours.

**2. Name it when starting the server.** Note that `--gwhost` still names the
real gateway:

```bash
erpl-rev serve \
  --gwhost sapgw.internal --gwserv 3300 \
  --tunnel-secret sap \
  --init-file /etc/erpl/init.sql
```

erpl-rev installs the extension, picks a free loopback port, opens the forward and
points the RFC registration at it. You do not set `ERPL_REV_GWHOST=127.0.0.1`, and
you do not choose a port. On success:

```
INFO [tunnel] gateway forward up secret="sap" gateway="sapgw.internal:3300" via="127.0.0.1:40511" …
INFO [server] listening (Ctrl-C to stop) program_id="ERPL_REV" gwhost="sapgw.internal" gwserv="3300" via="tunnel sap"
```

If the forward does not come up, the server **stops there** rather than
registering onto a dead port — five registrations against a socket nothing
forwards would surface as `CM_ALLOCATE_FAILURE_RETRY` and send you to inspect a
gateway that is fine.

**3. Tell `doctor` about it**, so it stops trying to prove something it cannot:

```bash
erpl-rev doctor --gwhost sapgw.internal --gwserv 3300 --tunnel-secret sap
```

```
[  ?   ] gateway reachable from the server
         sapgw.internal:3300 through tunnel 'sap'
         → Reached through an erpl-tunnel forward held by the server process, not
           by this one, so it cannot be probed from here -- an actual
           registration proves it.
```

An honest unknown, not a false pass. `erpl-rev setup` persists the secret's *name*
(never the credential) so later runs remember.

## Why not just write it by hand

You can do all of this with `--init-sql` and `ERPL_REV_GWHOST=127.0.0.1`, and it
works — but the configuration then says the SAP gateway is on loopback, and
everything downstream believes it:

- `doctor` probes `127.0.0.1:3300`, which is the *local* end of the forward. That
  socket is bound whether or not the far side is alive, so it reports the gateway
  as reachable on evidence that proves nothing.
- `setup` persists `gwhost=127.0.0.1` as the recorded address of the SAP system.
- The Basis handout renders `ACCESS=127.0.0.1 CANCEL=127.0.0.1`.
- The forward's local port and `ERPL_REV_GWSERV` have to be kept in step by hand,
  with no error when they drift.

`--tunnel-secret` exists so the gateway keeps its name everywhere a human reads
it, and the forward stays an implementation detail.

## Operating it

- **Flap means re-run.** Replication is idempotent (UPSERT, snapshot anti-join),
  so a load interrupted by a dropped forward can simply be run again. A live
  `quack_query` cursor is not idempotent and will surface as a truncated result.
- **Version coupling is exact.** A DuckDB extension is built against one engine
  version. erpl-rev embeds a specific DuckDB, and `INSTALL erpl_tunnel FROM
  community` needs a community build for exactly that version. If it is missing,
  the server says so and names the version rather than reporting an IO error.
- **Nothing changes inside SAP.** The mesh lives entirely in the erpl-rev process.
  The `ZERPL` package, the `S_RFC` role and the type-T destination are untouched.
- **The control plane is new audit surface.** Enrollment, key minting and ACL
  edits are not visible in SMGW, `gw/logging` or UCON. Whoever administers the
  tailnet can grant SAP reachability. Put that under the same change control as
  the gateway, and ship its logs to the same place.

## Turning it off

Drop `--tunnel-secret`. Nothing tunnel-related runs without it — no extension is
installed, no forward is opened, and the RFC layer dials the gateway directly,
exactly as it did before this feature existed.
