/*
 * 6x8 bitmap font, ASCII 32..126 (from frank-msx / frank-test).
 * Each row is left-justified in the top 6 bits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FONT6X8_H
#define FONT6X8_H

#include <stdint.h>

#define FONT6X8_GLYPHS 95
extern const uint8_t font6x8[FONT6X8_GLYPHS][8];

#endif
