cmake_minimum_required(VERSION 3.16)
project(erpl_rev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
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

# --- SAP NW RFC SDK ----------------------------------------------------------
set(SAPNWRFC_HOME "" CACHE PATH "Path to the SAP NW RFC SDK platform dir")
if(NOT SAPNWRFC_HOME)
  get_filename_component(SAPNWRFC_HOME "${CMAKE_CURRENT_SOURCE_DIR}/nwrfcsdk/${_sdk_sub}" ABSOLUTE)
endif()
if(NOT EXISTS "${SAPNWRFC_HOME}/include/sapnwrfc.h")
  message(FATAL_ERROR "NW RFC SDK not found at ${SAPNWRFC_HOME}; provide nwrfcsdk/${_sdk_sub} or set -DSAPNWRFC_HOME=...")
endif()
# Names resolve across platforms: lib{sapnwrfc,sapucum}.{so,dylib} / {sapnwrfc,libsapucum}.lib
find_library(SAP_NWRFC_LIB sapnwrfc PATHS "${SAPNWRFC_HOME}/lib" NO_DEFAULT_PATH)
find_library(SAP_UCUM_LIB  sapucum  PATHS "${SAPNWRFC_HOME}/lib" NO_DEFAULT_PATH)
if(NOT SAP_NWRFC_LIB OR NOT SAP_UCUM_LIB)
  message(FATAL_ERROR "Could not find sapnwrfc/sapucum under ${SAPNWRFC_HOME}/lib")
endif()
set(SAP_LIBS ${SAP_NWRFC_LIB} ${SAP_UCUM_LIB})

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

# --- SAP build definitions (64-bit Unicode, per platform) --------------------
if(WIN32)
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
  src/main.cpp)
target_include_directories(erpl_rev_server PRIVATE
  "${SAPNWRFC_HOME}/include" "${DUCKDB_INCLUDE}" src)
target_link_libraries(erpl_rev_server PRIVATE
  ${SAP_LIBS} ${DUCKDB_LIB} Threads::Threads ${CMAKE_DL_LIBS})

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
    test/bench_ingest.cpp
    src/duckdb_bridge.cpp
    src/cdc_dialect.cpp
    src/json_util.cpp
    src/sxml_binary.cpp)
  target_include_directories(erpl_rev_tests PRIVATE src "${DUCKDB_INCLUDE}")
  target_compile_definitions(erpl_rev_tests PRIVATE
    ERPL_REV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  target_link_libraries(erpl_rev_tests PRIVATE
    ${DUCKDB_LIB} Threads::Threads Catch2::Catch2WithMain)
  if(APPLE)
    set_target_properties(erpl_rev_tests PROPERTIES BUILD_RPATH "${DUCKDB_DIST}")
  elseif(UNIX)
    set_target_properties(erpl_rev_tests PROPERTIES BUILD_RPATH "${DUCKDB_DIST}")
  endif()
else()
  message(WARNING "libduckdb not found at ${DUCKDB_LIB}; skipping tests.")
endif()
