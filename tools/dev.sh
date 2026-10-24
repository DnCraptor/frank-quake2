#!/bin/bash
# frank-quake2 — dev loop: rebuild, reflash the game over the UART (~BOOT~),
# capture the boot log.   tools/dev.sh [SECONDS] [TARGET]
set -e
cd "$(dirname "$0")/.."
D=${Q2_UART:-/dev/cu.usbmodem11102}
SECS=${1:-20}
TARGET=${2:-frank-quake2}
make -C build -j10 "$TARGET" 2>&1 | grep -E "error|warning: .*implicit|FLASH:|  RAM:" || true
stty -f "$D" 115200 raw -echo cs8 -cstopb -parenb clocal
printf '~BOOT~' > "$D"
for i in $(seq 1 30); do picotool info >/dev/null 2>&1 && break; sleep 0.3; done
picotool load "build/${TARGET}.uf2" >/dev/null
(timeout "$SECS" cat "$D" > build/boot.log &)
sleep 0.3
picotool reboot >/dev/null
sleep "$SECS"
cat build/boot.log
