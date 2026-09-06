"! <p class="shorttext">erpl-rev CLI command driver</p>
"!
"! Executes commands the CLI queued in the DuckDB table `_erpl_rev_cli_cmd`.
"!
"! Why this exists: `erpl-adt object run` executes a class that takes no
"! parameters, so the first version of the CLI generated a class per command
"! with the parameters written into its source. That works, but it needs
"! S_DEVELOP -- a developer authorisation the erpl-rev service user does not
"! have on a production system -- and it makes every value the user types into
"! ABAP source, which is an injection surface that has to be defended.
"!
"! Here the parameters arrive as *data*, in a JSON column, and are only ever
"! read as values. Nothing is generated, nothing is created, nothing is deleted.
"!
"! Two things can drive it, and both end up here:
"!   - `object run ZCL_ERPL_REV_CLIDRV`, when the caller may run a classrun;
"!   - the periodic Z_ERPL_REV_DELTA heartbeat, which drains the queue on each
"!     tick -- needing no ADT call, and so no SAP authorisation, from the CLI.
CLASS zcl_erpl_rev_clidrv DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.

    TYPES: BEGIN OF ty_done,
             cmd_id TYPE string,
             verb   TYPE string,
             status TYPE string,
             result TYPE string,
             error  TYPE string,
           END OF ty_done.
    TYPES tt_done TYPE STANDARD TABLE OF ty_done WITH EMPTY KEY.

    "! Run every command currently pending, oldest first. Returns one row per
    "! command executed; an empty table means the queue was empty.
    "! `iv_max` bounds one drain so a backlog cannot monopolise a job step.
    CLASS-METHODS drain
      IMPORTING iv_max        TYPE i DEFAULT 20
      RETURNING VALUE(rt)     TYPE tt_done.

  PRIVATE SECTION.
    "! Claim the oldest pending command, returning its fields as one JSON row.
    CLASS-METHODS claim
      RETURNING VALUE(rs) TYPE zcl_erpl_rev_util=>ty_query.
    CLASS-METHODS execute
      IMPORTING iv_verb        TYPE string
                iv_params      TYPE string
      EXPORTING ev_result      TYPE string
                ev_error       TYPE string.
    CLASS-METHODS finish
      IMPORTING iv_id     TYPE string
                iv_result TYPE string
                iv_error  TYPE string.
    "! Read one string out of a flat JSON object. Values are data, never source.
    CLASS-METHODS jstr
      IMPORTING iv_json   TYPE string
                iv_key    TYPE string
      RETURNING VALUE(rv) TYPE string.
    CLASS-METHODS jint
      IMPORTING iv_json   TYPE string
                iv_key    TYPE string
                iv_def    TYPE i DEFAULT 0
      RETURNING VALUE(rv) TYPE i.
    "! Escape a value for a SQL string literal (doubling the apostrophe). Only
    "! ever applied to values we are writing *back*, never to user parameters
    "! on the way in -- those are read, not concatenated.
    CLASS-METHODS q
      IMPORTING iv_in     TYPE string
      RETURNING VALUE(rv) TYPE string.
ENDCLASS.

