/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C heap: newlib's allocator is replaced (its _malloc_r & co) by two
 * dlmalloc mspaces, one on the SRAM left after .bss, one on the PSRAM left
 * after .psram_bss. Allocations go to PSRAM once it is up (SRAM before);
 * the SRAM heap is for explicit requests (heap_caps_malloc INTERNAL);
 * free/realloc dispatch on the address. The engine's own memory is
 * one big block of the PSRAM heap (the arena, other/q_arena.c).
 *
 * dlmalloc is built without locks: only core 0 allocates (core 1 runs the
 * HDMI IRQ and draws spans, it never allocates).
 */

#include <errno.h>
#include <stdbool.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "psram_dlmalloc.h"
#include "q2_platform.h"

typedef void *mspace;
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void *mspace_malloc(mspace msp, size_t bytes);
extern void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
extern void *mspace_realloc(mspace msp, void *ptr, size_t bytes);
extern void mspace_free(mspace msp, void *ptr);
extern size_t mspace_usable_size(const void *ptr);

extern uint8_t end[];          /* end of .bss: start of the SRAM heap */
extern uint8_t __HeapLimit[];  /* end of the RAM region */
extern uint8_t __psram_heap_start__[];

#define PSRAM_BASE 0x11000000u
#define PSRAM_END  0x11800000u

static mspace sram_msp;
static size_t sram_size, sram_used;
static size_t psram_size, psram_used;

static inline int is_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= PSRAM_BASE && a < PSRAM_END;
}

static bool psram_ready;

static void sram_init(void) {
    uintptr_t b = ((uintptr_t)end + 15) & ~(uintptr_t)15;
    sram_size = (uintptr_t)__HeapLimit - b;
    sram_msp = create_mspace_with_base((void *)b, sram_size, 0);
}

/* After psram_init(): until then everything comes from SRAM (stdio, the
 * SDK's early allocations). */
void q2_heap_init(void) {
    if (!sram_msp) sram_init();
    uintptr_t pb = ((uintptr_t)__psram_heap_start__ + 4095) & ~(uintptr_t)4095;
    psram_size = PSRAM_END - pb;
    psram_heap_init((void *)pb, psram_size);
    psram_ready = true;
}

void q2_heap_report(void) {
    printf("heap: SRAM %u/%u KB used, PSRAM %u/%u KB used\n", (unsigned)(sram_used / 1024),
           (unsigned)(sram_size / 1024), (unsigned)(psram_used / 1024), (unsigned)(psram_size / 1024));
}

static void *sram_alloc(size_t n) {
    if (!sram_msp) sram_init();
    void *p = mspace_malloc(sram_msp, n);
    if (p) sram_used += mspace_usable_size(p);
    return p;
}

static void *psram_alloc(size_t n) {
    if (!psram_ready) return NULL;
    void *p = psram_malloc(n);
    if (p) psram_used += psram_usable_size(p);
    return p;
}

/* Once PSRAM is up, the SRAM heap is kept for what asks for it
 * (heap_caps MALLOC_CAP_INTERNAL: the renderer's z-buffer and colormap). */
void *q2_malloc(size_t n) {
    void *p = NULL;
    if (!psram_ready) p = sram_alloc(n);
    if (!p) p = psram_alloc(n);
    if (!p) p = sram_alloc(n);
    return p;
}

void q2_free(void *p) {
    if (!p) return;
    if (is_psram(p)) {
        psram_used -= psram_usable_size(p);
        psram_free(p);
    } else {
        sram_used -= mspace_usable_size(p);
        mspace_free(sram_msp, p);
    }
}

void *q2_realloc(void *p, size_t n) {
    if (!p) return q2_malloc(n);
    if (!n) {
        q2_free(p);
        return NULL;
    }
    size_t old = is_psram(p) ? psram_usable_size(p) : mspace_usable_size(p);
    if (n <= old) return p;
    void *q = q2_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    q2_free(p);
    return q;
}

/* ---- newlib's allocator entry points ---- */

void *_malloc_r(struct _reent *r, size_t n) {
    void *p = q2_malloc(n);
    if (!p) r->_errno = ENOMEM;
    return p;
}

void _free_r(struct _reent *r, void *p) {
    (void)r;
    q2_free(p);
}

void *_realloc_r(struct _reent *r, void *p, size_t n) {
    void *q = q2_realloc(p, n);
    if (!q && n) r->_errno = ENOMEM;
    return q;
}

void *_calloc_r(struct _reent *r, size_t a, size_t b) {
    size_t n = a * b;
    if (b && n / b != a) {
        r->_errno = ENOMEM;
        return NULL;
    }
    void *p = _malloc_r(r, n);
    if (p) memset(p, 0, n);
    return p;
}

void *_memalign_r(struct _reent *r, size_t align, size_t n) {
    if (!sram_msp) sram_init();
    void *p = NULL;
    {
        p = mspace_memalign(sram_msp, align, n);
        if (p) sram_used += mspace_usable_size(p);
    }
    if (!p) {
        /* the PSRAM mspace lives in psram_heap.c, which has no memalign:
         * over-allocate is not possible to free later, so only SRAM aligns */
        r->_errno = ENOMEM;
    }
    return p;
}

size_t _malloc_usable_size_r(struct _reent *r, void *p) {
    (void)r;
    return is_psram(p) ? psram_usable_size(p) : mspace_usable_size(p);
}

/* ---- ESP-IDF heap_caps, for the engine ---- */

void *heap_caps_malloc(size_t size, unsigned caps) {
    if (caps & MALLOC_CAP_SPIRAM) return psram_alloc(size);
    if (caps & MALLOC_CAP_INTERNAL) return sram_alloc(size);
    return q2_malloc(size);
}

void heap_caps_free(void *p) {
    q2_free(p);
}

size_t heap_caps_get_free_size(unsigned caps) {
    if (!sram_msp) sram_init();
    if (caps & MALLOC_CAP_SPIRAM) return psram_size - psram_used;
    return sram_size - sram_used;
}

/* The largest block that can be allocated: probed (called once, when the
 * engine reserves its arena, with the heap still nearly empty). */
size_t heap_caps_get_largest_free_block(unsigned caps) {
    size_t n = heap_caps_get_free_size(caps) & ~(size_t)4095;
    for (; n >= 4096; n -= 4096) {
        void *p = heap_caps_malloc(n, caps);
        if (p) {
            heap_caps_free(p);
            return n;
        }
    }
    return 0;
}
