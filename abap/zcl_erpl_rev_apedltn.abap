" DELTA-milestone driver, chunk N: AC-4 negatives (m1 + m2 + m9). See
" ZCL_ERPL_REV_APETEST for the budget note; N runs alone like V.
CLASS zcl_erpl_rev_apedltn DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_rev_apedltn IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    TRY.
        NEW zcl_erpl_rev_apetest( )->run_ac4( out ).
      CATCH cx_root INTO DATA(lx).
        out->write( |DUMP: { lx->get_text( ) }| ).
    ENDTRY.
  ENDMETHOD.
ENDCLASS.
