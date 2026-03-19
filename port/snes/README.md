# SNES Port

Scaffolding for compiling the C port (`src/`) back to a native SNES ROM (.sfc).

**Status: Scaffolding only -- nothing compiles yet.**

## Background

The original EarthBound was compiled from C using the VUCC compiler targeting the 65816. The C port in `src/` is a reimplementation of that same code. This port aims to compile it back to 65816, completing the "full circle."

## Compiler Options

| | tcc-65816 (PVSnesLib) | vbcc | Calypsi |
|---|---|---|---|
| **Cost** | Free | Free | Commercial (~$200-500) |
| **C Standard** | Partial C99 | Full C99 | Full C99 |
| **Optimization** | None (single-pass) | Good | Excellent |
| **Bank Crossing** | No (16-bit ptrs) | Yes (huge ptrs) | Yes (__near/__far) |
| **Inline ASM** | No | Yes | Yes |
| **SNES Library** | Yes (PVSnesLib) | No | No |
| **Best For** | Small projects | Large codebases (free) | Large codebases (best quality) |

**Recommendation**: vbcc is the most practical -- free, optimizing, handles banked memory, supports inline assembly. PVSnesLib's tcc-65816 is too limited for 50K+ LOC. Calypsi generates the best code but requires a license.

### Installing vbcc (65816 target)

```
# Build from source (no binary releases for 65816)
# See: http://www.compilers.de/vbcc.html
# 65816 target config: http://www.compilers.de/vbcc/targets/
```

### Installing Calypsi

```
# Commercial license from: https://www.calypsi.cc/
# Supports 65816 natively with __near/__far qualifiers
```

## Directory Structure

```
port/snes/
  README.md              # This file
  Makefile               # Build system (compiler-agnostic targets)
  snes_port.cfg          # Linker config (ROM layout, RAM regions)
  main.c                 # Entry point
  platform/
    snes_video.c         # platform_video_* -> SNES PPU hardware registers
    snes_audio.c         # platform_audio_* -> SPC700 APU port communication
    snes_input.c         # platform_input_* -> joypad register reads
    snes_timer.c         # platform_timer_* -> VBlank-based frame timing
  crt0.s                 # 65816 startup code
  vectors.s              # Interrupt vectors (NMI, IRQ, reset)
```

## Building

Not yet functional. Future usage:

```bash
# Extract assets first (same as assembly build)
ebtools extract

# Build SNES ROM
cd port/snes
make COMPILER=vbcc
# Output: build/earthbound_port.sfc
```

## Roadmap

### Phase 1: Remove hard blockers

1. ~~**Remove coroutine dependency**~~ -- **DONE.** The fiber/minicoro system has been removed. `wait_for_vblank()` now calls `host_process_frame()` directly. No platform-specific stack switching needed.

2. ~~**Remove malloc/free**~~ -- **DONE.** `asset_load*()` returns const pointers to compiled-in data (zero-copy, no free). `decomp_alloc()` replaced by `decomp_to_scratch()` using a static 64KB buffer. Debug image dumps use static buffers. Only `vendor/lakesnes/` (third-party, replaced by real SPC700 on SNES) and `verify/` (debug-only) retain malloc.

3. **Remove stdio/file I/O** -- No filesystem. Debug output compiled out via `#ifdef __SNES__`.

### Phase 2: Hardware abstraction alignment

4. **PPU struct as hardware mirror** -- The `ppu` struct already mirrors SNES registers. On SNES, VBlank handler DMAs dirty regions to real hardware. Software renderer (`ppu_render_frame()`) compiled out.

5. **Audio: real SPC700** -- Replace lakesnes emulator. `change_music()`/`play_sfx()` write to hardware ports `$2140-$2143` instead of `apu->inPorts[]`.

6. **DMA: real hardware** -- `snes/dma.c` emulation replaced with actual DMA channel configuration (`$4300-$437F`).

### Phase 3: Integer width and performance

7. **Audit uint32_t usage** -- On 65816, `int` is 16-bit. Every `uint32_t` requires multi-instruction sequences. Downsize where possible to match original assembly's 16-bit choices.

8. **Remove debug-only code** -- FPS overlay, BMP writer, VRAM visualizer, state dump. Guard with `#ifndef __SNES__`.

### Phase 4: Asset loading

9. **ROM-resident assets** -- `asset_load()` returns ROM pointer directly (no memcpy). Asset registry maps names to bank:offset addresses.

### Phase 5: Testing and iteration

10. **Incremental compilation** -- Start with simplest subsystems: `core/math.c`, `core/memory.c`, `game/game_state.c`. Build and test each individually.
