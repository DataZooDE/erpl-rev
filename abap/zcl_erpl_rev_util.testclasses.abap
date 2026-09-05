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
CLASS zcl_erpl_rev_util DEFINITION LOCAL FRIENDS ltcl_pipe.

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
