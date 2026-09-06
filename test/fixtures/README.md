# Test fixtures

## `control_schema_v1.duckdb`

A control-schema **version 1** DuckDB file: the shape `DuckDbBridge`'s constructor produced at
commit `1a97fbb`, before `_erpl_rev_schema_version` existed. Four control tables
(`_erpl_rev_delta_state`, `_erpl_rev_cdc`, `_erpl_rev_run_stats`, `_erpl_rev_cli_cmd`), the
`erpl_rev_run_stats` view and two sequences — and no version table.

It carries seeded rows on purpose, so a migration test proves customer data **survives** rather than
merely that the DDL runs: two delta-state targets (a `NUMTS` and a `DATETIME` watermark), two CDC
registrations of which one is `mode='FULL_IUD'` (the value migration v3 rewrites to `IMAGE_IUD`), a
run-stats row and a CLI-queue row.

Stored gzipped (2.6 MB → 10 KB); the test decompresses it to a temp file with zlib, which the test
target already links.

**This file cannot be regenerated once the constructor writes a version table.** It was produced by
the pre-change binary and checked in first, deliberately — a hand-synthesised copy made later from
v2+ code would not test the migration it claims to. Do not "refresh" it.

## Test naming convention

`ctest -R <area>` matches Catch2 **case names**, not tags, so every new case is
named `area: what it proves` (e.g. `sqlname: Token keeps ...`). Tags stay for
`./erpl_rev_tests "[sqlname]"`; the prefix is what makes CI able to run a subset.
