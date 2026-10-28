#!/bin/bash
#
# frank-quake2 — Quake II for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: ./build.sh [M1|M2|Z0] [CPU_MHz] [PSRAM_MHz] [FLASH_MHz]
# Defaults: M2, 504 MHz CPU, 168 MHz PSRAM (M1/Z0: 133), 126 MHz flash (read
# delays tuned per board with the diag firmware; see CMakeLists.txt).
# USB is always the keyboard/mouse host; the debug console is UART0 on GPIO0/1.
# AUDIO=HDMI ./build.sh: sound over HDMI instead of the I2S DAC (default I2S).
# Result: bin/<board>-frank-quake2-<video>-<audio>-<clocks>-v<version>.uf2.

set -e

BOARD=${1:-M2}
CPU=${2:-504}
if [ "$BOARD" = "M1" ] || [ "$BOARD" = "Z0" ]; then PSRAM=${3:-133}; else PSRAM=${3:-168}; fi
FLASH=${4:-126}
AUDIO=${AUDIO:-I2S}

if [ -z "$PICO_SDK_PATH" ] || [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    export PICO_SDK_PATH="$HOME/pico/pico-sdk"
fi

echo "Building frank-quake2: Board=$BOARD CPU=${CPU}MHz PSRAM=${PSRAM}MHz Flash=${FLASH}MHz Audio=$AUDIO"

rm -rf ./build
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 \
      -DBOARD_VARIANT=$BOARD \
      -DCPU_SPEED=$CPU \
      -DPSRAM_SPEED=$PSRAM \
      -DFLASH_SPEED=$FLASH \
      -DAUDIO_TYPE=$AUDIO \
      ..
make -j8
