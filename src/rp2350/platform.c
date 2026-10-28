/*
 * frank-quake2 — Quake II for RP2350 (board bring-up from frank-castle)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Board bring-up, following frank-fodder's main.c order:
 * voltage -> flash timings -> sys clock -> PSRAM (once, at the final clock).
 */

#include "platform.h"
#include "board_config.h"
#include "psram_init.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "ff.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Reboot                                                              */
/* ------------------------------------------------------------------ */

/* Every software reboot goes through here: the PSRAM must leave QPI mode
 * first, or the boot ROM cannot read the flash and the chip hangs until a
 * power cycle (see psram_reset_to_spi). */
void __attribute__((noreturn)) platform_reboot(bool bootsel) {
    /* The diagnostic target links the UART buffer; the game does not. */
    extern void uart_buffered_flush(void) __attribute__((weak));
    if (uart_buffered_flush) uart_buffered_flush();
    if (get_core_num() == 0) {
        /* stop core 1 (it may be reading PSRAM). Not multicore_reset_core1():
         * that waits for core 1's boot ROM to answer on the FIFO, and hung
         * here once; we are rebooting anyway, so just force it off. */
        hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
        while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) tight_loop_contents();
        busy_wait_us(10);
        /* release it again: it restarts into the boot ROM, which waits for a
         * launch and does not touch PSRAM. Left forced off, the bit survives
         * the watchdog reboot and the chip does not come back. */
        hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
        busy_wait_us(10);
    }
    save_and_disable_interrupts();
    psram_reset_to_spi();
    if (bootsel) reset_usb_boot(0, 0);
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}

static char dbg_cmd[96];
volatile bool platform_uart_scripted; /* a "~C" command came: letters stop being keys */
static volatile bool dbg_cmd_ready;

/* the game thread takes the pending "~C ...~" command, if any */
int Platform_DebugCmdLine(char *out, int n) {
    if (!dbg_cmd_ready) return 0;
    strncpy(out, dbg_cmd, (size_t)n - 1);
    out[n - 1] = 0;
    dbg_cmd_ready = false;
    return 1;
}

/* "~BOOT~" on the UART reboots into BOOTSEL, so the board can be reflashed
 * over USB (picotool) without touching it. Feed every received byte here. */
