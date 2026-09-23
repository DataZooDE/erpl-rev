CLASS zcl_erpl_rev_apetest DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
    " DELTA milestones, driven by the ZCL_ERPL_REV_APEDLT* classes in their own
    " classruns: the full m1-m6 sequence exceeds the classrun dialog budget
    " (TIME_OUT, HTTP 500, results lost). Each run_* re-runs m1 first
    " (idempotent registration), so drivers need no other setup. Order: A
    " (converge), B (surgery + carry + restore), C (recover + stale graph).
    " V (100k volume) runs alone: fixture generation plus the FULL scan fill a
    " classrun budget on their own.
    METHODS run_dlta IMPORTING iv_out TYPE REF TO if_oo_adt_classrun_out.
    METHODS run_dltb IMPORTING iv_out TYPE REF TO if_oo_adt_classrun_out.
    METHODS run_dltc IMPORTING iv_out TYPE REF TO if_oo_adt_classrun_out.
    METHODS run_dltv IMPORTING iv_out TYPE REF TO if_oo_adt_classrun_out.
    METHODS m1_register_gates.
    METHODS m4a_converge.
    METHODS m4b_carry_restore.
    METHODS m5_recover_spill.
    METHODS m6_stale_graph.
    METHODS m7_volume_100k.
    METHODS m8_drop_command.
    METHODS run_ac4 IMPORTING iv_out TYPE REF TO if_oo_adt_classrun_out.
    METHODS m9_columns_gate.
  PRIVATE SECTION.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS m2_release_gate.
    METHODS m3_full_cycle.
ENDCLASS.

