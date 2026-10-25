/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The flash asset image (models and decoded skins used in place, see
 * other/q_assets.c), built on the board from the paks on the SD card: on
 * the first start, when the paks change, or when /quake2/assets.rebuild
 * exists. An image made by tools/mkassets.py is kept as it is.
 *
 * Writing the flash: the QSPI bus is the flash's and the PSRAM's, so during
 * an erase or a program neither can be read. The SDK routines run from RAM
 * with this core's interrupts off; core 1 goes on with the HDMI interrupt,
 * whose code and data are all in SRAM. The ROM ends each operation with an
 * XIP cache flush that drops the lines (PSRAM ones included): the cache is
 * cleaned first so that no PSRAM write is lost. The data is assembled in
 * 4 KB sectors in SRAM (the renderer's ring buffer, unused at boot).
 * The ROM also puts the flash back at its boot timing: the board restarts
 * after a build.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/stdlib.h"
#include "q2_platform.h"
#include "other/q_assets.h"

#ifndef Q2_ASSETS_OFFSET
#define Q2_ASSETS_OFFSET 0x1C0000
#endif
#define ASSETS_SIZE (16 * 1024 * 1024 - Q2_ASSETS_OFFSET)
#define BLOCK 65536u
#define SECTOR 4096u

extern uint8_t q2_scratch4k[]; /* q2_fs.c */

typedef struct {
    int sector;          /* in the sector buffer, -1: none */
    int written;         /* sectors below this one are in the flash */
    uint32_t erased;     /* bytes erased from the start */
    uint32_t t0;
    int percent;
} writer_t;

/* Before each operation: the UART's pending output out first (its interrupt
 * code is partly in flash; a build without this locked up in the first
 * erase), the XIP cache cleaned (the ROM's flush drops dirty PSRAM lines). */
static void flash_op_prepare(void) {
    extern void uart_buffered_flush(void);
    uart_buffered_flush();
    xip_cache_clean_all();
}

static void flash_op_erase(uint32_t offs) {
    flash_op_prepare();
    uint32_t s = save_and_disable_interrupts();
    flash_range_erase(offs, BLOCK);
    restore_interrupts(s);
}

static void flash_op_program(uint32_t offs, const uint8_t *data, uint32_t size) {
    flash_op_prepare();
    uint32_t s = save_and_disable_interrupts();
    flash_range_program(offs, data, size);
    restore_interrupts(s);
}

static void erase_to(writer_t *w, uint32_t end) {
    while (w->erased < end) {
        flash_op_erase(Q2_ASSETS_OFFSET + w->erased);
        w->erased += BLOCK;
    }
}

static void flush(writer_t *w) {
    if (w->sector < 0) return;
    erase_to(w, (uint32_t)(w->sector + 1) * SECTOR);
    flash_op_program(Q2_ASSETS_OFFSET + (uint32_t)w->sector * SECTOR, q2_scratch4k, SECTOR);
    if (w->sector + 1 > w->written) w->written = w->sector + 1;
    w->sector = -1;
}

/* bytes in a sector already in the flash (the magic, written last over the
 * erased 0xFF of the header): program the page again, bits only cleared */
static void patch(uint32_t offset, const uint8_t *data, int size) {
    uint8_t *page = q2_scratch4k; /* the sector buffer is free: flushed */
    while (size > 0) {
        uint32_t base = offset & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
        int at = (int)(offset - base), n = (int)FLASH_PAGE_SIZE - at;
        if (n > size) n = size;
        memcpy(page, (const uint8_t *)(XIP_BASE + Q2_ASSETS_OFFSET + base), FLASH_PAGE_SIZE);
        memcpy(page + at, data, (size_t)n);
        flash_op_program(Q2_ASSETS_OFFSET + base, page, FLASH_PAGE_SIZE);
        offset += (uint32_t)n;
        data += n;
        size -= n;
    }
}

static int write_cb(void *ctx, int offset, const void *data, int size) {
    writer_t *w = ctx;
    const uint8_t *p = data;
    if (offset < 0 || (uint32_t)offset + (uint32_t)size > ASSETS_SIZE) return 0;
    if (offset / (int)SECTOR < w->written) {
        flush(w);
        patch((uint32_t)offset, p, size);
        return 1;
    }
    while (size > 0) {
        int sector = offset / (int)SECTOR, at = offset % (int)SECTOR;
        int n = (int)SECTOR - at;
        if (n > size) n = size;
        if (sector != w->sector) {
            flush(w);
            w->sector = sector;
            memset(q2_scratch4k, 0xff, SECTOR);
        }
        memcpy(q2_scratch4k + at, p, (size_t)n);
        offset += n;
        p += n;
        size -= n;
    }
    return 1;
}

static void progress_cb(void *ctx, int done, int total) {
    writer_t *w = ctx;
    int percent = total ? done * 100 / total : 100;
    if (percent == w->percent) return;
    w->percent = percent;
    q2_con_printf("\r  %3d %%  (%lu s)   ", percent, (unsigned long)((time_us_32() - w->t0) / 1000000));
}

/* Maps the image, building it first when needed. Returns the number of
 * assets (0: none). */
int q2_assets_init(const char *gamedir, const char *rebuild_flag) {
    const void *base = (const void *)(XIP_BASE + Q2_ASSETS_OFFSET);
    FILE *flag = fopen(rebuild_flag, "rb");
    bool forced = flag != NULL;
    if (flag) fclose(flag);

    unsigned key = Q_Assets_SourceKey(gamedir);
    if (!forced && Q_Assets_Current(base, ASSETS_SIZE, key))
        return Q_Assets_Init(base, ASSETS_SIZE);
    if (!key) return 0; /* no paks: the caller says so */

    q2_con_highlight("Building the models and skins in flash\n");
    q2_con_printf("from the paks (once, a few minutes)...\n");
    writer_t w = {.sector = -1, .percent = -1, .t0 = time_us_32()};
    assetwriter_t writer = {write_cb, progress_cb, &w};
    int n = Q_Assets_Build(gamedir, ASSETS_SIZE, &writer);
    flush(&w);
    if (n < 0) {
        q2_con_error("\nFailed: the game runs without them\n(big maps may run out of memory).\n");
        return 0;
    }
    if (forced) remove(rebuild_flag);
    q2_con_printf("\nDone: %d files in %lu s. Restarting...\n", n,
                  (unsigned long)((time_us_32() - w.t0) / 1000000));
    sleep_ms(1500);
    platform_reboot(false); /* the flash is back at its boot timing */
}
