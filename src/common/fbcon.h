/*
 * frank-quake2 (from frank-castle) — minimal text console on a BGR555 framebuffer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FBCON_H
#define FBCON_H

#include <stdint.h>

/* PS1 VRAM pixel: 0bBBBBBGGGGGRRRRR */
#define BGR555(r, g, b) ((uint16_t)((((b) >> 3) << 10) | (((g) >> 3) << 5) | ((r) >> 3)))

typedef struct {
    uint16_t *pixels;
    int width, height, stride; /* stride in pixels */
    int col, row;              /* cursor, in 6x8 cells */
    uint16_t fg, bg;
} fbcon_t;

void fbcon_init(fbcon_t *c, uint16_t *pixels, int width, int height, int stride);
void fbcon_clear(fbcon_t *c);
void fbcon_fill_rect(fbcon_t *c, int x, int y, int w, int h, uint16_t color);
void fbcon_putc(fbcon_t *c, char ch);
void fbcon_puts(fbcon_t *c, const char *s);
void fbcon_printf(fbcon_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fbcon_goto(fbcon_t *c, int col, int row);

#endif
