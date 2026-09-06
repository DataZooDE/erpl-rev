CLASS zcl_erpl_rev_daemontest DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* The streaming daemon, running for real.
*"*
*"* Everything else about the daemon is unit-tested: which targets are due,
*"* the backoff, the parking, the worker budget. All of that is a pure
*"* function in the server with its own cases. What none of it exercises is
*"* the thing a customer actually buys -- a background job that stays up,
*"* ticks, and replicates changes nobody asked it to replicate.
*"*
*"* So this test never calls run(). Not once. Every row that reaches a target
*"* here got there because the daemon's own loop decided it should, which is
*"* the only way to tell a working daemon from a well-tested planner.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_jobname TYPE tbtcjob-jobname VALUE 'ERPL_REV_DAEMON_T'.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS scalar IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE string.
    METHODS sql IMPORTING iv_sql TYPE string.
    METHODS start_daemon IMPORTING iv_secs TYPE i iv_dur TYPE i
                         RETURNING VALUE(rv_count) TYPE tbtcjob-jobcount.
    METHODS job_status IMPORTING iv_count TYPE tbtcjob-jobcount
                       RETURNING VALUE(rv) TYPE btcstatus.
    "! polls until iv_sql returns a value >= iv_want, or the deadline passes
    METHODS wait_until IMPORTING iv_sql TYPE string iv_want TYPE i iv_secs TYPE i
                       RETURNING VALUE(rv_got) TYPE i.
    METHODS seed_rows IMPORTING iv_from TYPE i iv_count TYPE i.
ENDCLASS.

