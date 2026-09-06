*&---------------------------------------------------------------------*
*& Report  Z_ERPL_REV_DELTA
*&---------------------------------------------------------------------*
*& Delta orchestration loop. On each tick it asks the server which targets are
*& DUE (cadence elapsed since last_run_ts, lease free) and runs one delta cycle
*& per target (lease -> dispatch by method -> commit -> release). All delta state
*& lives in the DuckDB table _erpl_rev_delta_state (no SAP Z table).
*&
*& Two framed blocks on the screen:
*&   "Run delta now"  - run every due target once (or a single named target), as a
*&                      one-shot job step, or as a foreground watch loop.
*&   "Schedule ..."   - install / remove ONE periodic background job that ticks this
*&                      report (each tick runs all due targets, so one job at the
*&                      finest period drives every per-target cadence). SM37-monitorable.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_delta LINE-SIZE 120.
TYPE-POOLS icon.   " icon_led_green / _red / _yellow, icon_alarm, icon_delete for the list

" ── Run delta now ─────────────────────────────────────────────────────────────
SELECTION-SCREEN BEGIN OF BLOCK run WITH FRAME TITLE t_run.
  SELECTION-SCREEN BEGIN OF LINE.
  SELECTION-SCREEN COMMENT 1(28) c_tgt FOR FIELD p_tgt.
  PARAMETERS p_tgt TYPE string.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN COMMENT /3(75) c_tgt2.
  SELECTION-SCREEN BEGIN OF LINE.
  PARAMETERS p_once RADIOBUTTON GROUP m DEFAULT 'X' USER-COMMAND md.
  SELECTION-SCREEN COMMENT 4(60) c_once FOR FIELD p_once.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN BEGIN OF LINE.
  PARAMETERS p_loop RADIOBUTTON GROUP m.
  SELECTION-SCREEN COMMENT 4(60) c_loop FOR FIELD p_loop.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN BEGIN OF LINE.
  SELECTION-SCREEN COMMENT 6(26) c_secs FOR FIELD p_secs MODIF ID lop.
  PARAMETERS p_secs TYPE i DEFAULT 60 MODIF ID lop.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN BEGIN OF LINE.
  SELECTION-SCREEN COMMENT 6(26) c_dur FOR FIELD p_dur MODIF ID lop.
  PARAMETERS p_dur TYPE i DEFAULT 600 MODIF ID lop.
  SELECTION-SCREEN END OF LINE.
SELECTION-SCREEN END OF BLOCK run.

" ── Monitor ───────────────────────────────────────────────────────────────────
" What ops looks at when the phone rings: one row per target, worst first, with
" the reason it is worst. Reads the SAME views the CLI, the TUI and the metrics
" endpoint read -- four surfaces over one definition of "healthy", rather than
" four queries that disagree the first time any of them changes.
"
" On this report rather than a new one: E-FOOTPRINT asserts the delivered object
" list exactly, and a monitor is not worth spending a report and a transaction
" code out of a fixed budget when ops already runs this program.
SELECTION-SCREEN BEGIN OF BLOCK mon WITH FRAME TITLE t_mon.
  SELECTION-SCREEN BEGIN OF LINE.
  PARAMETERS p_mon AS CHECKBOX.
  SELECTION-SCREEN COMMENT 4(60) c_mon FOR FIELD p_mon.
  SELECTION-SCREEN END OF LINE.
SELECTION-SCREEN END OF BLOCK mon.

" ── Schedule a periodic background job ────────────────────────────────────────
SELECTION-SCREEN BEGIN OF BLOCK sch WITH FRAME TITLE t_sch.
  SELECTION-SCREEN COMMENT /1(79) c_sch1.
  SELECTION-SCREEN BEGIN OF LINE.
  PARAMETERS p_sched AS CHECKBOX.
  SELECTION-SCREEN COMMENT 4(40) c_sched FOR FIELD p_sched.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN BEGIN OF LINE.
  SELECTION-SCREEN COMMENT 6(26) c_min FOR FIELD p_min.
  PARAMETERS p_min TYPE i DEFAULT 1.
  SELECTION-SCREEN END OF LINE.
  SELECTION-SCREEN BEGIN OF LINE.
  PARAMETERS p_unsch AS CHECKBOX.
  SELECTION-SCREEN COMMENT 4(40) c_unsch FOR FIELD p_unsch.
  SELECTION-SCREEN END OF LINE.
SELECTION-SCREEN END OF BLOCK sch.

INITIALIZATION.
  t_run   = 'Run delta now'.
  c_tgt   = 'Target (one table)'.
  c_tgt2  = 'Blank = run every DUE target; or name one to run it now (ignores cadence).'.
  c_once  = 'One tick, then stop  (the job step for periodic delta)'.
  c_loop  = 'Keep ticking  (foreground watch / sub-minute micro-batch)'.
  c_secs  = 'Tick every (seconds)'.
  c_dur   = 'Stop after (seconds)'.
  t_sch   = 'Schedule a periodic background job (the recommended way to run delta)'.
  c_sch1  = 'ONE periodic job; each tick runs every DUE target. One job drives all cadences.'.
  c_sched = 'Install / re-time the job'.
  c_min   = 'Every (minutes, min 1)'.
  c_unsch = 'Remove the job'.
  t_mon   = 'Monitor'.
  c_mon   = 'Show every target: lag, health, and why it is unhealthy'.

