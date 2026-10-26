/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PIO HDMI output with audio (see hdmi_pio.h). From the frank-msx / pico-nes
 * driver (TMDS by table, no CPU per pixel) and the HDMI framing of
 * shuichitakano's PicoDVI (pico-infonesPlus): a data island in every
 * line's hsync, after its preamble; the active video after its preamble and
 * guard band.
 *
 * The output state machine sends 10 instructions a pixel (clk_sys / 2 at
 * 504 MHz: 25.2 MHz). It is fed by DMA channel "conv", 16 bytes (two
 * pixels) at a time, from the address the address state machine makes of
 * each 16-bit index of the line: 0x2008_0000 | index << 4. Indices 0..255 are
 * the palette (SCRATCH_X, rewritten from a staged copy at vblank), 256..335
 * the HDMI entries (SCRATCH_Y): control symbols, the null islands and two
 * slots the IRQ encodes packets into.
 *
 * DMA channel "line" sends the indices of a line in 4 blocks, loaded by
 * channel "ctrl" from the line's block list: the front porch with the data
 * preamble, the island, the rest of the blanking, the pixels (a row buffer
 * shared by the line pair, or one blank index repeated). The IRQ at the end
 * of the third block points "ctrl" at the next line's list and makes the one
 * after: its list, its island, its row.
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
#include "hstx_packet.h"
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
#define DMA_CTRL 0
#define DMA_LINE 1
#define DMA_CONV_CTRL 2
#define DMA_CONV 3

#define ACTIVE_LINES 480
#define TOTAL_LINES 525
#define VSYNC_FIRST 490 /* 490, 491 */
#define ROW_PIXELS 320  /* indices: each two output pixels */

/* the HDMI entries (index 256 + E_...) */
enum {
    E_BLANK = 0,     /* [vsync] */
    E_HSYNC = 2,     /* [vsync] */
    E_PRE_DATA = 4,  /* [vsync] the preamble to a data island */
    E_PRE_VIDEO = 6, /* the preamble to the active video */
    E_GUARD_VIDEO = 7,
    E_NULL = 8,      /* [vsync] 18 entries each */
    E_SLOT = 44,     /* [2] 18 entries each */
    E_COUNT = 80,
};
#define ISLAND_ENTRIES 18 /* 36 pixels: 2 guard, 32 packet, 2 guard */
#define IDX(e) (256 + (e))

/* the two 4 KB banks, one after the other: one 64 KB window */
static uint32_t pal_tab[1024] __attribute__((section(".scratch_x.hdmi_pal"), aligned(4096)));
static uint32_t hdmi_tab[E_COUNT * 4] __attribute__((section(".scratch_y.hdmi_tab"), aligned(4096)));

typedef struct {
    const void *read;
    volatile void *write;
    uint32_t count;
    uint32_t ctrl;
} block_t;

/* the index lists (SRAM: read by DMA while the CPU works) */
static uint16_t head[2][8] __attribute__((section(".scratch_y.hdmi")));
static uint16_t isl_null[2][ISLAND_ENTRIES] __attribute__((section(".scratch_y.hdmi")));
static uint16_t isl_slot[2][ISLAND_ENTRIES] __attribute__((section(".scratch_y.hdmi")));
static uint16_t rest_active[54] __attribute__((section(".scratch_y.hdmi")));
static uint16_t rest_blank[2][54] __attribute__((section(".scratch_y.hdmi")));
static uint16_t blank_px[2] __attribute__((section(".scratch_y.hdmi")));
static uint16_t rows[2][ROW_PIXELS] __attribute__((section(".scratch_y.hdmi"), aligned(4)));
static block_t blocks[2][4] __attribute__((section(".scratch_y.hdmi"), aligned(16)));

static uint32_t ctrl_quiet, ctrl_irq; /* "line" ctrl words: incrementing reads */
static uint32_t ctrl_quiet_fixed;     /* the same, the read address fixed */

/* the palette staged by core 0, copied in at the next vblank */
static uint64_t stage[256];
static volatile int stage_pending;

static hdmi_pio_row_cb_t row_cb;
static psxv_vsync_cb_t vsync_cb;
static hdmi_pio_audio_source_t audio_source;

volatile uint32_t psxv_frame_count;
volatile uint32_t psxv_irq_us, psxv_irq_max_us, psxv_resyncs;

/* serialized lane symbols (see ser_lane): TERC4 by nibble, a lane each */
static uint64_t terc_lane[3][16];
static uint64_t guard12; /* the data island guard band on lanes 1 and 2 */

