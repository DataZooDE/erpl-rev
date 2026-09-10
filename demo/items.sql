-- The demo document, as DuckDB holds it.
--
-- In a file rather than typed inline because vhs's tape parser cannot nest
-- quotes, and this query needs both: shell quoting around it and SQL quoting
-- around the document number.
SELECT zeile AS item,
       bwart AS mvt_type,
       lgort AS storage_loc,
       menge AS qty,
       meins AS uom
FROM   stock_moves
WHERE  mblnr = '4999000001'
ORDER  BY zeile;