" The loop interval/duration only apply to "Keep ticking".
AT SELECTION-SCREEN OUTPUT.
  LOOP AT SCREEN.
    IF screen-group1 = 'LOP'.
      screen-input = COND i( WHEN p_loop = abap_true THEN 1 ELSE 0 ).
      MODIFY SCREEN.
    ENDIF.
  ENDLOOP.

AT SELECTION-SCREEN ON HELP-REQUEST FOR p_tgt.
  MESSAGE 'DuckDB target table to run now (e.g. mara). Blank = run every target whose cadence is due.' TYPE 'I'.
AT SELECTION-SCREEN ON HELP-REQUEST FOR p_secs.
  MESSAGE 'In "Keep ticking" mode: seconds between ticks (e.g. 10 for a fast demo).' TYPE 'I'.
AT SELECTION-SCREEN ON HELP-REQUEST FOR p_dur.
  MESSAGE 'In "Keep ticking" mode: stop the loop after this many seconds.' TYPE 'I'.
AT SELECTION-SCREEN ON HELP-REQUEST FOR p_min.
  MESSAGE 'Background-job period in minutes (minimum 1). For sub-minute, use "Keep ticking" instead.' TYPE 'I'.

START-OF-SELECTION.
  " Scheduling actions are setup, not a run: do them and stop.
  IF p_unsch = abap_true.
    WRITE: / icon_delete AS ICON, zcl_erpl_rev_delta=>schedule( iv_remove = abap_true ).
    RETURN.
  ENDIF.
  IF p_sched = abap_true.
    WRITE: / icon_alarm AS ICON, zcl_erpl_rev_delta=>schedule( iv_minutes = p_min ).
    RETURN.
  ENDIF.

  DATA lt_run TYPE zcl_erpl_rev_delta=>tt_run.

  " Drain anything the CLI queued before running the due targets. This is what
  " lets `erpl-rev sync`/`replicate` work for a caller with no SAP
  " authorisation at all: the CLI writes a row into a local DuckDB table and
  " this job -- already running on a schedule -- picks it up. See issue #85.
  PERFORM drain_cli.

  IF p_mon = abap_true.
    PERFORM monitor.
    RETURN.
  ENDIF.

  IF p_tgt IS NOT INITIAL.
    APPEND zcl_erpl_rev_delta=>run( p_tgt ) TO lt_run.
    PERFORM show USING lt_run.
    RETURN.
  ENDIF.

  IF p_loop = abap_true.
    GET TIME STAMP FIELD DATA(lv_t0).
    DO.
      lt_run = zcl_erpl_rev_delta=>run_due( ).
      PERFORM show USING lt_run.
      GET TIME STAMP FIELD DATA(lv_tn).
      IF cl_abap_tstmp=>subtract( tstmp1 = lv_tn tstmp2 = lv_t0 ) >= p_dur. EXIT. ENDIF.
      WAIT UP TO p_secs SECONDS.
    ENDDO.
  ELSE.
    lt_run = zcl_erpl_rev_delta=>run_due( ).
    PERFORM show USING lt_run.
  ENDIF.

*&---------------------------------------------------------------------*
*&  Run whatever the CLI queued, and report it in the job log.
*&---------------------------------------------------------------------*
FORM drain_cli.
  DATA(lt_cmd) = zcl_erpl_rev_clidrv=>drain( ).

  " Daemon starter. This job already runs every minute and already drains the
  " CLI queue, so it is also the cheapest place to notice that the streaming
  " daemon is gone -- after a system restart, or a cancelled job -- and put it
  " back. A dedicated watchdog job would cost another slot to do the same thing.
  PERFORM restart_daemon_if_stale.
  LOOP AT lt_cmd INTO DATA(ls_cmd).
    WRITE: / |cli { ls_cmd-cmd_id } { ls_cmd-verb } { ls_cmd-status } | &&
             |{ ls_cmd-result }{ ls_cmd-error }|.
  ENDLOOP.
ENDFORM.

*&---------------------------------------------------------------------*
*&  Show one tick's results as a compact, coloured table.
*&---------------------------------------------------------------------*
FORM show USING it_run TYPE zcl_erpl_rev_delta=>tt_run.
  WRITE: / |{ sy-datum DATE = USER } { sy-uzeit TIME = USER }| COLOR COL_GROUP, '— delta tick'.
  IF it_run IS INITIAL.
    WRITE: / icon_led_yellow AS ICON, 'No targets due this tick.'.
    ULINE.
    RETURN.
  ENDIF.

  FORMAT COLOR COL_HEADING.
  WRITE: /(22) 'Target', (12) 'Method', (8) 'rows', (6) 'ins', (6) 'upd', (6) 'del', 'Watermark'.
  FORMAT COLOR OFF.
  LOOP AT it_run INTO DATA(ls).
    IF ls-skipped = abap_true.
      WRITE: / icon_led_inactive AS ICON, (22) ls-target, 'skipped — another cycle holds the lease' COLOR COL_TOTAL.
    ELSEIF ls-error IS NOT INITIAL.
      WRITE: / icon_led_red AS ICON, (22) ls-target, (12) ls-method, 'ERROR:' COLOR COL_NEGATIVE, ls-error.
    ELSE.
      WRITE: / icon_led_green AS ICON, (22) ls-target, (12) ls-method,
               (8) ls-rows, (6) ls-ins, (6) ls-upd, (6) ls-del, ls-wm COLOR COL_NORMAL.
    ENDIF.
  ENDLOOP.
  ULINE.
