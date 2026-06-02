# Z_ERPL_REV_SQL — DuckDB SQL console

`Z_ERPL_REV_SQL` is an interactive SAP GUI report: a multi-line `CL_GUI_TEXTEDIT`
where you type DuckDB SQL, an **Execute** button, and a `CL_SALV_TABLE` grid that
shows the last statement's result set. The ABAP logic delegates everything to
`zcl_erpl_rev_util` / the C++ RFC server; this report is just the GUI shell.

## No Screen Painter / dynpro needed — fully headless deploy

Earlier this required a hand-built dynpro 0100 (ADT/`erpl-adt` cannot create
classic dynpros). It no longer does: the UI is a **`CL_GUI_DOCKING_CONTAINER`** +
**`CL_GUI_SPLITTER_CONTAINER`** hosted on the **standard selection screen** and
built in **`AT SELECTION-SCREEN OUTPUT`**. That whole construction is plain report
source, so the program is **created and activated entirely via `erpl-adt`** (it's
part of `scripts/deploy-abap.sh`). Only the *rendering* needs a SAP GUI — you run
it with SA38 → F8.

Recipe details + the three gotchas (build controls in `AT SELECTION-SCREEN
OUTPUT` not `START-OF-SELECTION`; the selection screen needs ≥1 input field or it
auto-skips; guard creation with `IF go_dock IS INITIAL`) are captured in the
memory note *“SAP GUI embedded controls without Screen Painter”*.

## Layout

- **Top pane** — `CL_GUI_TEXTEDIT`, editable, your SQL script (`;`-separated).
- **Execute button** — selection-screen pushbutton (`USER-COMMAND exec`); runs in
  `AT SELECTION-SCREEN` without leaving the screen.
- **Bottom pane** — `CL_SALV_TABLE` over the dynamic result table
  (`zcl_erpl_rev_util=>result_to_alv`), or a read-only TextEdit showing the error
  / “no result set” / row-count info.

On Execute the panes are torn down and rebuilt in PBO with the fresh result — this
side-steps having to mutate the ALV's column structure in place when the query
shape changes.

## Run it

Deploy (headless): `scripts/deploy-abap.sh` creates + activates `Z_ERPL_REV_SQL`
(among everything else). Then in the SAP GUI:

`SA38` → `Z_ERPL_REV_SQL` → **F8**. Prereqs (same as the rest of erpl-rev):

- the `erpl_rev_server` is running and registered (`make run`), and
- destination `ERPL_REV` + the FMs exist (`scripts/deploy-abap.sh`).

Type a script in the top pane and press **Execute**, e.g.:

```sql
SET threads TO 4;
SELECT payment_type, count(*) AS trips,
       CAST(sum(fare_amount) AS DECIMAL(10,2)) AS total
FROM read_parquet('/path/to/taxi.parquet')
GROUP BY 1 ORDER BY 1;
```

Only the **last** statement's result is shown in the grid; earlier statements
(INSTALL/LOAD/CREATE/INSERT) just run. Errors appear as read-only text in the
bottom pane.

## What's verified without the GUI

The console's data path — `zcl_erpl_rev_util=>query` → `Z_DUCKDB_QUERY` →
multi-statement DuckDB → `result_to_alv` — is covered by headless tests
(`zcl_erpl_rev_utiltest`, the Catch2 multi-statement test), and the report
**activates clean** via erpl-adt (compile proof). Only the on-screen rendering
(textedit + ALV) is exercised manually by running it in the SAP GUI.
