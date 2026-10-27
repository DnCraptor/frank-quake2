# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
#
# frank-quake2 — Quake II for RP2350 (FRANK / Murmulator M2 boards)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
#
# Licensed under the GNU General Public License v3.0 or later.
# The engine (src/quake2) is id Software's Quake II, GPL-2.0-or-later.
#

cmake_minimum_required(VERSION 3.13)

# M2: Murmulator 2 (HSTX HDMI on GPIO 12-19, I2S DAC on 9-11, SD on SPI0).
# Z0: Waveshare RP2350-PiZero (PIO HDMI on GPIO 32-39, an I2S module on
#     10-12, SD on SPI1); I2S sound only.
set(BOARD_VARIANT "M2" CACHE STRING "Board: M2 or Z0")
set_property(CACHE BOARD_VARIANT PROPERTY STRINGS M2 Z0)
if(BOARD_VARIANT STREQUAL "M2")
    set(PICO_BOARD frank_m2_rp2350b)
elseif(BOARD_VARIANT STREQUAL "Z0")
    set(PICO_BOARD frank_z0_rp2350b)
else()
    message(FATAL_ERROR "BOARD_VARIANT must be M2 or Z0")
endif()
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")
# Tuned on the frank-castle dev board with the diag firmware ('r' sweeps the
# PSRAM read delay, 'f' the flash one); use 133 MHz PSRAM if a board misbehaves.
# Z0: untuned yet, 133 MHz (504 / 4 = 126) with the derived read delay.
if(BOARD_VARIANT STREQUAL "Z0")
    set(PSRAM_SPEED_DEFAULT "133")
    set(PSRAM_RXDELAY_DEFAULT "")
else()
    set(PSRAM_SPEED_DEFAULT "168")
    set(PSRAM_RXDELAY_DEFAULT "6")
