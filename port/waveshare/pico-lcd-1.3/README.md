# Waveshare Pico-LCD-1.3 Port

Port of the EarthBound C reimplementation to the [Waveshare Pico-LCD-1.3](https://www.waveshare.com/wiki/Pico-LCD-1.3), an RP2040-based board with a 240x240 ST7789 IPS display, 4 buttons, and a 5-way joystick.

## Hardware

- **MCU:** RP2040 (Raspberry Pi Pico)
- **Flash:** 16 MB
- **Display:** 240x240 ST7789VW IPS LCD (SPI, 65K color)
- **Input:** 4 buttons (A/B/X/Y) + 5-way joystick (up/down/left/right/press)
- **Audio:** Disabled by default (RP2040 RAM too small for SPC700 emulator). PWM output on GPIO 0 when enabled.

### Button Mapping

| Board          | SNES   |
|----------------|--------|
| Joystick       | D-pad  |
| Joystick press | Start  |
| A              | A      |
| B              | B      |
| X              | X      |
| Y              | Y      |

Select, L, and R are currently unmapped (the board only has 9 inputs).

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

## Flashing

1. Hold the BOOTSEL button on the Pico and plug in USB
2. Drag `earthbound_pico.uf2` onto the `RPI-RP2` drive

### Audio

Audio is disabled by default — the RP2040's 264KB RAM is nearly filled by game state (~250KB BSS), leaving no room for the SPC700/DSP emulator's 64KB APU RAM. To enable audio (e.g. on an RP2350 with 520KB RAM), set `ENABLE_AUDIO ON` in `CMakeLists.txt`. PWM output goes to GPIO 0 (`PIN_AUDIO_PWM` in `board.h`); wire through an RC low-pass filter (1 kΩ + 100 nF) to an amplifier/speaker.

## Known Limitations

- **No audio** — RP2040 lacks sufficient RAM for the SPC700 emulator (see above).
- **Video performance** — the display path currently uses per-pixel writes via libdriver. A windowed write + DMA path is needed to hit 60fps.
- **Missing buttons** — Select, L, and R have no physical mapping. A chord/combo scheme (e.g. joystick press + A = Select) could be added.
