*&---------------------------------------------------------------------*
*& Report Z_ERPL_REV_DAEMON
*&---------------------------------------------------------------------*
*& The streaming daemon: one background job that ticks every few seconds,
*& asks the server what is due, and runs it. This is what makes
*& second-scale replication possible without any new SAP interface --
*& latency is one tick plus one cycle, and nothing is pushed.
*&
*& Deliberately NOT called "stream": query_stream and Z_DUCKDB_OPEN/FETCH/
*& CLOSE already own that word in this codebase, and a second meaning would
*& collide in the e2e markers.
*&
*& Singleton by lease on the one row in _erpl_rev_daemon. A second start
*& reports the running instance and exits rather than running the same
*& targets twice against each other. The per-target lease stops double
*& CYCLES; this stops double DAEMONS, which is a different thing.
*&
*& Restart after a system restart is the periodic Z_ERPL_REV_DELTA job's
*& business: it already runs every minute and drains the CLI queue, so it
*& checks this heartbeat too and re-submits when it is stale. That is
*& cheaper than a second watchdog job.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_daemon.

PARAMETERS: p_secs  TYPE i DEFAULT 2 OBLIGATORY,   " tick interval (>= 1s)
            p_dur   TYPE i DEFAULT 0,              " 0 = until stopped
            p_stop  AS CHECKBOX,                   " ask a running daemon to stop
            p_stat  AS CHECKBOX.                   " report and exit

CONSTANTS c_dest TYPE rfcdest VALUE 'ERPL_REV'.

DATA gv_instance TYPE string.

*---------------------------------------------------------------------*
FORM q USING iv_sql TYPE string CHANGING cv_rows TYPE string cv_err TYPE string.
  CLEAR: cv_rows, cv_err.
  CALL FUNCTION 'Z_DUCKDB_QUERY' DESTINATION c_dest
    EXPORTING iv_sql   = iv_sql
    IMPORTING ev_rows  = cv_rows
              ev_error = cv_err
    EXCEPTIONS communication_failure = 1 system_failure = 2 OTHERS = 3.
  IF sy-subrc <> 0 AND cv_err IS INITIAL.
    cv_err = |RFC subrc={ sy-subrc }|.
  ENDIF.
ENDFORM.

FORM plan USING iv_action TYPE string iv_target TYPE string iv_params TYPE string
          CHANGING cv_plan TYPE string cv_err TYPE string.
  CLEAR: cv_plan, cv_err.
  CALL FUNCTION 'Z_DUCKDB_PLAN' DESTINATION c_dest
    EXPORTING iv_action = iv_action
              iv_target = iv_target
              iv_params = iv_params
    IMPORTING ev_plan   = cv_plan
              ev_error  = cv_err
    EXCEPTIONS communication_failure = 1 system_failure = 2 OTHERS = 3.
  IF sy-subrc <> 0 AND cv_err IS INITIAL.
    cv_err = |RFC subrc={ sy-subrc }|.
  ENDIF.
ENDFORM.

