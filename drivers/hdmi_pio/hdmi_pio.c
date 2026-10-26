/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PIO HDMI output (see hdmi_pio.h), from frank-msx drivers/HDMI.c.
 *
 * A line buffer holds 400 indices, each two output pixels (800 = the line
 * with its blanking): 48 of hsync, 24 of back porch, the 320 source pixels,
 * 8 of front porch. DMA channel "line" feeds them to the address state
 * machine, which turns each into the address of its 16 bytes in the TMDS
 * table; channel "conv" copies those to the output state machine (10
 * instructions a pixel at clk_sys / (sys / 252 MHz): 25.2 MHz). The IRQ at
 * the start of each line prepares the other buffer, every second line: the
 * same buffer goes out twice, the line doubling.
 */

#include "hdmi_pio.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico.h"

#ifndef HDMI_PIO_BASE_PIN
#define HDMI_PIO_BASE_PIN 32
#endif
/* Z0 wires the data lanes first, then the clock, BGR, the pairs not
 * inverted; the others (frank-msx boards) clock first, RGB, inverted */
#ifndef HDMI_PIO_Z0_LAYOUT
#define HDMI_PIO_Z0_LAYOUT 1
#endif
#if HDMI_PIO_Z0_LAYOUT
#define PIN_RGB_NOT_BGR 0
#define PIN_INVERT_DIFFPAIRS 0
#define PIN_DATA (HDMI_PIO_BASE_PIN)
#define PIN_CLK (HDMI_PIO_BASE_PIN + 6)
#else
#define PIN_RGB_NOT_BGR 1
#define PIN_INVERT_DIFFPAIRS 1
#define PIN_DATA (HDMI_PIO_BASE_PIN + 2)
#define PIN_CLK (HDMI_PIO_BASE_PIN)
#endif

#define PIO_VIDEO pio0
#define SM_VIDEO 0
#define SM_CONV 1
#define DMA_LINE_CTRL 0
#define DMA_LINE 1
#define DMA_CONV_CTRL 2
#define DMA_CONV 3

#define CTRL_INX 251 /* 251..254: the sync words */
#define LINE_BYTES 400
#define ACTIVE_LINES 480
#define TOTAL_LINES 525

/* the TMDS table, 16 bytes an index, 4 KB aligned (the address machine
 * makes base | index << 4): all of SCRATCH_X, a bank of its own */
static uint32_t conv[1024] __attribute__((section(".scratch_x.hdmi_conv"), aligned(4096)));
static uint32_t line_buf[2][LINE_BYTES / 4];
static uint32_t *line_addr[2]; /* read by the "line ctrl" channel */
static uint8_t subst[256];     /* the source index to send: 251..254 replaced */

/* staged by core 0, copied in at the next vblank */
static uint64_t stage[256];
static uint8_t stage_subst[4];
static volatile int stage_pending;

static hdmi_pio_row_cb_t row_cb;
static psxv_vsync_cb_t vsync_cb;

volatile uint32_t psxv_frame_count;
volatile uint32_t psxv_irq_us, psxv_irq_max_us, psxv_resyncs;

