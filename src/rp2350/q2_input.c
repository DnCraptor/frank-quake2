/*
 * frank-quake2 — Quake II for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Input: USB HID keyboard and mouse (TinyUSB host, drivers/usbhid) and the
 * UART console. Polled once per frame on the game core, before the frame.
 */

#include <stdio.h>
#include <string.h>

#include "client/keys.h"
#include "pico/stdlib.h"
#include "q2_platform.h"
#include "quake2.h"
#include "usbhid.h"

/* HID usage (keyboard page) -> Quake key, 0: none */
static const unsigned char hid_to_key[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e', [0x09] = 'f',
    [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p', [0x14] = 'q', [0x15] = 'r',
    [0x16] = 's', [0x17] = 't', [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6',
    [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = K_ENTER, [0x29] = K_ESCAPE, [0x2A] = K_BACKSPACE, [0x2B] = K_TAB, [0x2C] = K_SPACE,
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x32] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
    [0x3A] = K_F1, [0x3B] = K_F2, [0x3C] = K_F3, [0x3D] = K_F4, [0x3E] = K_F5, [0x3F] = K_F6,
    [0x40] = K_F7, [0x41] = K_F8, [0x42] = K_F9, [0x43] = K_F10, [0x44] = K_F11, [0x45] = K_F12,
    [0x48] = K_PAUSE, [0x49] = K_INS, [0x4A] = K_HOME, [0x4B] = K_PGUP, [0x4C] = K_DEL,
    [0x4D] = K_END, [0x4E] = K_PGDN, [0x4F] = K_RIGHTARROW, [0x50] = K_LEFTARROW,
    [0x51] = K_DOWNARROW, [0x52] = K_UPARROW,
    [0x54] = K_KP_SLASH, [0x55] = '*', [0x56] = K_KP_MINUS, [0x57] = K_KP_PLUS, [0x58] = K_KP_ENTER,
    [0x59] = K_KP_END, [0x5A] = K_KP_DOWNARROW, [0x5B] = K_KP_PGDN, [0x5C] = K_KP_LEFTARROW,
    [0x5D] = K_KP_5, [0x5E] = K_KP_RIGHTARROW, [0x5F] = K_KP_HOME, [0x60] = K_KP_UPARROW,
    [0x61] = K_KP_PGUP, [0x62] = K_KP_INS, [0x63] = K_KP_DEL, [0x64] = '\\',
    /* drivers/usbhid reports the modifiers as these, left and right merged */
    [0xE0] = K_CTRL, [0xE1] = K_SHIFT, [0xE2] = K_ALT,
};

static int mouse_dx, mouse_dy;
static uint8_t mouse_buttons;

/* ---- UART console: lines for the Quake console ---- */

#define NLINES 8
static char uart_line[256];
static int uart_len;
static char lines[NLINES][256];
static unsigned lines_wr, lines_rd;

static void uart_poll(void) {
    int ch;
    while ((ch = getchar_timeout_us(0)) >= 0) {
        platform_uart_cmd(ch); /* ~BOOT~, ~RST~ */
        if (ch == '\r' || ch == '\n') {
            if (uart_len && uart_line[0] != '~' && lines_wr - lines_rd < NLINES) {
                uart_line[uart_len] = 0;
                memcpy(lines[lines_wr % NLINES], uart_line, (size_t)uart_len + 1);
                lines_wr++;
            }
            uart_len = 0;
        } else if (ch == 8 || ch == 127) {
            if (uart_len) uart_len--;
        } else if (uart_len < (int)sizeof(uart_line) - 1 && ch >= 32) {
            uart_line[uart_len++] = (char)ch;
        }
    }
}

/* Engine interface (Sys_ConsoleInput): the next console line, NULL if none */
char *QG_ConsoleInput(void) {
    static char line[256];
    uart_poll();
    if (lines_rd == lines_wr) return NULL;
    strcpy(line, lines[lines_rd % NLINES]);
    lines_rd++;
    return line;
}

/* ---- USB ---- */

void q2_input_poll(void) {
    platform_usb_poll();
    uart_poll();

    uint8_t code;
    int down;
    while (usbhid_get_key_action(&code, &down)) {
        int key = hid_to_key[code];
        if (key) Quake2_SendKey(key, down);
    }

    if (usbhid_mouse_connected()) {
        usbhid_mouse_state_t m;
        usbhid_get_mouse_state(&m);
        mouse_dx += m.dx;
        mouse_dy += m.dy;
        uint8_t changed = m.buttons ^ mouse_buttons;
        for (int b = 0; b < 3; b++)
            if (changed & (1u << b)) Quake2_SendKey(K_MOUSE1 + b, (m.buttons >> b) & 1);
        mouse_buttons = m.buttons;
        if (m.wheel) {
            int key = m.wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN;
            Quake2_SendKey(key, true);
            Quake2_SendKey(key, false);
        }
    }
}

/* Mouse movement since the last call (the engine applies sensitivity) */
void QG_GetMouseMove(int *x, int *y) {
    *x = mouse_dx;
    *y = mouse_dy;
    mouse_dx = mouse_dy = 0;
}