endif()
set(PSRAM_SPEED "${PSRAM_SPEED_DEFAULT}" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 168")
set(PSRAM_RXDELAY "${PSRAM_RXDELAY_DEFAULT}" CACHE STRING "PSRAM read sampling delay override (empty: derived from the clock)")
set(FLASH_SPEED "126" CACHE STRING "Flash chip max frequency in MHz")
set(FLASH_RXDELAY "5" CACHE STRING "Flash read sampling delay override (empty: derived from the clock)")
# The model/skin image (tools/mkassets.py) is flashed here; the firmware must end below it.
set(Q2_ASSETS_OFFSET "0x1C0000" CACHE STRING "Flash offset of the asset image")
set(Q2_SAMPLE_RATE "11025" CACHE STRING "Sound output rate: 11025 or 22050")
# One output per build: I2S (the board's DAC, GPIO 9-11) or HDMI audio
# (44.1 kHz data islands, 4x the 11025 Hz mix)
set(AUDIO_TYPE "I2S" CACHE STRING "Audio output: I2S or HDMI")
set_property(CACHE AUDIO_TYPE PROPERTY STRINGS I2S HDMI)
if(NOT AUDIO_TYPE STREQUAL "I2S" AND NOT AUDIO_TYPE STREQUAL "HDMI")
    message(FATAL_ERROR "AUDIO_TYPE must be I2S or HDMI")
endif()
if(AUDIO_TYPE STREQUAL "HDMI" AND NOT Q2_SAMPLE_RATE STREQUAL "11025")
    message(FATAL_ERROR "HDMI audio needs Q2_SAMPLE_RATE 11025 (a quarter of 44.1 kHz)")
endif()
# 3: the game never waits for vsync to free a buffer, but the 76.8 KB it
# takes leaves no room for the z-buffer in SRAM, which is worth more
set(Q2_FB_COUNT "2" CACHE STRING "8-bit framebuffers in SRAM: 2 or 3")
# Native USB is always the keyboard/mouse host; debug console is UART0 (GPIO0/1).
set(USB_HID_ENABLED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

# clk_peri (UART, SD card SPI): 0 keeps the 48 MHz USB PLL (SPI 24 MHz at most)
set(PERI_SPEED "168" CACHE STRING "clk_peri in MHz, from clk_sys (0: 48 MHz from the USB PLL)")
set(SD_SPEED "42" CACHE STRING "SD card SPI clock in MHz (clk_peri / an even divider)")
if(BOARD_VARIANT STREQUAL "Z0")
    set(SD_SPI_BUS spi1)
    set(SD_PIN_SCK 30)
    set(SD_PIN_MOSI 31)
    set(SD_PIN_MISO 40)
    set(SD_PIN_CS 43)
    set(I2S_DATA_PIN 10)
    set(I2S_CLK_PIN 11) # and LRCK 12
    set(I2S_PIO pio1)   # pio0 is the video's, its GPIO window starts at 16
    set(Q2_VIDEO_PIO 1)
else()
    set(SD_SPI_BUS spi0)
    set(SD_PIN_SCK 6)
    set(SD_PIN_MOSI 7)
    set(SD_PIN_MISO 4)
    set(SD_PIN_CS 5)
    set(I2S_DATA_PIN 9)
    set(I2S_CLK_PIN 10)
    set(I2S_PIO pio0)
    set(Q2_VIDEO_PIO 0)
    # a test of the Z0's PIO HDMI driver on an M2 (its HDMI pins, the
    # frank-msx M2 layout: clock pair first, RGB, the pairs inverted)
    option(M2_VIDEO_PIO "M2: PIO HDMI instead of HSTX (to test the Z0 video)" OFF)
    if(M2_VIDEO_PIO)
        set(Q2_VIDEO_PIO 1)
        set(HDMI_PIO_DEFINES HDMI_PIO_BASE_PIN=12 HDMI_PIO_Z0_LAYOUT=0)
    endif()
endif()

include(pico_sdk_import.cmake)

project(frank-quake2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" Q2_VERSION_RAW)
string(STRIP "${Q2_VERSION_RAW}" Q2_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" ";" Q2_VERSION_PARTS "${Q2_VERSION_RAW}")
list(GET Q2_VERSION_PARTS 0 Q2_VERSION_MAJOR)
list(GET Q2_VERSION_PARTS 1 Q2_VERSION_MINOR)
if(Q2_VERSION_MINOR LESS 10)
    set(Q2_VERSION "${Q2_VERSION_MAJOR}.0${Q2_VERSION_MINOR}")
else()
    set(Q2_VERSION "${Q2_VERSION_MAJOR}.${Q2_VERSION_MINOR}")
endif()

pico_sdk_init()

# ---------------------------------------------------------------------
# Drivers (shared with the other FRANK ports)
# ---------------------------------------------------------------------
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/usbhid)
add_subdirectory(drivers/audio)

if(Q2_VIDEO_PIO)
    set(VIDEO_SOURCES drivers/hdmi_pio/hdmi_pio.c drivers/hdmi/hstx_packet.c)
else()
    set(VIDEO_SOURCES
        drivers/hdmi/psx_video.c
        drivers/hdmi/hstx_packet.c
        drivers/hdmi/hstx_data_island_queue.c
    )
endif()
# the PIO HDMI's IRQ code must stay in SRAM: no loops turned into newlib calls
set_source_files_properties(drivers/hdmi_pio/hdmi_pio.c PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
add_library(drivers STATIC
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/psram_heap.c
    drivers/dlmalloc.c
    ${VIDEO_SOURCES}
)
target_include_directories(drivers PUBLIC drivers drivers/hdmi drivers/hdmi_pio)
target_link_libraries(drivers PUBLIC pico_stdlib hardware_dma hardware_irq hardware_clocks hardware_gpio hardware_pio)
set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1"
)
target_compile_definitions(drivers PUBLIC
    ${HDMI_PIO_DEFINES}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    $<$<BOOL:${PSRAM_RXDELAY}>:PSRAM_RXDELAY=${PSRAM_RXDELAY}>
    PSXV_PIN_BASE=12
    $<$<BOOL:${PSRAM_COOLDOWN}>:PSRAM_COOLDOWN=${PSRAM_COOLDOWN}>
)

set(Q2_DEFINES
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PERI_CLOCK_MHZ=${PERI_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    $<$<BOOL:${PSRAM_RXDELAY}>:PSRAM_RXDELAY=${PSRAM_RXDELAY}>
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    $<$<BOOL:${FLASH_RXDELAY}>:FLASH_RXDELAY=${FLASH_RXDELAY}>
    PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLK_PIN}
    Q2_VERSION="${Q2_VERSION}"
    Q2_ASSETS_OFFSET=${Q2_ASSETS_OFFSET}
    Q2_SAMPLE_RATE=${Q2_SAMPLE_RATE}
    Q2_AUDIO_${AUDIO_TYPE}=1
    Q2_VIDEO_PIO=${Q2_VIDEO_PIO}
    Q2_I2S_PIO=${I2S_PIO}
    Q2_FB_COUNT=${Q2_FB_COUNT}
)

# Board bring-up and the C heap, shared by the diag and game firmware
add_library(q2_platform STATIC
    src/common/font6x8.c
    src/common/fbcon.c
    src/rp2350/platform.c
    src/rp2350/uart_buffered.c
    src/rp2350/q2_malloc.c
)
target_include_directories(q2_platform PUBLIC src/common src/rp2350 src/rp2350/compat)
target_compile_definitions(q2_platform PUBLIC ${Q2_DEFINES})
target_link_libraries(q2_platform PUBLIC
    pico_stdlib pico_multicore hardware_clocks hardware_vreg hardware_watchdog
    drivers fatfs sdcard usbhid audio_driver)

# ---------------------------------------------------------------------
# Phase 0: hardware bring-up firmware
# ---------------------------------------------------------------------
if(NOT Q2_VIDEO_PIO) # the diag firmware is HSTX only
add_executable(frank-quake2-diag src/diag/diag_main.c)
target_link_libraries(frank-quake2-diag q2_platform)
target_link_options(frank-quake2-diag PRIVATE -Wl,--wrap=panic
    -Wl,--defsym=__q2_assets_offset=${Q2_ASSETS_OFFSET})
pico_set_linker_script(frank-quake2-diag ${CMAKE_CURRENT_SOURCE_DIR}/memmap_quake2.ld)
pico_enable_stdio_usb(frank-quake2-diag 0)
pico_enable_stdio_uart(frank-quake2-diag 1)
pico_add_extra_outputs(frank-quake2-diag)
endif()

# ---------------------------------------------------------------------
# The engine: Quake II from the ESP32-S3 port (quake2generic lineage),
# with the game (baseq2) and the software renderer linked in.
# ---------------------------------------------------------------------
set(Q2_DIR ${CMAKE_CURRENT_LIST_DIR}/src/quake2)

set(Q2_REF_SOFT_SOURCES
    r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_image.c r_light.c r_main.c
    r_misc.c r_model.c r_part.c r_poly.c r_polyse.c r_rast.c r_scan.c r_sprite.c r_surf.c
)
list(TRANSFORM Q2_REF_SOFT_SOURCES PREPEND ${Q2_DIR}/ref_soft/)

set(Q2_GAME_SOURCES
    g_ai.c g_chase.c g_cmds.c g_combat.c g_func.c g_items.c g_main.c g_misc.c
    g_monster.c g_phys.c g_save.c g_spawn.c g_svcmds.c g_target.c g_trigger.c
    g_turret.c g_utils.c g_weapon.c m_actor.c m_berserk.c m_boss2.c m_boss3.c
    m_boss31.c m_boss32.c m_brain.c m_chick.c m_flash.c m_flipper.c m_float.c
    m_flyer.c m_gladiator.c m_gunner.c m_hover.c m_infantry.c m_insane.c
    m_medic.c m_move.c m_mutant.c m_parasite.c m_soldier.c m_supertank.c
    m_tank.c p_client.c p_hud.c p_trail.c p_view.c p_weapon.c q_shared.c
)
list(TRANSFORM Q2_GAME_SOURCES PREPEND ${Q2_DIR}/game/)

set(Q2_CORE_SOURCES
    client/cl_cin.c client/cl_ents.c client/cl_fx.c client/cl_input.c
    client/cl_inv.c client/cl_main.c client/cl_newfx.c client/cl_parse.c
    client/cl_pred.c client/cl_scrn.c client/cl_tent.c client/cl_view.c
    client/console.c client/keys.c client/menu.c client/qmenu.c
    client/snd_dma.c client/snd_mem.c client/snd_mix.c
    server/sv_ccmds.c server/sv_ents.c server/sv_game.c server/sv_init.c
    server/sv_main.c server/sv_send.c server/sv_user.c server/sv_world.c
    qcommon/cmd.c qcommon/cmodel.c qcommon/common.c qcommon/crc.c
    qcommon/cvar.c qcommon/files.c qcommon/md4.c qcommon/net_chan.c
    qcommon/pmove.c qcommon/q_perf.c
    other/glob.c other/q_arena.c other/q_assets.c other/q_hunk.c
    other/q_system.c other/vid_lib.c other/vid_menu.c
    net/net_loop.c
    null/cd_null.c
    port_esp32.c
)
list(TRANSFORM Q2_CORE_SOURCES PREPEND ${Q2_DIR}/)

add_library(quake2_core OBJECT ${Q2_CORE_SOURCES} ${Q2_REF_SOFT_SOURCES} ${Q2_GAME_SOURCES})
target_include_directories(quake2_core PRIVATE ${Q2_DIR} src/rp2350/compat src/rp2350 src/common)
# ESP_PLATFORM: the ESP32 code paths of the engine (pak handle pool, big
# stdio buffers, EXT_RAM_BSS tables in PSRAM) apply here as they are.
# QUAKE_HEAP_RESERVE: PSRAM left to the C heap next to the engine arena
# (stdio buffers of the open paks, FatFs cluster maps, the profiler)
target_compile_definitions(quake2_core PRIVATE ESP_PLATFORM=1 PLATFORM_RP2350=1 ${Q2_DEFINES}
    QUAKE_HEAP_RESERVE=196608)
# 1997 C: no warnings. The engine expects signed chars (ARM's are unsigned).
# -fsingle-precision-constant: the FPU is single precision only.
target_compile_options(quake2_core PRIVATE -w -fsigned-char -fno-strict-aliasing
    -fsingle-precision-constant -O2)
set_source_files_properties(${Q2_REF_SOFT_SOURCES} TARGET_DIRECTORY quake2_core PROPERTIES
    COMPILE_OPTIONS "-O3;-funroll-loops;-fno-math-errno")
# (no libraries: INTERFACE libraries would compile their sources into it again)

add_executable(frank-quake2
    src/rp2350/main.c
    src/rp2350/q2_port.c
    src/rp2350/q2_video.c
    src/rp2350/q2_shell.c
    src/rp2350/q2_sound.c
    src/rp2350/q2_input.c
    src/rp2350/q2_fs.c
    src/rp2350/q2_prof.c
    src/rp2350/q2_cmds.c
    src/rp2350/q2_assets_flash.c
    $<TARGET_OBJECTS:quake2_core>
)
# IRQ code must stay in SRAM: stop GCC from turning copy loops into calls
# to newlib's memcpy/memset.
set_source_files_properties(src/rp2350/q2_video.c src/rp2350/q2_shell.c src/rp2350/q2_sound.c
    PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
target_include_directories(frank-quake2 PRIVATE ${Q2_DIR})
target_compile_definitions(frank-quake2 PRIVATE ESP_PLATFORM=1 PLATFORM_RP2350=1)
target_compile_options(frank-quake2 PRIVATE -fsigned-char)
target_compile_definitions(frank-quake2 PRIVATE PICO_MALLOC_PANIC=0)
target_compile_definitions(frank-quake2 PRIVATE ${Q2_DEFINES})
target_link_libraries(frank-quake2 q2_platform hardware_dma hardware_xip_cache hardware_flash pico_multicore pico_sync)
pico_set_linker_script(frank-quake2 ${CMAKE_CURRENT_SOURCE_DIR}/memmap_quake2.ld)
target_link_options(frank-quake2 PRIVATE
    -Wl,--no-enum-size-warning
    -Wl,--wrap=panic
    -Wl,--wrap=fclose
    -Wl,--defsym=__q2_assets_offset=${Q2_ASSETS_OFFSET}
    -Wl,--print-memory-usage
)
set_property(TARGET frank-quake2 APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/memmap_quake2.ld)
pico_enable_stdio_usb(frank-quake2 0)
pico_enable_stdio_uart(frank-quake2 1)
pico_add_extra_outputs(frank-quake2)
