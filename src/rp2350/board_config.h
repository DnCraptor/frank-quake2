/*
 * frank-quake2 — Quake II for RP2350 (from frank-castle)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/*
 * M2 GPIO layout on RP2350B (the only target for now):
 *   HDMI:  GPIO12-19 via HSTX (12/13 CLK, 14/15 D0, 16/17 D1, 18/19 D2)
 *   SD:    SPI0 SCK=6, MOSI=7, MISO=4, CS=5
 *   I2S:   DATA=9, BCLK=10, LRCK=11
 *   PSRAM: CS1=47 (RP2350B), 8 MB
 *   UART:  TX=0, RX=1 (debug console; native USB is the keyboard host)
 */

#if !defined(BOARD_M2)
#define BOARD_M2
#endif

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 504
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_65
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

#define PSRAM_PIN_RP2350A 8
#define PSRAM_PIN_RP2350B 47

static inline uint get_psram_pin(void) {
    uint32_t package_sel = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    return (package_sel & 1) ? PSRAM_PIN_RP2350A : PSRAM_PIN_RP2350B;
}

#define HDMI_BASE_PIN 12

#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5

#ifndef PICO_AUDIO_I2S_DATA_PIN
#define PICO_AUDIO_I2S_DATA_PIN 9
#endif
#ifndef PICO_AUDIO_I2S_CLOCK_PIN_BASE
#define PICO_AUDIO_I2S_CLOCK_PIN_BASE 10
#endif

#endif /* BOARD_CONFIG_H */
