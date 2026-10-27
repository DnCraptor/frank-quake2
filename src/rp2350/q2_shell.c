/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The boot shell: a welcome screen (the boot log in a riveted metal panel,
 * a 3D starfield around it) and the error screens, drawn into the
 * framebuffer on screen with their own palette until the engine starts.
 *
 * The starfield moves in the vsync callback, on core 1 (q2_vsync_hook):
 * it goes on while core 0 is busy, the asset build's flash erases
 * included, so it and its data are in SRAM. It draws only outside the
 * panel, where core 0 never writes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font6x8.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "q2_platform.h"

#ifndef Q2_VERSION
#define Q2_VERSION "?"
#endif
#if defined(BOARD_M1)
#define BOARD_VARIANT_NAME "M1"
#elif defined(BOARD_Z0)
#define BOARD_VARIANT_NAME "Z0"
#else
#define BOARD_VARIANT_NAME "M2"
#endif

#define W Q2_W
#define H Q2_H

/* palette */
enum {
    C_BG = 0,
    C_TEXT,      /* tan */
    C_HI,        /* amber */
    C_ERR,       /* red */
    C_DIM,       /* dark tan */
    C_PANEL,     /* panel fill */
    C_BEVEL_LT,
    C_BEVEL,
    C_BEVEL_DK,
    C_ERRBAR,
    C_WHITE,
    C_STAR0,     /* far .. near */
    C_STAR1,
    C_STAR2,
    C_STAR3,
    C_SHADOW,
    C_TITLE,     /* 32 rows of gradient */
};
#define TITLE_ROWS 32

/* layout: the panel, inset like the other FRANK welcome screens */
#define PX0 24
#define PY0 16
#define PX1 (W - 24)
#define PY1 (H - 16)
#define BEVEL 4
#define TX0 (PX0 + BEVEL + 6) /* text area */
#define TX1 (PX1 - BEVEL - 6)
#define TITLE_Y (PY0 + BEVEL + 6)
#define SUB_Y (TITLE_Y + TITLE_ROWS + 5)
#define SEP1_Y (SUB_Y + 21)
#define FOOT_Y (PY1 - BEVEL - 5 - 30)
#define SEP2_Y (FOOT_Y - 5)
#define LOG_COLS ((TX1 - TX0) / 6)

static uint8_t *scr;
static bool err_mode;
static int log_y0, log_rows, log_col, log_row;
static uint32_t shown_us;

/* ------------------------------------------------------------------ */
/* Starfield (core 1, between its jobs)                                */
/* ------------------------------------------------------------------ */

#define NSTARS 256
#define STAR_FAR 1024
#define STAR_FOCAL 160
#define STAR_SPEED 9
#define STAR_TRAIL (STAR_SPEED * 3) /* a streak: where the star was 3 frames ago */
#define STAR_MAXLEN 20

/* 6 bytes a star, nothing else: the pixels of a streak are worked out
 * again from the star's depth to erase it */
typedef struct {
    int16_t x, y, z;
} star_t;

static star_t stars[NSTARS];
static uint32_t star_seed = 0x2f6b1c3du;
static uint8_t *volatile star_scr;
static volatile bool star_busy;
static uint32_t star_frame;

static uint32_t __not_in_flash_func(star_rand)(void) {
    star_seed = star_seed * 1664525u + 1013904223u;
    return star_seed >> 8;
}

static void __not_in_flash_func(star_spawn)(star_t *s, int z) {
    s->x = (int16_t)((int)(star_rand() % 2048) - 1024);
    s->y = (int16_t)((int)(star_rand() % 1536) - 768);
    s->z = (int16_t)z;
}

/* the streak from depth z + STAR_TRAIL to the star at z, in C_BG when
 * erasing: off the screen and on the panel nothing is drawn. Returns false
 * when the star is off the screen. */
