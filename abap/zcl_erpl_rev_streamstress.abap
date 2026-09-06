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
    DATA: mv_cycle_err TYPE i, mv_cycle_skip TYPE i, mv_last_err TYPE string.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS scalar IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS register IMPORTING iv_target TYPE string iv_kind TYPE string
                               iv_col TYPE string iv_tcol TYPE string
                               iv_units TYPE i DEFAULT 0.
    METHODS report IMPORTING iv_target TYPE string iv_label TYPE string iv_sap TYPE int8.
    METHODS load_oracles.
    METHODS job_status IMPORTING iv_name TYPE tbtcjob-jobname iv_count TYPE tbtcjob-jobcount
                         RETURNING VALUE(rv_status) TYPE btcstatus.
    METHODS cycle IMPORTING iv_target TYPE string.
    " Two different renderings of "the key", on purpose, and they are not
    " interchangeable: key_expr builds the AUDIT's key format
    " (BUKRS/BELNR/GJAHR/BUZEI, no client) to match a keyval; key_cols lists the
    " TABLE's key columns. The latency query grouped by the first and so
    " collapsed distinct changes that differ only by client.
    METHODS key_expr IMPORTING iv_alias TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS key_cols RETURNING VALUE(rv) TYPE string.
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
    " A counter kind ignores safety_secs -- there is no clock to subtract from --
    " and uses safety_units instead. Registered with zero, the watermark jumped
    " to exactly max(staged), so any counter allocated before but committed
    " after the read fell below the next floor and was lost. That is real loss,
    " caused by the harness's own registration, and it would have sent someone
    " hunting a bug in the engine. Sized to the burst: one second of changes.
    register( iv_target = 'str_int'   iv_kind = 'INT'      iv_col = 'CHG_COUNTER' iv_tcol = ''
              iv_units = lv_rate ).

    " --- replicate while it runs ------------------------------------------
    " Cycle until the generator's JOB is finished, not until a counter says so.
    " The loop used to add 2 to a nominal clock per iteration while each
    " iteration also ran three real cycles, so the elapsed time and the budget
    " were unrelated -- and if the generator committed anything after the last
    " cycle, the oracles below would report the engine's correct behaviour as
    " loss. The job's terminal status is the only thing that says the workload
    " is over.
    DATA(lv_cycles) = 0.
    DATA lv_status TYPE btcstatus.
    DO 300 TIMES.
      cycle( 'str_numts' ).
      cycle( 'str_dt' ).
      cycle( 'str_int' ).
      lv_cycles = lv_cycles + 1.
      lv_status = job_status( iv_name = lv_jn iv_count = lv_jc ).
      IF lv_status = 'F' OR lv_status = 'A'. EXIT. ENDIF.
      WAIT UP TO 2 SECONDS.
    ENDDO.
    out->write( |{ lv_cycles } cycles per strategy, generator status { lv_status }| ).
    ok( cond = xsdbool( lv_status = 'F' )
        what = 'the generator ran to completion'
        detail = |status { lv_status } (F=finished, A=aborted)| ).

    " --- settle ------------------------------------------------------------
    " The cycle ceiling is read_start minus the safety window, so a change
    " committed in the workload's final second is deliberately NOT eligible
    " until a cycle starts a safety window later. Only now that the workload is
    " provably over does a fixed wait mean anything.
    WAIT UP TO 10 SECONDS.
    cycle( 'str_numts' ).
    cycle( 'str_dt' ).
    cycle( 'str_int' ).

    " A cycle that never ran is not data loss, and the oracle must be able to
    " tell the two apart -- it could not, because every run()'s error and
    " skipped flag were thrown away.
    ok( cond = xsdbool( mv_cycle_err = 0 )
        what = 'every cycle ran'
        detail = |{ mv_cycle_err } cycle(s) failed, last: { mv_last_err }| ).
    ok( cond = xsdbool( mv_cycle_skip = 0 )
        what = 'no cycle was skipped for a held lease'
        detail = |{ mv_cycle_skip } cycle(s) skipped| ).

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
      safety_units = iv_units
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
    " count of target ROWS that match, not of target-audit PAIRS. The join
    " counted pairs, and the audit holds one row per CHANGE, so a document
    " changed five times contributed five matches: on an update-heavy workload
    " the ratio ran into the hundreds of percent and the guard could never fire.
    DATA(lv_matched) = cnt(
      |SELECT count(*) AS c FROM { iv_target } x WHERE EXISTS (| &&
      |SELECT 1 FROM stress_audit a WHERE trim(a.keyval) = { key_expr( 'x' ) })| ).
    " A RATIO, not a zero test. Firing only when NOTHING matches meant that a
    " subset rendering differently -- one field padded another way -- left most
    " keys matching and reported the rest as phantoms: the engine indicted for
    " an oracle problem, which is the exact thing this guard exists to prevent.
    " Most target rows should match the audit; a workload that touches most of
    " the key space leaves little room for a legitimate shortfall.
    DATA(lv_ratio) = COND i( WHEN lv_rows = 0 THEN 100 ELSE lv_matched * 100 / lv_rows ).
    " Reported, never branched on. Making it a branch meant that when the guard
    " DID fire it skipped the phantom and missing anti-joins -- so a real engine
    " defect was reported as a harness problem and the actual checks never ran.
    " The oracle's health is a diagnostic printed beside the verdict, not a
    " reason to withhold the verdict.
    ok( cond   = xsdbool( lv_rows = 0 OR lv_ratio >= 80 )
        what   = |{ iv_label }: the audit oracle still matches the target's keys|
        detail = |only { lv_ratio }% of target rows match an audit key -- rendering drifted| ).
    DATA(lv_phantom) = cnt(
      |SELECT count(*) AS c FROM { iv_target } x | &&
      |WHERE NOT EXISTS (SELECT 1 FROM stress_truth t WHERE | &&
      same_key( iv_a = 'x' iv_b = 't' ) && |) | &&
      |AND NOT EXISTS (SELECT 1 FROM stress_audit a | &&
      |WHERE trim(a.keyval) = { key_expr( 'x' ) })| ).
    ok( cond   = xsdbool( lv_phantom = 0 )
        what   = |{ iv_label }: no phantom rows|
        detail = |{ lv_phantom } target key(s) the generator never wrote| ).

    " STALE PAYLOAD. A key can be present and still wrong: delivered once on
    " its insert and never refreshed by any of the updates that followed.
    " Neither anti-join above can see that, and it is the failure a customer
    " notices last.
    " Every payload column the generator maintains, not just DMBTR: a pipeline
    " that carried one column and dropped another would pass a single-column
    " check while replicating rubbish.
    DATA(lv_stale) = cnt(
      |SELECT count(*) AS c FROM stress_truth t JOIN { iv_target } x ON | &&
      same_key( iv_a = 'x' iv_b = 't' ) &&
      | WHERE x.dmbtr IS DISTINCT FROM t.dmbtr | &&
      |OR x.sgtxt IS DISTINCT FROM t.sgtxt | &&
      |OR x.chg_counter IS DISTINCT FROM t.chg_counter | &&
      |OR x.chg_tstamp IS DISTINCT FROM t.chg_tstamp| ).
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
      |GROUP BY { key_cols( ) }, _commit_ts)| ).
    out_line( |{ iv_label } latency { lv_stats }| ).

    DATA(lv_ops) = scalar(
      |SELECT count(*) FILTER (WHERE _op='I') AS i, | &&
      |count(*) FILTER (WHERE _op='U') AS u | &&
      |FROM _erpl_rev_log_{ iv_target }| ).
    out_line( |{ iv_label } applied  { lv_ops } rows={ lv_rows }| ).
  ENDMETHOD.

  METHOD cycle.
    " One cycle, with its verdict kept. run() reports an error and a skipped
    " flag; both used to be discarded, so a cycle that never executed arrived at
    " the anti-joins as N missing keys with nothing to say it had not run.
    DATA(rs) = zcl_erpl_rev_delta=>run( iv_target = iv_target ).
    IF rs-error IS NOT INITIAL.
      mv_cycle_err = mv_cycle_err + 1.
      mv_last_err  = |{ iv_target }: { rs-error }|.
    ENDIF.
    " A skipped cycle is not an error, but it is not a cycle either: the lease
    " was held elsewhere and nothing was read. Counted separately so a run whose
    " targets never actually cycled cannot read as a clean pass.
    IF rs-skipped = abap_true.
      mv_cycle_skip = mv_cycle_skip + 1.
    ENDIF.
  ENDMETHOD.

  METHOD job_status.
    " The job's own status, from the job table -- a single SELECT, not a wait:
    " the caller loops. SUBMIT ... strtimmed asks for an immediate start and
    " promises nothing, and nothing here ever looked at the job again.
    SELECT SINGLE status FROM tbtco
      WHERE jobname = @iv_name AND jobcount = @iv_count
      INTO @rv_status.
    IF sy-subrc <> 0. rv_status = '?'. ENDIF.
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
    " An empty alias renders bare column names, for a statement with only one
    " relation in scope -- the latency query, which used to group by BELNR alone
    " and so collapsed every distinct change sharing a document number into one
    " sample.
    DATA(lp) = COND string( WHEN iv_alias IS INITIAL THEN `` ELSE |{ iv_alias }.| ).
    rv = |{ lp }bukrs| && lc && |{ lp }belnr| && lc &&
         |{ lp }gjahr| && lc && |{ lp }buzei|.
  ENDMETHOD.

  METHOD key_cols.
    rv = `client, bukrs, belnr, gjahr, buzei`.
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
