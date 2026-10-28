/* NES/SNES pads over PIO, adapted from frank-msx (GPL-3.0-or-later). */
#ifndef Q2_NESPAD_H
#define Q2_NESPAD_H
#include <stdbool.h>
#include <stdint.h>

#define DPAD_A      0x000001u
#define DPAD_B      0x000004u
#define DPAD_SELECT 0x000010u
#define DPAD_START  0x000040u
#define DPAD_UP     0x000100u
#define DPAD_DOWN   0x000400u
#define DPAD_LEFT   0x001000u
#define DPAD_RIGHT  0x004000u
#define DPAD_Y      0x010000u
#define DPAD_X      0x040000u
#define DPAD_LT     0x100000u
#define DPAD_RT     0x400000u

extern uint32_t nespad_state, nespad_state2;
bool nespad_begin(uint32_t cpu_khz, uint8_t clk_pin, uint8_t data_pin, uint8_t latch_pin);
void nespad_read(void);
#endif
