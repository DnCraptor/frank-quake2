/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thread-local storage, one copy per core: the renderer keeps the state of
 * the code both cores run (the alias triangle rasterizer) in __thread
 * variables (R_TLS in ref_soft/r_local.h), as on the ESP32.
 *
 * arm-none-eabi GCC emulates TLS: every access goes through
 * __emutls_get_address(&__emutls_v.var). libgcc's version keeps a single
 * copy without threads; this one indexes the variable in a per-core table
 * (core number from SIO CPUID) and gives each core its own copy, taken from
 * a static per-core pool and initialized from the variable's template.
 */

#include <stdint.h>
#include <string.h>

#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico.h"
#include "q2_platform.h"

/* libgcc's layout of __emutls_v.<name> */
struct __emutls_object {
    uintptr_t size;
    uintptr_t align;
    uintptr_t index; /* 0: not registered yet, else index + 1 */
    void *templ;     /* initial value, NULL: zeroes */
};

#define MAX_VARS  96
#define POOL_SIZE 2048

static void *table[2][MAX_VARS + 1];
static uint8_t pool[2][POOL_SIZE] __attribute__((aligned(16)));
static uint32_t pool_used[2];
static uint32_t nvars;
static volatile uint32_t reg_lock;

static void __attribute__((noinline, noreturn)) tls_fail(const char *what) {
    panic("TLS: %s", what);
}

static uintptr_t __not_in_flash_func(tls_register)(struct __emutls_object *obj) {
    /* both cores can meet a variable for the first time together */
    while (__atomic_exchange_n(&reg_lock, 1, __ATOMIC_ACQUIRE)) tight_loop_contents();
    uintptr_t idx = obj->index;
    if (!idx) {
        if (nvars >= MAX_VARS) tls_fail("too many variables");
        idx = ++nvars;
        __atomic_store_n(&obj->index, idx, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&reg_lock, 0, __ATOMIC_RELEASE);
    return idx;
}

static void *__not_in_flash_func(tls_alloc)(struct __emutls_object *obj, uint32_t core, uintptr_t idx) {
    uintptr_t align = obj->align > 4 ? obj->align : 4;
    uint32_t at = (pool_used[core] + align - 1) & ~(uint32_t)(align - 1);
    if (at + obj->size > POOL_SIZE) tls_fail("pool full");
    void *p = &pool[core][at];
    pool_used[core] = at + obj->size;
    if (obj->templ)
        memcpy(p, obj->templ, obj->size);
    else
        memset(p, 0, obj->size);
    table[core][idx] = p;
    return p;
}

void *__not_in_flash_func(__emutls_get_address)(struct __emutls_object *obj) {
    uint32_t core = sio_hw->cpuid;
    uintptr_t idx = __atomic_load_n(&obj->index, __ATOMIC_ACQUIRE);
    if (__builtin_expect(!idx, 0)) idx = tls_register(obj);
    void *p = table[core][idx];
    if (__builtin_expect(!p, 0)) p = tls_alloc(obj, core, idx);
    return p;
}

size_t q2_tls_size(void) {
    return pool_used[0];
}
