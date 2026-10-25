/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console commands of the port: "prof SECONDS" (PC sampling, q2_prof.c),
 * "sysmem" (the C heaps), "sdtest" (SD card read speed).
 */

#include "qcommon/qcommon.h"
#include "q2_platform.h"

void q2_prof_start(int seconds);
void q2_sd_bench(void);

static void Prof_f(void) {
    q2_prof_start(Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 4);
}

unsigned q2_core1_stack_used(void);

static void Rstats_f(void) {
    Com_Printf("rstats: core 1 stack: %u bytes used at most\n", q2_core1_stack_used());
    extern int r_max_edges, r_max_surfs;
    extern int r_surf_builds, r_surf_allocs, r_surf_texels, r_stat_frames;
    int n = r_stat_frames ? r_stat_frames : 1;
    Com_Printf("rstats: %d frames, max %d edges, %d surfaces; per frame: %d surface cache builds "
               "(%d new), %d texels\n", r_stat_frames, r_max_edges, r_max_surfs, r_surf_builds / n,
               r_surf_allocs / n, r_surf_texels / n);
    {
        extern unsigned r_xip_miss[], r_xip_acc[];
        static const char *names[] = {"outside", "setup", "world", "bmodels", "scan edges",
                                      "entities", "particles", "rest", "D_DrawSurfaces"};
        for (int i = 0; i < 9; i++)
            Com_Printf("  xip %-14s %8u k acc %7u k miss (%u%%)\n", names[i], r_xip_acc[i] / 1000,
                       r_xip_miss[i] / 1000, r_xip_acc[i] ? (unsigned)((uint64_t)r_xip_miss[i] * 100 / r_xip_acc[i]) : 0);
        memset(r_xip_miss, 0, 9 * sizeof(unsigned));
        memset(r_xip_acc, 0, 9 * sizeof(unsigned));
    }
    {
        extern unsigned scr_us[];
        extern int r_scan_flushes, r_ds_calls;
        unsigned f = scr_us[5] ? scr_us[5] : 1;
        Com_Printf("  per frame: %u.%02u D_DrawSurfaces, %u.%02u scan flushes\n", r_ds_calls / f,
                   r_ds_calls * 100 / f % 100, r_scan_flushes / f, r_scan_flushes * 100 / f % 100);
        r_ds_calls = r_scan_flushes = 0;
        extern unsigned r_alias_setup_us, r_alias_models, r_alias_maxverts;
        Com_Printf("  per frame: %u alias models drawn, setup %u us; max %u verts\n", r_alias_models / f,
                   r_alias_setup_us / f, r_alias_maxverts);
        r_alias_setup_us = r_alias_models = 0;
        Com_Printf("  screen per frame: begin %u, view %u (of which render %u), 2D %u, end %u us\n",
                   scr_us[0] / f, scr_us[1] / f, scr_us[2] / f, scr_us[3] / f, scr_us[4] / f);
        memset(scr_us, 0, 6 * sizeof(unsigned));
    }
    r_max_edges = r_max_surfs = 0;
    r_surf_builds = r_surf_allocs = r_surf_texels = r_stat_frames = 0;
}

static void Sysmem_f(void) {
    q2_heap_report();
}

void q2_add_commands(void) {
    Cmd_AddCommand("prof", Prof_f);
    Cmd_AddCommand("sysmem", Sysmem_f);
    Cmd_AddCommand("rstats", Rstats_f);
    Cmd_AddCommand("sdtest", q2_sd_bench);
}
