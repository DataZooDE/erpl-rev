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
    METHODS load_oracles.
    METHODS key_expr IMPORTING iv_alias TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS same_key IMPORTING iv_a TYPE string iv_b TYPE string RETURNING VALUE(rv) TYPE string.
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

    " --- settle ------------------------------------------------------------
    " The cycle ceiling is read_start minus the safety window, so a change
    " committed in the workload's final second is deliberately NOT eligible
    " until a cycle starts a safety window later. Without this wait the
    " anti-joins below would report the engine's correct behaviour as loss.
    WAIT UP TO 10 SECONDS.
    zcl_erpl_rev_delta=>run( iv_target = 'str_numts' ).
    zcl_erpl_rev_delta=>run( iv_target = 'str_dt' ).
    zcl_erpl_rev_delta=>run( iv_target = 'str_int' ).

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

    load_oracles( ).

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

    " LOSS, by key, not by count. Comparing counts passes for a target holding
    " the right NUMBER of the wrong rows -- and under a workload that inserts
    " and deletes at the same time, the counts drift into agreement by
    " accident. So: every key still present in SAP must be present in the
    " target. This is an anti-join against the real source, evaluated in the
    " engine, over the two tables that were loaded side by side.
    DATA(lv_lost) = cnt(
      |SELECT count(*) AS c FROM stress_truth t WHERE NOT EXISTS (| &&
      |SELECT 1 FROM { iv_target } x WHERE | && same_key( iv_a = 'x' iv_b = 't' ) && |)| ).
    ok( cond   = xsdbool( lv_lost = 0 )
        what   = |{ iv_label }: no rows lost|
        detail = |{ lv_lost } key(s) in SAP are missing from the target | &&
                 |(sap={ iv_sap } target={ lv_rows })| ).

    " PHANTOMS, the other direction. A watermark tier cannot see a physical
    " delete, so a target key absent from SAP is legitimate EXACTLY when the
    " generator deleted it. A key that is in neither the source nor the
    " generator's audit was invented by the pipeline.
    DATA(lv_matched) = cnt(
      |SELECT count(*) AS c FROM { iv_target } x JOIN stress_audit a ON | &&
      |trim(a.keyval) = { key_expr( 'x' ) }| ).
    IF lv_matched = 0 AND lv_rows > 0.
      " Say so instead of reporting every row as a phantom: an audit key that
      " no longer renders the way the target's key columns do is a broken
      " oracle, and a broken oracle that fails LOUDLY beats one that indicts
      " the engine.
      ok( cond = abap_false what = |{ iv_label }: the audit oracle still matches the target's keys|
          detail = |no target row matches any audit keyval -- key rendering drifted| ).
    ELSE.
      DATA(lv_phantom) = cnt(
        |SELECT count(*) AS c FROM { iv_target } x | &&
        |WHERE NOT EXISTS (SELECT 1 FROM stress_truth t WHERE | &&
        same_key( iv_a = 'x' iv_b = 't' ) && |) | &&
        |AND NOT EXISTS (SELECT 1 FROM stress_audit a | &&
        |WHERE trim(a.keyval) = { key_expr( 'x' ) })| ).
      ok( cond   = xsdbool( lv_phantom = 0 )
          what   = |{ iv_label }: no phantom rows|
          detail = |{ lv_phantom } target key(s) the generator never wrote| ).
    ENDIF.

    " STALE PAYLOAD. A key can be present and still wrong: delivered once on
    " its insert and never refreshed by any of the updates that followed.
    " Neither anti-join above can see that, and it is the failure a customer
    " notices last.
    DATA(lv_stale) = cnt(
      |SELECT count(*) AS c FROM stress_truth t JOIN { iv_target } x ON | &&
      same_key( iv_a = 'x' iv_b = 't' ) && | WHERE x.dmbtr IS DISTINCT FROM t.dmbtr| ).
    ok( cond   = xsdbool( lv_stale = 0 )
        what   = |{ iv_label }: no stale payload|
        detail = |{ lv_stale } row(s) carry an older value than SAP does| ).

    ok( cond   = xsdbool( lv_log > 0 )
        what   = |{ iv_label }: the change log captured the workload|
        detail = |{ lv_log }| ).

    " ONE SAMPLE PER CHANGE, at its first apply -- the same definition the
    " server's latency view uses. The safety overlap re-reads recent rows on
    " purpose, so a change is logged several times with ever-later apply times;
    " counting every log row measures the overlap window rather than the
    " pipeline, and reported this five times slower than it is.
    DATA(lv_stats) = scalar(
      |SELECT count(*) AS n, | &&
      |round(quantile_disc(lat,0.50),2) AS p50, | &&
      |round(quantile_disc(lat,0.95),2) AS p95, | &&
      |round(quantile_disc(lat,0.99),2) AS p99, | &&
      |round(max(lat),2) AS worst FROM (| &&
      |SELECT epoch(min(_applied_at))-epoch(_commit_ts) AS lat | &&
      |FROM _erpl_rev_log_{ iv_target } WHERE _commit_ts IS NOT NULL | &&
      |GROUP BY belnr, _commit_ts)| ).
    out_line( |{ iv_label } latency { lv_stats }| ).

    DATA(lv_ops) = scalar(
      |SELECT count(*) FILTER (WHERE _op='I') AS i, | &&
      |count(*) FILTER (WHERE _op='U') AS u | &&
      |FROM _erpl_rev_log_{ iv_target }| ).
    out_line( |{ iv_label } applied  { lv_ops } rows={ lv_rows }| ).
  ENDMETHOD.

  METHOD load_oracles.
    " Both oracles are pulled into the engine by an ordinary full load, AFTER
    " the workload has finished and the deltas have settled. Comparing them
    " with the delta targets is then one SQL statement each, in one place,
    " instead of shipping key sets back into ABAP to be diffed in a loop.
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_ALL' iv_target = 'stress_truth'
                                  iv_record = abap_false ).
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_AUDIT' iv_target = 'stress_audit'
                                  iv_where = |RUNID = '{ c_run }'|
                                  iv_record = abap_false ).
    out_line( |oracles: stress_truth={ cnt( 'SELECT count(*) AS c FROM stress_truth' ) } | &&
              |stress_audit={ cnt( 'SELECT count(*) AS c FROM stress_audit' ) }| ).
  ENDMETHOD.

  METHOD key_expr.
    " The generator writes its audit key as BUKRS/BELNR/GJAHR/BUZEI. Rendering
    " it here from the target's own key columns is what makes the phantom
    " check a real comparison rather than a count.
    " The pipe is the string-template delimiter, so SQL's concatenation
    " operator is spelled with backtick literals rather than escaped inside
    " one: || in a template is a syntax error, and \|\| is unreadable.
    DATA(lc) = ` || '/' || `.
    rv = |{ iv_alias }.bukrs| && lc && |{ iv_alias }.belnr| && lc &&
         |{ iv_alias }.gjahr| && lc && |{ iv_alias }.buzei|.
  ENDMETHOD.

  METHOD same_key.
    rv = |{ iv_a }.client = { iv_b }.client AND { iv_a }.bukrs = { iv_b }.bukrs | &&
         |AND { iv_a }.belnr = { iv_b }.belnr AND { iv_a }.gjahr = { iv_b }.gjahr | &&
         |AND { iv_a }.buzei = { iv_b }.buzei|.
  ENDMETHOD.

  METHOD out_line.
    mo->write( iv_text ).
  ENDMETHOD.

ENDCLASS.
