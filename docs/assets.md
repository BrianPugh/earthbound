# Asset Pipeline Reference

`ebtools extract` pulls binary data from a donor ROM and exports human-friendly assets to `src/assets/`. Each asset type can be edited and packed back to binary with `ebtools pack`. Some JSON assets also have `ebtools generate` commands that produce C headers for the port.

## Quick Reference

| Asset | Export Path | Pack Command | Generate Command |
|-------|-----------|--------------|-----------------|
| Overworld Sprites | `overworld_sprites/` | `pack sprites` | — |
| Battle Sprites | `battle_sprites/` | `pack battle-sprites` | — |
| Items | `items/items.json` | `pack items` | `generate items-header` |
| Enemies | `enemies/enemies.json` | `pack enemies` | `generate enemies-header` |
| NPCs | `data/npc_config.json` | `pack npcs` | — |
| Battle Actions | `battle/battle_actions.json` | `pack battle-actions` | — |
| PSI Abilities | `battle/psi_abilities.json` | `pack psi-abilities` | — |
| Teleport Destinations | `data/teleport_destinations.json` | `pack teleport-destinations` | — |
| PSI Teleport Destinations | `data/psi_teleport_destinations.json` | `pack psi-teleport-destinations` | — |
| BG Config | `battle/bg_config.json` | `pack bg-config` | — |
| Battle Backgrounds | `battle_bgs/` | `pack battle-bg` | — |
| Town Maps | `town_maps/` | `pack town-map` | — |
| Intro/Ending Graphics | `intro/` | `pack intro-gfx` | — |
| Fonts | `fonts/` | `pack font` | — |
| Swirls | `swirls/swirls.json` | `pack swirls` | — |
| EXP Table | `data/exp_table.json` | `pack exp-table` | — |
| Stores | `data/stores.json` | `pack stores` | — |
| Music Tracks | `music_tracks.json` | `pack music` | `generate music-header` |

All paths are relative to `src/assets/` (C port assets directory).

---

## Sprites

### Overworld Sprites (`overworld_sprites/`)

463 overworld entity sprites, each exported as an **indexed PNG spritesheet** plus **JSON metadata**.

- `<name>.png` — Indexed (paletted) PNG. Each frame is a row of 4BPP tiles. Frames are stacked vertically, one per direction/animation state.
- `<name>.json` — Frame dimensions (tile width/height), animation info, and palette data.

**Edit workflow:** Modify the PNG in any indexed-color image editor (Aseprite, GIMP). Keep the palette and frame layout unchanged. See [editing-sprites.md](editing-sprites.md) for details.

**Pack:**
```
ebtools pack sprites <png_dir> <bin_dir> <output_dir>
```

### Battle Sprites (`battle_sprites/`)

110 battle sprites (enemy/boss graphics), each as an **indexed PNG** plus **JSON metadata**.

- `<id>.png` — Indexed PNG of the assembled sprite.
- `<id>.json` — Sprite dimensions, arrangement info, palette index.

**Pack:**
```
ebtools pack battle-sprites <png_dir> <bin_dir> <output_dir>
```

---

## Tilemap Assets

Town maps, intro/ending graphics, and battle backgrounds all share the same **tileset + arrangement + palette** export format:

- `*_tileset.png` — Indexed PNG containing all 8×8 tiles laid out in a 16-tile-wide grid. Edit individual tiles here.
- `*_arrangement.json` — JSON file mapping each grid position to a tile index. Each entry is a raw SNES 16-bit tilemap word (bits 0–9: tile index, bit 14: horizontal flip, bit 15: vertical flip). Also stores `tile_count` for lossless round-trip.
- `*.pal` — JASC-PAL palette file (text format, compatible with Aseprite and Paint Shop Pro).

### Town Maps (`town_maps/`)

6 town/location maps (32×32 tiles, 4BPP).

