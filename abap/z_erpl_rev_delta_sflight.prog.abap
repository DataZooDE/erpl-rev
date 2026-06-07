*&---------------------------------------------------------------------*
*& Report  Z_ERPL_REV_DELTA_SFLIGHT
*&---------------------------------------------------------------------*
*& Interactive SFLIGHT delta demo — run it in SAP GUI (SA38 -> F8) to drive the
*& whole incremental-extraction loop by hand on the familiar flight-booking demo
*& model, and SEE what landed in DuckDB.
*&
*&   [Setup]  full-load SFLIGHT -> DuckDB table `sflight`, register it as a
*&            SNAPSHOT delta target (SFLIGHT has no change column; the snapshot
*&            anti-join reflects inserts, updates AND physical deletes).
*&   [Update] bump one flight's PRICE (+100) and SEATSOCC (+1) in SAP.
*&   [Insert] clone a flight of (carrid,connid) at the chosen date.
*&   [Delete] physically delete one flight.
*&   [Run delta] run ONE delta cycle — the change is merged into `sflight`.
*&   [Refresh]  just re-read the DuckDB target.
*&
*& The log pane (top) records every action + the cycle's ins/upd/del counts and a
*& SAP-source-vs-DuckDB row count; the ALV pane (bottom) shows the current DuckDB
*& `sflight` contents — so you can watch a real SAP change flow into DuckDB and
*& debug exactly what was loaded. No dynpro: a docking container built in PBO,
*& deployable entirely via ADT.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_delta_sflight.

CONSTANTS c_target TYPE string VALUE 'sflight'.

TYPES: gtt_txt TYPE STANDARD TABLE OF char255 WITH EMPTY KEY.

DATA: go_dock   TYPE REF TO cl_gui_docking_container,
      go_split  TYPE REF TO cl_gui_splitter_container,
      go_top    TYPE REF TO cl_gui_container,
      go_bottom TYPE REF TO cl_gui_container,
      go_logbox TYPE REF TO cl_gui_textedit,
      go_salv   TYPE REF TO cl_salv_table,
      go_msg    TYPE REF TO cl_gui_textedit,
      gr_result TYPE REF TO data,        " DuckDB target rows (kept alive for the ALV)
      gt_log    TYPE gtt_txt,            " demo log lines (newest at the bottom)
      gv_err    TYPE string.

" Change target (also the input-enabled fields that keep the selection screen alive
" so the docking container renders).
PARAMETERS: p_carr TYPE s_carr_id,
            p_conn TYPE s_conn_id,
            p_date TYPE s_date.
SELECTION-SCREEN BEGIN OF LINE.
SELECTION-SCREEN PUSHBUTTON  2(18) b_setup USER-COMMAND setup.
SELECTION-SCREEN PUSHBUTTON 21(18) b_run   USER-COMMAND run.
SELECTION-SCREEN PUSHBUTTON 40(18) b_view  USER-COMMAND view.
SELECTION-SCREEN END OF LINE.
SELECTION-SCREEN BEGIN OF LINE.
SELECTION-SCREEN COMMENT  2(18) c_one.
SELECTION-SCREEN PUSHBUTTON 21(18) b_upd USER-COMMAND upd.
SELECTION-SCREEN PUSHBUTTON 40(18) b_ins USER-COMMAND ins.
SELECTION-SCREEN PUSHBUTTON 59(18) b_del USER-COMMAND del.
SELECTION-SCREEN END OF LINE.
" Mass operations (bulk demo flights in a far-future date range).
PARAMETERS p_mass TYPE i DEFAULT 1000.
SELECTION-SCREEN BEGIN OF LINE.
SELECTION-SCREEN COMMENT  2(18) c_mass.
SELECTION-SCREEN PUSHBUTTON 21(18) b_mupd USER-COMMAND mupd.
SELECTION-SCREEN PUSHBUTTON 40(18) b_mins USER-COMMAND mins.
SELECTION-SCREEN PUSHBUTTON 59(18) b_mdel USER-COMMAND mdel.
SELECTION-SCREEN END OF LINE.

