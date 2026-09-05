CLASS zcl_erpl_rev_cdc DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    " Thin executor for the opt-in trigger-CDC delta tier (epic #17 / ADR-0004).
    " It makes ZERO CDC decisions: the server (Z_DUCKDB_CDC_PLAN / Z_DUCKDB_CDC_APPLY)
    " generates every piece of SQL and owns all state; this class only
    "   (a) runs the opaque DDL the server returns on the SAP DB (cl_sql_statement),
    "   (b) stages the new log rows (replicate_native = ADBC read -> DuckDB), and
    "   (c) calls the server apply, then runs the opaque prune statement.
    " The triggers + log table are customer-owned, in-namespace objects created with
    " the customer's own DDL on the customer's own table (no SAP-proprietary CDC).

    TYPES: BEGIN OF ty_result,
             ins     TYPE i,
             upd     TYPE i,
             del     TYPE i,
             prune   TYPE i,
             applied TYPE abap_bool,
             error   TYPE string,
           END OF ty_result.

    "! Provision CDC for a target: fetch the plan, run the log-table + trigger DDL on
    "! the SAP DB, mark SEEDED. The DuckDB target must already be seeded (a full-load
    "! replicate) before the first run. iv_mode: DELETE_ONLY (default) | FULL_IUD.
    CLASS-METHODS provision
      IMPORTING iv_target TYPE string
                iv_source TYPE string
                iv_keys   TYPE string
                iv_mode   TYPE string DEFAULT 'DELETE_ONLY'
      RETURNING VALUE(rv_error) TYPE string.

    "! Run one CDC cycle: stage the new log rows (seq > position), apply them in the
    "! server (coalesce -> MERGE -> advance position), then prune the SAP log.
    CLASS-METHODS run
      IMPORTING iv_target TYPE string
      RETURNING VALUE(rs) TYPE ty_result.

    "! Tear down: drop the triggers + log table + sequence; mark DISABLED.
    CLASS-METHODS teardown
      IMPORTING iv_target TYPE string
      RETURNING VALUE(rv_error) TYPE string.

    "! Run one cycle for every provisioned (SEEDED/ACTIVE) CDC target — the heartbeat
    "! entry point, called from a periodic job alongside the watermark/snapshot tiers.
    "! Returns the targets it ran.
    CLASS-METHODS run_due
      RETURNING VALUE(rt_targets) TYPE string_table.

  PRIVATE SECTION.
    "! One integer out of a flat JSON object the server produced.
    CLASS-METHODS json_int
      IMPORTING iv_json   TYPE string
                iv_key    TYPE string
      RETURNING VALUE(rv) TYPE i.

    "! Rebuild key tuples from the server's net-key JSON, in key order.
    CLASS-METHODS net_key_rows
      IMPORTING iv_json     TYPE string
                it_key_cols TYPE string_table
      EXPORTING et_rows     TYPE zcl_erpl_rev_util=>tt_keyrows.

    "! DDIC datatype per key column, so numeric keys are not quoted.
    CLASS-METHODS key_types
      IMPORTING iv_source   TYPE csequence
                it_key_cols TYPE string_table
      EXPORTING et_types    TYPE string_table.

    CONSTANTS c_dest TYPE rfcdest VALUE 'ERPL_REV'.

    TYPES: BEGIN OF ty_plan,
             log_table     TYPE string,
             seq_name      TYPE string,
             op_col        TYPE string,
             seq_col       TYPE string,
             position      TYPE i,
             key_cols      TYPE string_table,
             provision_ddl TYPE string_table,
             teardown_ddl  TYPE string_table,
             read_sql      TYPE string,
             read_from     TYPE string,
             prune_sql     TYPE string,
             " KEYS_IUD needs three more things from the plan: which mode it is
             " in, what to re-read the row images from, and which keys the server
             " decided actually need re-reading.
             mode          TYPE string,
             source        TYPE string,
             netkeys_sql   TYPE string,
           END OF ty_plan.

    "! Ask the server for the plan (and drive the state transition for the action).
    CLASS-METHODS plan
      IMPORTING iv_target TYPE string
                iv_action TYPE string
                iv_source TYPE string DEFAULT ''
                iv_keys   TYPE string DEFAULT ''
                iv_mode   TYPE string DEFAULT ''
      EXPORTING es_plan   TYPE ty_plan
                ev_error  TYPE string.

    "! Apply one staged log batch in the server (Z_DUCKDB_CDC_APPLY).
    CLASS-METHODS apply
      IMPORTING iv_target  TYPE string
                iv_staging TYPE string
                iv_keys    TYPE string
                " KEYS_IUD only: the second staging table holding the row images
                " the cycle re-read from the source. Empty for the other modes.
                iv_images  TYPE string OPTIONAL
      RETURNING VALUE(rs)  TYPE ty_result.

    "! Run one opaque SQL string on the SAP DB. iv_ddl=true -> execute_ddl (CREATE/
    "! DROP), false -> execute_update (the prune DELETE).
    CLASS-METHODS exec_native
      IMPORTING iv_sql   TYPE string
                iv_ddl   TYPE abap_bool DEFAULT abap_true
      RETURNING VALUE(rv_error) TYPE string.

    CLASS-METHODS to_int IMPORTING iv TYPE string RETURNING VALUE(rv) TYPE i.
