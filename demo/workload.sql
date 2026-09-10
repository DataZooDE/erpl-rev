-- What the closing thirty seconds cost, per operation.
--
-- The six single changes earlier in the recording are followed one at a time,
-- which proves the mechanism and says nothing about sustained traffic. This is
-- the same measurement over roughly seven hundred changes: two independent
-- clocks, _commit_ts from SAP's trigger and _applied_at from this engine, with
-- the spread rather than one flattering figure.
--
-- Restricted to the 4998* documents the workload posted, so the earlier beats
-- do not dilute it. _commit_ts carries whole seconds, so every figure here is
-- +/- 1s -- a floor on precision, not a margin to hide behind.
SELECT _op                                                    AS op,
       count(*)                                               AS changes,
       round(min(epoch(_applied_at) - epoch(_commit_ts)), 2)  AS fastest_s,
       round(median(epoch(_applied_at) - epoch(_commit_ts)),2) AS median_s,
       round(max(epoch(_applied_at) - epoch(_commit_ts)), 2)  AS slowest_s
FROM   _erpl_rev_log_stock_moves
WHERE  mblnr LIKE '4998%'
GROUP  BY _op
ORDER  BY _op;
