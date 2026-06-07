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
    METHODS m5_sflight.
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
        m5_sflight( ).
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
      keys = 'CLIENT,ID' chg_col = 'CHANGED_AT' wm_kind = 'NUMTS'
      wm_value = condense( |{ lv_seed }| ) safety_secs = 0 cadence = 'manual' ) ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 10 ) what = 'M1 baseline=10' ).

    " Inject: update 3 rows + insert 1 (4 rows now have chg_col > wm).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000001' ).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000002' ).
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000003' ).
    zcl_erpl_rev_deltadrv=>insert_wm( '0000000011' ).

    DATA(r1) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( cond = xsdbool( r1-error IS INITIAL ) what = 'M1 cycle ok' detail = r1-error ).
    ok( cond = xsdbool( r1-rows = 4 ) what = 'M1 applied=4' detail = |{ r1-rows }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 11 ) what = 'M1 count=11' ).
    ok( cond = has( iv_sql = |SELECT name FROM delta_wm WHERE id='0000000001'| iv_sub = 'touched' )
        what = 'M1 updated row carries new value' ).

    " Idempotency: nothing changed in SAP -> next cycle applies 0, data identical.
    DATA(r2) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( cond = xsdbool( r2-rows = 0 ) what = 'M1 idempotent re-run applies 0' detail = |{ r2-rows }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_wm| ) = 11 ) what = 'M1 count still 11' ).
  ENDMETHOD.

  METHOD m2_snapshot.
    " SNAPSHOT: insert + update + PHYSICAL DELETE in SAP; one cycle reflects all
    " three, and the deleted row is gone from the target (the watermark path can't
    " see physical deletes — only the snapshot anti-join can).
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = 'delta_snap' ). " baseline
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'delta_snap' method = 'SNAPSHOT' source_from = 'ZDELTA_WM'
      keys = 'CLIENT,ID' cadence = 'manual' ) ).
    DATA(lv_before) = cnt( |SELECT count(*) AS c FROM delta_snap| ).

    zcl_erpl_rev_deltadrv=>insert_wm( '0000000012' ).     " new
    zcl_erpl_rev_deltadrv=>touch_wm( '0000000004' ).      " changed
    zcl_erpl_rev_deltadrv=>delete_wm( '0000000005' ).     " hard delete

    DATA(r) = zcl_erpl_rev_delta=>run( 'delta_snap' ).
    ok( cond = xsdbool( r-error IS INITIAL ) what = 'M2 cycle ok' detail = r-error ).
    ok( cond = xsdbool( r-del = 1 ) what = 'M2 one delete detected' detail = |{ r-del }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap| ) = lv_before )
        what = 'M2 net count (+1 -1)' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap WHERE id='0000000005'| ) = 0 )
        what = 'M2 hard-deleted row absent from target' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM delta_snap WHERE id='0000000012'| ) = 1 )
        what = 'M2 inserted row present in target' ).
  ENDMETHOD.

  METHOD m3_changedoc.
    " CHANGEDOC / INSERT_ONLY are exercised against a REAL change document written
    " by BAPI_MATERIAL_SAVEDATA (the MM02 path -> CDHDR OBJECTCLAS='MATERIAL' +
    " CDPOS): CHANGEDOC re-reads MAKT by MATNR; INSERT_ONLY appends the new CDPOS
    " items (the CDHDR->CHANGENR->CDPOS 2-step). This requires Materials Management,
    " which is absent on the bare ABAP Platform developer trial. When unavailable we
    " SKIP (a note, not a fail) so the harness still proves M1/M2/M4 on real
    " transactions; the CHANGEDOC/INSERT_ONLY engine itself is exercised by the
    " server merge unit tests and runs on any MM-equipped (S/4) system unchanged.
    DATA lv_matnr TYPE matnr.
    DATA lv_maktx TYPE string.
    DATA lv_merr  TYPE string.
    zcl_erpl_rev_deltadrv=>change_material(
      IMPORTING ev_matnr = lv_matnr ev_maktx = lv_maktx ev_error = lv_merr ).
    IF lv_merr IS NOT INITIAL.
      mo->write( |M3 SKIP (change-doc): { lv_merr }| ).
      RETURN.
    ENDIF.

    zcl_erpl_rev_util=>replicate( iv_tab = 'MAKT' iv_target = 'makt_cd' iv_maxrows = 500 ).
    DATA(lv_hw) = zcl_erpl_rev_delta=>cdhdr_highwater( 'MATERIAL' ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'makt_cd' method = 'CHANGEDOC' source_from = 'MAKT'
      keys = 'MANDT,MATNR,SPRAS' wm_kind = 'DATETIME' wm_value = lv_hw
      extra = '{"objectclas":"MATERIAL"}' cadence = 'manual' ) ).
    DATA(rc) = zcl_erpl_rev_delta=>run( 'makt_cd' ).
    ok( cond = xsdbool( rc-error IS INITIAL ) what = 'M3 changedoc cycle ok' detail = rc-error ).
    ok( cond = xsdbool( rc-rows >= 1 ) what = 'M3 changedoc applied >=1' detail = |{ rc-rows }| ).
    ok( cond = has( iv_sql = |SELECT maktx FROM makt_cd WHERE maktx LIKE 'erpl delta%'|
                    iv_sub = lv_maktx )
        what = 'M3 changed material description landed in MAKT target' detail = lv_maktx ).
  ENDMETHOD.

  METHOD m4_orchestration.
    " Lease blocks an overlapping cycle; the granularity gate rejects sub-hourly on
    " a date-only column; due-detection + run_due drive a catch-up cycle.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now() WHERE target='delta_wm'| ).
    DATA(rl) = zcl_erpl_rev_delta=>run( 'delta_wm' ).
    ok( cond = xsdbool( rl-skipped = abap_true ) what = 'M4 lease blocks overlapping cycle' ).
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='IDLE' WHERE target='delta_wm'| ).

    " Granularity gate: micro cadence on a date-only watermark is rejected.
    DATA(lv_gate) = zcl_erpl_rev_delta=>register( VALUE #(
      target = 'gate_t' method = 'WATERMARK' source_from = 'ZDELTA_WM'
      keys = 'CLIENT,ID' chg_col = 'CHANGED_AT' wm_kind = 'DATE' cadence = 'micro:60' ) ).
    ok( cond = xsdbool( lv_gate IS NOT INITIAL )
        what = 'M4 granularity gate rejects micro on DATE' detail = lv_gate ).

    " Due + run_due catch-up: make delta_wm due (micro:1, last run 10s ago), change a
    " row, and let the orchestrator pick it up.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET cadence='micro:1', | &&
      |last_run_ts = now() - INTERVAL '10' SECOND WHERE target='delta_wm'| ).
    DATA(lt_due) = zcl_erpl_rev_delta=>due( ).
    ok( cond = xsdbool( line_exists( lt_due[ table_line = 'delta_wm' ] ) )
        what = 'M4 delta_wm reported due' ).

    zcl_erpl_rev_deltadrv=>touch_wm( '0000000006' ).
    DATA(lt_run) = zcl_erpl_rev_delta=>run_due( ).
    DATA(lv_hit) = abap_false.
    LOOP AT lt_run INTO DATA(ls) WHERE target = 'delta_wm'.
      IF ls-error IS INITIAL AND ls-rows >= 1. lv_hit = abap_true. ENDIF.
    ENDLOOP.
    ok( cond = lv_hit what = 'M4 run_due executes the due catch-up cycle' ).
  ENDMETHOD.

  METHOD m5_sflight.
    " The SFLIGHT demo scenario behind Z_ERPL_REV_DELTA_SFLIGHT: SNAPSHOT delta on
    " the flight-booking demo table — insert + update + physical delete, each
    " reflected after one cycle, on a recognizable standard SAP table. Uses a fixed
    " far-future flight date (2099-12-31) so the asserts are deterministic and the
    " demo flight is added then removed (SFLIGHT is left as it was).
    DATA: lv_c TYPE s_carr_id, lv_n TYPE s_conn_id, lv_d TYPE s_date.
    zcl_erpl_rev_deltadrv=>sflight_default(
      IMPORTING ev_carrid = lv_c ev_connid = lv_n ev_fldate = lv_d ).
    ok( cond = xsdbool( lv_c IS NOT INITIAL ) what = 'M5 SFLIGHT demo data present' ).
    IF lv_c IS INITIAL. RETURN. ENDIF.
    DATA(lv_demo) = CONV s_date( '20991231' ).

    " purge any leftover demo flights (FLDATE >= 2099-01-01) for a clean baseline,
    " then full-load + register SNAPSHOT
    zcl_erpl_rev_deltadrv=>sflight_purge_demo( ).
    zcl_erpl_rev_util=>replicate( iv_tab = 'SFLIGHT' iv_target = 'sflight' ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'sflight' method = 'SNAPSHOT' source_from = 'SFLIGHT'
      keys = 'MANDT,CARRID,CONNID,FLDATE' cadence = 'manual' ) ).
    DATA(lv_n0) = cnt( |SELECT count(*) AS c FROM sflight| ).

    " INSERT a demo flight -> one cycle -> present in DuckDB
    zcl_erpl_rev_deltadrv=>sflight_change( iv_kind = 'I' iv_carrid = lv_c iv_connid = lv_n iv_fldate = lv_demo ).
    DATA(ri) = zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( ri-error IS INITIAL ) what = 'M5 insert cycle ok' detail = ri-error ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight| ) = lv_n0 + 1 )
        what = 'M5 inserted flight reflected (count +1)' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight WHERE fldate='2099-12-31'| ) = 1 )
        what = 'M5 inserted flight present in DuckDB' ).

    " UPDATE the demo flight's price (+100) -> one cycle -> new price in DuckDB
    DATA(lv_p0) = cnt( |SELECT CAST(price AS INTEGER) AS c FROM sflight WHERE fldate='2099-12-31'| ).
    zcl_erpl_rev_deltadrv=>sflight_change( iv_kind = 'U' iv_carrid = lv_c iv_connid = lv_n iv_fldate = lv_demo ).
    zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( cnt( |SELECT CAST(price AS INTEGER) AS c FROM sflight WHERE fldate='2099-12-31'| ) = lv_p0 + 100 )
        what = 'M5 updated price reflected (+100)' detail = |{ lv_p0 }| ).

    " DELETE the demo flight -> one cycle -> gone from DuckDB (snapshot anti-join)
    zcl_erpl_rev_deltadrv=>sflight_change( iv_kind = 'D' iv_carrid = lv_c iv_connid = lv_n iv_fldate = lv_demo ).
    DATA(rd) = zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( rd-del = 1 ) what = 'M5 delete detected (del=1)' detail = |{ rd-del }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight WHERE fldate='2099-12-31'| ) = 0 )
        what = 'M5 deleted flight absent from DuckDB' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight| ) = lv_n0 )
        what = 'M5 count restored to baseline' ).

    " MASS operations (bulk demo flights, FLDATE >= 2099-01-01). N=25 for a fast test.
    DATA(lv_m) = 25.
    zcl_erpl_rev_deltadrv=>sflight_mass( iv_kind = 'I' iv_carrid = lv_c iv_connid = lv_n iv_count = lv_m ).
    zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight| ) = lv_n0 + lv_m )
        what = 'M5 mass insert reflected (+25)' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight WHERE fldate >= '2099-01-01'| ) = lv_m )
        what = 'M5 25 demo flights present in DuckDB' ).

    DATA(lv_s0) = cnt( |SELECT CAST(sum(price) AS INTEGER) AS c FROM sflight WHERE fldate >= '2099-01-01'| ).
    zcl_erpl_rev_deltadrv=>sflight_mass( iv_kind = 'U' iv_carrid = lv_c iv_connid = lv_n iv_count = lv_m ).
    zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( cnt( |SELECT CAST(sum(price) AS INTEGER) AS c FROM sflight WHERE fldate >= '2099-01-01'| )
                        = lv_s0 + lv_m * 100 )
        what = 'M5 mass update reflected (+100 x 25)' detail = |{ lv_s0 }| ).

    zcl_erpl_rev_deltadrv=>sflight_mass( iv_kind = 'D' iv_carrid = lv_c iv_connid = lv_n iv_count = lv_m ).
    DATA(rmdel) = zcl_erpl_rev_delta=>run( 'sflight' ).
    ok( cond = xsdbool( rmdel-del = lv_m ) what = 'M5 mass delete detected (del=25)' detail = |{ rmdel-del }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight WHERE fldate >= '2099-01-01'| ) = 0 )
        what = 'M5 all demo flights gone' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM sflight| ) = lv_n0 )
        what = 'M5 mass delete restored baseline' ).
  ENDMETHOD.

ENDCLASS.
