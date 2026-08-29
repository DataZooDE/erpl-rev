# Convenience wrapper around the CMake build for erpl-rev.
#
#   make build     configure (if needed) + compile server and tests
#   make test      build + run the Catch2 unit tests (real DuckDB, no mocks)
#   make run       build + run the RFC server (registers at the SAP gateway)
#   make e2e       full end-to-end test against the live A4H docker system
#   make start-sap (re)start the A4H trial with the gateway ACL open
#   make clean     remove the build directory
#
# The SAP NW RFC SDK lives under nwrfcsdk/linux at the repo root (gitignored —
# see README). Override the SDK location if it lives elsewhere:
#   make build NWRFC_HOME=/path/to/nwrfcsdk/linux
#
# Portable deps (Catch2) come from vcpkg in manifest mode. Point VCPKG_ROOT at
# an external vcpkg checkout:
#   make build VCPKG_ROOT=/path/to/vcpkg

NWRFC_HOME ?= $(CURDIR)/nwrfcsdk/linux
NWRFC_LIB := $(NWRFC_HOME)/lib
BUILD_DIR := build

# DuckDB engine. The quack network server needs DuckDB >=1.5.4 and the matching
# public extension repo, so we use the official prebuilt distribution (fetched
# by `make duckdb-dist`). Point DUCKDB_DIST elsewhere to override; the CMake
# DUCKDB_DIST option follows suit.
DUCKDB_VERSION ?= 1.5.4
DUCKDB_DIST ?= $(CURDIR)/vendor/duckdb-$(DUCKDB_VERSION)

# Which NW RFC C ABI to build against: `sdk` (SAP's, vendored under nwrfcsdk/)
# or `proto` (erpl-proto's pure-Rust shim, which also supplies sapnwrfc.h, so no
# SAP download is needed at all):
#   make build RFC_BACKEND=proto ERPL_PROTO_ROOT=/path/to/erpl-proto
RFC_BACKEND ?= sdk
RFC_LINK ?= shared
ERPL_PROTO_ROOT ?=
# Override DUCKDB_URL/DUCKDB_SHA256 for non-Linux dists (osx-universal / windows-amd64).
DUCKDB_URL ?= https://github.com/duckdb/duckdb/releases/download/v$(DUCKDB_VERSION)/libduckdb-linux-amd64.zip
# Pinned SHA256 of libduckdb-linux-amd64.zip v1.5.4 — verified on download (supply
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

# libsapnwrfc.so dlopen()s the ICU libs by name at runtime, so the server (and
# anything linking libduckdb.so) needs these dirs on LD_LIBRARY_PATH.
# Under the proto backend the SAP lib dir is deliberately kept OFF the path: if
# anything still reached for libsapnwrfc or ICU the run would fail here rather
# than quietly succeed on a leftover.
ifeq ($(RFC_BACKEND),proto)
RUN_ENV := LD_LIBRARY_PATH=$(ERPL_PROTO_ROOT)/target/release:$(DUCKDB_LIB)
else
RUN_ENV := LD_LIBRARY_PATH=$(NWRFC_LIB):$(DUCKDB_LIB)
endif

# Prefer Ninja when available, else fall back to Make generator.
GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo Ninja || echo "Unix Makefiles")

DIST ?= dist

.PHONY: all build configure test run run-mem run-no-quack e2e duckdb-dist submodules start-sap clean bundle

all: build

# Single-file distributable: launcher + inner server + runtime libs (see scripts/bundle.sh).
bundle: build
	./scripts/bundle.sh linux \
	  $(BUILD_DIR)/erpl_rev_server $(BUILD_DIR)/erpl_rev_launch \
	  $(NWRFC_LIB) $(DUCKDB_DIST) $(DIST)/erpl-rev

# Fetch the official prebuilt DuckDB distribution (libduckdb.so + duckdb.hpp).
duckdb-dist: $(DUCKDB_DIST)/libduckdb.so
$(DUCKDB_DIST)/libduckdb.so:
	mkdir -p $(DUCKDB_DIST)
	curl -sL --fail -o $(DUCKDB_DIST)/dist.zip $(DUCKDB_URL)
	echo "$(DUCKDB_SHA256)  $(DUCKDB_DIST)/dist.zip" | sha256sum -c - \
	  || { echo "ERROR: DuckDB download checksum mismatch"; rm -f $(DUCKDB_DIST)/dist.zip; exit 1; }
	cd $(DUCKDB_DIST) && unzip -o dist.zip && rm -f dist.zip

CONFIGURE_DEPS := duckdb-dist submodules
ifeq ($(RFC_BACKEND),proto)
CONFIGURE_DEPS += proto-shim
endif

configure: $(CONFIGURE_DEPS)
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
	      -DCMAKE_BUILD_TYPE=Release -DSAPNWRFC_HOME=$(NWRFC_HOME) \
	      -DDUCKDB_DIST=$(DUCKDB_DIST) -DDUCKDB_VERSION=$(DUCKDB_VERSION) \
	      -DRFC_BACKEND=$(RFC_BACKEND) -DRFC_LINK=$(RFC_LINK) \
	      -DERPL_PROTO_ROOT=$(ERPL_PROTO_ROOT) \
	      $(VCPKG_FLAGS)

# erpl-proto's nwrfc shim; only needed for RFC_BACKEND=proto. The crate emits
# the shared object and the static archive together, so this serves both links.
proto-shim:
	@test -n "$(ERPL_PROTO_ROOT)" || { echo "set ERPL_PROTO_ROOT=<erpl-proto checkout>"; exit 1; }
	cargo build --release -p erpl-proto-nwrfc --manifest-path $(ERPL_PROTO_ROOT)/Cargo.toml

# The telemetry lib (third_party/posthog-telemetry) is a git submodule.
submodules:
	@git submodule update --init --recursive third_party/posthog-telemetry

build: configure
	cmake --build $(BUILD_DIR)

test: build
	$(RUN_ENV) ./$(BUILD_DIR)/erpl_rev_tests

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

start-sap:
	./scripts/start-sap.sh

clean:
	rm -rf $(BUILD_DIR)
