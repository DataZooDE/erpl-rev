*&---------------------------------------------------------------------*
*& Report Z_ERPL_REV_GEN
*&---------------------------------------------------------------------*
*& A change generator, for stress-testing streaming replication.
*&
*& Runs as a background job and commits a realistic mix of inserts,
*& updates and deletes against a real SAP table at a chosen rate, for a
*& chosen duration. Every committed change is recorded in ZDELTA_AUDIT
*& with its key, its operation and the moment it was committed.
*&
*& That audit table is what turns "it seemed to keep up" into evidence:
*&
*&   latency  -- the change log's _applied_at minus the source's change
*&              time, per row, so p50/p95/p99 are computable rather than
*&              estimated.
*&   loss     -- every key the generator committed must be accounted for
*&              in the target. A replicator that silently drops rows
*&              under load looks identical to one that does not, until
*&              somebody counts.
*&   phantoms -- and nothing may appear in the target that the generator
*&              never wrote.
*&
*& Test-only. Never delivered: it writes to business-shaped test tables
*& on purpose.
*&---------------------------------------------------------------------*
REPORT z_erpl_rev_gen.

PARAMETERS: p_tab   TYPE char30 DEFAULT 'ZDELTA_ALL' OBLIGATORY,
            p_rate  TYPE i DEFAULT 10,      " changes per second
            p_dur   TYPE i DEFAULT 60,      " seconds
            p_ins   TYPE i DEFAULT 40,      " mix, percent
            p_upd   TYPE i DEFAULT 40,
            p_del   TYPE i DEFAULT 20,
            p_run   TYPE char20 DEFAULT 'GEN1'.

DATA gv_seq TYPE i.

*---------------------------------------------------------------------*
FORM audit USING iv_key TYPE string iv_op TYPE char1.
  GET TIME STAMP FIELD DATA(lv_ts).
  gv_seq = gv_seq + 1.
  DATA ls TYPE zdelta_audit.
  ls-client       = sy-mandt.
  ls-seqno        = gv_seq.
  ls-runid        = p_run.
  ls-keyval       = iv_key.
  ls-op           = iv_op.
  ls-committed_at = lv_ts.
  INSERT zdelta_audit FROM ls.
ENDFORM.

*---------------------------------------------------------------------*
START-OF-SELECTION.

  " A fresh audit for this run, so a re-run cannot be confused with the
  " previous one's evidence.
  DELETE FROM zdelta_audit WHERE runid = @p_run.
  COMMIT WORK AND WAIT.

  DATA(lv_total) = p_rate * p_dur.
  DATA(lv_done)  = 0.
  DATA(lv_ins)   = 0.
  DATA(lv_upd)   = 0.
  DATA(lv_del)   = 0.
  DATA(lv_batch) = COND i( WHEN p_rate < 1 THEN 1 ELSE p_rate ).

  " A key space SMALLER than the change count, so updates and deletes land on
  " rows that exist. With one key per change every update and delete is a no-op
  " and the workload is really 100% insert -- which is the easy case, and not
  " what anyone is worried about.
  DATA(lv_keyspace) = COND i( WHEN lv_total < 50 THEN 10 ELSE lv_total / 5 ).

  " GET TIME STAMP, not GET RUN TIME: run time does not advance across the WAIT
  " between batches, so the duration check never fired and only the change count
  " ended the run.
  GET TIME STAMP FIELD DATA(lv_t0).

  WHILE lv_done < lv_total.

    DO lv_batch TIMES.
      IF lv_done >= lv_total. EXIT. ENDIF.
      lv_done = lv_done + 1.

      " Deterministic mix rather than random: a stress result nobody can
      " reproduce is an anecdote.
      DATA(lv_pick) = lv_done MOD 100.
      DATA lv_op TYPE char1.
      IF lv_pick < p_ins.
        lv_op = 'I'.
      ELSEIF lv_pick < p_ins + p_upd.
        lv_op = 'U'.
      ELSE.
        lv_op = 'D'.
      ENDIF.

      DATA lv_belnr TYPE char10.
      lv_belnr = |{ lv_done MOD lv_keyspace WIDTH = 10 ALIGN = RIGHT PAD = '0' }|.
      DATA(lv_key) = |1000/{ lv_belnr }/2026/001|.

      GET TIME STAMP FIELD DATA(lv_now_ts).

      CASE lv_op.
        WHEN 'I'.
          DATA ls TYPE zdelta_all.
          CLEAR ls.
          ls-client      = sy-mandt.
          ls-bukrs       = '1000'.
          ls-belnr       = lv_belnr.
          ls-gjahr       = '2026'.
          ls-buzei       = '001'.
          " Every strategy column maintained on every write, so the same row can
          " be replicated by any of them and the results compared.
          ls-chg_tstamp  = lv_now_ts.
          ls-chg_dats    = sy-datum.
          ls-chg_date2   = sy-datum.
          ls-chg_time    = sy-uzeit.
          ls-chg_counter = lv_done.
          ls-bschl       = '40'.
          ls-koart       = 'S'.
          ls-shkzg       = 'S'.
          ls-dmbtr       = lv_done.
          ls-wrbtr       = lv_done.
          ls-waers       = 'EUR'.
          ls-hkont       = '0000400000'.
          ls-kostl       = '0000001000'.
          ls-werks       = '1000'.
          ls-meins       = 'ST'.
          ls-sgtxt       = |generated { lv_done }|.
          ls-budat       = sy-datum.
          ls-bldat       = sy-datum.
          MODIFY zdelta_all FROM ls.
          IF sy-subrc = 0. lv_ins = lv_ins + 1. PERFORM audit USING lv_key 'I'. ENDIF.

        WHEN 'U'.
          UPDATE zdelta_all
             SET chg_tstamp  = @lv_now_ts,
                 chg_dats    = @sy-datum,
                 chg_date2   = @sy-datum,
                 chg_time    = @sy-uzeit,
                 chg_counter = @lv_done,
                 dmbtr       = @lv_done,
                 sgtxt       = 'updated'
           WHERE bukrs = '1000' AND belnr = @lv_belnr
             AND gjahr = '2026' AND buzei = '001'.
          IF sy-subrc = 0. lv_upd = lv_upd + 1. PERFORM audit USING lv_key 'U'. ENDIF.

        WHEN 'D'.
          " A physical delete: the case a watermark can never see, and the whole
          " reason the snapshot and trigger tiers exist.
          DELETE FROM zdelta_all
           WHERE bukrs = '1000' AND belnr = @lv_belnr
             AND gjahr = '2026' AND buzei = '001'.
          IF sy-subrc = 0. lv_del = lv_del + 1. PERFORM audit USING lv_key 'D'. ENDIF.
      ENDCASE.
    ENDDO.

    " Commit the batch, so changes become visible to the replicator at a
    " realistic granularity rather than all at the end.
    COMMIT WORK AND WAIT.

    GET TIME STAMP FIELD DATA(lv_now).
    IF cl_abap_tstmp=>subtract( tstmp1 = lv_now tstmp2 = lv_t0 ) >= p_dur. EXIT. ENDIF.
    WAIT UP TO 1 SECONDS.
  ENDWHILE.

  COMMIT WORK AND WAIT.
  WRITE: / |GEN RESULT pass={ lv_done } fail=0 ins={ lv_ins } upd={ lv_upd } del={ lv_del } | &&
           |keyspace={ lv_keyspace }|.