/* packets sent from the IRQ, made at init */
static hstx_packet_t pkt_acr, pkt_avi, pkt_aif;
static uint32_t audio_accum, audio_step; /* samples, 16.16: 735 a frame */
static uint32_t audio_packets;           /* mod 48: the B flag */
static uint32_t audio_prev;

extern uint8_t hstx_bch_table[256]; /* hstx_packet.c, in SRAM */

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

/* TERC4 (data islands) and the control symbols by CTL bits (c1 << 1 | c0),
 * bit 0 sent first, as in hstx_packet.c and PicoDVI */
static const uint16_t terc4[16] = {0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
                                   0x2cc, 0x139, 0x19c, 0x2c6, 0x28e, 0x271, 0x163, 0x2c3};
static const uint16_t ctl_sym[4] = {0x354, 0x0ab, 0x154, 0x2ab};
#define GUARD_DATA 0x133 /* lanes 1, 2 of a data island guard band */
#define GUARD_VIDEO_0 0x2cc
#define GUARD_VIDEO_1 0x133

/* One lane's 10-bit symbol as the output program shifts it: 10 steps of 6
 * bits (a 2-bit pair a lane, the second half of the word 2 bits on). Lane 0
 * is TMDS channel 0 (blue, the syncs), 1 green, 2 red. */
static uint64_t ser_lane(uint16_t sym, int lane) {
    int shift = lane == 1 ? 2 : (lane == 0) == (PIN_RGB_NOT_BGR != 0) ? 0 : 4;
    uint64_t out = 0;
    for (int i = 0; i < 10; i++) {
        out <<= 6;
        if (i == 5) out <<= 2;
        uint32_t b = (sym >> (9 - i)) & 1;
        b |= (b ^ 1) << 1;
        if (PIN_INVERT_DIFFPAIRS) b ^= 3;
        out |= (uint64_t)b << shift;
    }
    return out;
}

static uint64_t ser3(uint16_t l0, uint16_t l1, uint16_t l2) {
    return ser_lane(l0, 0) | ser_lane(l1, 1) | ser_lane(l2, 2);
}

static inline uint64_t *entry(int e) {
    return (uint64_t *)&hdmi_tab[e * 4];
}

static void set_hdmi_entry(int e, uint64_t v) {
    entry(e)[0] = v;
    entry(e)[1] = v;
}

/* a palette colour: the TMDS symbol, then its complement (DC balance) */
static inline void set_pal_entry(int i, uint64_t v) {
    uint64_t *c = (uint64_t *)&pal_tab[i * 4];
    c[0] = v;
    c[1] = v ^ 0x0003ffffffffffffull;
}

/* A data island into entries e..e+17, from a packet: lane 0 carries the
 * header and the syncs (hv: VSYNC << 1 | HSYNC levels), lanes 1 and 2 the
 * subpackets (laid out as hstx_packet.c does for the HSTX). */
static void __not_in_flash_func(encode_island)(int e, const hstx_packet_t *p, uint32_t hv) {
    uint64_t *d = entry(e);
    uint64_t g = terc_lane[0][0xc | hv] | guard12;
    d[0] = g;
    d[1] = g;
    d[34] = g;
    d[35] = g;
    for (int j = 0; j < 8; j++) {
        uint32_t v = p->subpacket[0][j] | p->subpacket[1][j] << 8 | p->subpacket[2][j] << 16 |
                     (uint32_t)p->subpacket[3][j] << 24;
        uint32_t x = (v ^ (v >> 7)) & 0x00aa00aa;
        v = v ^ x ^ (x << 7);
        x = (v ^ (v >> 14)) & 0x0000cccc;
        v = v ^ x ^ (x << 14);
        for (int k = 0; k < 4; k++) {
            int i = j * 4 + k;                   /* the packet's pixel */
            int sh = (k & 1) * 16 + (k >> 1) * 4; /* lane 1: 0, 16, 4, 20; lane 2: 8 more */
            uint32_t bit = (p->header[i >> 3] >> (i & 7)) & 1;
            uint32_t n0 = bit << 2 | hv | (i ? 8 : 0);
            d[2 + i] = terc_lane[0][n0] | terc_lane[1][(v >> sh) & 0xf] | terc_lane[2][(v >> (sh + 8)) & 0xf];
        }
    }
}

static inline uint32_t parity8(uint32_t v) {
    v ^= v >> 4;
    return (0x6996u >> (v & 0xf)) & 1;
}

