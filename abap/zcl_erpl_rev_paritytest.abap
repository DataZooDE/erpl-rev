CLASS zcl_erpl_rev_paritytest DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Two independent paths to the same data must agree, cell by cell.
*"*
*"* Every defect found in KEYS_IUD was type-specific or key-specific, and row
*"* counts matched throughout: rows were deleted and re-inserted empty, values
*"* were coerced wrong, keys were joined on the wrong column. A count check is
*"* blind to all of it. So is a spot check on the columns someone thought to
*"* look at.
*"*
*"*   Path A  the incremental pipeline under test -- the complicated one.
*"*   Path B  a plain full load of the same source at the same moment -- the
*"*           simplest path there is, and the least likely to be wrong.
*"*
*"* diff_joindiff from the anofox-tabular extension compares them and returns
*"* row-level added / changed / removed. Unlike a count it NAMES the offending
*"* key and column, which is the difference between "something is wrong" and a
*"* defect someone can fix.
*"*
*"* The corpus is deliberately nasty: negative decimals (ABAP's trailing sign
*"* has bitten twice), NUMC leading zeros, a value that is empty rather than
*"* null, unicode, and DATS/TIMS boundaries.
*"*
*"* LIMITS, because a green diff here is not a proof of correctness. Both paths
*"* are OURS: if they share a coercion bug they agree and are both wrong. The
*"* only comparison that crosses the boundary to SAP is `sync validate --full`,
*"* which uses the DDIC canonicalisation in zcl_erpl_rev_util=>fingerprint_cell.
*"* This suite is the cheap wide net; that is the anchor.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_a    TYPE string VALUE 'par_a'.       " incremental, under test
    CONSTANTS c_b    TYPE string VALUE 'par_b'.       " full load, the reference
    CONSTANTS c_src  TYPE string VALUE 'ZDELTA_ALL'.
    CONSTANTS c_keys TYPE string VALUE 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'.

    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok  IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS sql IMPORTING iv_sql TYPE string.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    "! rows of diff_joindiff(B, A) -- 0 means the two paths agree
    METHODS drift RETURNING VALUE(rv) TYPE i.
    "! the first few disagreements, named, for the failure message
    METHODS drift_detail RETURNING VALUE(rv) TYPE string.
    METHODS seed IMPORTING iv_from TYPE i iv_count TYPE i.
ENDCLASS.

