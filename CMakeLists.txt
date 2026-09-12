cmake_minimum_required(VERSION 3.16)
project(erpl_rev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizers, off by default:
#
#   cmake -S . -B build-asan -G Ninja -DERPL_REV_SANITIZE=address,undefined
#   cmake --build build-asan --target erpl_rev_tests
#   ASAN_OPTIONS=detect_leaks=0 ./build-asan/erpl_rev_tests
#
# Leak detection off on purpose: the suite exercises DuckDB and the RFC SDK,
# whose allocations at exit are not this project's to account for. The classes
# worth catching are use-after-scope and undefined behaviour.
#
# CI no longer runs this -- the job was removed by owner's decision, because a
# full instrumented rebuild of the dependency tree took about three hours with
# no cache and blocked every pull request behind it. The option stays, and is
# worth reaching for by hand when a symptom looks like memory rather than
# logic.
#
# What it is for, from the one time it mattered: a dangling capture in the TUI
# cost a live bisection against a running server with concurrent writers.
# FTXUI's canvas() stores its callback and runs it during layout, so a lambda
# capturing locals by reference read them destroyed and looped forever -- a
# spinning thread and a frozen display, indistinguishable from a hung query by
# eye, and three wrong diagnoses before the right one. ASAN reports that as
# stack-use-after-scope on the first frame, naming the line, in seconds, with
# no server and no terminal.
set(ERPL_REV_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined")
if(ERPL_REV_SANITIZE)
  add_compile_options(-fsanitize=${ERPL_REV_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${ERPL_REV_SANITIZE})
endif()

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

# --- RFC backend: erpl-proto ------------------------------------------------
# erpl-proto's pure-Rust implementation of the NW RFC C ABI, built from the
# pinned `erpl-proto/` submodule. It exports the same C ABI as SAP's closed
# libsapnwrfc and ships its own sapnwrfc.h, so nothing here needs a download
# from SAP -- which is why the released bundles carry no SDK and no ICU.
#
# There is no second backend. erpl-rev built against SAP's SDK until 2026-09;
# that path is gone rather than kept as a default nobody selected, which is how
# the build, the live suite and every CI check ended up exercising an
# implementation the product does not ship.
set(ERPL_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/erpl-proto"
    CACHE PATH "Path to an erpl-proto checkout (default: the submodule)")

# Shared or static. Static is the default and what ships: it puts the shim
# inside the binary, so `ldd` shows no RFC library at all.
set(RFC_LINK "static" CACHE STRING "How to link the RFC backend: shared | static")
set_property(CACHE RFC_LINK PROPERTY STRINGS shared static)
if(NOT RFC_LINK MATCHES "^(shared|static)$")
  message(FATAL_ERROR "RFC_LINK must be 'shared' or 'static', got '${RFC_LINK}'")
endif()

set(RFC_INCLUDE "${ERPL_PROTO_ROOT}/crates/erpl-proto-nwrfc/include")
if(NOT EXISTS "${RFC_INCLUDE}/sapnwrfc.h")
  message(FATAL_ERROR
    "erpl-proto's sapnwrfc.h not found at ${RFC_INCLUDE}.\n"
    "The submodule is probably not checked out: git submodule update --init erpl-proto\n"
    "(or point -DERPL_PROTO_ROOT=<checkout> somewhere else).")
endif()

# The crate emits a staticlib beside the shared object; both are named
# sapnwrfc on purpose -- a consumer resolves the shim by SONAME.
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
message(STATUS "erpl-rev RFC backend: erpl-proto (${_rfc_lib}), link=${RFC_LINK}")

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
find_package(ftxui CONFIG REQUIRED)

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
# No SAP platform switches at all. SAP's sapucrfc.h needs SAPwithUNICODE or it
# silently typedefs SAP_UC to a one-byte char while the library keeps writing
# UTF-16, overrunning every string buffer by 2x. erpl-proto's header has no such
# switch -- SAP_UC is uint16_t unconditionally -- so the whole per-platform
# define set went with the SDK.

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
  src/split_planner.cpp
  src/drift.cpp
  src/transform_macros.cpp
  src/validation.cpp
  src/publish.cpp
  src/metrics.cpp
  src/tui_model.cpp
  src/tui_graph.cpp
  src/tui_theme.cpp
  src/cmd_top.cpp
  src/cdc_status.cpp
  src/portion.cpp
  src/cds_delta.cpp
  src/latency.cpp
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
  src/cmd_abap.cpp
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
# headers they carry are not on the include path.
#
# OpenSSL and ZLIB belong on this list for the same reason the other two do, and
# were missed: payload.cpp includes <zlib.h> and erpl_rev_telemetry.cpp includes
# <openssl/sha.h>, and both moved into this library when the engine sources were
# unified. On Linux and macOS those headers sit in system include paths, so the
# omission was invisible; on Windows they exist only inside the vcpkg imported
# targets, and the build stopped at the first #include.
target_link_libraries(erpl_rev_core PUBLIC
  posthog_telemetry datazoo_banner OpenSSL::Crypto ZLIB::ZLIB
  ftxui::screen ftxui::dom ftxui::component)
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
    test/test_variant_coverage.cpp
    test/test_tick_planner.cpp
    test/test_cycle.cpp
    test/test_split_planner.cpp
    test/test_drift.cpp
    test/test_transform_macros.cpp
    test/test_validation.cpp
    test/test_publish.cpp
    test/test_stats_views.cpp
    test/test_metrics.cpp
    test/test_tui_model.cpp
    test/test_tui_graph.cpp
    test/test_cdc_status.cpp
    test/test_portion.cpp
    test/test_cds_delta.cpp
    test/test_footprint.cpp
    test/test_latency.cpp
    test/test_sxml_binary.cpp
    test/test_payload.cpp
    test/test_telemetry.cpp
    test/test_abap_codegen.cpp
    test/test_cli_common.cpp
    test/test_cmd_queue.cpp
    test/test_table_render.cpp
    test/test_sap_setup.cpp
    test/test_cmd_abap.cpp
    test/test_tunnel.cpp
    test/bench_ingest.cpp
    test/bench_cycle.cpp)
  # The coverage matrix in test/test_variant_coverage.cpp works by switching
  # over each enum with no `default:`. That is only a WARNING by default, so the
  # gate compiled happily with a variant missing and proved nothing -- the exact
  # shape of defect it exists to catch. Promoted to an error here, which is what
  # turns "somebody should notice" into "this does not build".
  if(NOT MSVC)
    target_compile_options(erpl_rev_tests PRIVATE -Werror=switch)
  else()
    target_compile_options(erpl_rev_tests PRIVATE /we4062)
  endif()
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