/* the next audio packet: 4 samples interpolated up to the source's frame */
static void __not_in_flash_func(make_audio_packet)(hstx_packet_t *p) {
    uint32_t cur = audio_source();
    int pl = (int16_t)audio_prev, pr = (int16_t)(audio_prev >> 16);
    int dl = (int16_t)cur - pl, dr = (int16_t)(cur >> 16) - pr;
    audio_prev = cur;
    p->header[0] = 0x02;
    p->header[1] = 0x0f;
    p->header[2] = audio_packets == 0 ? 0x10 : 0; /* B: a new 192-sample block */
    if (++audio_packets == 48) audio_packets = 0;
    uint8_t v = hstx_bch_table[p->header[0]];
    v = hstx_bch_table[p->header[1] ^ v];
    p->header[3] = hstx_bch_table[p->header[2] ^ v];
    for (int k = 0; k < 4; k++) {
        uint8_t *d = p->subpacket[k];
        int l = pl + dl * (k + 1) / 4, r = pr + dr * (k + 1) / 4;
        d[0] = 0;
        d[1] = (uint8_t)l;
        d[2] = (uint8_t)(l >> 8);
        d[3] = 0;
        d[4] = (uint8_t)r;
        d[5] = (uint8_t)(r >> 8);
        d[6] = (uint8_t)(parity8(d[1] ^ d[2]) << 3 | parity8(d[4] ^ d[5]) << 7);
        uint8_t b = hstx_bch_table[0];
        for (int i = 1; i < 7; i++) b = hstx_bch_table[d[i] ^ b];
        d[7] = b;
    }
}

/* 8-bit pixels to 16-bit indices */
static void __not_in_flash_func(convert_row)(uint16_t *dst, const uint8_t *src) {
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < ROW_PIXELS / 4; i++) {
        uint32_t w = s[i];
        d[2 * i] = (w & 0xff) | (w & 0xff00) << 8;
        d[2 * i + 1] = (w >> 16 & 0xff) | (w >> 24) << 16;
    }
}

static inline void set_block(block_t *b, const void *read, uint32_t count, uint32_t ctrl) {
    b->read = read;
    b->write = &PIO_VIDEO->txf[SM_CONV];
    b->count = count;
    b->ctrl = ctrl;
}

/* the block list, island and row of a line, into slot (the lines' parity
 * would not do: 525 a frame) */
static void __not_in_flash_func(prepare_line)(uint32_t line, uint32_t slot) {
    block_t *b = blocks[slot];
    int blank = line >= ACTIVE_LINES;
    int vs = line >= VSYNC_FIRST && line < VSYNC_FIRST + 2;
    uint32_t hv = vs ? 0 : 2; /* in the island: HSYNC asserted (low) */

    /* the packet, if any: the info frames and the clock regeneration on
     * some blanking lines, audio as due */
    audio_accum += audio_step;
    const hstx_packet_t *pkt = NULL;
    hstx_packet_t audio;
    if (blank && !vs && line == ACTIVE_LINES + 1)
        pkt = &pkt_avi;
    else if (blank && !vs && line == ACTIVE_LINES + 3)
        pkt = &pkt_aif;
    else if (blank && !vs && (line & 3) == 0)
        pkt = &pkt_acr;
    else if (audio_source && audio_accum >= 4u << 16) {
        audio_accum -= 4u << 16;
        make_audio_packet(&audio);
        pkt = &audio;
    }
    if (audio_accum > 8u << 16) audio_accum = 8u << 16; /* never a burst */

    set_block(&b[0], head[vs], 8, ctrl_quiet);
    if (pkt) {
        encode_island(E_SLOT + slot * ISLAND_ENTRIES, pkt, hv);
        set_block(&b[1], isl_slot[slot], ISLAND_ENTRIES, ctrl_quiet);
    } else {
        set_block(&b[1], isl_null[vs], ISLAND_ENTRIES, ctrl_quiet);
    }
    if (!blank) {
        uint16_t *row = rows[(line >> 1) & 1];
        if (!(line & 1)) {
            const uint8_t *src = row_cb ? row_cb(line >> 1) : NULL;
            if (src)
                convert_row(row, src);
            else
                for (int i = 0; i < ROW_PIXELS; i++) row[i] = 0;
        }
        set_block(&b[2], rest_active, 54, ctrl_irq);
        set_block(&b[3], row, ROW_PIXELS, ctrl_quiet);
    } else {
        set_block(&b[2], rest_blank[vs], 54, ctrl_irq);
        set_block(&b[3], &blank_px[vs], ROW_PIXELS, ctrl_quiet_fixed);
    }
}

