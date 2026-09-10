#!/usr/bin/env bash
# The lever, on screen, with syntax highlighting -- so nothing that follows has
# to be taken on trust.
#
# Read-only (-R) because this runs inside a recording where a stray keystroke
# would otherwise edit the file being demonstrated. The background is cleared to
# NONE so the editor sits in the terminal's own theme rather than dropping a
# differently-coloured rectangle into the middle of the frame.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# --clean: none of the operator's own nvim config. A plugin statusline leaking
# the branch name and a treesitter compile message into the frame is noise the
# viewer has to filter out, and the recording should not depend on whose laptop
# it was made on. The shipped syntax/abap.vim is all this needs.
exec nvim --clean -R \
  -c 'set termguicolors | colorscheme habamax | syntax on' \
  -c 'set laststatus=0 noshowcmd noruler signcolumn=no nonumber scrolloff=0' \
  -c 'hi Normal guibg=NONE ctermbg=NONE' \
  -c 'hi NonText guibg=NONE ctermbg=NONE' \
  -c 'hi EndOfBuffer guibg=NONE ctermbg=NONE' \
  demo/abap/zcl_goods_movement.abap
