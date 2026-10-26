/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PIO HDMI (DVI) output for boards whose HDMI pins are not the HSTX ones
 * (Z0: Waveshare RP2350-PiZero, GPIO 32..39). From the frank-msx / pico-nes
 * driver: DMA turns each 8-bit pixel into its TMDS words through a 4 KB
 * table, a PIO shifts them out. 640x480@60, a 320x240 8-bit source doubled
 * both ways. Indices 251..254 are the sync words: pixels of those colours
 * are shown in the nearest other colour.
 *
 * The same entry points as the HSTX driver (psx_video.h) where they apply.
 */

#ifndef HDMI_PIO_H
#define HDMI_PIO_H

#include <stdint.h>

/* source row 0..239 for the next pair of output lines, NULL: black. Runs in
 * the DMA IRQ: it and what it returns must be in SRAM. */
typedef const uint8_t *(*hdmi_pio_row_cb_t)(uint32_t row);
typedef void (*psxv_vsync_cb_t)(void);

/* after the final sys clock is set (252 MHz multiple); claims DMA channels
 * 0..3, two state machines of pio0, DMA_IRQ_0 (taken by psxv_start) */
void psxv_init(void);
void hdmi_pio_set_row_callback(hdmi_pio_row_cb_t cb);
void psxv_set_vsync_callback(psxv_vsync_cb_t cb);

/* starts the output, the IRQ bound to the calling core */
void psxv_start(void);

/* 256 RGB triplets, stride bytes apart, into the staging table; shown from
 * the next vblank after hdmi_pio_show_palette(). Waits for a pending one. */
void hdmi_pio_load_palette(const uint8_t *rgb, int stride);
void hdmi_pio_show_palette(void);
int hdmi_pio_palette_pending(void);

extern volatile uint32_t psxv_frame_count;
extern volatile uint32_t psxv_irq_us, psxv_irq_max_us, psxv_resyncs;

#endif /* HDMI_PIO_H */
