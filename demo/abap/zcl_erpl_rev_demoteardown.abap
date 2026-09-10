CLASS zcl_erpl_rev_demoteardown DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Undo everything the demo put on the system.
*"*
*"* The triggers matter most: ZCDC_* object names derive from the SOURCE table,
*"* and ZSTOCK_MOVE carries a million generated movements. A leftover trigger
*"* set pointing at a log table that no longer exists makes every INSERT on that
*"* table dump, which surfaces as an unrelated suite failing for no visible
*"* reason. $TMP only.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_erpl_rev_demoteardown IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    DATA(lv_te) = zcl_erpl_rev_cdc=>teardown( 'stock_moves' ).
    zcl_erpl_rev_util=>query( |DELETE FROM _erpl_rev_cdc WHERE target='stock_moves'| ).
    zcl_erpl_rev_util=>query( |DELETE FROM _erpl_rev_delta_state WHERE target='stock_moves'| ).
    zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS stock_moves| ).
    zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS _erpl_rev_log_stock_moves| ).
    DELETE FROM zstock_move WHERE mblnr = '4999000001'.
        " And whatever the closing workload posted. Left behind, they make the
        " opening count read 1,000,700 instead of 1,000,000 and the first beat
        " of the recording becomes a lie about a number that is on screen.
        DELETE FROM zstock_move WHERE mblnr LIKE '4998%'.
    COMMIT WORK AND WAIT.
    out->write( |DEMOTEARDOWN OK teardown={ COND string( WHEN lv_te IS INITIAL THEN 'clean' ELSE lv_te ) }| ).
  ENDMETHOD.
ENDCLASS.
