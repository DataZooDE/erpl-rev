# syntax=docker/dockerfile:1
#
# Runtime image for the erpl-rev RFC server. It bakes the EXACT single
# self-extracting bundle published on the release page (`erpl-rev-linux-amd64`):
# the launcher unpacks the inner server + SAP NW RFC libs + ICU + DuckDB into a
# temp cache on first run and sets the loader path itself — so the image deploys
# the same artifact a customer downloads, and needs no LD_LIBRARY_PATH. The CI
# `docker run … --smoke` step verifies it. See docs/docker.md.
FROM ubuntu:24.04

# The extracted inner server + libduckdb.so need libstdc++6/libgcc + libuuid1;
# ca-certificates is for the (opt-out) telemetry POST. The SAP/ICU/DuckDB libs
# themselves travel inside the bundle.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libstdc++6 libuuid1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# The single self-extracting bundle — same file as the GitHub release asset.
COPY erpl-rev /usr/local/bin/erpl-rev

# Sensible container defaults; every ERPL_REV_* var is overridable at `docker run`.
# No LD_LIBRARY_PATH: the launcher self-extracts to $TMPDIR (default /tmp) and sets
# it for the inner server. Mount an emptyDir at /tmp if the rootfs is read-only.
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
      org.opencontainers.image.description="Query and replicate SAP through DuckDB — a registered RFC server bridging ABAP RFC into DuckDB (self-extracting bundle)." \
      org.opencontainers.image.url="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.source="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.licenses="BUSL-1.1" \
      org.opencontainers.image.vendor="DataZoo GmbH"

ENTRYPOINT ["/usr/local/bin/erpl-rev"]
