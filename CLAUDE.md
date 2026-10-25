# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Workflow

Committing directly to `main` (or whatever the current branch is) is OK — no need to create a feature branch first. Still only commit when the user asks.

## Project Overview

ebsrc is a disassembly/decompilation of the SNES games EarthBound (US) and Mother 2 (Japan), with support for a US localization prototype (1995-03-27). The codebase is pure 65816 assembly (ca65 dialect) with SPC700 assembly for audio. The assembly disassembly is essentially complete — all ~1,000 previously-unknown functions have been identified and reorganized into proper subsystem directories.

A native C port (`src/`) plays the whole game start to finish (a playtest relay reached THE END on 2026-10-25). The project is currently in a **bug fixing, polish, and feature development** phase. Key priorities include:
- Fixing remaining bugs and visual glitches
- Performance optimization
- Feature development and modding support

## Build Commands

Building requires a donor ROM to extract assets first with `ebtools`, then assembling with `make`:

The US retail donor ROM is at `earthbound.sfc` in the repo root (gitignored).

```
# One-shot desktop build (recommended for fresh checkouts)
# Checks deps, creates .venv, installs ebtools, extracts assets, builds port/unix
make unix

# US Retail SNES ROM (args default to earthbound.yml and earthbound.sfc)
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

### Tests and linting

Pytest tests live in `tests/` (sprite/tile extraction, text DSL, asset migration, coverage, C-port/disassembly consistency). Run the full suite or a single test:

```bash
uv run python -m pytest tests                        # all (bare `pytest` also collects the tamp submodule's tests, which error)
uv run python -m pytest tests/test_overworld_sprites.py::test_name   # one
```

pytest is not a declared dependency; if the venv lacks it, `uv pip install pytest`. `uv run` may recreate `.venv` with a different interpreter; `uv run --no-sync` avoids re-syncing.

`tests/test_port_consistency.py` cross-checks the C port against the disassembly: `ROM_ADDR_*` constants vs the linker map, script callroutines vs dispatch cases, script WRAM writes vs `resolve_wram()`, script pointers vs loaded script banks, and no libc `rand()` in game code. It needs `build/earthbound.map` (`make`) and extracted assets; those tests skip otherwise.

`tests/test_battle_math.py` builds the host-only C test in `tests/c/` (libgame with `EB_HOST_TEST` plus a no-op platform) and replays `tests/c/battle_cases.txt`: battle-math routines (success rolls, dodge, variance, miss/SMAAAASH/damage, status ailments, level-up, 37 battle actions) recorded from the original ROM by `tests/c/battle_oracle.py`, which calls each asm routine in the playtest emulator. Re-record with that script after changing it (the test checks the file is current when the emulator is built); never edit the cases by hand.

CI (`.github/workflows/ci.yml`) runs ruff, the asset-free pytest subset, the gw cross-compile and cppcheck on every push and PR. cppcheck fails on findings outside `.github/cppcheck-suppressions.txt` (by id and file; run `.github/scripts/cppcheck.sh` locally, `--update` to rewrite the baseline, cppcheck 2.22.0). The asset jobs (desktop `-Werror` build at both viewports, `regress.py check` against the committed (hashed) goldens, the same cases under ASan+UBSan, the pico RAM budget) need a self-hosted runner with the ROM and saves and stay off until the `EB_ASSET_RUNNER` repo variable is `true`.

Ruff is configured in `pyproject.toml` (target Python 3.10, line length 120, numpy docstring convention) and runs on commit via `.pre-commit-config.yaml`. Install hooks once with `pre-commit install`, or invoke manually: `uv run ruff check .` / `uv run ruff format .`.

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

The `src/` directory contains a native C reimplementation of the game that runs on modern platforms (SDL2). It reuses the same donor ROM asset pipeline — binary game data is extracted by ebtools and embedded into the executable at build time. It is in the bug fixing and polish phase (see Project Overview).

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

**Pipeline gotchas:**
- Changing an `earthbound.yml` entry (new asset, different size) needs a re-run of `ebtools extract`; `make unix` skips extraction once assets exist.
- The pack-all stamp (`PACK_ALL_STAMP` in `src/CMakeLists.txt`) tracks the asset JSON, `earthbound.yml`/`commondefs.yml`, `asm/bin/assets.manifest` and every `ebtools/**/*.py`, so editing a packer reruns pack-all and the registry on the next build. The build runs the `ebtools` found at configure time: in a worktree, that is the main checkout's editable install unless `PYTHONPATH` points at the worktree.
- Text-pointer fields in data tables hold original SNES ROM addresses and must be remapped with `addr_remap` after dialogue packing (see the telephone-contacts and timed-delivery blocks in `pack_all.py`). A table packed by the plain simple packers keeps unresolvable addresses.
- Event scripts are only runnable if their ROM range is inside a registered script bank (`event_script_data.c`); otherwise `event_script_init` halts them with "unknown script ID".

**`asset_load` vs `asset_load_locale`**: In assembly, `LOCALEBINARY "path"` means the asset is locale-specific (extracted to `asm/bin/US/` or `asm/bin/JP/`); use `asset_load_locale()` in C. Plain `BINARY "path"` is locale-independent; use `asset_load()`.

### Editing Overworld Sprites

`ebtools extract` generates indexed PNG spritesheets and JSON metadata for all 463 overworld sprites in `src/assets/overworld_sprites/`. These can be edited in any image editor that supports indexed PNGs and repacked with `ebtools pack sprites`. Custom sprites go in `src/custom_assets/overworld_sprites/png/` and override the originals at build time.

See **[docs/editing-sprites.md](docs/editing-sprites.md)** for the full editing guide, spritesheet format, JSON metadata reference, and troubleshooting.

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
5. **Build and test** — `cmake --build build` after each logical change. Visual bugs from wrong constants are hard to diagnose after the fact. Before committing, run the regression check (`playtest/regress.py compare OLD_BIN NEW_BIN`, plus `framehash` for renderer changes; see Playtesting below).

As you read and understand assembly functions during porting, rename any `UNKNOWN_*` labels to descriptive names. This applies to both:
- **Global function labels** (`UNKNOWN_C0xxxx`): rename the label, the `.GLOBAL` symbol in `include/symbols/bankXX.inc.asm`, all callers (JSL/JSR), event macros in `include/eventmacros.asm`, and event macro callers in `asm/data/events/`. Build with `make` to verify.
- **Local branch labels** (`@UNKNOWNxx`): rename within the file to describe the branch's purpose (e.g., `@UNKNOWN7` → `@QUICK_MODE`, `@UNKNOWN13` → `@BUTTON_PRESSED`). Local labels only need to be unique within their file.

### Build

```
cd port/unix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A fresh configure is native 256×224 (`EB_VIEWPORT_WIDTH` defaults to 256); the wide viewport the game is played in needs `-DEB_VIEWPORT_WIDTH=320 -DEB_VIEWPORT_HEIGHT=240`, and wide-viewport bugs (off-centre scenes, gutters) only show in that build. Check `EB_VIEWPORT_WIDTH` in `CMakeCache.txt` before judging a screenshot.

