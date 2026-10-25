# frank-quake2

Quake II for the RP2350 (FRANK / Murmulator M2 boards): HDMI via HSTX,
8 MB PSRAM, I2S audio, USB keyboard and mouse, game data on the SD card.

The engine is the ESP32-S3 port of Quake II
([recrof/Quake2-ESP32S3](https://github.com/recrof/Quake2-ESP32S3), id's
source through quake2generic), with the software renderer and the game
(baseq2) linked in. The platform layer (drivers, clocks, linker layout) comes
from frank-castle. Work in progress.

- 320x240 8-bit software rendering, shown doubled on 640x480@60 HDMI
- both cores render (core 1 between its HDMI interrupts): the world spans,
  the model triangles and the surface cache are split between them, the
  edge sorting and the world are pipelined across them
- single player only (no network)
- demo1 timedemo: ~33 fps (689 frames in ~20.5 s); 30-45 fps in real-time
  play

## SD card

Copy the retail game data (patched to 3.20) to `/quake2/baseq2/`:

```
/quake2/baseq2/pak0.pak
/quake2/baseq2/pak1.pak
/quake2/baseq2/pak2.pak
```

Saves and `config.cfg` are written there too. An optional `/quake2/args.txt`
adds to the command line (e.g. `+map base1`). No game data is included in
this repository.

## Models and skins in flash

8 MB of PSRAM is not enough for the biggest maps unless the models and their
skins are used straight from flash, as on the ESP32-S3. Build the image once
from your paks and flash it next to the firmware (it stays across firmware
updates):

```
tools/mkassets.py assets.bin pak2.pak pak1.pak pak0.pak
./flash.sh assets assets.bin       # picotool, at 0x101C0000
```

It is required: without it even the first maps run out of memory.

## Building

```
./build.sh            # M2, 504 MHz CPU, 168 MHz PSRAM, 126 MHz flash
./flash.sh            # picotool, board in BOOTSEL
./flash.sh frank-quake2-diag   # bring-up firmware (HDMI, PSRAM, SD, I2S, USB)
```

Options (cmake `-D...`):

- `FLASH_SPEED=168 FLASH_RXDELAY=6`: flash at 168 MHz instead of 126, about
  2.5% faster (the models and skins are read from flash). Above the flash
  chip's rating; it ran a 15-minute demo loop without a fault on the dev
  board, but test yours.
- `PSRAM_SPEED`, `PSRAM_RXDELAY`, `SD_SPEED` (SPI MHz, 42), `PERI_SPEED`
  (clk_peri MHz, 168), `Q2_FB_COUNT`, `Q2_SAMPLE_RATE`.

Faster, blurrier textures (console or `/quake2/args.txt`): `sw_mipcap 2`
(+9% in demo1) or `sw_mipscale 2` (+5%).

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
