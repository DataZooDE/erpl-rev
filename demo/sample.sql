-- Ten of the million, so the count above is not the only evidence.
--
-- A row count proves a number. It does not prove that anything readable
-- arrived: a target full of NULLs, or of one row repeated, counts the same.
-- These are real material document line items with their movement types,
-- quantities, units and amounts.
--
-- Spread evenly through the table by row number rather than sampled at random
-- or taken from the top. The top ten rows are one document and prove nothing
-- about the other 999,996; a random draw makes every take differ, and a demo
-- that differs run to run cannot be checked against its own recording. A fixed
-- stride is the same ten rows every time, from one end of the table to the
-- other.
--
-- The stride is PRIME on purpose. Every hundred-thousandth row looked like the
-- fixture was degenerate -- ten identical lines, same material, same quantity,
-- same amount -- because the generator cycles its materials every 500 rows,
-- its quantities every 250 and its items every 4, and 100,000 is a multiple of
-- all of them. The sample was aliasing onto one point of every cycle. 99,991
-- shares no factor with any of them, so what comes back is what is actually in
-- the table.
SELECT mblnr AS document,
       zeile AS item,
       bwart AS mvt,
       matnr AS material,
       werks AS plant,
       lgort AS sloc,
       menge AS qty,
       meins AS uom,
       dmbtr AS amount,
       waers AS curr
FROM   (SELECT *, row_number() OVER (ORDER BY mblnr, zeile) AS rn
        FROM   stock_moves)
WHERE  rn % 99991 = 0
ORDER  BY rn
LIMIT  10;