- `<id>_tileset.png` — 512 tiles in a 16×32 grid.
- `<id>_arrangement.json` — 1024 tilemap entries (32×32).
- `<id>.pal` — 32-color JASC palette.

**Pack:**
```
ebtools pack town-map <tileset.png> <arrangement.json> <palette.pal> <output.bin.lzhal>
```
Output is HALLZ2-compressed binary (palette + tiles + arrangement).

### Intro/Ending Graphics (`intro/`)

5 intro/ending screen graphics (32×32 tiles, 4BPP):

| Name | Description |
|------|-------------|
| `ape_logo` | APE Inc. logo |
| `halken_logo` | HAL Laboratory logo |
| `nintendo_logo` | Nintendo logo |
| `title_screen` | Title screen |
| `gas_station` | Gas station intro scene |

Each has `*_tileset.png`, `*_arrangement.json`, and `*.pal`.

**Pack:**
```
ebtools pack intro-gfx <name> <tileset.png> <arrangement.json> <palette.pal> <bin_dir>
```
Writes compressed `.gfx.lzhal`, `.arr.lzhal`, `.pal.lzhal` files under `<bin_dir>`.

### Battle Backgrounds (`battle_bgs/`)

249 unique battle background combinations (32×32 tiles, 2BPP or 4BPP). Named by config index (`000`–`326`).

- `<idx>_tileset.png` — Tile graphics.
- `<idx>_arrangement.json` — 1024 tilemap entries.
- `<idx>.pal` — JASC palette.
- `<idx>.json` — Metadata: `config_index`, `graphics_index`, `arrangement_index`, `palette_index`, `bits_per_pixel`. Required for packing (maps back to the correct asset files).

**Pack:**
```
ebtools pack battle-bg <tileset.png> <arrangement.json> <palette.pal> <metadata.json> <bin_dir>
```
Writes compressed `.gfx.lzhal`, `.arr.lzhal`, and raw `.pal` files under `<bin_dir>/battle_bgs/`.

---

## Fonts (`fonts/`)

5 fonts, each with 96 glyphs (EB character codes `0x50`–`0xAF`):

| Font | Glyph Height | Description |
|------|-------------|-------------|
| `normal` | 32px (16px visible) | Main dialogue font |
| `battle` | 16px | Battle menu font |
| `tiny` | 8px | Small text font |
| `large` | 32px (16px visible) | Large display font |
| `mrsaturn` | 32px (16px visible) | Mr. Saturn's dialogue font |

- `<font>.png` — 1BPP indexed PNG grid (16 columns × 6 rows = 96 glyphs). Color 0 = background, color 1 = foreground.
- `<font>.json` — Metadata: `byte_stride`, `glyph_count`, `widths` (per-glyph pixel widths for variable-width rendering).

**Pack:**
```
ebtools pack font <name> <font.png> <font.json> <bin_dir>
```
Writes `.gfx` and `.bin` (width table) files under `<bin_dir>/US/fonts/`.

---

## JSON Data Tables

### Items (`items/items.json`)

Complete item database. Each entry includes name, type, equip slot, stats, price, and flags.

**Pack:** `ebtools pack items <items.json> <output_dir>`
**Generate:** `ebtools generate items-header <items.json> <output.h>` → produces `items_generated.h` with `#define` constants for item IDs.

### Enemies (`enemies/enemies.json`)

Enemy configuration table. Each entry includes name, sprite, stats, moves, drops, and behavior flags.

**Pack:** `ebtools pack enemies <enemies.json> <output_dir>`
**Generate:** `ebtools generate enemies-header <enemies.json> <output.h>` → produces `enemies_generated.h` with `#define` constants for enemy IDs.

### NPCs (`data/npc_config.json`)

NPC configuration table. Sprite IDs, palette assignments, movement types, event flags.

**Pack:** `ebtools pack npcs <npc_config.json> <output_dir>`

### Battle Actions (`battle/battle_actions.json`)

Battle action/command table (attack, PSI, items, defend, etc.).

