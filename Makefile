# Convenience wrapper around the CMake build for erpl-rev.
#
#   make build     configure (if needed) + compile server and tests
#   make test      build + run the Catch2 unit tests (real DuckDB, no mocks)
#   make run       build + run the RFC server (registers at the SAP gateway)
#   make e2e       full end-to-end test against the live A4H docker system
#   make start-sap (re)start the A4H trial with the gateway ACL open
#   make clean     remove the build directory
#
# The RFC implementation is erpl-proto, built from the pinned `erpl-proto/`
# submodule -- `make` builds the Rust shim itself. There is no SAP NW RFC SDK:
#   git submodule update --init erpl-proto
#
# Portable deps (Catch2) come from vcpkg in manifest mode. Point VCPKG_ROOT at
# an external vcpkg checkout:
#   make build VCPKG_ROOT=/path/to/vcpkg

BUILD_DIR := build

# DuckDB engine. Built from the pinned `duckdb/` submodule and linked
# statically, so the server is the whole distributable -- no libduckdb beside
# it, no launcher unpacking one at first run.
#
# STATIC_DUCKDB=OFF falls back to the prebuilt distribution in DUCKDB_DIST
# (fetched by `make duckdb-dist`), which builds in seconds instead of minutes
# and is the faster loop when the change has nothing to do with DuckDB.
DUCKDB_VERSION ?= 1.5.5
DUCKDB_DIST ?= $(CURDIR)/vendor/duckdb-$(DUCKDB_VERSION)
STATIC_DUCKDB ?= ON

# The RFC C ABI comes from erpl-proto's pure-Rust shim, built from the pinned
# `erpl-proto/` submodule. There is no SAP NW RFC SDK anywhere in this build.
RFC_LINK ?= static
ERPL_PROTO_ROOT ?= $(CURDIR)/erpl-proto
# Override DUCKDB_URL/DUCKDB_SHA256 for non-Linux dists (osx-universal / windows-amd64).
DUCKDB_URL ?= https://github.com/duckdb/duckdb/releases/download/v$(DUCKDB_VERSION)/libduckdb-linux-amd64.zip
# Pinned SHA256 of libduckdb-linux-amd64.zip v1.5.5 — verified on download (supply
# DUCKDB_SHA256= for another version).
DUCKDB_SHA256 ?= 838d98a85e697bab9935010c88a8c67d3312ccedcab4cb4a0ba01da65113bb70
DUCKDB_LIB := $(DUCKDB_DIST)

# vcpkg manifest-mode integration (statically links Catch2 via x64-linux).
VCPKG_ROOT ?= $(HOME)/.local/share/vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
VCPKG_TRIPLET ?= x64-linux
VCPKG_FLAGS := -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) \
               -DVCPKG_TARGET_TRIPLET=$(VCPKG_TRIPLET) \
               -DVCPKG_HOST_TRIPLET=$(VCPKG_TRIPLET)

# Only needed for a shared link (and for a non-static DuckDB). A static build --
# the default, and what ships -- resolves everything from inside the binary.
RUN_ENV := LD_LIBRARY_PATH=$(ERPL_PROTO_ROOT)/target/release:$(DUCKDB_LIB)

# Prefer Ninja when available, else fall back to Make generator.
GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo Ninja || echo "Unix Makefiles")

DIST ?= dist

.PHONY: all build configure test ctest run run-mem run-no-quack e2e e2e-full e2e-perf duckdb-dist submodules proto-shim start-sap clean bundle

all: build

# The distributable IS the server: DuckDB is linked in, so there is nothing to
# stage, pack or self-extract. Kept as a target so `make bundle` still does the
# expected thing for anyone with it in muscle memory.
bundle: build
	mkdir -p $(DIST)
	cp $(BUILD_DIR)/erpl_rev_server $(DIST)/erpl-rev

# Fetch the official prebuilt DuckDB distribution (libduckdb.so + duckdb.hpp).
duckdb-dist: $(DUCKDB_DIST)/libduckdb.so
$(DUCKDB_DIST)/libduckdb.so:
	mkdir -p $(DUCKDB_DIST)
	curl -sL --fail -o $(DUCKDB_DIST)/dist.zip $(DUCKDB_URL)
	echo "$(DUCKDB_SHA256)  $(DUCKDB_DIST)/dist.zip" | sha256sum -c - \
	  || { echo "ERROR: DuckDB download checksum mismatch"; rm -f $(DUCKDB_DIST)/dist.zip; exit 1; }
	cd $(DUCKDB_DIST) && unzip -o dist.zip && rm -f dist.zip

