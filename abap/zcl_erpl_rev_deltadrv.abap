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

    "! Make a REAL, committed change to the SFLIGHT demo table (the SNAPSHOT delta
    "! demo). iv_kind: 'U' bumps PRICE+100 / SEATSOCC+1 of one flight; 'I' clones a
    "! flight of (carrid,connid) at iv_fldate; 'D' deletes one flight. Returns a
    "! human-readable line for the demo log (prefixed 'ERROR:' on failure).
    CLASS-METHODS sflight_change
      IMPORTING iv_kind   TYPE c
                iv_carrid TYPE s_carr_id
                iv_connid TYPE s_conn_id
                iv_fldate TYPE s_date
      RETURNING VALUE(rv_msg) TYPE string.

    "! First SFLIGHT flight in key order — sensible defaults for the demo screen.
    CLASS-METHODS sflight_default
      EXPORTING ev_carrid TYPE s_carr_id
                ev_connid TYPE s_conn_id
                ev_fldate TYPE s_date.

ENDCLASS.

CLASS zcl_erpl_rev_deltadrv IMPLEMENTATION.

  METHOD now_ts.
    GET TIME STAMP FIELD rv.
  ENDMETHOD.

  METHOD seed_wm.
    DELETE FROM zdelta_wm.                                  "#EC CI_NOFIRST
    DATA lt TYPE STANDARD TABLE OF zdelta_wm.
    DATA(lv_ts) = now_ts( ).
    DATA lv_id TYPE n LENGTH 10.
    DO iv_rows TIMES.
      lv_id = sy-index.
      APPEND VALUE zdelta_wm( id = lv_id
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
    " BAPI_MATERIAL_SAVEDATA (the MM02 path) requires Materials Management, which
    " is NOT installed on the bare ABAP Platform developer trial (this system is
    " Basis + the SFLIGHT demo, not the S/4 fully-activated appliance). On an
    " MM-equipped system this method would issue the BAPI + BAPI_TRANSACTION_COMMIT
    " to write a genuine CDHDR/CDPOS change document under OBJECTCLAS='MATERIAL'.
    " The caller treats a non-empty ev_error as "skip the change-doc scenario".
    ev_error = 'BAPI_MATERIAL_SAVEDATA unavailable (no Materials Management on this system)'.
  ENDMETHOD.

  METHOD sflight_default.
    SELECT carrid, connid, fldate FROM sflight
      ORDER BY carrid, connid, fldate
      INTO @DATA(ls) UP TO 1 ROWS.
    ENDSELECT.
    ev_carrid = ls-carrid.
    ev_connid = ls-connid.
    ev_fldate = ls-fldate.
  ENDMETHOD.

  METHOD sflight_change.
    DATA ls TYPE sflight.
    CASE iv_kind.
      WHEN 'U'.
        SELECT SINGLE * FROM sflight
          WHERE carrid = @iv_carrid AND connid = @iv_connid AND fldate = @iv_fldate
          INTO @ls.
        IF sy-subrc <> 0.
          rv_msg = |ERROR: flight { iv_carrid } { iv_connid } { iv_fldate } not found|.
          RETURN.
        ENDIF.
        DATA(lv_old) = ls-price.
        ls-price    = ls-price + 100.
        ls-seatsocc = ls-seatsocc + 1.
        UPDATE sflight FROM @ls.
        COMMIT WORK AND WAIT.
        rv_msg = |UPDATE { iv_carrid } { iv_connid } { iv_fldate }: price { lv_old } -> { ls-price }|.
      WHEN 'I'.
        SELECT SINGLE * FROM sflight
          WHERE carrid = @iv_carrid AND connid = @iv_connid
          INTO @ls.
        IF sy-subrc <> 0.
          rv_msg = |ERROR: no template flight for { iv_carrid } { iv_connid }|.
          RETURN.
        ENDIF.
        ls-fldate   = iv_fldate.
        ls-price    = ls-price + 100.
        ls-seatsocc = 0.
        INSERT sflight FROM @ls.
        IF sy-subrc <> 0.
          rv_msg = |ERROR: insert { iv_carrid } { iv_connid } { iv_fldate } failed (already exists?)|.
          RETURN.
        ENDIF.
        COMMIT WORK AND WAIT.
        rv_msg = |INSERT { iv_carrid } { iv_connid } { iv_fldate } (price { ls-price })|.
      WHEN 'D'.
        DELETE FROM sflight
          WHERE carrid = @iv_carrid AND connid = @iv_connid AND fldate = @iv_fldate.
        IF sy-subrc <> 0.
          rv_msg = |ERROR: delete { iv_carrid } { iv_connid } { iv_fldate } matched nothing|.
          RETURN.
        ENDIF.
        COMMIT WORK AND WAIT.
        rv_msg = |DELETE { iv_carrid } { iv_connid } { iv_fldate }|.
      WHEN OTHERS.
        rv_msg = |ERROR: unknown change kind { iv_kind }|.
    ENDCASE.
  ENDMETHOD.

ENDCLASS.