CLASS zcl_erpl_rev_clidrv IMPLEMENTATION.

  METHOD if_oo_adt_classrun~main.
    DATA(lt) = drain( ).
    out->write( |ERPL-DRV count={ lines( lt ) }| ).
    LOOP AT lt INTO DATA(ls).
      out->write( |ERPL-DRV id={ ls-cmd_id };verb={ ls-verb };status={ ls-status }| &&
                  |;result={ ls-result };error={ ls-error }| ).
    ENDLOOP.
  ENDMETHOD.

  METHOD drain.
    " One command per iteration. zcl_erpl_rev_util=>query returns its rows as a
    " JSON array *string*, so claiming one at a time avoids parsing an array --
    " and it means a command that dumps cannot take the rest of the batch with
    " it, since each claim is its own statement.
    DO iv_max TIMES.
      DATA(ls_q) = claim( ).
      IF ls_q-error IS NOT INITIAL.
        APPEND VALUE #( status = 'ERROR' error = ls_q-error ) TO rt.
        RETURN.
      ENDIF.

      DATA(lv_id) = jstr( iv_json = ls_q-rows iv_key = 'cmd_id' ).
      IF lv_id IS INITIAL.
        RETURN.            " queue empty
      ENDIF.

      DATA(lv_verb) = jstr( iv_json = ls_q-rows iv_key = 'verb' ).
      DATA(lv_par)  = jstr( iv_json = ls_q-rows iv_key = 'params' ).

      execute( EXPORTING iv_verb = lv_verb iv_params = lv_par
               IMPORTING ev_result = DATA(lv_res) ev_error = DATA(lv_err) ).
      finish( iv_id = lv_id iv_result = lv_res iv_error = lv_err ).

      APPEND VALUE #( cmd_id = lv_id verb = lv_verb
                      status = COND string( WHEN lv_err IS INITIAL THEN 'DONE' ELSE 'ERROR' )
                      result = lv_res error = lv_err ) TO rt.
    ENDDO.
  ENDMETHOD.

  METHOD claim.
    " Claim and read in one statement: RETURNING hands back the row we just
    " marked, so two drivers racing cannot both take the same command.
    rs = zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_cli_cmd SET status = 'RUNNING', claimed_ts = now() | &&
      |WHERE cmd_id = ( SELECT cmd_id FROM _erpl_rev_cli_cmd | &&
      |                 WHERE status = 'PENDING' ORDER BY cmd_id LIMIT 1 ) | &&
      |RETURNING cmd_id, verb, params| ).
  ENDMETHOD.

  METHOD execute.
    CLEAR: ev_result, ev_error.

    CASE iv_verb.
      WHEN 'replicate'.
        DATA(ls_r) = zcl_erpl_rev_util=>replicate(
          iv_tab      = jstr( iv_json = iv_params iv_key = 'table' )
          iv_target   = jstr( iv_json = iv_params iv_key = 'target' )
          iv_columns  = jstr( iv_json = iv_params iv_key = 'columns' )
          iv_where    = jstr( iv_json = iv_params iv_key = 'where' )
          iv_params   = jstr( iv_json = iv_params iv_key = 'cds_params' )
          iv_init     = jstr( iv_json = iv_params iv_key = 'init' )
          iv_mode     = COND string( WHEN jstr( iv_json = iv_params iv_key = 'mode' ) IS INITIAL
                                     THEN 'UPSERT' ELSE jstr( iv_json = iv_params iv_key = 'mode' ) )
          iv_batch    = COND i( WHEN jint( iv_json = iv_params iv_key = 'batch' ) > 0
                                THEN jint( iv_json = iv_params iv_key = 'batch' ) ELSE 50000 )
          iv_maxrows  = jint( iv_json = iv_params iv_key = 'maxrows' )
          iv_truncate = COND abap_bool( WHEN jstr( iv_json = iv_params iv_key = 'truncate' ) = 'false'
                                        THEN abap_false ELSE abap_true ) ).
        ev_error  = ls_r-error.
        ev_result = |rows={ ls_r-rows_affected };seconds={ ls_r-seconds }|.

      WHEN 'sync_register'.
        " Every component of ty_state, by NAME, rather than a hand-written list.
        "
        " The hand-written list named ten of fifteen fields, so time_col,
        " safety_units, log_enabled, load_type_default and allow_empty_reload
        " were dropped on the floor by the DEFAULT CLI path -- the flags existed
        " on the command line, in the server, and in the schema, and did nothing.
        " This is the third writer of the same surface to drift, and each time
        " the symptom was the same: no compile error, no runtime error, the
        " feature simply does not happen. Iterating the structure makes a fourth
        " drift impossible -- a field added to ty_state is carried from the day
        " it is added.
        DATA ls_reg TYPE zcl_erpl_rev_delta=>ty_state.
        FIELD-SYMBOLS <lv_comp> TYPE any.
        DATA(lo_sd) = CAST cl_abap_structdescr(
                        cl_abap_typedescr=>describe_by_data( ls_reg ) ).
        LOOP AT lo_sd->components INTO DATA(ls_comp).
          ASSIGN COMPONENT ls_comp-name OF STRUCTURE ls_reg TO <lv_comp>.
          IF sy-subrc <> 0. CONTINUE. ENDIF.
          DATA(lv_key) = to_lower( CONV string( ls_comp-name ) ).
          IF ls_comp-type_kind = cl_abap_typedescr=>typekind_int.
            <lv_comp> = jint( iv_json = iv_params iv_key = lv_key ).
          ELSE.
            <lv_comp> = jstr( iv_json = iv_params iv_key = lv_key ).
          ENDIF.
        ENDLOOP.
        " The one field with a default rather than a blank: a zero safety window
        " on a clock kind means no overlap at all.
        IF ls_reg-safety_secs <= 0. ls_reg-safety_secs = 120. ENDIF.
        ev_error = zcl_erpl_rev_delta=>register( ls_reg ).
        IF ev_error IS INITIAL.
          ev_result = |registered { jstr( iv_json = iv_params iv_key = 'target' ) }|.
        ENDIF.

      WHEN 'sync_run'.
        DATA(lv_tgt) = jstr( iv_json = iv_params iv_key = 'target' ).
        DATA lt_run TYPE zcl_erpl_rev_delta=>tt_run.
        IF lv_tgt IS INITIAL.
          lt_run = zcl_erpl_rev_delta=>run_due( ).
        ELSE.
          APPEND zcl_erpl_rev_delta=>run( lv_tgt ) TO lt_run.
        ENDIF.
        LOOP AT lt_run INTO DATA(ls_run).
          ev_result = |{ ev_result }{ ls_run-target }:rows={ ls_run-rows },| &&
                      |ins={ ls_run-ins },upd={ ls_run-upd },del={ ls_run-del };|.
          IF ls_run-error IS NOT INITIAL.
            ev_error = |{ ev_error }{ ls_run-target }: { ls_run-error }; |.
          ENDIF.
        ENDLOOP.
        IF lt_run IS INITIAL.
          ev_result = 'nothing due'.
        ENDIF.

      WHEN 'daemon_status'.
        " The singleton row, as the operator sees it. A plain read, so it goes
        " through Z_DUCKDB_QUERY rather than a plan action.
        DATA(ls_dst) = zcl_erpl_rev_util=>query(
          |SELECT instance_id, status, ticks, tick_secs, max_workers, stop, | &&
          |epoch(now()) - epoch(heartbeat_ts) AS heartbeat_age_s | &&
          |FROM _erpl_rev_daemon WHERE id=1| ).
        ev_error  = ls_dst-error.
        ev_result = ls_dst-rows.

      WHEN 'daemon_stop'.
        " Sets the flag the daemon reads at the top of every tick. It used to be
        " reachable only by hand-written SQL.
        DATA(ls_dsp) = zcl_erpl_rev_util=>query(
          |UPDATE _erpl_rev_daemon SET stop=true WHERE id=1| ).
        ev_error  = ls_dsp-error.
        IF ev_error IS INITIAL.
          ev_result = |stop requested; the daemon ends at its next tick|.
        ENDIF.

      WHEN 'daemon_start'.
        " Optional tuning first, then the job. Both are idempotent: starting a
        " running daemon is refused by the singleton, not here.
        DATA(lv_tick) = jint( iv_json = iv_params iv_key = 'tick_secs' ).
        DATA(lv_wrk)  = jint( iv_json = iv_params iv_key = 'max_workers' ).
        IF lv_tick > 0.
          zcl_erpl_rev_util=>query(
            |UPDATE _erpl_rev_daemon SET tick_secs={ lv_tick } WHERE id=1| ).
        ENDIF.
        IF lv_wrk > 0.
          zcl_erpl_rev_util=>query(
            |UPDATE _erpl_rev_daemon SET max_workers={ lv_wrk } WHERE id=1| ).
        ENDIF.
        DATA lv_djn TYPE tbtcjob-jobname VALUE 'ERPL_REV_DAEMON'.
        DATA lv_djc TYPE tbtcjob-jobcount.
        CALL FUNCTION 'JOB_OPEN' EXPORTING jobname = lv_djn
          IMPORTING jobcount = lv_djc EXCEPTIONS OTHERS = 1.
        IF sy-subrc <> 0.
          ev_error = |JOB_OPEN failed subrc { sy-subrc }|.
        ELSE.
          DATA(lv_ts) = COND i( WHEN lv_tick > 0 THEN lv_tick ELSE 2 ).
          SUBMIT z_erpl_rev_daemon WITH p_secs = lv_ts WITH p_dur = 0
            VIA JOB lv_djn NUMBER lv_djc AND RETURN.
          CALL FUNCTION 'JOB_CLOSE' EXPORTING jobcount = lv_djc jobname = lv_djn
                                              strtimmed = abap_true EXCEPTIONS OTHERS = 1.
          IF sy-subrc <> 0.
            ev_error = |JOB_CLOSE failed subrc { sy-subrc }|.
          ELSE.
            ev_result = |daemon submitted as { lv_djn }/{ lv_djc }|.
          ENDIF.
        ENDIF.

      WHEN 'subs' OR 'retain'.
        " Both are server-side PLAN actions: the publish and the offset advance
        " are one transaction, and it happens on the server. The queue carries
        " the request, not the work.
        DATA(ls_pl) = zcl_erpl_rev_delta=>plan_json(
          iv_action = COND string( WHEN iv_verb = 'subs' THEN 'SUBS' ELSE 'RETAIN' )
          iv_target = jstr( iv_json = iv_params iv_key = 'target' )
          iv_params = iv_params ).
        ev_error  = ls_pl-error.
        ev_result = ls_pl-json.

      WHEN 'schedule'.
        ev_result = zcl_erpl_rev_delta=>schedule(
          iv_minutes = COND i( WHEN jint( iv_json = iv_params iv_key = 'minutes' ) > 0
                               THEN jint( iv_json = iv_params iv_key = 'minutes' ) ELSE 1 )
          iv_remove  = COND abap_bool( WHEN jstr( iv_json = iv_params iv_key = 'remove' ) = 'true'
                                       THEN abap_true ELSE abap_false ) ).
        IF ev_result CS 'ERROR:'.
          ev_error = ev_result.
        ENDIF.

      WHEN OTHERS.
        ev_error = |unknown verb '{ iv_verb }'|.
    ENDCASE.
  ENDMETHOD.

  METHOD finish.
    " Newlines would break the one-line result contract the CLI parses.
    DATA(lv_res) = replace( val = iv_result sub = cl_abap_char_utilities=>newline
                            with = ` ` occ = 0 ).
    DATA(lv_err) = replace( val = iv_error sub = cl_abap_char_utilities=>newline
                            with = ` ` occ = 0 ).
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_cli_cmd SET | &&
      |status = '{ COND string( WHEN lv_err IS INITIAL THEN 'DONE' ELSE 'ERROR' ) }', | &&
      |finished_ts = now(), result = '{ q( lv_res ) }', error = '{ q( lv_err ) }' | &&
      |WHERE cmd_id = { iv_id }| ).
  ENDMETHOD.

  METHOD jstr.
    " A deliberately small reader for the flat JSON the CLI writes: no nesting,
    " no arrays. /ui2/cl_json would drag a structure definition per verb into
    " this class for no benefit.
    DATA(lv_needle) = |"{ iv_key }":|.
    DATA(lv_off) = find( val = iv_json sub = lv_needle ).
    IF lv_off < 0.
      RETURN.
    ENDIF.
    DATA(lv_p) = lv_off + strlen( lv_needle ).
    WHILE lv_p < strlen( iv_json ) AND iv_json+lv_p(1) = ` `.
      lv_p = lv_p + 1.
    ENDWHILE.
    IF lv_p >= strlen( iv_json ).
      RETURN.
    ENDIF.
    IF iv_json+lv_p(1) <> '"'.
      " An unquoted scalar: a number, true/false or null. cmd_id arrives this
      " way, and reading only quoted values made the driver silently claim a
      " command and then decide there was nothing to run.
      WHILE lv_p < strlen( iv_json ).
        DATA(lv_u) = iv_json+lv_p(1).
        IF lv_u = ',' OR lv_u = '}' OR lv_u = ' '.
          EXIT.
        ENDIF.
        rv = rv && lv_u.
        lv_p = lv_p + 1.
      ENDWHILE.
      IF rv = 'null'.
        CLEAR rv.
      ENDIF.
      RETURN.
    ENDIF.
    lv_p = lv_p + 1.
    WHILE lv_p < strlen( iv_json ).
      DATA(lv_c) = iv_json+lv_p(1).
      IF lv_c = '\'.
        lv_p = lv_p + 1.
        IF lv_p < strlen( iv_json ).
          DATA(lv_e) = iv_json+lv_p(1).
          CASE lv_e.
            WHEN 'n'. rv = rv && cl_abap_char_utilities=>newline.
            WHEN 't'. rv = rv && cl_abap_char_utilities=>horizontal_tab.
            WHEN OTHERS. rv = rv && lv_e.
          ENDCASE.
          lv_p = lv_p + 1.
        ENDIF.
        CONTINUE.
      ENDIF.
      IF lv_c = '"'.
        EXIT.
      ENDIF.
      rv = rv && lv_c.
      lv_p = lv_p + 1.
    ENDWHILE.
  ENDMETHOD.

  METHOD jint.
    DATA(lv_s) = jstr( iv_json = iv_json iv_key = iv_key ).
    IF lv_s IS INITIAL.
      rv = iv_def.
      RETURN.
    ENDIF.
    TRY.
        rv = CONV i( lv_s ).
      CATCH cx_sy_conversion_error.
        rv = iv_def.
    ENDTRY.
  ENDMETHOD.

  METHOD q.
    rv = replace( val = iv_in sub = `'` with = `''` occ = 0 ).
  ENDMETHOD.

ENDCLASS.
