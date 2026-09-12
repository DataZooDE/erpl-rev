# syntax=docker/dockerfile:1
#
# Runtime image for the erpl-rev RFC server. It bakes the EXACT binary published
# on the release page (`erpl-rev-linux-amd64`): one self-contained file with the
# erpl-proto RFC implementation and DuckDB linked in — no SAP NW RFC SDK, no ICU,
# nothing to unpack and no LD_LIBRARY_PATH. So the image deploys the same artifact
# a customer downloads. The CI `docker run … --smoke` step verifies it.
# See docs/docker.md.
FROM ubuntu:24.04

# libuuid1 and ca-certificates (the latter for the opt-out telemetry POST).
# libstdc++/libgcc are linked statically, and so are DuckDB and the RFC shim.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libstdc++6 libuuid1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# The single binary — same file as the GitHub release asset.
COPY erpl-rev /usr/local/bin/erpl-rev

# Sensible container defaults; every ERPL_REV_* var is overridable at `docker run`.
# No LD_LIBRARY_PATH: there is nothing beside the binary to find.
ENV ERPL_REV_DB_PATH=/data/erpl-rev.duckdb \
    ERPL_REV_LOG_FORMAT=json

RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin erpl \
    && mkdir -p /data && chown erpl:erpl /data \
    && chmod +x /usr/local/bin/erpl-rev
USER erpl

VOLUME ["/data"]
# Only relevant when the optional quack network server is enabled (--quack).
# RFC registration is OUTBOUND to the SAP gateway, so no RFC port is exposed.
EXPOSE 9494

LABEL org.opencontainers.image.title="erpl-rev" \
      org.opencontainers.image.description="Query and replicate SAP through DuckDB — a registered RFC server bridging ABAP RFC into DuckDB (single static binary)." \
      org.opencontainers.image.url="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.source="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.licenses="BUSL-1.1" \
      org.opencontainers.image.vendor="DataZoo GmbH"

ENTRYPOINT ["/usr/local/bin/erpl-rev"]