CLASS zcl_erpl_rev_paritytest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD sql.
    zcl_erpl_rev_util=>query( iv_sql ).
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD drift.
    " LOAD every time: the extension is installed once into the server's
    " extension directory, but a server that has restarted since has not loaded
    " it, and a diff that errors is not a diff that passed.
    sql( |LOAD anofox_tabular| ).
    rv = cnt( |SELECT count(*) AS c FROM diff_joindiff('{ c_b }','{ c_a }',| &&
              |['client','bukrs','belnr','gjahr','buzei'])| ).
  ENDMETHOD.

  METHOD drift_detail.
    " Which rows, and what kind. A parity suite whose failure says only "3" has
    " told the reader nothing they can act on.
    sql( |LOAD anofox_tabular| ).
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT string_agg(concat_ws(' ', diff_type, belnr, buzei), ', ') AS s FROM (| &&
      |SELECT * FROM diff_joindiff('{ c_b }','{ c_a }',| &&
      |['client','bukrs','belnr','gjahr','buzei']) LIMIT 5)| ).
    FIND PCRE '"[^"]+"\s*:\s*"([^"]*)"' IN ls-rows SUBMATCHES rv.
    IF sy-subrc <> 0. rv = ls-rows. ENDIF.
  ENDMETHOD.

  METHOD seed.
    " An edge-value corpus, not a hundred copies of one row. Each of these has
    " a history of being got wrong somewhere in a replication pipeline.
    DATA ls TYPE zdelta_all.
    DO iv_count TIMES.
      DATA(lv_i) = iv_from + sy-index.
      GET TIME STAMP FIELD DATA(lv_ts).
      CLEAR ls.
      ls-client     = sy-mandt.
      ls-bukrs      = '1000'.
      ls-belnr      = |{ lv_i WIDTH = 10 ALIGN = RIGHT PAD = '0' }|.
      ls-gjahr      = '2026'.
      " NUMC with leading zeros, which a naive integer round-trip loses.
      ls-buzei      = |{ ( lv_i MOD 7 ) + 1 WIDTH = 3 ALIGN = RIGHT PAD = '0' }|.
      ls-chg_tstamp = lv_ts.
      ls-chg_dats   = sy-datum.
      ls-chg_date2  = COND #( WHEN lv_i MOD 5 = 0 THEN '00010101' ELSE sy-datum ).
      " Midnight and one second before it: TIMS boundaries.
      ls-chg_time   = SWITCH #( lv_i MOD 3 WHEN 0 THEN '000000'
                                           WHEN 1 THEN '235959'
                                           ELSE sy-uzeit ).
      ls-chg_counter = lv_i.
      " NEGATIVE decimals. ABAP carries the sign trailing, and getting that
      " wrong has cost this project twice.
      ls-dmbtr      = COND #( WHEN lv_i MOD 2 = 0 THEN lv_i * '-1.55' ELSE lv_i * '1.55' ).
      ls-wrbtr      = ls-dmbtr * -1.
      ls-pswbt      = '-0.01'.
      ls-menge      = COND #( WHEN lv_i MOD 3 = 0 THEN '-0.001' ELSE '0.001' ).
      ls-waers      = 'EUR'.
      ls-meins      = 'ST'.
      " Empty rather than null, which is a different thing and often conflated.
      ls-zuonr      = COND #( WHEN lv_i MOD 4 = 0 THEN space ELSE |Z{ lv_i }| ).
      " Unicode, including characters outside Latin-1.
      ls-sgtxt      = COND #( WHEN lv_i MOD 6 = 0 THEN 'Ümläut ß · 日本語 · Ω'
                                                  ELSE |parity { lv_i }| ).
      ls-xnegp      = COND #( WHEN lv_i MOD 2 = 0 THEN 'X' ELSE space ).
      ls-budat      = sy-datum.
      ls-bldat      = sy-datum.
      MODIFY zdelta_all FROM ls.
    ENDDO.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.

    " --- a clean slate --------------------------------------------------
    DELETE FROM zdelta_all.
    COMMIT WORK AND WAIT.
    zcl_erpl_rev_cdc=>teardown( c_a ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='{ c_a }'| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target IN ('{ c_a }','{ c_b }')| ).
    sql( |DROP TABLE IF EXISTS { c_a }| ).
    sql( |DROP TABLE IF EXISTS { c_b }| ).
    sql( |DROP TABLE IF EXISTS _erpl_rev_log_{ c_a }| ).
    sql( |DROP SEQUENCE IF EXISTS _erpl_rev_log_{ c_a }_seq| ).

    " --- the tool itself, before anything depends on it -----------------
    " Installed from the community repository, so no unsigned-extension
    " allowance is needed and the server runs unmodified. Asserted first: if
    " this is not available, every assertion below would pass by not running.
    TRY.
        sql( |INSTALL anofox_tabular FROM community| ).
        sql( |LOAD anofox_tabular| ).
      CATCH cx_root.
    ENDTRY.
    DATA(lv_have) = cnt( |SELECT count(*) AS c FROM duckdb_functions() | &&
                         |WHERE function_name='diff_joindiff'| ).
    ok( cond = xsdbool( lv_have > 0 )
        what = 'PARITY-TOOL: anofox-tabular loaded into the server'
        detail = |diff_joindiff overloads={ lv_have } | &&
                 |(INSTALL anofox_tabular FROM community -- needs network)| ).
    IF lv_have = 0.
      out->write( |PARITY RESULT pass={ mv_pass } fail={ mv_fail }| ).
      RETURN.
    ENDIF.

    " --- path A: the incremental pipeline under test --------------------
    seed( iv_from = 0 iv_count = 40 ).

    " Seed the DuckDB target, then register and provision. In that order: a
    " cycle applies a delta, and without the target it errors and the planner
    " skips the target from then on.
    DATA(ls_seed) = zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = c_a ).
    ok( cond = xsdbool( ls_seed-error IS INITIAL )
        what = 'PARITY-A: the incremental target was seeded' detail = ls_seed-error ).

    sql( |INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, | &&
         |log_enabled) VALUES ('{ c_a }','CDC','{ c_src }','{ c_keys }',true)| ).
    DATA(lv_pe) = zcl_erpl_rev_cdc=>provision(
      iv_target = c_a iv_source = c_src iv_keys = c_keys iv_mode = 'KEYS_IUD' ).
    ok( cond = xsdbool( lv_pe IS INITIAL )
        what = 'PARITY-A: triggers provisioned' detail = lv_pe ).
    IF lv_pe IS NOT INITIAL.
      out->write( |PARITY RESULT pass={ mv_pass } fail={ mv_fail }| ).
      RETURN.
    ENDIF.

    " Now change the source the way a working day does: rows added, rows
    " changed, rows physically removed -- and all three inside one window, so
    " the coalesce is exercised rather than three tidy cycles.
    seed( iv_from = 40 iv_count = 20 ).                       " inserts

    " The sign flip is done in ABAP rather than in the UPDATE. Open SQL will
    " not take `dmbtr * -1` in a SET clause, and the point of the flip is the
    " VALUE that reaches DuckDB, not where the arithmetic happened.
    SELECT * FROM zdelta_all WHERE belnr <= '0000000010'
      INTO TABLE @DATA(lt_upd).
    LOOP AT lt_upd ASSIGNING FIELD-SYMBOL(<u>).
      <u>-dmbtr       = <u>-dmbtr * -1.
      <u>-sgtxt       = 'corrected · Ümläut'.
      <u>-chg_counter = <u>-chg_counter + 1.
    ENDLOOP.
    MODIFY zdelta_all FROM TABLE @lt_upd.

    DELETE FROM zdelta_all WHERE belnr BETWEEN '0000000011' AND '0000000015'.
    COMMIT WORK AND WAIT.

    DATA(r1) = zcl_erpl_rev_cdc=>run( c_a ).
    ok( cond = xsdbool( r1-error IS INITIAL )
        what = 'PARITY-A: the cycle ran'
        detail = |err={ r1-error } ins={ r1-ins } upd={ r1-upd } del={ r1-del }| ).

    " --- path B: an independent full load, at this moment ---------------
    DATA(ls_full) = zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = c_b ).
    ok( cond = xsdbool( ls_full-error IS INITIAL )
        what = 'PARITY-B: the reference full load ran' detail = ls_full-error ).

    " --- they must agree, cell by cell ----------------------------------
    DATA(lv_rows_a) = cnt( |SELECT count(*) AS c FROM { c_a }| ).
    DATA(lv_rows_b) = cnt( |SELECT count(*) AS c FROM { c_b }| ).
    ok( cond = xsdbool( lv_rows_a = lv_rows_b AND lv_rows_a > 0 )
        what = 'PARITY: both paths carry the same number of rows'
        detail = |a={ lv_rows_a } b={ lv_rows_b }| ).

    DATA(lv_drift) = drift( ).
    ok( cond = xsdbool( lv_drift = 0 )
        what = 'PARITY: the incremental path matches a full load, cell by cell'
        detail = |{ lv_drift } row(s) differ: { drift_detail( ) }| ).

    " --- the negative control -------------------------------------------
    " A diff that returns zero proves nothing unless it can return non-zero.
    " This suite would otherwise pass just as happily against two empty tables,
    " a broken extension, or a join on the wrong keys -- and a quality gate that
    " passes hardest when it is most broken is worse than none. One cell, in
    " one column, in one row.
    sql( |UPDATE { c_a } SET sgtxt = 'tampered' WHERE belnr = '0000000020'| ).
    DATA(lv_caught) = drift( ).
    ok( cond = xsdbool( lv_caught = 1 )
        what = 'PARITY-CONTROL: one tampered cell is caught, and only that row'
        detail = |{ lv_caught } row(s) reported, expected 1: { drift_detail( ) }| ).

    " And a tampered KEY, which is the failure a value-only comparison misses:
    " the row is not changed, it is simultaneously missing and unexpected.
    sql( |UPDATE { c_a } SET belnr = '9999999999' WHERE belnr = '0000000021'| ).
    DATA(lv_key) = drift( ).
    ok( cond = xsdbool( lv_key >= 3 )
        what = 'PARITY-CONTROL: a tampered key reads as removed plus added'
        detail = |{ lv_key } row(s): { drift_detail( ) }| ).

    " --- leave nothing behind -------------------------------------------
    zcl_erpl_rev_cdc=>teardown( c_a ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='{ c_a }'| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target IN ('{ c_a }','{ c_b }')| ).
    sql( |DROP TABLE IF EXISTS { c_a }| ).
    sql( |DROP TABLE IF EXISTS { c_b }| ).
    DELETE FROM zdelta_all.
    COMMIT WORK AND WAIT.

    out->write( |PARITY RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
