# Modding Support Roadmap

This document tracks the state of our asset pipeline for modding, what works today, and remaining gaps.

## Current State

The pipeline supports JSON round-trip editing for ~70 asset types (items, enemies, NPCs, PSI abilities, battle actions, stores, EXP tables, sprites, battle backgrounds, fonts, etc.). Overworld and battle sprites have indexed PNG + metadata JSON with a custom asset override system. The embed registry provides zero-overhead INCBIN with family macros.

**Text and dialogue editing is fully operational.** The complete round-trip works:

1. `ebtools extract` generates human-readable dialogue YAML (`src/assets/dialogue/*.yml`) with symbolic label names, event flag names, and item/music/sprite references.
2. `ebtools text search "query"` searches all dialogue YAML and JSON configs for text.
3. `ebtools pack-all` (runs automatically via cmake) compiles dialogue YAML back to binary and remaps all text pointers.
4. JSON configs use inline text fields (`help_text`, `description`, `text_1`/`text_2`) and symbolic refs (`help_text_ref`, `dialogue_ref`) pointing to YAML labels.

**Modder workflow:**

```bash
# Find text
ebtools text search "sleepy bones"

# Edit dialogue
vim src/assets/dialogue/E01ONET0.yml

# Build (automatic — cmake runs pack-all)
cd port/unix && cmake --build build

# Play
./build/earthbound
```

What works well today:
- **Text/dialogue** — YAML round-trip with symbolic names, search, automatic repacking
- **Structured data tables** (items, enemies, PSI, stores, EXP) — clean JSON, easy to edit
- **Sprite pipeline** — indexed PNG + metadata JSON with `src/custom_assets/` overrides
- **Locale separation** (US/JP) handled transparently
- `ebtools pack-all` repacks everything from JSON to binary

## Gaps

### ~~1. Text/Dialogue~~ — IMPLEMENTED

See "Current State" above. Dialogue YAML files in `src/assets/dialogue/` support the full text bytecode DSL (text, pause, branching, menus, etc.) with symbolic label and flag names. JSON configs reference text inline or via symbolic refs to YAML labels.

**Text format note:** An `@` character at the start of a text string means "clear the text window before printing." It acts as a paragraph separator. Without it, text appends to whatever is already in the window. For example, `- text: '@Good morning,'` clears the window first, while `- text: ' sleepy bones!'` appends to the existing text.

### 2. Event Scripts Are Opaque Bytecode

**Impact: High — blocks narrative and gameplay mods.**

Event scripts (`data/events/*.bin`) are compiled bytecode with no decompiler or assembler. Adding new NPC behaviors, map triggers, cutscenes, or quest logic requires writing raw assembly. The event macro system (`include/eventmacros.asm`) is powerful but has no tooling bridge to the JSON/C pipeline.

**What's needed:**
- An event script disassembler that produces a human-readable DSL or annotated format
- An assembler that compiles the DSL back to bytecode
- Integration with NPC configs so `text_pointer` and `secondary_pointer` can reference scripts symbolically

### 3. Fixed Table Sizes

**Impact: Medium — limits what mods can add.**

Items are 256 entries × 39 bytes, enemies are 256 × 94 bytes, NPCs have a fixed count × 17 bytes. A modder can only replace existing slots — they can't append entry #257. The ROM build is inherently fixed, but the C port has no such constraint.

**What's needed:**
- Variable-length table support in the packer and embed registry (at least for the C port)
- C code changes to use runtime table sizes instead of hardcoded counts
- A mechanism for mods to declare new entries without conflicting with each other

### 4. Raw Pointer Coupling in JSON — PARTIALLY ADDRESSED

**Impact: Medium — makes JSON configs confusing and fragile.**

**Progress:** JSON configs now have symbolic refs for text (`help_text_ref`, `dialogue_ref` pointing to YAML labels). Dialogue YAML uses symbolic label names, event flag names, and item/music/sprite names throughout. Raw ROM addresses are no longer needed for text.

**Remaining:** Non-text pointers (`function_pointer`, `secondary_pointer`) are still raw ROM addresses. These are meaningless to modders and break if ROM layout changes.

**What's still needed:**
- Symbolic name resolution for non-text pointers (function and script references)
- Backward compatibility with raw addresses for advanced users

### 5. JSON Validation — IN PROGRESS

**Impact: Medium — modders get silent failures.**

**Progress:** Packer validation is being added. The text packer validates YAML structure and reports errors on malformed dialogue.

**Remaining:** No schema validation on most JSON edits. Setting an item's `type` to 99 or a sprite ID to 9999 produces a bad binary or runtime crash with no error message.

**What's still needed:**
- JSON Schema definitions for each asset type
- Validation in the packer with clear error messages (e.g., "item type must be 0-7", "sprite ID 9999 exceeds max 462")
- Range checks, enum validation, and cross-reference checks (e.g., "battle action references nonexistent PSI ability")

### 6. Map Data Is Binary/Compressed

**Impact: Medium — blocks map/level editing.**

Overworld tilemaps, sector arrangements, and collision data are LZHAL-compressed blobs with no editor integration. Modifying map layout requires external tools and manual compression.

**What's needed:**
- A map data extractor that produces an editable format (e.g., Tiled JSON)
- A packer that converts back to the game's compressed format
- Sector attribute editing (music, encounter rates, town map assignments — some of this is already JSON)

### 7. Audio Is Completely Opaque

**Impact: Low-Medium — blocks music/SFX mods.**

The 171 `.ebm` audio packs have no tooling for inspection or modification. Even basic metadata (instrument list, tempo, track count) isn't extracted. The `music_tracks.json` maps track names to pack indices but the actual audio data is untouchable.

**What's needed:**
- Audio pack inspection tool (list instruments, tracks, basic metadata)
- Integration with existing SPC700 music tools (e.g., EBMusEd format support)
- SFX table editing

### 8. Pack Commands Are Fragmented

**Impact: Low — minor workflow friction.**

`ebtools pack items`, `pack enemies`, `pack sprites`, etc. are all separate commands. `pack-all` exists but a modder editing one file doesn't know which specific command to run. There's no dependency tracking.

**What's needed:**
- File-watcher or dependency-aware incremental packing
- Clear documentation of which pack command corresponds to which JSON file
- Or just always recommend `pack-all` with better performance

## Priority Order

1. ~~**Text round-trip tooling** (#1)~~ — DONE
2. **JSON validation** (#5) — in progress, immediately improves modder experience
3. ~~**Symbolic pointer resolution** (#4)~~ — partially done (text refs complete, non-text pointers remain)
4. **Event script DSL** (#2) — high impact but high effort
5. **Variable table sizes** (#3) — important for the C port's modding story
6. **Map data tooling** (#6) — enables level editing
7. **Audio tooling** (#7) — niche but valuable
8. **Incremental packing** (#8) — quality of life
