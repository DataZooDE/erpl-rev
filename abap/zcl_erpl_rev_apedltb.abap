" DELTA-milestone driver, chunk B: surgery + carry + restore (m4b). Run
" after APEDLTA; see ZCL_ERPL_REV_APETEST for the budget note.
CLASS zcl_erpl_rev_apedltb DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_rev_apedltb IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    TRY.
        NEW zcl_erpl_rev_apetest( )->run_dltb( out ).
      CATCH cx_root INTO DATA(lx).
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
