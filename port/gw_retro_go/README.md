# Nintendo Game & Watch (Retro-Go SD) Port

Port of the EarthBound C reimplementation to the Nintendo Game & Watch handheld (STM32H7B0VB, 1 MB internal SRAM) running the [retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd) launcher firmware.

**Status: Scaffolding only — nothing compiles yet.**

## Hardware

- **MCU:** STM32H7B0VB (Cortex-M7 @ 280 MHz, 1 MB SRAM, 128 KB internal flash)
- **External flash:** OSPI NOR (1–64 MB, memory-mapped at `0x90000000` after boot)
- **Storage:** microSD card (game assets + emulator cores live here)
- **Display:** 320x240 RGB565 LCD
- **Input:** 8 buttons on the "Mario" unit, 12 on the "Zelda" unit (detected at runtime)
- **Audio:** SAI peripheral, 16 kHz

## Integration model

Unlike the Unix and Pico ports, this port does **not** drive its own build. The host firmware ([game-and-watch-retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd)) builds `src/` (libgame) directly into its multi-emulator launcher and provides:

- A retro-go-side `app_main_earthbound()` entry point that does the launcher's boilerplate (cache `.ro` rodata in external flash, fix up rodata pointers, init system services), then jumps into the upstream `earthbound_main()` defined in [`main.c`](main.c).
- Real implementations of every `platform_*` function in [`Core/Src/porting/earthbound/`](https://github.com/sylverb/game-and-watch-retro-go-sd/tree/main/Core/Src/porting/earthbound) (in the firmware repo).
- A linker section (`.rodata_earthbound`) that holds 3 MB of INCBIN'd asset data on an SD-card-backed extflash cache rather than in the executable image.

The files in this directory are **stubs only** — they exist so that:

1. Upstream CI can sanity-check that `src/`'s `platform.h` interface is satisfied by every advertised port.
2. Embedded-only cleanups (no `malloc`, no `printf`, no `setjmp`) shared with [`port/snes/`](../snes/) have a home that documents the constraint and a target that proves the cleanups don't regress the build.
3. The firmware-side implementation has a one-to-one mirror to point at when reading the upstream code.

The real porting work (input mapping, video scaling, audio bridging, etc.) lives in the firmware repo. See [`SNES-PORTING.md`](https://github.com/sylverb/game-and-watch-retro-go-sd/blob/main/SNES-PORTING.md) there for the full porting plan.

## Memory model

The launcher is a multi-emulator shell; only one emulator is resident in RAM at a time. The "RAM_EMU" region is ~724 KB, shared by `.text`, `.data`, `.rodata` (the parts that fit), and `.bss`. For EarthBound:

| Region | Lives in | Notes |
|---|---|---|
| Code + small const data | `RAM_EMU` | Streamed from `/roms/homebrew/EarthBound.bin` on SD into RAM at launch. |
| Bulk rodata (incl. 3 MB INCBIN'd assets) | OSPI external flash | Cached from `/roms/homebrew/earthbound.ro` on SD. Pointers fixed up at runtime by `PatchCodeRodataOffset` (Zelda 3 trick). |
| Save data | SD card | `/saves/earthbound.sav` (7680 bytes). |
| Audio (if enabled) | `RAM_EMU` BSS | +~64 KB. Initial builds will set `ENABLE_AUDIO=OFF`. |

The 3 MB of INCBIN'd asset bytes is routed into `.rodata_earthbound` by compiling [`embedded_assets_array.c`](https://github.com/yourname/earthbound/tree/main/embedded_assets_array.c) with `-DINCBIN_OUTPUT_SECTION=".rodata_assets"`. The asset index table is placed in RAM (so its pointers can be relocated) via `-DEBASSET_TABLE_ATTR='__attribute__((section(".noreloc")))'`.

## Directory layout

```
port/gw_retro_go/
  README.md                      this file
  main.c                         earthbound_main() — called by app_main_earthbound() in the firmware
  platform/
    gw_video.c                   platform_video_*   → odroid_display + lcd_get_active_buffer
    gw_input.c                   platform_input_*   → odroid_input + get_ofw_is_mario()
    gw_audio.c                   platform_audio_*   → SAI peripheral + audio_get_active_buffer
    gw_timer.c                   platform_timer_*   → wdog_refresh + lcd_wait_for_vblank
    gw_debug.c                   platform_debug_*   → no-ops (no filesystem dumps on device)
```

`platform_save_*` is not stubbed here. The firmware-side implementation is a thin wrapper around `odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ...)` + `fopen`/`fread`/`fwrite`; nothing port-specific worth a stub.

## Embedded constraints (shared with `port/snes/`)

Both this port and [`port/snes/`](../snes/) target environments with no `malloc`/`free`, no `printf`/`stderr`, no filesystem access from inside game code (file I/O goes through `platform_save_*` or stays out), and no `setjmp`/`longjmp`. Audits and refactors in `src/` that remove these dependencies benefit both targets — coordinate accordingly.

See the **Embedded Considerations** section of [`docs/porting-guide.md`](../../docs/porting-guide.md) for the current state of these cleanups.