static const uint16_t tmds_table[256] = {
    0x100, 0x1ff, 0x1fe, 0x101, 0x1fc, 0x103, 0x102, 0x1fd, 0x1f8, 0x107, 0x106, 0x1f9, 0x104, 0x1fb, 0x1fa, 0x105,
    0x1f0, 0x10f, 0x10e, 0x1f1, 0x10c, 0x1f3, 0x1f2, 0x10d, 0x108, 0x1f7, 0x1f6, 0x109, 0x1f4, 0x10b, 0x2a0, 0x25f,
    0x1e0, 0x11f, 0x11e, 0x1e1, 0x11c, 0x1e3, 0x1e2, 0x11d, 0x118, 0x1e7, 0x1e6, 0x119, 0x1e4, 0x11b, 0x2b0, 0x24f,
    0x110, 0x1ef, 0x1ee, 0x111, 0x1ec, 0x113, 0x2b8, 0x247, 0x1e8, 0x117, 0x2bc, 0x243, 0x2be, 0x241, 0x240, 0x2bf,
    0x1c0, 0x13f, 0x13e, 0x1c1, 0x13c, 0x1c3, 0x1c2, 0x13d, 0x138, 0x1c7, 0x1c6, 0x139, 0x1c4, 0x13b, 0x290, 0x26f,
    0x130, 0x1cf, 0x1ce, 0x131, 0x1cc, 0x133, 0x298, 0x267, 0x1c8, 0x137, 0x29c, 0x263, 0x29e, 0x261, 0x260, 0x29f,
    0x120, 0x1df, 0x1de, 0x121, 0x1dc, 0x123, 0x288, 0x277, 0x1d8, 0x127, 0x28c, 0x273, 0x28e, 0x271, 0x270, 0x28f,
    0x1d0, 0x12f, 0x284, 0x27b, 0x286, 0x279, 0x278, 0x287, 0x282, 0x27d, 0x27c, 0x283, 0x27e, 0x281, 0x280, 0x27f,
    0x180, 0x17f, 0x17e, 0x181, 0x17c, 0x183, 0x182, 0x17d, 0x178, 0x187, 0x186, 0x179, 0x184, 0x17b, 0x2d0, 0x22f,
    0x170, 0x18f, 0x18e, 0x171, 0x18c, 0x173, 0x2d8, 0x227, 0x188, 0x177, 0x2dc, 0x223, 0x2de, 0x221, 0x220, 0x2df,
    0x160, 0x19f, 0x19e, 0x161, 0x19c, 0x163, 0x2c8, 0x237, 0x198, 0x167, 0x2cc, 0x233, 0x2ce, 0x231, 0x230, 0x2cf,
    0x190, 0x16f, 0x2c4, 0x23b, 0x2c6, 0x239, 0x238, 0x2c7, 0x2c2, 0x23d, 0x23c, 0x2c3, 0x23e, 0x2c1, 0x2c0, 0x23f,
    0x140, 0x1bf, 0x1be, 0x141, 0x1bc, 0x143, 0x2e8, 0x217, 0x1b8, 0x147, 0x2ec, 0x213, 0x2ee, 0x211, 0x210, 0x2ef,
    0x1b0, 0x14f, 0x2e4, 0x21b, 0x2e6, 0x219, 0x218, 0x2e7, 0x2e2, 0x21d, 0x21c, 0x2e3, 0x21e, 0x2e1, 0x2e0, 0x21f,
    0x1a0, 0x15f, 0x2f4, 0x20b, 0x2f6, 0x209, 0x208, 0x2f7, 0x2f2, 0x20d, 0x20c, 0x2f3, 0x20e, 0x2f1, 0x2f0, 0x20f,
    0x2fa, 0x205, 0x204, 0x2fb, 0x206, 0x2f9, 0x2f8, 0x207, 0x202, 0x2fd, 0x2fc, 0x203, 0x2fe, 0x201, 0x200, 0x2ff,
};

/* the address machine: index << 4 | base (in x) */
static const uint16_t conv_program_instr[] = {
    0x80a0, /* pull block */
    0x40e8, /* in osr, 8 */
    0x4034, /* in x, 20 */
    0x8020, /* push block */
};
static const struct pio_program conv_program = {.instructions = conv_program_instr, .length = 4, .origin = -1};

/* the output: 10 bits a pixel on the 3 data pairs, the clock pair by side-set */
static const uint16_t video_program_instr[] = {
    0x7006, 0x7006, 0x7006, 0x7006, 0x7006, /* out pins, 6  side 2 */
    0x6806, 0x6806, 0x6806, 0x6806, 0x6806, /* out pins, 6  side 1 */
};
static const struct pio_program video_program = {.instructions = video_program_instr, .length = 10, .origin = -1};

/* the 10-bit symbols of the three lanes, serialized as the output program
 * shifts them: 2 bits (the pair) a lane, 6 a step, 10 steps */
static uint64_t ser_diff(uint16_t r, uint16_t g, uint16_t b) {
    uint64_t out = 0;
    for (int i = 0; i < 10; i++) {
        out <<= 6;
        if (i == 5) out <<= 2;
        uint8_t br = (r >> (9 - i)) & 1, bg = (g >> (9 - i)) & 1, bb = (b >> (9 - i)) & 1;
        br |= (br ^ 1) << 1;
        bg |= (bg ^ 1) << 1;
        bb |= (bb ^ 1) << 1;
        if (PIN_INVERT_DIFFPAIRS) {
            br ^= 3;
            bg ^= 3;
            bb ^= 3;
        }
        out |= PIN_RGB_NOT_BGR ? (uint64_t)(br << 4 | bg << 2 | bb) : (uint64_t)(bb << 4 | bg << 2 | br);
    }
    return out;
}

static inline void set_entry(int i, uint64_t v) {
    uint64_t *c = (uint64_t *)conv;
    c[i * 2] = v;
    c[i * 2 + 1] = v ^ 0x0003ffffffffffffull; /* the DC balance's other half */
}

static void __not_in_flash_func(fill)(uint8_t *p, uint8_t v, int n) {
    while (n--) *p++ = v;
}

