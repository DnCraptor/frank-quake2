#
# frank-fodder — Cannon Fodder port for Raspberry Pi Pico 2 (RP2350)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-fodder
#
# Licensed under the GNU General Public License v3.0 or later.
# See LICENSE for the full license text.
#

add_library(audio_driver STATIC
    ${CMAKE_CURRENT_LIST_DIR}/audio.c
)

target_include_directories(audio_driver PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_irq)

# Generate PIO header into audio_driver's binary dir
pico_generate_pio_header(audio_driver
    ${CMAKE_CURRENT_LIST_DIR}/audio_i2s.pio
)
