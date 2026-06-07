CLASS zcl_erpl_rev_deltatest DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    "! Integer value of a single-cell DuckDB query.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    "! True if a query's rows JSON contains a substring (cheap value assertion).
    METHODS has IMPORTING iv_sql TYPE string iv_sub TYPE string RETURNING VALUE(rv) TYPE abap_bool.
    METHODS m1_watermark.
    METHODS m2_snapshot.
    METHODS m3_changedoc.
    METHODS m4_orchestration.
ENDCLASS.

CLASS zcl_erpl_rev_deltatest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD has.
    DATA(ls) = zcl_erpl_rev_util=>query( iv_sql ).
    rv = xsdbool( ls-error IS INITIAL AND ls-rows CS iv_sub ).
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.
    TRY.
        m1_watermark( ).
        m2_snapshot( ).
        m3_changedoc( ).
        m4_orchestration( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    out->write( |DELTA RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD m1_watermark.
    " WATERMARK: a real Open SQL change to ZDELTA_WM is merged by chg_col > wm; a
    " re-run with no new change is a no-op (idempotent); the watermark advances.
    DATA(lv_seed) = zcl_erpl_rev_deltadrv=>seed_wm( 10 ).            " 10 rows @ seed ts
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'delta_wm' ). " baseline (+PK)
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'delta_wm' method = 'WATERMARK' source_from = 'ZDELTA_WM'
      keys = 'MANDT,ID' chg_col = 'CHANGED_AT' wm_kind = 'NUMTS'
      wm_value = condense( |{ lv_seed }| ) safety_secs = 0 cadence = 'manual' ) ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 10 ), 'M1 baseline=10' ).

    " Inject: update 3 rows + insert 1 (4 rows now have chg_col > wm).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000001' ).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000002' ).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000003' ).
    zcl_erpl_rev_deltadrv=>insert_wm( '0000000011' ).

    DATA(r1) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( xsdbool( r1-error IS INITIAL ), 'M1 cycle ok', r1-error ).
    ok( xsdbool( r1-rows = 4 ), 'M1 applied=4', |{ r1-rows }| ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 11 ), 'M1 count=11' ).
    ok( has( iv_sql = |SELECT name FROM delta_wm WHERE id='0000000001'| iv_sub = 'touched' ),
        'M1 updated row carries new value' ).

    " Idempotency: nothing changed in SAP -> next cycle applies 0, data identical.
    DATA(r2) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( xsdbool( r2-rows = 0 ), 'M1 idempotent re-run applies 0', |{ r2-rows }| ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 11 ), 'M1 count still 11' ).
  ENDMETHOD.

  METHOD m2_snapshot.
    " SNAPSHOT: insert + update + PHYSICAL DELETE in SAP; one cycle reflects all
    " three, and the deleted row is gone from the target (the watermark path can't
    " see physical deletes — only the snapshot anti-join can).
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'delta_snap' ). " baseline
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'delta_snap' method = 'SNAPSHOT' source_from = 'ZDELTA_WM'
      keys = 'MANDT,ID' cadence = 'manual' ) ).
    DATA(lv_before) = cnt( |SELECT count(*) AS c FROM delta_snap| ).

    zcl_erpl_rev_deltadrv=>insert_wm( '0000000012' ).     " new
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000004' ).      " changed
    zcl_erpl_rev_deltadrv=>delete_wm( '0000000005' ).     " hard delete

    DATA(r) = zcl_erpl_rev_delta=>run( 'delta_snap' ).
    ok( xsdbool( r-error IS INITIAL ), 'M2 cycle ok', r-error ).
    ok( xsdbool( r-del = 1 ), 'M2 one delete detected', |{ r-del }| ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap| ) = lv_before ), 'M2 net count (+1 -1)' ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap WHERE id='0000000005'| ) = 0 ),
        'M2 hard-deleted row absent from target' ).
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap WHERE id='0000000012'| ) = 1 ),
        'M2 inserted row present in target' ).
  ENDMETHOD.

  METHOD m3_changedoc.
    " A REAL material change (BAPI_MATERIAL_SAVEDATA = the MM02 path) writes a
    " genuine change document (CDHDR OBJECTCLAS='MATERIAL' + CDPOS). CHANGEDOC
    " re-reads MAKT by MATNR; INSERT_ONLY appends the new CDPOS items (2-step).
    zcl_erpl_rev_util=>replicate( iv_tab = 'MAKT' iv_target = 'makt_cd' iv_maxrows = 500 ).
    zcl_erpl_rev_util=>replicate( iv_tab = 'CDPOS' iv_target = 'cdpos_io' iv_maxrows = 10 ).
    DATA(lv_hw) = zcl_erpl_rev_delta=>cdhdr_highwater( 'MATERIAL' ).   " before the change

    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'makt_cd' method = 'CHANGEDOC' source_from = 'MAKT'
      keys = 'MANDT,MATNR,SPRAS' wm_kind = 'DATETIME' wm_value = lv_hw
      extra = '{"objectclas":"MATERIAL"}' cadence = 'manual' ) ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'cdpos_io' method = 'INSERT_ONLY' source_from = 'CDPOS'
      keys = 'MANDT,OBJECTCLAS,OBJECTID,CHANGENR,TABNAME,TABKEY,FNAME,CHNGIND'
      wm_kind = 'DATETIME' wm_value = lv_hw
      extra = '{"objectclas":"MATERIAL"}' cadence = 'manual' ) ).
    DATA(lv_cdpos0) = cnt( |SELECT count(*) AS c FROM cdpos_io| ).

    DATA lv_matnr TYPE matnr.
    DATA lv_maktx TYPE string.
    DATA lv_merr  TYPE string.
    zcl_erpl_rev_deltadrv=>change_material(
      IMPORTING ev_matnr = lv_matnr ev_maktx = lv_maktx ev_error = lv_merr ).
    ok( xsdbool( lv_merr IS INITIAL ), 'M3 BAPI material change committed', lv_merr ).
    IF lv_merr IS NOT INITIAL. RETURN. ENDIF.

    " CHANGEDOC: the new (unique, timestamped) MAKT description must appear.
    DATA(rc) = zcl_erpl_rev_delta=>run( 'makt_cd' ).
    ok( xsdbool( rc-error IS INITIAL ), 'M3 changedoc cycle ok', rc-error ).
    ok( xsdbool( rc-rows >= 1 ), 'M3 changedoc applied >=1', |{ rc-rows }| ).
    DATA lv_sub TYPE string.
    lv_sub = lv_maktx.
    ok( has( iv_sql = |SELECT maktx FROM makt_cd WHERE maktx LIKE 'erpl delta%' ORDER BY maktx DESC|
             iv_sub = lv_sub ),
        'M3 changed material description landed in MAKT target', lv_maktx ).

    " INSERT_ONLY (CDHDR->CHANGENR->CDPOS): the target grows; a re-run adds no dupes.
    DATA(ri) = zcl_erpl_rev_delta=>run( 'cdpos_io' ).
    ok( xsdbool( ri-error IS INITIAL ), 'M3 insert_only cycle ok', ri-error ).
    DATA(lv_cdpos1) = cnt( |SELECT count(*) AS c FROM cdpos_io| ).
    ok( xsdbool( lv_cdpos1 > lv_cdpos0 ), 'M3 insert_only grew CDPOS target',
        |{ lv_cdpos0 }->{ lv_cdpos1 }| ).
    zcl_erpl_rev_delta=>run( 'cdpos_io' ).               " re-run, same window
    ok( xsdbool( cnt( |SELECT count(*) AS c FROM cdpos_io| ) = lv_cdpos1 ),
        'M3 insert_only idempotent (no dupes on re-run)' ).
  ENDMETHOD.

  METHOD m4_orchestration.
    " Lease blocks an overlapping cycle; the granularity gate rejects sub-hourly on
    " a date-only column; due-detection + run_due drive a catch-up cycle.
    " Lease: simulate a fresh RUNNING lease, assert run() skips.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now() WHERE target='delta_wm'| ).
    DATA(rl) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( xsdbool( rl-skipped = abap_true ), 'M4 lease blocks overlapping cycle' ).
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='IDLE' WHERE target='delta_wm'| ).

    " Granularity gate: micro cadence on a date-only watermark is rejected.
    DATA(lv_gate) = zcl_erpl_rev_delta=>register( VALUE #(
      target = 'gate_t' method = 'WATERMARK' source_from = 'ZDELTA_WM'
      keys = 'MANDT,ID' chg_col = 'CHANGED_AT' wm_kind = 'DATE' cadence = 'micro:60' ) ).
    ok( xsdbool( lv_gate IS NOT INITIAL ), 'M4 granularity gate rejects micro on DATE', lv_gate ).

    " Due + run_due catch-up: make delta_wm due (micro:1, last run 10s ago), change a
    " row, and let the orchestrator pick it up.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET cadence='micro:1', | &&
      |last_run_ts = now() - INTERVAL '10' SECOND WHERE target='delta_wm'| ).
    DATA(lt_due) = zcl_erpl_rev_delta=>due( ).
    ok( xsdbool( line_exists( lt_due[ table_line = 'delta_wm' ] ) ), 'M4 delta_wm reported due' ).

    zcl_erpl_rev_deltadrv=>touch_wm( '0000000006' ).
    DATA(lt_run) = zcl_erpl_rev_delta=>run_due( ).
    DATA(lv_hit) = abap_false.
    LOOP AT lt_run INTO DATA(ls) WHERE target = 'delta_wm'.
      IF ls-error IS INITIAL AND ls-rows >= 1. lv_hit = abap_true. ENDIF.
    ENDLOOP.
    ok( lv_hit, 'M4 run_due executes the due catch-up cycle' ).
  ENDMETHOD.

ENDCLASS.
