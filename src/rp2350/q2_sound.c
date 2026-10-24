/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sound output (SNDDMA_*): the engine mixes 16-bit stereo into a ring in
 * SRAM, ahead of the play position; the I2S driver's DMA IRQ (core 0) copies
 * the ring out and moves the play position, like the DMA of a PC sound card.
 * A ring frame (left, right int16) is exactly an I2S frame (L | R << 16).
 */

#include <string.h>

#include "audio.h"
#include "board_config.h"
#include "client/client.h"
#include "client/snd_loc.h"
#include "pico.h"

#ifndef Q2_SAMPLE_RATE
#define Q2_SAMPLE_RATE 11025
#endif
#define RING_FRAMES 4096 /* power of 2: 0.37 s at 11 kHz */
#define DMA_FRAMES  256

static uint32_t ring[RING_FRAMES] __attribute__((aligned(4)));
static volatile uint32_t play_frame;
static bool i2s_started;

static void __not_in_flash_func(i2s_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    uint32_t f = play_frame;
    for (uint32_t i = 0; i < frames; i++) buf[i] = ring[(f + i) & (RING_FRAMES - 1)];
    play_frame = f + frames;
}

static void start_i2s(void) {
    static i2s_config_t cfg;
    cfg.sample_freq = Q2_SAMPLE_RATE;
    cfg.channel_count = 2;
    cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
    cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    cfg.pio = pio0;
    cfg.dma_trans_count = DMA_FRAMES;
    cfg.volume = 0;
    i2s_init(&cfg);
    i2s_set_fill_callback(i2s_fill);
    i2s_start();
}

qboolean SNDDMA_Init(void) {
    memset(ring, 0, sizeof(ring));
    dma.samplebits = 16;
    dma.speed = Q2_SAMPLE_RATE;
    dma.channels = 2;
    dma.samples = RING_FRAMES * 2;
    dma.samplepos = 0;
    dma.submission_chunk = 1;
    dma.buffer = (byte *)ring;
    if (!i2s_started) {
        start_i2s();
        i2s_started = true;
    }
    Com_Printf("SNDDMA_Init: %d Hz 16-bit stereo, I2S\n", Q2_SAMPLE_RATE);
    return true;
}

int SNDDMA_GetDMAPos(void) {
    dma.samplepos = (int)((play_frame & (RING_FRAMES - 1)) * 2);
    return dma.samplepos;
}

void SNDDMA_Shutdown(void) {
    /* the I2S output keeps running (snd_restart calls SNDDMA_Init again) */
    memset(ring, 0, sizeof(ring));
}

void SNDDMA_BeginPainting(void) {
}

void SNDDMA_Submit(void) {
}
