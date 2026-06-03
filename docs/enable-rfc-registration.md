# Enabling external RFC server registration on SAP (A4H trial)

To let `erpl-rev` (an external registered-server program) receive
`CALL FUNCTION … DESTINATION …` from ABAP, **two independent things** must be in
place. Both blocked the first E2E attempt.

## 1. Gateway registration ACL — lets the program attach to the gateway

The SAP gateway decides whether an external TP (program ID) may register.
Relevant profile parameters (A4H 2023 sets these in `DEFAULT.PFL`):

| param | meaning | A4H default |
|-------|---------|-------------|
| `gw/acl_mode` | enforce registration/start ACLs | `1` (on) |
| `gw/reg_info` | path to reginfo allowlist | often **unset** |
| `gw/sec_info` | path to secinfo (start ACL) | set |

With `acl_mode=1` **and no reginfo**, every external registration is denied — so
the server never reaches RUNNING at the gateway.

### Quick & dirty (throwaway trial): disable the ACL

Append to the instance profile, then restart the instance:

```
gw/acl_mode = 0
```

This is exactly what `scripts/start-sap.sh` in this directory does: it copies the
stock erpl profile, appends `gw/acl_mode = 0`, and re-runs the container. Fine
for a local trial; **never do this in production**.

### Proper: a reginfo allowlist (keeps the ACL on, reloadable live)

Keep `gw/acl_mode = 1` and point `gw/reg_info` at a file:

```
# profile
gw/acl_mode = 1
gw/reg_info = /usr/sap/reginfo
```

```
# /usr/sap/reginfo   — first match wins
#VERSION=2
P TP=ERPL_REV HOST=* ACCESS=* CANCEL=*
# trial convenience: keep internal registrations working
P TP=*
# production instead: explicit P entries, then a final  D TP=*
```

Reloading the **reginfo file** needs no restart: **SMGW → Goto → Expert
Functions → External Security → Reread** (or `gwmon` reread). Changing the
profile params themselves (acl_mode / reg_info path) does need a restart.

## 2. SM59 type-T destination — created from ABAP, **must be `method='R'`**

Registration only attaches the program to the gateway. ABAP still needs an **RFC
destination of type "T" / "Registered Server Program"** that names the
PROGRAM_ID. The DEVELOPER user **can** create it from a classrun (no admin
needed) — but the **activation mode is the make-or-break detail**:

```abap
" Recreate cleanly, then create as a REGISTERED server program (method='R'):
CALL FUNCTION 'RFC_MODIFY_TCPIP_DESTINATION'
  EXPORTING destination='ERPL_REV' action='D' EXCEPTIONS OTHERS=0.
CALL FUNCTION 'RFC_MODIFY_TCPIP_DESTINATION'
  EXPORTING destination='ERPL_REV' action='I'
            program='ERPL_REV' gwservice='sapgw00' method='R'
  EXCEPTIONS OTHERS=9.
```

Verify via RFCDES-RFCOPTIONS — it must contain **`a=ERPL_REV`** (registered):

| RFCOPTIONS | activation | result |
|---|---|---|
| `a=ERPL_REV g=sapgw00 …` (method='R') | **Registered Server Program** | ✅ gateway routes to our listening server |
| `G=… N=ERPL_REV …` (method='A', default, or `gwhost` passed) | "Start" mode | ❌ empty SYSTEM_FAILURE, server never reached |

Reference: the SAP-shipped registered dest `SAPLOCALGWREG` is `a=NABAP1 g=sapgw00 …`.

(`authority_not_available` is NOT a problem here — DEVELOPER returns subrc=0. If
you ever do hit it, fall back to SAP\* / SM59 GUI.)

## Debugging tip

`uvx erpl-adt object run <CLASS>` returns **HTTP 500 "SAP server internal
error"** on any ABAP runtime dump — not a useful message. Wrap classrun logic in
`TRY/CATCH` + `out->write( lx->get_text( ) )`, or read the dump in ST22.

## A4H coordinates (recap)

gateway `vhcala4hci`/`sapgw00` (host port 3300), ADT `localhost:50000`, client
`001`, user `DEVELOPER`, password = the SAP-published A4H trial default. Readiness check:
`curl -u DEVELOPER:"$SAP_PASSWORD" http://localhost:50000/sap/bc/adt/core/discovery`
→ 200. Boot ~5–7 min. **Restarting the container wipes `$TMP` ABAP objects.**
