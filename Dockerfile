# syntax=docker/dockerfile:1
#
# Runtime image for the erpl-rev RFC server. The binary + its runtime libs are
# built/staged in CI (scripts/stage_runtime.sh) and the build context IS that
# staged directory — this Dockerfile only assembles the runtime layer, it does
# not compile. See docs/docker.md.
FROM ubuntu:24.04

# erpl_rev_server static-links libstdc++/libgcc and OpenSSL, so beyond the
# bundled SAP/DuckDB/ICU libs it needs only a few base libs: libstdc++6 +
# libgcc (pulled in by libduckdb.so and the SAP libs), libuuid1, and TLS roots
# for the (opt-out) telemetry POST.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libstdc++6 libuuid1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/erpl-rev

# Build context = the staged payload: erpl_rev_server + libsapnwrfc.so,
# libsapucum.so, libicu{data,i18n,uc}.so.50, libduckdb.so.
COPY . /opt/erpl-rev/

# libsapnwrfc.so dlopen()s the ICU libs BY NAME, so the binary's $ORIGIN rpath
# alone is not enough — point the loader at the lib dir explicitly. Sensible
# container defaults; every ERPL_REV_* var can be overridden at `docker run`.
ENV LD_LIBRARY_PATH=/opt/erpl-rev \
    ERPL_REV_DB_PATH=/data/erpl-rev.duckdb \
    ERPL_REV_LOG_FORMAT=json

# Run unprivileged; /data holds the persisted DuckDB file (mount a volume).
RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin erpl \
    && mkdir -p /data && chown erpl:erpl /data
USER erpl
VOLUME ["/data"]

# Only relevant when the optional quack network server is enabled (--quack).
# RFC registration is OUTBOUND to the SAP gateway, so no RFC port is exposed.
EXPOSE 9494

LABEL org.opencontainers.image.title="erpl-rev" \
      org.opencontainers.image.description="Query and replicate SAP through DuckDB — a registered RFC server bridging ABAP RFC into DuckDB." \
      org.opencontainers.image.url="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.source="https://github.com/DataZooDE/erpl-rev" \
      org.opencontainers.image.licenses="BUSL-1.1" \
      org.opencontainers.image.vendor="DataZoo GmbH"

ENTRYPOINT ["/opt/erpl-rev/erpl_rev_server"]
