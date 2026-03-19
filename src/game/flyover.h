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

/* PLAY_FLYOVER_SCRIPT (C49EC4) — bytecode interpreter for flyover text scripts.
 * id: 0-7 index into FLYOVER_TEXT_POINTERS table. */
void play_flyover_script(uint16_t id);

/* COFFEETEA_SCENE (coffee_tea_scene.asm) — coffee/tea break special event.
 * type: 0 = coffee, 1 = tea. */
void coffeetea_scene(uint16_t type);

/* LOAD_BACKGROUND_ANIMATION (load_background_animation.asm) —
 * Sets up BG mode 1, configures BG1/BG2 VRAM locations, loads battle BG.
 * Used by COFFEETEA_SCENE and LOAD_CAST_SCENE. */
void load_background_animation(uint16_t bg1_layer, uint16_t bg2_layer);

/* UNDRAW_FLYOVER_TEXT (undraw_flyover_text.asm) — restore normal BG3 display
 * after flyover text. Reloads battle screen tilemap, window GFX, text tiles,
 * and character window palette. */
void undraw_flyover_text(void);

#endif /* GAME_FLYOVER_H */
