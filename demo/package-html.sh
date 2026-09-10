#!/usr/bin/env bash
# Package the recording as ONE file: a self-contained HTML page with the video
# embedded, so it can be attached to a mail or dropped in a chat and opened
# offline by anyone, with no server, no extension and no link to a host that
# might not be reachable from where they sit.
#
#   bash demo/package-html.sh [output.html]
#
# The chapter marks are computed from demo/realtime.tape's own Sleep and
# TypingSpeed values rather than typed in here. Anything else drifts the moment
# a beat is retimed, and a chapter list that lands on the wrong scene is worse
# than none.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=demo/realtime-subtitled.mp4
OUT=${1:-demo/realtime.html}
[ -f "$SRC" ] || { echo "no $SRC -- run: vhs demo/realtime.tape && bash demo/subtitle.sh" >&2; exit 1; }

python3 - "$SRC" "$OUT" <<'PY'
import base64, html, re, sys, pathlib

src, out = sys.argv[1], sys.argv[2]

# --- chapters, from the tape's own arithmetic -------------------------------
# Same walk as the recording: typing costs TypingSpeed per character, Sleep
# costs what it says, and the clock starts at Show.
tape = pathlib.Path('demo/realtime.tape').read_text().splitlines()
ts, t, shown, chapters = 0.028, 0.0, False, []
for raw in tape:
    line = raw.strip()
    if line == 'Show':
        shown = True
        continue
    if line == 'Hide':
        shown = False
        continue
    if not shown:
        continue
    m = re.match(r'#\s*-{5,}\s*\d+:\d+\s+(.*)$', line)
    if m:
        chapters.append((t, m.group(1).strip()))
        continue
    if line.startswith('#'):
        continue
    m = re.match(r'Type\s+"(.*)"$', line)
    if m:
        t += len(m.group(1)) * ts
        continue
    m = re.match(r'Sleep\s+([\d.]+)(m?s)$', line)
    if m:
        t += float(m.group(1)) / (1000 if m.group(2) == 'ms' else 1)
        continue
    if line in ('Enter', 'Escape') or line.startswith('Ctrl+'):
        t += ts
total = t

def clock(s):
    return f"{int(s) // 60}:{int(s) % 60:02d}"

marks = "\n".join(
    f'      <button type="button" data-at="{at:.1f}">'
    f'<span class="t">{clock(at)}</span>{html.escape(title)}</button>'
    for at, title in chapters)

b64 = base64.b64encode(pathlib.Path(src).read_bytes()).decode()

page = f"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>erpl-rev &mdash; SAP to DuckDB, live</title>
<style>
  :root {{ color-scheme: dark; }}
  * {{ box-sizing: border-box; }}
  body {{ margin: 0; padding: 2rem 1.25rem 3rem;
         background: #11111b; color: #cdd6f4;
         font: 15px/1.6 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }}
  main {{ max-width: 1180px; margin: 0 auto; }}
  h1 {{ font-size: 1.35rem; font-weight: 650; margin: 0 0 .35rem; letter-spacing: -.01em; }}
  .lede {{ color: #a6adc8; margin: 0 0 1.5rem; max-width: 70ch; }}
  video {{ width: 100%; border-radius: 10px; display: block;
           background: #11111b; box-shadow: 0 12px 40px rgba(0,0,0,.55); }}
  .cols {{ display: grid; gap: 1.75rem; grid-template-columns: 1fr; margin-top: 1.75rem; }}
  @media (min-width: 860px) {{ .cols {{ grid-template-columns: 1.15fr 1fr; }} }}
  h2 {{ font-size: .78rem; font-weight: 650; text-transform: uppercase;
        letter-spacing: .09em; color: #7f849c; margin: 0 0 .7rem; }}
  .marks {{ display: flex; flex-wrap: wrap; gap: .4rem; }}
  .marks button {{ appearance: none; cursor: pointer; font: inherit; font-size: .87rem;
                   color: #cdd6f4; background: #1e1e2e; border: 1px solid #313244;
                   border-radius: 7px; padding: .32rem .6rem; }}
  .marks button:hover {{ background: #313244; border-color: #585b70; }}
  .marks .t {{ color: #89b4fa; font-variant-numeric: tabular-nums;
               margin-right: .45rem; font-size: .8rem; }}
  dl {{ margin: 0; display: grid; grid-template-columns: auto 1fr; gap: .45rem 1.1rem; }}
  dt {{ color: #7f849c; }}
  dd {{ margin: 0; }}
  b {{ color: #a6e3a1; font-weight: 650; }}
  .note {{ margin-top: 2rem; padding-top: 1.1rem; border-top: 1px solid #313244;
           color: #7f849c; font-size: .86rem; max-width: 78ch; }}
  code {{ font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: .92em;
          color: #f9e2af; }}
</style>
<main>
  <h1>erpl-rev &mdash; SAP to DuckDB, live</h1>
  <p class="lede">One unedited terminal recording. Left is a real SAP system; right is
     <code>erpl-rev top</code>, started once and not touched again. Nothing is staged:
     every figure on screen is measured by a query you can see being typed.</p>

  <video id="v" controls playsinline preload="metadata"
         src="data:video/mp4;base64,{b64}"></video>

  <div class="cols">
    <section>
      <h2>Chapters</h2>
      <div class="marks" id="marks">
{marks}
      </div>
    </section>
    <section>
      <h2>What it measures</h2>
      <dl>
        <dt>Initial sync</dt><dd><b>1,000,000</b> goods movements in <b>10&nbsp;s</b></dd>
        <dt>Change latency</dt><dd><b>1.1&ndash;2.6&nbsp;s</b> from SAP commit to DuckDB</dd>
        <dt>Under load</dt><dd>644 operations in 30&nbsp;s, slowest <b>4.5&nbsp;s</b></dd>
        <dt>Captured by</dt><dd>database triggers &mdash; inserts, updates and
            <b>physical deletes</b></dd>
        <dt>In SAP</dt><dd>no agent, no API, no exit &mdash; ordinary ABAP writes</dd>
      </dl>
    </section>
  </div>

  <p class="note">Recorded on a laptop running SAP, the replication engine and DuckDB at
     once, against an ABAP Platform trial. <code>ZSTOCK_MOVE</code> is a fixture shaped
     like a material-document table, not a live MM posting &mdash; the shape is faithful,
     the provenance is not. The two clocks in the closing query are independent: when
     SAP's trigger saw the row change, and when the engine wrote it. Subtitles are
     commentary; the terminal is the evidence.</p>
</main>
<script>
  var v = document.getElementById('v');
  document.getElementById('marks').addEventListener('click', function (e) {{
    var b = e.target.closest('button');
    if (!b) return;
    v.currentTime = parseFloat(b.dataset.at);
    v.play();
  }});
</script>
</html>
"""
pathlib.Path(out).write_text(page)
print(f"{out}  {pathlib.Path(out).stat().st_size / 1048576:.1f} MB  "
      f"{len(chapters)} chapters  {clock(total)} runtime")
PY
