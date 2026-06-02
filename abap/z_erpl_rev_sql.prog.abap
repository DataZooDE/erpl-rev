*&---------------------------------------------------------------------*
*& Report  Z_ERPL_REV_SQL
*&---------------------------------------------------------------------*
*& erpl-rev: DuckDB SQL console. Type one or more ';'-separated
*& statements (e.g. INSTALL httpfs; LOAD httpfs; SELECT ...) into the
*& TextEdit pane, press the Execute button, and the LAST result set is
*& shown in the ALV pane below. All work is delegated to
*& zcl_erpl_rev_util / the C++ RFC server; this report is the GUI shell.
*&
*& NO Screen Painter / dynpro needed: the UI is a CL_GUI_DOCKING_CONTAINER
*& + CL_GUI_SPLITTER_CONTAINER hosted on the standard selection screen and
*& built in AT SELECTION-SCREEN OUTPUT — so it is created/activated entirely
*& via erpl-adt. (Controls only RENDER in the SAP GUI; run via SA38 -> F8.)
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_sql.

TYPES: gtt_txt TYPE STANDARD TABLE OF char255 WITH EMPTY KEY.

DATA: go_dock   TYPE REF TO cl_gui_docking_container,
      go_split  TYPE REF TO cl_gui_splitter_container,
      go_top    TYPE REF TO cl_gui_container,
      go_bottom TYPE REF TO cl_gui_container,
      go_editor TYPE REF TO cl_gui_textedit,
      go_salv   TYPE REF TO cl_salv_table,
      go_msg    TYPE REF TO cl_gui_textedit,
      gr_result TYPE REF TO data,        " dynamic result table (kept alive)
      gt_sql    TYPE gtt_txt,            " current editor content
      gv_err    TYPE string,
      gv_total  TYPE i,                  " rows fetched into the grid
      gv_truncated TYPE abap_bool,       " capped before the result was exhausted
      gv_info   TYPE string.

" An input-enabled field is required, else the selection screen is auto-skipped
" and AT SELECTION-SCREEN OUTPUT never fires (nothing renders).
PARAMETERS p_run AS CHECKBOX DEFAULT 'X'.
SELECTION-SCREEN PUSHBUTTON /1(30) b_exec USER-COMMAND exec.

INITIALIZATION.
  b_exec = 'Execute DuckDB SQL'.
  gt_sql = VALUE #( ( |SELECT 42 AS answer, 'hello duckdb' AS msg| ) ).

