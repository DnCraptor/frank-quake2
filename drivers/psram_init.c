/*
 * frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-fodder
 *
 * Licensed under the GNU General Public License v3.0 or later.
 * See LICENSE for the full license text.
 */

#include "psram_init.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

// PSRAM max frequency - set via CMake or use default
#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

void __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    const int clock_hz = clock_get_hz(clk_sys); 

    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    /* Send QPI-enable command. After reset PSRAM is in SPI mode.
     * This matches the frank-msx reference implementation exactly. */
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | 
                        QMI_DIRECT_CSR_EN_BITS | 
                        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    const uint CMD_QPI_EN = 0x35; 
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);


    const int max_psram_freq = PSRAM_MAX_FREQ_MHZ * 1000000; 
    
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }
    
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1; 
    }
#ifdef PSRAM_RXDELAY
    /* measured per board: at 504/3 = 168 MHz an APS6404 here reads
     * correctly with RXDELAY 5..7 and fails with the derived 4 */
    rxdelay = PSRAM_RXDELAY;
#endif

    const int clock_period_fs = 1000000000000000ll / clock_hz;
    
    const int max_select_val = (125 * 1000000) / clock_period_fs;

    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    qmi_hw->m[1].timing = 
        1 << QMI_M1_TIMING_COOLDOWN_LSB | 
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB | 
        max_select_val << QMI_M1_TIMING_MAX_SELECT_LSB | 
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB | 
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB | 
        divisor << QMI_M1_TIMING_CLKDIV_LSB;

    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB | 
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB | 
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB | 
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB | 
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB | 
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB | 
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;
    
    qmi_hw->m[1].rcmd = 0xEB; 

    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
    
    qmi_hw->m[1].wcmd = 0x38; 

    qmi_hw->direct_csr = 0;
    
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

/* Put the PSRAM back into its power-on SPI mode. The chip keeps QPI mode
 * across a chip reset that does not cycle its power, and a QPI-mode PSRAM
 * sharing the QSPI bus garbles the boot ROM's flash probe: the RP2350 then
 * hangs in the ROM until a power cycle. Call this (with the other core
 * stopped) before every software reboot. Runs from RAM: XIP is unusable
 * while the QMI is in direct mode. */
void __no_inline_not_in_flash_func(psram_reset_to_spi)(void) {
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    /* exit QPI: 0xF5 sent on all four lines */
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xF5;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    /* reset enable + reset, single line */
    static const uint8_t cmds[2] = {0x66, 0x99};
    for (int i = 0; i < 2; i++) {
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | cmds[i];
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    }
    for (volatile int i = 0; i < 1000; i++);
    qmi_hw->direct_csr = 0;
}
