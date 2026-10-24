/*
 * frank-castle — Castlevania: Symphony of the Night port for RP2350
 *
 * HSTX DVI/HDMI output for PS1-format (BGR555) scanlines.
 * Derived from pico_hdmi by fliperama86 (public domain / Unlicense).
 *
 * Output is always 640x480@60 (VIC 1, 25.2 MHz pixel clock). Each output
 * line is fed straight from a caller-owned buffer by DMA:
 *   - PSXV_LINE_DOUBLE: 320 PS1 pixels (u16), written to the HSTX FIFO as
 *     16-bit DMA transfers. The bus replicates the halfword into both halves
 *     of the FIFO word, so every pixel is emitted twice -> 640 pixels.
 *   - PSXV_LINE_NATIVE: 640 PS1 pixels (u16), 32-bit DMA, 1:1.
 *   - PSXV_LINE_DOUBLE_LM: like DOUBLE, but the HSTX itself sends the first
 *     PSXV_LM_OUT black pixels (a TMDS repeat command): the buffer holds the
 *     remaining 320 - PSXV_LM_OUT / 2 pixels. Frame buffers then need no
 *     left margin in memory.
 *
 * The line callback runs in the DMA IRQ during h-blank. It must only hand
 * back a pointer to an already prepared buffer.
 */

#ifndef PSX_VIDEO_H
#define PSX_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define PSXV_OUT_W 640
#define PSXV_OUT_H 480

typedef enum {
    PSXV_LINE_DOUBLE = 0, /* 320 source pixels, each shown twice */
    PSXV_LINE_NATIVE = 1, /* 640 source pixels */
    PSXV_LINE_DOUBLE_LM = 2, /* black left margin from the HSTX, then 288 doubled */
} psxv_line_mode_t;
#define PSXV_LM_OUT 64 /* output pixels of left margin in PSXV_LINE_DOUBLE_LM */

/* Shared between code built with and without -fno-short-enums: no enum
 * fields, every member has an explicit width. */
typedef struct {
    const uint16_t *pixels; /* BGR555 (PS1 VRAM format), bit 15 ignored */
    uint8_t mode;           /* psxv_line_mode_t */
} psxv_line_t;

/* out_line: 0..479 */
typedef void (*psxv_line_cb_t)(uint32_t out_line, psxv_line_t *line);
typedef void (*psxv_vsync_cb_t)(void);
typedef void (*psxv_task_fn)(void);

/* Call after the final sys clock is set. sys clock must be a multiple of
 * 126 MHz (252/378/504). Claims DMA channels 0 and 1 and DMA_IRQ_0. */
void psxv_init(void);

void psxv_set_line_callback(psxv_line_cb_t cb);
void psxv_set_vsync_callback(psxv_vsync_cb_t cb);

/* Pure DVI (no data islands). Default false = HDMI with null/audio islands. */
void psxv_set_dvi_mode(bool enabled);

/* HDMI audio over data islands. Samples are pushed with psxv_audio_push(). */
void psxv_set_audio_sample_rate(uint32_t sample_rate);

/* Starts HSTX + DMA with the IRQ bound to the calling core. Returns. */
void psxv_start(void);

/* Incremented once per frame at the start of vsync. */
extern volatile uint32_t psxv_frame_count;

#endif /* PSX_VIDEO_H */
