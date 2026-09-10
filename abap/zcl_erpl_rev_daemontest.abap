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
    METHODS out_line IMPORTING iv_text TYPE string.
ENDCLASS.

CLASS zcl_erpl_rev_daemontest IMPLEMENTATION.

  METHOD out_line.
    mo->write( iv_text ).
  ENDMETHOD.

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
    " Triggers first: the ZCDC_* object names derive from the SOURCE, so a set
    " left over from a previous run writing into a log table that no longer
    " exists makes every INSERT on ZDELTA_ALL dump -- including seed_rows below.
    zcl_erpl_rev_cdc=>teardown( 'dmn_cdc' ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='dmn_cdc'| ).
    sql( |DROP TABLE IF EXISTS dmn_cdc| ).
    sql( |DROP TABLE IF EXISTS _erpl_rev_log_dmn_cdc| ).
    sql( |DROP SEQUENCE IF EXISTS _erpl_rev_log_dmn_cdc_seq| ).
    sql( |DELETE FROM _erpl_rev_run_stats WHERE target='dmn_cdc'| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target LIKE 'dmn\\_%' ESCAPE '\\'| ).
    sql( |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false, instance_id=NULL, | &&
         |heartbeat_ts=NULL, ticks=0 WHERE id=1| ).

    " The clean slate, asserted rather than assumed. ticks is a CUMULATIVE
    " counter and the reset above is the only thing that zeroes it, so without
    " that clause this suite starts with the PREVIOUS suite's count already
    " past every gate below -- and then races the daemon's claim for the
    " verdict. A race is not a test: it passes on a quiet system and fails on a
    " busy one, and both readings look like the daemon's fault.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM _erpl_rev_daemon WHERE id=1 | &&
                             |AND coalesce(ticks,0)=0 AND instance_id IS NULL| ) = 1 )
        what = 'DAEMON-SLATE: the singleton starts reset, not inherited'
        detail = |ticks={ cnt( |SELECT coalesce(ticks,0) AS c FROM _erpl_rev_daemon | &&
                               |WHERE id=1| ) }| ).

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
      log_enabled = 'true'
      cadence     = 'micro:2' ) ).

    " The SAME source on the trigger tier, so the two methods run side by side
    " under one daemon. That is what makes DAEMON-CDC-DELETE below possible:
    " one physical delete, at one moment, leaving one target and not the other.
    " The order matters and is the operator's, not a convenience: SEED the
    " target, register it, then provision the triggers.
    "
    " A cycle applies a DELTA. Without the target table it has nothing to apply
    " to, the first cycle errors, _erpl_rev_cdc goes to ERROR, and the planner's
    " status gate then skips the target on every tick for ever after -- which
    " presents as a trigger tier that silently does nothing, the exact symptom
    " this stage exists to catch. The first version of this test made that
    " mistake and spent a run proving the product right.
    DATA(ls_seed) = zcl_erpl_rev_util=>replicate(
      iv_tab = 'ZDELTA_ALL' iv_target = 'dmn_cdc' iv_where = |BELNR = '9999999999'| ).
    ok( cond = xsdbool( ls_seed-error IS INITIAL )
        what = 'DAEMON-CDC: the trigger target was seeded' detail = ls_seed-error ).

    zcl_erpl_rev_delta=>register( VALUE #(
      target      = 'dmn_cdc'
      method      = 'CDC'
      source_from = 'ZDELTA_ALL'
      keys        = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'
      cadence     = 'micro:2'
      log_enabled = 'true' ) ).

    DATA(lv_cpe) = zcl_erpl_rev_cdc=>provision(
      iv_target = 'dmn_cdc' iv_source = 'ZDELTA_ALL'
      iv_keys = 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI' iv_mode = 'KEYS_IUD' ).
    ok( cond = xsdbool( lv_cpe IS INITIAL )
        what = 'DAEMON-CDC: the trigger target provisioned' detail = lv_cpe ).

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
    " Ninety seconds, not thirty. A background job is not started when it is
    " submitted -- it is started when a work process picks it up, and on a
    " system that has just finished a soak that can take a while. The daemon
    " being slow to be SCHEDULED is not the daemon failing, and a suite that
    " calls it one produces failures nobody can act on.
    " The CLAIM first, then the ticks. ticks is a cumulative counter, not a
    " fresh one: waiting on it alone was satisfied by the PREVIOUS suite's
    " leftover count, so this suite ran on past a daemon that had not started
    " yet and then reported the absence of an instance id as a defect. The
    " clean slate above now zeroes ticks too; waiting on the claim as well
    " means the order of the suites cannot decide this again.
    DATA(lv_claimed) = wait_until(
      iv_sql = |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
               |WHERE id=1 AND instance_id IS NOT NULL|
      iv_want = 1 iv_secs = 90 ).
    DATA(lv_ticks) = wait_until(
      iv_sql = |SELECT coalesce(ticks,0) AS c FROM _erpl_rev_daemon WHERE id=1|
      iv_want = 2 iv_secs = 90 ).
    ok( cond = xsdbool( lv_ticks >= 2 )
        what = 'DAEMON-TICK: the loop is running and beating'
        detail = |ticks={ lv_ticks }| ).
    DATA(lv_inst) = scalar( |SELECT instance_id FROM _erpl_rev_daemon WHERE id=1| ).
    ok( cond = xsdbool( lv_inst IS NOT INITIAL )
        what = 'DAEMON-TICK: it claimed the singleton'
        detail = |inst={ lv_inst } ticks={ lv_ticks } | &&
                 |status={ scalar( |SELECT status FROM _erpl_rev_daemon WHERE id=1| ) } | &&
                 |inst_null={ cnt( |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
                                   |WHERE id=1 AND instance_id IS NULL| ) } | &&
                 |hb_age={ cnt( |SELECT CAST(coalesce(epoch(now())-epoch(heartbeat_ts),-1) | &&
                                |AS BIGINT) AS c FROM _erpl_rev_daemon WHERE id=1| ) } | &&
                 |job={ job_status( lv_job ) }| ).

    " --- the point: it replicates without anyone asking ------------------
    DATA(lv_rows) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_wm|
                                iv_want = 5 iv_secs = 60 ).
    ok( cond = xsdbool( lv_rows >= 5 )
        what = 'DAEMON-WORK: the five seeded rows replicated, with no run() call'
        detail = |{ lv_rows }/5| ).

    " ...and keeps doing it for changes committed while it runs. This is the
    " streaming claim, and nothing else in the suite makes it.
    seed_rows( iv_from = 100 iv_count = 5 ).
    DATA(lv_rows2) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_wm|
                                 iv_want = 10 iv_secs = 60 ).
    ok( cond = xsdbool( lv_rows2 >= 10 )
        what = 'DAEMON-STREAM: rows committed while it ran were picked up'
        detail = |{ lv_rows2 }/10| ).

    " --- DAEMON-CDC: the trigger tier, driven by nothing but the daemon -----
    "
    " Three defects reached main together because every automated test drove
    " this tier by calling zcl_erpl_rev_cdc=>run() itself. The planner gated
    " trigger targets on _erpl_rev_cdc.shadow_rows, a column nothing wrote, so
    " a trigger target was never due. The daemon ran every planned cycle
    " through the WATERMARK entry point, because nothing read the method the
    " plan had always carried. And the trigger apply never wrote
    " _erpl_rev_delta_state, so `top`, `sync ls`, the Prometheus gauges and the
    " ALV report all showed a busy target as IDLE, never run, 0 rows.
    "
    " Every one of them is invisible to a test that calls run() itself, and
    " none of them survives a test that refuses to. A recorded demo found them,
    " which is a slow and expensive way to find anything.
    DATA(lv_cdc_rows) = wait_until( iv_sql = |SELECT count(*) AS c FROM dmn_cdc|
                                    iv_want = 5 iv_secs = 90 ).
    ok( cond = xsdbool( lv_cdc_rows >= 5 )
        what = 'DAEMON-CDC-WORK: the trigger target replicated, with no run() call'
        detail = |{ lv_cdc_rows }/5 shadow={ cnt( |SELECT coalesce(shadow_rows,0) AS c | &&
                 |FROM _erpl_rev_cdc WHERE target='dmn_cdc'| ) }| ).

    " The daemon DISPATCHED it as CDC. A watermark cycle against this target
    " could also move rows -- it is the same source -- so "rows arrived" alone
    " does not distinguish a working dispatch from the bug. The run statistics
    " name the entry point that ran.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM _erpl_rev_run_stats | &&
                             |WHERE target='dmn_cdc' AND method='CDC' | &&
                             |AND status='SUCCESS'| ) >= 1 )
        what = 'DAEMON-CDC-METHOD: the daemon ran it through the CDC entry point'
        detail = |methods={ scalar( |SELECT string_agg(DISTINCT method) AS m | &&
                                    |FROM _erpl_rev_run_stats WHERE target='dmn_cdc'| ) }| ).

    " And the operator can see it. This is the assertion that would have caught
    " the third defect on its own: the rows were arriving correctly the whole
    " time, and every surface an operator looks at said nothing was happening.
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM erpl_rev_targets | &&
                             |WHERE target='dmn_cdc' AND lag_seconds IS NOT NULL | &&
                             |AND last_rows > 0| ) = 1 )
        what = 'DAEMON-CDC-STATE: the operator views show it as run, not as never run'
        detail = |lag={ scalar( |SELECT CAST(lag_seconds AS VARCHAR) AS v | &&
                                |FROM erpl_rev_targets WHERE target='dmn_cdc'| ) } | &&
                 |rows={ cnt( |SELECT coalesce(last_rows,0) AS c FROM erpl_rev_targets | &&
                              |WHERE target='dmn_cdc'| ) }| ).

    " --- the reason the tier exists, stated as a difference -----------------
    " One physical delete, one moment, one daemon, two targets on the same
    " source. The trigger target loses the row. The watermark target cannot
    " see it leave and keeps it. If this ever passes for both, the tier has
    " stopped being worth its cost.
    DATA(lv_gone) = |{ 3 WIDTH = 10 ALIGN = RIGHT PAD = '0' }|.

    " Present in both FIRST, or "it left" is satisfied by a target that never
    " had it. Reintroducing the planner defect to check this stage was
    " load-bearing turned the other three assertions red and left this one
    " GREEN -- because nothing had replicated at all, and an empty target
    " trivially contains no deleted row. An assertion that passes hardest when
    " the feature is most broken is worse than no assertion.
    DATA(lv_before) = wait_until(
      iv_sql = |SELECT count(*) AS c FROM dmn_cdc WHERE belnr='{ lv_gone }'|
      iv_want = 1 iv_secs = 60 ).
    ok( cond = xsdbool( lv_before = 1 AND cnt( |SELECT count(*) AS c FROM dmn_wm | &&
                                               |WHERE belnr='{ lv_gone }'| ) = 1 )
        what = 'DAEMON-CDC-DELETE: the row was in both targets before it was deleted'
        detail = |cdc={ lv_before } wm={ cnt( |SELECT count(*) AS c FROM dmn_wm | &&
                                              |WHERE belnr='{ lv_gone }'| ) }| ).

    DELETE FROM zdelta_all WHERE belnr = @lv_gone.
    COMMIT WORK AND WAIT.
    DATA(lv_left) = wait_until(
      iv_sql = |SELECT 1 - count(*) AS c FROM dmn_cdc WHERE belnr='{ lv_gone }'|
      iv_want = 1 iv_secs = 90 ).
    ok( cond = xsdbool( lv_left = 1 )
        what = 'DAEMON-CDC-DELETE: a physical delete left the trigger target'
        detail = |still there={ cnt( |SELECT count(*) AS c FROM dmn_cdc | &&
                                     |WHERE belnr='{ lv_gone }'| ) }| ).
    ok( cond = xsdbool( cnt( |SELECT count(*) AS c FROM dmn_wm | &&
                             |WHERE belnr='{ lv_gone }'| ) = 1 )
        what = 'DAEMON-CDC-DELETE: the watermark target on the same source kept it'
        detail = |a watermark cannot see a row leave; if this fails the | &&
                 |comparison has stopped meaning anything| ).

    " --- DAEMON-LAT: how far behind the source the daemon actually is -------
    "
    " The number the product is sold on. The stress harness measures latency for
    " a hand-driven cycle loop; this measures it THROUGH THE DAEMON, which is
    " what a customer runs -- tick scheduling, the planner, the worker budget
    " and the cycle, end to end.
    "
    " ONE SAMPLE PER CHANGE, at its first apply. The safety overlap re-reads
    " recent rows on purpose, so a change is logged several times with
    " ever-later apply times; counting every log row measures the overlap window
    " rather than the pipeline, and reported it five times slower than it is.
    " In MILLISECONDS, as an integer, read with cnt().
    "
    " scalar() extracts a QUOTED JSON value, and a number is not quoted -- so
    " reading a p95 with it returned empty, the comparison ran against zero, and
    " the assertion passed however slow the daemon was. The one claim the
    " streaming tier makes, guarded by something that could not fail.
    DATA(lv_samples) = cnt(
      |SELECT count(*) AS c FROM (| &&
      |SELECT epoch(min(_applied_at))-epoch(_commit_ts) AS lat | &&
      |FROM _erpl_rev_log_dmn_wm WHERE _commit_ts IS NOT NULL | &&
      |GROUP BY client, bukrs, belnr, gjahr, buzei, _commit_ts)| ).
    DATA(lv_p95ms) = cnt(
      |SELECT CAST(coalesce(round(quantile_disc(lat,0.95)*1000),0) AS BIGINT) AS c FROM (| &&
      |SELECT epoch(min(_applied_at))-epoch(_commit_ts) AS lat | &&
      |FROM _erpl_rev_log_dmn_wm WHERE _commit_ts IS NOT NULL | &&
      |GROUP BY client, bukrs, belnr, gjahr, buzei, _commit_ts)| ).
    DATA(lv_n) = cnt( |SELECT count(*) AS c FROM _erpl_rev_log_dmn_wm| ).
    ok( cond = xsdbool( lv_n > 0 )
        what = 'DAEMON-LAT: the change log captured the daemon''s work'
        detail = |{ lv_n } row(s)| ).
    " p95 under five seconds on a two-second tick. Asserted, not printed: a
    " latency figure nobody checks is a number in a log, and this is the one
    " claim the streaming tier makes.
    " Samples must EXIST before a latency claim means anything: "no samples"
    " and "fast" are the same number otherwise.
    ok( cond = xsdbool( lv_samples > 0 )
        what = 'DAEMON-LAT: there are latency samples to judge'
        detail = |{ lv_samples } sample(s)| ).
    ok( cond = xsdbool( lv_samples > 0 AND lv_p95ms > 0 AND lv_p95ms <= 5000 )
        what = 'DAEMON-LAT: p95 source-commit to applied is within 5s'
        detail = |p95={ lv_p95ms }ms over { lv_samples } sample(s)| ).
    out_line( |DAEMON-LAT p95={ lv_p95ms }ms samples={ lv_samples }| ).

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
    " The triggers go first and unconditionally: a trigger set left on
    " ZDELTA_ALL writing into a log table this suite is about to drop makes
    " every later INSERT on that table dump, in suites that have nothing to do
    " with CDC.
    zcl_erpl_rev_cdc=>teardown( 'dmn_cdc' ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='dmn_cdc'| ).
    sql( |DELETE FROM _erpl_rev_delta_state WHERE target LIKE 'dmn\\_%' ESCAPE '\\'| ).
    sql( |DROP TABLE IF EXISTS dmn_wm| ).
    sql( |DROP TABLE IF EXISTS dmn_cdc| ).
    sql( |DROP TABLE IF EXISTS _erpl_rev_log_dmn_cdc| ).

    out->write( |DAEMON RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
