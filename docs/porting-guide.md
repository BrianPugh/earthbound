# Platform Porting Guide

This guide explains how to port the EarthBound C reimplementation to a new platform. The game logic lives in `src/` as a static library (`libgame.a`); each port provides a thin platform layer implementing `platform.h` plus a `main()` entry point.

## Architecture Overview

```
src/                          ← Platform-agnostic game library (libgame.a)
  core/                         Memory, math, decompression
  entity/                       Entity system, scripts, sprites
  game/                         Game logic (overworld, battle, text, menus)
  intro/                        Intro, title, file select
  snes/                         PPU renderer, DMA emulation
  platform/platform.h           ← THE interface your port implements
  game_main.h                   ← Entry points: game_init(), game_logic_entry()
  core/types.h                  ← pixel_t, VIEWPORT_*, PIXEL_RGB()

port/unix/                    ← Reference desktop port (SDL2)
  main.c                        Command-line args, init sequence
  platform/
    sdl2_video.c                Texture streaming
    sdl2_input.c                Keyboard + controller → pad bitmask
    sdl2_audio.c                SDL audio callback → SPC700 emulator
    sdl2_timer.c                SDL ticks, vsync frame pacing
    sdl2_debug.c                BMP screenshot/VRAM dump

port/waveshare/pico-lcd-1.3/  ← Embedded reference port (RP2040)
  main.c                        Bare-metal init
  platform/
    pico_video.c                SPI + DMA scanline streaming to ST7789
    pico_input.c                GPIO button reads
    pico_audio.c                PWM audio output (disabled by default)
    pico_timer.c                Hardware timer
    pico_debug.c                No-ops
```

## Minimal Port Checklist

To get the game running on a new platform, you need to:

1. Create `port/<your-platform>/`
2. Implement the 5 platform modules (video, input, audio, timer, debug)
3. Write `main.c` with the init sequence
4. Write a `CMakeLists.txt` that builds `src/` as a subdirectory and links it

That's it. The game library handles everything else — rendering, game logic, asset loading.

## The Platform Interface

All functions you must implement are declared in [`src/platform/platform.h`](../src/platform/platform.h). The interface has 5 subsystems:

### Video (required)

```c
bool platform_video_init(void);
void platform_video_shutdown(void);
void platform_video_begin_frame(void);
void platform_video_send_scanline(int y, const pixel_t *pixels);
pixel_t *platform_video_get_framebuffer(void);
void platform_video_end_frame(void);
```

The PPU renders one scanline at a time. Each frame:

1. `begin_frame()` — prepare for a new frame (lock texture, assert CS, etc.)
2. `send_scanline(y, pixels)` — called for y = 0 to VIEWPORT_HEIGHT-1. `pixels` is an array of VIEWPORT_WIDTH `pixel_t` values (RGB565).
3. `end_frame()` — present the frame (unlock texture, release CS, flip buffer, etc.)

`get_framebuffer()` returns a pointer to a contiguous pixel buffer (for desktop ports that need post-processing like FPS overlay), or `NULL` on embedded targets that stream scanlines directly. If you return NULL, the FPS overlay and debug dump features are automatically disabled.

**Desktop approach (SDL2):** Lock a streaming texture in `begin_frame()`, `memcpy` each scanline in `send_scanline()`, unlock + present in `end_frame()`. Return the locked pointer from `get_framebuffer()`.

**Embedded approach (SPI display):** Set the display window in `begin_frame()`, DMA each scanline in `send_scanline()`, finalize in `end_frame()`. Return NULL from `get_framebuffer()`. Byte-swap pixels if the display expects big-endian RGB565 (common for ST7789, ILI9341, etc.).

### Input (required)

```c
bool platform_input_init(void);
void platform_input_shutdown(void);
void platform_input_poll(void);
uint16_t platform_input_get_pad(void);
uint16_t platform_input_get_pad_new(void);
uint16_t platform_input_get_aux(void);
bool platform_input_quit_requested(void);
void platform_request_quit(void);
```

`platform_input_poll()` is called once per frame. Sample your hardware and update internal state. Then:

- `get_pad()` returns a bitmask of currently held SNES buttons (defined in `include/pad.h`)
- `get_pad_new()` returns buttons pressed this frame but not last frame
- `get_aux()` returns aux button state (debug/turbo, see `AUX_*` defines in `platform.h`)

**Button mapping priority** (from `platform.h` comment):