static void __not_in_flash_func(dma_irq)(void) {
    static uint32_t inx;
    static uint32_t line;
    uint32_t t0 = timer_hw->timerawl;

    dma_hw->ints0 = 1u << DMA_LINE_CTRL;
    dma_channel_set_read_addr(DMA_LINE_CTRL, &line_addr[inx & 1], false);
    line = line >= TOTAL_LINES - 1 ? 0 : line + 1;
    if (!(line & 1)) return; /* the buffer goes out again */
    inx++;
    uint8_t *buf = (uint8_t *)line_buf[inx & 1];

    if (line < ACTIVE_LINES) {
        const uint8_t *src = row_cb ? row_cb(line >> 1) : NULL;
        uint32_t *d = (uint32_t *)(buf + 72);
        if (src) {
            const uint32_t *s = (const uint32_t *)src;
            for (int i = 0; i < 80; i++) {
                uint32_t w = s[i];
                d[i] = subst[w & 0xff] | subst[(w >> 8) & 0xff] << 8 | subst[(w >> 16) & 0xff] << 16 |
                       (uint32_t)subst[w >> 24] << 24;
            }
        } else {
            for (int i = 0; i < 80; i++) d[i] = 0;
        }
        fill(buf + 48, CTRL_INX, 24);
        fill(buf, CTRL_INX + 1, 48);
        fill(buf + 392, CTRL_INX, 8);
    } else {
        if (line == ACTIVE_LINES + 1) {
            psxv_frame_count++;
            if (stage_pending) { /* the palette staged by core 0 */
                for (int i = 0; i < 256; i++)
                    if (i < CTRL_INX || i > CTRL_INX + 3) set_entry(i, stage[i]);
                for (int i = 0; i < 4; i++) subst[CTRL_INX + i] = stage_subst[i];
                stage_pending = 0;
            }
            if (vsync_cb) vsync_cb();
        }
        if (line >= 490 && line < 492) { /* vsync */
            fill(buf + 48, CTRL_INX + 2, 352);
            fill(buf, CTRL_INX + 3, 48);
        } else {
            fill(buf + 48, CTRL_INX, 352);
            fill(buf, CTRL_INX + 1, 48);
        }
    }
    uint32_t dt = timer_hw->timerawl - t0;
    psxv_irq_us += dt;
    if (dt > psxv_irq_max_us) psxv_irq_max_us = dt;
}

void hdmi_pio_set_row_callback(hdmi_pio_row_cb_t cb) {
    row_cb = cb;
}

void psxv_set_vsync_callback(psxv_vsync_cb_t cb) {
    vsync_cb = cb;
}

static uint32_t dist2(const uint8_t *a, const uint8_t *b) {
    int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

void hdmi_pio_load_palette(const uint8_t *rgb, int stride) {
    while (stage_pending) tight_loop_contents();
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = rgb + i * stride;
        stage[i] = ser_diff(tmds_table[c[0]], tmds_table[c[1]], tmds_table[c[2]]);
    }
    /* the sync indices' colours: shown as the nearest other one */
    for (int k = 0; k < 4; k++) {
        const uint8_t *t = rgb + (CTRL_INX + k) * stride;
        uint32_t best_d = 0xffffffffu;
        uint8_t best = 0;
        for (int j = 0; j < 256; j++) {
            if (j >= CTRL_INX && j <= CTRL_INX + 3) continue;
            uint32_t d = dist2(t, rgb + j * stride);
            if (d < best_d) {
                best_d = d;
                best = (uint8_t)j;
            }
        }
        stage_subst[k] = best;
    }
}

void hdmi_pio_show_palette(void) {
    __dmb();
    stage_pending = 1;
}

int hdmi_pio_palette_pending(void) {
    return stage_pending;
}

static void pio_set_x(PIO pio, uint sm, uint32_t v) {
    for (int i = 0; i < 8; i++) { /* right shifts: the low nibble first */
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, (v >> (i * 4)) & 0xf));
        pio_sm_exec(pio, sm, pio_encode_in(pio_x, 4));
    }
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_isr));
}

