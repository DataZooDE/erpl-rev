CLASS zcl_erpl_rev_wmtest DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* The watermark correctness suite.
*"*
*"* These are the cases the delta engine used to get wrong. Each one is
*"* DETERMINISTIC: the losses they cover are races, and a test that has to win a
*"* race to go red is a gate that passes by luck on a quiet system and blames the
*"* system when it fails on a busy one.
*"*
*"* So instead of a writer storm, the timing is CONSTRUCTED: rows are posted with
*"* chosen change-column values, which is exactly what a late commit looks like to
*"* the reader. The storm still exists as the broad regression, in its own lane.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS wm  IMPORTING iv_target TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS m1_late_commit_is_not_lost.
    METHODS m2_date_never_reads_today.
    METHODS m3_datetime_pair.
    METHODS m4_load_types.
ENDCLASS.

CLASS zcl_erpl_rev_wmtest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD wm.
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT wm_value AS v FROM _erpl_rev_delta_state WHERE target='{ iv_target }'| ).
    rv = ls-rows.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.
    TRY.
        m1_late_commit_is_not_lost( ).
        m2_date_never_reads_today( ).
        m3_datetime_pair( ).
        m4_load_types( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    out->write( |WM RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD m1_late_commit_is_not_lost.
    " THE D1 GATE, made deterministic.
    "
    " A row whose change timestamp is BELOW the maximum a cycle observes, but
    " which becomes visible only after that cycle read, is exactly the shape of a
    " transaction that was open across the read. Before the fix the watermark
    " advanced to the observed maximum and that row sat below the next floor
    " forever.
    "
    " Constructed rather than raced: post the high row, run a cycle, then post the
    " low row and run another. If the watermark advanced to the high row's value
    " the low row can never arrive; if it advanced to the read-start ceiling with
    " the safety window subtracted, it does.
    zcl_erpl_rev_deltadrv=>seed_wm( ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'zdelta_wm_late'
      method      = 'WATERMARK'
      source_from = 'ZDELTA_WM'
      keys        = 'CLIENT,ID'
      chg_col     = 'CHANGED_AT'
      wm_kind     = 'NUMTS'
      safety_secs = 120
      cadence     = 'manual' ) ).

    " A change well in the past, so the first cycle certainly carries it.
    zcl_erpl_rev_deltadrv=>insert_wm( iv_id = 'LATE_HI' iv_offset_secs = -600 ).
    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_wm_late' ).
    DATA(lv_after_first) = wm( 'zdelta_wm_late' ).

    " Now the straggler: an OLDER timestamp appearing after that cycle read. This
    " is what a long-running transaction looks like from the outside.
    zcl_erpl_rev_deltadrv=>insert_wm( iv_id = 'LATE_LO' iv_offset_secs = -30 ).
    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_wm_late' ).

    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_wm_late | &&
                             |WHERE id='LATE_LO'| ) = 1 )
        what = 'a late-arriving row below the observed maximum is delivered'
        detail = |watermark after the first cycle was { lv_after_first }| ).

    " ...and the run statistics make the overlap visible rather than hiding it.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM erpl_rev_run_stats | &&
                             |WHERE target='zdelta_wm_late' AND rows_read >= rows_applied| ) >= 1 )
        what = 're-delivered rows show as rows_read >= rows_applied' ).
  ENDMETHOD.

  METHOD m2_date_never_reads_today.
    " A DATE watermark must read COMPLETE days only. A cycle at 23:00 that
    " advances the watermark to today loses everything posted between 23:00 and
    " midnight -- silently, and every single day.
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'zdelta_d'
      method      = 'WATERMARK'
      source_from = 'ZDELTA_D'
      keys        = 'CLIENT,ID'
      chg_col     = 'CHANGED_ON'
      wm_kind     = 'DATE'
      cadence     = 'nightly' ) ).

    zcl_erpl_rev_deltadrv=>seed_dates( ).   " yesterday, today
    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_d' ).

    DATA(lv_today) = |{ sy-datum(4) }-{ sy-datum+4(2) }-{ sy-datum+6(2) }|.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_d | &&
                             |WHERE changed_on = DATE '{ lv_today }'| ) = 0 )
        what = `rows dated today are not read`
        detail = lv_today ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_d | &&
                             |WHERE changed_on < DATE '{ lv_today }'| ) > 0 )
        what = 'complete days are read' ).
    ok( cond = xsdbool( wm( 'zdelta_d' ) NS CONV string( sy-datum ) )
        what = 'the watermark never reaches today'
        detail = wm( 'zdelta_d' ) ).
  ENDMETHOD.

  METHOD m3_datetime_pair.
    " A DATS + TIMS pair, compared as one value. The failure mode lives in the
    " generated ABAP predicate -- literal quoting and operator precedence -- which
    " is why this is proven here and not only in a unit test.
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'zdelta_dt'
      method      = 'WATERMARK'
      source_from = 'ZDELTA_DT'
      keys        = 'CLIENT,ID'
      chg_col     = 'CHANGED_ON'
      time_col    = 'CHANGED_AT'
      wm_kind     = 'DATETIME'
      cadence     = 'manual' ) ).

    zcl_erpl_rev_deltadrv=>seed_datetimes( ).   " incl. 23:59:59 and 00:00:00
    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_dt' ).

    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_dt| ) > 0 )
        what = 'a DATS+TIMS pair target replicates at all' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_dt | &&
                             |WHERE changed_at = TIME '23:59:59'| ) = 1 )
        what = 'a row at 23:59:59 is not lost across the day boundary' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_dt | &&
                             |WHERE changed_at = TIME '00:00:00'| ) = 1 )
        what = 'a row at midnight is not lost' ).
  ENDMETHOD.

  METHOD m4_load_types.
    " I adopts a position without moving data; D then picks up only what is new;
    " F repairs data without touching the position.
    DATA(lv_before) = cnt( |SELECT count(*) AS c FROM zdelta_wm_late| ).

    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_wm_late' iv_load_type = 'I' ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM zdelta_wm_late| ) = lv_before )
        what = 'load type I transfers no rows' ).

    DATA(lv_wm) = wm( 'zdelta_wm_late' ).
    zcl_erpl_rev_delta=>run( iv_target = 'zdelta_wm_late' iv_load_type = 'F' ).
    ok( cond = xsdbool( wm( 'zdelta_wm_late' ) = lv_wm )
        what = 'load type F repairs data without moving the watermark'
        detail = |{ lv_wm } -> { wm( 'zdelta_wm_late' ) }| ).
  ENDMETHOD.

ENDCLASS.
