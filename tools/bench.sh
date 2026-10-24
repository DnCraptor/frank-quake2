#!/bin/bash
# frank-quake2 — benchmark: timedemo of demo1 with the perf recorder and the
# PC profiler. Output: build/bench.log, the per-function profile, the summary.
#   tools/bench.sh [PROF_SECONDS]
set -e
cd "$(dirname "$0")/.."
D=${Q2_UART:-/dev/cu.usbmodem11102}
P=${1:-8}
send() { printf '%s\n' "$1" >&3; sleep 0.3; }
stty -f "$D" 115200 raw -echo cs8 -cstopb -parenb clocal
(timeout 110 cat "$D" > build/bench.log &)
sleep 0.3
exec 3> "$D"            # one open: the first bytes after opening the port can be lost
printf '\n' >&3
sleep 0.2
send "timedemo 1"
send "perf start 3000"
send "map demo1.dm2"
sleep 6
send "prof $P"
sleep 70
send "perf sum"
sleep 2
send "timedemo 0"
sleep 32
grep -aE "PERFSUM|frames, .* seconds" build/bench.log | tr -d '\r' || true
python3 tools/prof.py build/bench.log build/frank-quake2.elf 35
