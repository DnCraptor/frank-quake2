/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Core 0: the game (client, server, game logic, renderer), USB host, SD
 * card, I2S audio IRQ. Core 1: HDMI (HSTX DMA IRQ) and the second half of
 * the renderer's span and triangle drawing (QG_Parallel2).
 *
 * SD card: /quake2/baseq2/pak0.pak (pak1, pak2 from the 3.20 patch),
 * optional /quake2/args.txt (extra command line, e.g. "+map base1").
 * Flash: the model/skin image at Q2_ASSETS_OFFSET, built from the paks on the
 * first start (or made by tools/mkassets.py).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ff.h"
#include "hardware/clocks.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#if Q2_VIDEO_PIO
#include "hdmi_pio.h"
#else
#include "psx_video.h"
#endif
#include "q2_platform.h"
#include "quake2.h"
#include "quakegeneric.h"
#include "other/q_assets.h"

#define BASEDIR "/quake2"
#define MAX_ARGS 32

extern float q2_take_fps(void);
extern void q2_add_commands(void);
extern void q2_prof_poll(void);
extern int64_t parallelWaitUs;

uint32_t core1_stack[3072 / 4]; /* the BSP walk recurses on core 1: 1.8 KB at most in demo1 and base1 (rstats) */

/* deepest use of core 1's stack so far ("rstats"; painted at boot) */
unsigned q2_core1_stack_used(void) {
    unsigned i = 0;
    while (i < sizeof(core1_stack) / 4 && core1_stack[i] == 0xa5a5a5a5u) i++;
    return sizeof(core1_stack) - i * 4;
}

static void wait_for_sd(void) {
    if (platform_mount_sd() == FR_OK) return;
    q2_shell_error("NO SD CARD", "Waiting for the card...");
    q2_con_printf("Insert a FAT32 or exFAT SD card with the Quake II paks:\n\n");
    q2_con_highlight("  " BASEDIR "/baseq2/pak0.pak\n  " BASEDIR "/baseq2/pak1.pak\n  " BASEDIR "/baseq2/pak2.pak\n");
    while (platform_mount_sd() != FR_OK) {
        platform_usb_poll();
        sleep_ms(100);
    }
    q2_shell_welcome();
    q2_con_printf("SD card mounted.\n");
}

/* argv: the defaults, then the words of /quake2/args.txt */
static int build_args(char **argv) {
    static char text[512];
    int argc = 0;
    argv[argc++] = "quake2";
    argv[argc++] = "+set";
    argv[argc++] = "basedir";
    argv[argc++] = BASEDIR;
    FILE *f = fopen(BASEDIR "/args.txt", "r");
    if (f) {
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = 0;
        for (char *p = strtok(text, " \t\r\n"); p && argc < MAX_ARGS - 1; p = strtok(NULL, " \t\r\n"))
            argv[argc++] = p;
        q2_con_printf("args.txt: %d words\n", argc - 4);
    }
    argv[argc] = NULL;
    return argc;
}

/* the model/skin image in flash, built from the paks when needed
 * (q2_assets_flash.c) */
static void map_assets(void) {
    extern int q2_assets_init(const char *gamedir, const char *rebuild_flag);
    int n = q2_assets_init(BASEDIR "/baseq2", BASEDIR "/assets.rebuild");
    if (n)
        q2_con_printf("Flash assets: %d models and skins\n", n);
    else
        q2_con_highlight("No models and skins in flash: they load to PSRAM (big maps may run out of memory).\n");
}

int main(void) {
    extern void q2_assets_probe_flash(void);
    /* SPI NOR JEDEC ID must be read at the boot clock, before core 1 and
     * the QMI PSRAM are started. No flash commands during game runtime. */
    q2_assets_probe_flash();
    platform_init_clocks();
    platform_report_previous_crash();
    platform_init_psram();

    psxv_init();
    q2_video_init();
    for (unsigned i = 0; i < sizeof(core1_stack) / 4; i++) core1_stack[i] = 0xa5a5a5a5u;
    multicore_reset_core1();
    multicore_launch_core1_with_stack(q2_core1_main, core1_stack, sizeof(core1_stack));

    q2_shell_welcome();
    q2_heap_report();

    platform_init_usb();
    q2_input_init(); /* PS/2 PIO keyboard, alongside USB HID */
    wait_for_sd();

    struct stat st;
    if (stat(BASEDIR "/baseq2/pak0.pak", &st)) {
        q2_shell_error("NO GAME DATA", "Press any key to restart");
        q2_con_printf("No " BASEDIR "/baseq2/pak0.pak on the SD card. Copy the paks of Quake II, patched to 3.20, there:\n\n");
        q2_con_highlight("  " BASEDIR "/baseq2/pak0.pak\n  " BASEDIR "/baseq2/pak1.pak\n  " BASEDIR "/baseq2/pak2.pak\n");
        q2_halt("");
    }
    map_assets();

    static char *argv[MAX_ARGS];
    int argc = build_args(argv);
    q2_con_highlight("Starting Quake II...\n");
    q2_shell_end(2500);
    Quake2_Init(argc, argv);
    q2_add_commands();

    int oldtime = QG_Milliseconds();
    uint32_t stats = time_us_32();
    q2_take_fps();
    while (1) {
        q2_input_poll();
        int newtime, msec;
        do {
            newtime = QG_Milliseconds();
            msec = newtime - oldtime;
        } while (msec < 1);
        oldtime = newtime;
        Quake2_Frame(msec);
        q2_prof_poll();
        if (time_us_32() - stats >= 5000000) {
            stats = time_us_32();
            extern volatile uint32_t core1_busy_us;
            extern int q2_split_share(void);
            printf("fps: %.1f (core 0 waited %lu ms for core 1, core 1 busy %lu ms of 5000, share %d/16)\n",
                   (double)q2_take_fps(), (unsigned long)(parallelWaitUs / 1000),
                   (unsigned long)(core1_busy_us / 1000), q2_split_share());
            parallelWaitUs = 0;
            core1_busy_us = 0;
            /* XIP cache (flash code and data, PSRAM): hit rate */
            static uint32_t last_acc, last_hit;
            uint32_t acc = xip_ctrl_hw->ctr_acc - last_acc, hit = xip_ctrl_hw->ctr_hit - last_hit;
            last_acc += acc;
            last_hit += hit;
            printf("xip: %lu k accesses, %lu k misses\n", (unsigned long)(acc / 1000),
                   (unsigned long)((acc - hit) / 1000));
        }
    }
}