INITIALIZATION.
  b_setup = 'Setup (load+register)'.
  b_run   = 'Run delta cycle'.
  b_view  = 'Refresh view'.
  c_one   = 'Single flight:'.
  b_upd   = 'Update flight'.
  b_ins   = 'Insert flight'.
  b_del   = 'Delete flight'.
  c_mass  = 'Mass (p_mass rows):'.
  b_mupd  = 'Mass update'.
  b_mins  = 'Mass insert'.
  b_mdel  = 'Mass delete'.
  zcl_erpl_rev_deltadrv=>sflight_default(
    IMPORTING ev_carrid = p_carr ev_connid = p_conn ev_fldate = p_date ).
  APPEND |Press Setup to full-load SFLIGHT into DuckDB and register the snapshot delta.| TO gt_log.

AT SELECTION-SCREEN.
  DATA lv_act TYPE syucomm.
  lv_act = sy-ucomm.
  IF lv_act = 'SETUP' OR lv_act = 'RUN' OR lv_act = 'VIEW'
     OR lv_act = 'UPD' OR lv_act = 'INS' OR lv_act = 'DEL'
     OR lv_act = 'MUPD' OR lv_act = 'MINS' OR lv_act = 'MDEL'.
    PERFORM act USING lv_act.
    IF go_dock IS BOUND.
      go_dock->free( ).
      CLEAR: go_dock, go_split, go_top, go_bottom, go_logbox, go_salv, go_msg.
    ENDIF.
  ENDIF.

*&---------------------------------------------------------------------*
FORM act USING iv_act TYPE syucomm.
  DATA lv_l TYPE string.
  CASE iv_act.
    WHEN 'SETUP'.
      DATA(lr) = zcl_erpl_rev_util=>replicate( iv_tab = 'SFLIGHT' iv_target = c_target ).
      IF lr-error IS NOT INITIAL.
        lv_l = |SETUP failed: { lr-error }|.
      ELSE.
        DATA(lv_re) = zcl_erpl_rev_delta=>register( VALUE #(
          target = c_target method = 'SNAPSHOT' source_from = 'SFLIGHT'
          keys = 'MANDT,CARRID,CONNID,FLDATE' cadence = 'manual' ) ).
        lv_l = |SETUP: loaded { lr-rows_affected } flights into DuckDB '{ c_target }'| &&
               COND string( WHEN lv_re IS INITIAL THEN |, registered SNAPSHOT delta.|
                            ELSE |; register error: { lv_re }| ).
      ENDIF.
    WHEN 'UPD'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_change(
               iv_kind = 'U' iv_carrid = p_carr iv_connid = p_conn iv_fldate = p_date ).
    WHEN 'INS'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_change(
               iv_kind = 'I' iv_carrid = p_carr iv_connid = p_conn iv_fldate = p_date ).
    WHEN 'DEL'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_change(
               iv_kind = 'D' iv_carrid = p_carr iv_connid = p_conn iv_fldate = p_date ).
    WHEN 'MINS'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_mass(
               iv_kind = 'I' iv_carrid = p_carr iv_connid = p_conn iv_count = p_mass ).
    WHEN 'MUPD'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_mass(
               iv_kind = 'U' iv_carrid = p_carr iv_connid = p_conn iv_count = p_mass ).
    WHEN 'MDEL'.
      lv_l = zcl_erpl_rev_deltadrv=>sflight_mass(
               iv_kind = 'D' iv_carrid = p_carr iv_connid = p_conn iv_count = p_mass ).
    WHEN 'RUN'.
      DATA(rs) = zcl_erpl_rev_delta=>run( c_target ).
      IF rs-target IS INITIAL OR rs-error CS 'no delta registration'.
        lv_l = |RUN: target '{ c_target }' is not registered — press Setup first.|.
      ELSEIF rs-skipped = abap_true.
        lv_l = |RUN: skipped (another cycle holds the lease).|.
      ELSEIF rs-error IS NOT INITIAL.
        lv_l = |RUN: delta cycle ERROR: { rs-error }|.
      ELSE.
        SELECT COUNT(*) FROM sflight INTO @DATA(lv_sap).
        DATA(lv_duck) = zcl_erpl_rev_delta=>scalar( |SELECT count(*) AS c FROM { c_target }| ).
        lv_l = |RUN: delta [{ rs-method }] ins={ rs-ins } upd={ rs-upd } del={ rs-del }| &&
               | -> SFLIGHT(SAP)={ lv_sap } DuckDB={ lv_duck }| &&
               COND string( WHEN lv_sap = lv_duck THEN | (in sync).| ELSE | (MISMATCH!).| ).
      ENDIF.
    WHEN 'VIEW'.
      lv_l = |Refreshed.|.
  ENDCASE.
  IF lv_l IS NOT INITIAL. APPEND lv_l TO gt_log. ENDIF.
