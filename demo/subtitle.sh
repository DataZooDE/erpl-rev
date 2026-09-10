#!/usr/bin/env bash
# Burn demo/subtitles.srt into the recording.
#
# vhs has no subtitle support, so this is a post-pass. The master that vhs
# writes stays untouched -- subtitles are an interpretation of the clip and the
# unnarrated one is the evidence. Re-run after every render; the cue times are
# arithmetic from the tape's own Sleep and TypingSpeed values, so they only stay
# correct while those do.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

in=demo/realtime.mp4
out=demo/realtime-subtitled.mp4
[ -f "$in" ] || { echo "no $in -- run: vhs demo/realtime.tape" >&2; exit 1; }

# A band is added below the frame rather than drawing over it: the terminal
# fills its own height, and a caption laid on top would cover the last line of
# whatever it is captioning. It once covered the DELETE it was describing.
#
# The band holds TWO lines even though every cue is written to fit on one. A
# cue that wraps is bottom-anchored, so the second line does not push the first
# down -- it pushes it UP, out of the band and over the terminal, which is
# exactly the overlap the band exists to prevent. Sixty-four pixels was one
# line of headroom and one line of damage.
ffmpeg -y -loglevel error -i "$in" \
  -vf "pad=iw:ih+118:0:0:0x11111b,\
subtitles=demo/subtitles.srt:force_style='FontName=DejaVu Sans,Fontsize=17,PrimaryColour=&H00E4E2E6,BorderStyle=1,Outline=0,Shadow=0,MarginV=14'" \
  -c:v libx264 -preset slow -crf 20 -pix_fmt yuv420p "$out"

echo "$out"
