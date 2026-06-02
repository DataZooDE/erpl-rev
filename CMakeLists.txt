cmake_minimum_required(VERSION 3.16)
project(erpl_rev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- SAP NW RFC SDK (repo-local, same approach as DataZooDE/erpl) ------------
# Resolves SAPNWRFC_HOME from nwrfcsdk/linux at the repo root (overridable with
# -DSAPNWRFC_HOME=...). The SDK is gitignored; see README for how to provide it
# (locally: copy it in; in CI: fetched from S3 by download_and_extract_nwrfc.sh).
include(${CMAKE_CURRENT_SOURCE_DIR}/scripts/functions.cmake)
set(SAPNWRFC_HOME "" CACHE PATH "Path to the SAP NW RFC SDK (defaults to nwrfcsdk/linux)")
default_linux_libraries()

# DuckDB engine + headers. Self-contained distribution (libduckdb.so +
# amalgamated duckdb.hpp) supplied via -DDUCKDB_DIST=<dir> — the Makefile
# defaults this to the official prebuilt release fetched into vendor/ by
# `make duckdb-dist` (>=1.5.3, matching the public extension repo so the quack
# network server's `INSTALL quack` resolves at runtime).
set(DUCKDB_DIST "" CACHE PATH "Standalone DuckDB dist dir (libduckdb.so + duckdb.hpp)")
if(NOT DUCKDB_DIST)
  message(FATAL_ERROR "DUCKDB_DIST not set; run `make duckdb-dist` or pass -DDUCKDB_DIST=<dir>")
endif()
set(DUCKDB_INCLUDE "${DUCKDB_DIST}")
set(DUCKDB_LIBDIR  "${DUCKDB_DIST}")
set(DUCKDB_LIB     "${DUCKDB_DIST}/libduckdb.so")

# Catch2 comes from vcpkg (manifest mode); the toolchain file is supplied
# externally via the Makefile (-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake).
find_package(Catch2 3 CONFIG REQUIRED)

# SAP NW RFC SDK build defines for 64-bit Unicode Linux.
add_compile_definitions(SAPonUNIX SAPwithUNICODE SAPonLIN SAPwithTHREADS
                        SAPonLINUX __NO_MATH_INLINES)

find_package(Threads REQUIRED)

# Statically link the C++ runtime; remaining dynamic deps are libduckdb.so and
# the SAP .so trio (libsapnwrfc/libsapucum/libicu*), none of which ship static.
add_link_options(-static-libstdc++ -static-libgcc)

# --- Server -----------------------------------------------------------------
add_executable(erpl_rev_server
  src/sap_uc.cpp
  src/rfc_metadata.cpp
  src/rfc_handlers.cpp
  src/duckdb_bridge.cpp
  src/json_util.cpp
  src/sxml_binary.cpp
  src/logging.cpp
  src/main.cpp)
target_include_directories(erpl_rev_server PRIVATE
  ${SAPNWRFC_HOME}/include
  ${DUCKDB_INCLUDE}
  src)
target_link_libraries(erpl_rev_server PRIVATE
  ${SAPNWRFC_LIB_FILES}
  ${DUCKDB_LIB}
  Threads::Threads
  ${CMAKE_DL_LIBS})
# rpath covers the SAP libs + libduckdb.so. (LD_LIBRARY_PATH to the SAP lib dir
# is still needed at runtime for the ICU libs that libsapnwrfc dlopens by name.)
set_target_properties(erpl_rev_server PROPERTIES
  BUILD_RPATH "${SAPNWRFC_HOME}/lib;${DUCKDB_LIBDIR}"
  INSTALL_RPATH "${SAPNWRFC_HOME}/lib;${DUCKDB_LIBDIR}")

# --- DuckDB bridge Catch2 tests ---------------------------------------------
if(EXISTS "${DUCKDB_LIB}")
  add_executable(erpl_rev_tests
    test/test_duckdb_bridge.cpp
    test/test_sxml_binary.cpp
    test/bench_ingest.cpp
    src/duckdb_bridge.cpp
    src/json_util.cpp
    src/sxml_binary.cpp)
  target_include_directories(erpl_rev_tests PRIVATE
    src ${DUCKDB_INCLUDE})
  target_compile_definitions(erpl_rev_tests PRIVATE
    ERPL_REV_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  target_link_libraries(erpl_rev_tests PRIVATE
    ${DUCKDB_LIB} Threads::Threads Catch2::Catch2WithMain)
  set_target_properties(erpl_rev_tests PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBDIR}"
    INSTALL_RPATH "${DUCKDB_LIBDIR}")
else()
  message(WARNING "libduckdb.so not found at ${DUCKDB_LIB}; skipping tests.")
endif()
