CLASS zcl_stock_workload DEFINITION PUBLIC FINAL CREATE PUBLIC.
*"* Thirty seconds of an ordinary working day on ZSTOCK_MOVE.
*"*
*"* The rest of the demo shows six changes, one at a time, so each can be
*"* followed. That proves the mechanism and says nothing about whether it holds
*"* up under continuous traffic. This runs for thirty seconds at a few dozen
*"* rows a second -- a mid-size site's goods-movement volume, not a benchmark --
*"* with all three operations mixed, the way a real table is actually written.
*"*
*"* Each second: a new material document is POSTED, a document from two seconds
*"* ago is CORRECTED, and one from six seconds ago is ARCHIVED. Documents are
*"* numbered from the tick, so every operation refers to a document this run
*"* created and the whole thing is self-contained and repeatable -- it never
*"* touches the million rows the initial sync loaded.
*"*
*"* NOT concurrent sessions. One session posting continuously, which is what the
*"* table and the replica see; it is not a test of SAP's own lock behaviour
*"* under N users. Say so rather than let the graph imply otherwise.
*"*
*"* $TMP only. Never delivered, never in the E-FOOTPRINT package.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
  PRIVATE SECTION.
    CONSTANTS c_seconds TYPE i      VALUE 30.
    CONSTANTS c_prefix  TYPE string VALUE '4998'.
    CONSTANTS c_mjahr   TYPE zstock_move-mjahr VALUE '2026'.
    " Two seconds back for a correction, six for an archiving run. Both are
    " longer than the target's two-second cadence, so the insert and the change
    " that follows it land in DIFFERENT cycles and are visible as two events
    " rather than collapsing into one net write.
    CONSTANTS c_correct_lag TYPE i VALUE 2.
    CONSTANTS c_archive_lag TYPE i VALUE 6.

    METHODS doc_no IMPORTING iv_tick     TYPE i
                   RETURNING VALUE(rv)   TYPE zstock_move-mblnr.
    METHODS items  IMPORTING iv_tick     TYPE i
                   RETURNING VALUE(rv)   TYPE i.
    METHODS post   IMPORTING iv_tick     TYPE i
                   RETURNING VALUE(rv)   TYPE i.
ENDCLASS.

CLASS zcl_stock_workload IMPLEMENTATION.

  METHOD if_oo_adt_classrun~main.

    DATA lv_start TYPE timestamp.
    DATA lv_now   TYPE timestamp.
    DATA lv_ins   TYPE i.
    DATA lv_upd   TYPE i.
    DATA lv_del   TYPE i.
    DATA lv_tick  TYPE i VALUE 0.

    " Idempotent, because INSERT dumps on a duplicate key and the document
    " numbers are derived from the tick: a second run without a setup in
    " between would re-post 4998000001 and short-dump on camera. Normally this
    " deletes nothing -- demo/setup.sh has already purged them.
    DELETE FROM zstock_move WHERE mblnr LIKE '4998%'.
    COMMIT WORK AND WAIT.

    GET TIME STAMP FIELD lv_start.

    DO.
      " The clock decides when to stop, not a tick count: each tick does real
      " database work of unpredictable duration, so counting ticks would make
      " the run anywhere from thirty to fifty seconds and the recording's
      " timings would drift with it.
      GET TIME STAMP FIELD lv_now.
      IF cl_abap_tstmp=>subtract( tstmp1 = lv_now tstmp2 = lv_start ) >= c_seconds.
        EXIT.
      ENDIF.
      lv_tick = lv_tick + 1.

* --------------------------------------------------------------- post
      lv_ins = lv_ins + post( lv_tick ).

* ------------------------------------------------------------ correct
      " A posting corrected after the fact: the storage location was wrong.
      " Ordinary, frequent, and invisible to a row count -- which is exactly
      " why the monitor reads updates from the cycle's own report.
      IF lv_tick > c_correct_lag.
        DATA(lv_cdoc) = doc_no( lv_tick - c_correct_lag ).
        DATA(lv_cupto) = CONV zstock_move-zeile( 6 + ( lv_tick MOD 5 ) ).
        GET TIME STAMP FIELD DATA(lv_cts).
        UPDATE zstock_move
           SET lgort       = '0002',
               bwart       = '311',
               sgtxt       = 'Correction: storage location',
               chg_tstamp  = @lv_cts,
               chg_dats    = @sy-datum,
               chg_date2   = @sy-datum,
               chg_time    = @sy-uzeit,
               chg_counter = chg_counter + 1
         WHERE mblnr = @lv_cdoc AND mjahr = @c_mjahr AND zeile <= @lv_cupto.
        lv_upd = lv_upd + sy-dbcnt.
      ENDIF.

