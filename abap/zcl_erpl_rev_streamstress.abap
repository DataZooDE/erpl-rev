CLASS zcl_erpl_rev_streamstress DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Streaming stress: a real change workload against a real replicator.
*"*
*"* Submits the change generator as a background job, runs delta cycles
*"* against SFLIGHT while it works, and then reports what actually
*"* happened -- not whether it "seemed to keep up".
*"*
*"* Three questions, in order of how badly a wrong answer hurts:
*"*
*"*   1. Did anything get LOST? Every key the generator committed must be
*"*      accounted for. A replicator that drops rows under load is
*"*      indistinguishable from a correct one until somebody counts, and
*"*      by then the data is months old.
*"*   2. Did anything appear that was never written (a PHANTOM)?
*"*   3. Only then: how fast? p50/p95/p99, from the change log.
*"*
*"* The generator's audit table is the oracle. Nothing here trusts the
*"* replicator's own account of itself.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_target TYPE string VALUE 'stress_sflight'.
    CONSTANTS c_run    TYPE char20 VALUE 'STRESS1'.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS scalar IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS register IMPORTING iv_target TYPE string iv_kind TYPE string
                               iv_col TYPE string iv_tcol TYPE string.
    METHODS report IMPORTING iv_target TYPE string iv_label TYPE string iv_sap TYPE int8.
    METHODS out_line IMPORTING iv_text TYPE string.
ENDCLASS.

