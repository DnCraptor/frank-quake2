/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console commands of the port: "prof SECONDS" (PC sampling, q2_prof.c),
 * "sysmem" (the C heaps).
 */

#include "qcommon/qcommon.h"
#include "q2_platform.h"

void q2_prof_start(int seconds);

static void Prof_f(void) {
    q2_prof_start(Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 4);
}

static void Sysmem_f(void) {
    q2_heap_report();
}

void q2_add_commands(void) {
    Cmd_AddCommand("prof", Prof_f);
    Cmd_AddCommand("sysmem", Sysmem_f);
}
