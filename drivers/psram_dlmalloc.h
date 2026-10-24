/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

#ifndef PSRAM_DLMALLOC_H
#define PSRAM_DLMALLOC_H

#include <stddef.h>

void  psram_heap_init(void *base, size_t size);
void *psram_malloc(size_t size);
void *psram_realloc(void *ptr, size_t size);
void  psram_free(void *ptr);
size_t psram_usable_size(void *ptr);

#endif
