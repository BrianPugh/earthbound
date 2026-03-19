# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ebsrc is a disassembly/decompilation of the SNES games EarthBound (US) and Mother 2 (Japan), with support for a US localization prototype (1995-03-27). The codebase is pure 65816 assembly (ca65 dialect) with SPC700 assembly for audio. The assembly disassembly is essentially complete — all ~1,000 previously-unknown functions have been identified and reorganized into proper subsystem directories.

A native C port (`src/`) is also nearly complete (~95%+ of executable code ported). The project is currently in a **bug fixing, polish, and feature development** phase. Key priorities include:
- Fixing remaining bugs and visual glitches
- Performance optimization
- Feature development and modding support

## Build Commands

Building requires a donor ROM to extract assets first with `ebtools`, then assembling with `make`:

The US retail donor ROM is at `earthbound.sfc` in the repo root (gitignored).

```
# US Retail (args default to earthbound.yml and earthbound.sfc)
ebtools extract
make

# Mother 2 (Japan)
ebtools extract mother2.yml "path/to/mother2.sfc"
make mother2

# US Prototype (1995-03-27)
ebtools extract earthbound-1995-03-27.yml "path/to/prototype.sfc"
make proto19950327
```

Build outputs go to `build/` (`.sfc` ROM, `.map` linker map, `.lst` listing files).

**Required tools**: ca65 v2.19+ (cc65), spcasm v1.1.0+, ebtools, GNU make.

## Python Environment

**Always use the project venv and `uv`.** Never use the system Python or pyenv global.

```bash
# Activate the venv (do this before any Python/ebtools commands)
source .venv/bin/activate

# Install ebtools in the venv
uv pip install -e .

# Run Python commands via uv when not activated
uv run python -m pytest
uv run ebtools extract
```

All `ebtools` CLI invocations, `pytest` runs, and Python scripts must use the `.venv` — never the system Python.

## Architecture

### Source Organization (`asm/`)

Code is organized by **game subsystem**, not by ROM bank:
- `bankconfig/` — Per-version bank layout files (`JP/`, `US/`, `US19950327/`, `common/`). Each bank file (bank00.asm–bank3f.asm) `.INCLUDE`s the actual implementation files from other directories.
- `battle/` — Battle system (combat logic, PSI, enemies, battle backgrounds)
- `overworld/` — Overworld navigation, NPCs, entity management
- `text/` — Text rendering and display system
- `system/` — Core system (DMA, interrupts, memory management, save/load)
- `data/` — Game data tables, music data, events, map data
- `audio/` — Audio engine and music playback
- `spc700/` — SPC700 sound processor assembly (`.s` files, compiled by spcasm)
- `ending/`, `intro/` — Ending sequence and title/file select
- `inventory/` — Inventory management
- `misc/` — Miscellaneous code
- `unused/` — Dead code from the original ROM

### Include Files (`include/`)

- `macros.asm` — C calling convention macros (BEGIN_C_FUNCTION, STACK_RESERVE_*, END_C_FUNCTION)
- `structs.asm` — Data structure definitions (char_struct, game_state, enemy_data)
- `enums.asm` — Global enumerations and constants
- `hardware.asm` — SNES hardware register definitions
- `eventmacros.asm` — Event scripting macros
- `config.asm` — Game balance constants (damage, PSI effects, etc.)
- `constants/` — Domain-specific enums (items, enemies, music, event_flags, etc.)
- `symbols/` — ROM address symbol definitions (e.g., bank00.inc.asm through globals.inc.asm)

### Conditional Compilation

Three ROM versions are built using preprocessor defines passed to ca65:
- `-D USA` — EarthBound US retail
- `-D JPN` — Mother 2 Japan
- `-D USA -D PROTOTYPE19950327` — US prototype

Use `.IF .DEFINED(USA)` / `.IF .DEFINED(JPN)` guards for version-specific code.

### ROM Memory Map (snes.cfg)

- BSS RAM: `$7E0000`–`$7E0B800` (47KB)
- RAM2: `$7EB800`–`$7EFFFF` (82KB)
- SRAM: `$300000`–`$302000` (8KB save)
- ROM: `$C00000`–`$C300000` (3MB, 64 banks: BANK00–BANK3F)