CLASS zcl_erpl_rev_apetest IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.
    TRY.
        m1_register_gates( ).
        m2_release_gate( ).
        m3_full_cycle( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    out->write( |APE RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD run_dlta.
    mo = iv_out.
    TRY.
        m1_register_gates( ).
        m4a_converge( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        mo->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    mo->write( |APEDLTA RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD run_dltb.
    mo = iv_out.
    TRY.
        m1_register_gates( ).
        m4b_carry_restore( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        mo->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    mo->write( |APEDLTB RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD run_dltc.
    mo = iv_out.
    TRY.
        m1_register_gates( ).
        m5_recover_spill( ).
        m6_stale_graph( ).
        m8_drop_command( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        mo->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    mo->write( |APEDLTC RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD run_dltv.
    mo = iv_out.
    TRY.
        m7_volume_100k( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        " The DB message hides in the nested chain (CX_SY_OPEN_SQL_DB's own
        " text is generic): walk it so the classrun reports the real cause.
        DATA(lv_t) = lx->get_text( ).
        DATA(lo_prev) = lx->previous.
        WHILE lo_prev IS BOUND.
          lv_t = lv_t && ` <- ` && lo_prev->get_text( ).
          lo_prev = lo_prev->previous.
        ENDWHILE.
        mo->write( |DUMP: { lv_t }| ).
    ENDTRY.
    mo->write( |APEDLTV RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD run_ac4.
    mo = iv_out.
    TRY.
        m1_register_gates( ).
        m2_release_gate( ).
        m9_columns_gate( ).
      CATCH cx_root INTO DATA(lx).
        mv_fail = mv_fail + 1.
        mo->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
    mo->write( |APEDLTN RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

  METHOD m1_register_gates.
    " FR-2: APE registration probes the source and fails fast with a telling
    " error. The negative gates run before any server contact, so they need
    " no running server; the positive path does.
    DATA ls TYPE zcl_erpl_rev_delta=>ty_state.
    ls-target = 'ape_t_neg1'.
    ls-method = 'APE_DELTA'.
    ls-source_from = 'SFLIGHT'.
    ls-keys = 'MANDT,CARRID,CONNID,FLDATE'.
    ls-cadence = 'hourly'.
    ls-subscriber_process = 'ERPLREV99'.
    DATA(lv_e) = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'CDS' ) what = 'M1 non-CDS source refused' detail = lv_e ).

    ls-target = 'ape_t_neg2'.
    ls-source_from = 'ZERPL_C_FLIGHTS'.
    ls-keys = 'CARRID,CONNID,FLDATE'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'dataExtraction' ) what = 'M1 missing annotation refused' detail = lv_e ).

    ls-target = 'ape_t_neg3'.
    ls-source_from = 'ZERPL_APE_D'.
    ls-keys = 'RID'.
    CLEAR ls-subscriber_process.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'subscriber' ) what = 'M1 missing subscriber refused' detail = lv_e ).

    " Enabled for extraction but not for replication: DELTA refused, FULL fine.
    ls-target = 'ape_t_neg4'.
    ls-source_from = 'ZERPL_APE_EMPTY'.
    ls-keys = 'RID'.
    ls-subscriber_process = 'ERPLREV99'.
    ls-cadence = 'hourly'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'replication' ) what = 'M1 delta-without-capture refused' detail = lv_e ).

    ls-subscriber_process = 'ERPLREV99'.
    ls-cadence = 'micro:30'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'micro' ) what = 'M1 micro cadence refused' detail = lv_e ).

    " Positive paths: DELTA (needs changeDataCapture, which ZERPL_APE_D has)
    " and FULL register cleanly and the knobs survive the round trip. The
    " fixtures are unreleased, so these carry the override; the release gate
    " itself is m2's subject, not m1's.
    ls-target = 'ape_t_pos'.
    ls-allow_unreleased = 'true'.
    ls-source_from = 'ZERPL_APE_D'.
    ls-keys = 'RID'.
    ls-cadence = 'hourly'.
    ls-chunk_size = 100.
    ls-wireformat = 'Required Conversions Plus Time Format and Currency'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL ) what = 'M1 delta registers' detail = lv_e ).
    DATA(ls_got) = zcl_erpl_rev_delta=>state( 'ape_t_pos' ).
    ok( cond = xsdbool( ls_got-subscriber_process = 'ERPLREV99' )
        what = 'M1 subscriber survives' detail = ls_got-subscriber_process ).
    ok( cond = xsdbool( ls_got-chunk_size = 100 )
        what = 'M1 chunk size survives' detail = |{ ls_got-chunk_size }| ).

    ls-target = 'ape_t_full'.
    ls-method = 'APE_FULL'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL ) what = 'M1 full registers' detail = lv_e ).

    ls-target = 'ape_t_fempty'.
    ls-source_from = 'ZERPL_APE_EMPTY'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL ) what = 'M1 full-without-capture registers' detail = lv_e ).
  ENDMETHOD.

  METHOD m2_release_gate.
    " FR-2 release gate: the ARS catalog (I_APISFORCLOUDDEVELOPMENT), not XCO
    " (STOB existence check fails before ARS) and not CDS_PUBLISHED (empty
    " even for SAP views). ZERPL_APE_D is $TMP: no catalog row. I_ADDRESS_2
    " is C1-RELEASED and extraction-enabled: the positive control.
    DATA ls TYPE zcl_erpl_rev_delta=>ty_state.
    ls-method = 'APE_DELTA'.
    ls-source_from = 'ZERPL_APE_D'.
    ls-keys = 'RID'.
    ls-cadence = 'hourly'.
    ls-subscriber_process = 'ERPLREV99'.

    ls-target = 'ape_t_rel1'.
    CLEAR ls-allow_unreleased.
    DATA(lv_e) = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'release' ) what = 'M2 unreleased refused' detail = lv_e ).

    ls-target = 'ape_t_rel2'.
    ls-allow_unreleased = 'true'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL ) what = 'M2 override accepted' detail = lv_e ).
    DATA(ls_got) = zcl_erpl_rev_delta=>state( 'ape_t_rel2' ).
    ok( cond = xsdbool( ls_got-last_warning CS 'WARN' )
        what = 'M2 override WARN observable' detail = ls_got-last_warning ).

    ls-target = 'ape_t_rel3'.
    ls-method = 'APE_FULL'.
    ls-source_from = 'I_ADDRESS_2'.
    ls-keys = 'AddressID,AddressPersonID,AddressRepresentationCode'.
    CLEAR ls-allow_unreleased.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL ) what = 'M2 released accepted' detail = lv_e ).
  ENDMETHOD.

  METHOD m3_full_cycle.
    " AC-1 first half: APE_FULL over ZERPL_APE_D (7 rows) matches the Open-SQL
    " read cell-for-cell -- except AMOUNT, which the engine currency-shifts by
    " design under ...Plus Currency wire formats (BRD FR-8). Keys + DESCR must
    " match exactly; counts must agree on all three sides (source, staged,
    " target). A second run reconciles to zero deletes (CURR semantics hold).
    TYPES: BEGIN OF ty_row, rid TYPE string, descr TYPE string, END OF ty_row.
    TYPES tt_row TYPE STANDARD TABLE OF ty_row WITH EMPTY KEY.
    DATA lt_src TYPE tt_row.
    SELECT rid, descr FROM zerpl_ape_d ORDER BY rid INTO TABLE @lt_src.
    DATA(lv_src) = lines( lt_src ).

    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_full' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL )
        what = 'M3 full runs' detail = ls_run-error ).
    ok( cond = xsdbool( ls_run-rows = lv_src )
        what = 'M3 staged all source rows' detail = |{ ls_run-rows }/{ lv_src }| ).
    ok( cond = xsdbool( ls_run-del = 0 )
        what = 'M3 full scan deletes nothing' detail = |{ ls_run-del }| ).

    DATA(ls_q) = zcl_erpl_rev_util=>query(
      `SELECT rid, descr FROM ape_t_full ORDER BY rid` ).
    ok( cond = xsdbool( ls_q-error IS INITIAL )
        what = 'M3 target readable' detail = ls_q-error ).
    DATA lt_got TYPE tt_row.
    TRY.
        /ui2/cl_json=>deserialize( EXPORTING json = ls_q-rows CHANGING data = lt_got ).
      CATCH cx_root INTO DATA(lx).
        ok( cond = abap_false what = 'M3 target parses'
            detail = lx->get_text( ) ).
        RETURN.
    ENDTRY.
    ok( cond = xsdbool( lt_got = lt_src )
        what = 'M3 keys+descr match Open SQL' detail = |{ lines( lt_got ) }/{ lv_src }| ).

    " Idempotent re-run: same scan, same content, still no deletes.
    ls_run = zcl_erpl_rev_delta=>run( 'ape_t_full' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-del = 0 )
        what = 'M3 re-run clean' detail = ls_run-error ).
  ENDMETHOD.

  METHOD m4a_converge.
    " AC-1 second half, part 1: one DELTA cycle converges the target onto the
    " source (initial state arrives as U). Split from the carry because the
    " whole m4 exceeds the classrun dialog budget (TIME_OUT, results lost).
    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_pos' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL )
        what = 'M4A delta converges' detail = ls_run-error ).
    SELECT COUNT(*) FROM zerpl_ape_d INTO @DATA(lv_src).
    DATA(lv_tgt) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_pos` ).
    ok( cond = xsdbool( lv_tgt = lv_src )
        what = 'M4A converged count' detail = |{ lv_tgt }/{ lv_src }| ).
  ENDMETHOD.

  METHOD m4b_carry_restore.
    " AC-1 second half, part 2: base-table surgery (I + U + D) must surface
    " after one cycle; restoring the base must converge back. COMMITs are
    " explicit (the engine reads in another session). No explicit WAIT: GC +
    " recover + seed + subscription lookup + preparation keep the cycle busy
    " for 1-2 minutes, past the ~40 s capture job cadence, before polling.
    " Snapshot the rows surgery will touch, so the test restores exactly.
    SELECT SINGLE * FROM zerpl_ape_t WHERE rid = 'R0001' INTO @DATA(ls_r1).
    SELECT SINGLE * FROM zerpl_ape_t WHERE rid = 'R0002' INTO @DATA(ls_r2).
    INSERT zerpl_ape_t FROM @( VALUE #( rid = 'R0005' descr = 'delta five'
      amount = '99.99' waers = 'EUR' qty = 5 rev = 0 ) ).
    UPDATE zerpl_ape_t SET descr = 'changed by m4' WHERE rid = 'R0001'.
    DELETE FROM zerpl_ape_t WHERE rid = 'R0002'.
    COMMIT WORK.
    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_pos' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL )
        what = 'M4B delta carries I/U/D' detail = ls_run-error ).
    " The streamed-delete witness comes AFTER the restore below (ls_run2-del):
    " the engine hands a commit's first two DMLs per window -- the carry's own
    " D is the clipped third, the restore commit opens with its D. Asserting
    " del on the carry alone would demand single-cycle D timeliness the engine
    " does not provide; the window-wide counter still witnesses a real streamed
    " delete (combined 0 would mean the D path is dead, seed-masking or not).
    TYPES: BEGIN OF ty_d, descr TYPE string, END OF ty_d.
    DATA(ls_q) = zcl_erpl_rev_util=>query(
      `SELECT descr FROM ape_t_pos WHERE rid='R0005'` ).
    DATA lt_d TYPE STANDARD TABLE OF ty_d WITH EMPTY KEY.
    /ui2/cl_json=>deserialize( EXPORTING json = ls_q-rows CHANGING data = lt_d ).
    ok( cond = xsdbool( lines( lt_d ) = 1 )
        what = 'M4B insert arrived' detail = ls_q-error ).
    ls_q = zcl_erpl_rev_util=>query(
      `SELECT descr FROM ape_t_pos WHERE rid='R0001'` ).
    CLEAR lt_d.
    /ui2/cl_json=>deserialize( EXPORTING json = ls_q-rows CHANGING data = lt_d ).
    READ TABLE lt_d INTO DATA(ls_got) INDEX 1.
    ok( cond = xsdbool( sy-subrc = 0 AND ls_got-descr = 'changed by m4' )
        what = 'M4B update arrived' detail = ls_q-error ).
    DATA(lv_tgt) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_pos WHERE rid='R0002'` ).
    ok( cond = xsdbool( lv_tgt = 0 )
        what = 'M4B delete applied' detail = |{ lv_tgt }| ).

    " Restore the fixture and converge back: the test leaves no trace.
    DELETE FROM zerpl_ape_t WHERE rid = 'R0005'.
    UPDATE zerpl_ape_t SET descr = @ls_r1-descr WHERE rid = 'R0001'.
    INSERT zerpl_ape_t FROM @ls_r2.
    COMMIT WORK.
    DATA(ls_run2) = zcl_erpl_rev_delta=>run( 'ape_t_pos' ).
    ok( cond = xsdbool( ls_run-del + ls_run2-del >= 1 )
        what = 'M4B delete streamed in window' detail = |carry={ ls_run-del } restore={ ls_run2-del }| ).
    SELECT COUNT(*) FROM zerpl_ape_d INTO @DATA(lv_src).
    lv_tgt = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_pos` ).
    ok( cond = xsdbool( ls_run2-error IS INITIAL AND ls_run2-skipped IS INITIAL
                        AND lv_tgt = lv_src )
        what = 'M4B restore converges' detail = |{ lv_tgt }/{ lv_src } { ls_run2-error }| ).
  ENDMETHOD.

  METHOD m5_recover_spill.
    " AC-2 without killing anything: plant two spill batches past the merge
    " position (a U then its D: net zero), replay via APE_RECOVER with no SAP
    " contact, and assert the replay, the discard, and the no-op second run.
    DATA(lv_pos) = zcl_erpl_rev_delta=>scalar(
      `SELECT spill_batch AS b FROM _erpl_rev_delta_state WHERE target='ape_t_pos'` ).
    " Crafted envelopes, not engine output: Table kind + Fields incl. the
    " indicator, bodies with no trailing newline (EOF terminates the row).
    DATA(lv_f) =
      `{"Name":"RID","Type":"CHAR10","Kind":"C","Length":10,"Decimals":0},` &&
      `{"Name":"DESCR","Type":"CHAR40","Kind":"C","Length":40,"Decimals":0},` &&
      `{"Name":"AMOUNT","Type":"S_PRICE","Kind":"P","Length":8,"Decimals":2},` &&
      `{"Name":"WAERS","Type":"CUKY","Kind":"C","Length":5,"Decimals":0},` &&
      `{"Name":"QTY","Type":"INT4","Kind":"I","Length":4,"Decimals":0},` &&
      `{"Name":"REV","Type":"INT4","Kind":"I","Length":4,"Decimals":0},` &&
      `{"Name":"/1DH/OPERATION","Type":"CHAR1","Kind":"C","Length":1,"Decimals":0}`.
    DATA(lv_p1) =
      `{"Encoding":"csv","Attributes":{"message.batchIndex":` && |{ lv_pos + 1 }| &&
      `,"message.lastBatch":false,"ABAP":{"Kind":"Table","Header":{},"Fields":[` &&
      lv_f && `]}},"Body":"R0998,recovered,1.00,EUR,1,0,U"}`.
    DATA(lv_p2) =
      `{"Encoding":"csv","Attributes":{"message.batchIndex":` && |{ lv_pos + 2 }| &&
      `,"message.lastBatch":false,"ABAP":{"Kind":"Table","Header":{},"Fields":[` &&
      lv_f && `]}},"Body":"R0998,,,,,,D"}`.
    zcl_erpl_rev_util=>query(
      |INSERT INTO _erpl_rev_ape_spill(target, batch_index, payload) VALUES | &&
      |('ape_t_pos', { lv_pos + 1 }, '{ lv_p1 }')| ).
    zcl_erpl_rev_util=>query(
      |INSERT INTO _erpl_rev_ape_spill(target, batch_index, payload) VALUES | &&
      |('ape_t_pos', { lv_pos + 2 }, '{ lv_p2 }')| ).
    DATA(ls_rec) = zcl_erpl_rev_delta=>plan_json(
      iv_action = 'APE_RECOVER' iv_target = 'ape_t_pos' ).
    ok( cond = xsdbool( ls_rec-error IS INITIAL )
        what = 'M5 recover runs' detail = ls_rec-error ).
    DATA(lv_rows) = zcl_erpl_rev_delta=>jstr( iv_json = ls_rec-json iv_key = 'rows' ).
    ok( cond = xsdbool( lv_rows = '2' )
        what = 'M5 both batches replayed' detail = ls_rec-json ).
    DATA(lv_left) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM _erpl_rev_ape_spill WHERE target='ape_t_pos'` ).
    ok( cond = xsdbool( lv_left = 0 )
        what = 'M5 spill discarded' detail = |{ lv_left }| ).
    DATA(lv_ghost) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_pos WHERE rid='R0998'` ).
    ok( cond = xsdbool( lv_ghost = 0 )
        what = 'M5 net-zero replay' detail = |{ lv_ghost }| ).
    ls_rec = zcl_erpl_rev_delta=>plan_json(
      iv_action = 'APE_RECOVER' iv_target = 'ape_t_pos' ).
    lv_rows = zcl_erpl_rev_delta=>jstr( iv_json = ls_rec-json iv_key = 'rows' ).
    ok( cond = xsdbool( lv_rows = '0' )
        what = 'M5 second recover no-op' detail = ls_rec-json ).
  ENDMETHOD.

  METHOD m6_stale_graph.
    " AC-3: abandon a running graph (stays R, session gone with this test),
    " then run a DELTA cycle. The cycle's GC must flip the abandoned graph to
    " stopped and proceed -- the next ordinary cycle resumes.
    DATA(lv_sub) = |M6STALE{ sy-datum }{ sy-uzeit }|.
    DATA(lv_graph) =
      `{"Attributes":{"graphid":"erpl_rev_m6","protocol":"v6",` &&
      `"graphkind":"user","multiplicity":"1"},` &&
      `"Processes":{"reader":{"Component":"com.sap.abap.cds.reader.v2",` &&
      `"Metadata":{"Config":{"subscriptionType":"New",` &&
      `"subscriptionName":"` && lv_sub &&
      `","cdsname":"ZERPL_APE_D",` &&
      `"action":"Replication","chunkSize":100}}}},` &&
      `"Connections":[],"Outports":{"0":{"Process":"reader",` &&
      `"Port":"outMessageData","Metadata":{"portNumber":0,"type":"message"}}}},` &&
      `"vTypes":{}}`.
    DATA lv_uuid TYPE char32.
    DATA lt_m TYPE dhape_t_graph_msg.
    CALL FUNCTION 'DHAPE_GRAPH_MANAGER'
      EXPORTING iv_mode = 'C' iv_appid = 'ERPL_REV' iv_graph = lv_graph
      IMPORTING ev_graph_uuid = lv_uuid et_msg = lt_m.
    IF lv_uuid IS INITIAL.
      LOOP AT lt_m INTO DATA(ls_cm).
        FIND REGEX `Graph UUID is ([0-9A-Fa-f]{32})` IN ls_cm-text SUBMATCHES lv_uuid.
        IF sy-subrc = 0. EXIT. ENDIF.
      ENDLOOP.
    ENDIF.
    ok( cond = xsdbool( lv_uuid IS NOT INITIAL )
        what = 'M6 wedge planted' detail = 'no uuid' ).
    " Abandoned here: no stop, the test session ends and the graph stays R.
    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_pos' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL )
        what = 'M6 cycle survives stale graph' detail = ls_run-error ).
    SELECT SINGLE status FROM dhape_graph WHERE uuid = @lv_uuid INTO @DATA(lv_st).
    ok( cond = xsdbool( sy-subrc = 0 AND lv_st <> 'R' )
        what = 'M6 stale graph flipped' detail = |{ lv_st }| ).
  ENDMETHOD.

  METHOD m7_volume_100k.
    " AC-1 volume: 100k rows through APE_FULL with key-count == row-count.
    " Dedicated fixture (ZERPL_APE_V has no CDC capture), so the burst leaves
    " no subscription backlog for later DELTA cycles; the table is emptied at
    " the end. chunk_size 20000 -> ~5 handovers. Cell-for-cell is m3's subject
    " (7 rows); here counts plus key uniqueness plus spot checks.
    CONSTANTS lc_n TYPE i VALUE 100000.
    " Idempotent start: a dead attempt's leftovers would double-count. The
    " precondition is loud: a nonzero after-count names the leftovers.
    SELECT COUNT(*) FROM zerpl_ape_v INTO @DATA(lv_pre).
    SELECT MIN( rid ) FROM zerpl_ape_v INTO @DATA(lv_pre_min).
    SELECT MAX( rid ) FROM zerpl_ape_v INTO @DATA(lv_pre_max).
    DELETE FROM zerpl_ape_v WHERE rid LIKE 'V%'.
    COMMIT WORK.
    SELECT COUNT(*) FROM zerpl_ape_v INTO @DATA(lv_pre2).
    ok( cond = xsdbool( lv_pre2 = 0 )
        what = 'M7 starts empty' detail = |before={ lv_pre } [{ lv_pre_min }..{ lv_pre_max }] after={ lv_pre2 }| ).
    IF lv_pre2 <> 0. RETURN. ENDIF.
    " Loop-local DATA keeps stale values across passes (the M4B restore
    " proved it for VALUE initialisation), so every counter lives outside the
    " loops. Keys are built digit by digit (MOD/DIV): template PAD = '0' pads
    " TRAILING (collapsing 1/10/100/... onto one key), and a strlen-guarded
    " prepend loop stopped one zero short on this kernel. No measuring, no
    " padding options -- nine digit extractions always yield nine digits.
    DATA lt_gen TYPE STANDARD TABLE OF zerpl_ape_v WITH EMPTY KEY.
    DATA: lv_base TYPE i, lv_k TYPE i, lv_batch TYPE i, lv_t TYPE i,
          lv_num TYPE string, lv_rid TYPE char10.
    DO 10 TIMES.
      CLEAR lt_gen.
      lv_batch = sy-index.
      lv_base = ( lv_batch - 1 ) * 10000.
      DO 10000 TIMES.
        lv_k = lv_base + sy-index.
        lv_t = lv_k.
        CLEAR lv_num.
        DO 9 TIMES.
          lv_num = |{ lv_t MOD 10 }{ lv_num }|.
          lv_t = lv_t DIV 10.
        ENDDO.
        lv_rid = 'V' && lv_num.
        APPEND VALUE #( rid   = lv_rid
                        descr = |volume { lv_k }|
                        amount = '123.45' waers = 'EUR' qty = 1 rev = 0 ) TO lt_gen.
      ENDDO.
      TRY.
          INSERT zerpl_ape_v FROM TABLE @lt_gen.
        CATCH cx_sy_open_sql_db INTO DATA(lx_gen).
          READ TABLE lt_gen INTO DATA(ls_first) INDEX 1.
          READ TABLE lt_gen INTO DATA(ls_last) INDEX lines( lt_gen ).
          ok( cond = abap_false what = 'M7 batch insert'
              detail = |batch={ lv_batch } base={ lv_base } keys=[{ ls_first-rid }..{ ls_last-rid }] rows={ lines( lt_gen ) }: { lx_gen->get_text( ) }| ).
          RETURN.
      ENDTRY.
    ENDDO.
    COMMIT WORK.
    SELECT COUNT(*) FROM zerpl_ape_v INTO @DATA(lv_src).
    ok( cond = xsdbool( lv_src = lc_n )
        what = 'M7 fixture generated' detail = |{ lv_src }| ).
    SELECT MIN( rid ) FROM zerpl_ape_v INTO @DATA(lv_gen_min).
    SELECT MAX( rid ) FROM zerpl_ape_v INTO @DATA(lv_gen_max).
    ok( cond = xsdbool( lv_gen_min = 'V000000001' AND lv_gen_max = 'V000100000' )
        what = 'M7 source keys exact' detail = |[{ lv_gen_min }..{ lv_gen_max }]| ).

    DATA ls TYPE zcl_erpl_rev_delta=>ty_state.
    ls-target = 'ape_t_vol'.
    ls-method = 'APE_FULL'.
    ls-source_from = 'ZERPL_APE_VOL'.
    ls-keys = 'RID'.
    ls-cadence = 'hourly'.
    ls-subscriber_process = 'ERPLREV99'.
    ls-chunk_size = 20000.
    ls-wireformat = 'Required Conversions Plus Time Format and Currency'.
    ls-allow_unreleased = 'true'.
    DATA(lv_e) = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL )
        what = 'M7 volume registers' detail = lv_e ).

    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_vol' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL )
        what = 'M7 full runs' detail = ls_run-error ).
    ok( cond = xsdbool( ls_run-rows = lv_src )
        what = 'M7 staged all source rows' detail = |{ ls_run-rows }/{ lv_src }| ).

    DATA(lv_cnt) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_vol` ).
    ok( cond = xsdbool( lv_cnt = lv_src )
        what = 'M7 target row-count' detail = |{ lv_cnt }/{ lv_src }| ).
    DATA(lv_keys) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(DISTINCT rid) AS c FROM ape_t_vol` ).
    ok( cond = xsdbool( lv_keys = lv_src )
        what = 'M7 key-count == row-count' detail = |{ lv_keys }/{ lv_src }| ).
    DATA(ls_qmm) = zcl_erpl_rev_util=>query(
      `SELECT MIN(rid) AS m, MAX(rid) AS x FROM ape_t_vol` ).
    ok( cond = xsdbool( ls_qmm-error IS INITIAL )
        what = 'M7 key range readable' detail = ls_qmm-error ).
    TYPES: BEGIN OF ty_mm, m TYPE string, x TYPE string, END OF ty_mm.
    DATA lt_mm TYPE STANDARD TABLE OF ty_mm WITH EMPTY KEY.
    /ui2/cl_json=>deserialize( EXPORTING json = ls_qmm-rows CHANGING data = lt_mm ).
    READ TABLE lt_mm INTO DATA(ls_mm) INDEX 1.
    ok( cond = xsdbool( sy-subrc = 0 AND ls_mm-m = 'V000000001' AND ls_mm-x = 'V000100000' )
        what = 'M7 key range exact' detail = |{ ls_qmm-rows }| ).

    TYPES: BEGIN OF ty_d, descr TYPE string, END OF ty_d.
    DATA lt_d TYPE STANDARD TABLE OF ty_d WITH EMPTY KEY.
    DATA(ls_q) = zcl_erpl_rev_util=>query(
      `SELECT descr FROM ape_t_vol WHERE rid='V000000001'` ).
    /ui2/cl_json=>deserialize( EXPORTING json = ls_q-rows CHANGING data = lt_d ).
    READ TABLE lt_d INTO DATA(ls_got) INDEX 1.
    ok( cond = xsdbool( sy-subrc = 0 AND condense( ls_got-descr ) = 'volume 1' )
        what = 'M7 first row spot-check'
        detail = |rows={ lines( lt_d ) } got='{ COND string( WHEN sy-subrc = 0 THEN ls_got-descr ELSE '' ) }' err={ ls_q-error }| ).
    ls_q = zcl_erpl_rev_util=>query(
      `SELECT descr FROM ape_t_vol WHERE rid='V000100000'` ).
    CLEAR lt_d.
    /ui2/cl_json=>deserialize( EXPORTING json = ls_q-rows CHANGING data = lt_d ).
    READ TABLE lt_d INTO ls_got INDEX 1.
    ok( cond = xsdbool( sy-subrc = 0 AND condense( ls_got-descr ) = 'volume 100000' )
        what = 'M7 last row spot-check'
        detail = |rows={ lines( lt_d ) } got='{ COND string( WHEN sy-subrc = 0 THEN ls_got-descr ELSE '' ) }' err={ ls_q-error }| ).

    " Leave no trace: the next m7 (and every other milestone) starts clean.
    DELETE FROM zerpl_ape_v WHERE rid LIKE 'V%'.
    COMMIT WORK.
    SELECT COUNT(*) FROM zerpl_ape_v INTO @DATA(lv_left).
    ok( cond = xsdbool( lv_left = 0 )
        what = 'M7 fixture cleaned' detail = |{ lv_left }| ).
  ENDMETHOD.

  METHOD m8_drop_command.
    " FR-9: the drop command erases the SAP-side subscription and the state
    " row. Throwaway target on the shared fixture with its OWN subscriber name
    " (ERPLREVDROP), so the erase cannot touch any milestone's subscription.
    " One small DELTA cycle first: it creates the named subscription the drop
    " must erase, so the 'erased' message witnesses the find+erase path (a
    " missing subscription would report 'no live subscription' instead).
    DATA ls TYPE zcl_erpl_rev_delta=>ty_state.
    ls-target = 'ape_t_drop1'.
    ls-method = 'APE_DELTA'.
    ls-source_from = 'ZERPL_APE_D'.
    ls-keys = 'RID'.
    ls-cadence = 'hourly'.
    ls-subscriber_process = 'ERPLREVDROP'.
    ls-chunk_size = 100.
    ls-wireformat = 'Required Conversions Plus Time Format and Currency'.
    ls-allow_unreleased = 'true'.
    DATA(lv_e) = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL )
        what = 'M8 drop target registers' detail = lv_e ).

    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_drop1' ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL )
        what = 'M8 pre-drop cycle runs' detail = ls_run-error ).

    " In-flight guard: a fresh RUNNING lease refuses the drop.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now() WHERE target='ape_t_drop1'| ).
    DATA(lv_refused) = zcl_erpl_rev_delta=>drop( 'ape_t_drop1' ).
    ok( cond = xsdbool( lv_refused CS 'ERROR:' )
        what = 'M8 drop refuses in-flight' detail = lv_refused ).
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='IDLE' WHERE target='ape_t_drop1'| ).

    " The drop: subscription erase CONFIRMED (re-lookup finds nothing),
    " state row gone. 'erased' alone is not enough: the pre-witness message
    " claimed erasures the engine never confirmed (W2).
    DATA(lv_msg) = zcl_erpl_rev_delta=>drop( 'ape_t_drop1' ).
    ok( cond = xsdbool( NOT lv_msg CS 'ERROR:' AND lv_msg CS 'erase confirmed' )
        what = 'M8 drop erases subscription' detail = lv_msg ).
    DATA(ls_gone) = zcl_erpl_rev_delta=>state( 'ape_t_drop1' ).
    ok( cond = xsdbool( ls_gone-target IS INITIAL )
        what = 'M8 state row gone' detail = ls_gone-target ).

    " Idempotent: the second drop reports, it does not fail.
    DATA(lv_msg2) = zcl_erpl_rev_delta=>drop( 'ape_t_drop1' ).
    ok( cond = xsdbool( NOT lv_msg2 CS 'ERROR:' )
        what = 'M8 second drop idempotent' detail = lv_msg2 ).

    " Clean slate: re-register and converge again, then drop for real so the
    " test leaves no trace (no state row, no subscription).
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL )
        what = 'M8 re-registers' detail = lv_e ).
    ls_run = zcl_erpl_rev_delta=>run( 'ape_t_drop1' ).
    SELECT COUNT(*) FROM zerpl_ape_d INTO @DATA(lv_src).
    DATA(lv_tgt) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_drop1` ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL
                        AND lv_tgt = lv_src )
        what = 'M8 re-run converges' detail = |{ lv_tgt }/{ lv_src } { ls_run-error }| ).
    " Final drop under a STALE running lease: an orphan is not a cycle, so
    " the fenced delete proceeds (a fresh one refused above). Leaves no
    " trace: no state row, erase witnessed.
    zcl_erpl_rev_util=>query(
      |UPDATE _erpl_rev_delta_state SET status='RUNNING', | &&
      |lease_ts=now() - INTERVAL '3600' SECOND WHERE target='ape_t_drop1'| ).
    lv_msg = zcl_erpl_rev_delta=>drop( 'ape_t_drop1' ).
    ok( cond = xsdbool( NOT lv_msg CS 'ERROR:' AND lv_msg CS 'erase confirmed' )
        what = 'M8 stale lease does not block' detail = lv_msg ).
    ls_gone = zcl_erpl_rev_delta=>state( 'ape_t_drop1' ).
    ok( cond = xsdbool( ls_gone-target IS INITIAL )
        what = 'M8 no trace' detail = ls_gone-target ).
  ENDMETHOD.

  METHOD m9_columns_gate.
    " BR-8/AC-4, durable home of the W6 proofs: unknown names fail at plan
    " time; non-APE methods refuse the knob; the subset shapes the target
    " and replicates; a changed set is refused; the drop leaves no trace.
    " (Filter refusal has no ABAP channel -- has_filter is set only by the
    " graph-spec builder -- so it stays a C++ unit proof in
    " test_ape_register.)
    DATA ls TYPE zcl_erpl_rev_delta=>ty_state.
    ls-target = 'ape_t_negcol'.
    ls-method = 'APE_DELTA'.
    ls-source_from = 'ZERPL_APE_D'.
    ls-keys = 'RID'.
    ls-cadence = 'hourly'.
    ls-subscriber_process = 'ERPLREVN'.
    ls-allow_unreleased = 'true'.
    ls-columns = 'RID,NOPE_COL'.
    DATA(lv_e) = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'unknown column' )
        what = 'M9 unknown column refused' detail = lv_e ).

    ls-target = 'ape_t_negcol2'.
    ls-method = 'SNAPSHOT'.
    ls-columns = 'RID'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e CS 'APE-only' )
        what = 'M9 non-APE refuses columns' detail = lv_e ).
    ls-method = 'APE_DELTA'.

    " Subset E2E on its own target + subscriber (never touches milestones).
    ls-target = 'ape_t_col1'.
    ls-subscriber_process = 'ERPLREVCOL'.
    ls-columns = 'RID,DESCR'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL )
        what = 'M9 subset registers' detail = lv_e ).
    DATA(ls_run) = zcl_erpl_rev_delta=>run( 'ape_t_col1' ).
    SELECT COUNT(*) FROM zerpl_ape_d INTO @DATA(lv_src).
    DATA(lv_tgt) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM ape_t_col1` ).
    DATA(lv_ncol) = zcl_erpl_rev_delta=>scalar(
      `SELECT count(*) AS c FROM duckdb_columns() WHERE lower(table_name) = 'ape_t_col1'` ).
    ok( cond = xsdbool( ls_run-error IS INITIAL AND ls_run-skipped IS INITIAL
                        AND lv_tgt = lv_src AND lv_ncol = 2 )
        what = 'M9 subset replicates' detail = |{ lv_tgt }/{ lv_src } cols={ lv_ncol } { ls_run-error }| ).

    " Changed set on the existing target: refused, drop + re-register.
    ls-columns = 'RID,AMOUNT'.
    lv_e = zcl_erpl_rev_delta=>register( ls ).
    ok( cond = xsdbool( lv_e IS INITIAL )
        what = 'M9 changed set re-registers' detail = lv_e ).
    ls_run = zcl_erpl_rev_delta=>run( 'ape_t_col1' ).
    ok( cond = xsdbool( ls_run-error CS 'drop and re-register' )
        what = 'M9 drift refused' detail = ls_run-error ).

    DATA(lv_msg) = zcl_erpl_rev_delta=>drop( 'ape_t_col1' ).
    ok( cond = xsdbool( NOT lv_msg CS 'ERROR:' AND lv_msg CS 'erase confirmed' )
        what = 'M9 drop clean' detail = lv_msg ).
    DATA(ls_gone) = zcl_erpl_rev_delta=>state( 'ape_t_col1' ).
    ok( cond = xsdbool( ls_gone-target IS INITIAL )
        what = 'M9 no trace' detail = ls_gone-target ).
  ENDMETHOD.

ENDCLASS.
