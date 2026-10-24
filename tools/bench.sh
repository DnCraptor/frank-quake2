#!/bin/bash
# frank-quake2 — benchmark: timedemo of demo1 with the perf recorder and the
# PC profiler (core 0). Output: build/bench.log, the timedemo result, the
# perf summary of 300 frames, the per-function profile.
#   tools/bench.sh [PROF_SECONDS]
set -e
cd "$(dirname "$0")/.."
P=${1:-8}
python3 tools/uart.py script build/bench.log \
    "cmd timedemo 1" "cmd map demo1.dm2" "until Installation 90" "wait 3" \
    "cmd perf start 300" "cmd prof $P" "until PROFEND $((P + 20))" "cmd perf sum" "until PERFSUM 10" \
    "until frames,.*seconds 150" "cmd timedemo 0" "wait 1"
LC_ALL=C grep -aE "PERFSUM|frames, .* seconds" build/bench.log | tr -d '\r' || true
python3 tools/prof.py build/bench.log build/frank-quake2.elf ${TOP:-30}
