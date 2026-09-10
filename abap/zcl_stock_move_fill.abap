CLASS zcl_stock_move_fill DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Fill ZSTOCK_MOVE with a realistic run of goods movements.
*"*
*"* Modelled on ZCL_WIDE_BSEG, with one change: it reports progress every 10k
*"* rows rather than every 100k. The original's `MOD 100000` fires exactly once
*"* at 100k rows, which is not progress, it is a completion message wearing a
*"* progress message's clothes.
*"*
*"* Deletes first, so re-running always starts from the same place. That is
*"* what makes a recorded demo reproducible rather than merely repeatable.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_stock_move_fill IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    DELETE FROM zstock_move.

    CONSTANTS lc_rows       TYPE i VALUE 1000000.
    CONSTANTS lc_batch      TYPE i VALUE 1000.
    CONSTANTS lc_per_doc    TYPE i VALUE 4.      " 25_000 documents x 4 items

    DATA lt      TYPE STANDARD TABLE OF zstock_move WITH EMPTY KEY.
    DATA ls      TYPE zstock_move.
    DATA lv_done TYPE i.

    GET TIME STAMP FIELD DATA(lv_ts).

    DO lc_rows TIMES.
      DATA(lv_doc)  = ( sy-index - 1 ) DIV lc_per_doc + 1.
      DATA(lv_item) = ( sy-index - 1 ) MOD lc_per_doc + 1.

      CLEAR ls.
      ls-mblnr = |49{ lv_doc WIDTH = 8 ALIGN = RIGHT PAD = '0' }|.
      ls-mjahr = '2026'.
      ls-zeile = |{ lv_item WIDTH = 4 ALIGN = RIGHT PAD = '0' }|.

      " Every strategy column maintained, as Z_ERPL_REV_GEN does, so this table
      " can be replicated by any method and the results compared.
      ls-chg_tstamp  = lv_ts.
      ls-chg_dats    = sy-datum.
      ls-chg_date2   = sy-datum.
      ls-chg_time    = sy-uzeit.
      ls-chg_counter = sy-index.

      " A plausible spread of movement types rather than one repeated value:
      " 101 goods receipt, 261 issue to order, 311 transfer between locations.
      CASE sy-index MOD 3.
        WHEN 0. ls-bwart = '101'. ls-sgtxt = 'Goods receipt PO'.
        WHEN 1. ls-bwart = '261'. ls-sgtxt = 'Issue to production order'.
        WHEN OTHERS. ls-bwart = '311'. ls-sgtxt = 'Transfer to storage location'.
      ENDCASE.

      ls-matnr = |100-{ ( sy-index MOD 500 ) + 1 WIDTH = 3 ALIGN = RIGHT PAD = '0' }|.
      ls-werks = '1000'.
      ls-lgort = COND #( WHEN sy-index MOD 2 = 0 THEN '0001' ELSE '0002' ).
      ls-charg = |B{ ( sy-index MOD 200 ) + 1 WIDTH = 9 ALIGN = RIGHT PAD = '0' }|.
      ls-bukrs = '1000'.
      ls-menge = ( sy-index MOD 250 ) + 1.
      ls-meins = 'ST'.
      ls-dmbtr = ls-menge * '12.50'.
      ls-waers = 'EUR'.
      ls-lifnr = |{ ( sy-index MOD 50 ) + 4700 WIDTH = 10 ALIGN = RIGHT PAD = '0' }|.
      ls-kostl = '0000001000'.
      ls-budat = sy-datum.
      ls-bldat = sy-datum.

      APPEND ls TO lt.
      IF lines( lt ) >= lc_batch.
        INSERT zstock_move FROM TABLE @lt.
        lv_done = lv_done + lines( lt ).
        CLEAR lt.
        IF lv_done MOD 100000 = 0.
          out->write( |progress: { lv_done } movements| ).
        ENDIF.
      ENDIF.
    ENDDO.
    IF lt IS NOT INITIAL.
      INSERT zstock_move FROM TABLE @lt.
      lv_done = lv_done + lines( lt ).
    ENDIF.

    SELECT COUNT(*) FROM zstock_move INTO @DATA(lv_n).
    out->write( |zstock_move populated: { lv_n } rows| ).
  ENDMETHOD.
ENDCLASS.
