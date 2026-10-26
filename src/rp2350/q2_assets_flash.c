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
 * The image takes the flash from Q2_ASSETS_OFFSET to its real end (8 MB
 * boards exist: writing past the end wrapped around and erased the
 * firmware, the build stopping at 37 %); Q_Assets_Build keeps the smaller
 * files when they do not all fit.
 *
 * The SDK's XIP restart sets the flash back to its boot timing: ours is put
 * back before any code runs from the flash again (flash_op). The board
 * restarts after a build anyway, the engine starting from a clean state.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/stdlib.h"
#include "q2_platform.h"
#include "other/q_assets.h"

#ifndef Q2_ASSETS_OFFSET
#define Q2_ASSETS_OFFSET 0x1C0000
#endif
/* The flash's real size: a smaller chip ignores the high address bits, so
 * past its end the XIP window shows its start again (the firmware's vector
 * table). Read only, no flash command. 16 MB when no mirror is found. */
static uint32_t flash_size(void) {
#ifdef Q2_FORCE_FLASH_MB	/* testing */
    return Q2_FORCE_FLASH_MB << 20;
#endif
    const uint8_t *start = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    for (uint32_t size = 2u << 20; size < 16u << 20; size <<= 1)
        if (!memcmp(start, start + size, 4096)) return size;
    return 16u << 20;
}

static uint32_t assets_size; /* flash_size() - Q2_ASSETS_OFFSET */
#define ASSETS_SIZE assets_size
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

/* Before each operation: the UART's pending output out, the XIP cache
 * cleaned (the ROM's flush drops dirty PSRAM lines). */
static void flash_op_prepare(void) {
    extern void uart_buffered_flush(void);
    uart_buffered_flush();
    xip_cache_clean_all();
}

/* The SDK ends each operation by running the boot-time XIP setup again,
 * which sets the flash timing for the boot clock (measured: 0x60007206,
 * divider 6, read delay 2, where ours is 0x60007504): at 504 MHz the read
 * delay is far too short, and the next code fetched from the flash can be
 * garbage (a fault, whose handler is in flash too: a lockup
 * on the first erase, on some boards every time). The operation and the
 * restore of our QMI window 0 setup run from RAM, with the interrupts off,
 * so nothing is fetched from the flash in between. */
static void __no_inline_not_in_flash_func(flash_op)(uint32_t offs, const uint8_t *data, uint32_t size) {
    uint32_t timing = qmi_hw->m[0].timing, rcmd = qmi_hw->m[0].rcmd, rfmt = qmi_hw->m[0].rfmt;
    uint32_t s = save_and_disable_interrupts();
    if (data)
        flash_range_program(offs, data, size);
    else
        flash_range_erase(offs, size);
    qmi_hw->m[0].timing = timing;
    qmi_hw->m[0].rcmd = rcmd;
    qmi_hw->m[0].rfmt = rfmt;
    __dsb();
    __isb();
    restore_interrupts(s);
}

static void flash_op_erase(uint32_t offs) {
    flash_op_prepare();
    flash_op(offs, NULL, BLOCK);
}

static void flash_op_program(uint32_t offs, const uint8_t *data, uint32_t size) {
    flash_op_prepare();
    flash_op(offs, data, size);
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
    uint32_t flash = flash_size();
    q2_con_printf("Flash: %lu MB\n", (unsigned long)(flash >> 20));
    if (flash <= Q2_ASSETS_OFFSET + (1u << 20)) {
        /* 2 MB: no room for the image next to the firmware */
        q2_con_highlight("Flash too small for the models and skins image\n");
        return 0;
    }
    assets_size = flash - Q2_ASSETS_OFFSET;
    if (flash < (16u << 20))
        q2_con_highlight("%lu MB flash: the biggest models load to PSRAM.\n", (unsigned long)(flash >> 20));
    FILE *flag = fopen(rebuild_flag, "rb");
    bool forced = flag != NULL;
    if (flag) fclose(flag);

    unsigned key = Q_Assets_SourceKey(gamedir);
    if (!forced && Q_Assets_Current(base, ASSETS_SIZE, key))
        return Q_Assets_Init(base, ASSETS_SIZE);
    if (!key) return 0; /* no paks: the caller says so */

    q2_con_highlight("Building the models and skins in flash from the paks (once, about 90 seconds)...\n");
    writer_t w = {.sector = -1, .percent = -1, .t0 = time_us_32()};
    q2_con_printf("  reading the paks   ");
    assetwriter_t writer = {write_cb, progress_cb, &w};
    int n = Q_Assets_Build(gamedir, ASSETS_SIZE, &writer);
    flush(&w);
    if (n < 0) {
        q2_con_error("\nFailed: the game runs without them (big maps may run out of memory).\n");
        return 0;
    }
    if (forced) remove(rebuild_flag);
    q2_con_printf("\nDone: %d files in %lu s. Restarting...\n", n,
                  (unsigned long)((time_us_32() - w.t0) / 1000000));
    sleep_ms(1500);
    platform_reboot(false);
}
