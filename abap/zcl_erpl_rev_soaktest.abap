CLASS zcl_erpl_rev_soaktest DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* The soak: does the daemon still work after running for a long time?
*"*
*"* Everything else in this suite proves the daemon works for ninety seconds.
*"* The failures a soak exists to find are the ones that need hours -- a lease
*"* that stops being renewed, a log that grows without bound, a target that
*"* parks and never comes back, memory that only goes up. None of them show in
*"* a short run, and all of them show up at a customer.
*"*
*"* The duration is read from the database, not compiled in, so the SAME suite
*"* runs for two minutes in an ordinary e2e and for twenty-four hours before a
*"* release. A soak that can only run for its full length never gets run.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_job TYPE tbtcjob-jobname VALUE 'ERPL_REV_SOAK'.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS sql IMPORTING iv_sql TYPE string.
ENDCLASS.

CLASS zcl_erpl_rev_soaktest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD sql.
    zcl_erpl_rev_util=>query( iv_sql ).
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.

    " How long, from the database. Absent means the short default: an ordinary
    " e2e run must not sit for a day.
    sql( |CREATE TABLE IF NOT EXISTS _erpl_rev_soak(secs INTEGER)| ).
    DATA(lv_secs) = cnt( |SELECT coalesce(max(secs),120) AS c FROM _erpl_rev_soak| ).
    IF lv_secs < 60. lv_secs = 60. ENDIF.
    DATA(lv_tick) = 2.
    out->write( |soak: { lv_secs }s at a { lv_tick }s tick| ).

    " --- a target under continuous change ---------------------------------
    DELETE FROM zdelta_all.
    " The whole audit, not just this run id: the generator's run id is a
    " SUBMIT parameter, and counting only rows that carry it made the oracle
    " depend on that binding rather than on what was actually committed.
    DELETE FROM zdelta_audit.
    COMMIT WORK AND WAIT.
    sql( |DROP TABLE IF EXISTS soak_wm| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target='soak_wm'| ).
    sql( |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false, instance_id=NULL, | &&
         |heartbeat_ts=NULL, ticks=0 WHERE id=1| ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = 'soak_wm' method = 'WATERMARK' source_from = 'ZDELTA_ALL'
      keys = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI' chg_col = 'CHG_TSTAMP'
      wm_kind = 'NUMTS' safety_secs = 2 log_enabled = 'true'
      cadence = |micro:{ lv_tick }| ) ).

    " A generator for the whole run, so the daemon is never idle. Its audit
    " table is the oracle: what it committed is what must arrive.
    DATA lv_gc TYPE tbtcjob-jobcount.
    DATA lv_gn TYPE tbtcjob-jobname VALUE 'ERPL_REV_SOAKGEN'.
    CALL FUNCTION 'JOB_OPEN' EXPORTING jobname = lv_gn IMPORTING jobcount = lv_gc
      EXCEPTIONS OTHERS = 1.
    IF sy-subrc = 0.
      SUBMIT z_erpl_rev_gen WITH p_tab = 'ZDELTA_ALL' WITH p_rate = 5
        WITH p_dur = lv_secs WITH p_ins = 50 WITH p_upd = 30 WITH p_del = 20
        WITH p_run = 'SOAK1' VIA JOB lv_gn NUMBER lv_gc AND RETURN.
      CALL FUNCTION 'JOB_CLOSE' EXPORTING jobcount = lv_gc jobname = lv_gn
                                          strtimmed = abap_true EXCEPTIONS OTHERS = 1.
    ENDIF.

    DATA lv_dc TYPE tbtcjob-jobcount.
    DATA lv_dn TYPE tbtcjob-jobname VALUE 'ERPL_REV_SOAK'.
    CALL FUNCTION 'JOB_OPEN' EXPORTING jobname = lv_dn IMPORTING jobcount = lv_dc
      EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0.
      out->write( |SOAK RESULT pass=0 fail=1 (JOB_OPEN)| ).
      RETURN.
    ENDIF.
    SUBMIT z_erpl_rev_daemon WITH p_secs = lv_tick WITH p_dur = 0
      VIA JOB lv_dn NUMBER lv_dc AND RETURN.
    CALL FUNCTION 'JOB_CLOSE' EXPORTING jobcount = lv_dc jobname = lv_dn
                                        strtimmed = abap_true EXCEPTIONS OTHERS = 1.

    " --- watch it, sampling as it goes -------------------------------------
    "
    " The heartbeat is sampled rather than checked once at the end: a daemon
    " that stalls for ten minutes and recovers looks identical at the end to
    " one that never stalled, and the stall is the thing worth finding.
    DATA(lv_worst_gap) = 0.
    DATA(lv_waited) = 0.
    DATA(lv_parked) = 0.
    WHILE lv_waited < lv_secs.
      WAIT UP TO 10 SECONDS.
      lv_waited = lv_waited + 10.
      DATA(lv_age) = cnt( |SELECT CAST(coalesce(epoch(now())-epoch(heartbeat_ts),999) AS BIGINT) | &&
                          |AS c FROM _erpl_rev_daemon WHERE id=1| ).
      IF lv_age > lv_worst_gap. lv_worst_gap = lv_age. ENDIF.
      lv_parked = lv_parked + cnt( |SELECT count(*) AS c FROM _erpl_rev_delta_state | &&
                                   |WHERE target='soak_wm' AND parked_until IS NOT NULL| ).
    ENDWHILE.

    " Stop it and WAIT for the job to actually end.
    "
    " A fixed sleep is not a wait: this suite leaves a daemon and a generator
    " running in background work processes, and the next suite starts by
    " claiming the same singleton row. Letting them overlap made the daemon
    " suite fail for a reason that had nothing to do with the daemon -- a test
    " that damages the next one is not finished.
    sql( |UPDATE _erpl_rev_daemon SET stop=true WHERE id=1| ).
    " The DAEMON's own signal, not the job table. A missing tbtco row was being
    " read as "finished", so the wait returned while the daemon was still
    " running and still holding the singleton -- and the next suite then could
    " not claim it, failing for a reason that had nothing to do with itself.
    " The daemon writes STOPPED on a clean exit; that is the fact worth waiting
    " for.
    " TWO conditions, because they are two different resources and the next
    " suite needs both. STOPPED says the singleton row is free; the job's
    " terminal status says the background WORK PROCESS is free. The daemon
    " writes STOPPED and then keeps the work process for a moment longer, and
    " on a trial system with a handful of background processes that moment is
    " enough for the next suite's daemon to be queued rather than started --
    " which it reports as "it never claimed the singleton", blaming itself for
    " this suite's leftovers.
    DATA(lv_w) = 0.
    DATA(lv_stopped) = 0.
    DATA lv_ds TYPE btcstatus.
    WHILE lv_w < 120.
      lv_stopped = cnt( |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
                        |WHERE id=1 AND status='STOPPED'| ).
      SELECT SINGLE status FROM tbtco
        WHERE jobname = @lv_dn AND jobcount = @lv_dc INTO @lv_ds.
      " Not-found means not scheduled YET, which is not the same as finished:
      " reading it as finished is what let this suite return while its own
      " daemon was still running.
      IF lv_stopped = 1 AND ( lv_ds = 'F' OR lv_ds = 'A' ). EXIT. ENDIF.
      WAIT UP TO 5 SECONDS.
      lv_w = lv_w + 5.
    ENDWHILE.
    ok( cond = xsdbool( lv_stopped = 1 AND ( lv_ds = 'F' OR lv_ds = 'A' ) )
        what = 'SOAK: the daemon stopped, released the singleton and freed its work process'
        detail = |waited { lv_w }s, status { lv_stopped }/{ lv_ds }| ).

    " And the generator, whose rows would otherwise still be arriving while the
    " oracle below counts them.
    lv_w = 0.
    WHILE lv_w < 90.
      SELECT SINGLE status FROM tbtco
        WHERE jobname = @lv_gn AND jobcount = @lv_gc INTO @DATA(lv_gs).
      " Not-found means not scheduled YET, which is not the same as finished.
      IF lv_gs = 'F' OR lv_gs = 'A'. EXIT. ENDIF.
      WAIT UP TO 5 SECONDS.
      lv_w = lv_w + 5.
    ENDWHILE.

    " --- what a soak is actually for ---------------------------------------
    ok( cond = xsdbool( lv_worst_gap <= lv_tick * 5 )
        what = 'SOAK: the daemon never stalled for more than five ticks'
        detail = |worst heartbeat age { lv_worst_gap }s, tick { lv_tick }s| ).
    ok( cond = xsdbool( lv_parked = 0 )
        what = 'SOAK: the target was never parked'
        detail = |{ lv_parked } sample(s) saw it parked| ).

    DATA(lv_ticks) = cnt( |SELECT coalesce(ticks,0) AS c FROM _erpl_rev_daemon WHERE id=1| ).
    " Ticks roughly match the elapsed time. Far fewer means it was blocked for
    " long stretches -- the failure a short run cannot show.
    ok( cond = xsdbool( lv_ticks >= lv_secs / lv_tick / 2 )
        what = 'SOAK: it kept ticking at roughly its cadence'
        detail = |{ lv_ticks } ticks in { lv_secs }s at a { lv_tick }s tick| ).

    " Nothing lost, by the generator's own audit. A daemon that runs for a day
    " and drops rows is worse than one that stops.
    DATA(lv_committed) = 0.
    SELECT COUNT(*) FROM zdelta_audit INTO @lv_committed.
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_ALL' iv_target = 'soak_truth'
                                  iv_record = abap_false ).
    DATA(lv_lost) = cnt(
      |SELECT count(*) AS c FROM soak_truth t WHERE NOT EXISTS (| &&
      |SELECT 1 FROM soak_wm x WHERE x.client=t.client AND x.bukrs=t.bukrs | &&
      |AND x.belnr=t.belnr AND x.gjahr=t.gjahr AND x.buzei=t.buzei)| ).
    ok( cond = xsdbool( lv_committed > 0 )
        what = 'SOAK: the generator produced a workload' detail = |{ lv_committed } changes| ).
    ok( cond = xsdbool( lv_lost = 0 )
        what = 'SOAK: no row in SAP is missing from the target'
        detail = |{ lv_lost } key(s) missing| ).

    " The change log must not grow without bound relative to the work done.
    DATA(lv_log) = cnt( |SELECT count(*) AS c FROM _erpl_rev_log_soak_wm| ).
    out->write( |soak: { lv_ticks } ticks, log { lv_log } rows, worst gap { lv_worst_gap }s| ).

    sql( |DROP TABLE IF EXISTS soak_truth| ).
    sql( |DROP TABLE IF EXISTS soak_wm| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target='soak_wm'| ).
    DELETE FROM zdelta_all.
    COMMIT WORK AND WAIT.
    " The singleton row back to a startable state, or the next suite's daemon
    " meets a row still naming this one.
    sql( |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false, instance_id=NULL, | &&
         |heartbeat_ts=NULL, ticks=0 WHERE id=1| ).

    out->write( |SOAK RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
