#!/bin/bash
#
# frank-quake2 — flash with picotool (board in BOOTSEL, or send ~BOOT~ on the UART).
#
#   ./flash.sh                     the game (build/frank-quake2.uf2)
#   ./flash.sh frank-quake2-diag   the bring-up firmware
#   ./flash.sh assets FILE.bin     the model/skin image from tools/mkassets.py
#
set -e
ASSETS_ADDR=${ASSETS_ADDR:-0x100E1000}   # XIP base + Q2_ASSETS_OFFSET (900 KiB)

if [ "$1" = "assets" ]; then
    [ -f "$2" ] || { echo "usage: $0 assets FILE.bin"; exit 1; }
    picotool load -f "$2" -t bin -o "$ASSETS_ADDR" && picotool reboot
    exit 0
fi
TARGET=${1:-frank-quake2}
picotool load -f "build/${TARGET}.uf2" && picotool reboot
