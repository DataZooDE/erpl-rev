CLASS zcl_erpl_rev_footprint DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* The footprint inventory.
*"*
*"* erpl-rev's central claim is that adding a replication source adds a row in
*"* DuckDB, not an object in SAP. That is only true while it stays true, and it
*"* is the kind of property that erodes one convenient exception at a time -- so
*"* it is asserted at the end of every end-to-end run, not reviewed occasionally.
*"*
*"* The expected list is checked in. A new object is a deliberate edit here, with
*"* a reviewer, rather than something that arrives unnoticed.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    DATA: mv_pass TYPE i, mv_fail TYPE i, mo TYPE REF TO if_oo_adt_classrun_out.
    METHODS ok IMPORTING cond TYPE abap_bool what TYPE string detail TYPE string DEFAULT ''.
    METHODS expected RETURNING VALUE(rt) TYPE string_table.
ENDCLASS.

CLASS zcl_erpl_rev_footprint IMPLEMENTATION.

  METHOD ok.
    IF cond = abap_true. mv_pass = mv_pass + 1.
    ELSE. mv_fail = mv_fail + 1. mo->write( |FAIL { what }: { detail }| ). ENDIF.
  ENDMETHOD.

  METHOD expected.
    " Everything erpl-rev delivers into a customer system. Test drivers, demo
    " reports and fixtures are NOT here -- they belong to a dev checkout.
    rt = VALUE #(
      ( `ZIF_ERPL_REV_PROGRESS` )
      ( `ZCL_ERPL_REV_TYPEMAP` )
      ( `ZCL_ERPL_REV_UTIL` )
      ( `ZCL_ERPL_REV_DELTA` )
      ( `ZCL_ERPL_REV_CDC` )
      ( `ZCL_ERPL_REV_MKFM` )
      ( `ZCL_ERPL_REV_SETUP` )
      ( `ZCL_ERPL_REV_DIAG` )
      ( `ZCL_ERPL_REV_CLIDRV` )
      ( `Z_ERPL_REV_REPL_WORKER` )
      ( `Z_ERPL_REV_REPLICATE` )
      ( `Z_ERPL_REV_SQL` )
      ( `Z_ERPL_REV_DELTA` )
      ( `Z_ERPL_REV_DAEMON` ) ).
  ENDMETHOD.

  METHOD if_oo_adt_classrun~main.
    mo = out.

    " Looked up by NAME, not by package. A delivered install has these in
    " ZERPL_CORE; a dev checkout puts them in $TMP. The invariant being tested is
    " "everything erpl-rev delivers is present", and that holds in both layouts --
    " tying it to one package would make the check pass or fail for a reason that
    " has nothing to do with the footprint.
    DATA(lt_exp) = expected( ).
    LOOP AT lt_exp INTO DATA(lv_e).
      DATA lv_obj TYPE tadir-obj_name.
      lv_obj = lv_e.
      SELECT SINGLE obj_name FROM tadir INTO @DATA(lv_found)
        WHERE obj_name = @lv_obj
          AND object IN ( 'CLAS', 'PROG', 'INTF', 'TABL' )
          AND delflag = @abap_false.
      ok( cond   = xsdbool( sy-subrc = 0 )
          what   = 'every delivered object is present'
          detail = |{ lv_e } is expected but missing| ).
    ENDLOOP.

    " Nothing erpl-rev owns may sit outside the customer namespace. The other
    " half of the invariant -- that nothing UNEXPECTED ships -- is asserted
    " against the embedded asset list in the C++ suite, because that list is what
    " actually reaches a customer; a dev system legitimately carries test classes
    " that are not delivered, so it cannot be judged here.
    LOOP AT lt_exp INTO lv_e.
      ok( cond   = xsdbool( lv_e CP 'Z*' )
          what   = 'nothing outside the customer namespace'
          detail = lv_e ).
    ENDLOOP.

    out->write( |FOOTPRINT RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
