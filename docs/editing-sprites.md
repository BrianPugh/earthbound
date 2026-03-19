# Editing Overworld Sprites

This guide explains how to view, edit, and repack overworld sprites for the EarthBound C port using the `ebtools` asset pipeline. The workflow is lossless: sprites are exported as indexed PNGs, edited in a standard image editor, and packed back into the binary format the game engine expects.

## Prerequisites

- **ebtools** installed (`uv pip install -e .` from the repo root)
- **A donor ROM** extracted (`ebtools extract`)
- **An image editor** that supports indexed/paletted PNGs (Aseprite, GraphicsGale, GIMP, Libresprite)
- **CMake + SDL2** for building the C port

## Quick Start

```bash
# 1. Extract assets (generates PNGs in src/assets/overworld_sprites/)
ebtools extract

# 2. Copy the sprite you want to edit into the custom overrides directory
mkdir -p src/custom_assets/overworld_sprites/png
cp src/assets/overworld_sprites/ness.png  src/custom_assets/overworld_sprites/png/
cp src/assets/overworld_sprites/ness.json src/custom_assets/overworld_sprites/png/

# 3. Edit ness.png in your image editor (keep it indexed mode, 16 colors max)

# 4. Rebuild the C port (CMake auto-runs pack sprites)
cd src && cmake --build build
```

That's it. The game will use your modified sprite. You can also run `pack sprites` manually if needed:

```bash
ebtools pack sprites src/custom_assets/overworld_sprites/png asm/bin src/custom_assets/overworld_sprites
```

## Generated Files

When you run `ebtools extract`, each of the 463 overworld sprites gets two files in `src/assets/overworld_sprites/`:

- **`<name>.png`** -- Indexed (paletted) PNG spritesheet. Columns are animation frames (2), rows are directions. Color 0 is transparent. Pixel values are exact SNES 4bpp tile indices, so the round-trip is lossless.
- **`<name>.json`** -- Metadata sidecar with sprite dimensions, direction labels, palette index, hitbox, bank, and the raw frame pointer values needed for repacking.

Example: `ness.png` is 32x192 px (16 px wide x 2 frames, 24 px tall x 8 directions).

These files are gitignored and regenerated from the donor ROM each time you extract.

## Spritesheet Format

Each spritesheet is laid out as a grid: **columns = animation frames**, **rows = directions**.

Every sprite has exactly **2 frames** (columns): frame 0 on the left, frame 1 on the right.

Rows are in **memory pointer order** (not the SNES direction enum order used in game code):

| Row | 4-dir sprites | 8-dir sprites |
|-----|---------------|---------------|
| 0   | up            | up            |
| 1   | right         | right         |
| 2   | down          | down          |
| 3   | left          | left          |
| 4   | --            | up-right      |
| 5   | --            | down-right    |
| 6   | --            | down-left     |
| 7   | --            | up-left       |

The `directions` array in the JSON sidecar lists the label for each row, so you can always check which direction is which.

### Dimensions

Sprite dimensions are measured in 8px tiles:

- **Pixel width** = `tile_width` x 8 (per frame)
- **Pixel height** = `tile_height` x 8 (per direction)
- **Image width** = pixel width x 2 (two frames side by side)
- **Image height** = pixel height x direction count

For Ness (`tile_width=2`, `tile_height=3`, 8 directions): each frame is 16x24 px, so the full spritesheet is 32x192 px.

## Editing Constraints

When editing a sprite PNG, you must follow these rules:

1. **Stay in indexed mode.** The PNG must use palette mode (mode `P`), not RGB or RGBA. If your editor converts to RGB, the packer will reject it.

2. **Maximum 16 colors.** SNES 4bpp sprites use color indices 0--15. Any pixel with an index above 15 will cause an error.

3. **Color 0 is transparent.** The first palette entry is always treated as the transparent color by the SNES hardware.

4. **Keep the same dimensions.** The spritesheet must be exactly the same pixel dimensions as the original. You cannot resize a sprite without restructuring the underlying binary data.

5. **Preserve the palette order.** Each pixel value (0--15) maps to a specific SNES palette color. Reordering the palette will scramble the sprite's appearance in-game.

## Palette System

The game uses 8 overworld sprite palettes, stored as binary files:

```
asm/bin/overworld_sprites/palettes/0.pal  (SNES BGR555 binary, 32 bytes)
asm/bin/overworld_sprites/palettes/1.pal
...
asm/bin/overworld_sprites/palettes/7.pal
```

