#!/usr/bin/env bash
# Every relative link and heading anchor in the docs must resolve.
#
# Written after a restructure moved a dozen files: the links were checked by hand
# each time, which works exactly until someone forgets. A moved heading or a
# deleted document otherwise rots quietly, and the first person to find it is a
# reader who wanted the thing it pointed at.
#
#   bash scripts/check-doc-links.sh
set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
import re, pathlib, sys

files = sorted(pathlib.Path('docs').glob('*.md')) + [pathlib.Path('README.md'),
                                                     pathlib.Path('TELEMETRY.md'),
                                                     pathlib.Path('SECURITY.md')]
files = [f for f in files if f.exists()]

def anchors(path):
    """GitHub's heading -> anchor rule, near enough: lowercase, drop anything
    that is not a word character, space or hyphen, then spaces to hyphens."""
    out = set()
    for line in path.read_text().splitlines():
        m = re.match(r'^#{1,6}\s+(.*?)\s*$', line)
        if not m:
            continue
        t = m.group(1).lower()
        t = re.sub(r'`|\*|_', '', t)
        t = re.sub(r'[^\w\s-]', '', t)
        # Each space becomes a hyphen -- NOT runs collapsed into one.
        # "SDK + DuckDB" loses the '+' and keeps both spaces, so the
        # anchor is ...sdk--duckdb with two hyphens.
        out.add(re.sub(r'\s', '-', t.strip()))
    return out

bad = []
for f in files:
    for m in re.finditer(r'\[[^\]]*\]\(([^)\s]+)\)', f.read_text()):
        target = m.group(1)
        if target.startswith(('http://', 'https://', 'mailto:')):
            continue
        path_part, _, frag = target.partition('#')
        dest = (f.parent / path_part).resolve() if path_part else f.resolve()
        if not dest.exists():
            bad.append(f"{f}: missing file -> {target}")
            continue
        if frag and dest.suffix == '.md' and frag not in anchors(dest):
            bad.append(f"{f}: missing anchor -> {target}")

if bad:
    print("BROKEN DOC LINKS:")
    for b in bad:
        print(f"  {b}")
    sys.exit(1)
print(f"DOC LINKS OK -- {len(files)} files, every relative link and anchor resolves")
PY
