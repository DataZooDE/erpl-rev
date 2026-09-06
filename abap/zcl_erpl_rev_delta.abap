CLASS zcl_erpl_rev_delta DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    " Delta (incremental) extraction engine. The ABAP side stays a thin reader:
    " it selects the changed rows with plain Open SQL and streams them through the
    " existing replicate()/MERGE path; the merge, the snapshot diff and ALL delta
    " state live in the C++/DuckDB server (_erpl_rev_delta_state, read/written via
    " Z_DUCKDB_QUERY — no new SAP Z table). Four methods, one merge engine:
    "   WATERMARK   - chg_col > wm (numeric high-water): keyed upsert.
    "   INSERT_ONLY - append-only source driven by CDHDR change numbers (2-step):
    "                 keyed upsert (the DDIC key dedups re-delivered rows).
    "   CHANGEDOC   - CDHDR(objectclas) feed -> business keys -> re-read source by
    "                 key -> keyed upsert (catches every change path; e.g. MARA/MAKT).
    "   SNAPSHOT    - full reload into <target>__snap + server-side anti-join merge
    "                 (the only path that reflects PHYSICAL deletes).
    " Every cycle is idempotent (key-based merge) and re-runnable; a per-target
    " lease prevents overlapping ticks. See docs/delta.md and HLD §3-§7.

    CONSTANTS c_lease_ttl TYPE i VALUE 600.   " a RUNNING lease older than this is stale

    TYPES: BEGIN OF ty_state,
             target      TYPE string,
             method      TYPE string,
             source_from TYPE string,
             keys        TYPE string,
             chg_col     TYPE string,
             wm_kind     TYPE string,   " NUMTS | TIMESTAMPL | DATETIME | DATE | INT
             time_col    TYPE string,   " DATETIME only: the TIMS half of the pair
             wm_value    TYPE string,
             safety_secs TYPE i,        " overlap in seconds, clock-based kinds
             safety_units TYPE i,       " overlap in values, counter kinds (INT)
             cadence     TYPE string,   " micro:<sec> | hourly | nightly | manual
             extra       TYPE string,   " JSON, e.g. {"objectclas":"MATERIAL"}
             " Three controls the server reads and nothing could write. The
             " change log itself, the load type the daemon schedules, and the
             " escape hatch the reload refusal names in its own error message --
             " all reachable only by hand-written SQL until they were added
             " here. Exactly the failure the comment in register() warns about.
             " TRI-STATE, and string rather than abap_bool for exactly that
             " reason: 'true', 'false', or empty meaning "the caller did not
             " say". abap_bool has no third value, so a re-registration that
             " never mentioned logging was indistinguishable from one that
             " asked for it off -- and silently turned it off.
             log_enabled       TYPE string,      " keep a per-target change log
             load_type_default TYPE string,      " D | F | I | L; F and L are one-shot
             allow_empty_reload TYPE string,     " let a reload empty the target
             status      TYPE string,
           END OF ty_state.

    TYPES: BEGIN OF ty_run,
             target  TYPE string,
             method  TYPE string,
             rows    TYPE i,
             ins     TYPE i,
             upd     TYPE i,
             del     TYPE i,
             wm      TYPE string,
             skipped TYPE abap_bool,
             " The server already committed this cycle: it advanced the
             " watermark, released the target and finished its own statistics
             " row, all inside one transaction. ABAP must then do NONE of those
             " again -- a second, unfenced UPDATE of wm_value can move the
             " watermark BACKWARDS if another cycle claimed the target in
             " between, and a second stats INSERT double-counts every run.
             server_committed TYPE abap_bool,
             error   TYPE string,
           END OF ty_run.
    TYPES tt_run TYPE STANDARD TABLE OF ty_run WITH EMPTY KEY.

    "! Register (or idempotently update) one delta target in _erpl_rev_delta_state.
    "! Enforces the granularity gate: a date-only change column (wm_kind=DATE)
    "! cannot run at micro cadence (set rv_error). Returns '' on success.
    CLASS-METHODS register
      IMPORTING is_state        TYPE ty_state
      RETURNING VALUE(rv_error) TYPE string.


    "! What the planning FM answered: the plan, or why there is none.
    "!
    "! Two fields rather than an empty string, because "no plan" and "the server
    "! refused, and here is why" are different things. Collapsing them cost the
    "! operator every reason a cycle was refused -- they all arrived as
    "! "BEGIN_CYCLE returned nothing".
    TYPES: BEGIN OF ty_plan_result,
             json  TYPE string,
             error TYPE string,
           END OF ty_plan_result.

    "! Call the server's planning FM. One function module, many actions, because
    "! the alternative is a new stub -- and a new upgrade event on every installed
    "! system -- per decision the server needs to make.
    CLASS-METHODS plan_json
      IMPORTING iv_action TYPE string
                iv_target TYPE string DEFAULT ''
                iv_params TYPE string DEFAULT ''
      RETURNING VALUE(rs) TYPE ty_plan_result.

    "! One scalar out of a flat JSON object the server produced.
    CLASS-METHODS jstr
      IMPORTING iv_json   TYPE string
                iv_key    TYPE string
      RETURNING VALUE(rv) TYPE string.

    "! Read the full config+state row for a target ('' target => not registered).
    CLASS-METHODS state
      IMPORTING iv_target    TYPE csequence
      RETURNING VALUE(rs)    TYPE ty_state.

    "! Advance the watermark + rows + last_run_ts and set status back to IDLE.
    CLASS-METHODS commit
      IMPORTING iv_target TYPE csequence
                iv_wm     TYPE string
                iv_rows   TYPE i.

    "! Try to take the per-target lease. False if another cycle holds a FRESH
    "! RUNNING lease (< c_lease_ttl old); a stale lease is reclaimed.
    CLASS-METHODS try_lease
      IMPORTING iv_target    TYPE csequence
      RETURNING VALUE(rv_ok) TYPE abap_bool.

    "! Release the lease, recording final status + last_error.
    CLASS-METHODS release
      IMPORTING iv_target TYPE csequence
                iv_status TYPE string DEFAULT 'IDLE'
                iv_error  TYPE string DEFAULT ''.

    "! Run ONE delta cycle for a target: lease -> dispatch by method -> commit ->
    "! release. rs-skipped=X when the lease is held by another cycle.
    "! One cycle. iv_load_type selects which of the four a run is:
    "!   D delta (default) - the bounded window
    "!   F full reload, watermark untouched - a data repair, not a re-seed
    "!   I init without data - adopt a position, transfer nothing
    "!   L init + full load
    CLASS-METHODS run
      IMPORTING iv_target    TYPE csequence
                iv_load_type TYPE csequence DEFAULT 'D'
      RETURNING VALUE(rs)    TYPE ty_run.

    "! Targets currently DUE (cadence elapsed since last_run_ts, lease free).
    "! cadence='manual' is never due. Used by the Z_ERPL_REV_DELTA job loop.
    CLASS-METHODS due
      RETURNING VALUE(rt) TYPE string_table.

    "! Run one tick: every due target, in order. Returns one ty_run per target.
    CLASS-METHODS run_due
      RETURNING VALUE(rt) TYPE tt_run.

    "! Current CDHDR high-water for an object class as 14-char YYYYMMDDHHMMSS
    "! (latest UDATE+UTIME). Use it to seed a CHANGEDOC/INSERT_ONLY watermark so the
    "! first cycle only picks up changes made AFTER registration.
    CLASS-METHODS cdhdr_highwater
      IMPORTING iv_cls    TYPE csequence
      RETURNING VALUE(rv) TYPE string.

    "! Evaluate a single-cell numeric DuckDB query (e.g. SELECT count(*) ...) and
    "! return the integer value. (query()'s row_count is the #result-rows, not the
    "! cell value, so a count needs the cell parsed out of the rows JSON.)
    CLASS-METHODS scalar
      IMPORTING iv_sql    TYPE string
      RETURNING VALUE(rv) TYPE i.

    "! Thin wrapper over the Z_DUCKDB_SNAPSHOT_MERGE FM (server anti-join merge).
    CLASS-METHODS snapshot_merge
      IMPORTING iv_target TYPE csequence
                iv_staging TYPE csequence
                iv_keys    TYPE string
      EXPORTING ev_ins     TYPE i
                ev_upd     TYPE i
                ev_del     TYPE i
                ev_error   TYPE string.

    "! Install (or remove) the periodic SAP background job that drives the sync —
    "! the supported way to run delta on a cron. It schedules report Z_ERPL_REV_DELTA
    "! (one tick = run_due, i.e. every DUE target) to start now and repeat every
    "! iv_minutes minutes; one tick at the finest period gates each target by its own
    "! `cadence`. Any existing job of the same name is removed first, so calling it
    "! again just re-times it. iv_remove=X removes it and does not reschedule.
    "! NB: a SAP background-job period is >= 1 minute; for sub-minute cadence use the
    "! report's loop mode (p_loop) or an external trigger. Returns a status line.
    CLASS-METHODS schedule
      IMPORTING iv_minutes TYPE i DEFAULT 1
                iv_remove  TYPE abap_bool DEFAULT abap_false
      RETURNING VALUE(rv_msg) TYPE string.

    "! Period in MINUTES implied by a cadence string (micro:N -> N/60, hourly->60,
    "! nightly->1440, manual/unknown->0). Used to derive the heartbeat-job period from
    "! a target's chosen refresh interval, so one setting drives both.
    CLASS-METHODS cadence_minutes
      IMPORTING iv_cadence TYPE csequence
      RETURNING VALUE(rv)  TYPE i.

  PRIVATE SECTION.
    CONSTANTS c_dest TYPE rfcdest VALUE 'ERPL_REV'.

    "! SQL string-literal escaping (single quotes doubled).
    CLASS-METHODS q IMPORTING iv TYPE csequence RETURNING VALUE(rv) TYPE string.
    "! Parse the extra-JSON objectclas (CHANGEDOC/INSERT_ONLY driver class).
    CLASS-METHODS objectclas IMPORTING iv_extra TYPE string RETURNING VALUE(rv) TYPE string.
    "! Current max(chg_col) of a (numeric-watermark) source, as text.
    CLASS-METHODS source_max
      IMPORTING is_state     TYPE ty_state
      RETURNING VALUE(rv)    TYPE string.
    "! The four dispatch implementations (each returns rows/ins/upd/del/wm/error).
    CLASS-METHODS run_watermark   IMPORTING is_state     TYPE ty_state
                                            iv_load_type TYPE string DEFAULT 'D'
                                  RETURNING VALUE(rs)    TYPE ty_run.
    CLASS-METHODS run_changedoc   IMPORTING is_state TYPE ty_state RETURNING VALUE(rs) TYPE ty_run.
    CLASS-METHODS run_insert_only IMPORTING is_state TYPE ty_state RETURNING VALUE(rs) TYPE ty_run.
    CLASS-METHODS run_snapshot    IMPORTING is_state TYPE ty_state RETURNING VALUE(rs) TYPE ty_run.
    "! CDHDR change feed since a 14-char YYYYMMDDHHMMSS watermark: change numbers,
    "! object ids, and the new high-water (max udate+utime). cls = OBJECTCLAS.
    CLASS-METHODS cdhdr_feed
      IMPORTING iv_cls     TYPE csequence
                iv_wm      TYPE string
      EXPORTING et_changenr TYPE string_table
                et_objectid TYPE string_table
                ev_new_wm   TYPE string.