static void __not_in_flash_func(dma_irq)(void) {
    static uint32_t line, slot; /* being sent */
    uint32_t t0 = timer_hw->timerawl;
    dma_hw->ints0 = 1u << DMA_LINE;
    uint32_t next = line + 1 == TOTAL_LINES ? 0 : line + 1;
    dma_channel_set_read_addr(DMA_CTRL, blocks[slot ^ 1], false);
    uint32_t after = next + 1 == TOTAL_LINES ? 0 : next + 1;
    if (line == ACTIVE_LINES + 1) {
        psxv_frame_count++;
        if (stage_pending) { /* the palette staged by core 0 */
            for (int i = 0; i < 256; i++) set_pal_entry(i, stage[i]);
            stage_pending = 0;
        }
        if (vsync_cb) vsync_cb();
    }
    prepare_line(after, slot); /* this line's slot: all loaded */
    line = next;
    slot ^= 1;
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

void hdmi_pio_set_audio_source(hdmi_pio_audio_source_t source) {
    audio_prev = 0;
    audio_packets = 0;
    __dmb();
    audio_source = source;
}

void hdmi_pio_load_palette(const uint8_t *rgb, int stride) {
    while (stage_pending) tight_loop_contents();
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = rgb + i * stride;
        stage[i] = ser3(tmds_table[c[2]], tmds_table[c[1]], tmds_table[c[0]]);
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

static void fill16(uint16_t *p, uint16_t v, int n) {
    while (n--) *p++ = v;
}

static void make_tables(void) {
    for (int lane = 0; lane < 3; lane++)
        for (int n = 0; n < 16; n++) terc_lane[lane][n] = ser_lane(terc4[n], lane);
    guard12 = ser_lane(GUARD_DATA, 1) | ser_lane(GUARD_DATA, 2);

    for (int i = 0; i < 256; i++) set_pal_entry(i, ser3(tmds_table[0], tmds_table[0], tmds_table[0]));
    for (int vs = 0; vs < 2; vs++) {
        uint32_t v = vs ? 0 : 2; /* VSYNC level (low when asserted) */
        set_hdmi_entry(E_BLANK + vs, ser3(ctl_sym[v | 1], ctl_sym[0], ctl_sym[0]));
        set_hdmi_entry(E_HSYNC + vs, ser3(ctl_sym[v], ctl_sym[0], ctl_sym[0]));
        set_hdmi_entry(E_PRE_DATA + vs, ser3(ctl_sym[v | 1], ctl_sym[1], ctl_sym[1]));
        hstx_packet_t null;
        hstx_packet_set_null(&null);
        encode_island(E_NULL + vs * ISLAND_ENTRIES, &null, v);
    }
    set_hdmi_entry(E_PRE_VIDEO, ser3(ctl_sym[3], ctl_sym[1], ctl_sym[0]));
    set_hdmi_entry(E_GUARD_VIDEO, ser3(GUARD_VIDEO_0, GUARD_VIDEO_1, GUARD_VIDEO_0));

    for (int vs = 0; vs < 2; vs++) {
        fill16(head[vs], IDX(E_BLANK + vs), 4);
        fill16(head[vs] + 4, IDX(E_PRE_DATA + vs), 4);
        for (int k = 0; k < ISLAND_ENTRIES; k++) isl_null[vs][k] = IDX(E_NULL + vs * ISLAND_ENTRIES + k);
        fill16(rest_blank[vs], IDX(E_HSYNC + vs), 30);
        fill16(rest_blank[vs] + 30, IDX(E_BLANK + vs), 24);
        blank_px[vs] = IDX(E_BLANK + vs);
    }
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < ISLAND_ENTRIES; k++) isl_slot[s][k] = IDX(E_SLOT + s * ISLAND_ENTRIES + k);
    /* hsync 30 (60 px, after the island's 36), back porch 19 (38 px), the
     * video preamble 4 (8 px), the guard band 1 (2 px) */
    fill16(rest_active, IDX(E_HSYNC), 30);
    fill16(rest_active + 30, IDX(E_BLANK), 19);
    fill16(rest_active + 49, IDX(E_PRE_VIDEO), 4);
    rest_active[53] = IDX(E_GUARD_VIDEO);

    /* 44.1 kHz: ACR N 6272, CTS 28000 at 25.2 MHz; 640x480 is VIC 1 */
    hstx_packet_set_acr(&pkt_acr, 6272, 28000);
    hstx_packet_set_avi_infoframe(&pkt_avi, 1, 0);
    hstx_packet_set_audio_infoframe(&pkt_aif, 44100, 2, 16);
    audio_step = (735u << 16) / TOTAL_LINES;
}

void psxv_init(void) {
    hard_assert((uint32_t)hdmi_tab == (uint32_t)pal_tab + 4096);
    make_tables();

    for (int i = 0; i < 4; i++) dma_channel_claim(i);
    pio_sm_claim(PIO_VIDEO, SM_VIDEO);
    pio_sm_claim(PIO_VIDEO, SM_CONV);
#if HDMI_PIO_BASE_PIN >= 16
    pio_set_gpio_base(PIO_VIDEO, 16); /* 0 or 16 only: GPIO 16..47 */
#endif
    /* the address machine: the window | index << 4 */
    static uint16_t conv_instr[5];
    conv_instr[0] = pio_encode_pull(false, true);
    conv_instr[1] = pio_encode_in(pio_null, 4);
    conv_instr[2] = pio_encode_in(pio_osr, 12);
    conv_instr[3] = pio_encode_in(pio_x, 16);
    conv_instr[4] = pio_encode_push(false, true);
    static const struct pio_program conv_program = {.instructions = conv_instr, .length = 5, .origin = -1};
    /* the output: a pixel in 10 steps on the 3 data pairs, the clock by side-set */
    static const uint16_t video_instr[] = {
        0x7006, 0x7006, 0x7006, 0x7006, 0x7006, /* out pins, 6  side 2 */
        0x6806, 0x6806, 0x6806, 0x6806, 0x6806, /* out pins, 6  side 1 */
    };
    static const struct pio_program video_program = {.instructions = video_instr, .length = 10, .origin = -1};
    uint off_conv = pio_add_program(PIO_VIDEO, &conv_program);
    uint off_video = pio_add_program(PIO_VIDEO, &video_program);

    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_conv, off_conv + conv_program.length - 1);
    sm_config_set_in_shift(&cc, true, false, 32);
    pio_sm_init(PIO_VIDEO, SM_CONV, off_conv, &cc);
    pio_set_x(PIO_VIDEO, SM_CONV, (uint32_t)pal_tab >> 16);

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

    /* "line": the ctrl words its blocks load (16-bit indices, chained to
     * "ctrl"; the IRQ after the third) */
    dma_channel_config d = dma_channel_get_default_config(DMA_LINE);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_CONV, true));
    channel_config_set_chain_to(&d, DMA_CTRL);
    channel_config_set_irq_quiet(&d, true);
    ctrl_quiet = channel_config_get_ctrl_value(&d);
    channel_config_set_irq_quiet(&d, false);
    ctrl_irq = channel_config_get_ctrl_value(&d);
    channel_config_set_irq_quiet(&d, true);
    channel_config_set_read_increment(&d, false);
    ctrl_quiet_fixed = channel_config_get_ctrl_value(&d);

    /* "ctrl": a block (4 words) into the registers of "line", the last
     * write (ctrl_trig) starts it */
    d = dma_channel_get_default_config(DMA_CTRL);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, 4);
    dma_channel_configure(DMA_CTRL, &d, &dma_hw->ch[DMA_LINE].read_addr, blocks[0], 4, false);

    /* "conv": an index's 16 bytes to the output machine */
    d = dma_channel_get_default_config(DMA_CONV);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, DMA_CONV_CTRL);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_VIDEO, true));
    dma_channel_configure(DMA_CONV, &d, &PIO_VIDEO->txf[SM_VIDEO], pal_tab, 4, false);

    /* "conv ctrl": the address machine's output into "conv", which it starts */
    d = dma_channel_get_default_config(DMA_CONV_CTRL);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, DMA_CONV);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_VIDEO, SM_CONV, false));
    dma_channel_configure(DMA_CONV_CTRL, &d, &dma_hw->ch[DMA_CONV].read_addr, &PIO_VIDEO->rxf[SM_CONV], 1, false);

    prepare_line(0, 0);
    prepare_line(1, 1);
}

void psxv_start(void) {
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_priority(DMA_IRQ_0, 0);
    dma_channel_acknowledge_irq0(DMA_LINE);
    dma_channel_set_irq0_enabled(DMA_LINE, true);
    irq_set_enabled(DMA_IRQ_0, true);
    pio_sm_set_enabled(PIO_VIDEO, SM_CONV, true);
    pio_sm_set_enabled(PIO_VIDEO, SM_VIDEO, true);
    dma_channel_start(DMA_CONV_CTRL);
    dma_channel_start(DMA_CTRL);
}
