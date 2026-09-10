CLASS zcl_erpl_rev_demosetup DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Puts the demo system into the exact state the tape expects, and says so in
*"* one line the setup script can check.
*"*
*"* It lives in ABAP rather than in setup.sh because provisioning the trigger
*"* tier has no CLI verb -- `cdc status` and `cdc repair` exist, `cdc provision`
*"* does not -- and because registering, seeding and provisioning have to happen
*"* in that order against one system. Splitting them across two languages would
*"* buy nothing and add a way for them to disagree.
*"*
*"* Idempotent by construction: it tears down before it builds, so running it
*"* twice leaves the same state as running it once. That is what makes a
*"* re-record identical rather than merely similar.
*"*
*"* $TMP only. Never delivered.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_target TYPE string VALUE 'stock_moves'.
    CONSTANTS c_source TYPE string VALUE 'ZSTOCK_MOVE'.
    CONSTANTS c_keys   TYPE string VALUE 'CLIENT,MBLNR,MJAHR,ZEILE'.
ENDCLASS.

CLASS zcl_erpl_rev_demosetup IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    TRY.
        " --- 1. the source keeps its history ------------------------------
        " Only the DEMO document is cleared. The million generated movements
        " stay: an initial sync of an empty table demonstrates nothing, and the
        " whole point of this version is that the target starts from real
        " volume rather than from nothing.
        DELETE FROM zstock_move WHERE mblnr = '4999000001'.
        " And whatever the closing workload posted. Left behind, they make the
        " opening count read 1,000,700 instead of 1,000,000 and the first beat
        " of the recording becomes a lie about a number that is on screen.
        DELETE FROM zstock_move WHERE mblnr LIKE '4998%'.
        COMMIT WORK AND WAIT.

        " --- 2. tear down whatever a previous take left ------------------
        " Triggers first: the ZCDC_* object names derive from the SOURCE, so a
        " leftover trigger set writing into a log table that no longer exists
        " makes every INSERT on ZDELTA_ALL dump.
        zcl_erpl_rev_cdc=>teardown( c_target ).
        zcl_erpl_rev_util=>query( |DELETE FROM _erpl_rev_cdc WHERE target='{ c_target }'| ).
        zcl_erpl_rev_util=>query( |DELETE FROM _erpl_rev_delta_state WHERE target='{ c_target }'| ).
        zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS { c_target }| ).
        zcl_erpl_rev_util=>query( |DROP TABLE IF EXISTS _erpl_rev_log_{ c_target }| ).
        zcl_erpl_rev_util=>query( |DROP SEQUENCE IF EXISTS _erpl_rev_log_{ c_target }_seq| ).

        " --- 3. create the target EMPTY, and leave it that way -----------
        " A one-row load, then emptied: it creates the DuckDB table with the
        " right column types so the opening query returns 0 rather than an
        " error -- an error is not the same story as an empty table -- while
        " leaving the full sync for the recording to perform on camera. That
        " sync is the performance half of the demo; doing it here would throw
        " it away.
        DATA(ls_r) = zcl_erpl_rev_util=>replicate(
                       iv_tab = c_source iv_target = c_target
                       iv_where = |MBLNR = '4999000000'| ).
        IF ls_r-error IS NOT INITIAL.
          out->write( |DEMOSETUP ERROR seed: { ls_r-error }| ).
          RETURN.
        ENDIF.
        zcl_erpl_rev_util=>query( |DELETE FROM { c_target }| ).

        " --- 4. register it as a TRIGGER target --------------------------
        " method=CDC is what makes the daemon plan it. The tick planner
        " dispatches on a single method per target, so a target is either a
        " watermark target or a trigger target -- it cannot be driven as both.
        " A trigger target is due the moment a row appears in the shadow
        " table rather than on a clock, which is why the cadence here is a
        " ceiling on latency rather than a polling interval.
        DATA(lv_err) = zcl_erpl_rev_delta=>register( VALUE #(
          target      = c_target
          method      = 'CDC'
          source_from = c_source
          keys        = c_keys
          cadence     = 'micro:2'
          log_enabled = 'true' ) ).
        IF lv_err IS NOT INITIAL.
          out->write( |DEMOSETUP ERROR register: { lv_err }| ).
          RETURN.
        ENDIF.

        " --- 5. provision the triggers ----------------------------------
        " KEYS_IUD: the trigger writes the KEY of a changed row and the cycle
        " re-reads the values. That keeps the write a customer's transaction
        " pays for narrow -- 14x cheaper on the write path than logging a full
        " row image, measured in docs/perf-results.md -- and it captures
        " inserts, updates AND physical deletes, which is the whole reason
        " this demo is on the trigger tier.
        DATA(lv_pe) = zcl_erpl_rev_cdc=>provision(
          iv_target = c_target iv_source = c_source
          iv_keys = c_keys iv_mode = 'KEYS_IUD' ).
        IF lv_pe IS NOT INITIAL.
          out->write( |DEMOSETUP ERROR provision: { lv_pe }| ).
          RETURN.
        ENDIF.

        " --- 6. the clock, reported so the caller can refuse a bad one ---
        " The engine reads a source timestamp AT TIME ZONE 'UTC' with no skew
        " correction. If this box and the recording box disagree, every
        " latency in the closing shot is off by exactly that offset and looks
        " entirely plausible. So setup.sh compares this against its own clock
        " and refuses to record.
        GET TIME STAMP FIELD DATA(lv_ts).
        out->write( |DEMOSETUP OK target={ c_target } mode=KEYS_IUD | &&
                    |cadence=micro:2 sap_utc={ lv_ts }| ).
      CATCH cx_root INTO DATA(lx).
        out->write( |DEMOSETUP ERROR { lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