static bool __not_in_flash_func(star_draw)(uint8_t *f, const star_t *s, bool erase) {
    int x1 = W / 2 + s->x * STAR_FOCAL / s->z;
    int y1 = H / 2 + s->y * STAR_FOCAL / s->z;
    if (x1 < 0 || x1 >= W || y1 < 0 || y1 >= H) return false;
    int tz = s->z + STAR_TRAIL;
    int x0 = W / 2 + s->x * STAR_FOCAL / tz;
    int y0 = H / 2 + s->y * STAR_FOCAL / tz;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int len = adx > ady ? adx : ady, n = len > STAR_MAXLEN ? STAR_MAXLEN : len;
    /* a pixel a step back along the major axis, from the head */
    int sx = len ? -(dx << 16) / len : 0, sy = len ? -(dy << 16) / len : 0;
    int fx = x1 << 16, fy = y1 << 16;
    /* the head's brightness by depth, fading along the tail */
    int head = s->z > 768 ? 0 : s->z > 512 ? 1 : s->z > 256 ? 2 : 3;
    for (int i = 0; i <= n; i++, fx += sx, fy += sy) {
        int x = (fx + 0x8000) >> 16, y = (fy + 0x8000) >> 16;
        if ((unsigned)x >= W || (unsigned)y >= H) break;
        if (x >= PX0 && x < PX1 && y >= PY0 && y < PY1) continue;
        int c = head - (i * 4) / (n + 1);
        f[y * W + x] = erase ? C_BG : (uint8_t)(C_STAR0 + (c < 0 ? 0 : c));
    }
    return true;
}

/* once a frame: all the streaks erased, moved, drawn again */
static void __not_in_flash_func(stars_idle)(void) {
    uint8_t *f = star_scr;
    if (!f || star_frame == q2_vsync_count) return;
    star_busy = true;
    __dmb();
    if (star_scr) { /* not stopped meanwhile */
        star_frame = q2_vsync_count;
        for (int i = 0; i < NSTARS; i++) star_draw(f, &stars[i], true);
        for (int i = 0; i < NSTARS; i++) {
            star_t *s = &stars[i];
            s->z -= STAR_SPEED;
            if (s->z < 16 || !star_draw(f, s, false)) star_spawn(s, STAR_FAR - (int)(star_rand() % 128));
        }
    }
    star_busy = false;
}

static void stars_stop(void) {
    star_scr = NULL;
    __dmb();
    while (star_busy) tight_loop_contents();
}

