"! <p class="shorttext">P-KEYS: what KEYS_IUD actually costs against IMAGE_IUD</p>
"!
"! The trigger tier's default-mode decision rests on one claim: that logging
"! KEYS on a wide hot table is cheaper than logging the whole row image,
"! because the cost moves off the source's write path and onto the cycle, which
"! re-reads the values it needs. That claim was never measured. It is the kind
"! of claim that is obviously true right up until the re-read turns out to cost
"! more than the wide log row it saved.
"!
"! So both arms do the SAME useful work -- get N changes from ZWIDE_BSEG (five
"! key columns, ~400 payload columns) into the same DuckDB target -- and differ
"! only in where the bytes travel:
"!
"!   KEYS_IUD   narrow trigger write, then a re-read of ~400 columns per key
"!   IMAGE_IUD  wide trigger write, then no re-read at all
"!
"! What is ASSERTED is correctness: both modes must converge the target to the
"! same content, or the timings are comparing two different things. The TIMINGS
"! are reported, not asserted -- this runs on a shared trial system, and a
"! threshold on a number that moves with someone else's background job is a
"! test that fails for reasons nobody can act on. Read the numbers; do not let
"! a green tick stand in for reading them.
CLASS zcl_erpl_rev_perftest DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    " 200 BELNR x this many BUZEI. The generator writes BUZEI '001'..'500' as
    " NUMC(3), so the slice predicate is an ordinary string comparison and does
    " not depend on how BELNR happens to be formatted.
    CONSTANTS c_buzei_max TYPE string VALUE '010'.
    CONSTANTS c_target    TYPE string VALUE 'perf_wide'.
    CONSTANTS c_source    TYPE string VALUE 'ZWIDE_BSEG'.
    CONSTANTS c_keys      TYPE string VALUE 'CLIENT,BUKRS,BELNR,GJAHR,BUZEI'.

    TYPES: BEGIN OF ty_arm,
             mode      TYPE string,
             write_ms  TYPE i,
             cycle_ms  TYPE i,
             changes   TYPE i,
             applied   TYPE i,
             did       TYPE abap_bool,
             rows      TYPE i,
             duck_sum  TYPE string,
             sap_sum   TYPE string,
             error     TYPE string,
           END OF ty_arm.

    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS cnt IMPORTING iv_sql TYPE string RETURNING VALUE(rv) TYPE i.
    METHODS sql IMPORTING iv_sql TYPE string.
    METHODS arm IMPORTING iv_mode TYPE string RETURNING VALUE(rs) TYPE ty_arm.
    METHODS ms  IMPORTING iv_t0 TYPE timestampl iv_t1 TYPE timestampl
                RETURNING VALUE(rv) TYPE i.
ENDCLASS.

