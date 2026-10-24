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
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "psx_video.h"
#include "q2_platform.h"
#include "qcommon/q_perf.h"
#include "quakegeneric.h"
#include "ref_soft/r_local.h"
#include "usbhid.h"

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
static volatile bool core1_ready;

int64_t parallelWaitUs; /* time the game core waited for core 1 */

void __not_in_flash_func(q2_core1_main)(void) {
    platform_cyccnt_enable();
    psxv_start();
    core1_ready = true;
    uint32_t seen = 0;
    while (1) {
        while (job_seq == seen) __wfe();
        seen = job_seq;
        __dmb();
        job_fn(job_arg, 1);
        __dmb();
        job_done_seq = seen;
        __sev();
    }
}

/* QG_NEED_BACK: the pending page flip happens at the next vsync */
void QG_WaitBack(void) {
    Perf_Push(PERF_PRESENT);
    q2_video_wait_back();
    Perf_Pop();
}

void QG_Parallel2(void (*fn)(void *arg, int part), void *arg) {
    QG_NEED_BACK(); /* both users (spans, model triangles) draw */
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
    __dmb();
    uint32_t seq = job_seq + 1;
    job_seq = seq;
    __sev();
    fn(arg, 0);
    uint32_t t0 = time_us_32();
    Perf_Push(PERF_PARALLEL);
    while (job_done_seq != seq) __wfe();
    __dmb();
    Perf_Pop();
    parallelWaitUs += time_us_32() - t0;
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
    q2_con_printf("Press a key to restart.\n");
    /* drain the keys held while the error came, then wait for a new one */
    sleep_ms(300);
    uint8_t code;
    int down;
    while (1) {
        platform_usb_poll();
        while (usbhid_get_key_action(&code, &down))
            if (down) platform_reboot(false);
        int ch = getchar_timeout_us(1000);
        if (ch >= 0) platform_uart_cmd(ch);
    }
}

void QG_FatalError(const char *message) {
    q2_con_show();
    q2_con_error("Error:\n");
    q2_halt(message);
}
