# erpl-rev: a million goods movements, then live ones.
#
#   bash demo/setup.sh && vhs demo/realtime.tape
#
# Left pane  : SAP -- the ABAP that posts a movement, then the movements.
# Right pane : `erpl-rev top` with the throughput graph, started once and
#              never touched again.
#
# Nothing here is staged. Every line is a real command against a real SAP
# system, and the closing query measures the gap between SAP's clock and this
# engine's rather than asserting anything about it.
#
# Only commands that complete on their own appear on camera. Every other CLI
# verb is queued into _erpl_rev_cli_cmd and finishes when the ABAP driver
# drains it, which would hang a recording on an interval nobody controls.

Output demo/realtime.gif
Output demo/realtime.mp4

Require tmux
Require bash

Set Shell       "bash"
Set FontSize    14
Set Width       1700
Set Height      800
Set Padding     20
Set Framerate   24
Set TypingSpeed 28ms
Set Theme       "Catppuccin Mocha"

Hide
Type "cd $(git rev-parse --show-toplevel)" Enter
Type "export PATH=$PWD/demo/bin:$PATH PS1='$ ' PROMPT_COMMAND=" Enter
Type "export DATAZOO_NO_BANNER=1" Enter
Type "export SAP_HOST=localhost SAP_PORT=50000 SAP_CLIENT=001 SAP_USER=DEVELOPER" Enter
# Read from the gitignored credentials file, in the hidden block, so the value
# never reaches the frame and never reaches a process list. Without it the sync
# stops on a password prompt and the recording is of a stalled terminal.
Type "export SAP_PASSWORD=$(demo/creds.sh)" Enter
Type "clear" Enter
Sleep 500ms
Type "bash demo/stage.sh" Enter
Sleep 9s
Type "clear && tmux -L demo attach -t d" Enter
Sleep 3s
Show

# ------------------------------------------------------- 0:00  the shape
Sleep 4s

# ------------------------------------------------------- 0:04  no trick
# The ABAP is shown once, in an editor, with syntax highlighting -- so nothing
# that follows has to be taken on trust. Held for the first two branches, then
# paged twice to the physical DELETE, which is the one the tiers disagree about.
Type "bash demo/show-abap.sh"
Enter
Sleep 9s
Ctrl+D
Sleep 6s
Ctrl+D
Sleep 6s
Escape
Type ":q"
Enter
Sleep 1s

# ------------------------------------------------------- 0:26  SAP has a million, DuckDB has none
Type "erpl-rev sql --quiet 'SELECT count(*) AS movements FROM stock_moves'"
Enter
Sleep 5s

# ------------------------------------------------------- 0:33  the initial sync
Type "erpl-rev replicate --table ZSTOCK_MOVE --target stock_moves --yes"
Enter
Sleep 18s

Type "erpl-rev sql --quiet 'SELECT count(*) AS movements FROM stock_moves'"
Enter
Sleep 6s

# ------------------------------------------------------- 1:01  ten of the million
# A count proves a number. It does not prove anything readable arrived -- a
# target full of NULLs counts the same. Ten rows from end to end of the table,
# at a fixed stride, so nothing here is the top of the table or a lucky draw.
Type "erpl-rev sql --quiet -f demo/sample.sql"
Enter
Sleep 9s

# ------------------------------------------------------- 1:11  post a goods receipt
Type "uvx erpl-adt object run ZCL_GOODS_MOVEMENT"
Enter
Sleep 10s

Type "erpl-rev sql --quiet -f demo/items.sql"
Enter
Sleep 7s

# ------------------------------------------------------- 1:30  transfer two items
Type "uvx erpl-adt object run ZCL_GOODS_MOVEMENT"
Enter
Sleep 10s

Type "erpl-rev sql --quiet -f demo/items.sql"
Enter
Sleep 7s

# ------------------------------------------------------- 1:50  archive one, physically
# The row leaves SAP with no tombstone. A watermark cannot see this; the
# trigger tier can, which is why this target runs on it.
Type "uvx erpl-adt object run ZCL_GOODS_MOVEMENT"
Enter
Sleep 10s

Type "erpl-rev sql --quiet -f demo/items.sql"
Enter
Sleep 7s

# ------------------------------------------------------- 2:09  what it cost
Type "erpl-rev sql --quiet -f demo/latency.sql"
Enter
Sleep 11s

# ------------------------------------------------------- 2:21  an ordinary working day
# Six changes one at a time prove the mechanism and say nothing about
# continuous traffic. Thirty seconds at a few dozen rows a second, all three
# operations mixed: postings, corrections, and an archiving run pruning behind
# them. The left pane is silent for the whole half minute -- the classrun only
# prints when it returns -- which is the point: the right pane is where the
# work shows.
Type "uvx erpl-adt object run ZCL_STOCK_WORKLOAD"
Enter
Sleep 42s

Type "erpl-rev sql --quiet -f demo/workload.sql"
Enter
Sleep 12s

Hide
Type "tmux -L demo kill-server" Enter