ENDFORM.


*&---------------------------------------------------------------------*
*& Re-submit Z_ERPL_REV_DAEMON when its heartbeat has gone stale.
*&
*& "Stale" is judged against the daemon's own tick interval, read from the
*& same row, so a deliberately slow tick is not mistaken for a dead daemon.
*& The daemon itself claims the row atomically, so a race here is harmless:
*& the loser reports the winner and exits.
*&---------------------------------------------------------------------*
FORM restart_daemon_if_stale.
  DATA lv_rows TYPE string.
  DATA lv_err  TYPE string.

  CALL FUNCTION 'Z_DUCKDB_QUERY' DESTINATION 'ERPL_REV'
    EXPORTING iv_sql = |SELECT count(*) AS c FROM _erpl_rev_daemon | &&
                       |WHERE id=1 AND status='RUNNING' | &&
                       |AND (heartbeat_ts IS NULL | &&
                       |     OR heartbeat_ts < now() - INTERVAL '1' MINUTE)|
    IMPORTING ev_rows = lv_rows ev_error = lv_err
    EXCEPTIONS communication_failure = 1 system_failure = 2 OTHERS = 3.
  IF sy-subrc <> 0 OR lv_err IS NOT INITIAL.
    RETURN.   " server unreachable: nothing useful to do from here
  ENDIF.
  IF lv_rows NS '"c":1'.
    RETURN.   " either not running, or running and healthy
  ENDIF.

  DATA lv_jobname TYPE tbtcjob VALUE 'ERPL_REV_DAEMON'.
  DATA lv_jobcount TYPE tbtcjob-jobcount.
  CALL FUNCTION 'JOB_OPEN'
    EXPORTING jobname = lv_jobname
    IMPORTING jobcount = lv_jobcount
    EXCEPTIONS OTHERS = 1.
  IF sy-subrc <> 0. RETURN. ENDIF.

  SUBMIT z_erpl_rev_daemon VIA JOB lv_jobname NUMBER lv_jobcount AND RETURN.

  CALL FUNCTION 'JOB_CLOSE'
    EXPORTING jobname = lv_jobname jobcount = lv_jobcount strtimmed = abap_true
    EXCEPTIONS OTHERS = 1.
ENDFORM.

*&---------------------------------------------------------------------*
*& The monitor screen.
*&
*& Health first, because it answers "is anything wrong" before the eye
*& reaches the table, then one ALV row per target ordered worst-first.
*& The data comes from zcl_erpl_rev_delta=>monitor_rows, which is the same
*& call the e2e asserts on -- the screen adds no logic of its own.
*&---------------------------------------------------------------------*
FORM monitor.
  DATA(ls_h) = zcl_erpl_rev_delta=>monitor_health( ).
  IF ls_h-error IS NOT INITIAL.
    MESSAGE |monitor: { ls_h-error }| TYPE 'I'.
    RETURN.
  ENDIF.
  WRITE: / 'erpl-rev monitor'.
  WRITE: / ls_h-rows.
  SKIP.

  DATA(ls_t) = zcl_erpl_rev_delta=>monitor_rows( ).
  IF ls_t-error IS NOT INITIAL.
    MESSAGE |monitor: { ls_t-error }| TYPE 'I'.
    RETURN.
  ENDIF.
  IF ls_t-row_count = 0.
    WRITE: / 'no registered targets'.
    RETURN.
  ENDIF.

  " result_to_alv builds a typed table from the result's own columns, so the
  " grid follows the view rather than a hand-maintained field catalogue that
  " would go stale the moment a column is added.
  DATA(lr_tab) = zcl_erpl_rev_util=>result_to_alv( ls_t ).
  FIELD-SYMBOLS <mon> TYPE STANDARD TABLE.
  ASSIGN lr_tab->* TO <mon>.
  IF <mon> IS NOT ASSIGNED. RETURN. ENDIF.

  DATA lo_alv TYPE REF TO cl_salv_table.
  TRY.
      cl_salv_table=>factory( IMPORTING r_salv_table = lo_alv
                              CHANGING  t_table      = <mon> ).
      lo_alv->get_functions( )->set_all( ).
      lo_alv->get_columns( )->set_optimize( abap_true ).
      lo_alv->display( ).
    CATCH cx_root INTO DATA(lx_alv).
      MESSAGE |monitor: { lx_alv->get_text( ) }| TYPE 'I'.
  ENDTRY.
ENDFORM.
