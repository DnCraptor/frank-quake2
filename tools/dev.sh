#!/bin/bash
# frank-quake2 — dev loop: rebuild, reflash the game over the UART (~BOOT~),
# capture the boot log.   tools/dev.sh [SECONDS] [TARGET]
set -e
cd "$(dirname "$0")/.."
SECS=${1:-20}
TARGET=${2:-frank-quake2}
if ! make -C build -j10 "$TARGET" > build/make.log 2>&1; then
    grep -E "error" build/make.log | head -20
    echo "BUILD FAILED"
    exit 1
fi
grep -E "warning: .*implicit|FLASH:|  RAM:" build/make.log || true
picotool info >/dev/null 2>&1 || python3 tools/uart.py bootsel >/dev/null
for i in $(seq 1 40); do picotool info >/dev/null 2>&1 && break; sleep 0.25; done
if ! picotool load "build/${TARGET}.uf2" >/dev/null 2>&1; then
    echo "FLASH FAILED (board not in BOOTSEL?)"
    exit 1
fi
echo "flashed build/${TARGET}.uf2"
picotool reboot >/dev/null
python3 tools/uart.py cap "$SECS" build/boot.log
cat build/boot.log
