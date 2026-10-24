#!/usr/bin/env python3
# frank-quake2 — averages the r_dspeeds lines of a log (median per column).
#   tools/dspeeds.py LOG
import re, statistics, sys
rx = re.compile(r'^\s*[\d.]+\s+(\d+)\s+(\d+)p\s+(\d+)w\s+(\d+)b\s+(\d+)s \(\s*(\d+)d\s+(\d+)c\s+(\d+)x\)\s+(\d+)e \(\s*(\d+)t\s+(\d+)f\)\s+(\d+)a')
rows = [list(map(int, m.groups())) for l in open(sys.argv[1], errors='replace') for m in [rx.match(l)] if m][2:]
names = 'total particles world bmodels scan D_DrawSurfaces cache records entities xform tris alpha'.split()
print('%d frames' % len(rows))
for i, n in enumerate(names):
    print('%-16s mean %5.1f  median %4.1f ms' % (n, sum(r[i] for r in rows) / len(rows), statistics.median(r[i] for r in rows)))
