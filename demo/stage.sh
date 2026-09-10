#!/usr/bin/env bash
# Build the two-pane stage for the recording.
#
# In a script rather than in the tape because vhs's parser cannot nest quotes,
# and because a stage that can be built outside a render can be checked outside
# a render.
set -u
S=d
T=(-L demo -f demo/tmux.conf)
tmux "${T[@]}" kill-server 2>/dev/null
tmux "${T[@]}" new-session -d -s $S -x "${COLS:-181}" -y "${ROWS:-45}"

# -d: do NOT take focus. The monitor is born running and is never typed into;
# without -d the split steals focus and every keystroke meant for the driver
# pane lands in the monitor.
tmux "${T[@]}" split-window -h -d -t $S 'erpl-rev top'

# Quiet the driver shell before labelling anything: the default prompt emits an
# OSC title escape and tmux takes that as the pane title, so a title set first
# is overwritten by a home directory the moment the prompt is drawn.
tmux "${T[@]}" send-keys -t $S.0 "export PS1='$ ' PROMPT_COMMAND=" Enter
tmux "${T[@]}" send-keys -t $S.0 clear Enter
# The throughput graph is behind a key, so the stage presses it -- as a bare
# keystroke, read by the monitor's event loop, not as a command. Sent after the
# monitor has had a moment to draw, or it lands before there is anything to
# receive it.
sleep 2
tmux "${T[@]}" send-keys -t $S.1 g

sleep 1
tmux "${T[@]}" select-pane -t $S.0
