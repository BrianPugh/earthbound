#ifndef GAME_FLYOVER_H
#define GAME_FLYOVER_H

#include "core/types.h"

/* Flyover text system — port of the C4 bank flyover rendering functions.
 * Renders large-font text onto BG3 with scrolling, used for:
 *   - Intro location text ("The War Against Giygas!")
 *   - Coffee/tea break text sequences
 *   - Ending text
 *
 * The system uses its own VWF buffer separate from the window system. */

/* PLAY_FLYOVER_SCRIPT (C49EC4) prologue — bytecode interpreter for flyover
 * text scripts; id: 0-7 index into FLYOVER_TEXT_POINTERS table. Runs the
 * synchronous front half (entity-23 tick disable, screen init, asset lookup,
 * word-wrap off) and returns the GAME_MODE_FLYOVER (FO_SCRIPT) init scalars;
 * false = bad id / missing asset (no flyover; a missing asset leaves entity
 * 23 disabled, matching the original early-return). The interpreter itself
 * runs as GAME_MODE_FLYOVER — pushed by GAME_MODE_ACTIONSCRIPT_FRAME for its
 * one caller, the PLAY_FLYOVER_SCRIPT callroutine. */
bool play_flyover_script_prepare(uint16_t id, uint16_t *saved_ent23_tick_hi,
                                 uint32_t *script_size);

/* LOAD_BACKGROUND_ANIMATION (load_background_animation.asm) —
 * Sets up BG mode 1, configures BG1/BG2 VRAM locations, loads battle BG.
 * Used by COFFEETEA_SCENE and LOAD_CAST_SCENE. */
void load_background_animation(uint16_t bg1_layer, uint16_t bg2_layer);

/* UNDRAW_FLYOVER_TEXT (undraw_flyover_text.asm) — restore normal BG3 display
 * after flyover text. Reloads battle screen tilemap, window GFX, text tiles,
 * and character window palette. */
void undraw_flyover_text(void);

/* ---- Savestate snapshot ----
 * mode_step_flyover scrolls the flyover text ("War Against Giygas!" etc.) across
 * many frames; its render cursor lives in flyover.c file-statics (the FlyoverState
 * phase/pos is on the captured mode stack, but these render positions are not). One
 * tagged section so a save taken mid-flyover resumes without corrupting the scroll. */
typedef struct {
    uint16_t flyover_vwf_x;
    uint16_t flyover_vwf_y;
    uint16_t flyover_tiles_per_row;
    uint16_t flyover_tile_offset;
    uint16_t flyover_dirty_min;
    uint16_t flyover_dirty_max;
    uint16_t flyover_screen_offset;
    uint16_t flyover_pixel_offset;
    uint16_t flyover_byte_offset;
} FlyoverSaveState;
void flyover_savestate_pack(void *out);   /* out: FlyoverSaveState* */
void flyover_savestate_unpack(const void *in);

#endif /* GAME_FLYOVER_H */
