---
name: window-tile-and-text-memory-gotchas
description: BG2 tile-exhaustion is a leak/missing-clear (not a small buffer); text CC window-memory registers (working_memory vs _storage vs argument)
metadata:
  type: reference
---

Debugging facts for the C port's windowing/text system (learned fixing Goods→Give).

## "alloc_bg2_tilemap_entry: tile exhaustion!" is NOT a buffer-size issue
- `USED_BG2_TILE_MAP_DEFAULTS` in `window.c` (`init_used_bg2_tile_map`) **matches the
  assembly byte-for-byte** (`asm/data/system/used_bg2_tile_map_defaults.asm`): ~407
  dynamically-allocatable BG2 content tiles. Do NOT bump it — it can't exceed the
  original and would diverge.
- Exhaustion = a **leak or a missing `CLEAR_FOCUS_WINDOW_CONTENT`**, not too-small a
  pool. Find where the assembly frees content tiles that the C port doesn't.
- Real example: Goods→Give kept BOTH the giver inventory (`WINDOW_INVENTORY`) and the
  recipient inventory (`WINDOW_OVERWORLD_CHAR_SELECT`) rendered at once. The asm
  (`open_menu.asm` @GOODS_GIVE) calls `CLEAR_FOCUS_WINDOW_CONTENT` on the giver window
  before the recipient char-select; the C port omitted it → two full inventories
  (~200 tiles each) overran ~407. Fix = port the missing clear
  (`clear_focus_window_content_far()`).
- `WINDOW_TILEMAP_MAX` (450) caps `content_tilemap`; if a window's
  `(w-2)*(h-2) > 450`, text past index 450 leaks (not tracked → not freed). No
  stock give-flow window hits this, but watch for it on large custom windows.

## Text CC window-memory registers — verify which one against the asm
- A window (`window_stats` / `WindowInfo`) has THREE memory regs:
  `working_memory`, `working_memory_storage`, `argument_memory` (+ secondary). Setters
  in `display_text.c`: `set_working_memory` / `set_working_memory_storage` /
  `set_argument_memory` (operate on the **focus** window).
- `CC_1C_02 PRINT_CHAR_NAME` arg byte: **`$FF` → working_memory_STORAGE**, `$00` →
  argument_memory, else → literal char id. (asm `print_character_name.asm`.) It is NOT
  working_memory for `$FF` — that mis-port made Goods Give print the giver's name as
  the recipient. The Goods give/carry messages (`ESYSTEM`, the only `$FF` users) print
  the recipient via `$FF`, so they need `set_working_memory_storage(recipient)`.
- `EBTEXT "@"` = EB code 0x70 = the BULLET glyph (centered dot). Give/rearrange
  messages literally begin with `@`, so a leading "•" before the name is **faithful**,
  not a glitch. See [[eb-character-encoding]].

## Inventory space check: 14 slots, not 13
- `find_inventory_space2` (give-target "has space?") must port `FIND_INVENTORY_SPACE`
  (asm), which scans ALL 14 item slots (`SIZEOF char_struct::items`, slots 0..13 via
  BRANCHGTS). Do NOT reuse `find_empty_inventory_slot` (scans only 0..12) — it
  mis-reports a character whose sole free slot is the 14th (index 13, where the freed
  slot lands after a give compacts the inventory) as full ("carrying too much stuff").
