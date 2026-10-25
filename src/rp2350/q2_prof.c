/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PC-sampling profiler for core 0 ("prof SECONDS" on the Quake console).
 * A TIMER1 alarm interrupts core 0 at 1 kHz and the naked handler takes the
 * interrupted PC from the exception frame (after frank-castle's profiler).
 * When done the samples are sorted and printed as a histogram of
 * addresses ("PROF <addr> <count>"), short enough to survive a lossy UART;
 * tools/prof.py maps the buckets to functions.
 */

#include <stdio.h>
#include <stdlib.h>

#include "hardware/irq.h"
#include "hardware/structs/timer.h"
#include "pico/stdlib.h"
#include "q2_platform.h"

#define PROF_HZ   1000
#define PROF_MAX  8192 /* 32 KB: 8 s */
#define BUCKET    0 /* exact; bit 0 tags the callers of memcpy/memset */

static uint32_t samples[PROF_MAX] __attribute__((section(".psram_bss.prof")));
static volatile uint32_t nsamples, want;
static volatile bool running;
static bool installed;

extern char memcpy[], memset[], strcmp[];

/* a sample in memcpy or memset is charged to its caller (LR), tagged by
 * bit 0 (tools/prof.py shows it as "memcpy/memset from ...") */
void __not_in_flash_func(q2_prof_record)(uint32_t pc, uint32_t lr) {
    timer_hw_t *t = timer1_hw;
    t->intr = 1u << 0;
    if (!running) return;
    t->alarm[0] = t->timerawl + 1000000 / PROF_HZ;
    if (pc - (uint32_t)(uintptr_t)memcpy < 0x100 || pc - (uint32_t)(uintptr_t)memset < 0x100 ||
        pc - (uint32_t)(uintptr_t)strcmp < 0x100)
        pc = (lr & ~1u) | 1u;
    samples[nsamples++] = pc;
    if (nsamples >= want) {
        running = false;
        t->inte &= ~1u;
    }
}

/* exception frame: r0 r1 r2 r3 r12 lr pc xpsr */
void __attribute__((naked)) __not_in_flash_func(q2_prof_isr)(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "ldr r1, [r0, #20]\n"
        "ldr r0, [r0, #24]\n"
        "b q2_prof_record\n");
}

void q2_prof_start(int seconds) {
    if (running) return;
    if (seconds < 1) seconds = 1;
    uint32_t n = (uint32_t)seconds * PROF_HZ;
    want = n > PROF_MAX ? PROF_MAX : n;
    nsamples = 0;
    timer_hw_t *t = timer1_hw;
    if (!installed) {
        irq_set_exclusive_handler(TIMER1_IRQ_0, q2_prof_isr);
        irq_set_priority(TIMER1_IRQ_0, 0x40);
        irq_set_enabled(TIMER1_IRQ_0, true);
        installed = true;
    }
    running = true;
    t->inte |= 1u;
    t->alarm[0] = t->timerawl + 1000;
    printf("prof: sampling %lu ticks\n", (unsigned long)want);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* called by the game loop: prints the histogram once the samples are in */
void q2_prof_poll(void) {
    static bool printed = true;
    if (running) {
        printed = false;
        return;
    }
    if (printed || !nsamples) return;
    printed = true;
    uint32_t n = nsamples;
    for (uint32_t i = 0; i < n; i++) samples[i] >>= BUCKET;
    qsort(samples, n, sizeof(samples[0]), cmp_u32);
    printf("PROFBEGIN %lu %d\n", (unsigned long)n, 1 << BUCKET);
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        while (j < n && samples[j] == samples[i]) j++;
        if (j - i >= 2) printf("PROF %08lx %lu\n", (unsigned long)(samples[i] << BUCKET), (unsigned long)(j - i));
        i = j;
    }
    printf("PROFEND\n");
}
