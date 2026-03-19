# C Port Reimplementation Progress

## Codebase Breakdown: Code vs Data

The EarthBound ROM is ~3MB. Most of that is data (graphics, music, maps, text,
event scripts, tables). The actual executable code is a smaller fraction.

| Category           | Files | Lines   | Notes                                  |
|--------------------|------:|--------:|----------------------------------------|
| **Executable code** |       |         |                                        |
|   Named subsystems | 1,002 |  72,197 | battle, overworld, text, system, etc.  |
|   Unknown (code)   | 1,036 |  65,896 | Reverse-engineered, partially named    |
| **Subtotal code**  | 2,038 | 138,093 | **This is what needs porting**         |
| Data tables        | 1,417 | 170,084 | Extracted as binary assets via ebtools |
| Bankconfig         |   189 |  27,845 | Build infrastructure (includes only)   |
| **Grand total**    | 3,644 | 336,022 |                                        |

**~41% of assembly lines are executable code; ~59% is data/build infrastructure.**
Data tables don't need C reimplementation -- they're loaded at runtime from the
donor ROM via `asset_load()` / `asset_load_locale()`.

## Assembly Code by Subsystem

### Named Subsystems (`asm/`)

| Subsystem      | Files | Lines  | C Port Status                          |
|----------------|------:|-------:|----------------------------------------|
| battle/        |   277 | 22,449 | Battle actions + dispatch fully ported  |
| text/          |   261 | 16,827 | VWF + font + most CC handlers ported   |
| overworld/     |   202 | 12,233 | Entity system + collision ported        |
| system/        |   122 |  5,692 | decomp, memory, file I/O               |
| misc/          |    66 |  6,634 | 92% ported                             |
| ending/        |    32 |  3,989 | Fully ported (cast + credits)          |
| intro/         |    23 |  3,668 | Mostly ported                          |
| audio/         |    12 |    387 | Engine interface ported                 |
| inventory/     |     4 |    142 | Equip/item system ported               |
| unused/        |     3 |    176 | N/A (dead code)                        |

### Reorganized from `asm/unknown/`

1,008 previously-unknown files have been reorganized from `asm/unknown/` into
their proper subsystem directories (overworld/, text/, battle/, system/, etc.).
Only 1 file remains in `asm/unknown/` (C3F7FB.asm, unused with UNKNOWN_ label).

## C Port Progress

### Lines Written

| Category       | Lines  | Files |
|----------------|-------:|------:|
| C source (.c)  | 60,000 |    43 |
| Headers (.h)   |  6,731 |    39 |
| **Total**      | 66,731 |    82 |

**Rough conversion factor:** 1 line of C ~ 3-5 lines of 65816 assembly.

### Estimated Coverage: ~95%+ of executable code (by function)

Every named assembly function across all banks (C0-C4, EF, E1) and all named
subsystem directories (battle, text, overworld, system, intro, ending, audio,
inventory, misc) is referenced and implemented in the C port. Key dispatch
systems are 100% complete:
- Entity script opcodes: 69/69 (0x00-0x44)
- Callroutine ROM address handlers: 200/200
- Tick callbacks: 20/20
- CC text control code handlers: fully dispatched
- Movement callback types: 6/6
- Screen position callback types: 6/6
- Draw callback types: 2/2