*---------------------------------------------------------------------*
START-OF-SELECTION.

  DATA lv_rows TYPE string.
  DATA lv_err  TYPE string.

  " --- status only -----------------------------------------------------
  IF p_stat = abap_true.
    PERFORM q USING |SELECT instance_id, status, ticks, | &&
                    |epoch(now()) - epoch(heartbeat_ts) AS age_s | &&
                    |FROM _erpl_rev_daemon|
              CHANGING lv_rows lv_err.
    IF lv_err IS NOT INITIAL.
      WRITE: / |DAEMON RESULT pass=0 fail=1 error={ lv_err }|.
    ELSE.
      WRITE: / |DAEMON STATUS { lv_rows }|.
    ENDIF.
    RETURN.
  ENDIF.

  " --- ask a running daemon to stop ------------------------------------
  IF p_stop = abap_true.
    PERFORM q USING |UPDATE _erpl_rev_daemon SET stop=true|
              CHANGING lv_rows lv_err.
    WRITE: / COND string( WHEN lv_err IS INITIAL
                          THEN |DAEMON STOP requested; it ends at the next tick|
                          ELSE |DAEMON STOP failed: { lv_err }| ).
    RETURN.
  ENDIF.

  IF p_secs < 1. p_secs = 1. ENDIF.

  " --- claim the singleton ---------------------------------------------
  " One UPDATE with the staleness test in its WHERE, so two daemons starting
  " at the same moment cannot both believe they won: exactly one row is
  " changed. A heartbeat older than three ticks means the holder is gone.
  gv_instance = |{ sy-host }/{ sy-uname }/{ sy-datum }{ sy-uzeit }|.
  PERFORM q USING
    |UPDATE _erpl_rev_daemon SET instance_id='{ gv_instance }', status='RUNNING', | &&
    |heartbeat_ts=now(), started_ts=now(), stop=false, ticks=0 | &&
    |WHERE id=1 AND (status <> 'RUNNING' | &&
    |     OR heartbeat_ts IS NULL | &&
    |     OR heartbeat_ts < now() - INTERVAL '{ p_secs * 3 }' SECOND)|
    CHANGING lv_rows lv_err.
  IF lv_err IS NOT INITIAL.
    WRITE: / |DAEMON RESULT pass=0 fail=1 error={ lv_err }|.
    RETURN.
  ENDIF.

  " Did we get it? Read back rather than trusting the update count.
  PERFORM q USING |SELECT instance_id FROM _erpl_rev_daemon WHERE id=1|
            CHANGING lv_rows lv_err.
  IF lv_rows NS gv_instance.
    " Someone else holds it. Report their status and leave -- starting a second
    " daemon would run every target twice, against each other.
    PERFORM q USING |SELECT instance_id, status, ticks FROM _erpl_rev_daemon|
              CHANGING lv_rows lv_err.
    WRITE: / |DAEMON ALREADY RUNNING { lv_rows }|.
    RETURN.
  ENDIF.

  WRITE: / |DAEMON START instance={ gv_instance } tick={ p_secs }s|.

  " --- the tick loop ----------------------------------------------------
  DATA lv_t0 TYPE i.
  DATA lv_elapsed TYPE i.
  DATA lv_ticks TYPE i.
  GET RUN TIME FIELD DATA(lv_start).

  DO.
    DATA lv_plan TYPE string.
    PERFORM plan USING `TICK` `` `` CHANGING lv_plan lv_err.

    IF lv_err IS NOT INITIAL.
      " The server is unreachable or unhappy. Do not spin: wait a tick and try
      " again, so a restarting server is picked up without hammering the gateway.
      WRITE: / |DAEMON TICK error={ lv_err }|.
    ELSE.
      IF lv_plan CS '"stop":true'.
        WRITE: / |DAEMON STOP acknowledged after { lv_ticks } ticks|.
        EXIT.
      ENDIF.

      " The plan names the due targets; run() does the cycle. Each is leased
      " individually, so a target already running elsewhere is skipped there.
      zcl_erpl_rev_delta=>run_due( ).
    ENDIF.

    " The CLI queue is drained here too, so a `daemon stop` or a `sync run`
    " typed at a terminal is picked up within one tick rather than waiting for
    " the one-minute batch job.
    TRY.
        zcl_erpl_rev_clidrv=>drain( ).
      CATCH cx_root ##NO_HANDLER.
    ENDTRY.

    lv_ticks = lv_ticks + 1.
    PERFORM q USING |UPDATE _erpl_rev_daemon SET heartbeat_ts=now(), ticks={ lv_ticks } | &&
                    |WHERE id=1 AND instance_id='{ gv_instance }'|
              CHANGING lv_rows lv_err.

    GET RUN TIME FIELD DATA(lv_now).
    lv_elapsed = ( lv_now - lv_start ) / 1000000.
    IF p_dur > 0 AND lv_elapsed >= p_dur.
      WRITE: / |DAEMON END after { lv_ticks } ticks ({ lv_elapsed }s)|.
      EXIT.
    ENDIF.

    WAIT UP TO p_secs SECONDS.
  ENDDO.

  " Release the singleton so the next start does not have to wait for the
  " heartbeat to go stale.
  PERFORM q USING |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false | &&
                  |WHERE id=1 AND instance_id='{ gv_instance }'|
            CHANGING lv_rows lv_err.

  WRITE: / |DAEMON RESULT pass={ lv_ticks } fail=0|.
