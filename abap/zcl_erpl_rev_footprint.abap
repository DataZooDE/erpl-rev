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

    " Classes and programs actually present in the delivered package.
    SELECT obj_name FROM tadir
      INTO TABLE @DATA(lt_actual)
      WHERE devclass = 'ZERPL_CORE'
        AND object IN ( 'CLAS', 'PROG', 'INTF', 'TABL' )
        AND delflag = @abap_false
      ORDER BY obj_name.

    DATA(lt_exp) = expected( ).

    " Anything present that is not expected: the footprint grew.
    LOOP AT lt_actual INTO DATA(ls_a).
      DATA(lv_name) = CONV string( ls_a-obj_name ).
      ok( cond   = xsdbool( line_exists( lt_exp[ table_line = lv_name ] ) )
          what   = 'no unexpected object in the delivered package'
          detail = |{ lv_name } is in ZERPL_CORE but not in the expected list| ).
    ENDLOOP.

    " Anything expected that is missing: the install is incomplete, which would
    " otherwise surface much later as a dump in an unrelated place.
    LOOP AT lt_exp INTO DATA(lv_e).
      ok( cond   = xsdbool( line_exists( lt_actual[ obj_name = lv_e ] ) )
          what   = 'every delivered object is present'
          detail = |{ lv_e } is expected but missing| ).
    ENDLOOP.

    " And nothing at all outside the Z namespace.
    LOOP AT lt_actual INTO ls_a.
      ok( cond   = xsdbool( ls_a-obj_name CP 'Z*' )
          what   = 'nothing outside the customer namespace'
          detail = CONV string( ls_a-obj_name ) ).
    ENDLOOP.

    out->write( |FOOTPRINT RESULT pass={ mv_pass } fail={ mv_fail }| ).
  ENDMETHOD.

ENDCLASS.
