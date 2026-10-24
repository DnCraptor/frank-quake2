/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

// PSRAM allocator — now delegates to dlmalloc mspace in psram_heap.c
// This file only provides legacy compatibility stubs and utility functions.
#include "psram_allocator.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PSRAM_BASE 0x11000000
#define PSRAM_SIZE (8 * 1024 * 1024)

static uint8_t *psram_start = (uint8_t *)PSRAM_BASE;
extern uint8_t __psram_bss_end__[];

// These are no longer used — psram_malloc/free/realloc are in psram_heap.c
// Kept for compatibility with code that references them.

void psram_set_temp_mode(int enable) { (void)enable; }
void psram_set_sram_mode(int enable) { (void)enable; }
void psram_reset_temp(void) {}
size_t psram_get_temp_offset(void) { return 0; }
void psram_set_temp_offset(size_t offset) { (void)offset; }
void psram_reset(void) {}
void psram_mark_session(void) {}
void psram_restore_session(void) {}
void psram_print_stats(void) {}

void *psram_get_scratch_1(size_t size) { (void)size; return NULL; }
void *psram_get_scratch_2(size_t size) { (void)size; return NULL; }
void *psram_get_file_buffer(size_t size) { (void)size; return NULL; }