* ------------------------------------------------------------ archive
      " A physical delete, which is the case a watermark cannot see at all.
      IF lv_tick > c_archive_lag.
        DATA(lv_ddoc)  = doc_no( lv_tick - c_archive_lag ).
        DATA(lv_dupto) = CONV zstock_move-zeile( 3 + ( lv_tick MOD 3 ) ).
        DELETE FROM zstock_move
         WHERE mblnr = @lv_ddoc AND mjahr = @c_mjahr AND zeile <= @lv_dupto.
        lv_del = lv_del + sy-dbcnt.
      ENDIF.

      " One unit of work per second, as a posting run would commit it. The
      " trigger fires on the write, so this is also what paces the change log.
      COMMIT WORK AND WAIT.

      " Paced against the wall clock, not by sleeping a second per tick. Tick N
      " is due at start + N seconds; if the work already took longer than that
      " the wait is skipped and the run catches up, instead of a slow moment
      " compounding into a run that is nothing like thirty seconds of steady
      " traffic. One early run managed five ticks in thirty seconds this way,
      " and the cause -- contention on a trial system that had just served a
      " million-row read -- was never established. This makes it not matter.
      GET TIME STAMP FIELD lv_now.
      IF cl_abap_tstmp=>subtract( tstmp1 = lv_now tstmp2 = lv_start ) < lv_tick.
        WAIT UP TO 1 SECONDS.
      ENDIF.
    ENDDO.

    " sy-dbcnt throughout, not the numbers this class intended to write: a
    " report of what a run meant to do is not a report of what it did.
    out->write( |Workload finished: { lv_tick } seconds, | &&
                |{ lv_ins } inserts, { lv_upd } updates, { lv_del } deletes | &&
                |({ ( lv_ins + lv_upd + lv_del ) / lv_tick } rows/s)| ).
  ENDMETHOD.

  METHOD post.
    DATA lt TYPE STANDARD TABLE OF zstock_move.
    DATA ls TYPE zstock_move.

    GET TIME STAMP FIELD DATA(lv_ts).
    DATA(lv_items) = items( iv_tick ).

    DO lv_items TIMES.
      CLEAR ls.
      ls-client = sy-mandt.
      ls-mblnr  = doc_no( iv_tick ).
      ls-mjahr  = c_mjahr.
      ls-zeile  = sy-index.
      " A spread of materials, plants and movement types, so the rows are not
      " N copies of one row with a different key. Derived from the counters
      " rather than random: a demo that differs run to run cannot be checked
      " against its own recording.
      CASE sy-index MOD 5.
        WHEN 0. ls-matnr = '100-100'. WHEN 1. ls-matnr = '100-200'.
        WHEN 2. ls-matnr = '100-300'. WHEN 3. ls-matnr = '100-400'.
        WHEN OTHERS. ls-matnr = '100-500'.
      ENDCASE.
      CASE ( iv_tick + sy-index ) MOD 3.
        WHEN 0.      ls-bwart = '101'.   " goods receipt
        WHEN 1.      ls-bwart = '261'.   " issue to order
        WHEN OTHERS. ls-bwart = '311'.   " transfer
      ENDCASE.
      ls-werks = COND #( WHEN iv_tick MOD 2 = 0 THEN '1000' ELSE '2000' ).
      ls-lgort = '0001'.
      ls-charg = |B{ iv_tick WIDTH = 9 PAD = '0' ALIGN = RIGHT }|.
      ls-bukrs = '1000'.
      ls-menge = 10 + ( sy-index MOD 40 ).
      ls-meins = 'ST'.
      ls-dmbtr = ls-menge * '12.50'.
      ls-waers = 'EUR'.
      ls-lifnr = '0000004711'.
      ls-kostl = '0000001000'.
      ls-budat = sy-datum.
      ls-bldat = sy-datum.
      ls-sgtxt = 'Goods movement, daily posting run'.
      ls-chg_tstamp  = lv_ts.
      ls-chg_dats    = sy-datum.
      ls-chg_date2   = sy-datum.
      ls-chg_time    = sy-uzeit.
      ls-chg_counter = 1.
      APPEND ls TO lt.
    ENDDO.

    INSERT zstock_move FROM TABLE @lt.
    rv = sy-dbcnt.
  ENDMETHOD.

  METHOD items.
    " Eight to fourteen line items, varying with the tick. A constant document
    " size draws a flat bar, which reads as a generator rather than as work.
    rv = 8 + ( iv_tick MOD 7 ).
  ENDMETHOD.

  METHOD doc_no.
    " 4998xxxxxx, kept clear of the 4999000001 the single-change beats use, so
    " demo/items.sql and the setup purge can tell the two apart.
    rv = |{ c_prefix }{ iv_tick WIDTH = 6 PAD = '0' ALIGN = RIGHT }|.
  ENDMETHOD.

ENDCLASS.
