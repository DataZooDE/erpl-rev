" DELTA-milestone driver, chunk A: converge (m4a). See ZCL_ERPL_REV_APETEST
" for the budget note; run the APEDLTA/B/C drivers in order A, B, C.
CLASS zcl_erpl_rev_apedlta DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_rev_apedlta IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    TRY.
        NEW zcl_erpl_rev_apetest( )->run_dlta( out ).
      CATCH cx_root INTO DATA(lx).
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
