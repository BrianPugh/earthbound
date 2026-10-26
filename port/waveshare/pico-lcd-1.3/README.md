# Waveshare Pico-LCD-1.3 Port

Port of the EarthBound C reimplementation to the [Waveshare Pico-LCD-1.3](https://www.waveshare.com/wiki/Pico-LCD-1.3), an RP2040-based board with a 240x240 ST7789 IPS display, 4 buttons, and a 5-way joystick.

## Hardware

- **MCU:** RP2040 (Raspberry Pi Pico)
- **Flash:** 16 MB
- **Display:** 240x240 ST7789VW IPS LCD (SPI, 65K color)
- **Input:** 4 buttons (A/B/X/Y) + 5-way joystick (up/down/left/right/press)
- **Audio:** Disabled by default (RP2040 RAM too small for SPC700 emulator). PWM output on GPIO 0 when enabled.

### Button Mapping

| Board    | SNES  | In game |
|----------|-------|---------|
| Joystick | D-pad | Move, menu navigation |
| X        | B     | Cancel/back; HP/PP display on the overworld |
| Y        | L     | Talk/check, confirm |
| B        | X     | Town map |
| A        | —     | Toggle the FPS counter |
| Joystick press | — | Cycle the display gamma (100, 120, 140, 160, 180; shown as `GAM` on the FPS overlay) |

SNES A, Y, Select, Start and R have no button.

The panel shows the SNES palette washed out, so the build darkens the midtones with a display gamma: each 5-bit channel becomes `31 * (c / 31)^(gamma / 100)`, applied when the palette is converted (no per-pixel cost). The boot value is `EB_DISPLAY_GAMMA` (default 120, configure with `-DEB_DISPLAY_GAMMA=...`); 100 is the direct colour mapping.

Nothing sends SNES A, which opens the command window on the overworld. The original game has a second route to it: X (B) opens the HP/PP display, and Y (L) there opens the command window, so it takes 2 presses. Y talks/checks on the overworld; in menus X cancels and Y confirms. The Game & Watch port uses the same layout.

## Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) installed and `PICO_SDK_PATH` set
- `pico_sdk_import.cmake` copied into this directory (from `$PICO_SDK_PATH/external/`)
- Game assets extracted via `ebtools extract` (see top-level README)
- ARM cross-compiler (`arm-none-eabi-gcc`)

## Building

```bash
# From the repo root, extract assets first
source .venv/bin/activate
ebtools extract

# Copy the SDK import helper
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake port/waveshare/pico-lcd-1.3/

# Build
cd port/waveshare/pico-lcd-1.3
cmake -S . -B build
cmake --build build
```

This produces `build/earthbound_pico.uf2`.

### RAM budget

Every link runs `cmake/check_ram_budget.cmake`, which fails the build when `.data + .bss` exceeds `RAM_BUDGET_BYTES` (set in `CMakeLists.txt`; `.data` includes the `.time_critical` PPU code copied to SRAM). `arm-none-eabi-size -A build/earthbound_pico.elf` on 2026-10-25 (pico-sdk 2.2.0, arm-none-eabi-gcc 15.3, Release):

| Section | Dual core (default) | Single core |
|---|---|---|
| `.ram_vector_table` | 192 | 192 |
| `.data` | 35,472 | 34,616 |
| `.bss` | 141,944 | 133,492 |
| `.scratch_x` | 960 (core 1's DMA line buffers) + 3,072 core 1 stack | 4,096 (save sector buffer) |
| `.stack_dummy` (core 0, SCRATCH_Y) | 2,048 | 2,048 |
| **`.data + .bss`** | **177,416** (budget 181,512) | **168,108** |

Main SRAM is 256 KB and holds only the vector table, `.data` and `.bss` (the heap is empty), so about 83 KB of it is free; the core 0 stack has the 4 KB SCRATCH_Y bank to itself.

### Dual-core rendering

`EB_ENABLE_DUAL_CORE_PPU` (ON by default) splits each frame between the cores: core 0 renders the top half of the screen and core 1 the bottom half, each with its own render context, so both keep the renderer's per-tile-row reuse. The halves take turns on the SPI bus a line at a time, and each line first moves the LCD's write pointer to its row; the renderer draws straight into the DMA buffers. Core 1's tile cache is half size, and in-game saves borrow core 0's render scratch for the 4 KB flash sector buffer (saves run between frames). Measured outside Ness's house at night (FPS overlay): 40.6 fps single core, 52.4 even/odd scanline split, 56.6 this band split. Configure with `-DEB_ENABLE_DUAL_CORE_PPU=OFF` for the single-core build.

## Flashing

1. Hold the BOOTSEL button on the Pico and plug in USB
2. Drag `earthbound_pico.uf2` onto the `RPI-RP2` drive

To start from a save (e.g. one of the playtest saves, to skip the intro), bundle it into the UF2 first; this replaces any save on the board:

```bash
python tools/uf2_add_save.py build/earthbound_pico.uf2 ../../../playtest/saves/01-Start.srm earthbound_pico_save.uf2
```

The save region holds the same raw 8 KB SRAM image as the desktop port's `earthbound.srm`.

### Audio

Audio is disabled by default. The SPC700/DSP emulator needs ~70 KB (64 KB of APU RAM), which fits in the ~83 KB of free main SRAM, but the APU state is heap-allocated (the Pico heap is empty) and the RP2040's CPU budget for emulating it is unmeasured. To enable audio (e.g. on an RP2350 with 520KB RAM), set `ENABLE_AUDIO ON` in `CMakeLists.txt`. PWM output goes to GPIO 0 (`PIN_AUDIO_PWM` in `board.h`); wire through an RC low-pass filter (1 kΩ + 100 nF) to an amplifier/speaker.

## Known Limitations

- **No audio** — the SPC700 emulator is not built for the RP2040 (see above).
- **Video performance** — the display path currently uses per-pixel writes via libdriver. A windowed write + DMA path is needed to hit 60fps.
- **Missing buttons** — Select, L, and R have no physical mapping. A chord/combo scheme (e.g. joystick press + A = Select) could be added.
