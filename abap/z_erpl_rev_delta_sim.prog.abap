*&---------------------------------------------------------------------*
*& Report  Z_ERPL_REV_DELTA_SIM
*&---------------------------------------------------------------------*
*& Interactive delta SIMULATOR — run it by hand in SAP GUI to inject a real,
*& committed SAP change and watch one delta cycle carry it into the DuckDB target.
*& Handy for demos and for testing the pipeline without the E2E harness.
*&
*& Pick a scenario:
*&   r_seed - (re)seed ZDELTA_WM with p_n rows                (target delta_wm)
*&   r_upd  - update one ZDELTA_WM row  (id p_id)             (target delta_wm)
*&   r_ins  - insert one ZDELTA_WM row  (id p_id)             (target delta_wm)
*&   r_del  - delete one ZDELTA_WM row  (id p_id) [SNAPSHOT]  (target delta_snap)
*&   r_mat  - change a material description via BAPI (MM02)   (target makt_cd)
*& It registers the matching delta target if needed, injects the change, runs one
*& cycle, and reports the applied counts + before/after target row counts.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_delta_sim.

PARAMETERS:
  r_seed RADIOBUTTON GROUP s DEFAULT 'X',
  r_upd  RADIOBUTTON GROUP s,
  r_ins  RADIOBUTTON GROUP s,
  r_del  RADIOBUTTON GROUP s,
  r_mat  RADIOBUTTON GROUP s,
  p_n    TYPE i DEFAULT 10,                    " seed row count
  p_id   TYPE c LENGTH 10 DEFAULT '0000000003'." id for upd/ins/del
  .

START-OF-SELECTION.
  DATA lv_target TYPE string.

  IF r_mat = abap_true.
    " CHANGEDOC target re-reads MAKT by MATNR (objectclas MATERIAL).
    lv_target = 'makt_cd'.
    " Ensure a seed full-load + registration exist for the demo.
    zcl_erpl_rev_util=>replicate( iv_tab = 'MAKT' iv_target = lv_target iv_maxrows = 200 ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = lv_target method = 'CHANGEDOC' source_from = 'MAKT'
      keys = 'MANDT,MATNR,SPRAS' wm_kind = 'DATETIME'
      cadence = 'manual' extra = '{"objectclas":"MATERIAL"}' ) ).
    DATA lv_matnr TYPE matnr.
    DATA lv_maktx TYPE string.
    DATA lv_merr  TYPE string.
    zcl_erpl_rev_deltadrv=>change_material(
      IMPORTING ev_matnr = lv_matnr ev_maktx = lv_maktx ev_error = lv_merr ).
    IF lv_merr IS NOT INITIAL.
      WRITE: / 'material change failed:', lv_merr. RETURN.
    ENDIF.
    WRITE: / 'changed material', lv_matnr, '->', lv_maktx.
  ELSEIF r_del = abap_true.
    " Physical delete only shows up through the SNAPSHOT method.
    lv_target = 'delta_snap'.
    zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = lv_target ).
    zcl_erpl_rev_delta=>register( VALUE #(
      target = lv_target method = 'SNAPSHOT' source_from = 'ZDELTA_WM'
      keys = 'MANDT,ID' cadence = 'manual' ) ).
    zcl_erpl_rev_deltadrv=>delete_wm( p_id ).
    WRITE: / 'deleted ZDELTA_WM id', p_id.
  ELSE.
    " Watermark scenarios on ZDELTA_WM.
    lv_target = 'delta_wm'.
    zcl_erpl_rev_delta=>register( VALUE #(
      target = lv_target method = 'WATERMARK' source_from = 'ZDELTA_WM'
      keys = 'MANDT,ID' chg_col = 'CHANGED_AT' wm_kind = 'NUMTS'
      safety_secs = 0 cadence = 'manual' ) ).
    IF r_seed = abap_true.
      zcl_erpl_rev_deltadrv=>seed_wm( p_n ).
      " A fresh seed is a new full population — reset the watermark + reload.
      zcl_erpl_rev_util=>replicate( iv_tab = 'ZDELTA_WM' iv_target = lv_target ).
      WRITE: / 'seeded ZDELTA_WM with', p_n, 'rows (full reload of', lv_target, ')'.
    ELSEIF r_ins = abap_true.
      zcl_erpl_rev_deltadrv=>insert_wm( p_id ).
      WRITE: / 'inserted ZDELTA_WM id', p_id.
    ELSE.
      zcl_erpl_rev_deltadrv=>touch_wm( p_id ).
      WRITE: / 'updated ZDELTA_WM id', p_id.
    ENDIF.
  ENDIF.

  DATA(lv_before) = zcl_erpl_rev_delta=>scalar( |SELECT count(*) AS c FROM { lv_target }| ).
  DATA(ls_run)    = zcl_erpl_rev_delta=>run( lv_target ).
  DATA(lv_after)  = zcl_erpl_rev_delta=>scalar( |SELECT count(*) AS c FROM { lv_target }| ).

  ULINE.
  IF ls_run-error IS NOT INITIAL.
    WRITE: / 'delta cycle ERROR:', ls_run-error.
  ELSE.
    WRITE: / 'delta cycle:', ls_run-method,
             '| rows', ls_run-rows, 'ins', ls_run-ins, 'upd', ls_run-upd, 'del', ls_run-del.
    WRITE: / 'target', lv_target, '| rows before', lv_before, '-> after', lv_after.
  ENDIF.
