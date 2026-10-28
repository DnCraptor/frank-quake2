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
 * The image takes the flash from Q2_ASSETS_OFFSET to the size read from the
 * SPI NOR JEDEC ID (2/4/8/16 MiB chips); an unknown ID disables flash
 * writes instead of risking a wrap into the firmware. Q_Assets_Build stores
 * the subset that fits and reports any omitted models/skins.
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
#define Q2_ASSETS_OFFSET 0xE1000
#endif

static uint32_t physical_flash_bytes; /* 0 means unknown: never erase! */
static uint32_t flash_jedec_id;

/* Must run BEFORE platform_init_clocks(), platform_init_psram(), and the
 * second core. flash_do_cmd exits XIP and reenables it via boot ROM: doing
 * so after PSRAM/QMI timing setup is unsafe. Common SPI NOR density codes
 * 0x15..0x18 mean 2, 4, 8, 16 MiB respectively. The RP2350 XIP CS0 window
 * is 16 MiB, so larger devices are capped to that addressable window. */
void q2_assets_probe_flash(void) {
    const uint8_t tx[4] = {0x9f, 0, 0, 0}; /* RDID */
    uint8_t rx[4] = {0};
    flash_do_cmd(tx, rx, sizeof(tx));
    flash_jedec_id = (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
    unsigned exp = rx[3];
    if (rx[1] != 0 && rx[1] != 0xff && exp >= 0x14 && exp <= 0x18)
        physical_flash_bytes = 1u << exp;
    else if (rx[1] != 0 && rx[1] != 0xff && exp == 0x19)
        physical_flash_bytes = 16u << 20; /* 32 MiB chip: 16 MiB XIP aperture */
}

static uint32_t flash_size(void) {
#ifdef Q2_FORCE_FLASH_MB /* testing may reduce, never enlarge, the JEDEC limit */
    uint32_t forced = (uint32_t)Q2_FORCE_FLASH_MB << 20;
    return forced < physical_flash_bytes ? forced : physical_flash_bytes;
#else
    return physical_flash_bytes;
#endif
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

/* Before each operation: clean the XIP cache (the ROM's flush drops
 * dirty PSRAM lines). The game firmware has no UART output. */
static void flash_op_prepare(void) {
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

static void flash_op_erase(uint32_t offs, uint32_t size) {
    flash_op_prepare();
    flash_op(offs, NULL, size);
}

static void flash_op_program(uint32_t offs, const uint8_t *data, uint32_t size) {
    flash_op_prepare();
    flash_op(offs, data, size);
}

static void erase_to(writer_t *w, uint32_t end) {
    while (w->erased < end) {
        uint32_t offs = Q2_ASSETS_OFFSET + w->erased;
        /* Flash writes start at 900 KiB (0xE1000): 4 KiB sectors up to
         * the next 64 KiB boundary, then fast aligned 64 KiB blocks.
         * Never erase outside the physically detected chip. */
        uint32_t size = ((offs & (BLOCK - 1u)) == 0 &&
                         ASSETS_SIZE - w->erased >= BLOCK) ? BLOCK : SECTOR;
        flash_op_erase(offs, size);
        w->erased += size;
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
    /* Bounds-check without a signed/unsigned addition that can wrap. */
    if (offset < 0 || size < 0 || (uint32_t)offset > ASSETS_SIZE ||
        (uint32_t)size > ASSETS_SIZE - (uint32_t)offset) return 0;
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
    if (!flash) {
        q2_con_error("Flash JEDEC ID %06lX: unknown capacity; asset writes disabled.\n",
                     (unsigned long)flash_jedec_id);
        return 0;
    }
    q2_con_printf("Flash: %lu MiB (JEDEC %06lX), firmware 0x%lX\n",
                  (unsigned long)(flash >> 20), (unsigned long)flash_jedec_id,
                  (unsigned long)Q2_ASSETS_OFFSET);
    if (flash <= Q2_ASSETS_OFFSET + SECTOR) {
        q2_con_error("Flash too small: no space for the model/skin image.\n");
        return 0;
    }
    assets_size = flash - Q2_ASSETS_OFFSET;
    q2_con_printf("Flash asset partition: %lu KiB available\n",
                  (unsigned long)(assets_size >> 10));
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
    asset_build_stats_t stats = {0};
    int n = Q_Assets_Build(gamedir, ASSETS_SIZE, &writer, &stats);
    flush(&w);
    if (n < 0) {
        q2_con_error("\nFailed: the game runs without them (big maps may run out of memory).\n");
        if (stats.skipped_capacity)
            q2_con_error("Flash insufficient: need %lu KiB, available %lu KiB.\n",
                         (unsigned long)((stats.required_bytes + 1023) / 1024),
                         (unsigned long)(ASSETS_SIZE >> 10));
        return 0;
    }
    if (forced) remove(rebuild_flag);
    if (stats.skipped_capacity) {
        /* This occurs only when the asset image has to omit models/skins.
         * Keep the result on screen: the original five-second delay made
         * the actual shortage impossible to read during first boot. */
        unsigned long deficit = (unsigned long)(stats.required_bytes > ASSETS_SIZE ?
            stats.required_bytes - ASSETS_SIZE : 0);
        q2_shell_error("FLASH CAPACITY", "Press any key / pad to restart");
        q2_con_error("Missing: %lu bytes (%lu KiB)\n", deficit, (deficit + 1023) / 1024);
        q2_con_printf("Required: %lu bytes\n", (unsigned long)stats.required_bytes);
        q2_con_printf("Capacity: %lu bytes\n", (unsigned long)ASSETS_SIZE);
        q2_con_printf("Stored: %lu bytes; free: %lu bytes\n",
                      (unsigned long)stats.used_bytes,
                      (unsigned long)(ASSETS_SIZE - stats.used_bytes));
        q2_con_highlight("%d models/skins omitted; loaded from SD into PSRAM.\n",
                         stats.skipped_capacity);
        q2_con_printf("First omitted: %s\n", stats.first_skipped);
        q2_con_printf("\nDone: %d files in %lu s.\n", n,
                      (unsigned long)((time_us_32() - w.t0) / 1000000));
        q2_halt(""); /* USB HID, PS/2 or NESpad button -> reboot */
    }
    q2_con_printf("\nDone: %d files in %lu s. Restarting...\n", n,
                  (unsigned long)((time_us_32() - w.t0) / 1000000));
    sleep_ms(1500);
    platform_reboot(false);
}
