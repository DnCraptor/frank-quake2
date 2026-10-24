/*
 * frank-quake2 — the ESP-IDF heap_caps API on the RP2350's two heaps:
 * MALLOC_CAP_INTERNAL is the SRAM heap, MALLOC_CAP_SPIRAM the PSRAM one
 * (see src/rp2350/q2_malloc.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef Q2_COMPAT_ESP_HEAP_CAPS_H
#define Q2_COMPAT_ESP_HEAP_CAPS_H

#include <stddef.h>

#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)

void *heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);

#endif