| Priority | Button | Function |
|----------|--------|----------|
| Required | D-pad | Movement and menu navigation |
| Required | PAD_A (0x0080) | Confirm, interact, open command window |
| Required | PAD_B (0x8000) | Cancel, back, show HP/PP on overworld |
| Recommended | PAD_X (0x0040) | Toggle town map |
| Recommended | PAD_START (0x1000) | Start game from title screen |
| Recommended | PAD_L (0x0020) | "Check/talk to" shortcut |
| Optional | PAD_SELECT (0x2000) | Duplicate of B |
| Optional | PAD_R (0x0010) | Bicycle bell (cosmetic) |
| Optional | PAD_Y (0x4000) | No normal gameplay function |

The game tests button *groups* (`PAD_CONFIRM = PAD_A | PAD_L`, `PAD_CANCEL = PAD_B | PAD_SELECT`), so mapping one button per group is sufficient.

Aux buttons (`AUX_*`) are optional — edge detection and toggle logic are handled centrally in `game_main.c`. Just report raw level state (bit set = currently held).

### Audio (optional)

```c
bool platform_audio_init(void);
void platform_audio_shutdown(void);
void platform_audio_lock(void);
void platform_audio_unlock(void);
```

Audio runs the SPC700/DSP emulator (lakesnes) on a separate thread. The lock/unlock functions guard shared state between the audio callback and the game thread.

**To disable audio:** implement these as no-ops returning true/doing nothing, and set `ENABLE_AUDIO OFF` in CMake. The game library will compile without the lakesnes emulator, saving ~64KB RAM.

**To enable audio:** see `port/unix/platform/sdl2_audio.c` for the reference implementation. Key points:
- The audio callback must call `audio_generate_samples()` with **exactly `SAMPLES_PER_FRAME = 534`** samples per call. Calling with fewer samples causes pitch distortion.
- Use an overflow buffer to always request full 534-sample frames and serve the OS-requested chunk from that.
- `platform_audio_lock/unlock` must be a real mutex if the audio callback runs on a separate thread.

### Timer (required)

```c
bool platform_timer_init(void);
void platform_timer_shutdown(void);
void platform_timer_frame_start(void);
void platform_timer_frame_end(void);
uint32_t platform_timer_ticks(void);
uint32_t platform_timer_ticks_per_sec(void);
uint32_t platform_timer_get_fps_tenths(void);
```

Call `frame_start()` at the top of the main loop and `frame_end()` at the bottom. `frame_end()` sleeps or busy-waits to maintain ~60 fps (NTSC timing).

`get_fps_tenths()` returns FPS * 10 (e.g. 600 = 60.0 fps) for the optional FPS overlay.

**Embedded targets** can often just busy-wait in `frame_end()` or use a hardware timer interrupt. If vsync is handled by the display (e.g. tearing-free SPI writes), `frame_end()` can be a no-op.

### Debug (optional)

```c
void platform_debug_dump_ppu(const pixel_t *framebuffer);
void platform_debug_dump_vram_image(void);
void platform_debug_dump_state(void);
```

Desktop ports write BMP/binary files to a `debug/` directory. Embedded ports should implement these as no-ops.

## The main() Entry Point

Your `main()` follows this pattern:

```c
#include "platform/platform.h"
#include "game_main.h"

int main(void) {
    // 1. Platform-specific early init (clocks, GPIO, etc.)

    // 2. Initialize platform subsystems
    platform_video_init();
    platform_input_init();
    platform_timer_init();
    platform_audio_init();   // optional; no-op if audio disabled

    // 3. Initialize game systems
    game_init();

    // 4. Start the main loop
    platform_timer_frame_start();
    for (;;) {
        game_logic_entry();  // returns on game-over Continue
    }
}
```

`game_logic_entry()` runs the entire game — intro, menus, overworld, battles — returning only when the player dies and selects "Continue." The `for(;;)` loop handles restarts. The game calls `wait_for_vblank()` internally, which calls `host_process_frame()` to do rendering, input polling, and frame timing.

## CMake Integration

Your port's `CMakeLists.txt` must:

1. Add the game library as a subdirectory
2. Create an executable linking against it
3. Set viewport dimensions if different from 256x224

