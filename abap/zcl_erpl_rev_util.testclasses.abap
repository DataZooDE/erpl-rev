*"* Unit tests for zcl_erpl_rev_util.
*"*
*"* These live in the class's testclasses section, not in a ZCL_*TEST class of
*"* their own: a separate class per unit would be a new SAP object each time and
*"* the footprint inventory counts objects. They are never embedded into the
*"* binary (cmake/embed_abap.cmake ships main sections only), so a customer
*"* install carries no test code.
*"*
*"* Run: erpl-adt test ZCL_ERPL_REV_UTIL

CLASS ltcl_pipe DEFINITION DEFERRED.
CLASS ltcl_keys DEFINITION DEFERRED.
CLASS zcl_erpl_rev_util DEFINITION LOCAL FRIENDS ltcl_pipe ltcl_keys.

CLASS ltcl_pipe DEFINITION FINAL FOR TESTING
  DURATION SHORT
  RISK LEVEL HARMLESS.

  PRIVATE SECTION.
    METHODS drain_timeout_is_an_error FOR TESTING.
    METHODS drain_clean_reports_no_error FOR TESTING.
ENDCLASS.

CLASS ltcl_pipe IMPLEMENTATION.

  METHOD drain_timeout_is_an_error.
    " A WAIT that expires returns normally. Before the post-wait check, a drain
    " that gave up looked exactly like one that finished, and replicate went on
    " to report a short row count as success -- which becomes a lost row the
    " moment a cycle stages and the server merges the stage without the package
    " still in flight.
    DATA(lo) = NEW zcl_erpl_rev_util( ).
    lo->mv_wait    = 1.     " seconds; the production budget is 3600
    lo->mv_pending = 1.     " one package that will never complete

    lo->pipe_drain( ).

    cl_abap_unit_assert=>assert_not_initial(
      act = lo->mv_err
      msg = 'a drain that timed out must report an error, not return quietly' ).
    cl_abap_unit_assert=>assert_char_cp(
      act = lo->mv_err
      exp = '*in flight*'
      msg = 'the error should say how many packages were still outstanding' ).
  ENDMETHOD.

  METHOD drain_clean_reports_no_error.
    " Nothing pending: the drain must stay silent, or every clean load fails.
    DATA(lo) = NEW zcl_erpl_rev_util( ).
    lo->mv_wait    = 1.
    lo->mv_pending = 0.

    lo->pipe_drain( ).

    cl_abap_unit_assert=>assert_initial(
      act = lo->mv_err
      msg = 'a drain with nothing in flight must not invent an error' ).
  ENDMETHOD.

ENDCLASS.


CLASS ltcl_keys DEFINITION FINAL FOR TESTING
  DURATION SHORT
  RISK LEVEL HARMLESS.

  PRIVATE SECTION.
    METHODS single_key_is_a_plain_in_list FOR TESTING.
    METHODS composite_key_uses_tuples FOR TESTING.
    METHODS numeric_keys_are_unquoted FOR TESTING.
    METHODS quotes_in_a_value_are_escaped FOR TESTING.
    METHODS empty_key_set_selects_nothing FOR TESTING.
    METHODS a_large_key_set_is_chunked FOR TESTING.
ENDCLASS.

CLASS ltcl_keys IMPLEMENTATION.

  METHOD single_key_is_a_plain_in_list.
    " The change-document re-read has always produced this shape; it must keep
    " producing it, because that path is covered by the existing delta e2e.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `MATNR` ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = VALUE #( ( VALUE #( ( `4711` ) ) ) ( VALUE #( ( `4712` ) ) ) ) ).
    cl_abap_unit_assert=>assert_equals(
      act = lv exp = |MATNR IN ( '4711','4712' )| ).
  ENDMETHOD.

  METHOD composite_key_uses_tuples.
    " What the single-key form could not express at all, and what the trigger
    " cycle needs: SFLIGHT is keyed on four columns.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `MANDT` ) ( `CARRID` ) )
      it_key_types = VALUE #( ( `CLNT` ) ( `CHAR` ) )
      it_rows      = VALUE #( ( VALUE #( ( `001` ) ( `LH` ) ) )
                              ( VALUE #( ( `001` ) ( `AA` ) ) ) ) ).
    cl_abap_unit_assert=>assert_equals(
      act = lv exp = |( MANDT,CARRID ) IN ( ( '001','LH' ),( '001','AA' ) )| ).
  ENDMETHOD.

  METHOD numeric_keys_are_unquoted.
    " A quoted numeric compares as text, which silently matches nothing.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `BELNR` ) )
      it_key_types = VALUE #( ( `INT4` ) )
      it_rows      = VALUE #( ( VALUE #( ( `17` ) ) ) ) ).
    cl_abap_unit_assert=>assert_equals( act = lv exp = |BELNR IN ( 17 )| ).
  ENDMETHOD.

  METHOD quotes_in_a_value_are_escaped.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `NAME` ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = VALUE #( ( VALUE #( ( `O'BRIEN` ) ) ) ) ).
    cl_abap_unit_assert=>assert_equals( act = lv exp = |NAME IN ( 'O''BRIEN' )| ).
  ENDMETHOD.

  METHOD empty_key_set_selects_nothing.
    " The dangerous default. An empty predicate means "no filter", so a cycle
    " with nothing to re-read would re-read the entire source table.
    DATA lt_none TYPE zcl_erpl_rev_util=>tt_keyrows.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `MATNR` ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = lt_none ).
    cl_abap_unit_assert=>assert_equals( act = lv exp = |1 = 0| ).
  ENDMETHOD.

  METHOD a_large_key_set_is_chunked.
    " One IN list of thousands of values is refused by the parser; the chunks are
    " OR-ed so every key is still selected.
    DATA lt_rows TYPE zcl_erpl_rev_util=>tt_keyrows.
    DO 7 TIMES.
      APPEND VALUE #( ( |K{ sy-index }| ) ) TO lt_rows.
    ENDDO.
    DATA(lv) = zcl_erpl_rev_util=>key_in_predicate(
      it_key_cols  = VALUE #( ( `MATNR` ) )
      it_key_types = VALUE #( ( `CHAR` ) )
      it_rows      = lt_rows
      iv_chunk     = 3 ).
    cl_abap_unit_assert=>assert_char_cp( act = lv exp = '*OR*' ).
    " All seven still present.
    DO 7 TIMES.
      cl_abap_unit_assert=>assert_char_cp( act = lv exp = |*'K{ sy-index }'*| ).
    ENDDO.
  ENDMETHOD.

ENDCLASS.