Each `.pal` file is 32 bytes: 16 colors x 2 bytes each, in SNES BGR555 format (little-endian). The JSON metadata's `palette_index` field (0--7) tells you which palette a sprite uses.

### JASC-PAL exports

During extraction, `ebtools extract` also writes human-readable JASC-PAL palette files:

```
src/assets/overworld_sprites/palettes/0.pal  (JASC-PAL text format)
src/assets/overworld_sprites/palettes/1.pal
...
src/assets/overworld_sprites/palettes/7.pal
```

These text-format `.pal` files can be loaded directly in Aseprite (Palette > Load Palette) or any editor that supports the JASC-PAL format. They are generated files and are gitignored.

### Editing palette colors

During PNG export, the palette embedded in the PNG image is populated from the corresponding `.pal` file. Each PNG contains a clean 16-entry palette (no padding entries).

To change a sprite's colors, edit the palette in the PNG itself -- the pixel indices stay the same, but the displayed colors change. When you run `pack sprites`, it reads the palette back from each PNG and writes updated `.pal` files if the colors have changed.

**Shared palettes:** Multiple sprites can share the same palette index. If you change palette colors in one sprite's PNG, `pack sprites` will warn you that the change affects all sprites sharing that palette. If two edited sprites on the same palette have *different* palettes, `pack sprites` will report an error.

## JSON Metadata Reference

Each sprite's JSON sidecar contains:

```json
{
  "sprite_id": 1,
  "name": "ness",
  "tile_width": 2,
  "tile_height": 3,
  "pixel_width": 16,
  "pixel_height": 24,
  "direction_count": 8,
  "frames_per_direction": 2,
  "directions": ["up", "right", "down", "left", "up_right", "down_right", "down_left", "up_left"],
  "palette_index": 5,
  "palette_byte": 26,
  "size_byte": 5,
  "width_byte": 32,
  "bank": 17,
  "bank_byte": 209,
  "hitbox": {
    "width_ud": 8,
    "height_ud": 8,
    "width_lr": 8,
    "height_lr": 8
  },
  "frame_pointers": [...]
}
```

### Field descriptions

| Field | Description | Editable? |
|-------|-------------|-----------|
| `sprite_id` | Unique sprite index (0--462) | No (identifies which sprite to update) |
| `name` | Human-readable name | No (informational only) |
| `tile_width` | Width in 8px tiles | No (structural) |
| `tile_height` | Height in 8px tiles | No (structural) |
| `pixel_width` | `tile_width` x 8 | No (derived) |
| `pixel_height` | `tile_height` x 8 | No (derived) |
| `direction_count` | Number of direction rows (4 or 8) | No (structural) |
| `frames_per_direction` | Always 2 | No (fixed) |
| `directions` | Label for each row in the PNG | No (informational) |
| `palette_index` | Which palette file (0--7) | No (structural) |
| `palette_byte` | Raw ROM byte | No (internal) |
| `size_byte` | Spritemap type selector | No (internal) |
| `width_byte` | Raw width byte from ROM | No (internal) |
| `bank` | Sprite graphics bank (0x11--0x15) | No (structural) |
| `bank_byte` | Raw bank byte from ROM | No (internal) |
| `hitbox.width_ud` | Hitbox width when facing up/down (0--255) | **Yes** |
| `hitbox.height_ud` | Hitbox height when facing up/down (0--255) | **Yes** |
| `hitbox.width_lr` | Hitbox width when facing left/right (0--255) | **Yes** |
| `hitbox.height_lr` | Hitbox height when facing left/right (0--255) | **Yes** |
| `frame_pointers` | Raw 16-bit within-bank offsets | No (preserved for lossless round-trip) |

Only `hitbox` values can be changed in the JSON. Sprite dimensions, bank assignment, and frame pointers are structural and cannot be modified without restructuring the binary data.

## Validation and Errors

`pack sprites` validates all inputs before writing any output. If there are errors, it reports **all of them at once** so you can fix everything in one pass.

### Error messages