```cmake
cmake_minimum_required(VERSION 3.16)
project(earthbound_myplatform C)
set(CMAKE_C_STANDARD 11)

# Viewport — defaults to SNES native 256x224
set(VIEWPORT_WIDTH 256 CACHE STRING "Viewport width")
set(VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height")

# Build the game library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Platform executable
file(GLOB PLATFORM_SOURCES "platform/*.c")
add_executable(earthbound_myplatform main.c ${PLATFORM_SOURCES})
target_link_libraries(earthbound_myplatform earthbound_game)

# Platform-specific libraries (SDL2, Pico SDK, etc.)
# ...
```

### CMake Options (set in your port's CMakeLists.txt or via `-D`)

| Option | Default | Effect |
|--------|---------|--------|
| `VIEWPORT_WIDTH` | 256 | Pixel width of rendered frame |
| `VIEWPORT_HEIGHT` | 224 | Pixel height of rendered frame |
| `ENABLE_AUDIO` | ON | Compile lakesnes SPC700/DSP emulator |
| `ENABLE_VERIFY` | OFF | Side-by-side ROM verification (debug) |
| `ENABLE_ASAN` | OFF | AddressSanitizer (debug builds) |
| `ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer (debug builds) |

## Pixel Format

`pixel_t` is `uint16_t` in RGB565 format (native-endian). Use `PIXEL_RGB(r, g, b)` to construct pixels from 8-bit components, and `bgr555_to_pixel()` for SNES palette conversion.

If your display expects a different byte order (e.g. big-endian RGB565 for SPI displays), byte-swap in `send_scanline()`:

```c
void platform_video_send_scanline(int y, const pixel_t *pixels) {
    for (int x = 0; x < VIEWPORT_WIDTH; x++) {
        uint16_t c = pixels[x];
        display_buf[x] = (c >> 8) | (c << 8);  // LE → BE
    }
    dma_transfer(display_buf, VIEWPORT_WIDTH);
}
```

## Viewport Sizing

The SNES renders at 256x224. The viewport can be larger (for borders, debug overlays, etc.) via the `VIEWPORT_WIDTH`/`VIEWPORT_HEIGHT` CMake variables. The game image is centered in the viewport with `VIEWPORT_PAD_LEFT` and `VIEWPORT_PAD_TOP` offsets (defined in `core/types.h`).

For displays smaller than 256x224, you'll need to implement scaling/cropping in `send_scanline()`.

## RAM Budget

The game library's BSS usage is approximately:

| Component | Size | Notes |
|-----------|------|-------|
| PPU state (`ppu`) | ~136 KB | VRAM (64KB), OAM, CGRAM, registers |
| Entity runtime (`ert`) | ~96 KB | Entity arrays + 64KB work buffer |
| Game state | ~8 KB | Player data, flags, overworld state |
| Battle state | ~4 KB | Battle system variables |
| Text/window system | ~8 KB | Window stack, VWF buffers |
| Audio (lakesnes) | ~64 KB | SPC700 RAM (only if ENABLE_AUDIO=ON) |
| **Total** | **~250-315 KB** | Without/with audio |

The RP2040 (264KB SRAM) fits *without* audio. Platforms with ≥320KB RAM can enable audio.

## Embedded Considerations

### No Dynamic Allocation

The game library uses no `malloc`/`free`. All assets are compiled into the executable via `incbin.h` and accessed as const pointers. The decompression scratch buffer is a static 64KB array (`ert.buffer`).

### No File I/O

Assets are embedded at compile time — no filesystem needed. Save/load uses `save_game()`/`load_game()` which currently serialize to a memory buffer. Persistent saves on embedded platforms would need a flash/EEPROM backend (not yet abstracted).

### Frame Timing

`wait_for_vblank()` calls `host_process_frame()` which calls your `platform_timer_frame_end()`. This is where frame pacing happens. On embedded targets, if your display write in `end_frame()` naturally takes ~16ms, you may not need any additional delay.

### Stack Size

The game has deep call stacks (battle → PSI animation → text display → menu → selection). Budget at least 16KB of stack. The RP2040 default of 8KB may need increasing depending on the port.

## Existing Ports

| Port | Platform | Status | Audio | Display |
|------|----------|--------|-------|---------|
| `port/unix/` | Desktop (SDL2) | Complete | Yes | Window (texture streaming) |
| `port/waveshare/pico-lcd-1.3/` | RP2040 | Playable | No (RAM) | 240x240 SPI LCD |
| `port/snes/` | SNES (65816) | Scaffolding | Real HW | Real PPU |

Study `port/unix/` as the reference implementation and `port/waveshare/pico-lcd-1.3/` for embedded patterns.
