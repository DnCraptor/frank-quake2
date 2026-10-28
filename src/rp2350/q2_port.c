/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The engine's porting interface (quakegeneric.h and the QG_* hooks of
 * port_esp32.c / other/q_system.c): time, the second core, frame output,
 * quit and fatal errors.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#if Q2_VIDEO_PIO
#include "hdmi_pio.h"
#else
#include "psx_video.h"
#endif
#include "q2_platform.h"
#include "qcommon/q_perf.h"
#include "quakegeneric.h"
#include "ref_soft/r_local.h"
#include "usbhid.h"
#include "ps2kbd.h"
#include "nespad.h"

int QG_Milliseconds(void) {
    return (int)(time_us_64() / 1000);
}

int QG_Microseconds(void) {
    return (int)time_us_32();
}

/* ------------------------------------------------------------------ */
/* Second core: HDMI and the other half of the renderer's work         */
/* ------------------------------------------------------------------ */

/* Core 1 runs the HSTX DMA IRQ (psxv_start binds it to the calling core)
 * and, between interrupts, the jobs of QG_Parallel2. */
static void (*volatile job_fn)(void *arg, int part);
static void *volatile job_arg;
static volatile uint32_t job_seq, job_done_seq;
static volatile uint32_t core1_part_us; /* time of core 1's last part */
static volatile bool job_async;          /* the job is not a QG_Parallel2 part */
/* A job deferred to the next async job: core 1 runs it after that one
 * (or QG_WaitAsync runs it here when none comes) */
static void (*volatile defer_fn)(void *arg, int part);
static void *volatile defer_arg;
static volatile bool core1_ready;

int64_t parallelWaitUs; /* time the game core waited for core 1 */
volatile uint32_t core1_busy_us; /* time core 1 spent in jobs */

/* run by core 1 between jobs, at each wake up (every HDMI line's IRQ): the
 * boot shell's starfield. In SRAM, it goes on during flash writes. */
void (*volatile q2_core1_idle)(void);

void __not_in_flash_func(q2_core1_main)(void) {
    platform_cyccnt_enable();
    psxv_start();
    core1_ready = true;
    uint32_t seen = 0;
    while (1) {
        while (job_seq == seen) {
            __wfe();
            void (*idle)(void) = q2_core1_idle;
            if (idle) idle();
        }
        seen = job_seq;
        __dmb();
        uint32_t t0 = time_us_32();
        job_fn(job_arg, 1);
        if (job_async && defer_fn) {
            void (*fn)(void *, int) = defer_fn;
            defer_fn = NULL;
            fn(defer_arg, 1);
        }
        uint32_t dt = time_us_32() - t0;
        if (!job_async) core1_part_us = dt; /* for the balance of QG_Parallel2 */
        core1_busy_us += dt;
        __dmb();
        job_done_seq = seen;
        __sev();
    }
}

/* QG_NEED_BACK: the pending page flip happens at the next vsync */
/* "rstats": where the waits for the page flip come from */
uint32_t q2_waitback_site[8], q2_waitback_us[8];

void QG_WaitBack(void) {
    uint32_t site = (uint32_t)(uintptr_t)__builtin_return_address(0), t0 = time_us_32();
    Perf_Push(PERF_PRESENT);
    q2_video_wait_back();
    Perf_Pop();
    for (int i = 0; i < 8; i++)
        if (q2_waitback_site[i] == site || !q2_waitback_site[i]) {
            q2_waitback_site[i] = site;
            q2_waitback_us[i] += time_us_32() - t0;
            break;
        }
}

/* The share of the work given to core 1: k of every 16 items (rows,
 * triangles, columns), spread evenly. Core 0 also runs the game and takes
 * interrupts, so an even split leaves core 1 waiting; the share follows
 * the measured times of both parts. */
volatile unsigned qg_split_mask = 0xaaaa;
volatile int qg_split_k = 8;
static uint32_t part_us[2]; /* time of each core's parts since the last balance */

static void set_split(int k) {
    unsigned m = 0;
    for (int i = 0; i < 16; i++)
        if (((i + 1) * k) / 16 != (i * k) / 16) m |= 1u << i;
    qg_split_k = k;
    qg_split_mask = m;
}

static void balance_split(void) {
    if (part_us[0] + part_us[1] < 20000) return; /* 20 ms of parallel work */
    int k = qg_split_k;
    /* core 0's part longer by more than 1/16: one more item to core 1 */
    if (part_us[0] * 16 > part_us[1] * 17 && k < 13)
        set_split(k + 1);
    else if (part_us[1] * 16 > part_us[0] * 17 && k > 3)
        set_split(k - 1);
    part_us[0] = part_us[1] = 0;
}

int q2_split_share(void) {
    return qg_split_k;
}

void QG_DeferAsync(void (*fn)(void *arg, int part), void *arg) {
    QG_WaitAsync(); /* runs a previous deferred job */
    defer_arg = arg;
    __dmb();
    defer_fn = fn;
}

/* A job for core 1 alone, core 0 goes on (QG_WaitAsync) */
void QG_RunAsync(void (*fn)(void *arg, int part), void *arg) {
    if (!core1_ready) {
        fn(arg, 1);
        QG_WaitAsync(); /* the deferred job */
        return;
    }
    /* the previous job, not the deferred one: core 1 takes it after this */
    while (job_done_seq != job_seq) __wfe();
    __dmb();
    job_fn = fn;
    job_arg = arg;
    job_async = true;
    __dmb();
    job_seq = job_seq + 1;
    __sev();
}

