/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"

void psram_init(uint cs_pin);
void psram_reset_to_spi(void);

#endif
