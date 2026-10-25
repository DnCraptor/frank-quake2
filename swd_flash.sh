#!/bin/bash
#
# frank-quake2 — flash over SWD with a Debugprobe (CMSIS-DAP).
#
# Usage: ./swd_flash.sh [target] [--reset-only]   (default target: frank-quake2)
#
# 1 MHz: with the diag/game driving HSTX on GPIO12..19, flash erase at 4 MHz
# timed out (ROM call never halted); 1 MHz verifies. Same as frank-linux probe.sh.
set -euo pipefail
cd "$(dirname "$0")"

TARGET=frank-quake2
RESET_ONLY=0
for a in "$@"; do
    case "$a" in
        --reset-only) RESET_ONLY=1 ;;
        *) TARGET="$a" ;;
    esac
done

ARGS=(-f interface/cmsis-dap.cfg -c "adapter speed ${SWD_KHZ:-1000}" -f target/rp2350.cfg)

if [ "$RESET_ONLY" = 1 ]; then
    exec openocd "${ARGS[@]}" -c init -c "reset run" -c exit
fi

ELF="build/${TARGET}.elf"
[ -f "$ELF" ] || { echo "ERROR: $ELF not found. Run ./build.sh first." >&2; exit 1; }
openocd "${ARGS[@]}" -c "program $ELF verify reset exit"