### VUCC Calling Convention

The original game was compiled from C using the VUCC compiler. Key conventions:
- Stack frame lives on the direct page, grows upward, adjusted by DP register
- Virtual registers at `$00`–`$0C` (two 8-bit, two 16-bit, two 32-bit)
- First 3 **16-bit** parameters passed via A, X, Y registers (8-bit params are NOT register-passed)
- Return values via A register (small) or virtual registers of the caller (large)
- Functions can be near (RTS) or far (RTL)

### Naming Conventions

- Labels: `UPPERCASE_SNAKE_CASE` (e.g., `REDUCE_HP`, `SET_HP`)
- Local stack variables: `@LOCALxx`
- Virtual register aliases: `@VIRTUALxx`
- Constants: `.DEFINE CONSTANT_NAME value`
- Each `.asm` file declares its target bank with `.SEGMENT "BANKxx"`

### Data Extraction (ebtools)

YAML configuration files (`earthbound.yml`, `mother2.yml`, `commondefs.yml`) describe ROM asset locations and sizes. `ebtools extract` extracts binary assets from a donor ROM into the assembly source tree.

Install ebtools with: `uv pip install -e .` (from the repo root).

## C Port (`src/`)

### Goal

The `src/` directory contains a native C reimplementation of the game that runs on modern platforms (SDL2). It reuses the same donor ROM asset pipeline — binary game data is extracted by ebtools and embedded into the executable at build time. The port is nearly complete (~95%+ of executable code) and is in the bug fixing and polish phase.

### What Belongs in C Source Code

**Allowed** — original work that constitutes functional reimplementation:
- Algorithm implementations (how the game logic works — interpreters, renderers, fade engines, etc.)
- Structural definitions we author (opcode enums, callback IDs, state machine constants)
- Hardware/memory layout constants (WRAM addresses, VRAM layout, register definitions)
- ROM address constants used for dispatch (e.g., `#define ROM_ADDR_SPAWN_ENTITY 0xC09E71`) — these document the interface between code and data, not the data itself
- Data structure definitions (entity tables, script system, priority queues)
- Platform abstraction (SDL input, rendering, audio)

**NOT allowed** — proprietary game data that must come from the donor ROM:
- Game bytecode / script data (event scripts, movement scripts)
- Graphics data (tiles, tilemaps, spritemaps, sprite composition tables)
- Palette data (color tables, fade targets)
- Audio data (music, sound effects, instrument samples)
- Text / dialogue strings
- Any byte array that reproduces ROM contents, even if hand-assembled from the disassembly

### Asset Pipeline

All game data flows through ebtools extraction:

1. Add an entry to `earthbound.yml` specifying the ROM offset, size, and output path
2. `ebtools extract` extracts binary blobs into `asm/bin/` AND human-editable JSON into `src/assets/`
3. At build time, `ebtools pack-all` packs JSON from `src/assets/` back to binary in `build/packed_assets/`, overriding `asm/bin/` originals
4. `ebtools embed-registry` generates C code that INCBINs all binary assets into the executable
5. C code loads assets at runtime via `asset_load()` / `asset_load_locale()`

**Editing game data:** Edit the JSON files in `src/assets/` (items, enemies, NPCs, etc.), then rebuild. The pack-all step automatically regenerates the binary. No need to touch `asm/bin/` or `earthbound.yml`.

**`asset_load` vs `asset_load_locale`**: In assembly, `LOCALEBINARY "path"` means the asset is locale-specific (extracted to `asm/bin/US/` or `asm/bin/JP/`); use `asset_load_locale()` in C. Plain `BINARY "path"` is locale-independent; use `asset_load()`.

### Editing Overworld Sprites

`ebtools extract` generates indexed PNG spritesheets and JSON metadata for all 463 overworld sprites in `src/assets/overworld_sprites/`. These can be edited in any image editor that supports indexed PNGs and repacked with `ebtools pack sprites`. Custom sprites go in `src/custom_assets/overworld_sprites/png/` and override the originals at build time.

See **[docs/editing-sprites.md](../docs/editing-sprites.md)** for the full editing guide, spritesheet format, JSON metadata reference, and troubleshooting.

### VRAM Addressing

