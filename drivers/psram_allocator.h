/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

#ifndef PSRAM_ALLOCATOR_H
#define PSRAM_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *psram_malloc(size_t size);
void *psram_realloc(void *ptr, size_t size);
void psram_free(void *ptr);
void psram_reset(void);
void psram_mark_session(void);    // Mark current offset for game session
void psram_restore_session(void); // Restore to marked offset
void *psram_get_scratch_1(size_t size);
void *psram_get_scratch_2(size_t size);
void *psram_get_file_buffer(size_t size);

void psram_set_temp_mode(int enable);
void psram_reset_temp(void);
size_t psram_get_temp_offset(void);
void psram_set_temp_offset(size_t offset);

void psram_set_sram_mode(int enable); // Force SRAM allocation for proper malloc/free

void psram_print_stats(void); // Print memory usage stats

#ifdef __cplusplus
}
#endif

#endif
