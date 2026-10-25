/*
 * frank-quake2 — ESP-IDF attribute macros used by the engine, on the RP2350.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef Q2_COMPAT_ESP_ATTR_H
#define Q2_COMPAT_ESP_ATTR_H

/* IRAM_ATTR: the renderer's inner loops run from SRAM, out of the XIP cache
 * that the PSRAM traffic keeps thrashing (.time_critical lands in .data) */
#define IRAM_ATTR __attribute__((section(".time_critical.q2"), noinline))

/* Q2_COLD: stays in flash even in a file whose code runs from SRAM
 * (memmap_quake2.ld keeps .flashtext in flash) */
#define Q2_COLD __attribute__((section(".flashtext.q2"), noinline))

#endif
