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
*"* NOT EVERY METHOD CAN MATCH A FULL LOAD, and a matrix that asserted otherwise
*"* would be asserting something false. Each method is held to what it CLAIMS:
*"*
*"*   CDC KEYS_IUD    inserts, updates and physical deletes  -> exact parity
*"*   CDC IMAGE_IUD   the same, from logged row images       -> exact parity
*"*   SNAPSHOT        deletes found by anti-join             -> exact parity
*"*   WATERMARK       inserts and updates only               -> exact parity, and
*"*                   a physical delete must make it DIVERGE
*"*
*"* That last one is the point of the trigger tier written as a test rather than
*"* as a sentence in a document: a watermark cannot see a row leave, so parity
*"* after a delete is a claim it must FAIL. If it ever passes, either the
*"* workload stopped deleting or the diff stopped comparing.
*"*
*"* LIMITS, because a green diff here is not a proof of correctness. Both paths
*"* are OURS: if they share a coercion bug they agree and are both wrong. The
*"* only comparison that crosses the boundary to SAP is `sync validate --full`,
*"* which uses the DDIC canonicalisation in zcl_erpl_rev_util=>fingerprint_cell.
*"* This suite is the cheap wide net; that is the anchor.
*"*
*"* Not covered yet, and named rather than left as a silent gap: CDC DELETE_ONLY,
*"* whose claim is only meaningful paired with a watermark tier, and CHANGEDOC,
*"* which would need synthetic change documents built for ZDELTA_ALL's five-part
*"* key before a diff of it could mean anything.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_b    TYPE string VALUE 'par_ref'.     " the full-load reference
    CONSTANTS c_src  TYPE string VALUE 'ZDELTA_ALL'.
    CONSTANTS c_keys TYPE string VALUE 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'.

    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok  IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS sql IMPORTING iv_sql TYPE string.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    "! rows of diff_joindiff(reference, target) -- 0 means the two paths agree
    METHODS drift IMPORTING iv_target TYPE string RETURNING VALUE(rv) TYPE i.
    "! the first few disagreements, named, for the failure message
    METHODS drift_detail IMPORTING iv_target TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS seed IMPORTING iv_from TYPE i iv_count TYPE i.
    "! drop the target and everything registered against it
    METHODS reset_target IMPORTING iv_target TYPE string.
    "! path B: a plain full load of the source, right now
    METHODS reference RETURNING VALUE(rv) TYPE string.
    "! inserts and updates only -- the workload every method can follow
    METHODS churn_iu.
    "! and the one only the delete-aware tiers can
    METHODS churn_delete.
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
    rv = cnt( |SELECT count(*) AS c FROM diff_joindiff('{ c_b }','{ iv_target }',| &&
              |['client','bukrs','belnr','gjahr','buzei'])| ).
  ENDMETHOD.

  METHOD reset_target.
    zcl_erpl_rev_cdc=>teardown( iv_target ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='{ iv_target }'| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target='{ iv_target }'| ).
    sql( |DROP TABLE IF EXISTS { iv_target }| ).
    sql( |DROP TABLE IF EXISTS _erpl_rev_log_{ iv_target }| ).
    sql( |DROP SEQUENCE IF EXISTS _erpl_rev_log_{ iv_target }_seq| ).
  ENDMETHOD.

  METHOD reference.
    " Rebuilt for every case rather than once: the reference must be the source
    " AS IT IS NOW, and each case leaves the source in a different state.
    sql( |DROP TABLE IF EXISTS { c_b }| ).
    DATA(ls) = zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = c_b ).
    rv = ls-error.
  ENDMETHOD.

  METHOD churn_iu.
    seed( iv_from = 40 iv_count = 20 ).                       " inserts
    " The sign flip is done in ABAP rather than in the UPDATE. Open SQL will not
    " take `dmbtr * -1` in a SET clause, and the point is the VALUE that reaches
    " DuckDB, not where the arithmetic happened.
    SELECT * FROM zdelta_all WHERE belnr <= '0000000010'
      INTO TABLE @DATA(lt_upd).
    LOOP AT lt_upd ASSIGNING FIELD-SYMBOL(<u>).
      <u>-dmbtr       = <u>-dmbtr * -1.
      <u>-sgtxt       = 'corrected · Ümläut'.
      <u>-chg_counter = <u>-chg_counter + 1.
      GET TIME STAMP FIELD <u>-chg_tstamp.
    ENDLOOP.
    MODIFY zdelta_all FROM TABLE @lt_upd.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD churn_delete.
    DELETE FROM zdelta_all WHERE belnr BETWEEN '0000000011' AND '0000000015'.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD drift_detail.
    " Which rows, and what kind. A parity suite whose failure says only "3" has
    " told the reader nothing they can act on.
    sql( |LOAD anofox_tabular| ).
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT string_agg(concat_ws(' ', diff_type, belnr, buzei), ', ') AS s FROM (| &&
      |SELECT * FROM diff_joindiff('{ c_b }','{ iv_target }',| &&
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
    reset_target( 'par_keys' ).
    reset_target( 'par_img' ).
    reset_target( 'par_snap' ).
    reset_target( 'par_wm' ).
    sql( |DROP TABLE IF EXISTS { c_b }| ).

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

    " ================= CDC / KEYS_IUD =================================
    " The tier that shipped broken, in the mode that shipped broken. Logs the
    " KEY of a changed row and re-reads the values, so this exercises the
    " re-read and the coercion of every column type in the corpus.
    DELETE FROM zdelta_all. COMMIT WORK AND WAIT.
    reset_target( 'par_keys' ).
    seed( iv_from = 0 iv_count = 40 ).
    zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = 'par_keys' ).
    sql( |INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, | &&
         |log_enabled) VALUES ('par_keys','CDC','{ c_src }','{ c_keys }',true)| ).
    DATA(lv_pk) = zcl_erpl_rev_cdc=>provision(
      iv_target = 'par_keys' iv_source = c_src iv_keys = c_keys iv_mode = 'KEYS_IUD' ).
    ok( cond = xsdbool( lv_pk IS INITIAL )
        what = 'PARITY KEYS_IUD: provisioned' detail = lv_pk ).

    churn_iu( ).
    churn_delete( ).
    DATA(rk) = zcl_erpl_rev_cdc=>run( 'par_keys' ).
    reference( ).
    DATA(lv_dk) = drift( 'par_keys' ).
    ok( cond = xsdbool( rk-error IS INITIAL AND lv_dk = 0 )
        what = 'PARITY KEYS_IUD: matches a full load, cell by cell'
        detail = |err={ rk-error } drift={ lv_dk }: { drift_detail( 'par_keys' ) }| ).

    " The negative controls ride on this case, because they are about the DIFF
    " rather than about the method. A diff that returns zero proves nothing
    " until it has been shown to return non-zero: without these, the suite
    " would pass just as happily over two empty tables, a join on the wrong
    " keys, or an extension that never loaded.
    sql( |UPDATE par_keys SET sgtxt = 'tampered' WHERE belnr = '0000000020'| ).
    DATA(lv_c1) = drift( 'par_keys' ).
    ok( cond = xsdbool( lv_c1 = 1 )
        what = 'PARITY-CONTROL: one tampered cell is caught, and only that row'
        detail = |{ lv_c1 } row(s), expected 1: { drift_detail( 'par_keys' ) }| ).

    " A tampered KEY is the failure a value-only comparison misses: the row is
    " not changed, it is simultaneously missing and unexpected.
    sql( |UPDATE par_keys SET belnr = '9999999999' WHERE belnr = '0000000021'| ).
    DATA(lv_c2) = drift( 'par_keys' ).
    ok( cond = xsdbool( lv_c2 >= 3 )
        what = 'PARITY-CONTROL: a tampered key reads as removed plus added'
        detail = |{ lv_c2 } row(s): { drift_detail( 'par_keys' ) }| ).
    reset_target( 'par_keys' ).

    " ================= CDC / IMAGE_IUD ================================
    " The same claim through a different code path: the trigger logs the whole
    " row image, so the values come from the LOG rather than from a re-read.
    " A coercion bug in one path and not the other is exactly the kind of thing
    " a single-mode test cannot see.
    DELETE FROM zdelta_all. COMMIT WORK AND WAIT.
    reset_target( 'par_img' ).
    seed( iv_from = 0 iv_count = 40 ).
    zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = 'par_img' ).
    sql( |INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, | &&
         |log_enabled) VALUES ('par_img','CDC','{ c_src }','{ c_keys }',true)| ).
    DATA(lv_pi) = zcl_erpl_rev_cdc=>provision(
      iv_target = 'par_img' iv_source = c_src iv_keys = c_keys iv_mode = 'IMAGE_IUD' ).
    ok( cond = xsdbool( lv_pi IS INITIAL )
        what = 'PARITY IMAGE_IUD: provisioned' detail = lv_pi ).

    churn_iu( ).
    churn_delete( ).
    DATA(ri) = zcl_erpl_rev_cdc=>run( 'par_img' ).
    reference( ).
    DATA(lv_di) = drift( 'par_img' ).
    ok( cond = xsdbool( ri-error IS INITIAL AND lv_di = 0 )
        what = 'PARITY IMAGE_IUD: matches a full load, cell by cell'
        detail = |err={ ri-error } drift={ lv_di }: { drift_detail( 'par_img' ) }| ).
    reset_target( 'par_img' ).

    " ================= SNAPSHOT =======================================
    " No triggers at all: deletes are found by anti-joining the target against
    " a fresh read of the source. It claims all three operations, so it is held
    " to the same standard as the trigger tier.
    DELETE FROM zdelta_all. COMMIT WORK AND WAIT.
    reset_target( 'par_snap' ).
    seed( iv_from = 0 iv_count = 40 ).
    zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = 'par_snap' ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'par_snap' method = 'SNAPSHOT' source_from = c_src
      keys = c_keys cadence = 'manual' ) ).

    churn_iu( ).
    churn_delete( ).
    DATA(rs) = zcl_erpl_rev_delta=>run( 'par_snap' ).
    reference( ).
    DATA(lv_ds) = drift( 'par_snap' ).
    ok( cond = xsdbool( rs-error IS INITIAL AND lv_ds = 0 )
        what = 'PARITY SNAPSHOT: matches a full load, cell by cell'
        detail = |err={ rs-error } drift={ lv_ds }: { drift_detail( 'par_snap' ) }| ).
    reset_target( 'par_snap' ).

    " ================= WATERMARK ======================================
    " Held to a DIFFERENT claim, because it makes a different one. A watermark
    " reads rows whose change column moved; a row that was deleted has no
    " change column left to read. So:
    "
    "   after inserts and updates      -> exact parity, like everyone else
    "   after a physical delete        -> it MUST diverge
    "
    " The second half is the reason the trigger tier exists, written as a test
    " instead of as a sentence in a document. If it ever passes, either the
    " workload stopped deleting or the diff stopped comparing -- and both of
    " those have happened to this project before.
    DELETE FROM zdelta_all. COMMIT WORK AND WAIT.
    reset_target( 'par_wm' ).
    seed( iv_from = 0 iv_count = 40 ).
    zcl_erpl_rev_util=>replicate( iv_tab = c_src iv_target = 'par_wm' ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'par_wm' method = 'WATERMARK' source_from = c_src
      keys = c_keys chg_col = 'CHG_TSTAMP' wm_kind = 'NUMTS'
      safety_secs = 0 cadence = 'manual' ) ).

    churn_iu( ).
    DATA(rw) = zcl_erpl_rev_delta=>run( 'par_wm' ).
    reference( ).
    DATA(lv_dw) = drift( 'par_wm' ).
    ok( cond = xsdbool( rw-error IS INITIAL AND lv_dw = 0 )
        what = 'PARITY WATERMARK: matches a full load after inserts and updates'
        detail = |err={ rw-error } drift={ lv_dw }: { drift_detail( 'par_wm' ) }| ).

    churn_delete( ).
    zcl_erpl_rev_delta=>run( 'par_wm' ).
    reference( ).
    DATA(lv_blind) = drift( 'par_wm' ).
    ok( cond = xsdbool( lv_blind = 5 )
        what = 'PARITY WATERMARK: five deleted rows survive in the target, as it cannot see them'
        detail = |{ lv_blind } row(s) differ, expected exactly the 5 deleted: | &&
                 |{ drift_detail( 'par_wm' ) }| ).
    reset_target( 'par_wm' ).

    " --- leave nothing behind -------------------------------------------
    sql( |DROP TABLE IF EXISTS { c_b }| ).
    DELETE FROM zdelta_all.
    COMMIT WORK AND WAIT.

    out->write( |PARITY RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
