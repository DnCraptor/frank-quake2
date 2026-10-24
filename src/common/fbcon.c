/*
 * frank-quake2 (from frank-castle) — minimal text console on a BGR555 framebuffer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fbcon.h"
#include "font6x8.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void fbcon_init(fbcon_t *c, uint16_t *pixels, int width, int height, int stride) {
    c->pixels = pixels;
    c->width = width;
    c->height = height;
    c->stride = stride;
    c->col = c->row = 0;
    c->fg = BGR555(255, 255, 255);
    c->bg = 0;
}

void fbcon_fill_rect(fbcon_t *c, int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width) w = c->width - x;
    if (y + h > c->height) h = c->height - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = c->pixels + (y + j) * c->stride + x;
        for (int i = 0; i < w; i++) p[i] = color;
    }
}

void fbcon_clear(fbcon_t *c) {
    fbcon_fill_rect(c, 0, 0, c->width, c->height, c->bg);
    c->col = c->row = 0;
}

void fbcon_goto(fbcon_t *c, int col, int row) {
    c->col = col;
    c->row = row;
}

static void scroll(fbcon_t *c) {
    int rows = c->height / 8;
    for (int y = 0; y < (rows - 1) * 8; y++)
        memcpy(c->pixels + y * c->stride, c->pixels + (y + 8) * c->stride, c->width * 2);
    fbcon_fill_rect(c, 0, (rows - 1) * 8, c->width, 8, c->bg);
    c->row = rows - 1;
}

void fbcon_putc(fbcon_t *c, char ch) {
    int cols = c->width / 6, rows = c->height / 8;
    if (ch == '\n') {
        c->col = 0;
        if (++c->row >= rows) scroll(c);
        return;
    }
    if (ch == '\r') {
        c->col = 0;
        return;
    }
    if (c->col >= cols) {
        c->col = 0;
        if (++c->row >= rows) scroll(c);
    }
    unsigned char u = (unsigned char)ch;
    const uint8_t *g = font6x8[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
    uint16_t *base = c->pixels + c->row * 8 * c->stride + c->col * 6;
    for (int y = 0; y < 8; y++) {
        uint16_t *p = base + y * c->stride;
        uint8_t bits = g[y];
        for (int x = 0; x < 6; x++) p[x] = (bits & (0x80 >> x)) ? c->fg : c->bg;
    }
    c->col++;
}

void fbcon_puts(fbcon_t *c, const char *s) {
    while (*s) fbcon_putc(c, *s++);
}

void fbcon_printf(fbcon_t *c, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fbcon_puts(c, buf);
}
