-- Source commit to DuckDB apply, per change. This is the claim the whole
-- recording exists to support, and it is measured rather than asserted.
--
-- Two independent clocks: _commit_ts is when SAP's trigger saw the row change,
-- _applied_at is when this engine wrote it. The gap between them is the answer
-- to "how far behind is the replica".
--
-- The trigger tier prunes its shadow log after each cycle, so every change
-- appears here exactly once and no de-duplication is needed. That is NOT true
-- of the watermark tier, where a safety overlap deliberately re-reads recent
-- rows and each one is logged again per cycle -- so if this demo is ever
-- re-pointed at a watermark target, take min(_applied_at) grouped by key and
-- _commit_ts, the way src/latency.cpp does, or the numbers come out several
-- times too pessimistic.
--
-- The movement type is deliberately not shown: a delete carries only the key,
-- so it would render NULL there and read as a fault rather than as the fact
-- that a deleted row has no image left to report.
--
-- _commit_ts carries whole seconds (that is what the trigger records), so each
-- figure below is +/- 1s. It is a floor on precision, not a margin of error to
-- hide behind.
SELECT _op                                              AS op,
       zeile                                            AS item,
       strftime(_commit_ts,  '%H:%M:%S')                AS sap_changed,
       strftime(_applied_at, '%H:%M:%S')                AS duckdb_applied,
       round(epoch(_applied_at) - epoch(_commit_ts), 2) AS seconds_behind
FROM   _erpl_rev_log_stock_moves
WHERE  mblnr = '4999000001'   -- this document only; the workload has its own shot
ORDER  BY _seq;
