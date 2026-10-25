/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Video: the software renderer draws 320x240 8-bit frames into SRAM
 * framebuffers; HDMI (drivers/hdmi, 640x480@60 on core 1) shows them
 * doubled both ways. There is no scanout loop: the line callback, in the
 * HSTX DMA IRQ, turns the next source row into a BGR555 line through the
 * palette (320 lookups, about a microsecond) once per pair of output lines,
 * so core 1 is free for the renderer's second half (QG_Parallel2).
 *
 * Page flips and palette changes are latched at vsync. Everything the IRQ
 * touches is in SRAM (the HSTX path must never wait on flash or PSRAM).
 *
 * Also a 6x8 text console drawn into a framebuffer (boot log, fatal errors).
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font6x8.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "psx_video.h"
#include "q2_platform.h"

#ifndef Q2_FB_COUNT
#define Q2_FB_COUNT 2
#endif

#define NSLOT 4 /* line buffers: the DMA reads a slot up to two lines after the callback */

static uint8_t fb[Q2_FB_COUNT][Q2_W * Q2_H] __attribute__((aligned(4)));
static uint16_t pal[2][256] __attribute__((aligned(4)));
static uint32_t line_buf[NSLOT][Q2_W / 2];

static volatile int fb_front;        /* shown */
static volatile int fb_pending = -1; /* shown from the next vsync */
static int fb_back = 1;              /* being drawn */
static volatile int pal_front;
static volatile int pal_pending = -1;

volatile uint32_t q2_vsync_count;

static void __not_in_flash_func(on_vsync)(void) {
    if (fb_pending >= 0) {
        fb_front = fb_pending;
        fb_pending = -1;
    }
    if (pal_pending >= 0) {
        pal_front = pal_pending;
        pal_pending = -1;
    }
    q2_vsync_count++;
}

static void __not_in_flash_func(on_line)(uint32_t out_line, psxv_line_t *line) {
    uint32_t row = out_line >> 1;
    uint32_t *dst = line_buf[row & (NSLOT - 1)];
    if (!(out_line & 1)) {
        const uint32_t *src = (const uint32_t *)(fb[fb_front] + row * Q2_W);
        const uint16_t *p = pal[pal_front];
        for (int i = 0; i < Q2_W / 4; i++) {
            uint32_t s = src[i];
            dst[2 * i] = p[s & 0xff] | (uint32_t)p[(s >> 8) & 0xff] << 16;
            dst[2 * i + 1] = p[(s >> 16) & 0xff] | (uint32_t)p[s >> 24] << 16;
        }
    }
    line->pixels = (const uint16_t *)dst;
    line->mode = PSXV_LINE_DOUBLE;
}

void q2_video_init(void) {
    psxv_set_line_callback(on_line);
    psxv_set_vsync_callback(on_vsync);
}

uint8_t *q2_video_backbuffer(void) {
    return fb[fb_back];
}

void q2_video_wait_vsync(void) {
    uint32_t n = q2_vsync_count;
    while (q2_vsync_count == n) tight_loop_contents();
}

/* The finished back buffer is shown from the next vsync; returns the buffer
 * to draw the next frame into. With two buffers that is the one still on
 * screen until then: qg_back_busy tells the renderer to wait for the flip
 * (QG_NEED_BACK) before its first pixel write, not here. */
volatile int qg_back_busy;

bool q2_vsync = true; /* false: flip at once (tearing, no wait) */

uint8_t *q2_video_present(void) {
    if (!q2_vsync) {
        /* the line callback takes the new buffer from its next line on; the
         * one it showed is drawn into right away */
        while (fb_pending >= 0) tight_loop_contents();
        __dmb();
        int old = fb_front;
        fb_front = fb_back;
        fb_back = old;
        qg_back_busy = 0;
        return fb[fb_back];
    }
    while (fb_pending >= 0) tight_loop_contents(); /* one flip in flight at most */
    qg_back_busy = 0;
    __dmb();
#if Q2_FB_COUNT == 2
    int next = fb_front; /* != fb_back */
    fb_pending = fb_back;
    fb_back = next;
    qg_back_busy = 1;
#else
    fb_pending = fb_back;
    int f = fb_front, p = fb_pending;
    for (int i = 0; i < Q2_FB_COUNT; i++)
        if (i != f && i != p && i != fb_back) {
            fb_back = i;
            break;
        }
#endif
    return fb[fb_back];
}