void psxv_init(void) {
    for (int i = 0; i < 256; i++) subst[i] = (uint8_t)i;
    for (int i = 0; i < 256; i++)
        if (i < CTRL_INX || i > CTRL_INX + 3) set_entry(i, ser_diff(tmds_table[0], tmds_table[0], tmds_table[0]));
    /* the sync words: blank, hsync, vsync, both */
    const uint16_t b0 = 0x354, b1 = 0x0ab, b2 = 0x154, b3 = 0x2ab;
    set_entry(CTRL_INX + 0, ser_diff(b0, b0, b3));
    set_entry(CTRL_INX + 1, ser_diff(b0, b0, b2));
    set_entry(CTRL_INX + 2, ser_diff(b0, b0, b1));
    set_entry(CTRL_INX + 3, ser_diff(b0, b0, b0));
    /* the pair stays as set: [i*2+1] is not the complement for these */
    uint64_t *c = (uint64_t *)conv;
    for (int k = 0; k < 4; k++) c[(CTRL_INX + k) * 2 + 1] = c[(CTRL_INX + k) * 2];
    for (int i = 0; i < 2; i++) {
        memset(line_buf[i], CTRL_INX, LINE_BYTES);
        line_addr[i] = line_buf[i];
    }

    for (int i = 0; i < 4; i++) dma_channel_claim(i);
    pio_sm_claim(PIO_VIDEO, SM_VIDEO);
    pio_sm_claim(PIO_VIDEO, SM_CONV);
#if HDMI_PIO_BASE_PIN >= 16
    pio_set_gpio_base(PIO_VIDEO, 16); /* 0 or 16 only: GPIO 16..47 */
#endif
    uint off_conv = pio_add_program(PIO_VIDEO, &conv_program);
    uint off_video = pio_add_program(PIO_VIDEO, &video_program);

    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_conv, off_conv + conv_program.length - 1);
    sm_config_set_in_shift(&cc, true, false, 32);
    pio_sm_init(PIO_VIDEO, SM_CONV, off_conv, &cc);
    pio_set_x(PIO_VIDEO, SM_CONV, (uint32_t)conv >> 12);

    cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_video, off_video + video_program.length - 1);
    sm_config_set_sideset_pins(&cc, PIN_CLK);
    sm_config_set_sideset(&cc, 2, false, false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, PIN_CLK + i);
        gpio_set_drive_strength(PIN_CLK + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(PIN_CLK + i, GPIO_SLEW_RATE_FAST);
    }
    for (int i = 0; i < 6; i++) {
        pio_gpio_init(PIO_VIDEO, PIN_DATA + i);
        gpio_set_drive_strength(PIN_DATA + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(PIN_DATA + i, GPIO_SLEW_RATE_FAST);
    }
    uint64_t clk_mask = (uint64_t)3u << PIN_CLK;
    pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_VIDEO, clk_mask, clk_mask);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_VIDEO, clk_mask, clk_mask);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_VIDEO, PIN_DATA, 6, true);
    sm_config_set_out_pins(&cc, PIN_DATA, 6);
    sm_config_set_out_shift(&cc, true, true, 30);
    sm_config_set_fifo_join(&cc, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&cc, (float)clock_get_hz(clk_sys) / 252000000.0f);
    pio_sm_init(PIO_VIDEO, SM_VIDEO, off_video, &cc);

    /* line: the buffer's indices to the address machine, then line ctrl */
    dma_channel_config d = dma_channel_get_default_config(DMA_LINE);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_8);
    channel_config_set_chain_to(&d, DMA_LINE_CTRL);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_CONV, true));
    dma_channel_configure(DMA_LINE, &d, &PIO_VIDEO->txf[SM_CONV], line_buf[0], LINE_BYTES, false);

    /* line ctrl: the next buffer's address into "line", which it starts */
    d = dma_channel_get_default_config(DMA_LINE_CTRL);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, DMA_LINE);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    dma_channel_configure(DMA_LINE_CTRL, &d, &dma_hw->ch[DMA_LINE].read_addr, &line_addr[0], 1, false);

    /* conv: an index's 16 bytes to the output machine */
    d = dma_channel_get_default_config(DMA_CONV);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, DMA_CONV_CTRL);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_VIDEO, true));
    dma_channel_configure(DMA_CONV, &d, &PIO_VIDEO->txf[SM_VIDEO], conv, 4, false);

    /* conv ctrl: the address machine's output into "conv", which it starts */
    d = dma_channel_get_default_config(DMA_CONV_CTRL);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, DMA_CONV);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_CONV, false));
    dma_channel_configure(DMA_CONV_CTRL, &d, &dma_hw->ch[DMA_CONV].read_addr, &PIO_VIDEO->rxf[SM_CONV], 1, false);
}

void psxv_start(void) {
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_priority(DMA_IRQ_0, 0);
    dma_channel_acknowledge_irq0(DMA_LINE_CTRL);
    dma_channel_set_irq0_enabled(DMA_LINE_CTRL, true);
    irq_set_enabled(DMA_IRQ_0, true);
    pio_sm_set_enabled(PIO_VIDEO, SM_CONV, true);
    pio_sm_set_enabled(PIO_VIDEO, SM_VIDEO, true);
    dma_channel_start(DMA_CONV_CTRL);
    dma_channel_start(DMA_LINE_CTRL);
}