int QG_SecondCore(void) {
#ifdef Q2_SINGLE_CORE
    return 0;
#else
    return core1_ready;
#endif
}

uint32_t q2_waitasync_site[8], q2_waitasync_us[8]; /* "rstats" */

void QG_WaitAsync(void) {
    if (job_done_seq == job_seq) {
        if (defer_fn) { /* no async job came to take it */
            void (*fn)(void *, int) = defer_fn;
            defer_fn = NULL;
            fn(defer_arg, 1);
        }
        return;
    }
    uint32_t site = (uint32_t)(uintptr_t)__builtin_return_address(0), t0 = time_us_32();
    Perf_Push(PERF_PARALLEL);
    while (job_done_seq != job_seq) __wfe();
    __dmb();
    Perf_Pop();
    for (int i = 0; i < 8; i++)
        if (q2_waitasync_site[i] == site || !q2_waitasync_site[i]) {
            q2_waitasync_site[i] = site;
            q2_waitasync_us[i] += time_us_32() - t0;
            break;
        }
}

void QG_Parallel2(void (*fn)(void *arg, int part), void *arg) {
    if (job_done_seq != job_seq) {
        /* core 1 is on an async job (it scans the edges while this core
         * draws a full span buffer): do both parts here */
        fn(arg, 0);
        fn(arg, 1);
        return;
    }
#ifdef Q2_SINGLE_CORE
    const bool two = false;
#else
    const bool two = core1_ready;
#endif
    if (!two) {
        fn(arg, 0);
        fn(arg, 1);
        return;
    }
    job_fn = fn;
    job_arg = arg;
    job_async = false;
    __dmb();
    uint32_t seq = job_seq + 1;
    job_seq = seq;
    __sev();
    uint32_t ts = time_us_32();
    fn(arg, 0);
    uint32_t t0 = time_us_32();
    part_us[0] += t0 - ts;
    Perf_Push(PERF_PARALLEL);
    while (job_done_seq != seq) __wfe();
    __dmb();
    Perf_Pop();
    part_us[1] += core1_part_us;
    parallelWaitUs += time_us_32() - t0;
    balance_split();
}

/* PSRAM written through its uncached alias: no line fill for the lines a
 * surface cache build overwrites. Dirty lines in the range are written back
 * first (a cached header can share a line), then all its lines dropped. */
void *QG_WriteThrough(void *p, int size) {
    uintptr_t a = (uintptr_t)p;
    if (a < 0x11000000u || a >= 0x11800000u || size <= 0) return p;
    uintptr_t start = a & ~(uintptr_t)7, end = (a + (uintptr_t)size + 7) & ~(uintptr_t)7;
    xip_cache_clean_range(start - XIP_BASE, end - start);
    xip_cache_invalidate_range(start - XIP_BASE, end - start);
    return (void *)(a + 0x04000000u);
}

void QG_Uncache(void *p, int size) {
    uintptr_t a = (uintptr_t)p;
    if (a < 0x11000000u || a >= 0x11800000u || size <= 0) return;
    uintptr_t start = a & ~(uintptr_t)7, end = (a + (uintptr_t)size + 7) & ~(uintptr_t)7;
    xip_cache_invalidate_range(start - XIP_BASE, end - start);
}

/* the "perf" recorder only records the game */
int QG_IsGameTask(void) {
    return get_core_num() == 0;
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

/* The renderer's frame buffer (SWimp_SetMode): the back buffer, in SRAM */
void *QG_ScreenBuffer(int size) {
    return size == Q2_W * Q2_H ? q2_video_backbuffer() : NULL;
}

void QG_SetPalette(unsigned char palette[768]) {
    q2_video_set_palette(palette, 3);
}

static uint32_t frames, fps_t0;

/* SWimp_EndFrame: show the frame, draw the next one in the other buffer */
void QG_DrawFrame(void *pixels) {
    (void)pixels;
    vid.buffer = q2_video_present();
    frames++;
}

/* frames per second since the last call */
float q2_take_fps(void) {
    uint32_t now = time_us_32();
    float fps = (now - fps_t0) ? frames * 1e6f / (float)(now - fps_t0) : 0;
    frames = 0;
    fps_t0 = now;
    return fps;
}

/* ------------------------------------------------------------------ */
/* Quit and errors                                                     */
/* ------------------------------------------------------------------ */

void QG_Quit(void) {
    printf("quit\n");
    platform_reboot(false);
}

void __attribute__((noreturn)) q2_halt(const char *msg) {
    if (msg && *msg) q2_con_error("%s\n", msg);
    /* drain the keys held while the error came, then wait for a new one */
    sleep_ms(300);
    uint8_t code;
    int down;
    uint32_t old_pad = nespad_state | nespad_state2;
    while (1) {
        platform_usb_poll();
        while (usbhid_get_key_action(&code, &down))
            if (down) platform_reboot(false);
        while (ps2kbd_get_key_action(&code, &down))
            if (down) platform_reboot(false);
        nespad_read();
        uint32_t pad = nespad_state | nespad_state2;
        if (pad & ~old_pad) platform_reboot(false);
        old_pad = pad;
        sleep_ms(1);
    }
}

void QG_FatalError(const char *message) {
    q2_shell_error("ERROR", "Press any key to restart");
    q2_halt(message);
}