*&---------------------------------------------------------------------*
*&  PAI: on Execute, read the editor, run the script, stash the result,
*&  then drop the container so PBO rebuilds the panes with fresh data
*&  (rebuilding side-steps changing the ALV's column structure in place).
*&---------------------------------------------------------------------*
AT SELECTION-SCREEN.
  IF sy-ucomm = 'EXEC'.
    IF go_editor IS BOUND.
      " get_text_as_r3table declares CLASSIC exceptions; if none are mapped, a
      " raised one (notably POTENTIAL_DATA_LOSS) becomes an uncaught short dump.
      " POTENTIAL_DATA_LOSS still returns the text (it only warns that very long
      " lines could be truncated into the char255 table), so we ignore it and
      " only treat real transport errors as failures.
      go_editor->get_text_as_r3table(
        IMPORTING table               = gt_sql
        EXCEPTIONS potential_data_loss = 0
                   error_dp            = 1
                   error_dp_create     = 2
                   OTHERS              = 3 ).
      IF sy-subrc <> 0.
        MESSAGE |Could not read SQL editor (rc={ sy-subrc })| TYPE 'I'.
        RETURN.
      ENDIF.
    ENDIF.
    DATA(lv_sql) = concat_lines_of( table = gt_sql sep = | | ).

    " Stream the result via the cursor FMs (fixed memory, binary sXML pages).
    " Cap what the grid holds so a multi-million-row SELECT can't blow up the
    " front end; the server stops pulling once the cap is reached.
    CONSTANTS c_display_cap TYPE i VALUE 100000.
    DATA(ls_res) = zcl_erpl_rev_util=>query_stream(
                     iv_sql = lv_sql iv_maxrows = c_display_cap ).
    CLEAR: gr_result, gv_err, gv_info, gv_total.
    IF ls_res-error IS NOT INITIAL.
      gv_err = ls_res-error.
    ELSE.
      gr_result   = ls_res-data.
      gv_total    = ls_res-row_count.
      gv_truncated = ls_res-truncated.
      gv_info     = |{ ls_res-row_count } row(s)|.
      IF gr_result IS NOT BOUND.
        gv_info = |statement ok, no result set ({ gv_info })|.
      ENDIF.
    ENDIF.

    IF go_dock IS BOUND.
      go_dock->free( ).
      CLEAR: go_dock, go_split, go_top, go_bottom, go_editor, go_salv.
    ENDIF.
  ENDIF.

*&---------------------------------------------------------------------*
*&  PBO: build the docking container with a TextEdit (top) and either the
*&  ALV result or an error/info TextEdit (bottom).
*&---------------------------------------------------------------------*
AT SELECTION-SCREEN OUTPUT.
  IF go_dock IS INITIAL.
    go_dock  = NEW #( repid = sy-repid dynnr = sy-dynnr
                      side  = cl_gui_docking_container=>dock_at_bottom
                      ratio = 88 ).
    go_split = NEW #( parent = go_dock rows = 2 columns = 1 ).
    go_split->set_row_height( id = 1 height = 35 ).
    go_top    = go_split->get_container( row = 1 column = 1 ).
    go_bottom = go_split->get_container( row = 2 column = 1 ).

    " Top pane: editable SQL editor, seeded with the current script.
    go_editor = NEW #( parent = go_top
                       wordwrap_mode = cl_gui_textedit=>wordwrap_off ).
    go_editor->set_text_as_r3table( EXPORTING table = gt_sql
                                    EXCEPTIONS OTHERS = 0 ).

    " Bottom pane: result grid, or the error / info message as read-only text.
    IF gr_result IS BOUND.
      FIELD-SYMBOLS <t> TYPE STANDARD TABLE.
      ASSIGN gr_result->* TO <t>.
      TRY.
          cl_salv_table=>factory( EXPORTING r_container  = go_bottom
                                  IMPORTING r_salv_table = go_salv
                                  CHANGING  t_table      = <t> ).
          " Dynamic (RTTS) columns have no DDIC text, so SALV shows blank
          " headers. Set each column's heading from its own name (= the DuckDB
          " column name). scrtext_s/m/l cap at 10/20/40 chars; set all three so
          " SALV always has a label to show whatever the column width allows.
          DATA(lo_cols) = go_salv->get_columns( ).
          lo_cols->set_optimize( ).
          LOOP AT lo_cols->get( ) INTO DATA(ls_colref).
            DATA(lv_h) = CONV string( ls_colref-columnname ).
            ls_colref-r_column->set_short_text(  CONV scrtext_s( lv_h ) ).
            ls_colref-r_column->set_medium_text( CONV scrtext_m( lv_h ) ).
            ls_colref-r_column->set_long_text(   CONV scrtext_l( lv_h ) ).
          ENDLOOP.
          " Title flags a capped (streamed-and-truncated) result.
          FIELD-SYMBOLS <rt> TYPE STANDARD TABLE.
          ASSIGN gr_result->* TO <rt>.
          DATA(lv_shown) = lines( <rt> ).
          DATA(lv_title) = COND lvc_title(
            WHEN gv_truncated = abap_true
            THEN |DuckDB: first { lv_shown } rows (more available)|
            ELSE |DuckDB: { lv_shown } row(s)| ).
          go_salv->get_display_settings( )->set_list_header( lv_title ).
          go_salv->get_functions( )->set_all( ).
          go_salv->display( ).
        CATCH cx_salv_msg INTO DATA(lx).
          gv_err = lx->get_text( ).
      ENDTRY.
    ENDIF.

    IF gr_result IS NOT BOUND.
      DATA(lv_msg) = COND string( WHEN gv_err IS NOT INITIAL
                                  THEN |ERROR: { gv_err }|
                                  ELSE COND string( WHEN gv_info IS NOT INITIAL
                                                    THEN gv_info
                                                    ELSE |Enter SQL above and press Execute.| ) ).
      go_msg = NEW #( parent = go_bottom ).
      go_msg->set_readonly_mode( 1 ).
      go_msg->set_text_as_r3table( EXPORTING table = VALUE gtt_txt( ( CONV char255( lv_msg ) ) )
                                   EXCEPTIONS OTHERS = 0 ).
    ENDIF.
  ENDIF.
