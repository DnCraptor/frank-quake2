/* NES/SNES PIO autopoll driver, adapted from frank-msx (GPL-3.0-or-later).
 * One PIO SM reads two adjacent DATA inputs at ~1 kHz; no main-loop delays.
 */
#include "nespad.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

static const uint16_t nespad_instructions[] = {
    0xea01, /* set pins, 1 side 0 [10]: latch high */
    0xe02f, /* set x, 15       side 0: 16 buttons */
    0xe000, /* set pins, 0    side 0: latch low */
    0x4402, /* in pins, 2     side 0 [4]: DATA 1+2 */
    0xf500, /* set pins, 0    side 1 [5]: clock high */
    0x0043, /* jmp x--, 3     side 0: next button */
    0xe03f, /* set x, 31      side 0: delay counter */
    0x0f47, /* jmp x--, 7     side 0 [15]: inter-poll delay */
};
static const struct pio_program nespad_program = {
    .instructions = nespad_instructions, .length = 8, .origin = -1
};
static PIO pad_pio = pio1;
static int pad_sm = -1;
uint32_t nespad_state, nespad_state2;

bool nespad_begin(uint32_t cpu_khz, uint8_t clk_pin, uint8_t data_pin, uint8_t latch_pin) {
    if (pad_sm >= 0 || !pio_can_add_program(pad_pio, &nespad_program)) return false;
    int sm = pio_claim_unused_sm(pad_pio, false);
    if (sm < 0) return false;
    pad_sm = sm;
    uint offset = pio_add_program(pad_pio, &nespad_program);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 7);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, clk_pin);
    sm_config_set_in_pins(&c, data_pin);
    sm_config_set_set_pins(&c, latch_pin, 1);
    pio_gpio_init(pad_pio, clk_pin);
    pio_gpio_init(pad_pio, data_pin);
    pio_gpio_init(pad_pio, data_pin + 1);
    pio_gpio_init(pad_pio, latch_pin);
    gpio_pull_up(clk_pin);
    gpio_pull_up(data_pin);
    gpio_pull_up(data_pin + 1);
    gpio_pull_up(latch_pin);
    uint32_t output_mask = (1u << clk_pin) | (1u << latch_pin);
    uint32_t input_mask = (1u << data_pin) | (1u << (data_pin + 1));
    pio_sm_set_pindirs_with_mask(pad_pio, pad_sm, output_mask,
                                 output_mask | input_mask);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    /* 1 MHz PIO instructions, hence ~1 ms between reads. */
    sm_config_set_clkdiv(&c, (float)cpu_khz / 1000.0f);
    pio_sm_init(pad_pio, pad_sm, offset, &c);
    pio_sm_set_enabled(pad_pio, pad_sm, true);
    return true;
}

void nespad_read(void) {
    if (pad_sm < 0) return;
    uint32_t bits = 0;
    bool got = false;
    while (!pio_sm_is_rx_fifo_empty(pad_pio, (uint)pad_sm)) {
        bits = pio_sm_get(pad_pio, (uint)pad_sm);
        got = true;
    }
    if (!got) return;
    bits = ~bits;
    uint32_t a = bits & 0x555555u;
    uint32_t b = (bits >> 1) & 0x555555u;
    /* An unplugged input is pulled high; all 12 buttons cannot be pressed. */
    nespad_state = (a == 0x555555u) ? 0 : a;
    nespad_state2 = (b == 0x555555u) ? 0 : b;
}
