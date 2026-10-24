#!/usr/bin/env python3
#
# frank-quake2 — turns a "prof" dump (PROF <addr> <count> lines from the UART)
# into a per-function table, using the symbols of the ELF.
#
#   tools/prof.py LOG [build/frank-quake2.elf] [TOP]
#
# SPDX-License-Identifier: GPL-3.0-or-later
import bisect, re, subprocess, sys

log = sys.argv[1]
elf = sys.argv[2] if len(sys.argv) > 2 else "build/frank-quake2.elf"
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

syms = []
for line in subprocess.run(["arm-none-eabi-nm", "-n", "-S", "--defined-only", elf],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "tTwW":
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
syms.sort()
addrs = [s[0] for s in syms]


def name_of(a):
    i = bisect.bisect_right(addrs, a) - 1
    while i >= 0:
        start, size, name = syms[i]
        if start <= a < start + max(size, 2):
            return name
        if a - start > 0x10000:
            break
        i -= 1
    return "?%08x" % a


total = 0
funcs = {}
pat = re.compile(r"PROF ([0-9a-f]{8}) (\d+)")
for line in open(log, errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    a, n = int(m.group(1), 16), int(m.group(2))
    a &= ~1
    total += n
    f = name_of(a)
    region = "sram" if a >= 0x20000000 else "flash"
    k = (f, region)
    funcs[k] = funcs.get(k, 0) + n

print("%d samples" % total)
for (f, region), n in sorted(funcs.items(), key=lambda kv: -kv[1])[:top]:
    print("%6.2f%%  %6d  %-5s %s" % (100.0 * n / total, n, region, f))
