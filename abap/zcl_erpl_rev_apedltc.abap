" DELTA-milestone driver, chunk C: spill replay (m5) + stale graph (m6) +
" drop command (m8). Run after APEDLTB; see ZCL_ERPL_REV_APETEST for the
" budget note.
CLASS zcl_erpl_rev_apedltc DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_rev_apedltc IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    TRY.
        NEW zcl_erpl_rev_apetest( )->run_dltc( out ).
      CATCH cx_root INTO DATA(lx).
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