void platform_uart_cmd(int ch) {
    static const char magic[] = "~BOOT~";
    static unsigned pos;
    pos = (ch == magic[pos]) ? pos + 1 : (ch == magic[0]);
    if (pos == sizeof(magic) - 1) {
        printf("\nrebooting to BOOTSEL\n");
        sleep_ms(20);
        platform_reboot(true);
    }
    /* "~RST~": plain reboot (test scripts restart the game this way) */
    static const char rst[] = "~RST~";
    static unsigned rpos;
    rpos = (ch == rst[rpos]) ? rpos + 1 : (ch == rst[0]);
    if (rpos == sizeof(rst) - 1) {
        printf("\nrebooting\n");
        sleep_ms(20);
        platform_reboot(false);
    }
    /* "~VERB~": toggle the detailed perf log */
    static const char verb[] = "~VERB~";
    static unsigned vpos;
    vpos = (ch == verb[vpos]) ? vpos + 1 : (ch == verb[0]);
    if (vpos == sizeof(verb) - 1) {
        extern volatile bool psxv_perf_verbose __attribute__((weak));
        vpos = 0;
        if (&psxv_perf_verbose) psxv_perf_verbose = !psxv_perf_verbose;
    }
    /* "~C LINE~": a debug command for the game thread (src/pc/debug_cmd.c),
     * except "pad BITS FRAMES", handled by the pad code */
    {
        static char line[96];
        static int cst, clen;
        if (cst == 3) {
            if (ch == '~') {
                line[clen] = 0;
                cst = 0;
                unsigned bits, frames;
                if (sscanf(line, "pad %x %u", &bits, &frames) == 2) {
                    extern void platform_pad_hold(unsigned bits, unsigned frames) __attribute__((weak));
                    if (platform_pad_hold) platform_pad_hold(bits, frames);
                } else if (!dbg_cmd_ready) {
                    memcpy(dbg_cmd, line, (size_t)clen + 1);
                    dbg_cmd_ready = true;
                }
            } else if (clen < (int)sizeof(line) - 1) {
                line[clen++] = (char)ch;
            } else {
                cst = 0;
            }
        } else if (ch == '~') {
            cst = 1;
        } else if (cst == 1 && ch == 'C') {
            cst = 2;
        } else if (cst == 2 && ch == ' ') {
            cst = 3;
            platform_uart_scripted = true;
            clen = 0;
        } else {
            cst = 0;
        }
    }
    /* "~PROF~": profile from now on (profiler builds link profiler_rearm) */
    extern void profiler_rearm(void) __attribute__((weak));
    static const char prof[] = "~PROF~";
    static unsigned ppos;
    ppos = (ch == prof[ppos]) ? ppos + 1 : (ch == prof[0]);
    if (ppos == sizeof(prof) - 1) {
        ppos = 0;
        if (profiler_rearm) {
            printf("\nprofiling\n");
            profiler_rearm();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Crash capture (survives the watchdog reboot)                        */
/* ------------------------------------------------------------------ */

static volatile uint32_t __attribute__((section(".uninitialized_data"))) crash_info[12];

#define CRASH_HARDFAULT 0xDEADBEEFu
#define CRASH_PANIC     0xDEADCAFEu

void __real_panic(const char *fmt, ...);

void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    /* TinyUSB host asserts during flaky enumeration are not worth a reboot:
     * park this core and let the other one keep running. */
    if (fmt && (strstr(fmt, "Host") || strstr(fmt, "HCD") || strstr(fmt, "tuh") ||
                strstr(fmt, "EP"))) {
        while (1) tight_loop_contents();
    }
    void *lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    crash_info[0] = CRASH_PANIC;
    crash_info[1] = (uint32_t)(uintptr_t)lr;
    crash_info[2] = (uint32_t)(uintptr_t)fmt;
    crash_info[3] = get_core_num();
    printf("\n*** PANIC on core %u ***\n", (unsigned)crash_info[3]);
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
    }
    sleep_ms(100);
    platform_reboot(false);
}

void __attribute__((used)) HardFault_Handler_C(uint32_t *frame) {
    crash_info[0] = CRASH_HARDFAULT;
    crash_info[1] = frame[6]; /* PC */
    crash_info[2] = frame[5]; /* LR */
    crash_info[3] = frame[0];
    crash_info[4] = frame[1];
    crash_info[5] = *(volatile uint32_t *)0xE000ED28; /* CFSR */
    crash_info[6] = *(volatile uint32_t *)0xE000ED2C; /* HFSR */
    crash_info[7] = *(volatile uint32_t *)0xE000ED38; /* BFAR */
    crash_info[8] = get_core_num();
    printf("\n*** HARDFAULT core %u PC=0x%08x LR=0x%08x CFSR=0x%08x BFAR=0x%08x ***\n",
           (unsigned)crash_info[8], (unsigned)crash_info[1], (unsigned)crash_info[2],
           (unsigned)crash_info[5], (unsigned)crash_info[7]);
    busy_wait_ms(500);
    platform_reboot(false);
}

void __attribute__((naked, used)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b HardFault_Handler_C\n");
}

void platform_report_previous_crash(void) {
    if (crash_info[0] == CRASH_HARDFAULT) {
        printf("*** PREVIOUS CRASH (HardFault) core %u ***\n", (unsigned)crash_info[8]);
        printf("PC=0x%08x LR=0x%08x R0=0x%08x R1=0x%08x\n", (unsigned)crash_info[1],
               (unsigned)crash_info[2], (unsigned)crash_info[3], (unsigned)crash_info[4]);
        printf("CFSR=0x%08x HFSR=0x%08x BFAR=0x%08x\n", (unsigned)crash_info[5],
               (unsigned)crash_info[6], (unsigned)crash_info[7]);
    } else if (crash_info[0] == CRASH_PANIC) {
        printf("*** PREVIOUS CRASH (panic) core %u LR=0x%08x ***\n", (unsigned)crash_info[3],
               (unsigned)crash_info[1]);
        const char *msg = (const char *)(uintptr_t)crash_info[2];
        if (msg) printf("Message: %s\n", msg);
    }
    crash_info[0] = 0;
}

/* ------------------------------------------------------------------ */
/* Clocks                                                              */
/* ------------------------------------------------------------------ */

/* rxdelay counts half sys clock cycles, so a delay tuned for the final
 * clock is far too long at the boot clock: the safe timing goes in before
 * the clock is raised, the fast one after (tools: diag 'f' sweeps it). */
/* COOLDOWN 1; 3 (0xE0007000) measured no faster for the flash */
#ifndef FLASH_TIMING_BASE
#define FLASH_TIMING_BASE 0x60007000u
#endif

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int max_mhz, int rx_override) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;
    if (rx_override >= 0) rxdelay = rx_override;

    qmi_hw->m[0].timing = FLASH_TIMING_BASE | rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                          divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

#ifndef FLASH_RXDELAY
#define FLASH_RXDELAY -1
#endif
#define FLASH_SAFE_MHZ 66