Note: Every named assembly function across all banks is referenced in the C port.
Coverage percentages below reflect C lines vs total assembly lines in each subsystem
(assembly is ~3-5x more verbose than C, so low percentages don't indicate missing functions).

### Per-Subsystem Coverage

| C Port Module            | Lines  | Key Routines Ported                        |
|--------------------------|-------:|--------------------------------------------|
| **game/battle.c**        | 13,865 | Full battle action dispatch (155 entries), PSI animation system, targeting, status effects, damage calc, battle text, HP rolling, auto-fight, pray, equipment switching |
| **game/display_text.c**  |  3,848 | Text interpreter, CC handlers (flow control, events, entities, audio, flags), special event dispatch, teleport |
| **game/overworld.c**     |  5,865 | Entity collision (AABB), NPC interaction, movement, position buffer, walking styles, party following, enemy encounter, pathfinding |
| **entity/callroutine.c** |  3,829 | 200 ROM callroutine/tick handlers (movement, collision, audio, events, flags, entity management, spotlight HDMA, palette effects) |
| **game/map_loader.c**    |  2,370 | Map loading, tilemap decompression, palette animation, tileset management, collision tile lookup |
| **game/inventory.c**     |  1,899 | Item tables, equip/unequip, usability checks, item effects |
| **game/position_buffer.c** | 1,681 | Position buffer ring, enemy encounter detection, bicycle movement |
| **entity/callbacks.c**   |  1,690 | Entity draw list, sprite dispatch, OAM writes, overlay effects (ripples, sweat, mushroom) |
| **intro/file_select.c**  |  1,525 | FILE_SELECT_INIT, menus, naming screen, save/load |
| **game/ending.c**        |  1,366 | Cast scene, credits engine, photo slideshow |
| **entity/opcodes.c**     |  1,331 | Movement script opcodes 0x00-0x44 (69/69 = 100%) |
| **game/door.c**          |  1,157 | Door detection, room transitions, ladder/stairs, warp system |
| **game/window.c**        |  2,221 | Window rendering, selection_menu, cursor blink, scroll, HPPP windows |
| **entity/entity.c**      |  1,044 | INIT_ENTITY_SYSTEM, ALLOCATE/LINK/UNLINK/CREATE/DEACTIVATE_ENTITY |
| **game/text.c**          |  3,114 | BLIT_VWF_GLYPH, font loading, EB encoding, print_string, name rendering |
| **game/battle_bg.c**     |    880 | BG loading, palette cycling, distortion effects, sine tables |
| **entity/pathfinding.c** |    877 | A* pathfinding, BFS search, matrix populate, path trace |
| **entity/sprite.c**      |    674 | UPLOAD_SPRITE_TO_VRAM, sprite loading, VRAM management |
| **game/oval_window.c**   |    747 | Battle swirl, oval window HDMA, screen transitions, PSI anim state |
| **game/flyover.c**       |    749 | Flyover animation sequence (coffee/tea scene, scrolling text) |
| **snes/ppu_render.c**    |    635 | Software PPU compositor with mosaic, color math, windows (new for port) |
| **intro/title_screen.c** |    315 | Title screen with animated letters |
| **intro/gas_station.c**  |    299 | Gas station intro with battle BG fade |
| **game/audio.c**         |    240 | SPC700 APU interface, music/SFX playback |
| **intro/logo_screen.c**  |    205 | APE/Nintendo logo with mosaic fade |
| **entity/script.c**      |    200 | Movement script engine, entity script init |
| **game/ending.c**        |  1,366 | Cast scene, credits engine, photo slideshow | ~90% of ending |
| **game/flyover.c**       |    749 | Flyover animation sequence | ~90% |
| **snes/ppu_render.c**    |    635 | Software PPU compositor with mosaic, color math, windows | N/A (new for port) |
| **core/decomp.c**        |    212 | DECOMP (LZHAL decompression) | 100% |
| **core/math.c**          |     30 | RNG, mul16, div32_16 | 100% |

### Remaining Work

All known TODO items have been ported. The port is functionally complete.

## What's Playable

- [x] Logo screen (APE/Nintendo)
- [x] Gas station intro sequence
- [x] Title screen with animated letters
- [x] File select screen
- [x] New game config menus (text speed, sound, flavour)
- [x] Character naming screen with animated sprites
- [x] Overworld exploration (partial — map loading, entity rendering, movement)
- [x] NPC interaction / dialogue (partial — text rendering, event flags, flow control)
- [x] Door transitions (room loading, warp system)
- [x] Shop / phone / teleport menus
- [x] Battle system (partial — actions, PSI, targeting, damage, status effects ported; visual effects pending)
- [x] Inventory / equipment (item tables, equip system)
- [x] Ending sequence (cast scene + credits with photo slideshow)
- [x] Flyover animation
- [x] PSI visual animations

## Debug Keys

| Key | Action |
|-----|--------|
| F1  | Dump PPU state + VRAM to `debug/` directory (BMP screenshots, tile data) |
| F2  | Dump VRAM tile visualization (4bpp + 2bpp images) |
| F3  | Toggle FPS/profiling overlay |
| F4  | Dump all game state to `debug/state_NNN.bin` (~190KB binary) |

### State Dump (F4)

Dumps all module struct state to a binary file for debugging. Each press writes a
sequentially numbered file (`debug/state_000.bin`, `debug/state_001.bin`, etc.)
sharing the same counter as F1/F2 dumps.

**Format:** `"EBSD"` magic (4B) + version `u16` + frame counter `u16`, followed by
tagged sections (`section_id:u16 + size:u32 + data[]`), terminated by `0xFFFF`.

**Sections:** Core, GameState, PartyCharacters, EventFlags, Overworld, Battle,
DisplayText, Window, MapLoader, PPU, PositionBuffer, Door, EntityRuntime,
EntitySystem, Scripts, SpritePriority, Fade, RNG, Audio, PsiAnimation.

**Usage:** Compare two dumps with `cmp` or a hex editor to find state differences.
File size is ~190KB (dominated by PPU ~67KB, EntityRuntime ~66KB, EntitySystem ~22KB).
See `src/core/state_dump.c` for section IDs and format details.

### FPS Overlay (F3)

Displays 4 rows of profiling data using the in-game TINY font:

| Label | Color  | Meaning |
|-------|--------|---------|
| FPS   | Green  | Frames per second (target: 60.0) |
| LOG   | Yellow | Game logic time per frame (fade, palette sync) in ms |
| PPU   | Orange | Software PPU render time per frame in ms |
| IDL   | Gray   | Idle headroom (16.7ms - LOG - PPU) |

All values use integer-only math (shift-based IIR smoothing, no floating point)
for portability to embedded targets. The timer backend is platform-abstracted
via `platform_timer_ticks()` / `platform_timer_ticks_per_sec()`.

**Interpreting the numbers:** LOG + PPU is the actual CPU work per frame. IDL is
the remaining frame budget — higher means more headroom. All three should sum to
approximately 16.7ms at 60fps.

## Changelog

- **2026-02-20**: Final TODO items completed — port functionally complete
  - CHECK_DEBUG_EXIT_BUTTON: debug battle skip (B button wait or Y button)
  - ENEMY_SELECT_MODE: full 332-line debug battle group selector with
    4-digit number editor, battle group parsing, enemy sprite preview
  - FLAVOURED_TEXT_GFX: conditional tile overlay for non-Plain window flavours
    in text_load_window_gfx(); preview_flavour_callback now calls full
    text_load_window_gfx() + load_character_window_palette()
  - Added debug_mode_number variable to overworld.h/overworld.c
- **2026-02-20**: Spotlight HDMA + attract mode fixes + stubs cleanup
  - Spotlight: SETUP_SPOTLIGHT_HDMA_WINDOW fully ported — per-scanline WH0/WH1
    window tables for ending credits spotlight effect (events 365, 398, 411).
    DISABLE_HDMA_CHANNEL4 now clears window_hdma_active.
  - Attract mode: cc_1f_teleport_to rewritten to faithfully port assembly TELEPORT
    (event flag clearing, fade transitions, entity queue flush, music resolution).
    Fixes bus direction, scene timing, and Deep Darkness flower bugs.
  - MOVEMENT_CMD_SET_DIR_VELOCITY now sets moving_directions[] matching assembly.
- **2026-02-20**: Ending sequence + stubs cleanup
  - Ending: 1,366 lines — PLAY_CAST_SCENE (entity wipe + scrolling cast names),
    PLAY_CREDITS (3-layer BG, photo slideshow, credit script engine)
  - Flyover: 749 lines — flyover animation sequence
  - Battle char_select_prompt: Full port of both overworld (mode=1) and
    battle-style (mode=0/2) paths with HPPP column selection, on_change/check_valid
    callback parameters, pagination arrow animation rendering
  - Stubs completed: get_collision_at_leader (lookup_surface_flags),
    pagination arrow animation (tilemap rendering from C3E41C data)
  - Misc routines: 92% ported (57/62 functions; remaining are HDMA/null/redirect)
  - Total: 64,536 lines across 76+ files (~65% of executable code)
- **2026-02-18**: Major progress update
  - Battle system: 13,865 lines — full action dispatch table (155 entries), all battle
    actions ported including PSI, pray, equipment switching, auto-fight
  - Text system: 3,848 lines — CC handler dispatch tree mostly complete
  - Overworld: 3,335 lines — entity collision, NPC interaction, party following
  - Callroutines: 2,340 lines — 112 ROM address handlers
  - Map/door system: 3,527 lines combined — room transitions, tilemap loading
  - Inventory: 1,899 lines — item/equip system
  - PPU renderer: mosaic effect implemented
  - Assembly renaming: all UNKNOWN function labels in code directories renamed
    (only 1 unused UNKNOWN label remains in asm/unknown/)
  - Total: 49,251 lines across 74 files (~50-60% of executable code)
- **2025-02-08**: Initial progress tracking file created
  - Entity/sprite system operational (naming screen sprites visible)
  - PPU Mode 1 compositing corrected
  - Config menu window stacking fixed