The `VRAM_*` constants in `src/include/constants.h` are **word addresses** (matching the SNES assembly's `VRAM::` enum). The C port's `ppu.vram[]` is a **byte array** (64KB). Always multiply by 2 to convert: `ppu.vram[VRAM_CONSTANT * 2]`. Likewise, `COPY_TO_VRAM` destination offsets in assembly are word offsets — multiply by 2 for byte offsets into `ppu.vram[]`.

### Development Workflow

**Port faithfully from assembly first, then refactor.** Every C function that corresponds to an assembly routine must be written by reading the assembly and porting its exact values and logic — never guess tile indices, VRAM addresses, palette numbers, asset names, or other constants. Get it working and faithful to the original ASM before improving code quality.

Concretely: before writing a C function, find and read the corresponding assembly, trace every constant back to its source, and use those exact values. "Reasonable-sounding" placeholder values (like made-up tile indices) cause hard-to-diagnose visual bugs.

**NEVER add workarounds or hacks.** If a bug appears because a dependency function is stubbed, the fix is to port the full dependency — not to add compensating logic that the assembly doesn't have. Workarounds diverge from the assembly, mask the real problem, and create subtle state bugs. When a stub causes issues, port the complete function it stubs.

See **[docs/assembly-to-c.md](docs/assembly-to-c.md)** for the full guide covering VUCC calling conventions, assembly patterns, variable/register mapping, common gotchas, and worked examples.

**C Port Porting Checklist** (follow for every function/feature):

1. **Find the exact assembly routine** — `grep` for the label in `asm/`, read the full `.asm` file. Do not work from summaries or plans alone.
2. **Trace every constant** — follow `LDA #immediate` values, `.DEFINE` names, and enum references back to their definitions in `include/` (`macros.asm`, `enums.asm`, `hardware.asm`, `constants/`). Use the actual values.
3. **Check related subsystems** — if the function writes tiles, check what tiles are already at those VRAM positions (e.g., `TEXT_WINDOW_GFX` scattered uploads overwrite tiles 0x50-0x54, 0x5F-0x69, 0x70-0x79, 0x80, 0x90). If it uses the cursor, check how `selection_menu()` renders cursors (two-frame blink with tiles 0x41/0x51 and 0x28D/0x29D).
4. **Grep before naming** — before using any constant, tile index, or function name, grep the codebase to confirm it matches the assembly and doesn't collide with existing definitions.
5. **Build and test** — `cmake --build build` after each logical change. Visual bugs from wrong constants are hard to diagnose after the fact.

As you read and understand assembly functions during porting, rename any `UNKNOWN_*` labels to descriptive names. This applies to both:
- **Global function labels** (`UNKNOWN_C0xxxx`): rename the label, the `.GLOBAL` symbol in `include/symbols/bankXX.inc.asm`, all callers (JSL/JSR), event macros in `include/eventmacros.asm`, and event macro callers in `asm/data/events/`. Build with `make` to verify.
- **Local branch labels** (`@UNKNOWNxx`): rename within the file to describe the branch's purpose (e.g., `@UNKNOWN7` → `@QUICK_MODE`, `@UNKNOWN13` → `@BUTTON_PRESSED`). Local labels only need to be unique within their file.

### Build

```
cd port/unix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For debugging specific issues, use a debug build with sanitizers:

```
cd port/unix
cmake -S . -B build-debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

Requires SDL2 and the extracted assets from ebtools.

**Architecture:** `src/` builds as a static library (`libgame.a`) containing all platform-agnostic game logic. `port/unix/` contains SDL2/Unix platform code and builds the `earthbound_port` executable, linking against the game library. Other ports (e.g., `port/snes/`) implement the same `platform.h` interface. See **[docs/porting-guide.md](docs/porting-guide.md)** for how to add a new platform port.

**SNES port (`port/snes/`):** Scaffolding for compiling the C port back to a native SNES ROM. Not yet functional — requires a 65816 C compiler (vbcc or Calypsi) and significant `src/` changes (removing coroutines, malloc, stdio). See `port/snes/README.md` for the full roadmap.

### Debug Hotkeys

- **F5** — Toggle debug mode (`ow.debug_flag`). Enables: overworld debug menu (hold B+R), battle debug (A=PSI cycle, B=swirl cycle, SELECT+START=instant win)
- **F1** — Dump PPU state and VRAM as BMP images
- **F2** — VRAM visualization
- **F3** — Toggle FPS counter (shows game logic time, PPU render time, idle headroom)
- **F4** — Dump all game state to `debug/state_NNN.bin` (binary, ~190KB per file). Useful for diffing state between two points in time or attaching to bug reports. Format: "EBSD" header + tagged sections for each module struct (core, game_state, overworld, battle, entities, PPU, etc.) + 0xFFFF terminator. See `src/core/state_dump.c` for section IDs.

### C Port Source Organization

Large subsystems are split into focused files sharing an `*_internal.h` header:

- **`battle.c`** + `battle_actions.c`, `battle_calc.c`, `battle_targeting.c`, `battle_psi.c`, `battle_ui.c` — shared via `battle_internal.h`
- **`display_text.c`** + `display_text_cc.c`, `display_text_menus.c` — shared via `display_text_internal.h`
- **`callroutine.c`** + `callroutine_movement.c`, `callroutine_palette.c`, `callroutine_screen.c`, `callroutine_sprite.c`, `callroutine_action.c` — shared via `callroutine_internal.h`
- **`overworld.c`** + `overworld_collision.c`, `overworld_interaction.c`, `overworld_palette.c`, `overworld_spawn.c`, `overworld_teleport.c` — shared via `overworld_internal.h`

New `.c` files are auto-detected by CMake (`file(GLOB_RECURSE)`); just add the file and reconfigure.

### Structured Data Access

Binary game data tables are accessed via packed C structs, not raw byte offsets. Key structs:

| Struct | Header | Entry Size | Description |
|--------|--------|-----------|-------------|
| `ItemConfig` | `inventory.h` | 39 bytes | Item names, type, cost, effect, help text |
| `EnemyData` | `battle.h` | 94 bytes | Enemy stats, AI, drops, sprites |
| `NpcConfig` | `map_loader.h` | 17 bytes | NPC type, sprite, script, event flag |
| `DeliveryEntry` | `overworld.h` | 20 bytes | Timed delivery sprites, text ptrs, speeds |
| `PsiAnimConfig` | `battle_psi.c` | 12 bytes | PSI animation GFX, palette, timing |
| `HotspotCoords` | `overworld.h` | 8 bytes | Map hotspot boundaries |
| `ScreenTransitionConfig` | `door.h` | 12 bytes | Door/screen transition data |
| `BattleAction` | `battle.h` | 12 bytes | Battle action table entries |
| `PsiAbility` | `battle.h` | 15 bytes | PSI ability definitions |

All use `PACKED_STRUCT` / `END_PACKED_STRUCT` / `ASSERT_STRUCT_SIZE` (from `core/types.h`) to guarantee binary layout matches the ROM. Access pattern: `asset_load()` returns `const uint8_t *`, cast to the struct pointer type.

Byte-order helpers (`read_u16_le`, `read_u32_le`, `write_u16_le`) are in `src/include/binary.h` for any remaining raw binary access.

### Entity Indexing Convention

Entity arrays in the C port are packed `[MAX_ENTITIES]` and indexed directly by slot number. The `ENT(slot)` macro in `entity.h` is identity (`#define ENT(slot) (slot)`) — it exists for readability and to document intent, but no longer multiplies by 2.

```c
#define ENT(slot) (slot)
entities.abs_x[ENT(slot)] = x;
```

Note: the assembly side (`ram.asm`) still uses `MAX_ENTITIES * 2` for word-sized WRAM tables. The WRAM-address mapping in `opcodes.c` bridges between the ROM's word-indexed layout and the C port's packed arrays by dividing addresses by 2.

### Common Bug Patterns

**Little-endian byte ordering in script reads:** When reading 16-bit values from game script bytecode, the SNES stores them little-endian (low byte first, high byte second). A common bug is swapping the byte order. The correct pattern:

```c
uint16_t lo = script_read_byte(r);
uint16_t hi = script_read_byte(r);
return lo | (hi << 8);
```

## Python Style (ebtools)

- Do **not** use `from __future__ import annotations`. The project targets Python 3.10+ where `X | None` and other modern annotation syntax work natively.