Warning flags live in one place, `cmake/EbWarnings.cmake` (`eb_target_warnings()`), shared by `src/` and every port. `EB_WERROR` makes them errors: ON for `port/unix` and the gw stub, OFF for the RP2040 port (arm gcc still warns about `%X` vs `uint32_t`, sign-compare and a few others). Mark an intended switch fall-through with `EB_FALLTHROUGH;` (`core/types.h`), not a comment.

For debugging specific issues, use a debug build with sanitizers:

```
cd port/unix
cmake -S . -B build-debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

Requires SDL2 and the extracted assets from ebtools.

**Architecture:** `src/` builds as a static library (`libgame.a`) containing all platform-agnostic game logic. Each port directory implements the `platform.h` interface and links against the library. See **[docs/porting-guide.md](docs/porting-guide.md)** for how to add a new platform port.

- **`port/unix/`** — SDL2 desktop port (Windows/macOS/Linux). Primary development target; builds `build/earthbound`.
- **`port/waveshare/pico-lcd-1.3/`** — RP2040 (Raspberry Pi Pico) embedded port for the Waveshare 1.3" LCD board (240x240 ST7789, 9 inputs). Playable, no audio yet.
- **`port/snes/`** — Scaffolding for compiling the C port back to a native SNES ROM. Not yet functional — requires a 65816 C compiler (vbcc or Calypsi) and significant `src/` changes (removing coroutines, malloc, stdio). See `port/snes/README.md` for the full roadmap.
- **`port/gw_retro_go/`** — Nintendo Game & Watch (STM32H7B0VB) embedded port. Compile-only scaffolding here; the real build is driven by the firmware repo [`game-and-watch-retro-go-sd`](https://github.com/sylverb/game-and-watch-retro-go-sd), which compiles `src/` directly into its multi-emulator launcher and provides the `platform_*` implementations. `CMakeLists.txt` here cross-compiles the stubs with `arm-none-eabi-gcc` as a `platform.h` interface-drift check. See `port/gw_retro_go/README.md`.

### Debug Hotkeys

- **F5** — Toggle debug mode (`ow.debug_flag`). Enables: overworld debug menu (hold B+R), battle debug (A=PSI cycle, B=swirl cycle, SELECT+START=instant win)
- **F1** — Dump PPU state and VRAM as BMP images
- **F2** — VRAM visualization
- **F3** — Toggle FPS counter (on the Pico: board A). Read it with care: `LOG` times only the fade/palette prelude of `host_process_frame`, not the game logic, which runs between calls and is unmeasured; with frame skipping on (`EB_MAX_FRAME_SKIP`, 2 on the Pico) `PPU` averages skipped frames in as 0 ms, and `FPS` is 1/(time of a rendered frame), not frames shown per second
- **F6** — Save-anywhere snapshot to `savestate.bin` (tamp-compressed, ~25-30 KB per slot; ~141 KiB raw, ~47% of it the PPU VRAM mirror). The request is deferred to the next root-loop boundary (torn-safe): the game free-runs any in-flight blocking helper to completion, then captures. Format (v16): "EBSD" header with a `layout_id` + tagged sections for each module struct (core, game_state, overworld, battle, entities, PPU, mode stack, etc.) + 0xFFFF terminator. `layout_id` hashes the source of every serialized type (`src/core/state_layout.h`, generated by `src/tools/savestate_layout.py`; after changing a serialized struct, a `ModeState` member or a phase enum, rerun it, or `tests/test_savestate_layout.py` fails), so a slot from a build with a different layout is rejected. See `src/core/state_dump.c` for section IDs and `host_root_boundary()` in `src/game_main.c` for the capture-safety gate.
- **F7** — Restore the last `savestate.bin` (`state_dump_load`). Deferred to the same root-loop boundary as F6 (loading replaces the mode stack wholesale, so it free-runs to the root first). Loads work across processes of the same build (pointer slots are stored as canonical values); a slot from a build whose section layout differs, or a corrupt one, is rejected before any live state changes. Self-tests: `port/unix/build/earthbound --selftest-savestate` (round trip, cold load, crash safety, rejection) and `EB_BIN=<absolute path> uv run --no-sync python playtest/savestate_coldload.py` (save in one process, load in a fresh one).

### Run Options (desktop port)

- **`--smooth-camera`** (or `EB_SMOOTH_CAMERA=1`) — Presentation-only smooth scrolling. The stock BG scroll is the integer leader position, so the scenery judders 0/1/2 px per frame while walking (the original does too). With the option, plain overworld walking frames (mode stack = OVERWORLD only, no window, `camera_mode` 0, full brightness, mode 1, BG1 scroll = leader camera) are rendered twice (normal pass + a pass with the party's OAM entries hidden; the PPU records which pixels the party won) and the scenery is drawn shifted by the leader's 16.16 fraction at window scale >= 2, with the party pixels unshifted on top. Other frames, and scale 1, use the normal path; game state and savestates are untouched. Code: `src/game/smooth_camera.c`, `ppu_set_obj_mask()` in `src/snes/ppu_render.c`, `platform_render_frame()` in `port/unix/platform/sdl2_video.c`; it compiles in only with the `EB_SMOOTH_CAMERA` CMake option (ON for `port/unix`, OFF for embedded ports). `EB_SMOOTH_CAMERA_PROFILE=1` prints per-pass timings; `EB_SMOOTH_CAMERA_CAPTURE=FILE` (headless) writes the final 3x-scaled output as raw rgb24 frames, following the recorder's `record on|off` (add `--record-video /dev/null --record-paused` so frame-skip drops nothing).

### Playtesting (headless harness)

To play or verify a change in-game, use the interactive session driver `playtest/pt.py` (see `playtest/README.md`): each step returns decoded window text, game state and a screenshot PNG. Batch scripts: `playtest/run.py` / `--input FILE`. Story saves go in gitignored `playtest/saves/` (README says where to get them).

The `playtest` skill (`.claude/skills/playtest/SKILL.md`) holds the workflow, debug shortcuts, gotchas and a table of verified scene recipes (save + exact steps); load it before playing, and add a row when you reach a new scene.

Many things that look wrong match the original (narrow battle-BG bands, purple/blue map rectangles, the phone's $1 charge, the HQ wanderer pile-up, ...). Check `docs/known-original-behaviour.md` before investigating a suspected divergence, and add a line there (plus a short comment at the code site, if one routine explains it) whenever a check concludes "matches the original".

- In zsh, wrap the driver in a function (`pt(){ uv run --no-sync python playtest/pt.py "$@"; }`); a `PT="uv run ..."` variable won't word-split.
- Several agents can play at once: give each its own build dir (`EB_BIN=port/unix/build-X/earthbound`) and session-name prefix. While any agent is editing source in the main checkout, the others build from their own `git worktree add --detach ../ebsrc-X HEAD` (symlink `asm/bin`, `src/assets`, `src/vendor/tamp`, plus `playtest/saves`, which `regress.py` and `savestate_coldload.py` need); a build from the shared tree picks up half-done edits. A worktree has no `.venv`, and `uv run` there creates an empty one (no Pillow), so run its Python tools with the main checkout's `.venv/bin/python`. The symlinked submodule makes `git worktree remove` refuse, so remove these with `--force` (after checking `git -C <wt> diff --stat HEAD` is empty).
- Don't wait on a background job whose output has stopped changing for a few minutes (`playtest/stale_jobs.py` lists idle repo processes); macOS has no `timeout`, so use Python-side timeouts.
- The pre-commit hook (ruff only) stashes unstaged files during a commit, which yanks running agents' in-progress edits. With parallel agents, stage only your own paths or hunks (`git apply --cached` of a filtered `git diff`), run `uv run --no-sync ruff check`/`ruff format --check` on staged Python yourself, and commit with `--no-verify`.
- `pt.py start NAME --emu --save ...` runs the original ROM (headless snes9x core) with the same commands, for checking what the original does; build the core once with `playtest/emu/build.sh` (needs `make` for `build/earthbound.map`). Emu shots are 256 px wide.
- **Regression check before committing a fix:** build the pre-fix and post-fix trees (wide-viewport flags, separate build dirs, a `git worktree` for the old one) and run `uv run --no-sync python playtest/regress.py compare OLD_BIN NEW_BIN` (~30 short replay cases, under a minute). Explain every reported diff (intended effect of the fix, or a regression) in the commit/report; see the skill's "Regression check". For renderer changes also run `regress.py framehash OLD_BIN NEW_BIN` (every frame, changed pixels split into SNES band vs gutters).

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

**Randomness:** always use the game RNG (`rng_next_byte()`, `RAND_MOD(N)` = `RAND % (N+1)`), never libc `rand()`. It must advance the serialized RNG state like the original. Check the asm's arithmetic too: some sites scale (`RAND * N >> 8` via `MULT168`) rather than take a modulo.

**Width and signedness of asm math:** `CLC; SBC #k; BRANCHLTEQS` branches when `A - k - 1 < 0`, i.e. `A <= k` (reading it as `A - k - 1 <= 0` caused two off-by-one bugs); `BLTEQ` is `BCC`+`BEQ` (unsigned); `LSR` is a logical shift (use `uint16_t`, not signed `>>`); 16-bit `ADC`/`SBC` wrap. `TRUNCATE_16_TO_8` takes an 8-bit *factor* but returns the full 16-bit result, so don't narrow it. Watch for `.A8`/`.A16` annotations that disagree with the caller's actual accumulator mode: e.g. `UPDATE_PARTY_SPRITE_POSITION`'s `AND #$0000 / CLC` executes as `AND #$1800`.

**Callroutine dispatch:** an unknown callroutine address falls to `callroutine_dispatch`'s default, which leaves `pc` unchanged, so the routine's parameter bytes then run as script opcodes. A wrong `ROM_ADDR_*` value fails silently this way.

**`ert.buffer` is 20 KB, not the asm's $F800:** `BUFFER_SIZE` is 0x5000 (`src/entity/entity.h`). Code ported from asm that stages more than that overruns into `scripts` and corrupts the entity/script free lists, which shows up as an unrelated infinite loop. Every user is a named region in `src/entity/buffer_layout.h`, whose `_Static_assert`s check fit and non-overlap (deliberate aliases are listed there); add new users as regions and reach them through `BUF_REGION()` / `buf_at()`, which bounds-check in Debug. For a hang with looping entity lists, try an ASan build first.

## Python Style (ebtools)

- Do **not** use `from __future__ import annotations`. The project targets Python 3.10+ where `X | None` and other modern annotation syntax work natively.