ENDFORM.

*&---------------------------------------------------------------------*
*&  PBO: docking container — log (top) + DuckDB `sflight` ALV (bottom).
*&---------------------------------------------------------------------*
AT SELECTION-SCREEN OUTPUT.
  IF go_dock IS INITIAL.
    " Read the current DuckDB target into a dynamic table for the ALV.
    CLEAR: gr_result, gv_err.
    DATA(ls_v) = zcl_erpl_rev_util=>query_stream(
      iv_sql = |SELECT carrid, connid, fldate, price, currency, seatsmax, seatsocc |
            && |FROM { c_target } ORDER BY carrid, connid, fldate|
      iv_maxrows = 100000 ).
    IF ls_v-error IS INITIAL AND ls_v-data IS BOUND.
      gr_result = ls_v-data.
    ELSE.
      gv_err = ls_v-error.   " typically "table sflight does not exist" before Setup
    ENDIF.

    go_dock  = NEW #( repid = sy-repid dynnr = sy-dynnr
                      side  = cl_gui_docking_container=>dock_at_bottom ratio = 85 ).
    go_split = NEW #( parent = go_dock rows = 2 columns = 1 ).
    go_split->set_row_height( id = 1 height = 30 ).
    go_top    = go_split->get_container( row = 1 column = 1 ).
    go_bottom = go_split->get_container( row = 2 column = 1 ).

    " Top pane: the demo log (read-only).
    go_logbox = NEW #( parent = go_top wordwrap_mode = cl_gui_textedit=>wordwrap_off ).
    go_logbox->set_readonly_mode( 1 ).
    go_logbox->set_text_as_r3table(
      EXPORTING table = CORRESPONDING gtt_txt( gt_log ) EXCEPTIONS OTHERS = 0 ).

    " Bottom pane: the DuckDB `sflight` target, or a hint if it isn't loaded yet.
    IF gr_result IS BOUND.
      FIELD-SYMBOLS <t> TYPE STANDARD TABLE.
      ASSIGN gr_result->* TO <t>.
      TRY.
          cl_salv_table=>factory( EXPORTING r_container  = go_bottom
                                  IMPORTING r_salv_table = go_salv
                                  CHANGING  t_table      = <t> ).
          DATA(lo_cols) = go_salv->get_columns( ).
          lo_cols->set_optimize( ).
          LOOP AT lo_cols->get( ) INTO DATA(ls_c).
            DATA(lv_h) = CONV string( ls_c-columnname ).
            ls_c-r_column->set_short_text(  CONV scrtext_s( lv_h ) ).
            ls_c-r_column->set_medium_text( CONV scrtext_m( lv_h ) ).
            ls_c-r_column->set_long_text(   CONV scrtext_l( lv_h ) ).
          ENDLOOP.
          go_salv->get_display_settings( )->set_list_header(
            CONV lvc_title( |DuckDB '{ c_target }': { lines( <t> ) } flight(s)| ) ).
          go_salv->get_functions( )->set_all( ).
          go_salv->display( ).
        CATCH cx_salv_msg INTO DATA(lx).
          gv_err = lx->get_text( ).
      ENDTRY.
    ENDIF.

    IF gr_result IS NOT BOUND.
      go_msg = NEW #( parent = go_bottom ).
      go_msg->set_readonly_mode( 1 ).
      go_msg->set_text_as_r3table(
        EXPORTING table = VALUE gtt_txt(
          ( CONV char255( COND string( WHEN gv_err IS NOT INITIAL
              THEN |No DuckDB target yet ({ gv_err }). Press Setup.|
              ELSE |Press Setup to load SFLIGHT into DuckDB.| ) ) ) )
        EXCEPTIONS OTHERS = 0 ).
    ENDIF.
  ENDIF.
