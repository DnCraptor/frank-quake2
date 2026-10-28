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
 * M1 (Murmulator 1):
 *   HDMI:  GPIO6-13 via PIO (6/7 CLK, 8/9 D0, 10/11 D1, 12/13 D2)
 *   SD:    SPI0 SCK=2, MOSI=3, MISO=4, CS=5
 *   I2S:   DATA=26, BCLK=27, LRCK=28
 *   PSRAM: CS1=19 (RP2350A) or 47 (RP2350B)
 *   PS/2:  CLK=0, DATA=1 (UART disabled in game firmware)
 *
 * M2 GPIO layout on RP2350B:
 *   HDMI:  GPIO12-19 via HSTX (12/13 CLK, 14/15 D0, 16/17 D1, 18/19 D2)
 *   SD:    SPI0 SCK=6, MOSI=7, MISO=4, CS=5
 *   I2S:   DATA=9, BCLK=10, LRCK=11
 *   PSRAM: CS1=47 (RP2350B), 8 MB
 *   PS/2:  CLK=2, DATA=3; native USB is an independent HID host
 *
 * Z0 (Waveshare RP2350-PiZero, RP2350B):
 *   HDMI:  GPIO32-39 via PIO (32..37 the data pairs, 38/39 CLK)
 *   SD:    SPI1 SCK=30, MOSI=31, MISO=40, CS=43
 *   I2S:   a PCM5102A module, DATA=10, BCLK=11, LRCK=12
 *   PSRAM: CS1=47, 8 MB
 *   PS/2:  CLK=2, DATA=3; native USB is an independent HID host
 *
 * The pins the drivers use come from CMakeLists.txt.
 */

#if !defined(BOARD_M1) && !defined(BOARD_M2) && !defined(BOARD_Z0)
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

#ifdef BOARD_M1
#define PSRAM_PIN_RP2350A 19
#else
#define PSRAM_PIN_RP2350A 8
#endif
#define PSRAM_PIN_RP2350B 47

static inline uint get_psram_pin(void) {
    uint32_t package_sel = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    return (package_sel & 1) ? PSRAM_PIN_RP2350A : PSRAM_PIN_RP2350B;
}

#ifdef BOARD_M1
#define HDMI_BASE_PIN 6
#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK  2
#define SDCARD_PIN_SPI0_MOSI 3
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5
#endif
#elif defined(BOARD_Z0)
#define HDMI_BASE_PIN 32
#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK  30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40
#define SDCARD_PIN_SPI0_CS   43
#endif
#else
#define HDMI_BASE_PIN 12
#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5
#endif
#endif

#ifndef PICO_AUDIO_I2S_DATA_PIN
#define PICO_AUDIO_I2S_DATA_PIN 9
#endif
#ifndef PICO_AUDIO_I2S_CLOCK_PIN_BASE
#define PICO_AUDIO_I2S_CLOCK_PIN_BASE 10
#endif

#endif /* BOARD_CONFIG_H */
