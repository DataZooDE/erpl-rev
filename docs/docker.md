# Running erpl-rev with Docker

erpl-rev publishes a `linux/amd64` container image to GitHub Container Registry:

```
ghcr.io/datazoode/erpl-rev:latest        # newest release
ghcr.io/datazoode/erpl-rev:<version>     # a specific release, e.g. 2026.06.13
ghcr.io/datazoode/erpl-rev:edge          # latest main (unreleased)
ghcr.io/datazoode/erpl-rev:sha-<commit>  # exact commit
```

The image bundles the server, the SAP NW RFC SDK runtime libs, ICU, and DuckDB —
the same payload as the single-file release bundle. It runs as a non-root user
(`uid 10001`) and stores its DuckDB file on a `/data` volume.

## Quick start

```bash
docker run -d --name erpl-rev \
  -e ERPL_REV_GWHOST=sap-gateway.example.com \
  -e ERPL_REV_GWSERV=sapgw00 \
  -e ERPL_REV_PROGRAM_ID=ERPL_REV \
  -v erpl-data:/data \
  ghcr.io/datazoode/erpl-rev:latest
```

Add the DuckDB network server (quack) and publish its port:

```bash
docker run -d --name erpl-rev \
  -e ERPL_REV_GWHOST=sap-gateway.example.com -e ERPL_REV_GWSERV=sapgw00 \
  -e ERPL_REV_QUACK_TOKEN=<your-token> \
  -v erpl-data:/data -p 9494:9494 \
  ghcr.io/datazoode/erpl-rev:latest --quack --quack-listen quack:0.0.0.0:9494
```

## Configuration

Everything is driven by environment variables (CLI flags override them):

| Env var | Default | Purpose |
|---------|---------|---------|
| `ERPL_REV_GWHOST` | `localhost` | SAP gateway host |
| `ERPL_REV_GWSERV` | `3300` | SAP gateway service (e.g. `sapgw00`) |
| `ERPL_REV_PROGRAM_ID` | `ERPL_REV` | registered program id (must match `reginfo`) |
| `ERPL_REV_REG_COUNT` | `5` | parallel gateway registrations |
| `ERPL_REV_DB_PATH` | `/data/erpl-rev.duckdb` | DuckDB file (set `:memory:` for throwaway) |
| `ERPL_REV_QUACK` / `ERPL_REV_QUACK_LISTEN` / `ERPL_REV_QUACK_TOKEN` | off | DuckDB network server |
| `ERPL_REV_LOG_LEVEL` / `ERPL_REV_LOG_FORMAT` | `info` / `json` | logging (image defaults to `json`) |
| `ERPL_REV_NO_TELEMETRY` / `DATAZOO_DISABLE_TELEMETRY` | — | opt out of telemetry ([docs/telemetry.md](telemetry.md)) |

## Networking

RFC registration is **outbound**: the container connects to the SAP gateway and
the gateway dispatches RFC calls back over that connection. So:

- The container needs network reachability to `ERPL_REV_GWHOST:ERPL_REV_GWSERV`.
- **No inbound RFC port** needs publishing. Only `-p 9494:9494` if you enable quack.
- The gateway's `reginfo` ACL must permit `ERPL_REV_PROGRAM_ID` from the
  container host's IP (see [docs/enable-rfc-registration.md](enable-rfc-registration.md)).

## Persistence

The DuckDB file lives at `/data/erpl-rev.duckdb`. Mount a named volume (or host
path) at `/data` so replicated data survives container restarts.

## Verifying the image

`--smoke` loads the bundled SAP SDK + DuckDB and exits without contacting any
gateway — handy as a post-pull sanity check:

```bash
docker run --rm ghcr.io/datazoode/erpl-rev:latest --smoke
# -> erpl-rev smoke ok: SAP NW RFC SDK 750 ...; DuckDB {"v":"v1.5.3"}
```

## Notes

- **Platform:** `linux/amd64` only (the SAP NW RFC SDK is not available for
  linux/arm64 in our build pipeline).
- **SAP SDK:** the image embeds the SAP NW RFC SDK runtime libraries, identical
  to what the published release bundles already ship.