ENDCLASS.


CLASS zcl_erpl_rev_cdc IMPLEMENTATION.

  METHOD plan.
    DATA lv_plan TYPE string.
    DATA lv_err  TYPE string.
    DATA lv_msg  TYPE c LENGTH 255.
    CALL FUNCTION 'Z_DUCKDB_CDC_PLAN' DESTINATION c_dest
      EXPORTING iv_target   = iv_target
                iv_source   = iv_source
                iv_keys     = iv_keys
                iv_mode     = iv_mode
                iv_action   = iv_action
      IMPORTING ev_plan     = lv_plan
                ev_error    = lv_err
      EXCEPTIONS system_failure        = 1 MESSAGE lv_msg
                 communication_failure = 2 MESSAGE lv_msg
                 OTHERS                = 3.
    IF sy-subrc <> 0.
      ev_error = |RFC subrc={ sy-subrc } { lv_msg }|.
      RETURN.
    ENDIF.
    IF lv_err IS NOT INITIAL.
      ev_error = lv_err.
      RETURN.
    ENDIF.
    IF lv_plan IS NOT INITIAL.
      /ui2/cl_json=>deserialize( EXPORTING json = lv_plan CHANGING data = es_plan ).
    ENDIF.
  ENDMETHOD.

  METHOD apply.
    DATA: lv_ins TYPE string, lv_upd TYPE string, lv_del TYPE string,
          lv_pru TYPE string, lv_app TYPE string.

    " A KEYS_IUD cycle carries a second staging table of re-read row images.
    " Routed through the planning FM only as a fallback: see below.
    IF abap_false = abap_true.
      DATA lv_plan TYPE string.
      DATA lv_perr TYPE string.
      DATA lv_msg2 TYPE c LENGTH 255.
      CALL FUNCTION 'Z_DUCKDB_PLAN' DESTINATION c_dest
        EXPORTING iv_action = 'CDC_APPLY'
                  iv_target = iv_target
                  iv_params = |\{"staging":"{ iv_staging }","keys":"{ iv_keys }",| &&
                              |"images":"{ iv_images }"\}|
        IMPORTING ev_plan   = lv_plan
                  ev_error  = lv_perr
        EXCEPTIONS system_failure        = 1 MESSAGE lv_msg2
                   communication_failure = 2 MESSAGE lv_msg2
                   OTHERS                = 3.
      IF sy-subrc <> 0.
        rs-error = |RFC subrc={ sy-subrc } { lv_msg2 }|.
        RETURN.
      ENDIF.
      IF lv_perr IS NOT INITIAL.
        rs-error = lv_perr.
        RETURN.
      ENDIF.
      rs-ins     = json_int( iv_json = lv_plan iv_key = 'ins' ).
      rs-upd     = json_int( iv_json = lv_plan iv_key = 'upd' ).
      rs-del     = json_int( iv_json = lv_plan iv_key = 'del' ).
      rs-prune   = json_int( iv_json = lv_plan iv_key = 'prune' ).
      rs-applied = xsdbool( lv_plan CS '"applied":true' ).
      RETURN.
    ENDIF.

    DATA lv_msg TYPE c LENGTH 255.
    CALL FUNCTION 'Z_DUCKDB_CDC_APPLY' DESTINATION c_dest
      EXPORTING iv_target  = iv_target
                iv_staging = iv_staging
                iv_keys    = iv_keys
                " Empty for DELETE_ONLY and IMAGE_IUD; the re-read images table
                " for a KEYS_IUD cycle. Optional, so a caller generated before
                " this parameter existed still binds.
                iv_images  = iv_images
      IMPORTING ev_ins     = lv_ins
                ev_upd     = lv_upd
                ev_del     = lv_del
                ev_prune   = lv_pru
                ev_applied = lv_app
                ev_error   = rs-error
      EXCEPTIONS system_failure        = 1 MESSAGE lv_msg
                 communication_failure = 2 MESSAGE lv_msg
                 OTHERS                = 3.
    IF sy-subrc <> 0.
      rs-error = |RFC subrc={ sy-subrc } { lv_msg }|.
      RETURN.
    ENDIF.
    rs-ins     = to_int( lv_ins ).
    rs-upd     = to_int( lv_upd ).
    rs-del     = to_int( lv_del ).
    rs-prune   = to_int( lv_pru ).
    rs-applied = xsdbool( lv_app = 'X' ).
  ENDMETHOD.

  METHOD exec_native.
    TRY.
        IF iv_ddl = abap_true.
          NEW cl_sql_statement( )->execute_ddl( iv_sql ).
        ELSE.
          NEW cl_sql_statement( )->execute_update( iv_sql ).
        ENDIF.
      CATCH cx_root INTO DATA(lx).
        rv_error = lx->get_text( ).
    ENDTRY.
  ENDMETHOD.

  METHOD provision.
    " Safety gate (ADR-0004): triggers can only go on a TRANSPARENT table. Pool/cluster
    " tables, views and activation-request (ADSO) objects are not trigger-trackable —
    " refuse with guidance to use SNAPSHOT instead, rather than failing obscurely.
    DATA lv_src TYPE tabname.
    lv_src = to_upper( iv_source ).
    SELECT SINGLE tabclass FROM dd02l INTO @DATA(lv_class) WHERE tabname = @lv_src.
    IF sy-subrc = 0 AND lv_class <> 'TRANSP'.
      rv_error = |CDC: source { iv_source } is { lv_class }; trigger-CDC needs a | &&
                 |transparent table (pool/cluster/view/ADSO are not trigger-trackable — | &&
                 |use the SNAPSHOT delta method instead)|.
      RETURN.
    ENDIF.

    DATA ls TYPE ty_plan.
    plan( EXPORTING iv_target = iv_target iv_action = 'PROVISION'
                    iv_source = iv_source iv_keys = iv_keys iv_mode = iv_mode
          IMPORTING es_plan = ls ev_error = rv_error ).
    IF rv_error IS NOT INITIAL. RETURN. ENDIF.
    " Drop any leftover objects from a prior run first (best-effort) so provisioning
    " is idempotent even when the server's DuckDB state was reset but the SAP-side
    " trigger/log objects still exist. The trigger is dropped first (teardown order).
    LOOP AT ls-teardown_ddl INTO DATA(lv_drop).
      exec_native( iv_sql = lv_drop iv_ddl = abap_true ).
    ENDLOOP.
    LOOP AT ls-provision_ddl INTO DATA(lv_ddl).
      rv_error = exec_native( iv_sql = lv_ddl iv_ddl = abap_true ).
      IF rv_error IS NOT INITIAL. RETURN. ENDIF.
    ENDLOOP.
    " triggers + log are live and the log starts empty -> mark SEEDED (position 0).
    plan( EXPORTING iv_target = iv_target iv_action = 'SEED' IMPORTING ev_error = rv_error ).
  ENDMETHOD.

  METHOD run.
    DATA ls TYPE ty_plan.
    plan( EXPORTING iv_target = iv_target iv_action = 'CYCLE'
          IMPORTING es_plan = ls ev_error = rs-error ).
    IF rs-error IS NOT INITIAL. RETURN. ENDIF.

    " Stage the new log rows (seq > current position) into a DuckDB staging table
    " via the ADBC native read — the server then coalesces + applies them.
    DATA(lv_stg) = |{ iv_target }__cdclog|.
    DATA(lr) = zcl_erpl_rev_util=>replicate_native(
      iv_from   = ls-read_from
      iv_target = lv_stg
      iv_where  = |"{ ls-seq_col }" > { ls-position }| ).
    IF lr-error IS NOT INITIAL. rs-error = lr-error. RETURN. ENDIF.

    DATA(lv_keys) = concat_lines_of( table = ls-key_cols sep = `,` ).

    " KEYS_IUD: the shadow log carries key + op + sequence only, so the row VALUES
    " have to be re-read from the source. That is the whole point of the mode --
    " the trigger a customer's transactions pay for stays small, instead of writing
    " a full row image on every change to a wide hot table.
    "
    " The server decides WHICH keys need re-reading (netkeys_sql coalesces the
    " batch to a net op per key and returns only the net inserts and updates); ABAP
    " re-reads them and stages the images. Deletes are NOT re-read: the row is
    " gone, and the log is the only remaining evidence it existed.
    DATA lv_img TYPE string.
    IF ls-mode = 'KEYS_IUD' AND ls-netkeys_sql IS NOT INITIAL.
      DATA(lk) = zcl_erpl_rev_util=>query( ls-netkeys_sql ).
      IF lk-error IS NOT INITIAL. rs-error = lk-error. RETURN. ENDIF.

      IF lk-row_count > 0.
        lv_img = |{ iv_target }__cdcimg|.

        " Rebuild the key tuples from the server's answer, then let the shared
        " predicate builder turn them into a WHERE -- the same one the
        " change-document re-read uses, so a composite key works and a large key
        " set is chunked rather than exceeding the parser.
        DATA lt_rows TYPE zcl_erpl_rev_util=>tt_keyrows.
        net_key_rows( EXPORTING iv_json = lk-rows it_key_cols = ls-key_cols
                      IMPORTING et_rows = lt_rows ).

        DATA lt_types TYPE string_table.
        key_types( EXPORTING iv_source = ls-source it_key_cols = ls-key_cols
                   IMPORTING et_types = lt_types ).

        DATA(lv_where) = zcl_erpl_rev_util=>key_in_predicate(
          it_key_cols  = ls-key_cols
          it_key_types = lt_types
          it_rows      = lt_rows ).

        DATA(li) = zcl_erpl_rev_util=>replicate(
          iv_tab      = ls-source
          iv_target   = lv_img
          iv_mode     = 'INSERT'
          iv_truncate = abap_true
          iv_where    = lv_where
          iv_record   = abap_false ).
        IF li-error IS NOT INITIAL. rs-error = li-error. RETURN. ENDIF.
      ENDIF.
    ENDIF.

    rs = apply( iv_target = iv_target iv_staging = lv_stg iv_keys = lv_keys
                iv_images = lv_img ).
    IF rs-error IS NOT INITIAL. RETURN. ENDIF.

    " Prune the SAP log up to the server-confirmed position (watermark-driven, never
    " destructive-on-read). Only when something was actually applied.
    IF rs-applied = abap_true.
      DATA(lv_prune) = replace( val = ls-prune_sql sub = `%CONF%` with = |{ rs-prune }| occ = 0 ).
      rs-error = exec_native( iv_sql = lv_prune iv_ddl = abap_false ).
    ENDIF.
  ENDMETHOD.

  METHOD teardown.
    DATA ls TYPE ty_plan.
    plan( EXPORTING iv_target = iv_target iv_action = 'DISABLE'
          IMPORTING es_plan = ls ev_error = rv_error ).
    IF rv_error IS NOT INITIAL. RETURN. ENDIF.
    " Best-effort: drop everything, keep the first error (so a missing object doesn't
    " abort the rest of the cleanup).
    LOOP AT ls-teardown_ddl INTO DATA(lv_ddl).
      DATA(lv_e) = exec_native( iv_sql = lv_ddl iv_ddl = abap_true ).
      IF lv_e IS NOT INITIAL AND rv_error IS INITIAL. rv_error = lv_e. ENDIF.
    ENDLOOP.
  ENDMETHOD.

  METHOD run_due.
    DATA(ls) = zcl_erpl_rev_util=>query(
      |SELECT target FROM _erpl_rev_cdc WHERE status IN ('SEEDED','ACTIVE')| ).
    IF ls-error IS NOT INITIAL OR ls-row_count = 0. RETURN. ENDIF.
    TYPES: BEGIN OF ty_t, target TYPE string, END OF ty_t.
    DATA lt TYPE STANDARD TABLE OF ty_t WITH EMPTY KEY.
    /ui2/cl_json=>deserialize( EXPORTING json = ls-rows CHANGING data = lt ).
    LOOP AT lt INTO DATA(ls_t).
      run( ls_t-target ).
      APPEND ls_t-target TO rt_targets.
    ENDLOOP.
  ENDMETHOD.

  METHOD to_int.
    IF iv CO ` 0123456789-`. rv = CONV i( iv ). ENDIF.
  ENDMETHOD.


  METHOD net_key_rows.
    " The server returns the net insert/update keys as JSON rows. Turn them back
    " into key tuples in KEY ORDER, which is what the predicate builder expects --
    " the order is what makes a composite tuple line up with its columns.
    CLEAR et_rows.
    DATA lt_generic TYPE STANDARD TABLE OF string WITH EMPTY KEY.
    SPLIT iv_json AT '},' INTO TABLE lt_generic.
    LOOP AT lt_generic INTO DATA(lv_obj).
      DATA lt_tuple TYPE string_table.
      CLEAR lt_tuple.
      LOOP AT it_key_cols INTO DATA(lv_col).
        DATA(lv_lc) = to_lower( lv_col ).
        DATA lv_val TYPE string.
        CLEAR lv_val.
        " Built by concatenation, not as a string template: inside |...| the
        " braces are expression delimiters, and a regex character class that
        " needs a literal '}' cannot be expressed there without escaping every
        " one of them.
        DATA(lv_pat) = `"` && lv_lc && `"\s*:\s*"?([^",` && `}` && `]*)"?`.
        FIND PCRE lv_pat IN lv_obj SUBMATCHES lv_val.
        APPEND lv_val TO lt_tuple.
      ENDLOOP.
      " Skip a fragment that yielded nothing: a trailing separator, not a key.
      IF line_exists( lt_tuple[ table_line = `` ] ) AND lines( lt_tuple ) = 1.
        CONTINUE.
      ENDIF.
      APPEND lt_tuple TO et_rows.
    ENDLOOP.
  ENDMETHOD.

  METHOD key_types.
    " DDIC types for the key columns, so a numeric key is written unquoted. A
    " quoted numeric compares as text and matches nothing -- silently.
    CLEAR et_types.
    DATA(ld) = zcl_erpl_rev_util=>describe_table( iv_tab = iv_source iv_target = 'x' ).
    LOOP AT it_key_cols INTO DATA(lv_col).
      DATA(lv_up) = to_upper( lv_col ).
      DATA(lv_ty) = `CHAR`.
      LOOP AT ld-fields INTO DATA(lf) WHERE name = lv_up.
        lv_ty = lf-datatype.
      ENDLOOP.
      APPEND lv_ty TO et_types.
    ENDLOOP.
  ENDMETHOD.


  METHOD json_int.
    DATA lv_val TYPE string.
    DATA(lv_pat) = `"` && iv_key && `"\s*:\s*(-?[0-9]+)`.
    FIND PCRE lv_pat IN iv_json SUBMATCHES lv_val.
    IF sy-subrc = 0.
      rv = lv_val.
    ENDIF.
  ENDMETHOD.

ENDCLASS.