| Problem | Error message |
|---------|--------------|
| PNG file missing for a JSON | `<name>.json: no matching PNG file (<name>.png)` |
| Invalid JSON syntax | `<name>.json: invalid JSON: <detail>` |
| Missing or invalid `sprite_id` / `hitbox` | `<name>.json: <pydantic validation details>` |
| Unknown sprite ID | `<name>.json: sprite_id <id> not found in grouping data` |
| Bank not available | `<name>: bank index <idx> (bank 0x<hex>) not loaded` |
| PNG is RGB instead of indexed | `<name>.png: must be indexed/paletted PNG (mode 'P'), got mode '<mode>'` |
| Wrong dimensions | `<name>.png: wrong dimensions <w>x<h>, expected <ew>x<eh> (<pw>px x 2 frames, <ph>px x <dirs> directions)` |
| Pixel index > 15 | `<name>.png: <count> pixel(s) have color index > 15 (max found: <max>). SNES 4bpp sprites only support indices 0-15` |
| Frame data exceeds bank | `<name>: frame data at offset 0x<offset> + <size> bytes exceeds bank size (<bank_size> bytes)` |

### Common fixes

- **"must be indexed/paletted PNG"** -- Your editor saved the image as RGB. In GIMP: Image > Mode > Indexed. In Aseprite: File > Color Mode > Indexed.
- **"wrong dimensions"** -- You resized the image or cropped it. Undo and keep the original canvas size.
- **"pixel(s) have color index > 15"** -- You added colors beyond the 16-color palette. Reduce the palette back to 16 colors using your editor's color quantization.

## Pipeline Architecture

```
                         ebtools extract
                               |
               +---------------+---------------+
               |                               |
     asm/bin/ (binary blobs)      src/assets/overworld_sprites/
     [banks, palettes, etc.]      [<name>.png + <name>.json x 463]
               |                               |
               |                    Developer edits PNGs
               |                               |
               |                    src/custom_assets/overworld_sprites/png/
               |                    [modified <name>.png + <name>.json]
               |                               |
               +---------------+---------------+
                               |
                      ebtools pack sprites
                               |
               src/custom_assets/overworld_sprites/
               [banks/*.bin + sprite_grouping_data.bin]
                               |
                    CMake embed-registry
                    (custom overrides original)
                               |
                        Game executable
```

The key concept is **overrides**: `pack sprites` writes binary files into `src/custom_assets/overworld_sprites/`. At build time, the embed-registry prefers these override files over the originals in `asm/bin/`. If you delete the custom assets, the game reverts to the original sprites.

CMake automatically runs `pack sprites` when it detects PNGs in the custom sprites directory, so you only need to place your edited files and rebuild.

## Technical Details

This section explains the underlying SNES sprite format. You don't need this to edit sprites, but it may help if you're debugging issues or building tools.

### SNES 4bpp tile format

Each 8x8 tile is 32 bytes. The pixel data is stored as four bitplanes, interleaved in pairs:

- Bytes 0--15: bitplanes 0 and 1 (interleaved row by row, 2 bytes per row)
- Bytes 16--31: bitplanes 2 and 3 (same layout)

A pixel's color index is assembled from the corresponding bit in each plane: `(bp3 << 3) | (bp2 << 2) | (bp1 << 1) | bp0`.

### Bank structure

Sprite tile data is stored across banks 0x11--0x15 (five 64KB banks). Each sprite's `bank` field in the JSON tells you which bank contains its graphics. The `frame_pointers` are 16-bit offsets within that bank, with flag bits in the low bit(s).

### The override mechanism

The C port's CMake build uses an embed-registry that maps asset paths to binary files. When `src/custom_assets/overworld_sprites/` contains packed bank files, the registry uses those instead of the originals from `asm/bin/`. This means your edits override on a per-bank basis -- if you edit any sprite in bank 0x11, the entire bank 0x11 is replaced with your version (which includes all other bank-0x11 sprites unchanged).

## Troubleshooting

**My sprite looks scrambled in-game.**
The PNG palette order may have changed. Ensure your editor isn't reordering palette entries when saving. In GIMP, check "Do not write color space information" in the PNG export options.

**My editor converted the image to RGB when I opened it.**
Some editors default to RGB mode. Convert back to indexed before saving: GIMP (Image > Mode > Indexed), Aseprite (Sprite > Color Mode > Indexed).

**I want to change a sprite's colors but not its shape.**
Edit the palette entries in the PNG (not the pixel values). The pixel indices map to palette slots, so changing palette colors recolors the sprite without altering its shape. When you run `pack sprites`, it will detect the palette change and write an updated `.pal` file. Be aware that all sprites sharing the same `palette_index` will be affected.

**I edited a sprite but the game still shows the old one.**
Make sure you rebuilt the C port (`cmake --build build`). CMake automatically runs `pack sprites` when PNGs or JSONs in `src/custom_assets/overworld_sprites/png/` change. If you added new files, CMake will re-configure automatically via `CONFIGURE_DEPENDS`.

