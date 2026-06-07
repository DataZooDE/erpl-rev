*&---------------------------------------------------------------------*
*& Report  Z_ERPL_REV_DELTA
*&---------------------------------------------------------------------*
*& Delta orchestration loop. On each tick it asks the server which targets are
*& DUE (cadence elapsed since last_run_ts, lease free) and runs one delta cycle
*& per target (lease -> dispatch by method -> commit -> release). All delta state
*& lives in the DuckDB table _erpl_rev_delta_state (no SAP Z table).
*&
*& Run modes:
*&   p_once  - one tick, then stop (the default — this is the job step you put on a
*&             periodic SAP background job for cron-style delta).
*&   p_loop  - keep ticking every p_secs seconds for up to p_dur seconds (handy
*&             for sub-minute micro-batch during a demo / a foreground watch).
*& A single explicit target (p_tgt) runs that target once regardless of cadence.
*&
*& Scheduling (the recommended way to run periodically):
*&   p_sched + p_min - install a PERIODIC background job 'ERPL_REV_DELTA' that runs
*&                     this report (p_once) every p_min minutes; each tick runs every
*&                     DUE target, so one job at the finest period drives all the
*&                     per-target cadences. Monitor/stop it in SM37. (min period 1
*&                     minute; sub-minute -> use p_loop or an external trigger.)
*&   p_unsch         - remove that periodic job.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_delta.

PARAMETERS:
  p_tgt  TYPE string,                         " run ONE target now (blank = all due)
  p_once TYPE abap_bool DEFAULT 'X' RADIOBUTTON GROUP m,
  p_loop TYPE abap_bool RADIOBUTTON GROUP m,
  p_secs TYPE i DEFAULT 60,                   " loop tick interval (seconds)
  p_dur  TYPE i DEFAULT 600.                  " loop max duration (seconds)
SELECTION-SCREEN SKIP.
PARAMETERS:
  p_sched AS CHECKBOX,                        " install the periodic background job
  p_min   TYPE i DEFAULT 1,                   " ... every p_min minutes
  p_unsch AS CHECKBOX.                        " remove the periodic background job

START-OF-SELECTION.
  " Scheduling actions are setup, not a run: do them and stop.
  IF p_unsch = abap_true.
    WRITE: / zcl_erpl_rev_delta=>schedule( iv_remove = abap_true ).
    RETURN.
  ENDIF.
  IF p_sched = abap_true.
    WRITE: / zcl_erpl_rev_delta=>schedule( iv_minutes = p_min ).
    RETURN.
  ENDIF.

  DATA lt_run TYPE zcl_erpl_rev_delta=>tt_run.

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

FORM show USING it_run TYPE zcl_erpl_rev_delta=>tt_run.
  IF it_run IS INITIAL.
    WRITE: / 'no targets due'.
    RETURN.
  ENDIF.
  LOOP AT it_run INTO DATA(ls).
    IF ls-skipped = abap_true.
      WRITE: / ls-target, '(skipped — lease held)'.
    ELSEIF ls-error IS NOT INITIAL.
      WRITE: / ls-target, ls-method, 'ERROR', ls-error.
    ELSE.
      WRITE: / ls-target, ls-method,
               'rows', ls-rows, 'ins', ls-ins, 'upd', ls-upd, 'del', ls-del,
               'wm', ls-wm.
    ENDIF.
  ENDLOOP.
ENDFORM.
