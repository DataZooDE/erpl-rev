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
    "! Run one queued command by name. Public so a test can drive the exact
    "! path a CLI verb takes: the CLI parses arguments and queues; drain claims
    "! the row and calls this. Testing the verb without the parser is testing
    "! the whole mechanism.
    CLASS-METHODS execute
      IMPORTING iv_verb        TYPE string
                iv_params      TYPE string
      EXPORTING ev_result      TYPE string
                ev_error       TYPE string.

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

    " Every branch, inside one TRY. A dump anywhere in here reaches the operator
    " as "SAP server internal error" with no detail, and leaves the queue row
    " claimed forever -- so the command can never be retried and never reports.
    " Converting it to an error message is the difference between a diagnosable
    " failure and a mute one.
    TRY.
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

      WHEN 'mass_run'.
        " The server cuts the portions and persists them BEFORE any worker
        " starts -- that is what makes the run restartable -- and ABAP dispatches
        " them through the one portion runner the daemon also uses.
        DATA(lv_mt) = jstr( iv_json = iv_params iv_key = 'target' ).
        DATA(lv_ms) = jstr( iv_json = iv_params iv_key = 'source' ).
        DATA(lv_pc) = jstr( iv_json = iv_params iv_key = 'part_col' ).
        DATA(lv_facts) = ``.
        IF lv_pc IS NOT INITIAL AND lv_ms IS NOT INITIAL.
          " Scalar MIN/MAX and a count -- the shape parallel replication has used
          " here for a long time. A per-value histogram would be a better input,
          " but it needs a multi-row dynamic SELECT: an inline target cannot
          " infer a type from a dynamic select list, and a declared target that
          " does not fit the columns dumps the work process instead of raising.
          "
          " These are FACTS. Every boundary is still decided by the server, so
          " one code path cuts every strategy.
          DATA: lv_mn TYPE c LENGTH 40, lv_mx TYPE c LENGTH 40.
          DATA lv_tot TYPE i.
          TRY.
              DATA(lv_mmsel) = |MIN( { lv_pc } ), MAX( { lv_pc } )|.
              SELECT (lv_mmsel) FROM (lv_ms) INTO (@lv_mn, @lv_mx).
              ENDSELECT.
              SELECT COUNT(*) FROM (lv_ms) INTO @lv_tot.
            CATCH cx_root INTO DATA(lx_h).
              ev_error = |bounds of { lv_ms }.{ lv_pc } could not be read: | &&
                         |{ lx_h->get_text( ) }|.
              RETURN.
          ENDTRY.
          IF lv_tot = 0.
            ev_error = |{ lv_ms } has no rows to load|.
            RETURN.
          ENDIF.
          lv_facts = |"range_min":"{ condense( CONV string( lv_mn ) ) }",| &&
                     |"range_max":"{ condense( CONV string( lv_mx ) ) }",| &&
                     |"total_rows":{ lv_tot },|.
        ELSE.
          ev_error = |mass run needs --part-col: the portions are cut on it|.
          RETURN.
        ENDIF.

        " The facts are spliced into the params the CLI sent, so the server sees
        " one object with everything it needs.
        DATA(lv_mp) = iv_params.
        REPLACE FIRST OCCURRENCE OF `{` IN lv_mp WITH |\{{ lv_facts }|.

        DATA(ls_sp2) = zcl_erpl_rev_delta=>plan_json(
          iv_action = 'SPLIT' iv_target = lv_mt iv_params = lv_mp ).
        IF ls_sp2-error IS NOT INITIAL.
          ev_error = ls_sp2-error.
        ELSE.
          " Predicates in the order the server numbered them: a portion list
          " reordered here would not match the persisted one a restart reads.
          DATA(lt_pred) = zcl_erpl_rev_delta=>jarr_items( iv_json = ls_sp2-json
                                                          iv_key = 'predicates' ).
          DATA lt_port TYPE zcl_erpl_rev_util=>tt_portion.
          LOOP AT lt_pred INTO DATA(lv_pr).
            APPEND VALUE #( portion_no = sy-tabix predicate = lv_pr ) TO lt_port.
          ENDLOOP.
          IF lt_port IS INITIAL.
            ev_error = |SPLIT produced no portions|.
          ELSE.
            " EVERY optional parameter supplied, explicitly.
            "
            " replicate_portions takes iv_columns/iv_init/iv_params as
            " generically typed OPTIONAL parameters and passes them on. Leaving
            " one unsupplied does not default it -- it has no value at all, and
            " the first method that reads it dumps the work process. That
            " reaches the operator as "SAP server internal error" and says
            " nothing, which cost a long afternoon to find.
            DATA(ls_mr) = zcl_erpl_rev_util=>replicate_portions(
              iv_tab      = lv_ms
              iv_target   = lv_mt
              it_portions = lt_port
              iv_recreate = abap_true
              iv_columns  = ``
              iv_init     = ``
              iv_batch    = 0
              iv_params   = `` ).
            ev_error = ls_mr-error.
            IF ev_error IS INITIAL.
              ev_result = |{ lines( lt_port ) } portion(s), { ls_mr-rows_affected } rows|.
            ENDIF.
          ENDIF.
        ENDIF.

      WHEN 'cdc_status' OR 'cdc_repair'.
        " Two round trips, because the catalogue lives in HANA and the registry
        " lives in DuckDB and neither can see the other. The server says what to
        " ask; ABAP asks the database; the server derives the verdict.
        DATA(lv_ct) = jstr( iv_json = iv_params iv_key = 'target' ).
        DATA(ls_pr) = zcl_erpl_rev_delta=>plan_json(
          iv_action = 'CDC_PROBE' iv_target = lv_ct ).
        IF ls_pr-error IS NOT INITIAL.
          ev_error = ls_pr-error.
        ELSE.
          DATA(lv_tj) = zcl_erpl_rev_cdc=>probe_names(
                          zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                    iv_key = 'tables_sql' ) ).
          DATA(lv_sj) = zcl_erpl_rev_cdc=>probe_names(
                          zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                    iv_key = 'sequences_sql' ) ).
          DATA(lv_gj) = zcl_erpl_rev_cdc=>probe_names(
                          zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                    iv_key = 'triggers_sql' ) ).
          IF lv_tj IS INITIAL OR lv_sj IS INITIAL OR lv_gj IS INITIAL.
            " A probe that could not run is not an empty catalogue. Deriving a
            " status from it would mark a healthy target INCONSISTENT.
            ev_error = |CDC probe failed: the catalogue could not be read|.
          ELSE.
            " The triggers come back as "<name>:<IS_VALID>"; split them into the
            " enabled and disabled sets the derivation compares against.
            DATA lv_en TYPE string VALUE `[`.
            DATA lv_di TYPE string VALUE `[`.
            SPLIT lv_gj AT ',' INTO TABLE DATA(lt_tg).
            LOOP AT lt_tg INTO DATA(lv_tg).
              REPLACE ALL OCCURRENCES OF `[` IN lv_tg WITH ``.
              REPLACE ALL OCCURRENCES OF `]` IN lv_tg WITH ``.
              REPLACE ALL OCCURRENCES OF `"` IN lv_tg WITH ``.
              IF lv_tg IS INITIAL. CONTINUE. ENDIF.
              SPLIT lv_tg AT ':' INTO DATA(lv_nm) DATA(lv_fl).
              IF lv_fl = 'TRUE' OR lv_fl = 'true' OR lv_fl = 'X'.
                IF lv_en <> `[`. lv_en = lv_en && `,`. ENDIF.
                lv_en = lv_en && `"` && lv_nm && `"`.
              ELSE.
                IF lv_di <> `[`. lv_di = lv_di && `,`. ENDIF.
                lv_di = lv_di && `"` && lv_nm && `"`.
              ENDIF.
            ENDLOOP.
            lv_en = lv_en && `]`.
            lv_di = lv_di && `]`.

            DATA(lv_sp) = |\{"tables":{ lv_tj },"sequences":{ lv_sj },| &&
                          |"enabled_triggers":{ lv_en },"disabled_triggers":{ lv_di },| &&
                          |"log_table":"{ zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json iv_key = 'log_table' ) }",| &&
                          |"seq_name":"{ zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json iv_key = 'seq_name' ) }",| &&
                          |"triggers":{ zcl_erpl_rev_delta=>jarr( iv_json = ls_pr-json iv_key = 'triggers' ) }\}|.
            DATA(ls_st) = zcl_erpl_rev_delta=>plan_json(
              iv_action = 'CDC_STATUS' iv_target = lv_ct iv_params = lv_sp ).
            ev_error  = ls_st-error.
            ev_result = ls_st-json.

            " repair re-creates ONLY what the probe found missing. Re-running the
            " whole provision DDL would recreate the shadow table and reset the
            " position, discarding every change captured since.
            IF iv_verb = 'cdc_repair' AND ev_error IS INITIAL.
              DATA(ls_rp) = zcl_erpl_rev_delta=>plan_json(
                iv_action = 'CDC_REPAIR' iv_target = lv_ct iv_params = lv_sp ).
              IF ls_rp-error IS NOT INITIAL.
                ev_error = ls_rp-error.
              ELSE.
                " Parsed, not split. A CREATE TRIGGER body is full of commas
                " and quoted identifiers, so splitting the raw array on a
                " separator produces fragments that are not SQL.
                DATA(lt_ddl) = zcl_erpl_rev_delta=>jarr_items( iv_json = ls_rp-json
                                                               iv_key = 'ddl' ).
                LOOP AT lt_ddl INTO DATA(lv_one).
                  IF lv_one IS INITIAL. CONTINUE. ENDIF.
                  DATA(lv_er) = zcl_erpl_rev_cdc=>repair_exec( lv_one ).
                  IF lv_er IS NOT INITIAL. ev_error = lv_er. EXIT. ENDIF.
                ENDLOOP.
                IF ev_error IS INITIAL.
                  " Re-derive and PERSIST the status. Without this the repair
                  " recreated the objects and left the target INCONSISTENT, so
                  " the tick planner went on skipping it: the operator is told
                  " the repair worked and replication stays stopped. The status
                  " is derived from the catalogue, so the only way to know it is
                  " fixed is to look again.
                  DATA(lv_tj2) = zcl_erpl_rev_cdc=>probe_names(
                                   zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                             iv_key = 'tables_sql' ) ).
                  DATA(lv_sj2) = zcl_erpl_rev_cdc=>probe_names(
                                   zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                             iv_key = 'sequences_sql' ) ).
                  DATA(lv_gj2) = zcl_erpl_rev_cdc=>probe_names(
                                   zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json
                                                             iv_key = 'triggers_sql' ) ).
                  IF lv_tj2 IS NOT INITIAL AND lv_sj2 IS NOT INITIAL AND lv_gj2 IS NOT INITIAL.
                    DATA lv_en2 TYPE string VALUE `[`.
                    DATA lv_di2 TYPE string VALUE `[`.
                    SPLIT lv_gj2 AT ',' INTO TABLE DATA(lt_tg2).
                    LOOP AT lt_tg2 INTO DATA(lv_tg2).
                      REPLACE ALL OCCURRENCES OF `[` IN lv_tg2 WITH ``.
                      REPLACE ALL OCCURRENCES OF `]` IN lv_tg2 WITH ``.
                      REPLACE ALL OCCURRENCES OF `"` IN lv_tg2 WITH ``.
                      IF lv_tg2 IS INITIAL. CONTINUE. ENDIF.
                      SPLIT lv_tg2 AT ':' INTO DATA(lv_nm2) DATA(lv_fl2).
                      IF lv_fl2 = 'TRUE' OR lv_fl2 = 'true' OR lv_fl2 = 'X'.
                        IF lv_en2 <> `[`. lv_en2 = lv_en2 && `,`. ENDIF.
                        lv_en2 = lv_en2 && `"` && lv_nm2 && `"`.
                      ELSE.
                        IF lv_di2 <> `[`. lv_di2 = lv_di2 && `,`. ENDIF.
                        lv_di2 = lv_di2 && `"` && lv_nm2 && `"`.
                      ENDIF.
                    ENDLOOP.
                    lv_en2 = lv_en2 && `]`.
                    lv_di2 = lv_di2 && `]`.
                    DATA(lv_sp3) = |\{"tables":{ lv_tj2 },"sequences":{ lv_sj2 },| &&
                                   |"enabled_triggers":{ lv_en2 },"disabled_triggers":{ lv_di2 },| &&
                                   |"log_table":"{ zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json iv_key = 'log_table' ) }",| &&
                                   |"seq_name":"{ zcl_erpl_rev_delta=>jstr( iv_json = ls_pr-json iv_key = 'seq_name' ) }",| &&
                                   |"triggers":{ zcl_erpl_rev_delta=>jarr( iv_json = ls_pr-json iv_key = 'triggers' ) }\}|.
                    DATA(ls_st2) = zcl_erpl_rev_delta=>plan_json(
                      iv_action = 'CDC_STATUS' iv_target = lv_ct iv_params = lv_sp3 ).
                    ev_result = |repaired { lines( lt_ddl ) } object(s); { ls_st2-json }|.
                  ELSE.
                    ev_result = |repaired { lines( lt_ddl ) } object(s)|.
                  ENDIF.
                ENDIF.
              ENDIF.
            ENDIF.
          ENDIF.
        ENDIF.

      WHEN 'validate'.
        " Two-sided, cell by cell. A replica that is the right SIZE and the
        " wrong CONTENT passes every count check there is, so both sides render
        " the same columns as canonical text and the fingerprints are compared.
        "
        " The SAP half is rendered here because it has to be: fingerprint_cell
        " turns a DDIC-typed value into that text, and moving it to the server
        " would duplicate the DDIC knowledge where the two could silently
        " disagree -- which is the whole failure this feature exists to catch.
        DATA(lv_vt) = jstr( iv_json = iv_params iv_key = 'target' ).
        DATA(ls_vs) = zcl_erpl_rev_delta=>state( lv_vt ).
        DATA(lv_vsrc) = COND string( WHEN ls_vs-source_from IS NOT INITIAL
                                     THEN ls_vs-source_from ELSE lv_vt ).
        DATA(lv_vn) = zcl_erpl_rev_delta=>jstr( iv_json = iv_params iv_key = 'sample_rows' ).
        DATA(lv_vfull) = xsdbool( jstr( iv_json = iv_params iv_key = 'mode' ) = 'full' ).
        " Guarded: --sample-rows is an operator's free text, and CONV i( 'all' )
        " dumps the work process and orphans the queue row forever.
        DATA lv_vmax TYPE i VALUE 1000.
        IF lv_vn CO '0123456789' AND lv_vn IS NOT INITIAL.
          lv_vmax = CONV i( lv_vn ).
        ENDIF.
        IF lv_vmax <= 0. lv_vmax = 1000. ENDIF.

        DATA(ls_vd) = zcl_erpl_rev_util=>describe_table( iv_tab = lv_vsrc iv_target = lv_vt ).
        IF ls_vd-error IS NOT INITIAL.
          ev_error = ls_vd-error.
        ELSE.
          " The comparable columns, in one order, used for both the ORDER BY and
          " the fingerprint. Position is the contract between the two sides.
          DATA lt_vf TYPE string_table.
          " The registered key columns, in order, so both sides render the same
          " identity for the same row.
          DATA(lt_vkeys) = VALUE string_table( ).
          SPLIT ls_vs-keys AT ',' INTO TABLE lt_vkeys.
          DATA lv_vfj TYPE string VALUE `[`.
          LOOP AT ls_vd-fields INTO DATA(ls_vfl).
            " FLTP is excluded on both sides: binary floating point does not
            " round-trip through decimal text and would report mismatches on
            " correct data.
            IF ls_vfl-datatype = 'FLTP'. CONTINUE. ENDIF.
            APPEND ls_vfl-name TO lt_vf.
            IF lv_vfj <> `[`. lv_vfj = lv_vfj && `,`. ENDIF.
            lv_vfj = lv_vfj && |\{"name":"{ ls_vfl-name }","datatype":"{ ls_vfl-datatype }",| &&
                     |"length":{ ls_vfl-length },"decimals":{ ls_vfl-decimals }\}|.
          ENDLOOP.
          lv_vfj = lv_vfj && `]`.

          IF lt_vf IS INITIAL.
            ev_error = |{ lv_vt } has no comparable columns|.
          ELSE.
            DATA(lv_vcols) = concat_lines_of( table = lt_vf sep = `,` ).
            DATA lv_vrows TYPE string VALUE `[`.
            " The row type comes from the SOURCE's own DDIC definition. A
            " dynamic select list gives the compiler nothing to infer, so an
            " inline target is not allowed -- and a hand-declared one that does
            " not fit the columns dumps the work process rather than raising.
            DATA lo_vt TYPE REF TO data.
            FIELD-SYMBOLS <vtab> TYPE STANDARD TABLE.
            FIELD-SYMBOLS <vr> TYPE any.
            FIELD-SYMBOLS <vc> TYPE any.
            TRY.
                CREATE DATA lo_vt TYPE TABLE OF (lv_vsrc).
                ASSIGN lo_vt->* TO <vtab>.
                " --full means every row. The cap was applied regardless, so a
                " full validation of any table over the sample size compared
                " 1000 SAP rows against the whole replica and reported FAILED on
                " the row-count difference -- the one verdict an operator acts
                " on destructively.
                IF lv_vfull = abap_true.
                  SELECT (lv_vcols) FROM (lv_vsrc)
                    ORDER BY (lv_vcols)
                    INTO CORRESPONDING FIELDS OF TABLE @<vtab>.
                ELSE.
                  SELECT (lv_vcols) FROM (lv_vsrc)
                    ORDER BY (lv_vcols)
                    INTO CORRESPONDING FIELDS OF TABLE @<vtab> UP TO @lv_vmax ROWS.
                ENDIF.
                LOOP AT <vtab> ASSIGNING <vr>.
                  DATA(lv_fp) = ``.
                  LOOP AT ls_vd-fields INTO DATA(ls_vf2).
                    IF ls_vf2-datatype = 'FLTP'. CONTINUE. ENDIF.
                    ASSIGN COMPONENT ls_vf2-name OF STRUCTURE <vr> TO <vc>.
                    IF <vc> IS NOT ASSIGNED. CONTINUE. ENDIF.
                    IF lv_fp IS NOT INITIAL. lv_fp = lv_fp && `|`. ENDIF.
                    lv_fp = lv_fp && zcl_erpl_rev_util=>fingerprint_cell(
                                       is_field = ls_vf2 iv_val = <vc> ).
                  ENDLOOP.
                  " The row's IDENTITY alongside its fingerprint. Sending
                  " fingerprints alone made the two sides pair by position,
                  " which assumes HANA and DuckDB order a column identically --
                  " they need not, and a single misalignment reports every row
                  " after it as wrong.
                  DATA(lv_k) = ``.
                  LOOP AT lt_vkeys INTO DATA(lv_kc).
                    ASSIGN COMPONENT lv_kc OF STRUCTURE <vr> TO <vc>.
                    IF <vc> IS NOT ASSIGNED. CONTINUE. ENDIF.
                    READ TABLE ls_vd-fields INTO DATA(ls_kf) WITH KEY name = lv_kc.
                    IF sy-subrc <> 0. CONTINUE. ENDIF.
                    IF lv_k IS NOT INITIAL. lv_k = lv_k && `|`. ENDIF.
                    lv_k = lv_k && zcl_erpl_rev_util=>fingerprint_cell(
                                     is_field = ls_kf iv_val = <vc> ).
                  ENDLOOP.
                  " No registered keys: the row is its own identity, which still
                  " pairs identical rows and still reports one that exists on
                  " only one side.
                  IF lv_k IS INITIAL. lv_k = lv_fp. ENDIF.

                  IF lv_vrows <> `[`. lv_vrows = lv_vrows && `,`. ENDIF.
                  lv_vrows = lv_vrows &&
                    |\{"k":"{ escape( val = lv_k format = cl_abap_format=>e_json_string ) }",| &&
                    |"fp":"{ escape( val = lv_fp format = cl_abap_format=>e_json_string ) }"\}|.
                ENDLOOP.
                lv_vrows = lv_vrows && `]`.
              CATCH cx_root INTO DATA(lx_v).
                ev_error = |reading { lv_vsrc } for validation failed: { lx_v->get_text( ) }|.
                RETURN.
            ENDTRY.

            DATA(lv_vp) = |\{"fields":{ lv_vfj },"rows":{ lv_vrows },| &&
                          |"mode":"{ jstr( iv_json = iv_params iv_key = 'mode' ) }",| &&
                          |"sample_rows":{ lv_vmax }\}|.
            DATA(ls_vr) = zcl_erpl_rev_delta=>plan_json(
              iv_action = 'VALIDATE' iv_target = lv_vt iv_params = lv_vp ).
            ev_error  = ls_vr-error.
            ev_result = ls_vr-json.
          ENDIF.
        ENDIF.

      WHEN 'unpark'.
        DATA(ls_up) = zcl_erpl_rev_delta=>plan_json(
          iv_action = 'UNPARK'
          iv_target = jstr( iv_json = iv_params iv_key = 'target' ) ).
        ev_error  = ls_up-error.
        ev_result = ls_up-json.

      WHEN 'set_wm' OR 'preview'.
        " Server-side actions: set_wm moves the position and records the run
        " that moved it in one transaction; preview reads through whatever a
        " subscriber would read.
        DATA(ls_op) = zcl_erpl_rev_delta=>plan_json(
          iv_action = COND string( WHEN iv_verb = 'set_wm' THEN 'SET_WM' ELSE 'PREVIEW' )
          iv_target = jstr( iv_json = iv_params iv_key = 'target' )
          iv_params = iv_params ).
        ev_error  = ls_op-error.
        ev_result = ls_op-json.

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
      CATCH cx_root INTO DATA(lx_verb).
        ev_error = |{ iv_verb } failed: { lx_verb->get_text( ) }|.
    ENDTRY.
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