ENDCLASS.

CLASS zcl_erpl_rev_delta IMPLEMENTATION.

  METHOD q.
    rv = iv.
    REPLACE ALL OCCURRENCES OF `'` IN rv WITH `''`.
  ENDMETHOD.

  METHOD objectclas.
    IF iv_extra IS INITIAL. RETURN. ENDIF.
    TYPES: BEGIN OF ty_x, objectclas TYPE string, END OF ty_x.
    DATA ls_x TYPE ty_x.
    TRY.
        /ui2/cl_json=>deserialize( EXPORTING json = iv_extra CHANGING data = ls_x ).
      CATCH cx_root ##NO_HANDLER.
    ENDTRY.
    rv = ls_x-objectclas.
  ENDMETHOD.

  METHOD register.
    " Granularity gate: a date-only change column cannot be sub-hourly (HLD §7).
    IF is_state-wm_kind = 'DATE' AND is_state-cadence CP 'micro:*'.
      rv_error = |granularity gate: wm_kind=DATE cannot use cadence { is_state-cadence }|.
      RETURN.
    ENDIF.
    " Keys are not optional. Every merge, every delete anti-join and every log
    " op is keyed, so a keyless registration produces a cycle that cannot merge
    " and a reload whose delete accounting renders as a broken predicate. The
    " trigger tier already refuses this at registration; so does this one now.
    IF is_state-keys IS INITIAL.
      rv_error = |keys are required: a cycle merges, deletes and logs by key|.
      RETURN.
    ENDIF.
    IF is_state-load_type_default IS NOT INITIAL AND
       is_state-load_type_default <> 'D' AND is_state-load_type_default <> 'F' AND
       is_state-load_type_default <> 'I' AND is_state-load_type_default <> 'L'.
      rv_error = |unknown load type { is_state-load_type_default }; expected D, F, I or L|.
      RETURN.
    ENDIF.
    " Cross-checked against the METHOD, here, where it is a typo an operator can
    " still fix. Accepted, it becomes a target the planner hands an impossible
    " load type on every due tick -- refused each time, and until the refusal
    " learned to block, never backed off.
    IF is_state-load_type_default IS NOT INITIAL AND is_state-load_type_default <> 'D' AND
       is_state-method <> 'WATERMARK'.
      rv_error = |load type { is_state-load_type_default } is not implemented for | &&
                 |method { is_state-method }; only WATERMARK targets support one|.
      RETURN.
    ENDIF.
    " Every column of the state row is written here. A field added to ty_state but
    " forgotten in this statement is not a compile error and not a runtime error:
    " it simply arrives at the server as empty, and the feature that needed it
    " does nothing. time_col was exactly that -- a DATETIME target registered
    " cleanly, replicated its first batch, and then silently stopped, because the
    " server was comparing a pair whose time half it had never been told about.
    " Rendered before the statement: a multi-line COND inside a string template
    " is not a template any more.
    " NULL for "not said", which is what makes the coalescing upsert below mean
    " anything. These used to render 'false'/'false'/'D' for an unset field, so
    " every registration asserted all three whether the caller had mentioned
    " them or not -- and a re-registration for an unrelated reason silently
    " turned off a target's change log and cancelled its pending seed.
    DATA(lv_log) = COND string( WHEN is_state-log_enabled IS INITIAL THEN 'NULL'
                                WHEN is_state-log_enabled = 'true' THEN 'true' ELSE 'false' ).
    DATA(lv_aer) = COND string( WHEN is_state-allow_empty_reload IS INITIAL THEN 'NULL'
                                WHEN is_state-allow_empty_reload = 'true' THEN 'true'
                                ELSE 'false' ).
    DATA(lv_lt)  = COND string( WHEN is_state-load_type_default IS INITIAL THEN 'NULL'
                                ELSE |'{ q( is_state-load_type_default ) }'| ).
    DATA(lv_sql) =
      |INSERT INTO _erpl_rev_delta_state | &&
      |(target,method,source_from,keys,chg_col,time_col,wm_kind,wm_value,| &&
      |safety_secs,safety_units,cadence,extra,log_enabled,load_type_default,| &&
      |allow_empty_reload,status) VALUES (| &&
      |'{ q( is_state-target ) }','{ q( is_state-method ) }','{ q( is_state-source_from ) }',| &&
      |'{ q( is_state-keys ) }','{ q( is_state-chg_col ) }',| &&
      |{ COND string( WHEN is_state-time_col IS INITIAL THEN 'NULL' ELSE |'{ q( is_state-time_col ) }'| ) },| &&
      |'{ q( is_state-wm_kind ) }',| &&
      |{ COND string( WHEN is_state-wm_value IS INITIAL THEN 'NULL' ELSE |'{ q( is_state-wm_value ) }'| ) },| &&
      |{ is_state-safety_secs },{ is_state-safety_units },'{ q( is_state-cadence ) }',| &&
      |{ COND string( WHEN is_state-extra IS INITIAL THEN 'NULL' ELSE |'{ q( is_state-extra ) }'| ) },| &&
      |{ lv_log },{ lv_lt },{ lv_aer },'IDLE') | &&
      |ON CONFLICT (target) DO UPDATE SET method=excluded.method, source_from=excluded.source_from, | &&
      |keys=excluded.keys, chg_col=excluded.chg_col, time_col=excluded.time_col, | &&
      |wm_kind=excluded.wm_kind, wm_value=excluded.wm_value, | &&
      |safety_secs=excluded.safety_secs, safety_units=excluded.safety_units, | &&
      |cadence=excluded.cadence, extra=excluded.extra, | &&
      |log_enabled=coalesce(excluded.log_enabled, _erpl_rev_delta_state.log_enabled), | &&
      |load_type_default=coalesce(excluded.load_type_default, | &&
      |                           _erpl_rev_delta_state.load_type_default), | &&
      |allow_empty_reload=coalesce(excluded.allow_empty_reload, | &&
      |                            _erpl_rev_delta_state.allow_empty_reload)|.
    DATA(ls) = zcl_erpl_rev_util=>query( lv_sql ).
    rv_error = ls-error.
  ENDMETHOD.

  METHOD state.
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT target, method, source_from, keys, coalesce(chg_col,'') AS chg_col, | &&
      |coalesce(time_col,'') AS time_col, | &&
      |coalesce(wm_kind,'') AS wm_kind, coalesce(wm_value,'') AS wm_value, | &&
      |coalesce(safety_secs,0) AS safety_secs, | &&
      |coalesce(safety_units,0) AS safety_units, coalesce(cadence,'manual') AS cadence, | &&
      |coalesce(extra,'') AS extra, | &&
      |CASE WHEN log_enabled THEN 'true' ELSE 'false' END AS log_enabled, | &&
      |coalesce(load_type_default,'D') AS load_type_default, | &&
      |CASE WHEN allow_empty_reload THEN 'true' ELSE 'false' END AS allow_empty_reload, | &&
      |coalesce(status,'IDLE') AS status | &&
      |FROM _erpl_rev_delta_state WHERE target='{ q( iv_target ) }'| ).
    IF ls-error IS NOT INITIAL OR ls-row_count = 0. RETURN. ENDIF.
    DATA lt TYPE STANDARD TABLE OF ty_state WITH EMPTY KEY.
    /ui2/cl_json=>deserialize( EXPORTING json = ls-rows CHANGING data = lt ).
    READ TABLE lt INTO rs INDEX 1.
  ENDMETHOD.

  METHOD commit.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET | &&
      |wm_value={ COND string( WHEN iv_wm IS INITIAL THEN 'wm_value' ELSE |'{ q( iv_wm ) }'| ) }, | &&
      |rows_applied={ iv_rows }, last_run_ts=now(), status='IDLE', last_error=NULL | &&
      |WHERE target='{ q( iv_target ) }'| ).
  ENDMETHOD.

  METHOD try_lease.
    " Check-then-set: free unless another cycle holds a fresh RUNNING lease.
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT count(*) AS c FROM _erpl_rev_delta_state | &&
      |WHERE target='{ q( iv_target ) }' AND status='RUNNING' | &&
      |AND lease_ts >= now() - INTERVAL '{ c_lease_ttl }' SECOND| ).
    IF ls-error IS NOT INITIAL. RETURN. ENDIF.
    DATA(held) = ls-rows.
    CONDENSE held.
    IF held CS '"c":1' OR held CS '"c": 1'. rv_ok = abap_false. RETURN. ENDIF.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now() | &&
      |WHERE target='{ q( iv_target ) }'| ).
    rv_ok = abap_true.
  ENDMETHOD.

  METHOD release.
    " A failed cycle INCREMENTS fail_count, and that is what makes the planner's
    " backoff and parking exist at all. Nothing incremented it before: the column
    " was created with DEFAULT 0 and reset on success, so the delay never grew,
    " a target was never parked, and a broken one retried at the full micro
    " cadence forever -- hammering SAP for as long as it stayed broken. The
    " planner's backoff tests were passing against behaviour no runtime path
    " could reach.
    "
    " active_run_id is cleared here too: an errored cycle no longer owns the
    " target, and leaving the token set would block every later claim.
    " fail_count is zeroed only by SUCCESS. It used to be zeroed by every
    " non-ERROR release -- including a refusal -- so a target turned away for a
    " bad parameter wiped whatever backoff a genuinely failing target had
    " accumulated, and went straight back to full cadence.
    DATA(lv_fail) = COND string(
      WHEN iv_status = 'ERROR'
      THEN |, fail_count = coalesce(fail_count,0) + 1, active_run_id = NULL|
      WHEN iv_error IS NOT INITIAL
      THEN |, active_run_id = NULL|
      ELSE |, fail_count = 0, active_run_id = NULL| ).
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='{ q( iv_status ) }', | &&
      |last_error={ COND string( WHEN iv_error IS INITIAL THEN 'NULL' ELSE |'{ q( iv_error ) }'| ) }| &&
      |{ lv_fail } | &&
      |WHERE target='{ q( iv_target ) }'| ).
  ENDMETHOD.

  METHOD run.
    DATA(ls_state) = state( iv_target ).
    rs-target = iv_target.
    rs-method = ls_state-method.
    IF ls_state-target IS INITIAL.
      rs-error = |no delta registration for { iv_target }|.
      RETURN.
    ENDIF.
    IF try_lease( iv_target ) = abap_false.
      rs-skipped = abap_true.
      RETURN.
    ENDIF.
    " Defensive only -- the server refuses this in BEGIN_CYCLE, before anything
    " is claimed, because it is a decision about the request rather than an
    " executor's business. Kept here so an older server cannot silently drop the
    " parameter on the floor and run an ordinary cycle while reporting a repair.
    "
    " Released as IDLE, not ERROR: an operator typing the wrong load type is not
    " a target failure, and 'ERROR' increments fail_count, engages the backoff
    " and eventually parks a target that is working perfectly.
    IF iv_load_type <> 'D' AND ls_state-method <> 'WATERMARK'.
      rs-error = |load type { iv_load_type } is not implemented for method | &&
                 |{ ls_state-method }; only WATERMARK targets support one|.
      release( iv_target = iv_target iv_status = 'IDLE' iv_error = rs-error ).
      RETURN.
    ENDIF.

    GET TIME STAMP FIELD DATA(lv_t0).
    CASE ls_state-method.
      WHEN 'WATERMARK'.   rs = run_watermark( is_state = ls_state iv_load_type = CONV string( iv_load_type ) ).
      WHEN 'INSERT_ONLY'. rs = run_insert_only( ls_state ).
      WHEN 'CHANGEDOC'.   rs = run_changedoc( ls_state ).
      WHEN 'SNAPSHOT'.    rs = run_snapshot( ls_state ).
      WHEN OTHERS.        rs-error = |unknown delta method { ls_state-method }|.
    ENDCASE.
    rs-target = iv_target.
    rs-method = ls_state-method.
    GET TIME STAMP FIELD DATA(lv_t1).
    IF rs-server_committed = abap_true.
      " Nothing to do. CYCLE_COMMIT advanced the watermark, cleared
      " active_run_id, set the status and finished the run-statistics row -- in
      " one transaction, fenced on the run id. Repeating any of it here would
      " undo the fencing that transaction exists to provide.
      IF rs-error IS NOT INITIAL.
        release( iv_target = iv_target iv_status = 'ERROR' iv_error = rs-error ).
      ENDIF.
    ELSEIF rs-error IS INITIAL.
      commit( iv_target = iv_target iv_wm = rs-wm iv_rows = rs-rows ).
      release( iv_target = iv_target iv_status = 'IDLE' ).
    ELSE.
      release( iv_target = iv_target iv_status = 'ERROR' iv_error = rs-error ).
    ENDIF.
    " Dashboard stats: one DELTA run row per cycle (every method). WATERMARK/CHANGEDOC/
    " INSERT_ONLY report a row count but not an I/U/D split, so attribute it to ins so
    " rows_applied is meaningful; SNAPSHOT carries the real ins/upd/del.
    " The server writes its own statistics row for a cycle it committed; a second
    " one here would double every count in the dashboard view and carry
    " duration_ms=0, making rows_per_sec meaningless for half the rows.
    IF rs-server_committed = abap_true.
      RETURN.
    ENDIF.
    zcl_erpl_rev_util=>record_run(
      iv_target   = iv_target
      iv_source   = ls_state-source_from
      iv_run_type = 'DELTA'
      iv_method   = ls_state-method
      iv_status   = COND #( WHEN rs-error IS INITIAL THEN 'SUCCESS' ELSE 'ERROR' )
      iv_ms       = CONV i( cl_abap_tstmp=>subtract( tstmp1 = lv_t1 tstmp2 = lv_t0 ) * 1000 )
      iv_read     = rs-rows
      iv_ins      = COND i( WHEN rs-ins + rs-upd + rs-del = 0 THEN rs-rows ELSE rs-ins )
      iv_upd      = rs-upd
      iv_del      = rs-del
      iv_wm_from  = ls_state-wm_value
      iv_wm_to    = rs-wm
      iv_error    = rs-error ).
  ENDMETHOD.

  METHOD source_max.
    " Numeric high-water: max(chg_col) over the source, as plain text. DEC(21,7)
    " holds a TIMESTAMPL (sub-second) precisely and an integer/CHANGENR acceptably.
    DATA lv_max TYPE p LENGTH 11 DECIMALS 7.
    DATA lt_sel TYPE string_table.
    APPEND |max( { is_state-chg_col } )| TO lt_sel.
    TRY.
        SELECT SINGLE (lt_sel) FROM (is_state-source_from) INTO @lv_max.
      CATCH cx_root ##NO_HANDLER.
    ENDTRY.
    rv = condense( |{ lv_max }| ).
  ENDMETHOD.

  METHOD run_watermark.
    " The cycle contract. The server decides the window and owns the commit; this
    " reads and stages.
    "
    " BEGIN_CYCLE returns VALUES -- a floor, a ceiling, an as-of date, a run id --
    " and the predicate is composed here, so the driver's "parameters are only
    " ever read as values" posture holds on this path too.
    "
    " The watermark advances to the CEILING inside CYCLE_COMMIT, never to the
    " maximum of whatever happened to arrive. A row that commits during a read
    " longer than the safety window carries a value below that maximum, and
    " advancing to the maximum would put it below the next floor permanently.
    DATA(ls_begin) = plan_json( iv_action = 'BEGIN_CYCLE'
                               iv_target = is_state-target
                               " SAP's own clock travels with the request. A DATS
                               " or TIMS column is wall-clock in THIS system's
                               " timezone, and the server has no way to know what
                               " that is -- on A4H it is UTC while the server sat
                               " at UTC+2, which showed up not as a timezone
                               " complaint but as a DATETIME target that
                               " replicated once and then went quiet.
                               iv_params = |\{"load_type":"{ iv_load_type }",| &&
                                           |"sap_now":"{ sy-datum }{ sy-uzeit }"\}| ).
    IF ls_begin-error IS NOT INITIAL.
      rs-error = ls_begin-error.
      RETURN.
    ENDIF.
    DATA(lv_plan) = ls_begin-json.
    IF lv_plan IS INITIAL.
      rs-error = 'BEGIN_CYCLE returned no plan and no reason'.
      RETURN.
    ENDIF.

    DATA(lv_stage)  = jstr( iv_json = lv_plan iv_key = 'stage' ).
    DATA(lv_run_id) = jstr( iv_json = lv_plan iv_key = 'run_id' ).
    DATA(lv_floor)  = jstr( iv_json = lv_plan iv_key = 'floor' ).
    DATA(lv_ceil)   = jstr( iv_json = lv_plan iv_key = 'ceiling' ).
    DATA(lv_asof)   = jstr( iv_json = lv_plan iv_key = 'as_of_date' ).
    DATA(lv_chg)    = jstr( iv_json = lv_plan iv_key = 'chg_col' ).
    DATA(lv_tcol)   = jstr( iv_json = lv_plan iv_key = 'time_col' ).
    " The server decides what this cycle reads, and it already ships the answer.
    " Reading is_state-source_from instead meant one registration row had two
    " independent readers per cycle -- the state() SELECT here and the server's
    " own LoadState inside the fenced transaction. register() is an upsert, so a
    " re-registration between the two left the stage read from one source and
    " merged on another registration's keys, inside a transaction that believed
    " itself consistent. Falls back to the local copy only if an older server
    " does not send it.
    DATA(lv_src)    = jstr( iv_json = lv_plan iv_key = 'source_from' ).
    IF lv_src IS INITIAL. lv_src = is_state-source_from. ENDIF.

    IF lv_stage IS INITIAL OR lv_run_id IS INITIAL.
      rs-error = |BEGIN_CYCLE gave no run: { lv_plan }|.
      RETURN.
    ENDIF.

    " A load type that transfers nothing still opens and commits a cycle: that is
    " how it adopts a position without moving data.
    DATA lv_rows TYPE i.
    IF lv_plan CS '"read_rows":true'.
      DATA lv_where TYPE string.

      IF lv_tcol IS NOT INITIAL AND lv_floor IS NOT INITIAL.
        " A DATS + TIMS pair, compared as one value. The parentheses matter: the
        " same-day case has to bind tighter than the later-day case, or every row
        " after the floor's time-of-day is selected on every later day.
        "
        " The substring is taken INSIDE this guard, not before it. On an initial
        " load there is no floor at all, and offsetting into an empty string is a
        " short dump rather than an empty predicate.
        DATA(lv_fd) = lv_floor(8).
        DATA(lv_ft) = COND string( WHEN strlen( lv_floor ) >= 14 THEN lv_floor+8(6)
                                   ELSE '000000' ).
        lv_where = |( { lv_chg } > '{ lv_fd }' OR | &&
                   |( { lv_chg } = '{ lv_fd }' AND { lv_tcol } > '{ lv_ft }' ) )|.
      ELSEIF lv_tcol IS INITIAL AND lv_floor IS NOT INITIAL.
        lv_where = |{ lv_chg } > '{ lv_floor }'|.
      ENDIF.

      " The ceiling bounds the READ only when the server says it should, which is
      " the day-granular case: today is not a complete day, so reading it would
      " lose everything posted later today.
      "
      " For a clock-based column it must NOT bound the read. The ceiling is the
      " read start minus the safety window, so capping the read there would hide
      " every change until that window elapsed -- 120 seconds of latency on a
      " 2-second tick. The cycle reads everything above the floor and advances the
      " watermark only to the ceiling; rows above it are delivered now and
      " re-delivered next cycle, which the keyed merge absorbs.
      IF lv_ceil IS NOT INITIAL AND lv_plan CS '"ceiling_bounds_read":true'.
        DATA(lv_ub) = |{ lv_chg } < '{ lv_ceil }'|.
        lv_where = COND string( WHEN lv_where IS INITIAL THEN lv_ub
                                ELSE |{ lv_where } AND { lv_ub }| ).
      ENDIF.

      DATA(r) = zcl_erpl_rev_util=>replicate(
        iv_tab      = lv_src
        iv_target   = lv_stage
        iv_mode     = 'INSERT'
        iv_truncate = abap_true
        iv_where    = lv_where
        iv_record   = abap_false      " the cycle is recorded by run() as one DELTA row
        " No primary key on the stage. Building one is right for a full load,
        " whose target keeps the index for the rest of its life; a delta stage is
        " created, read exactly once by the merge, and dropped. Measured by
        " P-STAGE-PK: the index never pays for itself and costs 37% of the cycle
        " at 100k rows -- a fixed tax on the hot path of every streaming tick.
        iv_build_pk = abap_false
        " Drift is about the TARGET, not the stage this cycle happens to be
        " filling. Without this the watchdog compared the DDIC field list against
        " a staging table that does not exist yet and quietly did nothing.
        iv_drift_target = is_state-target ).
      IF r-error IS NOT INITIAL.
        rs-error = r-error.
        RETURN.
      ENDIF.
      lv_rows = r-rows_affected.
    ENDIF.

    " One transaction on the server: merge the stage, append the change log,
    " advance the watermark to the ceiling, finish the stats row, drop the stage.
    DATA(ls_done) = plan_json( iv_action = 'CYCLE_COMMIT'
                               iv_target = is_state-target
                               iv_params = |\{"run_id":{ lv_run_id },"rows_read":{ lv_rows }\}| ).
    IF ls_done-error IS NOT INITIAL.
      rs-error = ls_done-error.
      RETURN.
    ENDIF.
    DATA(lv_done) = ls_done-json.
    IF lv_done IS INITIAL.
      rs-error = 'CYCLE_COMMIT returned no result and no reason'.
      RETURN.
    ENDIF.

    rs-rows = lv_rows.
    rs-wm   = jstr( iv_json = lv_done iv_key = 'wm' ).
    rs-server_committed = abap_true.
  ENDMETHOD.

  METHOD cdhdr_feed.
    " The canonical incremental CDHDR read (HLD §5.2 / research): watermark on
    " UDATE+UTIME (CHANGENR is buffered/non-monotonic — never `changenr > wm`).
    DATA lv_d TYPE d.
    DATA lv_t TYPE t.
    IF iv_wm IS NOT INITIAL AND strlen( iv_wm ) >= 14.
      lv_d = iv_wm(8).
      lv_t = iv_wm+8(6).
    ENDIF.
    DATA lv_cls TYPE cdobjectcl.
    lv_cls = iv_cls.
    TYPES: BEGIN OF ty_h, changenr TYPE cdchangenr, objectid TYPE cdobjectv,
                          udate TYPE cddatum, utime TYPE cduzeit, END OF ty_h.
    DATA lt_h TYPE STANDARD TABLE OF ty_h.
    IF lv_d IS INITIAL.
      SELECT changenr objectid udate utime FROM cdhdr
        INTO TABLE lt_h
        WHERE objectclas = lv_cls
        ORDER BY udate utime changenr.
    ELSE.
      SELECT changenr objectid udate utime FROM cdhdr
        INTO TABLE lt_h
        WHERE objectclas = lv_cls
          AND ( udate > lv_d OR ( udate = lv_d AND utime > lv_t ) )
        ORDER BY udate utime changenr.
    ENDIF.
    DATA lv_maxd TYPE d.
    DATA lv_maxt TYPE t.
    lv_maxd = lv_d.
    lv_maxt = lv_t.
    LOOP AT lt_h INTO DATA(ls_h).
      APPEND |{ ls_h-changenr }| TO et_changenr.
      APPEND condense( |{ ls_h-objectid }| ) TO et_objectid.
      IF ls_h-udate > lv_maxd OR ( ls_h-udate = lv_maxd AND ls_h-utime > lv_maxt ).
        lv_maxd = ls_h-udate.
        lv_maxt = ls_h-utime.
      ENDIF.
    ENDLOOP.
    IF lv_maxd IS NOT INITIAL.
      ev_new_wm = |{ lv_maxd }{ lv_maxt }|.
    ELSE.
      ev_new_wm = iv_wm.
    ENDIF.
  ENDMETHOD.

  METHOD run_changedoc.
    " CDHDR(objectclas) feed -> distinct business keys -> re-read current rows from
    " the real source by key -> keyed upsert (op I/U; deletes ride the nightly
    " SNAPSHOT). source_from = the table to re-read (e.g. MARA/MAKT); the key field
    " is the first DDIC key (e.g. MATNR), matched against CDHDR.OBJECTID.
    DATA lt_chg TYPE string_table.
    DATA lt_oid TYPE string_table.
    cdhdr_feed( EXPORTING iv_cls = objectclas( is_state-extra ) iv_wm = is_state-wm_value
                IMPORTING et_changenr = lt_chg et_objectid = lt_oid ev_new_wm = rs-wm ).
    SORT lt_oid.
    DELETE ADJACENT DUPLICATES FROM lt_oid.
    IF lt_oid IS INITIAL.
      rs-wm = is_state-wm_value.
      RETURN.
    ENDIF.
    " Business key field = the first NON-client key column (CDHDR.OBJECTID is the
    " business key without the client, e.g. MATNR for MANDT,MATNR,SPRAS).
    SPLIT is_state-keys AT ',' INTO TABLE DATA(lt_keys).
    DATA lv_keyf TYPE string.
    LOOP AT lt_keys INTO DATA(lv_k).
      DATA(lv_ku) = to_upper( condense( lv_k ) ).
      IF lv_ku = 'MANDT' OR lv_ku = 'CLIENT' OR lv_ku = 'MANDANT'. CONTINUE. ENDIF.
      lv_keyf = lv_ku.
      EXIT.
    ENDLOOP.
    IF lv_keyf IS INITIAL. lv_keyf = to_upper( VALUE string( lt_keys[ 1 ] OPTIONAL ) ). ENDIF.
    " Built by the shared predicate helper rather than by hand here: the trigger
    " tier re-reads by the FULL composite key and needs the same code, and a
    " hand-rolled IN list has no chunking -- a change document touching thousands
    " of objects would exceed what the parser accepts.
    DATA lt_rows TYPE zcl_erpl_rev_util=>tt_keyrows.
    LOOP AT lt_oid INTO DATA(lv_oid).
      APPEND VALUE #( ( lv_oid ) ) TO lt_rows.
    ENDLOOP.
    DATA(lv_where) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( lv_keyf ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = lt_rows ).
    DATA(r) = zcl_erpl_rev_util=>replicate(
      iv_tab      = is_state-source_from
      iv_target   = is_state-target
      iv_mode     = 'MERGE'
      iv_truncate = abap_false
      iv_where    = lv_where
      iv_record   = abap_false ).   " the cycle is recorded by run() as one DELTA row
    rs-rows  = r-rows_affected.
    rs-error = r-error.
    IF r-error IS NOT INITIAL. rs-wm = is_state-wm_value. ENDIF.
  ENDMETHOD.

  METHOD run_insert_only.
    " Append-only source driven by CDHDR change numbers (2-step, portable across
    " ECC cluster / S4 transparent CDPOS): CDHDR feed -> CHANGENR list -> re-read
    " by CHANGENR -> keyed upsert (DDIC key dedups re-delivered rows). chg_col is
    " informational; the high-water is the CDHDR UDATE+UTIME pair.
    DATA lt_chg TYPE string_table.
    DATA lt_oid TYPE string_table.
    cdhdr_feed( EXPORTING iv_cls = objectclas( is_state-extra ) iv_wm = is_state-wm_value
                IMPORTING et_changenr = lt_chg et_objectid = lt_oid ev_new_wm = rs-wm ).
    SORT lt_chg.
    DELETE ADJACENT DUPLICATES FROM lt_chg.
    IF lt_chg IS INITIAL.
      rs-wm = is_state-wm_value.
      RETURN.
    ENDIF.
    DATA lt_crows TYPE zcl_erpl_rev_util=>tt_keyrows.
    LOOP AT lt_chg INTO DATA(lv_c).
      APPEND VALUE #( ( lv_c ) ) TO lt_crows.
    ENDLOOP.
    DATA(lv_where) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `CHANGENR` ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = lt_crows ).
    DATA(r) = zcl_erpl_rev_util=>replicate(
      iv_tab      = is_state-source_from
      iv_target   = is_state-target
      iv_mode     = 'MERGE'
      iv_truncate = abap_false
      iv_where    = lv_where
      iv_record   = abap_false ).   " the cycle is recorded by run() as one DELTA row
    rs-rows  = r-rows_affected.
    rs-error = r-error.
    IF r-error IS NOT INITIAL. rs-wm = is_state-wm_value. ENDIF.
  ENDMETHOD.

  METHOD run_snapshot.
    " Full reload into <target>__snap, then a server-side anti-join merge onto the
    " target (upsert all + delete keys absent from the snapshot). The only path
    " that reflects PHYSICAL deletes. Self-seeds the target (CREATE IF NOT EXISTS
    " + PK) so the first snapshot cycle also works.
    DATA(lv_stg) = |{ is_state-target }__snap|.
    DATA(ld) = zcl_erpl_rev_util=>describe_table(
      iv_tab = is_state-source_from iv_target = is_state-target ).
    IF ld-error IS NOT INITIAL. rs-error = ld-error. RETURN. ENDIF.
    DATA(lc) = zcl_erpl_rev_util=>query( ld-ddl ).   " ensure target exists (+PK)
    IF lc-error IS NOT INITIAL. rs-error = lc-error. RETURN. ENDIF.

    " Optional parallel reload: extra may carry {"jobs":N,"part_col":"X"} (e.g. set by
    " Z_ERPL_REV_REPLICATE when the seed used the Parallel tab). With jobs>1 and a
    " numeric partition column, the per-cycle full reload runs across N background
    " workers via the proven parallel full-load engine; otherwise it's a serial read.
    TYPES: BEGIN OF ty_par, jobs TYPE i, part_col TYPE string, END OF ty_par.
    DATA ls_par TYPE ty_par.
    IF is_state-extra IS NOT INITIAL.
      TRY.
          /ui2/cl_json=>deserialize( EXPORTING json = is_state-extra CHANGING data = ls_par ).
        CATCH cx_root ##NO_HANDLER.
      ENDTRY.
    ENDIF.

    DATA r TYPE zcl_erpl_rev_util=>ty_repl.
    DATA(lv_pcol) = ls_par-part_col.
    IF ls_par-jobs > 1 AND lv_pcol IS INITIAL.
      lv_pcol = zcl_erpl_rev_util=>pick_partition_col( ld-fields ).
    ENDIF.
    IF ls_par-jobs > 1 AND lv_pcol IS NOT INITIAL.
      r = zcl_erpl_rev_util=>replicate_parallel(
            iv_tab      = is_state-source_from
            iv_target   = lv_stg
            iv_part_col = lv_pcol
            iv_jobs     = ls_par-jobs
            iv_record   = abap_false ).   " staging reload; the cycle is one DELTA row
    ENDIF.
    " Serial reload when not parallel, or as a safe fallback if the parallel reload
    " could not run (e.g. no suitable numeric partition column / no free batch WPs).
    IF NOT ( ls_par-jobs > 1 AND lv_pcol IS NOT INITIAL ) OR r-error IS NOT INITIAL.
      r = zcl_erpl_rev_util=>replicate(
            iv_tab      = is_state-source_from
            iv_target   = lv_stg
            iv_truncate = abap_true
            iv_record   = abap_false ).
    ENDIF.
    IF r-error IS NOT INITIAL. rs-error = r-error. RETURN. ENDIF.
    snapshot_merge( EXPORTING iv_target = is_state-target iv_staging = lv_stg iv_keys = is_state-keys
                    IMPORTING ev_ins = rs-ins ev_upd = rs-upd ev_del = rs-del ev_error = rs-error ).
    rs-rows = rs-ins + rs-upd + rs-del.
    " Bookkeeping watermark = current run timestamp (snapshot is full each cycle).
    GET TIME STAMP FIELD DATA(lv_ts).
    rs-wm = condense( |{ lv_ts }| ).
  ENDMETHOD.

  METHOD cdhdr_highwater.
    DATA lv_cls TYPE cdobjectcl.
    lv_cls = iv_cls.
    DATA lv_maxd TYPE cddatum.
    DATA lv_maxt TYPE cduzeit.
    SELECT MAX( udate ) FROM cdhdr WHERE objectclas = @lv_cls INTO @lv_maxd.
    IF lv_maxd IS INITIAL. RETURN. ENDIF.
    SELECT MAX( utime ) FROM cdhdr
      WHERE objectclas = @lv_cls AND udate = @lv_maxd INTO @lv_maxt.
    rv = |{ lv_maxd }{ lv_maxt }|.
  ENDMETHOD.

  METHOD scalar.
    DATA(ls) = zcl_erpl_rev_util=>query( iv_sql ).
    IF ls-error IS NOT INITIAL. RETURN. ENDIF.
    FIND PCRE ':\s*(-?[0-9]+)' IN ls-rows SUBMATCHES DATA(lv).
    IF sy-subrc = 0. rv = CONV i( lv ). ENDIF.
  ENDMETHOD.

  METHOD snapshot_merge.
    DATA: lv_ins TYPE string, lv_upd TYPE string, lv_del TYPE string.
    DATA lv_msg TYPE c LENGTH 255.
    CALL FUNCTION 'Z_DUCKDB_SNAPSHOT_MERGE' DESTINATION c_dest
      EXPORTING  iv_target  = CONV string( iv_target )
                 iv_staging = CONV string( iv_staging )
                 iv_keys    = iv_keys
      IMPORTING  ev_ins     = lv_ins
                 ev_upd     = lv_upd
                 ev_del     = lv_del
                 ev_error   = ev_error
      EXCEPTIONS system_failure = 1 MESSAGE lv_msg
                 communication_failure = 2 MESSAGE lv_msg
                 OTHERS = 3.
    IF sy-subrc <> 0.
      ev_error = |RFC subrc={ sy-subrc } { lv_msg }|.
      RETURN.
    ENDIF.
    IF lv_ins CO ' 0123456789'. ev_ins = CONV i( lv_ins ). ENDIF.
    IF lv_upd CO ' 0123456789'. ev_upd = CONV i( lv_upd ). ENDIF.
    IF lv_del CO ' 0123456789'. ev_del = CONV i( lv_del ). ENDIF.
  ENDMETHOD.

  METHOD due.
    " Due = cadence elapsed since last_run_ts (or never run) and lease free.
    " micro:<sec> -> <sec>; hourly -> 3600; nightly -> 86400; manual -> never.
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT target, cadence, status, | &&
      |coalesce(epoch(now()) - epoch(last_run_ts), 9.0e18) AS age, | &&
      |coalesce(epoch(now()) - epoch(lease_ts), 9.0e18) AS lease_age | &&
      |FROM _erpl_rev_delta_state WHERE cadence <> 'manual'| ).
    IF ls-error IS NOT INITIAL OR ls-row_count = 0. RETURN. ENDIF.
    TYPES: BEGIN OF ty_d, target TYPE string, cadence TYPE string, status TYPE string,
                          age TYPE f, lease_age TYPE f, END OF ty_d.
    DATA lt TYPE STANDARD TABLE OF ty_d WITH EMPTY KEY.
    /ui2/cl_json=>deserialize( EXPORTING json = ls-rows CHANGING data = lt ).
    LOOP AT lt INTO DATA(ls_d).
      DATA(lv_int) = COND f( WHEN ls_d-cadence CP 'micro:*'
                               THEN CONV f( substring_after( val = ls_d-cadence sub = ':' ) )
                             WHEN ls_d-cadence = 'hourly'  THEN 3600
                             WHEN ls_d-cadence = 'nightly' THEN 86400
                             ELSE 86400 ).
      DATA(lv_busy) = xsdbool( ls_d-status = 'RUNNING' AND ls_d-lease_age < c_lease_ttl ).
      IF lv_busy = abap_false AND ls_d-age >= lv_int.
        APPEND ls_d-target TO rt.
      ENDIF.
    ENDLOOP.
  ENDMETHOD.

  METHOD run_due.
    LOOP AT due( ) INTO DATA(lv_t).
      APPEND run( lv_t ) TO rt.
    ENDLOOP.
  ENDMETHOD.

  METHOD schedule.
    CONSTANTS lc_job TYPE btcjob VALUE 'ERPL_REV_DELTA'.
    " Remove any existing scheduled/released/ready periodic job of this name first
    " (status S=scheduled, R=released/ready, Y=ready, P=planned), so a re-schedule
    " just re-times it instead of stacking duplicates.
    SELECT jobname, jobcount FROM tbtco
      WHERE jobname = @lc_job AND status IN ( 'S', 'R', 'Y', 'P' )
      INTO TABLE @DATA(lt_old).
    LOOP AT lt_old INTO DATA(ls_old).
      CALL FUNCTION 'BP_JOB_DELETE'
        " commitmode, not commit_flag: BP_JOB_DELETE has no COMMIT_FLAG, and
        " passing one dumps with CALL_FUNCTION_PARM_UNKNOWN. This loop only runs
        " when a previous job exists, so the first-ever schedule worked and every
        " re-schedule and unschedule after it aborted.
        EXPORTING jobcount = ls_old-jobcount jobname = ls_old-jobname
                  forcedmode = abap_true commitmode = abap_true
        EXCEPTIONS OTHERS = 0.
    ENDLOOP.
    DATA(lv_removed) = lines( lt_old ).

    IF iv_remove = abap_true.
      rv_msg = |unscheduled: removed { lv_removed } '{ lc_job }' job(s).|.
      RETURN.
    ENDIF.

    DATA(lv_min) = COND i( WHEN iv_minutes > 0 THEN iv_minutes ELSE 1 ).
    DATA lv_jc TYPE btcjobcnt.
    CALL FUNCTION 'JOB_OPEN'
      EXPORTING jobname = lc_job IMPORTING jobcount = lv_jc EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0. rv_msg = |ERROR: JOB_OPEN subrc { sy-subrc }|. RETURN. ENDIF.

    " The job step: report Z_ERPL_REV_DELTA with its defaults (p_once -> one tick over
    " every DUE target). JOB_SUBMIT (an FM) is used rather than `SUBMIT … VIA JOB` so
    " it also works when scheduled from a non-dialog context.
    CALL FUNCTION 'JOB_SUBMIT'
      EXPORTING authcknam = sy-uname jobcount = lv_jc jobname = lc_job report = 'Z_ERPL_REV_DELTA'
      EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0. rv_msg = |ERROR: JOB_SUBMIT subrc { sy-subrc }|. RETURN. ENDIF.

    " Schedule it to start now and repeat every lv_min minutes (the cron). An
    " explicit start date/time + PRDMINS is the canonical way to create a PERIODIC
    " job (more reliable than STRTIMMED, which is geared to a one-off immediate run).
    CALL FUNCTION 'JOB_CLOSE'
      EXPORTING jobcount = lv_jc jobname = lc_job
                sdlstrtdt = sy-datum sdlstrttm = sy-uzeit prdmins = lv_min
      EXCEPTIONS OTHERS = 1.
    IF sy-subrc <> 0. rv_msg = |ERROR: JOB_CLOSE subrc { sy-subrc }|. RETURN. ENDIF.

    rv_msg = |scheduled '{ lc_job }' to run every { lv_min } min| &&
             COND string( WHEN lv_removed > 0 THEN | (replaced { lv_removed } old)| ELSE `` ) &&
             |; monitor/stop in SM37.|.
  ENDMETHOD.

  METHOD cadence_minutes.
    IF iv_cadence CP 'micro:*'.
      DATA(lv_sec) = CONV i( condense( substring_after( val = iv_cadence sub = ':' ) ) ).
      rv = COND #( WHEN lv_sec >= 60 THEN lv_sec / 60 ELSE 1 ).
    ELSEIF iv_cadence = 'hourly'.
      rv = 60.
    ELSEIF iv_cadence = 'nightly'.
      rv = 1440.
    ELSE.
      rv = 0.   " manual / unknown -> not scheduled
    ENDIF.
  ENDMETHOD.


  METHOD plan_json.
    DATA lv_msg TYPE c LENGTH 255.
    CALL FUNCTION 'Z_DUCKDB_PLAN' DESTINATION c_dest
      EXPORTING iv_action = iv_action
                iv_target = iv_target
                iv_params = iv_params
      IMPORTING ev_plan   = rs-json
                ev_error  = rs-error
      EXCEPTIONS system_failure        = 1 MESSAGE lv_msg
                 communication_failure = 2 MESSAGE lv_msg
                 OTHERS                = 3.
    IF sy-subrc <> 0.
      CLEAR rs-json.
      rs-error = |{ iv_action } RFC subrc={ sy-subrc } { lv_msg }|.
      RETURN.
    ENDIF.
    " A server-side refusal carries its reason. It is kept, not discarded: the
    " reason is the whole value of the message, and an operator told only that
    " something "returned nothing" has to go and read a server log to learn that
    " their target was reclaimed, or blocked by drift, or has no registration.
    IF rs-error IS NOT INITIAL.
      CLEAR rs-json.
    ENDIF.
  ENDMETHOD.

  METHOD jstr.
    " Quoted string first, then a bare number: the server writes run_id and the
    " counts unquoted, and everything else quoted.
    DATA(lv_p1) = `"` && iv_key && `"\s*:\s*"([^"]*)"`.
    FIND PCRE lv_p1 IN iv_json SUBMATCHES rv.
    IF sy-subrc = 0. RETURN. ENDIF.
    DATA(lv_p2) = `"` && iv_key && `"\s*:\s*(-?[0-9.]+)`.
    FIND PCRE lv_p2 IN iv_json SUBMATCHES rv.
    IF sy-subrc <> 0. CLEAR rv. ENDIF.
  ENDMETHOD.

ENDCLASS.
