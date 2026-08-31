# Which DuckDB extensions get linked into the static engine.
#
# A bare source build links only core_functions and parquet. The prebuilt
# libduckdb release we used before ALSO carried icu, json and autocomplete, so
# building from source without this file silently produces a narrower engine —
# and the failure is not a missing-extension error, it is 58 unit tests failing
# on things like `TIMESTAMP WITH TIME ZONE - INTERVAL`, whose operator lives in
# icu. Match the release set so switching the link mode changes no behaviour.
#
# httpfs is out-of-tree in 1.5.x and quack is third-party: both stay runtime
# loads (INSTALL/LOAD), which is why extension autoloading is left enabled.
duckdb_extension_load(core_functions)
duckdb_extension_load(parquet)
duckdb_extension_load(icu)
duckdb_extension_load(json)
duckdb_extension_load(autocomplete)
