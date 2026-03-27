# Modding Support Roadmap

This document tracks the gaps in our asset pipeline that prevent easy modding, and plans for addressing them.

## Current State

The pipeline already supports JSON round-trip editing for ~70 asset types (items, enemies, NPCs, PSI abilities, battle actions, stores, EXP tables, sprites, battle backgrounds, fonts, etc.). Overworld and battle sprites have indexed PNG + metadata JSON with a custom asset override system. The embed registry provides zero-overhead INCBIN with family macros.

What works well today:
- Structured data tables (items, enemies, PSI, stores, EXP) — clean JSON, easy to edit
- Sprite pipeline — indexed PNG + metadata JSON with `src/custom_assets/` overrides
- Locale separation (US/JP) handled transparently
- `ebtools pack-all` repacks everything from JSON to binary

## Gaps

### 1. Text/Dialogue Has No Round-Trip Tooling

**Impact: High — blocks nearly every modding use case.**

Item help text, NPC dialogue, enemy attack names, and PSI descriptions are stored as raw ROM address pointers in JSON (e.g., `"help_text_pointer": "0xC539C4"`). A modder can change an item's stats but can't edit what its description *says* without working in assembly.

Almost every interesting mod (new weapon, new enemy, new NPC) needs custom text, making this the single biggest barrier.

**What's needed:**
- A text table extractor that decodes EB-encoded text blocks into human-readable strings
- A text packer that re-encodes edited strings and updates pointer references
- Integration with the existing JSON configs so that e.g. `items.json` can reference text by symbolic name instead of ROM address

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

### 4. Raw Pointer Coupling in JSON

**Impact: Medium — makes JSON configs confusing and fragile.**

Many JSON fields are raw ROM addresses (`text_pointer`, `function_pointer`, `secondary_pointer`). These are meaningless to modders and break if ROM layout changes. Some fields already use symbolic names (e.g., `event_flag_name`), but most pointers don't.

**What's needed:**
- Symbolic name resolution in the packer (e.g., `"help_text": "desc_cracked_bat"` instead of `"help_text_pointer": "0xC539C4"`)
- A symbol table that maps names to addresses, updated during extraction
- Backward compatibility with raw addresses for advanced users

### 5. No JSON Validation

**Impact: Medium — modders get silent failures.**

There's no schema validation on JSON edits. Setting an item's `type` to 99 or a sprite ID to 9999 produces a bad binary or runtime crash with no error message. Modders have to guess what went wrong.

**What's needed:**
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

1. **Text round-trip tooling** (#1) — unblocks the most modding use cases
2. **JSON validation** (#5) — cheap to implement, immediately improves modder experience
3. **Symbolic pointer resolution** (#4) — natural companion to text tooling
4. **Event script DSL** (#2) — high impact but high effort
5. **Variable table sizes** (#3) — important for the C port's modding story
6. **Map data tooling** (#6) — enables level editing
7. **Audio tooling** (#7) — niche but valuable
8. **Incremental packing** (#8) — quality of life