CLASS zcl_erpl_rev_daemontest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD scalar.
    " Extract the VALUE. This used to return ls-rows -- the unparsed JSON --
    " so `instance_id IS NOT INITIAL` was green for {"instance_id":null} and
    " comparing two of them compared two JSON blobs that were equally green
    " when both were null. Three assertions about the singleton could not
    " fail, which is the whole thing this suite exists to check.
    DATA(ls) = zcl_erpl_rev_util=>query( iv_sql ).
    FIND PCRE '"[^"]+"\s*:\s*"([^"]*)"' IN ls-rows SUBMATCHES rv.
    IF sy-subrc <> 0. CLEAR rv. ENDIF.
  ENDMETHOD.

  METHOD sql.
    zcl_erpl_rev_util=>query( iv_sql ).
  ENDMETHOD.

  METHOD job_status.
    SELECT SINGLE status FROM tbtco
      WHERE jobname = @c_jobname AND jobcount = @iv_count INTO @rv.
    IF sy-subrc <> 0. rv = '?'. ENDIF.
  ENDMETHOD.

  METHOD start_daemon.
    CALL FUNCTION 'JOB_OPEN' EXPORTING jobname = c_jobname
      IMPORTING jobcount = rv_count EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0. CLEAR rv_count. RETURN. ENDIF.
    SUBMIT z_erpl_rev_daemon WITH p_secs = iv_secs WITH p_dur = iv_dur
      VIA JOB c_jobname NUMBER rv_count AND RETURN.
    CALL FUNCTION 'JOB_CLOSE' EXPORTING jobcount = rv_count jobname = c_jobname
                                        strtimmed = abap_true EXCEPTIONS OTHERS = 1.
  ENDMETHOD.

  METHOD wait_until.
    DATA(lv_waited) = 0.
    DO.
      rv_got = cnt( iv_sql ).
      IF rv_got >= iv_want OR lv_waited >= iv_secs. RETURN. ENDIF.
      WAIT UP TO 1 SECONDS.
      lv_waited = lv_waited + 1.
    ENDDO.
  ENDMETHOD.

  METHOD seed_rows.
    DATA ls TYPE zdelta_all.
    DO iv_count TIMES.
      GET TIME STAMP FIELD DATA(lv_ts).
      CLEAR ls.
      ls-client     = sy-mandt.
      ls-bukrs      = '1000'.
      ls-belnr      = |{ iv_from + sy-index WIDTH = 10 ALIGN = RIGHT PAD = '0' }|.
      ls-gjahr      = '2026'.
      ls-buzei      = '001'.
      ls-chg_tstamp = lv_ts.
      ls-dmbtr      = sy-index.
      ls-sgtxt      = |daemon { sy-index }|.
      MODIFY zdelta_all FROM ls.
    ENDDO.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.

    " --- a clean slate ---------------------------------------------------
    DELETE FROM zdelta_all.
    COMMIT WORK AND WAIT.
    sql( |DROP TABLE IF EXISTS dmn_wm| ).
    sql( |DROP TABLE IF EXISTS _erpl_rev_log_dmn_wm| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target LIKE 'dmn\\_%' ESCAPE '\\'| ).
    sql( |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false, instance_id=NULL, | &&
         |heartbeat_ts=NULL WHERE id=1| ).

    " A target on a 2-second cadence, so the daemon has something due on
    " almost every tick.
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'dmn_wm'
      method      = 'WATERMARK'
      source_from = 'ZDELTA_ALL'
      keys        = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'
      chg_col     = 'CHG_TSTAMP'
      wm_kind     = 'NUMTS'
      safety_secs = 2
      cadence     = 'micro:2' ) ).

    seed_rows( iv_from = 0 iv_count = 5 ).

    " --- start it --------------------------------------------------------
    " iv_dur = 0: it runs until something STOPS it. With a duration the daemon
    " ends on its own timer, and both stop assertions below were satisfied by
    " that timer rather than by the flag -- the release path is identical for
    " either exit. This is also production's setting.
    DATA(lv_job) = start_daemon( iv_secs = 2 iv_dur = 0 ).
    ok( cond = xsdbool( lv_job IS NOT INITIAL )
        what = 'DAEMON-START: the daemon job was submitted' detail = |{ lv_job }| ).
    IF lv_job IS INITIAL.
      out->write( |DAEMON RESULT pass={ mv_pass } fail={ mv_fail }| ).
      RETURN.
    ENDIF.

    " It claims the singleton row and starts beating.
    DATA(lv_ticks) = wait_until(
      iv_sql = |SELECT coalesce(ticks,0) AS c FROM _erpl_rev_daemon WHERE id=1|
      iv_want = 2 iv_secs = 30 ).
    ok( cond = xsdbool( lv_ticks >= 2 )
        what = 'DAEMON-TICK: the loop is running and beating'
        detail = |ticks={ lv_ticks }| ).
    DATA(lv_inst) = scalar( |SELECT instance_id FROM _erpl_rev_daemon WHERE id=1| ).
    ok( cond = xsdbool( lv_inst IS NOT INITIAL )
        what = 'DAEMON-TICK: it claimed the singleton' detail = lv_inst ).

    " --- the point: it replicates without anyone asking ------------------
    DATA(lv_rows) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_wm|
                                iv_want = 5 iv_secs = 30 ).
    ok( cond = xsdbool( lv_rows >= 5 )
        what = 'DAEMON-WORK: the five seeded rows replicated, with no run() call'
        detail = |{ lv_rows }/5| ).

    " ...and keeps doing it for changes committed while it runs. This is the
    " streaming claim, and nothing else in the suite makes it.
    seed_rows( iv_from = 100 iv_count = 5 ).
    DATA(lv_rows2) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_wm|
                                 iv_want = 10 iv_secs = 30 ).
    ok( cond = xsdbool( lv_rows2 >= 10 )
        what = 'DAEMON-STREAM: rows committed while it ran were picked up'
        detail = |{ lv_rows2 }/10| ).

    " --- a second daemon must not start ----------------------------------
    " Two daemons run every target against each other: same source, same
    " target, two cycles racing for one lease. The per-target lease stops
    " double CYCLES; this is what stops double DAEMONS.
    DATA(lv_job2) = start_daemon( iv_secs = 2 iv_dur = 0 ).
    WAIT UP TO 12 SECONDS.
    DATA(lv_inst2) = scalar( |SELECT instance_id FROM _erpl_rev_daemon WHERE id=1| ).
    ok( cond = xsdbool( lv_inst2 = lv_inst )
        what = 'DAEMON-SINGLE: the second daemon did not take the singleton'
        detail = |was { lv_inst }, now { lv_inst2 }| ).
    ok( cond = xsdbool( job_status( lv_job2 ) = 'F' )
        what = 'DAEMON-SINGLE: the second daemon reported and exited'
        detail = |status { job_status( lv_job2 ) }| ).
    ok( cond = xsdbool( job_status( lv_job ) = 'R' )
        what = 'DAEMON-SINGLE: the first daemon is still running'
        detail = |status { job_status( lv_job ) }| ).

    " --- one broken target must not stop the others ----------------------
    " The backoff and the parking are unit-tested as a pure function. What that
    " cannot show is that a target which fails FOR REAL, inside a running
    " daemon, is counted, backed off, and eventually left alone -- while the
    " healthy target on the same tick keeps going. A daemon that dies on one bad
    " target takes every other target down with it.
    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'dmn_broken'
      method      = 'WATERMARK'
      source_from = 'ZNO_SUCH_TABLE_AT_ALL'
      keys        = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'
      chg_col     = 'CHG_TSTAMP'
      wm_kind     = 'NUMTS'
      safety_secs = 2
      cadence     = 'micro:2' ) ).

    DATA(lv_fails) = wait_until(
      iv_sql = |SELECT coalesce(fail_count,0) AS c FROM _erpl_rev_delta_state | &&
               |WHERE target='dmn_broken'|
      iv_want = 2 iv_secs = 40 ).
    ok( cond = xsdbool( lv_fails >= 2 )
        what = 'DAEMON-BACKOFF: the broken target was tried, failed and counted'
        detail = |fail_count={ lv_fails }| ).

    " The healthy one is still being replicated on the same ticks.
    seed_rows( iv_from = 200 iv_count = 3 ).
    DATA(lv_rows3) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_wm|
                                 iv_want = 13 iv_secs = 40 ).
    ok( cond = xsdbool( lv_rows3 >= 13 )
        what = 'DAEMON-BACKOFF: the healthy target kept replicating throughout'
        detail = |{ lv_rows3 }/13| ).

    " And the daemon itself is unharmed: still the same instance, still ticking.
    ok( cond = xsdbool( scalar( |SELECT instance_id FROM _erpl_rev_daemon WHERE id=1| )
                          = lv_inst )
        what = 'DAEMON-BACKOFF: the daemon survived a failing target' ).

    " The id must be unique per PROCESS, not per second. Two daemons launched
    " in the same second used to build the same host/user/timestamp id, and the
    " loser's substring read-back then found its own string in the winner's and
    " marched on -- two daemons, one row, every target run twice. This suite
    " cannot stage that race (its two starts are a minute and a half apart), so
    " what is asserted here is the property that removes it: the id carries a
    " per-process unique component rather than a one-second clock.
    ok( cond = xsdbool( strlen( lv_inst ) > 40 AND lv_inst CS '/' )
        what = 'DAEMON-SINGLE: the instance id is unique per process, not per second'
        detail = lv_inst ).

    " --- stop it ---------------------------------------------------------
    " Alive first, so "it finished" below means the flag ended it. With
    " iv_dur=0 nothing else can.
    ok( cond = xsdbool( job_status( lv_job ) = 'R' )
        what = 'DAEMON-STOP: it was still running before the flag was set'
        detail = |status { job_status( lv_job ) }| ).
    sql( |UPDATE _erpl_rev_daemon SET stop=true WHERE id=1| ).
    DATA(lv_stopped) = wait_until(
      iv_sql = |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
               |WHERE id=1 AND status='STOPPED'|
      iv_want = 1 iv_secs = 30 ).
    ok( cond = xsdbool( lv_stopped = 1 )
        what = 'DAEMON-STOP: the stop flag ended the loop and released the singleton'
        detail = |status={ scalar( |SELECT status FROM _erpl_rev_daemon WHERE id=1| ) }| ).
    ok( cond = xsdbool( job_status( lv_job ) = 'F' )
        what = 'DAEMON-STOP: the job finished rather than being killed'
        detail = |status { job_status( lv_job ) }| ).

    " The stop flag is cleared on the way out, or the next daemon would stop
    " on its first tick.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
                             |WHERE id=1 AND stop=false| ) = 1 )
        what = 'DAEMON-STOP: the flag was cleared for the next start' ).

    " Leave nothing behind. dmn_broken points at a table that does not exist on
    " purpose; left registered, every later daemon or batch tick keeps failing
    " it and piling up fail_count on a system that is not under test.
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target LIKE 'dmn\\_%' ESCAPE '\\'| ).
    sql( |DROP TABLE IF EXISTS dmn_wm| ).

    out->write( |DAEMON RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
