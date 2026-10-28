/* PS/2 Set 2 keyboard, PIO receiver adapted from frank-msx (GPL-2.0-or-later).
 * PIO1 IRQ1 moves bytes into a ring even while Quake II is busy rendering.
 * Exposes USB HID usages so both keyboards use Quake II's existing mapping.
 */
#include "ps2kbd.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "ps2kbd.pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#define RX_SIZE 128u /* power of two; one producer (IRQ), one consumer (core 0) */
static uint8_t rx[RX_SIZE];
static volatile uint32_t wr, rd;
static PIO kbd_pio;
static uint kbd_sm;
static bool extended, released;
static int pause_skip;

static void __not_in_flash_func(ps2_irq)(void) {
    while (!pio_sm_is_rx_fifo_empty(kbd_pio, kbd_sm)) {
        int byte = ps2_rx_decode_frame(pio_sm_get(kbd_pio, kbd_sm));
        if (byte < 0) continue;
        uint32_t w = wr;
        if (w - rd == RX_SIZE) continue; /* no overwrite of unread events */
        rx[w & (RX_SIZE - 1)] = (uint8_t)byte;
        __dmb();
        wr = w + 1;
    }
}

bool ps2kbd_init(PIO pio, uint clk_gpio) {
    if (!pio_can_add_program(pio, &ps2_rx_program)) return false;
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return false;
    kbd_pio = pio;
    kbd_sm = (uint)sm;
    uint offset = pio_add_program(pio, &ps2_rx_program);
    wr = rd = 0;
    extended = released = false;
    pause_skip = 0;
    ps2_rx_program_init(pio, kbd_sm, offset, clk_gpio);
    /* IRQ1: HDMI uses PIO0 and I2S uses DMA IRQs. */
    uint irq_num = pio == pio0 ? PIO0_IRQ_1 : PIO1_IRQ_1;
    irq_set_exclusive_handler(irq_num, ps2_irq);
    pio_set_irqn_source_enabled(pio, 1, pis_sm0_rx_fifo_not_empty + kbd_sm, true);
    irq_set_enabled(irq_num, true);
    return true;
}

/* Scan Code Set 2 -> HID keyboard usage ID (0 means not a Quake key). */
static const uint8_t set2_to_hid[256] = {
    [0x1c]=0x04, [0x32]=0x05, [0x21]=0x06, [0x23]=0x07, [0x24]=0x08,
    [0x2b]=0x09, [0x34]=0x0a, [0x33]=0x0b, [0x43]=0x0c, [0x3b]=0x0d,
    [0x42]=0x0e, [0x4b]=0x0f, [0x3a]=0x10, [0x31]=0x11, [0x44]=0x12,
    [0x4d]=0x13, [0x15]=0x14, [0x2d]=0x15, [0x1b]=0x16, [0x2c]=0x17,
    [0x3c]=0x18, [0x2a]=0x19, [0x1d]=0x1a, [0x22]=0x1b, [0x35]=0x1c,
    [0x1a]=0x1d,
    [0x16]=0x1e, [0x1e]=0x1f, [0x26]=0x20, [0x25]=0x21, [0x2e]=0x22,
    [0x36]=0x23, [0x3d]=0x24, [0x3e]=0x25, [0x46]=0x26, [0x45]=0x27,
    [0x5a]=0x28, [0x76]=0x29, [0x66]=0x2a, [0x0d]=0x2b, [0x29]=0x2c,
    [0x4e]=0x2d, [0x55]=0x2e, [0x54]=0x2f, [0x5b]=0x30, [0x5d]=0x31,
    [0x4c]=0x33, [0x52]=0x34, [0x0e]=0x35, [0x41]=0x36, [0x49]=0x37,
    [0x4a]=0x38, [0x58]=0x39,
    [0x05]=0x3a, [0x06]=0x3b, [0x04]=0x3c, [0x0c]=0x3d, [0x03]=0x3e,
    [0x0b]=0x3f, [0x83]=0x40, [0x0a]=0x41, [0x01]=0x42, [0x09]=0x43,
    [0x78]=0x44, [0x07]=0x45,
    [0x77]=0x53, [0x7c]=0x55, [0x7b]=0x56, [0x79]=0x57,
    [0x69]=0x59, [0x72]=0x5a, [0x7a]=0x5b, [0x6b]=0x5c, [0x73]=0x5d,
    [0x74]=0x5e, [0x6c]=0x5f, [0x75]=0x60, [0x7d]=0x61, [0x70]=0x62,
    [0x71]=0x63,
    [0x14]=0xe0, [0x12]=0xe1, [0x59]=0xe5, [0x11]=0xe2,
};

static uint8_t extended_to_hid(uint8_t scan) {
    switch (scan) {
    case 0x11: return 0xe6; /* right alt */
    case 0x14: return 0xe4; /* right ctrl */
    case 0x4a: return 0x54; /* keypad slash */
    case 0x5a: return 0x58; /* keypad enter */
    case 0x69: return 0x4d; /* End */
    case 0x6b: return 0x50; /* Left */
    case 0x6c: return 0x4a; /* Home */
    case 0x70: return 0x49; /* Insert */
    case 0x71: return 0x4c; /* Delete */
    case 0x72: return 0x51; /* Down */
    case 0x74: return 0x4f; /* Right */
    case 0x75: return 0x52; /* Up */
    case 0x7a: return 0x4e; /* Page down */
    case 0x7d: return 0x4b; /* Page up */
    default: return 0;
    }
}

int ps2kbd_get_key_action(uint8_t *hid, int *down) {
    while (rd != wr) {
        uint8_t b = rx[rd & (RX_SIZE - 1)];
        rd++;
        if (pause_skip) { --pause_skip; continue; }
        if (b == 0xe1) { pause_skip = 7; extended = released = false; continue; }
        if (b == 0xe0) { extended = true; continue; }
        if (b == 0xf0) { released = true; continue; }
        /* Keyboard power-on/reset responses aren't key presses. */
        if (b == 0xaa || b == 0xfa || b == 0xfe) {
            extended = released = false;
            continue;
        }
        uint8_t usage = extended ? extended_to_hid(b) : set2_to_hid[b];
        int pressed = !released;
        extended = released = false;
        if (usage) { *hid = usage; *down = pressed; return 1; }
    }
    return 0;
}
