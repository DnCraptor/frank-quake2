/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

// PSRAM heap using dlmalloc mspace — real malloc/free/realloc on PSRAM
#include "psram_dlmalloc.h"
#include "psram_allocator.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define PSRAM_BASE      0x11000000
#define PSRAM_END       0x11800000
#define PSRAM_NOCACHE   0x15000000

/* Heap starts after every static PSRAM section (data, bss, overlay slots). */
extern uint8_t __psram_heap_start__[];

// dlmalloc mspace API (from dlmalloc.c compiled with ONLY_MSPACES=1)
typedef void* mspace;
extern mspace create_mspace_with_base(void* base, size_t capacity, int locked);
extern void*  mspace_malloc(mspace msp, size_t bytes);
extern void*  mspace_realloc(mspace msp, void* ptr, size_t bytes);
extern void   mspace_free(mspace msp, void* ptr);
extern size_t mspace_usable_size(const void* ptr);

static mspace psram_msp = NULL;
static int psram_initialized = 0;

static void psram_heap_init_internal(void *base, size_t size, int verbose) {
    psram_msp = create_mspace_with_base(base, size, 0);
    if (psram_msp) {
        psram_initialized = 1;
        if (verbose) {
            printf("PSRAM heap: %dKB at 0x%08x (dlmalloc mspace)\n",
                   (int)(size/1024), (unsigned)(uintptr_t)base);
        }
    } else {
        if (verbose) {
            printf("PSRAM heap: FAILED to create mspace\n");
        }
    }
}

void psram_heap_init(void *base, size_t size) {
    psram_heap_init_internal(base, size, 1);
}

// Reserve top 2MB of PSRAM for tile cache (managed by cache1d, not dlmalloc)
#define TILE_CACHE_SIZE (256 * 1024)
static void *tile_cache_base = NULL;
static size_t tile_cache_size = 0;

static void psram_heap_auto_init_internal(int verbose) {
    if (!psram_initialized) {
        uintptr_t bss_end = (uintptr_t)__psram_heap_start__;
        bss_end = (bss_end + 0xFFF) & ~0xFFF;
        size_t total_free = PSRAM_END - bss_end;

        // frank-castle: no tile cache, the whole remainder is heap
        (void)total_free;
        tile_cache_size = 0;
        tile_cache_base = (void*)PSRAM_END;

        // Give the rest to dlmalloc
        size_t heap_size = (uintptr_t)tile_cache_base - bss_end;
        psram_heap_init_internal((void*)bss_end, heap_size, verbose);

    }
}

void psram_heap_preinit(void) {
    psram_heap_auto_init_internal(0);
}

void psram_heap_auto_init(void) {
    psram_heap_auto_init_internal(1);
}

void *psram_get_tile_cache(size_t *out_size) {
    if (out_size) *out_size = tile_cache_size;
    return tile_cache_base;
}

static void ensure_init(void) {
    if (!psram_initialized) {
        psram_heap_auto_init();
    }
}

static int is_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= PSRAM_BASE && a < PSRAM_END) ||
           (a >= PSRAM_NOCACHE && a < PSRAM_NOCACHE + (PSRAM_END - PSRAM_BASE));
}

static size_t psram_total_alloc = 0;

void *psram_malloc(size_t size) {
    ensure_init();
    if (!psram_msp) return NULL;
    void *p = mspace_malloc(psram_msp, size);
    if (p) psram_total_alloc += size;
    return p;
}

void *psram_realloc(void *ptr, size_t size) {
    ensure_init();
    if (!psram_msp) return NULL;
    if (!ptr) return mspace_malloc(psram_msp, size);
    if (!is_psram(ptr)) {
        // Moving from SRAM to PSRAM
        void *np = mspace_malloc(psram_msp, size);
        if (np && ptr) {
            memcpy(np, ptr, size); // best effort
            extern void __real_free(void*);
            __real_free(ptr);
        }
        return np;
    }
    return mspace_realloc(psram_msp, ptr, size);
}

void psram_free(void *ptr) {
    if (!ptr) return;
    if (is_psram(ptr)) {
        if (psram_msp) mspace_free(psram_msp, ptr);
        return;
    }
    extern void __real_free(void*);
    __real_free(ptr);
}

size_t psram_usable_size(void *ptr) {
    return ptr ? mspace_usable_size(ptr) : 0;
}
