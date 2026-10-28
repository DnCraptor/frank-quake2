#!/bin/bash
#
# frank-quake2 — Quake II for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-quake2
#
# Licensed under the GNU General Public License v3.0 or later.
# See LICENSE for the full license text.
#
# release.sh - Build release firmware for frank-quake2 (M1 / M2 / Z0)
#
# Usage: ./release.sh [VERSION] [--publish]
#   VERSION    version string (e.g. "1.00"), prompted interactively if omitted
#   --publish  also tag vVERSION, push the tag and create a GitHub release
#              with the UF2 (gh CLI); without it the files stay in release/
#
# Output: release/m2p2_frank-quake2_hdmi_A_BB.uf2 (M2, sound over HDMI),
#         release/m2p2_frank-quake2_i2s_A_BB.uf2 (M2, sound from the I2S DAC),
#         release/m1p2_frank-quake2_{hdmi,i2s}_A_BB.uf2 (M1: Murmulator 1)
#         release/z0p2_frank-quake2_{hdmi,i2s}_A_BB.uf2 (Z0: Waveshare RP2350-PiZero)
# Clocks: 504 MHz CPU / 168 MHz PSRAM (M1/Z0: 133) / 126 MHz flash (as tuned on the M2;
# the flash at 168 MHz stays a build option, see README).
# USB is the keyboard/mouse host; the debug console is UART0.
# No game data is included: the player copies the paks to the SD card, the
# first start builds the model/skin image in flash from them.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

RELEASE_CPU_SPEED="${RELEASE_CPU_SPEED:-504}"
RELEASE_PSRAM_SPEED="${RELEASE_PSRAM_SPEED:-168}"
RELEASE_FLASH_SPEED="${RELEASE_FLASH_SPEED:-126}"

# Build matrix: "board_variant:audio:prefix"
BUILD_MATRIX=(
    "M2:HDMI:m2p2_"
    "M2:I2S:m2p2_"
    "M1:HDMI:m1p2_"
    "M1:I2S:m1p2_"
    "Z0:HDMI:z0p2_"
    "Z0:I2S:z0p2_"
)

PUBLISH=0
ARGS=()
for a in "$@"; do
    case "$a" in
        --publish) PUBLISH=1 ;;
        *) ARGS+=("$a") ;;
    esac
done

