#include "transform_macros.hpp"

namespace erpl_rev {

const char *TransformMacroSql() {
    return
        // ALPHA: SAP zero-pads a numeric-looking key to the domain length. A key
        // that is not numeric (a material like 'ABC-1') is NOT padded, and
        // padding it anyway would corrupt it.
        "CREATE OR REPLACE MACRO erpl_rev_alpha_in(v, len) AS "
        "  CASE WHEN v IS NULL OR v = '' THEN v "
        "       WHEN regexp_matches(v, '^[0-9]+$') THEN lpad(v, len, '0') "
        "       ELSE v END;"

        "CREATE OR REPLACE MACRO erpl_rev_alpha_out(v) AS "
        "  CASE WHEN v IS NULL OR v = '' THEN v "
        "       WHEN regexp_matches(v, '^[0-9]+$') THEN CAST(CAST(v AS BIGINT) AS VARCHAR) "
        "       ELSE v END;"

        // XFELD: SAP's boolean is 'X' or blank.
        "CREATE OR REPLACE MACRO erpl_rev_xfeld(v) AS (coalesce(v, '') = 'X');"

        // DATS/TIMS: '00000000' and '000000' are SAP's empty values. Parsing them
        // as real dates either fails or invents the year zero, so they are NULL.
        "CREATE OR REPLACE MACRO erpl_rev_dats(v) AS "
        "  CASE WHEN v IS NULL OR v = '' OR v = '00000000' THEN NULL "
        "       ELSE try_strptime(v, '%Y%m%d')::DATE END;"

        "CREATE OR REPLACE MACRO erpl_rev_tims(v) AS "
        "  CASE WHEN v IS NULL OR v = '' THEN NULL "
        "       ELSE try_strptime(v, '%H%M%S')::TIME END;"

        ;
}

const char *CurrencyMacroSql(bool have_tcurx) {
    // SAP stores every currency amount with two implied decimals whatever the
    // currency, so 1234 yen -- which has no decimals -- is stored as 12.34.
    // Reading it without correcting is wrong by a factor of 100, silently, on
    // real money.
    if (have_tcurx)
        return "CREATE OR REPLACE MACRO erpl_rev_curr_decimals(waers) AS ("
               "  SELECT coalesce(max(currdec), 2) FROM tcurx WHERE currkey = waers);"
               "CREATE OR REPLACE MACRO erpl_rev_curr_amount(amount, waers) AS "
               "  amount * power(10, 2 - erpl_rev_curr_decimals(waers));";

    // No TCURX: refuse, and say what is missing. Returning the uncorrected
    // number would be a wrong amount presented as a right one -- the failure
    // mode this macro exists to prevent.
    //
    // error() is called DIRECTLY in the amount macro, not through a nested
    // lookup macro: DuckDB folds a scalar subquery nested inside another macro's
    // arithmetic to NULL instead of raising, so the message would never be seen.
    return "CREATE OR REPLACE MACRO erpl_rev_curr_decimals(waers) AS ("
           "  SELECT error('erpl-rev: currency correction needs TCURX, which is not "
           "replicated into this database. Register TCURX as a target and restart the "
           "server.'));"
           "CREATE OR REPLACE MACRO erpl_rev_curr_amount(amount, waers) AS "
           "  error('erpl-rev: currency correction needs TCURX, which is not replicated "
           "into this database. Register TCURX as a target and restart the server.');";
}

}  // namespace erpl_rev