**Pack:** `ebtools pack battle-actions <battle_actions.json> <output_dir>`

### PSI Abilities (`battle/psi_abilities.json`)

PSI ability database: names, PP cost, effects, targeting.

**Pack:** `ebtools pack psi-abilities <psi_abilities.json> <output_dir>`

### BG Config (`battle/bg_config.json`)

Battle background configuration table (327 entries). Maps config indices to graphics/arrangement/palette asset indices plus animation and distortion parameters.

**Pack:** `ebtools pack bg-config <bg_config.json> <output_dir>`

### Teleport Destinations (`data/teleport_destinations.json`)

Teleport destination table: map coordinates, names, flags.

**Pack:** `ebtools pack teleport-destinations <teleport_destinations.json> <output_dir>`

### PSI Teleport Destinations (`data/psi_teleport_destinations.json`)

PSI Teleport (Ness/Poo) destination table.

**Pack:** `ebtools pack psi-teleport-destinations <psi_teleport_destinations.json> <output_dir>`

### EXP Table (`data/exp_table.json`)

Experience point thresholds for character level progression.

**Pack:** `ebtools pack exp-table <exp_table.json> <output_dir>`

### Stores (`data/stores.json`)

Shop inventory and pricing.

**Pack:** `ebtools pack stores <stores.json> <output_dir>`

### Swirls (`swirls/swirls.json`)

Battle swirl/transition effect parameters.

**Pack:** `ebtools pack swirls <swirls.json> <output_dir>`

---

## Music (`music_tracks.json`)

Combined music track database with names and pack assignments. Each entry maps a track ID to:

```json
{
    "1": {
        "name": "MUSIC_GAS_STATION",
        "display": "Gas Station",
        "primary_sample_pack": 2,
        "secondary_sample_pack": 3,
        "sequence_pack": 4
    }
}
```

- **`name`** — C constant name (used in generated header).
- **`display`** — Human-readable track title.
- **`primary_sample_pack`** / **`secondary_sample_pack`** / **`sequence_pack`** — Indices into the 171 audio packs in `asm/bin/audiopacks/`. The SPC700 engine loads these packs to play the track.

**Pack:** `ebtools pack music <music_tracks.json> <output_dir>` → writes `music/dataset_table.bin`.
**Generate:** `ebtools generate music-header <music_tracks.json> <output.h>` → produces `music_generated.h` with `#define` constants for track IDs and a name lookup table.

The 171 audio pack files (`asm/bin/audiopacks/0.ebm` through `170.ebm`) are individual binary files extracted directly from the ROM and don't have a human-friendly format.

---

## HALLZ2 Compression Utilities

Several assets use HALLZ2 compression (`.lzhal` files). Standalone compress/decompress commands are available:

```
ebtools hallz compress <input> [-o output]
ebtools hallz decompress <input> [-o output]
```

Compress appends `.lzhal` to the input filename by default. Decompress strips the `.lzhal` suffix (or appends `.bin`).

---

## Directory Structure

```
src/assets/
├── music_tracks.json
├── battle/
│   ├── battle_actions.json
│   ├── bg_config.json
│   └── psi_abilities.json
├── battle_bgs/
│   └── <idx>_tileset.png, <idx>_arrangement.json, <idx>.pal, <idx>.json
├── battle_sprites/
│   └── <id>.png, <id>.json
├── data/
│   ├── exp_table.json
│   ├── npc_config.json
│   ├── psi_teleport_destinations.json
│   ├── stores.json
│   └── teleport_destinations.json
├── enemies/
│   └── enemies.json
├── fonts/
│   └── <font>.png, <font>.json
├── intro/
│   └── <name>_tileset.png, <name>_arrangement.json, <name>.pal
├── items/
│   └── items.json
├── overworld_sprites/
│   └── <name>.png, <name>.json
├── swirls/
│   └── swirls.json
└── town_maps/
    └── <id>_tileset.png, <id>_arrangement.json, <id>.pal
```