uint32_t platform_init_clocks(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n==== frank-quake2 %s ====\n", Q2_VERSION);
    if (watchdog_caused_reboot()) printf("(watchdog reboot)\n");

#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_SAFE_MHZ, -1);
    sleep_ms(10);
#endif
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        printf("%d MHz unreachable, falling back to 252 MHz\n", CPU_CLOCK_MHZ);
        set_sys_clock_khz(252 * 1000, true);
    }
    if (FLASH_MAX_FREQ_MHZ > FLASH_SAFE_MHZ)
        set_flash_timings((int)(clock_get_hz(clk_sys) / 1000000), FLASH_MAX_FREQ_MHZ, FLASH_RXDELAY);
#if PERI_CLOCK_MHZ
    /* clk_peri (UART, SPI) from clk_sys instead of the 48 MHz USB PLL: the
     * SD card's SPI runs at clk_peri / an even divider, 24 MHz at most from
     * 48 MHz. The UART divisor is set again below. */
    {
        uint32_t sys = clock_get_hz(clk_sys);
        uint32_t div = (sys + PERI_CLOCK_MHZ * 1000000u / 2) / (PERI_CLOCK_MHZ * 1000000u);
        if (div < 1) div = 1;
        clock_configure_int_divider(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sys, div);
    }
#endif
    /* Diag retains its UART console; game uses neither UART pins nor UART stdio. */
    stdio_init_all();
    extern void uart_buffered_init(void) __attribute__((weak));
    if (uart_buffered_init) uart_buffered_init();
    uint32_t hz = clock_get_hz(clk_sys);
    printf("sys clock %lu Hz, peri %lu Hz\n", (unsigned long)hz, (unsigned long)clock_get_hz(clk_peri));
    return hz;
}

/* ------------------------------------------------------------------ */
/* PSRAM                                                               */
/* ------------------------------------------------------------------ */

extern uint8_t __psram_data_start__, __psram_data_end__, __psram_data_load__;
extern uint8_t __psram_bss_start__, __psram_bss_end__;
extern void q2_heap_init(void);

unsigned platform_init_psram(void) {
    uint pin = get_psram_pin();
    psram_init(pin);

    size_t n = (size_t)(&__psram_data_end__ - &__psram_data_start__);
    if (n) memcpy(&__psram_data_start__, &__psram_data_load__, n);
    n = (size_t)(&__psram_bss_end__ - &__psram_bss_start__);
    if (n) memset(&__psram_bss_start__, 0, n);

    q2_heap_init();
    printf("PSRAM on CS%u, %u KB static\n", pin,
           (unsigned)((&__psram_bss_end__ - &__psram_data_start__) / 1024));
    return pin;
}

/* ------------------------------------------------------------------ */
/* USB host                                                            */
/* ------------------------------------------------------------------ */

extern void usbhid_init(void);
extern void usbhid_task(void);

/* The host task runs on the game thread, once per frame (platform_usb_poll
 * from VSync), not in a timer interrupt: TinyUSB enumerates with blocking
 * delays, and something on the port that never enumerates (another host,
 * e.g. a PC on the USB-C port) kept it looping inside the interrupt, which
 * froze core 0 for good. A one-shot alarm bounds each call: if one blocks
 * for over a second the USB interrupt is switched off, the call drains and
 * returns, and the host stays off until the next boot. */
static volatile bool usb_dead;
static alarm_id_t usb_guard;

static int64_t usb_guard_cb(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    usb_dead = true;
    irq_set_enabled(USBCTRL_IRQ, false);
    return 0;
}

void platform_init_usb(void) {
    usbhid_init();
}

void platform_usb_poll(void) {
    if (usb_dead) return;
    usb_guard = add_alarm_in_ms(1000, usb_guard_cb, NULL, true);
    usbhid_task();
    if (usb_guard > 0) cancel_alarm(usb_guard);
    if (usb_dead) printf("USB host: device on the port does not enumerate, host disabled\n");
}

/* ------------------------------------------------------------------ */
/* SD card                                                             */
/* ------------------------------------------------------------------ */

int platform_mount_sd(void) {
    static FATFS fs;
    FRESULT fr = FR_NOT_READY;
    for (int attempt = 0; attempt < 6 && fr != FR_OK; attempt++) {
        fr = f_mount(&fs, "", 1);
        if (fr != FR_OK) sleep_ms(50 << attempt);
    }
    printf("SD mount: %d\n", fr);
    return fr;
}

void platform_cyccnt_enable(void) {
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24); /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001004 = 0;           /* CYCCNT */
    *(volatile uint32_t *)0xE0001000 |= 1u;         /* DWT_CTRL.CYCCNTENA */
}
