/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sound output (SNDDMA_*): the engine mixes 16-bit stereo into a ring in
 * PSRAM, ahead of the play position, which the output moves like the DMA of
 * a PC sound card. A ring frame (left, right int16) is L | R << 16.
 *
 * AUDIO_TYPE I2S: the I2S driver's DMA IRQ (core 0) copies the ring out.
 *
 * AUDIO_TYPE HDMI: the HSTX IRQ (core 1) takes a frame for each audio packet,
 * 11025 a second (44.1 kHz, 4 samples interpolated to a packet, see
 * drivers/hdmi/hstx_data_island_queue.c), so HDMI is the clock. That IRQ
 * must never wait on PSRAM (a late line costs a resync of the picture): a
 * timer on core 0 copies the frames about to play into a small SRAM ring.
 */

#include <string.h>

#if Q2_AUDIO_HDMI
#include "hardware/sync.h"
#if Q2_VIDEO_PIO
#include "hdmi_pio.h"
#define hstx_di_queue_set_audio_source hdmi_pio_set_audio_source
#else
#include "hstx_data_island_queue.h"
#endif
#include "pico/time.h"
#else
#include "audio.h"
#endif
#include "board_config.h"
#include "client/client.h"
#include "client/snd_loc.h"
#include "pico.h"

#ifndef Q2_SAMPLE_RATE
#define Q2_SAMPLE_RATE 11025
#endif
#define RING_FRAMES 4096 /* power of 2: 0.37 s at 11 kHz */
#define DMA_FRAMES  256

/* in PSRAM: only core 0 reads or writes it (the mixer, the output's IRQ) */
static uint32_t ring[RING_FRAMES] __attribute__((aligned(4), section(".psram_bss.sound")));
static volatile uint32_t play_frame;
static bool output_started;

#if Q2_AUDIO_HDMI

#define HDMI_RING  512 /* SRAM, power of 2 */
#define HDMI_AHEAD 384 /* frames copied ahead of the play position: 35 ms */
#define HDMI_COPY_US 5000
#define HDMI_GAIN_SHIFT 1 /* +6 dB: the mix peaks near -12 dBFS, quiet on a TV */

static uint32_t hdmi_ring[HDMI_RING];
static volatile uint32_t copied_frame; /* the frames before it are in hdmi_ring */
static repeating_timer_t copy_timer;

/* core 1, HSTX IRQ: the next frame; silence when the copy is behind */
static uint32_t __not_in_flash_func(hdmi_source)(void) {
    uint32_t f = play_frame;
    play_frame = f + 1;
    return (int32_t)(copied_frame - f) > 0 ? hdmi_ring[f & (HDMI_RING - 1)] : 0;
}

static inline int16_t hdmi_gain(int v) {
    v <<= HDMI_GAIN_SHIFT;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* core 0, timer IRQ: the frames up to HDMI_AHEAD past the play position.
 * Their slots held frames played at least HDMI_RING - HDMI_AHEAD ago. */
static bool hdmi_copy(repeating_timer_t *t) {
    (void)t;
    uint32_t p = play_frame, c = copied_frame;
    if ((int32_t)(c - p) < 0) c = p; /* stalled (flash writes): start again here */
    for (uint32_t end = p + HDMI_AHEAD; (int32_t)(end - c) > 0; c++) {
        uint32_t lr = ring[c & (RING_FRAMES - 1)];
        uint16_t l = (uint16_t)hdmi_gain((int16_t)lr), r = (uint16_t)hdmi_gain((int16_t)(lr >> 16));
        hdmi_ring[c & (HDMI_RING - 1)] = l | (uint32_t)r << 16;
    }
    __dmb();
    copied_frame = c;
    return true;
}

static void start_output(void) {
    copied_frame = play_frame;
    add_repeating_timer_us(-HDMI_COPY_US, hdmi_copy, NULL, &copy_timer);
    hstx_di_queue_set_audio_source(hdmi_source);
}

#define OUTPUT_NAME "HDMI"

#else /* I2S */

/* Keep the mixer at its native rate, but run the physical I2S DAC at
 * 44.1 kHz. At 11.025 kHz its LRCLK/image frequencies are audible on
 * some boards. Linear interpolation suppresses sample-hold images. */
#define Q2_I2S_RATE 44100u
#if (Q2_I2S_RATE % Q2_SAMPLE_RATE) != 0
#error "I2S output rate must be an integer multiple of the mixer rate"
#endif
#define Q2_I2S_FACTOR (Q2_I2S_RATE / Q2_SAMPLE_RATE)
static unsigned i2s_phase;
static uint32_t i2s_cur, i2s_next;
static bool i2s_primed;

static void __not_in_flash_func(i2s_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    uint32_t f = play_frame;
    if (!i2s_primed) {
        i2s_cur = ring[f & (RING_FRAMES - 1)];
        i2s_next = ring[(f + 1) & (RING_FRAMES - 1)];
        i2s_primed = true;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t l0 = (int16_t)i2s_cur, r0 = (int16_t)(i2s_cur >> 16);
        int32_t l1 = (int16_t)i2s_next, r1 = (int16_t)(i2s_next >> 16);
        int32_t l = l0 + (l1 - l0) * (int32_t)i2s_phase / Q2_I2S_FACTOR;
        int32_t r = r0 + (r1 - r0) * (int32_t)i2s_phase / Q2_I2S_FACTOR;
        /* PIO shifts MSB first: WS=0 is high 16 bits (left channel). */
        buf[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
        if (++i2s_phase == Q2_I2S_FACTOR) {
            i2s_phase = 0;
            ++f;
            i2s_cur = i2s_next;
            i2s_next = ring[(f + 1) & (RING_FRAMES - 1)];
        }
    }
    play_frame = f; /* units are mixer frames, not 44.1 kHz output frames */
}

static void start_output(void) {
    static i2s_config_t cfg;
    cfg.sample_freq = Q2_I2S_RATE;
    cfg.channel_count = 2;
    cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
    cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    cfg.pio = Q2_I2S_PIO; /* pio0 on M2, pio1 on Z0 (pio0 is its video) */
    cfg.dma_trans_count = DMA_FRAMES;
    cfg.volume = 0;
    i2s_init(&cfg);
    i2s_set_fill_callback(i2s_fill);
    i2s_start();
}

#define OUTPUT_NAME "I2S"

#endif

qboolean SNDDMA_Init(void) {
    memset(ring, 0, sizeof(ring));
    dma.samplebits = 16;
    dma.speed = Q2_SAMPLE_RATE;
    dma.channels = 2;
    dma.samples = RING_FRAMES * 2;
    dma.samplepos = 0;
    dma.submission_chunk = 1;
    dma.buffer = (byte *)ring;
    if (!output_started) {
        start_output();
        output_started = true;
    }
    Com_Printf("SNDDMA_Init: %d Hz 16-bit stereo, " OUTPUT_NAME "\n", Q2_SAMPLE_RATE);
    return true;
}

int SNDDMA_GetDMAPos(void) {
    dma.samplepos = (int)((play_frame & (RING_FRAMES - 1)) * 2);
    return dma.samplepos;
}

void SNDDMA_Shutdown(void) {
    /* the output keeps running (snd_restart calls SNDDMA_Init again) */
    memset(ring, 0, sizeof(ring));
}

void SNDDMA_BeginPainting(void) {
}

void SNDDMA_Submit(void) {
}
