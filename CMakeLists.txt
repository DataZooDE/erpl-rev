cmake_minimum_required(VERSION 3.16)
project(erpl_rev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Static CRT (/MT), to match DuckDB. duckdb/CMakeLists.txt sets
  # CMAKE_MSVC_RUNTIME_LIBRARY unconditionally as a plain variable, so it wins
  # inside its own subdirectory and cannot be overridden from here -- the only
  # workable direction is for us to match it. Mixing the two produces LNK2038
  # "mismatch detected for 'RuntimeLibrary'" for every DuckDB object.
  #
  # This is why the Windows triplet is x64-windows-static and no longer
  # x64-windows-static-md: the -md variant existed to match the prebuilt DuckDB
  # DLL, which no longer exists. A fully static CRT also means one fewer thing
  # the machine has to have installed.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
endif()
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
  # A Rust staticlib does not carry its own dependencies: the C++ consumer must
  # name them or the link fails with unresolved symbols that read like our bug.
  # Linux got away with nothing here only because its set is implicit. These are
  # the lists `cargo rustc -- --print native-static-libs` reports for the shim,
  # captured per platform by erpl-proto's `cross` CI job.
  if(RFC_LINK STREQUAL "static")
    if(WIN32)
      # kernel32 ntdll userenv ws2_32 dbghelp, plus /defaultlib:msvcrt -- the
      # DYNAMIC CRT, which is what the x64-windows-static-md triplet also uses,
      # so the two agree. A mismatch here shows up as duplicate CRT symbols.
      list(APPEND SAP_LIBS ntdll userenv ws2_32 dbghelp)
    elseif(APPLE)
      # -lSystem -lc -lm, all implicit in the default macOS link line.
    else()
      # -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc, all implicit on Linux; the
      # explicit Threads/dl links elsewhere in this file already cover them.
    endif()
  endif()
  message(STATUS "erpl-rev RFC backend: proto (${_rfc_lib}), link=${RFC_LINK}")
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

# --- DuckDB -----------------------------------------------------------------
# Static by default, from the pinned `duckdb/` submodule.
#
# The prebuilt libduckdb_static.a in DuckDB's release zip cannot be linked: it
# leaves duckdb::ExtensionHelper::LoadAllExtensions undefined *inside the
# archive*, expecting extension libraries the zip does not ship.
# `dummy_static_extension_loader`, a target of the DuckDB build itself, is what
# provides that symbol -- which is why this needs the source, not the release.
#
# Autoloading and autoinstall stay ON: the static loader covers *statically
# linked* extensions (there are none), and the server still installs `quack` and
# opportunistically loads `httpfs` at runtime. Turning these off would build
# fine and then fail at the first `INSTALL quack`.
# Reported by --smoke and stamped into telemetry; the engine itself comes from
# the submodule, so this is a label, not a selector.
if(NOT DEFINED DUCKDB_VERSION OR DUCKDB_VERSION STREQUAL "")
  set(DUCKDB_VERSION "1.5.5")
endif()

# DuckDB is built from the pinned `duckdb/` submodule and linked statically.
# There is deliberately no prebuilt-libduckdb path: DuckDB's release
# amalgamation and its in-tree headers do not agree on their own API (result
# column names are `string` in one and `Identifier` in the other), so supporting
# both means a compatibility shim on every call site -- for a path CI would
# never exercise and which would rot unnoticed. One engine, always tested.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/CMakeLists.txt")
  message(FATAL_ERROR
    "duckdb/ submodule is empty -- run `git submodule update --init --recursive`.")
endif()

# We want the engine, not DuckDB's own shell, unit tests or benchmarks. Those
# add minutes per platform per build, and its test tree resolves a relative
# source path against *our* source dir, so configuring it fails outright.
#
# BUILD_UNITTESTS in particular is not merely slow: with it ON, DuckDB exposes
# internals that are private in a normal build, so code can compile against
# members it has no business touching and then break the moment the flag flips.
# DuckDB derives its version string from `git describe`. In a shallow submodule
# clone that produces nonsense -- "v1.6.0-dev82307" for a checkout sitting
# exactly on the v1.5.4 tag -- which then goes into --smoke output, telemetry and
# the extension-repository path. State it instead of deriving it.
set(DUCKDB_EXPLICIT_VERSION "v${DUCKDB_VERSION}" CACHE STRING "" FORCE)

set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

# Autoloading and autoinstall stay ON: the statically linked set below is the
# engine's built-in extensions, while the server still installs `quack` and
# opportunistically loads `httpfs` at runtime.
set(ENABLE_EXTENSION_AUTOLOADING TRUE)
set(ENABLE_EXTENSION_AUTOINSTALL TRUE)
set(DUCKDB_EXTENSION_CONFIGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_extensions.cmake")
if(WIN32)
  set(DUCKDB_EXPLICIT_PLATFORM "windows_amd64")
endif()

add_subdirectory(duckdb EXCLUDE_FROM_ALL)
set(DUCKDB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include")

# duckdb_static, then the real generated loader, then the extension archives it
# calls into. NOT dummy_static_extension_loader: that target exists for the
# prebuilt libduckdb_static.a, which ships without a loader. Linking it here
# satisfies the loader symbol with a no-op, so the extensions below compile in
# and then never register -- which surfaces as 58 unit tests failing on icu
# operators, not as anything mentioning extensions.
set(DUCKDB_LIB
    duckdb_static
    duckdb_generated_extension_loader
    core_functions_extension
    parquet_extension
    icu_extension
    json_extension
    autocomplete_extension)

# Catch2 from vcpkg (manifest mode); toolchain supplied by the Makefile/CI.
find_package(Catch2 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# --- Telemetry: DataZooDE/posthog-telemetry submodule ------------------------
# The shared lib needs DuckDB's patched httplib header (third_party/httplib)
# plus DuckDB's source includes. Both come from the duckdb/ submodule, which is
# the same tree the engine is compiled from — so the headers cannot drift from
# the engine, which they could when this was a separate tarball download.
# OpenSSL is linked statically via the vcpkg static triplet, so the
# single-file bundle gains no new runtime .so/.dylib/.dll.
find_package(OpenSSL REQUIRED)

# zlib inflates a gzip-framed ingest payload (src/payload.cpp). Like OpenSSL it
# comes from the vcpkg static triplet, so the single-file bundle gains no new
# runtime .so/.dylib/.dll and DuckDB stays the only bundled library.
find_package(ZLIB REQUIRED)

# The ABAP objects `erpl-rev setup` deploys, compiled into the binary: a PyPI
# install has no checkout to read them from.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_abap.cmake")
erpl_generate_abap_assets(ERPL_ABAP_GENERATED)


set(DUCKDB_SRC_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include")
set(DUCKDB_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party")
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
  # No SAP platform switches. One define, so --smoke can name the backend it
  # actually contains rather than claiming SAP's SDK.
  add_compile_definitions(ERPL_RFC_BACKEND_PROTO)
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

# --- Shared engine (one source list, two consumers) ---------------------------
# The server and the test binary compile the same engine sources. Listing them
# twice invites the failure where a new file is added to one list only and the
# test binary silently keeps testing an older translation unit -- so they are
# listed once, here, and both targets link the objects.
#
# Deliberately NOT here: sap_uc/rfc_metadata/rfc_handlers (need the RFC SDK
# headers), logging (only the server and rfc_handlers use it) and main.
add_library(erpl_rev_core OBJECT
  src/duckdb_bridge.cpp
  src/control_schema.cpp
  src/delta_plan.cpp
  src/load_type.cpp
  src/rfc_contract.cpp
  src/tick_planner.cpp
  src/cycle.cpp
  src/cdc_dialect.cpp
  src/json_util.cpp
  src/sxml_binary.cpp
  src/payload.cpp
  src/adt.cpp
  src/abap_codegen.cpp
  src/db_client.cpp
  src/table_render.cpp
  src/cmd_sql.cpp
  src/cmd_sync.cpp
  src/abap_skeletons.cpp
  src/cli_common.cpp
  src/tunnel.cpp
  src/sap_setup.cpp
  src/erpl_rev_telemetry.cpp
  ${ERPL_ABAP_GENERATED})
target_include_directories(erpl_rev_core PUBLIC src "${DUCKDB_INCLUDE}")
target_compile_definitions(erpl_rev_core PUBLIC
  ERPL_REV_DUCKDB_VERSION="${DUCKDB_VERSION}"
  ERPL_REV_VERSION="${ERPL_REV_VERSION}")
# Linked for their INTERFACE include directories: an OBJECT library does not
# inherit a consumer's link interface, so these have to be named here or the
# telemetry/banner headers are not on the include path.
target_link_libraries(erpl_rev_core PUBLIC posthog_telemetry datazoo_banner)
set_target_properties(erpl_rev_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Server ------------------------------------------------------------------
add_executable(erpl_rev_server
  src/sap_uc.cpp
  src/rfc_metadata.cpp
  src/rfc_handlers.cpp
  src/logging.cpp
  src/main.cpp)

# DuckDB is linked INTO this executable, so a loaded .duckdb_extension resolves
# its duckdb:: symbols against the binary rather than a libduckdb.so. -rdynamic
# keeps those symbols in the dynamic table.
#
# Measured: httpfs and quack both load correctly WITHOUT this, so it is not
# load-bearing for the extensions erpl-rev uses today. It is kept because flapi,
# which loads its whole extension set as prebuilt binaries, does need it -- and
# an extension that does resolve host symbols fails as a segfault inside its own
# LoadInternal, which reads as a broken extension rather than a missing flag.
set_target_properties(erpl_rev_server PROPERTIES ENABLE_EXPORTS TRUE)

target_include_directories(erpl_rev_server PRIVATE "${RFC_INCLUDE}")
target_link_libraries(erpl_rev_server PRIVATE
  erpl_rev_core ${SAP_LIBS} ${DUCKDB_LIB} Threads::Threads ${CMAKE_DL_LIBS}
  posthog_telemetry datazoo_banner OpenSSL::Crypto ZLIB::ZLIB)

# So the extracted inner binary finds the bundled libs sitting next to it
# (belt-and-suspenders with the launcher's loader-path env var).
if(APPLE)
  set_target_properties(erpl_rev_server PROPERTIES
    BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(erpl_rev_server PROPERTIES
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

# --- DuckDB-bridge Catch2 tests ----------------------------------------------
if(TRUE)  # tests always build; DuckDB is in-tree
  add_executable(erpl_rev_tests
    test/test_duckdb_bridge.cpp
    test/test_cdc_dialect.cpp
    test/test_sql_name.cpp
    test/test_control_schema.cpp
    test/test_delta_plan.cpp
    test/test_load_type.cpp
    test/test_rfc_contract.cpp
    test/test_cdc_apply_keys.cpp
    test/test_tick_planner.cpp
    test/test_cycle.cpp
    test/test_sxml_binary.cpp
    test/test_payload.cpp
    test/test_telemetry.cpp
    test/test_abap_codegen.cpp
    test/test_cli_common.cpp
    test/test_cmd_queue.cpp
    test/test_table_render.cpp
    test/test_sap_setup.cpp
    test/test_tunnel.cpp
    test/bench_ingest.cpp)
  target_compile_definitions(erpl_rev_tests PRIVATE
    ERPL_REV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ERPL_REV_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures"
    ERPL_REV_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(erpl_rev_tests PRIVATE
    erpl_rev_core ${DUCKDB_LIB} Threads::Threads Catch2::Catch2WithMain
    posthog_telemetry datazoo_banner OpenSSL::Crypto ZLIB::ZLIB)

  # ctest, so CI (and a bisect) can run one case or one tag instead of the whole
  # binary. catch_discover_tests registers each Catch2 case as its own ctest
  # test, which is what makes `ctest -R watermark` work; a single aggregate
  # add_test() would not. `make test` still runs the binary directly.
  enable_testing()
  include(Catch)
  catch_discover_tests(erpl_rev_tests)
  # No BUILD_RPATH: DuckDB is inside the binary, so there is nothing to find.
else()
  message(WARNING "libduckdb not found at ${DUCKDB_LIB}; skipping tests.")
endif()
