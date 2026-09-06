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
    DATA(lv_sql1) = |SELECT instance_id, status, ticks, | &&
                    |epoch(now()) - epoch(heartbeat_ts) AS age_s | &&
                    |FROM _erpl_rev_daemon|.
    PERFORM q USING lv_sql1 CHANGING lv_rows lv_err.
    IF lv_err IS NOT INITIAL.
      WRITE: / |DAEMON RESULT pass=0 fail=1 error={ lv_err }|.
    ELSE.
      WRITE: / |DAEMON STATUS { lv_rows }|.
    ENDIF.
    RETURN.
  ENDIF.

  " --- ask a running daemon to stop ------------------------------------
  IF p_stop = abap_true.
    DATA(lv_sql2) = |UPDATE _erpl_rev_daemon SET stop=true|.
    PERFORM q USING lv_sql2 CHANGING lv_rows lv_err.
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
  " UNIQUE per process, not per second. This was host/user/date+time at
  " one-second resolution, checked with a substring test -- so two daemons
  " launched in the same second (a scheduler retrying a failed SUBMIT, two
  " strtimmed jobs released together, an operator starting twice) built the
  " SAME id. The loser's claim then changed zero rows, it read back the
  " winner's id, found its own string inside it, and marched into the tick
  " loop. Both beat the same row, both ran every due target against each
  " other, and whichever finished first wrote STOPPED out from under the
  " other -- precisely what the singleton exists to prevent.
  DATA lv_uuid TYPE string.
  TRY.
      lv_uuid = cl_system_uuid=>create_uuid_c32_static( ).
    CATCH cx_uuid_error.
      lv_uuid = |{ sy-datum }{ sy-uzeit }{ sy-timlo }|.
  ENDTRY.
  gv_instance = |{ sy-host }/{ sy-uname }/{ lv_uuid }|.
  DATA(lv_claim) =
    |UPDATE _erpl_rev_daemon SET instance_id='{ gv_instance }', status='RUNNING', | &&
    |heartbeat_ts=now(), started_ts=now(), stop=false, ticks=0 | &&
    |WHERE id=1 AND (status <> 'RUNNING' | &&
    |     OR heartbeat_ts IS NULL | &&
    |     OR heartbeat_ts < now() - INTERVAL '{ p_secs * 3 }' SECOND)|.
  PERFORM q USING lv_claim CHANGING lv_rows lv_err.
  IF lv_err IS NOT INITIAL.
    WRITE: / |DAEMON RESULT pass=0 fail=1 error={ lv_err }|.
    RETURN.
  ENDIF.

  " Did we get it? An EXACT match on a unique id, not a substring test on a
  " shared one: count the row that names us, and require exactly one.
  DATA(lv_who) = |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
                 |WHERE id=1 AND instance_id='{ gv_instance }'|.
  PERFORM q USING lv_who CHANGING lv_rows lv_err.
  IF lv_rows NS '"c":1'.
    " Someone else holds it. Report their status and leave -- starting a second
    " daemon would run every target twice, against each other.
    DATA(lv_st) = |SELECT instance_id, status, ticks FROM _erpl_rev_daemon|.
    PERFORM q USING lv_st CHANGING lv_rows lv_err.
    WRITE: / |DAEMON ALREADY RUNNING { lv_rows }|.
    RETURN.
  ENDIF.

  WRITE: / |DAEMON START instance={ gv_instance } tick={ p_secs }s|.

  " --- the tick loop ----------------------------------------------------
  DATA lv_elapsed TYPE i.
  DATA lv_ticks TYPE i.
  " GET TIME STAMP, not GET RUN TIME. Run time measures CPU-ish program time and
  " does not advance across a WAIT, so a daemon started with p_dur=60 slept
  " through its own deadline and ran until something stopped it.
  GET TIME STAMP FIELD DATA(lv_start).

  DO.
    " STOP is read here, unconditionally, before anything else can fail.
    "
    " It used to be read only out of the tick plan, inside the branch taken
    " when the planner SUCCEEDED -- so a daemon whose planner was erroring for
    " any reason ignored stop=true forever, and with p_dur=0 (which is
    " production) had no other way out. The control channel for "stop" ran
    " through the exact dependency whose misbehaviour is the likeliest reason
    " an operator reaches for it. One statement per tick against a single-row
    " table.
    DATA(lv_stopq) = |SELECT count(*) AS c FROM _erpl_rev_daemon WHERE id=1 AND stop|.
    PERFORM q USING lv_stopq CHANGING lv_rows lv_err.
    IF lv_err IS INITIAL AND lv_rows CS '"c":1'.
      WRITE: / |DAEMON STOP acknowledged after { lv_ticks } ticks|.
      EXIT.
    ENDIF.

    DATA lv_plan TYPE string.
    DATA(lv_a) = `TICK`.
    DATA(lv_t) = ``.
    DATA(lv_p) = ``.
    PERFORM plan USING lv_a lv_t lv_p CHANGING lv_plan lv_err.

    IF lv_err IS NOT INITIAL.
      " The server is unreachable or unhappy. Do not spin: wait a tick and try
      " again, so a restarting server is picked up without hammering the gateway.
      " The server's own words are kept -- "the planner refused" without the
      " reason sends an operator to a log file to learn what this line already
      " knew.
      WRITE: / |DAEMON TICK error={ lv_err }|.
    ELSE.
      " The plan's own stop flag: a fast path only. The authoritative read is
      " at the top of the loop, where a planner error cannot hide it.
      IF lv_plan CS '"stop":true'.
        WRITE: / |DAEMON STOP acknowledged after { lv_ticks } ticks|.
        EXIT.
      ENDIF.

      " Run exactly what the PLAN says, in the order it says.
      "
      " This used to call run_due(), which re-decided "what is due" with its own
      " ABAP query -- one that knows nothing about backoff, parking, the worker
      " budget, a target BLOCKED by schema drift, or whether a trigger target's
      " shadow table has anything in it. So the planner existed, was tested, and
      " governed nothing: a blocked target kept replicating on every tick, and
      " the whole reason the planner is server-side (one definition of "due",
      " shared by the batch tick and the daemon) was quietly false.
      PERFORM run_planned_cycles USING lv_plan.
    ENDIF.

    " The CLI queue is drained here too, so a `daemon stop` or a `sync run`
    " typed at a terminal is picked up within one tick rather than waiting for
    " the one-minute batch job.
    TRY.
        zcl_erpl_rev_clidrv=>drain( ).
      CATCH cx_root ##NO_HANDLER.
    ENDTRY.

    lv_ticks = lv_ticks + 1.
    DATA(lv_beat) = |UPDATE _erpl_rev_daemon SET heartbeat_ts=now(), ticks={ lv_ticks } | &&
                    |WHERE id=1 AND instance_id='{ gv_instance }'|.
    PERFORM q USING lv_beat CHANGING lv_rows lv_err.

    GET TIME STAMP FIELD DATA(lv_now).
    lv_elapsed = CONV i( cl_abap_tstmp=>subtract( tstmp1 = lv_now tstmp2 = lv_start ) ).
    IF p_dur > 0 AND lv_elapsed >= p_dur.
      WRITE: / |DAEMON END after { lv_ticks } ticks ({ lv_elapsed }s)|.
      EXIT.
    ENDIF.

    WAIT UP TO p_secs SECONDS.
  ENDDO.

  " Release the singleton so the next start does not have to wait for the
  " heartbeat to go stale.
  DATA(lv_rel) = |UPDATE _erpl_rev_daemon SET status='STOPPED', stop=false | &&
                 |WHERE id=1 AND instance_id='{ gv_instance }'|.
  PERFORM q USING lv_rel CHANGING lv_rows lv_err.

  WRITE: / |DAEMON RESULT pass={ lv_ticks } fail=0|.