CLASS zcl_erpl_rev_perftest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD cnt.
    rv = zcl_erpl_rev_delta=>scalar( iv_sql ).
  ENDMETHOD.

  METHOD sql.
    zcl_erpl_rev_util=>query( iv_sql ).
  ENDMETHOD.

  METHOD ms.
    " Milliseconds between two timestamps. cl_abap_tstmp=>subtract returns whole
    " seconds, which for a sub-second arm reports 0 and makes the comparison
    " meaningless -- so the difference is taken on the fractional values.
    DATA(lv_d) = CONV decfloat34( iv_t1 - iv_t0 ).
    rv = lv_d * 1000.
  ENDMETHOD.

  METHOD arm.
    rs-mode = iv_mode.
    " A stage marker before each step, and one catch around the lot. A dump
    " here kills the classrun and reports a type name with no location, which
    " says nothing about WHICH of six calls raised it.
    DATA lv_stage TYPE string VALUE 'start'.
    TRY.

    " --- a clean slate for this arm ---------------------------------------
    " Teardown first: the triggers from the previous arm are still on the
    " source, and leaving them there would have the other mode's write path
    " measured on top of this one's.
    lv_stage = 'teardown-pre'.
    zcl_erpl_rev_cdc=>teardown( c_target ).
    sql( |DROP TABLE IF EXISTS { c_target }| ).
    sql( |DELETE FROM _erpl_rev_cdc WHERE target='{ c_target }'| ).

    DATA(lv_where) = |BUZEI <= '{ c_buzei_max }'|.

    " Seed the DuckDB target from the slice this arm will change, so the cycle
    " below is an UPDATE path in both arms rather than an insert path in one.
    lv_stage = 'seed'.
    DATA(ls_r) = zcl_erpl_rev_util=>replicate(
                   iv_tab = c_source iv_target = c_target
                   iv_where = lv_where iv_init = 'SET threads TO 1;' ).
    IF ls_r-error IS NOT INITIAL.
      rs-error = |seed: { ls_r-error }|.
      RETURN.
    ENDIF.
    rs-rows = ls_r-rows_affected.

    lv_stage = 'provision'.
    DATA(lv_pe) = zcl_erpl_rev_cdc=>provision(
      iv_target = c_target iv_source = c_source iv_keys = c_keys iv_mode = iv_mode ).
    IF lv_pe IS NOT INITIAL.
      rs-error = |provision: { lv_pe }|.
      RETURN.
    ENDIF.

    " --- the write path: what the triggers cost the source ----------------
    " This is the number the default-mode decision actually rests on. The
    " COMMIT is inside the measurement because a trigger's cost is not paid
    " until the transaction is durable.
    " Declared, not inferred. GET TIME STAMP FIELD DATA(x) infers `timestamp`,
    " which is whole SECONDS -- every arm would have measured 0 ms and the
    " comparison would have read as a tie.
    DATA lv_w0 TYPE timestampl.
    DATA lv_w1 TYPE timestampl.
    lv_stage = 'write'.
    GET TIME STAMP FIELD lv_w0.
    UPDATE zwide_bseg SET wrbtr0000 = wrbtr0000 + 1
     WHERE buzei <= @c_buzei_max.
    rs-changes = sy-dbcnt.
    COMMIT WORK AND WAIT.
    GET TIME STAMP FIELD lv_w1.
    rs-write_ms = ms( iv_t0 = lv_w0 iv_t1 = lv_w1 ).

    " --- the cycle: what the mode costs to APPLY ---------------------------
    " KEYS_IUD pays here what it saved above: the shadow log carries keys, so
    " the values come from a re-read of the source.
    DATA lv_c0 TYPE timestampl.
    DATA lv_c1 TYPE timestampl.
    GET TIME STAMP FIELD lv_c0.
    lv_stage = 'cycle'.
    DATA(ls_c) = zcl_erpl_rev_cdc=>run( c_target ).
    GET TIME STAMP FIELD lv_c1.
    rs-cycle_ms = ms( iv_t0 = lv_c0 iv_t1 = lv_c1 ).
    " `applied` on ty_result is a FLAG (abap_bool), not a count -- assigning it
    " to an integer converts 'X' and dumps. The row count is the three counters.
    rs-applied  = ls_c-ins + ls_c-upd + ls_c-del.
    rs-did      = ls_c-applied.
    IF ls_c-error IS NOT INITIAL.
      " The registration this cycle ran against, so a staging-name error names
      " the row that produced it instead of leaving it to be guessed at.
      DATA(ls_reg) = zcl_erpl_rev_util=>query(
        |SELECT target, source, mode, log_table, position, status | &&
        |FROM _erpl_rev_cdc WHERE target='{ c_target }'| ).
      rs-error = |cycle: { ls_c-error } -- registration: { ls_reg-rows }|.
      RETURN.
    ENDIF.

    " Two-sided, against the SOURCE rather than against the other arm. The
    " second arm updates the same rows a second time, so the two arms' values
    " differ by design and comparing them to each other would fail for a reason
    " that has nothing to do with either mode. What must hold in BOTH arms is
    " that the target agrees with SAP -- KEYS_IUD reaching that through a
    " re-read, IMAGE_IUD through the logged image. A mode that is fast because
    " it is wrong is not faster.
    lv_stage = 'verify'.
    SELECT SUM( wrbtr0000 ) FROM zwide_bseg
     WHERE buzei <= @c_buzei_max INTO @DATA(lv_src).
    rs-sap_sum  = |{ CONV i( lv_src ) }|.
    rs-duck_sum = |{ cnt( |SELECT CAST(coalesce(round(sum(wrbtr0000)),0) AS BIGINT) AS c | &&
                          |FROM { c_target }| ) }|.

    lv_stage = 'teardown'.
    zcl_erpl_rev_cdc=>teardown( c_target ).
      CATCH cx_root INTO DATA(lx).
        rs-error = |dumped at stage '{ lv_stage }': { lx->get_text( ) }|.
    ENDTRY.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.
    TRY.
        " The precondition, stated. ZWIDE_BSEG is populated by deploy-abap.sh
        " and by nothing in the e2e, so an unseeded system ran both arms over
        " zero rows and called the tie a result. Run ZCL_WIDE_BSEG first.
        SELECT COUNT(*) FROM zwide_bseg INTO @DATA(lv_have).
        ok( cond = xsdbool( lv_have > 0 )
            what = 'P-KEYS: the source table is populated'
            detail = |{ lv_have } rows -- run ZCL_WIDE_BSEG if 0| ).
        IF lv_have = 0.
          out->write( |PERF RESULT pass={ mv_pass } fail={ mv_fail }| ).
          RETURN.
        ENDIF.

        DATA(ls_k) = arm( 'KEYS_IUD' ).
        ok( cond = xsdbool( ls_k-error IS INITIAL )
            what = 'P-KEYS: the KEYS_IUD arm ran' detail = ls_k-error ).
        DATA(ls_i) = arm( 'IMAGE_IUD' ).
        ok( cond = xsdbool( ls_i-error IS INITIAL )
            what = 'P-KEYS: the IMAGE_IUD arm ran' detail = ls_i-error ).

        IF ls_k-error IS INITIAL AND ls_i-error IS INITIAL.
          out->write( |P-KEYS on { c_source } (~400 payload columns), | &&
                      |{ ls_k-changes } changes over { ls_k-rows } seeded rows| ).
          " The pipe is the string-template delimiter, so a table separator
          " inside one has to be escaped -- an unescaped column rule is not a
          " rendering problem, it is a syntax error the activation reports as a
          " bare HTTP 400.
          out->write( |  mode      \| write ms \| cycle ms \| total ms \| applied| ).
          out->write( |  KEYS_IUD  \| { ls_k-write_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_k-cycle_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_k-write_ms + ls_k-cycle_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_k-applied WIDTH = 7 ALIGN = RIGHT }| ).
          out->write( |  IMAGE_IUD \| { ls_i-write_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_i-cycle_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_i-write_ms + ls_i-cycle_ms WIDTH = 8 ALIGN = RIGHT } | &&
                      |\| { ls_i-applied WIDTH = 7 ALIGN = RIGHT }| ).

          " The comparison is only meaningful if both arms did the same work.
          ok( cond = xsdbool( ls_k-changes = ls_i-changes AND ls_k-changes > 0 )
              what = 'P-KEYS: both arms changed the same number of source rows'
              detail = |keys={ ls_k-changes } image={ ls_i-changes }| ).
          ok( cond = xsdbool( ls_k-applied = ls_i-applied AND ls_k-applied > 0 )
              what = 'P-KEYS: both arms applied the same number of rows'
              detail = |keys={ ls_k-applied }/{ ls_k-did } | &&
                       |image={ ls_i-applied }/{ ls_i-did }| ).
          ok( cond = xsdbool( ls_k-duck_sum = ls_k-sap_sum )
              what = 'P-KEYS: KEYS_IUD converged the target to what SAP holds'
              detail = |duck={ ls_k-duck_sum } sap={ ls_k-sap_sum }| ).
          ok( cond = xsdbool( ls_i-duck_sum = ls_i-sap_sum )
              what = 'P-KEYS: IMAGE_IUD converged the target to what SAP holds'
              detail = |duck={ ls_i-duck_sum } sap={ ls_i-sap_sum }| ).
        ENDIF.
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    out->write( |PERF RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