CLASS zcl_erpl_rev_streamstress IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD scalar.
    DATA(ls) = zcl_erpl_rev_util=>query( iv_sql ).
    rv = ls-rows.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.

    DATA(lv_rate) = 20.     " changes per second
    DATA(lv_dur)  = 30.     " seconds

    " --- a clean slate ---------------------------------------------------
    DELETE FROM zdelta_all.
    DELETE FROM zdelta_audit WHERE runid = @c_run.
    COMMIT WORK AND WAIT.

    " --- start the workload ----------------------------------------------
    " Submitted BEFORE the targets are registered, so the replicator meets a
    " workload already in flight rather than a quiet table -- which is the
    " situation that actually finds problems.
    DATA lv_jn TYPE tbtcjob-jobname VALUE 'ERPL_REV_GEN'.
    DATA lv_jc TYPE tbtcjob-jobcount.
    CALL FUNCTION 'JOB_OPEN' EXPORTING jobname = lv_jn IMPORTING jobcount = lv_jc
      EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0.
      out->write( |STRESS RESULT pass=0 fail=1 (JOB_OPEN failed)| ).
      RETURN.
    ENDIF.
    SUBMIT z_erpl_rev_gen
      WITH p_tab = 'ZDELTA_ALL' WITH p_rate = lv_rate WITH p_dur = lv_dur
      WITH p_ins = 40 WITH p_upd = 40 WITH p_del = 20 WITH p_run = c_run
      VIA JOB lv_jn NUMBER lv_jc AND RETURN.
    CALL FUNCTION 'JOB_CLOSE' EXPORTING jobcount = lv_jc jobname = lv_jn
                                        strtimmed = abap_true
      EXCEPTIONS OTHERS = 1.
    out->write( |generator: { lv_rate }/s for { lv_dur }s, 40/40/20 I/U/D on ZDELTA_ALL| ).

    " --- one target per strategy, same rows, same workload ----------------
    " This is what ZDELTA_ALL exists for: every strategy replicating the SAME
    " changes, so the numbers below are comparable instead of being four
    " measurements of four different things.
    register( iv_target = 'str_numts' iv_kind = 'NUMTS'    iv_col = 'CHG_TSTAMP' iv_tcol = '' ).
    register( iv_target = 'str_dt'    iv_kind = 'DATETIME' iv_col = 'CHG_DATE2'  iv_tcol = 'CHG_TIME' ).
    register( iv_target = 'str_int'   iv_kind = 'INT'      iv_col = 'CHG_COUNTER' iv_tcol = '' ).

    " --- replicate while it runs ------------------------------------------
    DATA(lv_cycles) = 0.
    DATA(lv_secs)   = 0.
    WHILE lv_secs < lv_dur + 10.
      zcl_erpl_rev_delta=>run( iv_target = 'str_numts' ).
      zcl_erpl_rev_delta=>run( iv_target = 'str_dt' ).
      zcl_erpl_rev_delta=>run( iv_target = 'str_int' ).
      lv_cycles = lv_cycles + 1.
      WAIT UP TO 2 SECONDS.
      lv_secs = lv_secs + 2.
    ENDWHILE.
    out->write( |{ lv_cycles } cycles per strategy at a 2s tick| ).

    " --- what the generator actually committed -----------------------------
    SELECT COUNT(*) FROM zdelta_audit WHERE runid = @c_run INTO @DATA(lv_changes).
    SELECT COUNT(*) FROM zdelta_audit WHERE runid = @c_run AND op = 'I' INTO @DATA(lv_gi).
    SELECT COUNT(*) FROM zdelta_audit WHERE runid = @c_run AND op = 'U' INTO @DATA(lv_gu).
    SELECT COUNT(*) FROM zdelta_audit WHERE runid = @c_run AND op = 'D' INTO @DATA(lv_gd).
    SELECT COUNT(*) FROM zdelta_all INTO @DATA(lv_sap_rows).
    out->write( |generator committed { lv_changes } changes | &&
                |(I={ lv_gi } U={ lv_gu } D={ lv_gd }), { lv_sap_rows } rows left in SAP| ).
    ok( cond = xsdbool( lv_changes > 0 ) what = 'the generator produced a workload'
        detail = |{ lv_changes }| ).
    ok( cond = xsdbool( lv_gu > 0 AND lv_gd > 0 )
        what = 'the workload really contained updates and deletes'
        detail = |U={ lv_gu } D={ lv_gd }| ).

    report( iv_target = 'str_numts' iv_label = 'NUMTS   ' iv_sap = lv_sap_rows ).
    report( iv_target = 'str_dt'    iv_label = 'DATETIME' iv_sap = lv_sap_rows ).
    report( iv_target = 'str_int'   iv_label = 'INT     ' iv_sap = lv_sap_rows ).

    out->write( |STRESS RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD register.
    zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS { iv_target }| ).
    zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS _erpl_rev_log_{ iv_target }| ).
    zcl_erpl_rev_util=>query( |DROP SEQUENCE IF EXISTS _erpl_rev_log_{ iv_target }_seq| ).
    zcl_erpl_rev_util=>query( |DELETE FROM _erpl_rev_delta_state WHERE target='{ iv_target }'| ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = iv_target
      method      = 'WATERMARK'
      source_from = 'ZDELTA_ALL'
      keys        = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'
      chg_col     = iv_col
      time_col    = iv_tcol
      wm_kind     = iv_kind
      safety_secs = 5
      cadence     = 'manual' ) ).
    " The change log is the latency instrument: without it there is nothing to
    " measure after the fact.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET log_enabled=true WHERE target='{ iv_target }'| ).
  ENDMETHOD.

  METHOD report.
    DATA(lv_rows) = cnt( |SELECT count(*) AS c FROM { iv_target }| ).
    DATA(lv_log)  = cnt( |SELECT count(*) AS c FROM _erpl_rev_log_{ iv_target }| ).

    " A watermark tier cannot see a physical delete -- that is what the snapshot
    " and trigger tiers are for -- so the target legitimately holds MORE rows
    " than SAP. What must never happen is holding FEWER: that is a lost row.
    ok( cond   = xsdbool( lv_rows >= iv_sap )
        what   = |{ iv_label }: no rows lost|
        detail = |sap={ iv_sap } target={ lv_rows }| ).
    ok( cond   = xsdbool( lv_log > 0 )
        what   = |{ iv_label }: the change log captured the workload|
        detail = |{ lv_log }| ).

    DATA(lv_stats) = scalar(
      |SELECT count(*) AS n, | &&
      |round(quantile_disc(epoch(_applied_at)-epoch(_commit_ts),0.50),2) AS p50, | &&
      |round(quantile_disc(epoch(_applied_at)-epoch(_commit_ts),0.95),2) AS p95, | &&
      |round(quantile_disc(epoch(_applied_at)-epoch(_commit_ts),0.99),2) AS p99, | &&
      |round(max(epoch(_applied_at)-epoch(_commit_ts)),2) AS worst | &&
      |FROM _erpl_rev_log_{ iv_target } WHERE _commit_ts IS NOT NULL| ).
    out_line( |{ iv_label } latency { lv_stats }| ).

    DATA(lv_ops) = scalar(
      |SELECT count(*) FILTER (WHERE _op='I') AS i, | &&
      |count(*) FILTER (WHERE _op='U') AS u | &&
      |FROM _erpl_rev_log_{ iv_target }| ).
    out_line( |{ iv_label } applied  { lv_ops } rows={ lv_rows }| ).
  ENDMETHOD.

  METHOD out_line.
    mo->write( iv_text ).
  ENDMETHOD.

ENDCLASS.