/* the back buffer is off screen once the pending flip happened */
void q2_video_wait_back(void) {
    while (fb_pending >= 0) tight_loop_contents();
    qg_back_busy = 0;
}

/* palette: 256 RGB triplets, stride bytes apart */
void q2_video_set_palette(const uint8_t *rgb, int stride) {
    while (pal_pending >= 0) tight_loop_contents();
    int n = pal_front ^ 1;
    for (int i = 0; i < 256; i++, rgb += stride)
        pal[n][i] = (uint16_t)((rgb[2] >> 3) << 10 | (rgb[1] >> 3) << 5 | rgb[0] >> 3);
    __dmb();
    pal_pending = n;
}

/* ------------------------------------------------------------------ */
/* Text console                                                        */
/* ------------------------------------------------------------------ */

#define CON_COLS (Q2_W / 6)
#define CON_ROWS (Q2_H / 8)

enum { CON_BG = 0, CON_FG = 1, CON_HI = 2, CON_ERR = 3 };

static uint8_t *con_fb;
static int con_col, con_row;
static uint8_t con_color = CON_FG;

static void con_palette(void) {
    static const uint8_t rgb[4 * 3] = {
        0, 0, 0,        /* background */
        200, 200, 200,  /* text */
        255, 200, 64,   /* highlight */
        255, 80, 64,    /* error */
    };
    uint8_t full[256 * 3];
    memset(full, 0, sizeof(full));
    memcpy(full, rgb, sizeof(rgb));
    q2_video_set_palette(full, 3);
}

/* Shows the console (in the buffer on screen), with its own palette. */
void q2_con_show(void) {
    while (fb_pending >= 0) tight_loop_contents();
    con_fb = fb[fb_front];
    memset(con_fb, CON_BG, Q2_W * Q2_H);
    con_col = con_row = 0;
    con_palette();
}

static void con_scroll(void) {
    memmove(con_fb, con_fb + Q2_W * 8, Q2_W * 8 * (CON_ROWS - 1));
    memset(con_fb + Q2_W * 8 * (CON_ROWS - 1), CON_BG, Q2_W * 8);
    con_row = CON_ROWS - 1;
}

static void con_putc(char ch) {
    if (ch == '\n') {
        con_col = 0;
        if (++con_row >= CON_ROWS) con_scroll();
        return;
    }
    if (ch == '\r') {
        con_col = 0;
        return;
    }
    if (con_col >= CON_COLS) {
        con_col = 0;
        if (++con_row >= CON_ROWS) con_scroll();
    }
    unsigned char u = (unsigned char)ch;
    const uint8_t *g = font6x8[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
    uint8_t *base = con_fb + con_row * 8 * Q2_W + con_col * 6;
    for (int y = 0; y < 8; y++) {
        uint8_t *p = base + y * Q2_W;
        for (int x = 0; x < 6; x++) p[x] = (g[y] & (0x80 >> x)) ? con_color : CON_BG;
    }
    con_col++;
}

static void con_vprint(int color, const char *fmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    printf("%s", buf);
    if (!con_fb) return;
    con_color = (uint8_t)color;
    for (const char *s = buf; *s; s++) con_putc(*s);
}

void q2_con_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    con_vprint(CON_FG, fmt, ap);
    va_end(ap);
}

void q2_con_highlight(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    con_vprint(CON_HI, fmt, ap);
    va_end(ap);
}

void q2_con_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    con_vprint(CON_ERR, fmt, ap);
    va_end(ap);
}
