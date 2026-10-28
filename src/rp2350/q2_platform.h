/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The RP2350 side of the port (src/rp2350/q2_*.c). */

#ifndef Q2_PLATFORM_H
#define Q2_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define Q2_W 320
#define Q2_H 240

/* q2_malloc.c */
void q2_heap_init(void);
void q2_heap_report(void);
void *q2_malloc(size_t n);
void q2_free(void *p);
void *q2_realloc(void *p, size_t n);


/* q2_video.c */
void q2_video_init(void);
uint8_t *q2_video_backbuffer(void);
uint8_t *q2_video_present(void);
void q2_video_set_palette(const uint8_t *rgb, int stride);
void q2_video_set_palette_now(const uint8_t *rgb, int stride);
void q2_video_wait_vsync(void);
void q2_video_wait_back(void);
extern volatile uint32_t q2_vsync_count;
uint8_t *q2_video_front(void);

/* q2_shell.c: the welcome and error screens, the boot log */
void q2_shell_welcome(void);
void q2_shell_error(const char *title, const char *hint);
void q2_shell_end(unsigned min_ms);
void q2_shell_release(void);
extern bool q2_shell_active;
void q2_con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void q2_con_highlight(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void q2_con_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* q2_input.c */
void q2_input_init(void);
void q2_input_poll(void);

/* q2_port.c */
void q2_core1_main(void);
extern void (*volatile q2_core1_idle)(void);
void __attribute__((noreturn)) q2_halt(const char *msg);

#endif /* Q2_PLATFORM_H */