static void stars_start(uint8_t *f) {
    for (int i = 0; i < NSTARS; i++) star_spawn(&stars[i], 16 + (int)(star_rand() % (STAR_FAR - 16)));
    star_frame = q2_vsync_count;
    star_scr = f;
    q2_core1_idle = stars_idle;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void fill(int x0, int y0, int x1, int y1, uint8_t c) {
    for (int y = y0; y < y1; y++) memset(scr + y * W + x0, c, (size_t)(x1 - x0));
}

static void palette(void) {
    static const uint8_t base[C_TITLE * 3] = {
        0, 0, 0,        /* bg */
        206, 186, 148,  /* text */
        255, 168, 40,   /* highlight */
        255, 72, 48,    /* error */
        132, 112, 84,   /* dim */
        26, 21, 16,     /* panel */
        176, 120, 64,   /* bevel light */
        110, 66, 30,    /* bevel */
        44, 26, 12,     /* bevel dark */
        132, 20, 10,    /* error bar */
        255, 255, 255,  /* white */
        70, 70, 86,     /* stars */
        120, 120, 140,
        180, 180, 196,
        255, 255, 255,
        8, 5, 2,        /* title shadow */
    };
    uint8_t rgb[256 * 3];
    memset(rgb, 0, sizeof(rgb));
    memcpy(rgb, base, sizeof(base));
    /* the title: pale gold to orange, a bright rim halfway, down to rust */
    for (int i = 0; i < TITLE_ROWS; i++) {
        uint8_t *p = rgb + (C_TITLE + i) * 3;
        int r, g, b;
        if (i < 14) {
            r = 255; g = 230 - i * 6; b = 150 - i * 9;
        } else if (i < 17) {
            r = 255; g = 236; b = 180;
        } else {
            int k = i - 17;
            r = 220 - k * 7; g = 120 - k * 5; b = 30 - k * 2;
        }
        p[0] = (uint8_t)r;
        p[1] = (uint8_t)(g < 0 ? 0 : g);
        p[2] = (uint8_t)(b < 0 ? 0 : b);
    }
    q2_video_set_palette_now(rgb, 3);
}

static void glyph(int x, int y, char ch, uint8_t c) {
    unsigned u = (unsigned char)ch;
    const uint8_t *g = font6x8[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
    for (int r = 0; r < 8; r++)
        for (int k = 0; k < 6; k++)
            if (g[r] & (0x80 >> k)) scr[(y + r) * W + x + k] = c;
}

static void text(int x, int y, const char *s, uint8_t c) {
    for (; *s; s++, x += 6) glyph(x, y, *s, c);
}

static void text_center(int y, const char *s, uint8_t c) {
    text((W - 6 * (int)strlen(s)) / 2, y, s, c);
}

/* the title, the font scaled 4x: shadow, then the gradient by row */
static void title(const char *s) {
    const int scale = 4, n = (int)strlen(s);
    const int x0 = (W - n * 6 * scale) / 2;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            unsigned u = (unsigned char)s[i];
            const uint8_t *g = font6x8[u - 32];
            for (int r = 0; r < 8; r++)
                for (int k = 0; k < 6; k++) {
                    if (!(g[r] & (0x80 >> k))) continue;
                    int x = x0 + (i * 6 + k) * scale + (pass ? 0 : 2);
                    int y = TITLE_Y + r * scale + (pass ? 0 : 2);
                    for (int dy = 0; dy < scale; dy++)
                        memset(scr + (y + dy) * W + x, pass ? C_TITLE + r * scale + dy : C_SHADOW,
                               (size_t)scale);
                }
        }
}

static void separator(int y) {
    fill(TX0, y, TX1, y + 1, C_BEVEL_DK);
    fill(TX0, y + 1, TX1, y + 2, C_BEVEL);
}

static void rivet(int x, int y) {
    fill(x, y, x + 3, y + 3, C_BEVEL_DK);
    scr[y * W + x] = C_BEVEL_LT;
    scr[y * W + x + 1] = C_BEVEL_LT;
    scr[(y + 1) * W + x] = C_BEVEL_LT;
}

static void panel(void) {
    fill(PX0, PY0, PX1, PY1, C_BEVEL);
    fill(PX0, PY0, PX1, PY0 + 1, C_BEVEL_LT); /* lit from the top left */
    fill(PX0, PY0, PX0 + 1, PY1, C_BEVEL_LT);
    fill(PX0, PY1 - 1, PX1, PY1, C_BEVEL_DK);
    fill(PX1 - 1, PY0, PX1, PY1, C_BEVEL_DK);
    fill(PX0 + BEVEL - 1, PY0 + BEVEL - 1, PX1 - BEVEL + 1, PY1 - BEVEL + 1, C_BEVEL_DK);
    fill(PX0 + BEVEL, PY0 + BEVEL, PX1 - BEVEL, PY1 - BEVEL, C_PANEL);
    rivet(PX0 + BEVEL + 2, PY0 + BEVEL + 2);
    rivet(PX1 - BEVEL - 5, PY0 + BEVEL + 2);
    rivet(PX0 + BEVEL + 2, PY1 - BEVEL - 5);
    rivet(PX1 - BEVEL - 5, PY1 - BEVEL - 5);
}

static void footer(const char *hint) {
    char line[64];
    fill(TX0, FOOT_Y, TX1, FOOT_Y + 30, C_PANEL);
    if (hint) {
        text_center(FOOT_Y, hint, C_HI);
    } else {
        unsigned sys = (unsigned)(clock_get_hz(clk_sys) / 1000000);
        unsigned psram = sys / ((qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >> QMI_M1_TIMING_CLKDIV_LSB);
        unsigned flash = sys / ((qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB);
        snprintf(line, sizeof(line), "%s  CPU %u  PSRAM %u  FLASH %u MHz", BOARD_VARIANT_NAME, sys, psram, flash);
        text_center(FOOT_Y, line, C_DIM);
    }
    text_center(FOOT_Y + 11, "Port (c) 2026 Mikhail Matveev", C_TEXT);
    text_center(FOOT_Y + 22, "github.com/rh1tech/frank-quake2", C_HI);
}

/* the whole screen; error: the red title bar over the log */
static void screen(const char *error, const char *hint) {
    stars_stop();
    scr = q2_video_front();
    memset(scr, C_BG, W * H);
    palette();
    panel();
    title("QUAKE II");
    text_center(SUB_Y, "for RP2350  -  frank-quake2 " Q2_VERSION, C_TEXT);
    text_center(SUB_Y + 10, "Quake II (c) 1997 id Software", C_DIM);
    separator(SEP1_Y);
    separator(SEP2_Y);
    err_mode = error != NULL;
    log_y0 = SEP1_Y + 6;
    if (err_mode) {
        fill(TX0, log_y0, TX1, log_y0 + 12, C_ERRBAR);
        fill(TX0, log_y0, TX1, log_y0 + 1, C_ERR);
        text_center(log_y0 + 2, error, C_WHITE);
        log_y0 += 17;
    }
    log_rows = (SEP2_Y - 3 - log_y0) / 9;
    log_col = log_row = 0;
    footer(hint);
    stars_start(scr);
    q2_shell_active = true;
}

void q2_shell_welcome(void) {
    screen(NULL, NULL);
    shown_us = time_us_32();
}

void q2_shell_error(const char *title, const char *hint) {
    screen(title, hint);
}

bool q2_shell_active; /* on screen, the engine's palette held (q2_video.c) */

/* the engine is about to start: the welcome stays up min_ms at least, and
 * on through the engine's start, until its first frame */
void q2_shell_end(unsigned min_ms) {
    while (time_us_32() - shown_us < min_ms * 1000u) tight_loop_contents();
    scr = NULL; /* no more log lines on the screen */
}

void q2_shell_release(void) {
    stars_stop();
    scr = NULL;
    q2_shell_active = false;
}

/* ------------------------------------------------------------------ */
/* The log                                                             */
/* ------------------------------------------------------------------ */

static void log_scroll(void) {
    int top = log_y0 * W;
    memmove(scr + top, scr + top + 9 * W, (size_t)(9 * W * (log_rows - 1)));
    fill(TX0, log_y0 + 9 * (log_rows - 1), TX1, log_y0 + 9 * log_rows, C_PANEL);
    log_row = log_rows - 1;
}

static void log_newline(void) {
    log_col = 0;
    if (++log_row >= log_rows) log_scroll();
}

static void log_putc(char ch, uint8_t c) {
    if (ch == '\n') {
        log_newline();
        return;
    }
    if (ch == '\r') { /* the line is written again: what was there goes */
        log_col = 0;
        fill(TX0, log_y0 + log_row * 9, TX1, log_y0 + log_row * 9 + 9, C_PANEL);
        return;
    }
    if (log_col >= LOG_COLS) log_newline();
    int x = TX0 + log_col * 6, y = log_y0 + log_row * 9;
    fill(x, y, x + 6, y + 9, C_PANEL);
    glyph(x, y, ch, c);
    log_col++;
}

static void log_vprint(uint8_t c, const char *fmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    printf("%s", buf);
    if (!scr) return;
    /* word wrap */
    for (const char *s = buf; *s;) {
        if (*s != ' ' && *s != '\n' && *s != '\r') {
            int n = 0;
            while (s[n] && s[n] != ' ' && s[n] != '\n' && s[n] != '\r') n++;
            if (log_col > 0 && log_col + n > LOG_COLS) log_newline();
            while (n--) log_putc(*s++, c);
        } else {
            if (*s == ' ' && log_col >= LOG_COLS) log_newline();
            else log_putc(*s, c);
            s++;
        }
    }
}

void q2_con_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprint(C_TEXT, fmt, ap);
    va_end(ap);
}

void q2_con_highlight(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprint(C_HI, fmt, ap);
    va_end(ap);
}

void q2_con_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprint(C_ERR, fmt, ap);
    va_end(ap);
}