*&---------------------------------------------------------------------*
*& Run the cycles the server's tick plan named, and only those.
*&
*& The plan is JSON: {"cycles":[{"target":"x","load_type":"D",...},...]}.
*& Parsed with a scan rather than a JSON library because this runs every
*& couple of seconds and the shape is ours, not user input.
*&---------------------------------------------------------------------*
FORM run_planned_cycles USING iv_plan TYPE string.
  DATA lv_rest TYPE string.
  DATA lv_obj  TYPE string.
  DATA lv_off  TYPE i.

  " Everything after "cycles":[
  FIND '"cycles":[' IN iv_plan MATCH OFFSET lv_off.
  IF sy-subrc <> 0. RETURN. ENDIF.
  lv_rest = iv_plan+lv_off.

  SPLIT lv_rest AT '{' INTO TABLE DATA(lt_parts).
  LOOP AT lt_parts INTO lv_obj.
    DATA lv_target TYPE string.
    DATA lv_load   TYPE string.
    CLEAR: lv_target, lv_load.
    FIND PCRE '"target"\s*:\s*"([^"]*)"' IN lv_obj SUBMATCHES lv_target.
    IF sy-subrc <> 0 OR lv_target IS INITIAL. CONTINUE. ENDIF.
    FIND PCRE '"load_type"\s*:\s*"([^"]*)"' IN lv_obj SUBMATCHES lv_load.
    IF lv_load IS INITIAL. lv_load = 'D'. ENDIF.

    " Each cycle still takes the per-target lease, so a target already running
    " elsewhere is skipped there rather than run twice.
    zcl_erpl_rev_delta=>run( iv_target = lv_target iv_load_type = lv_load ).
  ENDLOOP.
ENDFORM.
