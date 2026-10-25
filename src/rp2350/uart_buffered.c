/*
 * frank-quake2 (from frank-castle) — non-blocking stdio on the debug UART.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SDK's UART stdio blocks until every byte is on the wire: a 1 KB perf
 * report at 115200 baud stalls the game for ~90 ms, five or six frames.
 * Output goes into a ring instead, drained into the UART's 32-byte FIFO by
 * the UART's own TX interrupt (a repeating alarm did this before, and died
 * when the alarm pool was also used per frame). When the ring is full, or
 * interrupts are off (crash handlers), writes fall back to blocking so
 * nothing is lost.
 */

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_uart.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

#define RING 1024 /* power of two; SRAM is tight */

static char ring[RING];
static volatile uint32_t wr, rd;
static uart_inst_t *const u = uart0;

static void drain(void) {
    while (rd != wr && uart_is_writable(u)) {
        uart_get_hw(u)->dr = (uint8_t)ring[rd & (RING - 1)];
        rd = rd + 1;
    }
}

/* TX interrupt: the FIFO dropped to its trigger level. Refill it; stop
 * interrupting once the ring is empty. The PL011 raises the TX interrupt only
 * as the FIFO level falls, so kick() primes the FIFO by hand. */
/* RX: the game loop may not read for seconds (map loads), more than the
 * 32-byte FIFO holds: the interrupt keeps the bytes in a ring. */
#define RX_RING 512
static char rx_ring[RX_RING];
static volatile uint32_t rx_wr, rx_rd;

static void __not_in_flash_func(uart_tx_irq)(void) {
    while (uart_is_readable(u)) {
        char c = (char)uart_get_hw(u)->dr;
        if (rx_wr - rx_rd < RX_RING) {
            rx_ring[rx_wr & (RX_RING - 1)] = c;
            rx_wr = rx_wr + 1;
        }
    }
    drain();
    if (rd == wr) uart_get_hw(u)->imsc &= ~UART_UARTIMSC_TXIM_BITS;
}

static void kick(void) {
    uint32_t s = save_and_disable_interrupts();
    drain();
    if (rd != wr) uart_get_hw(u)->imsc |= UART_UARTIMSC_TXIM_BITS;
    restore_interrupts(s);
}

static bool irqs_enabled(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    return !(primask & 1);
}

static void put(char c) {
    while (wr - rd >= RING) { /* full: make room ourselves */
        uint32_t s = save_and_disable_interrupts();
        drain();
        restore_interrupts(s);
    }
    ring[wr & (RING - 1)] = c;
    __dmb();
    wr = wr + 1;
}

static void out_chars(const char *buf, int len) {
    if (!irqs_enabled()) {
        /* fault handler or critical section: nobody will drain, go direct */
        while (rd != wr) {
            while (!uart_is_writable(u)) tight_loop_contents();
            uart_get_hw(u)->dr = (uint8_t)ring[rd & (RING - 1)];
            rd = rd + 1;
        }
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') uart_putc_raw(u, '\r');
            uart_putc_raw(u, buf[i]);
        }
        return;
    }
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') put('\r');
        put(buf[i]);
    }
    kick();
}

static void out_flush(void) {
    while (rd != wr) {
        uint32_t s = save_and_disable_interrupts();
        drain();
        restore_interrupts(s);
    }
    uart_tx_wait_blocking(u);
}

static int in_chars(char *buf, int len) {
    int n = 0;
    while (n < len && rx_rd != rx_wr) {
        buf[n++] = rx_ring[rx_rd & (RX_RING - 1)];
        rx_rd = rx_rd + 1;
    }
    return n ? n : PICO_ERROR_NO_DATA;
}

static stdio_driver_t buffered_uart = {
    .out_chars = out_chars,
    .out_flush = out_flush,
    .in_chars = in_chars,
};

/* Call after stdio_init_all() (the UART is set up by then); takes over from
 * the blocking SDK driver. Runs the drain timer on the calling core. */
void uart_buffered_init(void) {
    stdio_set_driver_enabled(&stdio_uart, false);
    stdio_set_driver_enabled(&buffered_uart, true);
    int irq = u == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, uart_tx_irq);
    uart_get_hw(u)->imsc &= ~UART_UARTIMSC_TXIM_BITS;
    uart_get_hw(u)->imsc |= UART_UARTIMSC_RXIM_BITS | UART_UARTIMSC_RTIM_BITS;
    irq_set_enabled(irq, true);
}

void uart_buffered_flush(void) { out_flush(); }
