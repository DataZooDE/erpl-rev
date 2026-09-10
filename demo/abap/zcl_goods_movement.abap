CLASS zcl_goods_movement DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* One material document, three states. The demo's only lever.
*"*
*"* A classrun takes no arguments, so the state is read from the DOCUMENT
*"* itself: post it, transfer it, archive it. Purging the table is the reset,
*"* and the lever can never drift out of step with what is on screen.
*"*
*"* $TMP only. Never delivered, never in the E-FOOTPRINT package.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_mblnr TYPE zstock_move-mblnr VALUE '4999000001'.
    CONSTANTS c_mjahr TYPE zstock_move-mjahr VALUE '2026'.
    METHODS item IMPORTING iv_zeile TYPE zstock_move-zeile
                           iv_matnr TYPE zstock_move-matnr
                           iv_menge TYPE zstock_move-menge
                           iv_ts    TYPE timestampl
                 RETURNING VALUE(rs) TYPE zstock_move.
ENDCLASS.

CLASS zcl_goods_movement IMPLEMENTATION.

  METHOD if_oo_adt_classrun~main.

    DATA lv_ts TYPE timestampl.        " NOT inferred: DATA(x) gives whole seconds
    GET TIME STAMP FIELD lv_ts.        " UTC, which is how the engine reads it back

    SELECT COUNT(*) FROM zstock_move   " which state are we in? ask the document
      WHERE mblnr = @c_mblnr AND mjahr = @c_mjahr
      INTO @DATA(lv_items).

* ---------------------------------------------------------------- 1. POST
    IF lv_items = 0.                   " nothing there yet -> a goods receipt
      DATA lt TYPE STANDARD TABLE OF zstock_move.
      APPEND item( iv_zeile = '0001' iv_matnr = '100-100'
                   iv_menge = 120 iv_ts = lv_ts ) TO lt.
      APPEND item( iv_zeile = '0002' iv_matnr = '100-200'
                   iv_menge = 60  iv_ts = lv_ts ) TO lt.
      APPEND item( iv_zeile = '0003' iv_matnr = '100-300'
                   iv_menge = 25  iv_ts = lv_ts ) TO lt.

      INSERT zstock_move FROM TABLE @lt.   " an ordinary insert. no API, no exit
      COMMIT WORK AND WAIT.                " AND WAIT: durable before we return

      out->write( |Material document { c_mblnr }/{ c_mjahr } posted: | &&
                  |101 goods receipt, 3 items, plant 1000 / storage 0001| ).
      RETURN.
    ENDIF.

* ------------------------------------------------------------ 2. TRANSFER
    SELECT COUNT(*) FROM zstock_move       " still sitting in storage loc 0001?
      WHERE mblnr = @c_mblnr AND mjahr = @c_mjahr AND lgort = '0001'
      INTO @DATA(lv_in_0001).

    IF lv_in_0001 > 1.
      UPDATE zstock_move                   " movement type 311: a transfer
         SET lgort = '0002', bwart = '311',
             sgtxt = 'Transfer to storage location 0002',
             chg_tstamp = @lv_ts, chg_dats = @sy-datum,
             chg_date2 = @sy-datum, chg_time = @sy-uzeit,
             chg_counter = chg_counter + 1
       WHERE mblnr = @c_mblnr AND mjahr = @c_mjahr
         AND zeile IN ('0001', '0002').    " both in ONE unit of work
      COMMIT WORK AND WAIT.

      out->write( |Material document { c_mblnr }: items 0001 and 0002 | &&
                  |transferred 0001 -> 0002 (311)| ).
      RETURN.
    ENDIF.

* ------------------------------------------------------------- 3. ARCHIVE
    SELECT COUNT(*) FROM zstock_move
      WHERE mblnr = @c_mblnr AND mjahr = @c_mjahr AND zeile = '0003'
      INTO @DATA(lv_0003).

    IF lv_0003 > 0.
      DELETE FROM zstock_move              " PHYSICAL. no flag, no tombstone --
       WHERE mblnr = @c_mblnr              " the case a watermark cannot see,
         AND mjahr = @c_mjahr              " and the reason this runs on
         AND zeile = '0003'.               " the trigger tier
      COMMIT WORK AND WAIT.

      out->write( |Archiving run removed { c_mblnr } item 0003 | &&
                  |-- physically gone from SAP, no tombstone| ).
      RETURN.
    ENDIF.

    out->write( |Nothing left to do for { c_mblnr } -- run demo/setup.sh to reset| ).
  ENDMETHOD.

  METHOD item.
    rs-client = sy-mandt.  rs-mblnr = c_mblnr.  rs-mjahr = c_mjahr.
    rs-zeile  = iv_zeile.  rs-matnr = iv_matnr. rs-menge = iv_menge.
    " Every change column maintained, so any replication method could drive this
    " table. The trigger tier watches none of them -- it fires on the write.
    rs-chg_tstamp = iv_ts.    rs-chg_dats  = sy-datum.
    rs-chg_date2  = sy-datum. rs-chg_time  = sy-uzeit.
    rs-chg_counter = 1.
    rs-bwart = '101'.         rs-werks = '1000'.   rs-lgort = '0001'.
    rs-charg = 'B000000042'.  rs-bukrs = '1000'.   rs-meins = 'ST'.
    rs-dmbtr = iv_menge * '12.50'.                 rs-waers = 'EUR'.
    rs-lifnr = '0000004711'.  rs-kostl = '0000001000'.
    rs-budat = sy-datum.      rs-bldat = sy-datum.
    rs-sgtxt = 'Goods receipt PO 4500001234'.
  ENDMETHOD.

ENDCLASS.
