// The shipped transformation macros.
//
// Everything the blueprint does with per-field transfer rules in ABAP -- ALPHA
// conversion, XFELD to boolean, SAP date/time parsing, currency-decimal
// correction -- is a DuckDB macro here. No new SAP object, no ABAP CPU per row,
// and testable without SAP.
//
// They are CREATE OR REPLACE on every open, beside the run-stats view, and
// deliberately NOT in the migration list: a macro stored as versioned DDL in a
// customer's file could never be corrected, so a currency bug found later would
// stay broken on every file already created.
#pragma once

namespace erpl_rev {

// The SQL that (re)creates the macros with no external dependency. Run at every
// boot.
const char *TransformMacroSql();

// The currency macros are separate because DuckDB binds a macro body eagerly:
// referencing TCURX in one would make the server refuse to BOOT on any system
// that has not replicated it yet. So when TCURX is absent the macro is defined
// to raise a clear error naming it -- the failure lands at use, on the one query
// that needs it, instead of at startup.
//
// Replicating TCURX takes effect at the next boot, when this is re-evaluated.
const char *CurrencyMacroSql(bool have_tcurx);

}  // namespace erpl_rev
