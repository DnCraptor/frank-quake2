# frank-quake2

Quake II for the RP2350: FRANK / Murmulator M2 boards (HDMI via HSTX, sound
over HDMI or from the I2S DAC) and the Waveshare RP2350-PiZero, "Z0" (HDMI
via PIO, sound from an I2S DAC module); 8 MB PSRAM, USB keyboard and mouse,
game data on the SD card.

The engine is the ESP32-S3 port of Quake II
([recrof/Quake2-ESP32S3](https://github.com/recrof/Quake2-ESP32S3), id's
source through quake2generic), with the software renderer and the game
(baseq2) linked in. The platform layer (drivers, clocks, linker layout) comes
from frank-castle. Work in progress.

- 320x240 8-bit software rendering, shown doubled on 640x480@60 HDMI
- both cores render (core 1 between its HDMI interrupts): the world spans,
  the model triangles and the surface cache are split between them, the
  edge sorting and the world are pipelined across them
- sound: 11025 Hz 16-bit stereo, over HDMI (44.1 kHz audio packets, about
  1.5% slower) or from the board's I2S DAC: one firmware for each
- single player only (no network)
- demo1 timedemo: ~33 fps (689 frames in ~20.5 s); 30-45 fps in real-time
  play

## Installing

1. Pick the release's firmware for your board and sound output:
   - M2: `m2p2_frank-quake2_hdmi_A_BB.uf2` (the TV's speakers) or
     `m2p2_frank-quake2_i2s_A_BB.uf2` (the board's I2S DAC);
   - Z0 (Waveshare RP2350-PiZero): `z0p2_frank-quake2_i2s_A_BB.uf2`, see
     below.

   Hold BOOTSEL, plug the board in, and copy it to the RP2350 drive (or
   `picotool load -f FILE.uf2`).
2. Copy the retail game data (patched to 3.20) to the SD card (FAT32 or
   exFAT):

   ```
   /quake2/baseq2/pak0.pak
   /quake2/baseq2/pak1.pak
   /quake2/baseq2/pak2.pak
   ```

3. Start the board. The first start builds the models and skins into flash
   from the paks (about 90 seconds, once) and restarts; then the game starts.

Saves and `config.cfg` are written to `/quake2/baseq2/`. An optional
`/quake2/args.txt` adds to the command line (e.g. `+map base1`). No game data
is included in this repository or in the releases.

### Z0 (Waveshare RP2350-PiZero)

- Picture on the mini-HDMI port (PIO on GPIO 32-39); no HDMI sound.
- Sound from an I2S DAC module (PCM5102A): DIN GP10, BCK GP11, LRCK GP12.
- SD card slot (SPI1: 30/31/40/43), PSRAM on board (CS GPIO47).
- Keyboard and mouse on the native USB port (USB host, through an OTG
  adapter or hub). The debug UART (GPIO0/1) is not on its header.
- PSRAM at 504 / 4 = 126 MHz (the M2 runs it at 168, tuned there): somewhat
  slower until it is tuned on a Z0.

## Models and skins in flash

8 MB of PSRAM is not enough for the maps unless the models and their skins
are used straight from flash, as on the ESP32-S3: the image (about 14 MB,
at 0x101C0000) is built on the board from the paks, again when the paks
change, or when `/quake2/assets.rebuild` exists on the card (deleted after
the build). It stays across firmware updates.

The board's flash size is detected. With 8 MB of flash the image holds about
6 MB (the biggest models and skins are left out and load to PSRAM; big maps
may then run out of memory); 2 MB is not enough for the image. Firmware 1.01
and older assumed 16 MB: on an 8 MB board the build stopped at 37 % and
erased the firmware. Flash the new UF2 again (the SD card is not touched).

It can also be built on a PC and flashed (the board then keeps it as it is):

```
tools/mkassets.py assets.bin pak2.pak pak1.pak pak0.pak
./flash.sh assets assets.bin       # picotool, at 0x101C0000
```

The PC image is for 16 MB of flash; for a smaller chip pass
`--size $(( (8 << 20) - 0x1C0000 ))` (8 MB) to mkassets.py.

## Building

```
./build.sh            # M2, 504 MHz CPU, 168 MHz PSRAM, 126 MHz flash, I2S sound
AUDIO=HDMI ./build.sh # sound over HDMI
./build.sh Z0         # Waveshare RP2350-PiZero
./flash.sh            # picotool, board in BOOTSEL
./swd_flash.sh        # or over SWD with a Debugprobe
./flash.sh frank-quake2-diag   # bring-up firmware (HDMI, PSRAM, SD, I2S, USB)
./release.sh [A.BB] [--publish]  # release/m2p2_frank-quake2_{hdmi,i2s}_A_BB.uf2 (+ GitHub release)
```

Options (cmake `-D...`):

- `BOARD_VARIANT=M2` (the default) or `Z0`.
- `AUDIO_TYPE=HDMI` or `I2S` (the default): the sound output (Z0: I2S only).
- `FLASH_SPEED=168 FLASH_RXDELAY=6`: flash at 168 MHz instead of 126, about
  2.5% faster (the models and skins are read from flash). Above the flash
  chip's rating; it ran a 15-minute demo loop without a fault on the dev
  board, but test yours.
- `PSRAM_SPEED`, `PSRAM_RXDELAY`, `SD_SPEED` (SPI MHz, 42), `PERI_SPEED`
  (clk_peri MHz, 168), `Q2_FB_COUNT`, `Q2_SAMPLE_RATE`.

Faster, blurrier textures (console or `/quake2/args.txt`): `sw_mipcap 2`
(+9% in demo1) or `sw_mipscale 2` (+5%). `vsync 0` stops waiting for the
page flip at 60 Hz: a few % faster, but the picture tears.

Debug console: UART0 on GPIO0/1, 115200. Lines typed there go to the Quake
console; `~BOOT~` reboots into BOOTSEL.

## Development

- `tools/dev.sh`: rebuild, reflash over the UART, capture the boot log
- `tools/bench.sh`: demo1 timedemo with the perf recorder and the profiler
- `tools/uart.py`: console lines, scripted steps, log capture
- `tools/grab.sh`: a frame from an HDMI capture card
- console: `prof N` (PC sampling, `tools/prof.py` maps it), `perf start` /
  `perf sum`, `rstats` (edges, surfaces, XIP cache misses per renderer
  phase, waits between the cores), `r_dspeeds 1` (`tools/dspeeds.py`),
  `sdtest`, `sysmem`
- `sw_pscan 0`, `sw_pworld 0`: the single-core edge scan and world paths
- `tools/install-hooks.sh`: git hooks of this repository

## License

GPL-3.0-or-later. The Quake II engine (`src/quake2`) is GPL-2.0-or-later
(id Software, see `LICENSE.quake2`). Drivers carry their own headers:
frank-fodder drivers (GPL-3.0-or-later), pico_hdmi (public domain), FatFs
(BSD-style), sdcard (BSD-2).