**I want to revert a sprite to the original.**
Delete the sprite's PNG and JSON from `src/custom_assets/overworld_sprites/png/`, re-run `pack sprites` (or delete the entire `src/custom_assets/overworld_sprites/` directory), and rebuild.

**`pack sprites` reports no errors but the sprite looks wrong.**
Double-check that color 0 in your palette is the transparent color. If you accidentally put a visible color at index 0, transparent areas will show that color instead.

---

# Editing Battle Sprites

Battle sprites are the enemy graphics shown during battle encounters. They follow a similar PNG export/pack workflow to overworld sprites, but with some differences.

## Quick Start

```bash
# 1. Extract assets (generates PNGs in src/assets/battle_sprites/)
ebtools extract

# 2. Copy the sprite you want to edit
mkdir -p src/custom_assets/battle_sprites/png
cp src/assets/battle_sprites/042.png  src/custom_assets/battle_sprites/png/
cp src/assets/battle_sprites/042.json src/custom_assets/battle_sprites/png/

# 3. Edit the PNG (keep it indexed mode, 16 colors max)

# 4. Rebuild (CMake auto-runs pack battle-sprites)
cd src && cmake --build build
```

You can also run the packer manually:

```bash
ebtools pack battle-sprites src/custom_assets/battle_sprites/png asm/bin src/custom_assets/battle_sprites
```

## Key Differences from Overworld Sprites

| Aspect | Overworld | Battle |
|--------|-----------|--------|
| Image layout | Spritesheet (frames x directions) | Single static image |
| Compression | None (raw bank data) | LZHAL compressed per-sprite |
| Tile arrangement | Row-major | 4x4 block interleaved |
| Palettes | 8 shared | 32 shared |
| Palette assignment | Per-sprite (in grouping data) | Per-enemy (different enemies can use the same sprite with different palettes) |
| Sizes | Variable tile dimensions | 6 fixed sizes: 32x32 to 128x128 |
| Locale variants | None | Sprites 23 and 62 have US-specific versions |

## Generated Files

After `ebtools extract`, each battle sprite gets two files in `src/assets/battle_sprites/`:

- **`<NNN>.png`** -- Indexed PNG (sprite ID zero-padded to 3 digits). Single image, not a spritesheet.
- **`<NNN>.json`** -- Metadata sidecar with sprite ID, size enum, dimensions, and palette assignment.

Palettes are exported as JASC-PAL files in `src/assets/battle_sprites/palettes/`.

## Sprite Sizes

There are 6 fixed size categories:

| Size enum | Dimensions | Tile grid |
|-----------|-----------|-----------|
| 1 | 32x32 | 4x4 |
| 2 | 64x32 | 8x4 |
| 3 | 32x64 | 4x8 |
| 4 | 64x64 | 8x8 |
| 5 | 128x64 | 16x8 |
| 6 | 128x128 | 16x16 |

You cannot change a sprite's size category -- the PNG must match the original dimensions exactly.

## Palette System

Battle sprites use 32 palettes (vs 8 for overworld). Palette assignment is **per-enemy**, not per-sprite: the same sprite graphic can appear with different palettes depending on which enemy uses it. During extraction, `ebtools` scans the enemy configuration table and assigns each sprite the most commonly used palette.

The `palette_index` in the JSON (0--31) indicates which palette was used for the exported PNG. Palette files are at:

```
asm/bin/battle_sprites/palettes/0.pal through 31.pal  (SNES BGR555 binary)
src/assets/battle_sprites/palettes/0.pal through 31.pal  (JASC-PAL text)
```

## Editing Constraints

Same rules as overworld sprites:

1. Stay in indexed/paletted PNG mode (mode `P`)
2. Maximum 16 colors (indices 0--15)
3. Color 0 is transparent
4. Keep the same pixel dimensions
5. Preserve the palette order

## JSON Metadata

```json
{
  "sprite_id": 42,
  "name": "042",
  "size_enum": 4,
  "tile_width": 8,
  "tile_height": 8,
  "pixel_width": 64,
  "pixel_height": 64,
  "palette_index": 12
}
```

All fields except `palette_index` are structural and should not be changed.

## Tile Arrangement

Unlike overworld sprites (which store tiles row by row), battle sprites use a 4x4 block interleaved arrangement. The pack/export pipeline handles this automatically -- you just edit the PNG as a normal image.