if [ -z "$PICO_SDK_PATH" ] || [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    export PICO_SDK_PATH="$HOME/pico/pico-sdk"
fi

VERSION_FILE="version.txt"
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                  frank-quake2 Release Builder                   │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo -e "Variants: ${CYAN}${#BUILD_MATRIX[@]}${NC} (M1/M2/Z0)"
echo -e "Clocks:   ${CYAN}${RELEASE_CPU_SPEED}/${RELEASE_PSRAM_SPEED}/${RELEASE_FLASH_SPEED} MHz${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
if [[ ${#ARGS[@]} -gt 0 ]]; then
    INPUT_VERSION="${ARGS[0]}"
    echo -e "Version (from command line): ${CYAN}${INPUT_VERSION}${NC}"
else
    read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
    INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}
fi

if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))
if [[ $MAJOR -lt 0 || $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: bad version ${INPUT_VERSION}${NC}"
    exit 1
fi

VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
VERSION_DOT="${MAJOR}.$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${VERSION_DOT}${NC}"
echo "$MAJOR $MINOR" > "$VERSION_FILE"

RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"
BUILD_DIR="$SCRIPT_DIR/build-release"

SUCCEEDED=()
FAILED=()
for ENTRY in "${BUILD_MATRIX[@]}"; do
    IFS=':' read -r BOARD AUDIO PREFIX <<< "$ENTRY"
    AUDIO_LC=$(echo "$AUDIO" | tr '[:upper:]' '[:lower:]')
    LABEL="${PREFIX}${AUDIO_LC}"
    if [[ "$BOARD" = "M2" ]]; then
        VIDEO=HSTX
        PSRAM_MHZ=$RELEASE_PSRAM_SPEED
    else
        VIDEO=PIO
        PSRAM_MHZ=133
    fi
    if (( RELEASE_CPU_SPEED >= 504 )); then VOLT=1.65V
    elif (( RELEASE_CPU_SPEED >= 378 )); then VOLT=1.60V
    else VOLT=1.50V
    fi
    OUTPUT_NAME="${PREFIX%_}-frank-quake2-${VIDEO}-${AUDIO}-${RELEASE_CPU_SPEED}MHz-${VOLT}-P${PSRAM_MHZ}-F${RELEASE_FLASH_SPEED}-v${VERSION_DOT}.uf2"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${CYAN}Building: $OUTPUT_NAME${NC}"

    rm -rf "$BUILD_DIR"
    mkdir "$BUILD_DIR"
    if (cd "$BUILD_DIR" && cmake .. \
            -DPICO_PLATFORM=rp2350 \
            -DPICO_SDK_PATH="$PICO_SDK_PATH" \
            -DBOARD_VARIANT="$BOARD" \
            -DCPU_SPEED="$RELEASE_CPU_SPEED" \
            $( [ "$BOARD" = "M2" ] && echo "-DPSRAM_SPEED=$RELEASE_PSRAM_SPEED" ) \
            -DFLASH_SPEED="$RELEASE_FLASH_SPEED" \
            -DAUDIO_TYPE="$AUDIO" > cmake.log 2>&1) &&
       (cd "$BUILD_DIR" && make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" frank-quake2 > make.log 2>&1) &&
       [[ -f "$BUILD_DIR/frank-quake2.uf2" ]]; then
        cp "$BUILD_DIR/frank-quake2.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
        echo -e "  ${GREEN}✓ $LABEL${NC} → release/$OUTPUT_NAME"
        SUCCEEDED+=("$OUTPUT_NAME")
    else
        echo -e "  ${RED}✗ $LABEL: build failed (see $BUILD_DIR/*.log)${NC}"
        FAILED+=("$LABEL")
    fi
done
[[ ${#FAILED[@]} -eq 0 ]] && rm -rf "$BUILD_DIR"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
for FNAME in "${SUCCEEDED[@]}"; do
    ls -la "$RELEASE_DIR/$FNAME" | awk '{printf "  %-55s (%s bytes)\n", $9, $5}'
done
echo -e "Version: ${CYAN}${VERSION_DOT}${NC}"
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}${#FAILED[@]} variant(s) failed${NC}"
    exit 1
fi

if [[ $PUBLISH -eq 1 ]]; then
    TAG="v${VERSION_DOT}"
    NOTES="$RELEASE_DIR/notes_${VERSION}.md"
    cat > "$NOTES" <<NOTES_EOF
Quake II for Murmulator M1/M2 and Waveshare RP2350-PiZero (Z0) (RP2350, 8 MB PSRAM, HDMI, USB keyboard and mouse).

**Install**

1. Pick the firmware for your board and sound output. M2: \`${SUCCEEDED[0]}\` plays the
   sound over HDMI (the TV's speakers), \`${SUCCEEDED[1]}\` from the board's I2S DAC.
   M1 (Murmulator 1): \`${SUCCEEDED[2]}\` over PIO HDMI, \`${SUCCEEDED[3]}\` from the
   board's I2S DAC (GP26/27/28).
   Z0 (Waveshare RP2350-PiZero): \`${SUCCEEDED[4]}\` over HDMI, \`${SUCCEEDED[5]}\` from an
   I2S DAC module (PCM5102A: DIN GP10, BCK GP11, LRCK GP12).
   Hold BOOTSEL, plug the board in, and copy it to the RP2350 drive
   (or \`picotool load -f FILE.uf2\`).
2. Copy the retail Quake II data, patched to 3.20, to the SD card (FAT32 or exFAT):
   \`/quake2/baseq2/pak0.pak\`, \`pak1.pak\`, \`pak2.pak\`.
3. Start the board. The first start builds the models and skins into flash from the
   paks (about 90 seconds, once) and restarts. Then the game starts.

No game data is included. Saves and config.cfg go to \`/quake2/baseq2/\` on the card.
See the README for the options (flash at 168 MHz, \`vsync 0\`, faster textures).
NOTES_EOF
    git add "$VERSION_FILE"
    git commit -m "Release ${VERSION_DOT}" -- "$VERSION_FILE" || true
    git tag -a "$TAG" -m "frank-quake2 ${VERSION_DOT}"
    git push origin HEAD "$TAG"
    gh release create "$TAG" "${SUCCEEDED[@]/#/$RELEASE_DIR/}" --title "frank-quake2 ${VERSION_DOT}" --notes-file "$NOTES"
    echo -e "${GREEN}Published ${TAG}${NC}"
fi
