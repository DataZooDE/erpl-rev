CLASS zcl_erpl_rev_deltadrv DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    " Change-injection driver for delta E2E + the interactive simulator. It makes
    " REAL, committed SAP changes so a delta cycle has something to pick up:
    "   * ZDELTA_WM  - direct Open SQL insert/update/delete on a test table whose
    "                  CHANGED_AT is a numeric (DEC15 YYYYMMDDHHMMSS) watermark.
    "   * material   - BAPI_MATERIAL_SAVEDATA (the MM02 path) writes a genuine
    "                  change document (CDHDR OBJECTCLAS='MATERIAL' + CDPOS), which
    "                  the CHANGEDOC / INSERT_ONLY readers consume.
    " Shared by zcl_erpl_rev_deltatest (automated proof) and Z_ERPL_REV_DELTA_SIM
    " (run it by hand in SAP GUI to demo to others).

    "! Current UTC time as a high-resolution TIMESTAMPL (DEC21,7) watermark value —
    "! sub-second so a seed and a same-second change still compare strictly greater.
    CLASS-METHODS now_ts RETURNING VALUE(rv) TYPE timestampl.

    "! (Re)seed ZDELTA_WM with iv_rows rows (id = 0000000001..). All get the
    "! current timestamp. Returns the high-water just written (max changed_at).
    CLASS-METHODS seed_wm
      IMPORTING iv_rows  TYPE i DEFAULT 10
      RETURNING VALUE(rv) TYPE timestampl.

    "! Update one ZDELTA_WM row (bumps VAL + NAME + CHANGED_AT = now).
    CLASS-METHODS touch_wm IMPORTING iv_id TYPE csequence.
    "! Insert one new ZDELTA_WM row (CHANGED_AT = now).
    CLASS-METHODS insert_wm IMPORTING iv_id TYPE csequence.
    "! Physically delete one ZDELTA_WM row.
    CLASS-METHODS delete_wm IMPORTING iv_id TYPE csequence.

    "! Change a material description via BAPI_MATERIAL_SAVEDATA + COMMIT (real MM02,
    "! writes a CDHDR/CDPOS change document under OBJECTCLAS='MATERIAL'). Picks an
    "! existing material when iv_matnr is blank. Returns the material + new text.
    CLASS-METHODS change_material
      IMPORTING iv_matnr TYPE matnr OPTIONAL
      EXPORTING ev_matnr TYPE matnr
                ev_maktx TYPE string
                ev_error TYPE string.

  PRIVATE SECTION.
    CLASS-METHODS id10 IMPORTING iv TYPE i RETURNING VALUE(rv) TYPE c LENGTH 10.
ENDCLASS.

CLASS zcl_erpl_rev_deltadrv IMPLEMENTATION.

  METHOD now_ts.
    GET TIME STAMP FIELD rv.
  ENDMETHOD.

  METHOD id10.
    DATA lv TYPE n LENGTH 10.
    lv = iv.
    rv = lv.
  ENDMETHOD.

  METHOD seed_wm.
    DELETE FROM zdelta_wm.                                  "#EC CI_NOFIRST
    DATA lt TYPE STANDARD TABLE OF zdelta_wm.
    DATA(lv_ts) = now_ts( ).
    DO iv_rows TIMES.
      APPEND VALUE zdelta_wm( id = id10( sy-index )
                             name = |row { sy-index }|
                             val = sy-index
                             changed_at = lv_ts ) TO lt.
    ENDDO.
    INSERT zdelta_wm FROM TABLE @lt.
    COMMIT WORK AND WAIT.
    rv = lv_ts.
  ENDMETHOD.

  METHOD touch_wm.
    DATA(lv_ts) = now_ts( ).
    DATA lv_id TYPE zdelta_wm-id.
    lv_id = iv_id.
    UPDATE zdelta_wm
      SET name = @( |touched { lv_ts }| ),
          val = val + 1,
          changed_at = @lv_ts
      WHERE id = @lv_id.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD insert_wm.
    DATA ls TYPE zdelta_wm.
    ls-id = iv_id.
    ls-name = |inserted|.
    ls-val = 1.
    ls-changed_at = now_ts( ).
    INSERT zdelta_wm FROM @ls.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD delete_wm.
    DATA lv_id TYPE zdelta_wm-id.
    lv_id = iv_id.
    DELETE FROM zdelta_wm WHERE id = @lv_id.
    COMMIT WORK AND WAIT.
  ENDMETHOD.

  METHOD change_material.
    " Pick an existing, non-deletion-flagged material if none was given.
    DATA lv_matnr TYPE matnr.
    lv_matnr = iv_matnr.
    IF lv_matnr IS INITIAL.
      SELECT matnr FROM mara UP TO 1 ROWS
        WHERE lvorm = @space
        ORDER BY matnr
        INTO @lv_matnr.
      ENDSELECT.
    ENDIF.
    IF lv_matnr IS INITIAL.
      ev_error = 'no material found in MARA'.
      RETURN.
    ENDIF.

    DATA(lv_ts)   = now_ts( ).
    DATA(lv_text) = |erpl delta { lv_ts }|.

    DATA ls_head TYPE bapimathead.
    ls_head-material = lv_matnr.
    DATA lt_desc TYPE STANDARD TABLE OF bapi_makt.
    APPEND VALUE bapi_makt( langu = sy-langu matl_desc = lv_text ) TO lt_desc.
    DATA lt_ret TYPE STANDARD TABLE OF bapiret2.

    CALL FUNCTION 'BAPI_MATERIAL_SAVEDATA'
      EXPORTING  headdata       = ls_head
      IMPORTING  return         = DATA(ls_ret)
      TABLES     materialdescription = lt_desc
                 returnmessages = lt_ret.

    LOOP AT lt_ret INTO DATA(ls_r) WHERE type CA 'EAX'.
      ev_error = |{ ls_r-type } { ls_r-id }{ ls_r-number } { ls_r-message }|.
    ENDLOOP.
    IF ev_error IS INITIAL AND ls_ret-type CA 'EAX'.
      ev_error = |{ ls_ret-type } { ls_ret-message }|.
    ENDIF.

    IF ev_error IS INITIAL.
      CALL FUNCTION 'BAPI_TRANSACTION_COMMIT' EXPORTING wait = abap_true.
      ev_matnr = lv_matnr.
      ev_maktx = lv_text.
    ELSE.
      CALL FUNCTION 'BAPI_TRANSACTION_ROLLBACK'.
    ENDIF.
  ENDMETHOD.

ENDCLASS.
