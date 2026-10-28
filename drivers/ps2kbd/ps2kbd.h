/* PS/2 Set 2 keyboard input; scan events are exposed as USB HID usages.
 * Derived from frank-msx PS/2 receiver (GPL-2.0-or-later).
 */
#ifndef Q2_PS2KBD_H
#define Q2_PS2KBD_H
#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"

bool ps2kbd_init(PIO pio, uint clk_gpio);
/* Returns one HID keyboard usage plus make/break, or 0 if none pending. */
int ps2kbd_get_key_action(uint8_t *hid, int *down);
#endif
