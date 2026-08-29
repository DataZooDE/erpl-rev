cmake_minimum_required(VERSION 3.16)
project(erpl_rev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Product version baked in for telemetry. CI passes -DERPL_REV_VERSION=X.Y.Z;
# otherwise it falls back to "dev" (see src/main.cpp / src/erpl_rev_telemetry.cpp).
if(NOT DEFINED ERPL_REV_VERSION OR ERPL_REV_VERSION STREQUAL "")
  set(ERPL_REV_VERSION "dev")
endif()

# Platform sub-dir of the repo-local NW RFC SDK (gitignored; provided locally or
# fetched from S3 in CI — same convention as DataZooDE/erpl).
if(WIN32)
  set(_sdk_sub win)
elseif(APPLE)
  set(_sdk_sub osx)
else()
  set(_sdk_sub linux)
endif()

# --- RFC backend -------------------------------------------------------------
# Which implementation of the NW RFC C ABI to build against:
#   sdk   - SAP's closed-source libsapnwrfc (+ libsapucum, + ICU), downloaded
#           from SAP and vendored under nwrfcsdk/.
#   proto - erpl-proto's pure-Rust drop-in shim, which exports the same C ABI
#           and ships its own sapnwrfc.h, so building needs no SAP download at
#           all.
#
# The two are ABI-identical, so this is purely a change of header and link
# target: not one source file has an #ifdef for it.
set(RFC_BACKEND "sdk" CACHE STRING "RFC implementation: sdk | proto")
set_property(CACHE RFC_BACKEND PROPERTY STRINGS sdk proto)
if(NOT RFC_BACKEND MATCHES "^(sdk|proto)$")
  message(FATAL_ERROR "RFC_BACKEND must be 'sdk' or 'proto', got '${RFC_BACKEND}'")
endif()
# Shared or static, for the proto backend only: SAP ships no static library.
set(RFC_LINK "shared" CACHE STRING "How to link the RFC backend: shared | static")
set_property(CACHE RFC_LINK PROPERTY STRINGS shared static)
if(RFC_BACKEND STREQUAL "sdk" AND RFC_LINK STREQUAL "static")
  message(FATAL_ERROR "RFC_LINK=static needs RFC_BACKEND=proto; SAP ships no static libsapnwrfc")
endif()

if(RFC_BACKEND STREQUAL "proto")
  # erpl-proto's shim, built by `cargo build --release -p erpl-proto-nwrfc`. The
  # shared object is named libsapnwrfc.so on purpose - a consumer resolves it by
  # SONAME - and the crate emits a staticlib beside it.
  set(ERPL_PROTO_ROOT "" CACHE PATH "Path to an erpl-proto checkout")
  if(NOT ERPL_PROTO_ROOT)
    message(FATAL_ERROR "RFC_BACKEND=proto needs -DERPL_PROTO_ROOT=<erpl-proto checkout>")
  endif()
  set(RFC_INCLUDE "${ERPL_PROTO_ROOT}/crates/erpl-proto-nwrfc/include")
  if(NOT EXISTS "${RFC_INCLUDE}/sapnwrfc.h")
    message(FATAL_ERROR "erpl-proto's sapnwrfc.h not found at ${RFC_INCLUDE}")
  endif()
  if(RFC_LINK STREQUAL "static")
    set(_rfc_lib "${ERPL_PROTO_ROOT}/target/release/${CMAKE_STATIC_LIBRARY_PREFIX}sapnwrfc${CMAKE_STATIC_LIBRARY_SUFFIX}")
  else()
    set(_rfc_lib "${ERPL_PROTO_ROOT}/target/release/${CMAKE_SHARED_LIBRARY_PREFIX}sapnwrfc${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()
  if(NOT EXISTS "${_rfc_lib}")
    message(FATAL_ERROR
      "${_rfc_lib} is missing.\n"
      "Build it with: cargo build --release -p erpl-proto-nwrfc --manifest-path ${ERPL_PROTO_ROOT}/Cargo.toml")
  endif()
  set(SAP_LIBS "${_rfc_lib}")
  message(STATUS "erpl-rev RFC backend: proto (${_rfc_lib})")
else()
  # --- SAP NW RFC SDK --------------------------------------------------------
  set(SAPNWRFC_HOME "" CACHE PATH "Path to the SAP NW RFC SDK platform dir")
  if(NOT SAPNWRFC_HOME)
    get_filename_component(SAPNWRFC_HOME "${CMAKE_CURRENT_SOURCE_DIR}/nwrfcsdk/${_sdk_sub}" ABSOLUTE)
  endif()
  if(NOT EXISTS "${SAPNWRFC_HOME}/include/sapnwrfc.h")
    message(FATAL_ERROR "NW RFC SDK not found at ${SAPNWRFC_HOME}; provide nwrfcsdk/${_sdk_sub}, set -DSAPNWRFC_HOME=..., or build with -DRFC_BACKEND=proto")
  endif()
  # Names resolve across platforms: lib{sapnwrfc,sapucum}.{so,dylib} / {sapnwrfc,libsapucum}.lib
  find_library(SAP_NWRFC_LIB sapnwrfc PATHS "${SAPNWRFC_HOME}/lib" NO_DEFAULT_PATH)
  find_library(SAP_UCUM_LIB  sapucum  PATHS "${SAPNWRFC_HOME}/lib" NO_DEFAULT_PATH)
  if(NOT SAP_NWRFC_LIB OR NOT SAP_UCUM_LIB)
    message(FATAL_ERROR "Could not find sapnwrfc/sapucum under ${SAPNWRFC_HOME}/lib")
  endif()
  set(SAP_LIBS ${SAP_NWRFC_LIB} ${SAP_UCUM_LIB})
  set(RFC_INCLUDE "${SAPNWRFC_HOME}/include")
  message(STATUS "erpl-rev RFC backend: sdk (${SAPNWRFC_HOME})")
endif()

# --- DuckDB (self-contained dist: libduckdb + amalgamated duckdb.hpp) --------
set(DUCKDB_DIST "" CACHE PATH "Standalone DuckDB dist dir (libduckdb + duckdb.hpp)")
if(NOT DUCKDB_DIST)
  message(FATAL_ERROR "DUCKDB_DIST not set; run `make duckdb-dist` or pass -DDUCKDB_DIST=<dir>")
endif()
set(DUCKDB_INCLUDE "${DUCKDB_DIST}")
if(WIN32)
  set(DUCKDB_LIB "${DUCKDB_DIST}/duckdb.lib")        # import lib; duckdb.dll bundled at runtime
elseif(APPLE)
  set(DUCKDB_LIB "${DUCKDB_DIST}/libduckdb.dylib")
else()
  set(DUCKDB_LIB "${DUCKDB_DIST}/libduckdb.so")
endif()

# Catch2 from vcpkg (manifest mode); toolchain supplied by the Makefile/CI.
find_package(Catch2 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# --- Telemetry: DataZooDE/posthog-telemetry submodule ------------------------
# The shared lib needs DuckDB's patched httplib header (third_party/httplib) plus
# DuckDB's source includes, which the prebuilt DUCKDB_DIST (amalgamation only)
# does not ship. Fetch the matching DuckDB v1.5.4 SOURCE (headers only) and hand
# the submodule the two paths it expects — mirroring flapi's "Expose DuckDB
# paths" block. Fetch the matching DuckDB v1.5.4 SOURCE (headers only).
# OpenSSL is linked statically via the vcpkg static triplet, so the
# single-file bundle gains no new runtime .so/.dylib/.dll.
find_package(OpenSSL REQUIRED)

# Must match the prebuilt DUCKDB_DIST engine (Makefile passes the same value).
if(NOT DEFINED DUCKDB_VERSION OR DUCKDB_VERSION STREQUAL "")
  set(DUCKDB_VERSION "1.5.4")
endif()

include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)  # allow FetchContent_Populate (headers only)
endif()
FetchContent_Declare(duckdb_source
  URL https://github.com/duckdb/duckdb/archive/refs/tags/v${DUCKDB_VERSION}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(duckdb_source)
if(NOT duckdb_source_POPULATED)
  message(STATUS "Fetching DuckDB v${DUCKDB_VERSION} source (headers only for posthog-telemetry)")
  FetchContent_Populate(duckdb_source)
endif()
set(DUCKDB_SRC_INCLUDE "${duckdb_source_SOURCE_DIR}/src/include")
set(DUCKDB_THIRD_PARTY "${duckdb_source_SOURCE_DIR}/third_party")
add_subdirectory(third_party/posthog-telemetry)

# Feedback banner (header-only). Telemetry ON so a printed banner is counted as
# a banner_shown event through the instance configured above.
set(DATAZOO_BANNER_TELEMETRY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/datazoo-banner)

# --- SAP build definitions (64-bit Unicode, per platform) --------------------
#
# Only for the SDK backend. Without SAPwithUNICODE, sapucrfc.h silently typedefs
# SAP_UC to a one-byte char while the library keeps writing UTF-16, and every
# string API overruns its buffer by 2x. erpl-proto's header has no such switch -
# SAP_UC is uint16_t unconditionally - so under the proto backend these are not
# merely unnecessary but a per-platform detail to get wrong for no gain.
if(RFC_BACKEND STREQUAL "proto")
  # nothing to define
elseif(WIN32)
  add_compile_definitions(SAPonNT WIN32 SAPwithUNICODE SAPwithTHREADS
                          UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
  add_compile_definitions(SAPonUNIX SAPonDARW SAPwithUNICODE SAPwithTHREADS)
else()
  add_compile_definitions(SAPonUNIX SAPonLIN SAPonLINUX SAPwithUNICODE SAPwithTHREADS
                          __NO_MATH_INLINES)
endif()

# Statically link the C++/gcc runtime on Linux so the bundle needs no toolchain
# libs on the target. (clang/macOS uses libc++; MSVC handled below.)
if(UNIX AND NOT APPLE)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# --- Server ------------------------------------------------------------------
add_executable(erpl_rev_server
  src/sap_uc.cpp
  src/rfc_metadata.cpp
  src/rfc_handlers.cpp
  src/duckdb_bridge.cpp
  src/cdc_dialect.cpp
  src/json_util.cpp
  src/sxml_binary.cpp
  src/logging.cpp
  src/erpl_rev_telemetry.cpp
  src/main.cpp)
target_include_directories(erpl_rev_server PRIVATE
  "${RFC_INCLUDE}" "${DUCKDB_INCLUDE}" src)
target_compile_definitions(erpl_rev_server PRIVATE
  ERPL_REV_VERSION="${ERPL_REV_VERSION}"
  ERPL_REV_DUCKDB_VERSION="${DUCKDB_VERSION}")
target_link_libraries(erpl_rev_server PRIVATE
  ${SAP_LIBS} ${DUCKDB_LIB} Threads::Threads ${CMAKE_DL_LIBS}
  posthog_telemetry datazoo_banner OpenSSL::Crypto)

# So the extracted inner binary finds the bundled libs sitting next to it
# (belt-and-suspenders with the launcher's loader-path env var).
if(APPLE)
  set_target_properties(erpl_rev_server PROPERTIES
    BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(erpl_rev_server PROPERTIES
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

# --- Self-extracting launcher (the single-file distributable wraps this) -----
add_executable(erpl_rev_launch launcher/erpl_rev_launch.cpp)
if(UNIX AND NOT APPLE)
  target_link_options(erpl_rev_launch PRIVATE -static-libstdc++ -static-libgcc)
endif()

# --- DuckDB-bridge Catch2 tests ----------------------------------------------
if(EXISTS "${DUCKDB_LIB}")
  add_executable(erpl_rev_tests
    test/test_duckdb_bridge.cpp
    test/test_cdc_dialect.cpp
    test/test_sxml_binary.cpp
    test/test_telemetry.cpp
    test/bench_ingest.cpp
    src/duckdb_bridge.cpp
    src/cdc_dialect.cpp
    src/json_util.cpp
    src/sxml_binary.cpp
    src/erpl_rev_telemetry.cpp)
  target_include_directories(erpl_rev_tests PRIVATE src "${DUCKDB_INCLUDE}")
  target_compile_definitions(erpl_rev_tests PRIVATE
    ERPL_REV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ERPL_REV_DUCKDB_VERSION="${DUCKDB_VERSION}")
  target_link_libraries(erpl_rev_tests PRIVATE
    ${DUCKDB_LIB} Threads::Threads Catch2::Catch2WithMain
    posthog_telemetry datazoo_banner OpenSSL::Crypto)
  if(APPLE)
    set_target_properties(erpl_rev_tests PROPERTIES BUILD_RPATH "${DUCKDB_DIST}")
  elseif(UNIX)
    set_target_properties(erpl_rev_tests PROPERTIES BUILD_RPATH "${DUCKDB_DIST}")
  endif()
else()
  message(WARNING "libduckdb not found at ${DUCKDB_LIB}; skipping tests.")
endif()
