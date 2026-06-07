CLASS zcl_erpl_rev_cdctest DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS m1_delete_only.
ENDCLASS.

CLASS zcl_erpl_rev_cdctest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.
    TRY.
        m1_delete_only( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    out->write( |CDC RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD m1_delete_only.
    " Trigger-CDC delete-only on ZDELTA_WM (string keys CLIENT,ID — no type casting):
    " provision real HANA triggers on the source, physically delete rows, run one CDC
    " cycle, and assert the physical deletes are reflected in the DuckDB target — the
    " gap the watermark tier cannot see. Idempotent re-run is a no-op; teardown drops
    " the trigger/log/sequence (leaving ZDELTA_WM clean for the other suites).

    " Seed the source + full-load the DuckDB target.
    zcl_erpl_rev_deltadrv=>seed_wm( 10 ).
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'cdc_wm' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM cdc_wm| ) = 10 ) what = 'CDC baseline=10' ).

    " Provision delete-only triggers (self-cleans any leftovers from a prior run).
    DATA(lv_pe) = zcl_erpl_rev_cdc=>provision(
      iv_target = 'cdc_wm' iv_source = 'ZDELTA_WM' iv_keys = 'CLIENT,ID' iv_mode = 'DELETE_ONLY' ).
    ok( cond = xsdbool( lv_pe IS INITIAL ) what = 'CDC provision ok' detail = lv_pe ).

    " Physically delete two rows -> the AFTER DELETE trigger logs them.
    zcl_erpl_rev_deltadrv=>delete_wm( '0000000003' ).
    zcl_erpl_rev_deltadrv=>delete_wm( '0000000005' ).

    " One CDC cycle: read the log, apply, prune.
    DATA(r1) = zcl_erpl_rev_cdc=>run( 'cdc_wm' ).
    ok( cond = xsdbool( r1-error IS INITIAL ) what = 'CDC cycle ok' detail = r1-error ).
    ok( cond = xsdbool( r1-del = 2 ) what = 'CDC two physical deletes captured' detail = |{ r1-del }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM cdc_wm| ) = 8 ) what = 'CDC count 10->8' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM cdc_wm WHERE id='0000000003'| ) = 0 )
        what = 'CDC deleted row 3 absent from target' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM cdc_wm WHERE id='0000000005'| ) = 0 )
        what = 'CDC deleted row 5 absent from target' ).

    " Idempotent re-run: no new log rows -> nothing applied, target unchanged.
    DATA(r2) = zcl_erpl_rev_cdc=>run( 'cdc_wm' ).
    ok( cond = xsdbool( r2-applied = abap_false ) what = 'CDC idempotent re-run is a no-op'
        detail = |applied={ r2-applied } del={ r2-del }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM cdc_wm| ) = 8 ) what = 'CDC count still 8' ).

    " Teardown: drop trigger + log + sequence (trigger first, so ZDELTA_WM stays usable).
    DATA(lv_te) = zcl_erpl_rev_cdc=>teardown( 'cdc_wm' ).
    ok( cond = xsdbool( lv_te IS INITIAL ) what = 'CDC teardown ok (no orphan objects)' detail = lv_te ).
  ENDMETHOD.

ENDCLASS.
