# Z_ERPL_REV_SQL — the DuckDB SQL console in SAP GUI

Type DuckDB SQL in SAP GUI and get a result grid back. Useful when the person who
wants the answer is in SAP and not at a terminal — and the same queries are available
headless with `erpl-rev sql`.

## Run it

```
SA38  →  Z_ERPL_REV_SQL  →  F8
```

(`SA38` is "run a program by name"; see the [glossary](glossary.md).)

You need what the rest of erpl-rev needs and nothing more: the server running and
registered, and the `ERPL_REV` destination plus its function modules deployed — both
of which `erpl-rev setup` does. If the console reports it cannot reach the server,
`erpl-rev doctor` says which half is missing.

Type one or more `;`-separated statements in the top pane and press **Execute**. The
last statement's result set fills the grid below; an error, or a statement with no
result set, shows as text instead.

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

## How it is built, and why that matters

There is no Screen Painter dynpro behind this, which is the only reason it can be
deployed headlessly: ADT cannot create classic dynpros. The UI is a
`CL_GUI_DOCKING_CONTAINER` plus a `CL_GUI_SPLITTER_CONTAINER` hosted on the standard
selection screen and built in `AT SELECTION-SCREEN OUTPUT` — all of which is plain
report source, so the program is created and activated over ADT like everything else.
Only the *rendering* needs a SAP GUI.

Three things that will bite anyone editing it: build the controls in
`AT SELECTION-SCREEN OUTPUT` rather than `START-OF-SELECTION`; the selection screen
needs at least one input field or it auto-skips; and guard creation with
`IF go_dock IS INITIAL`.

On Execute the panes are torn down and rebuilt with the fresh result, which side-steps
mutating the ALV's column structure in place when the query shape changes.