CONFIGURE_DEPS := submodules
ifneq ($(STATIC_DUCKDB),ON)
CONFIGURE_DEPS += duckdb-dist          # only the prebuilt path needs the zip
endif
CONFIGURE_DEPS += proto-shim

configure: $(CONFIGURE_DEPS)
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DERPL_REV_STATIC_DUCKDB=$(STATIC_DUCKDB) \
	      -DDUCKDB_DIST=$(DUCKDB_DIST) -DDUCKDB_VERSION=$(DUCKDB_VERSION) \
	      -DRFC_LINK=$(RFC_LINK) -DERPL_PROTO_ROOT=$(ERPL_PROTO_ROOT) \
	      $(VCPKG_FLAGS)

# erpl-proto's nwrfc shim. The crate emits the shared object and the static
# archive together, so this serves both links.
proto-shim:
	@test -f "$(ERPL_PROTO_ROOT)/Cargo.toml" || { \
	  echo "erpl-proto is missing at $(ERPL_PROTO_ROOT)."; \
	  echo "  git submodule update --init erpl-proto"; \
	  echo "(or set ERPL_PROTO_ROOT=<checkout>)"; exit 1; }
	cargo build --release -p erpl-proto-nwrfc --manifest-path $(ERPL_PROTO_ROOT)/Cargo.toml

# The telemetry lib (third_party/posthog-telemetry) is a git submodule.
submodules:
	@git submodule update --init --recursive third_party/posthog-telemetry
	@git submodule update --init erpl-proto
ifeq ($(STATIC_DUCKDB),ON)
	@git submodule update --init --recursive duckdb
endif

build: configure
	cmake --build $(BUILD_DIR)

# TEST_SPEC passes a Catch2 spec straight through, so a single case or tag can
# be run without remembering the binary's path:
#   make test TEST_SPEC='[watermark]'
TEST_SPEC ?=
test: build
	$(RUN_ENV) ./$(BUILD_DIR)/erpl_rev_tests $(TEST_SPEC)

# The same suite through ctest, which is how CI selects a subset by name:
#   make ctest CTEST_ARGS='-R watermark'
CTEST_ARGS ?=
ctest: build
	cd $(BUILD_DIR) && $(RUN_ENV) ctest --output-on-failure $(CTEST_ARGS)

# Run the RFC server WITH the quack network server enabled. Override the bind
# address with QUACK_LISTEN (default loopback; use quack:0.0.0.0:9494 to expose).
QUACK_LISTEN ?= quack:localhost
# File-backed by default (data survives restarts); override the path with DB=,
# or use `make run-mem` for a throwaway in-memory DB.
DB ?= erpl-rev.duckdb
run: build
	$(RUN_ENV) ERPL_REV_QUACK_LISTEN=$(QUACK_LISTEN) ERPL_REV_DB_PATH=$(DB) \
	  ./$(BUILD_DIR)/erpl_rev_server --quack

# Same, but a throwaway in-memory DB (no persistence across restarts).
run-mem: build
	$(RUN_ENV) ERPL_REV_QUACK_LISTEN=$(QUACK_LISTEN) ERPL_REV_DB_PATH=:memory: \
	  ./$(BUILD_DIR)/erpl_rev_server --quack

# RFC server only, without the quack network server.
run-no-quack: build
	$(RUN_ENV) ERPL_REV_DB_PATH=$(DB) ./$(BUILD_DIR)/erpl_rev_server

e2e: build
	./scripts/e2e.sh

# EVERYTHING except the perf lane, in one pass. This is the release gate.
#
# `make e2e` skips @soak -- the daemon running for real, the soak, the streaming
# stress -- because those take minutes rather than seconds. They are also the
# only lanes that drive the product the way a customer does, and every defect
# that has reached main from this tree so far was invisible to the rest.
#
# One pass rather than ONLY='@soak', so a release is gated on everything having
# been green TOGETHER, on one system, in one state. Two lanes that each pass
# alone say nothing about the order they run in.
e2e-full: build
	ERPL_REV_E2E_SKIP='@perf' ./scripts/e2e.sh

# The measured-numbers lane behind docs/perf-results.md.
e2e-perf: build
	ERPL_REV_E2E_ONLY='@perf' ./scripts/e2e.sh

start-sap:
	./scripts/start-sap.sh

clean:
	rm -rf $(BUILD_DIR)
