#ifndef GAME_ENDING_H
#define GAME_ENDING_H

#include "core/types.h"

/*
 * Ending sequence system — cast scene + staff credits.
 *
 * Ports of:
 *   PLAY_CAST_SCENE              (asm/ending/play_cast_scene.asm)
 *   PLAY_CREDITS                 (asm/ending/play_credits.asm)
 *   LOAD_CAST_SCENE              (asm/ending/load_cast_scene.asm)
 *   INITIALIZE_CREDITS_SCENE     (asm/ending/initialize_credits_scene.asm)
 *   CREDITS_SCROLL_FRAME         (asm/ending/credits_scroll_frame.asm)
 *   HANDLE_CAST_SCROLLING        (asm/ending/handle_cast_scrolling.asm)
 *   COUNT_PHOTO_FLAGS            (asm/ending/count_photo_flags.asm)
 *   TRY_RENDERING_PHOTOGRAPH     (asm/ending/try_rendering_photograph.asm)
 *   SLIDE_CREDITS_PHOTOGRAPH     (asm/ending/slide_credits_photograph.asm)
 *   ENQUEUE_CREDITS_DMA          (asm/ending/enqueue_credits_dma.asm)
 *   PROCESS_CREDITS_DMA_QUEUE    (asm/ending/process_credits_dma_queue.asm)
 *   RENDER_CAST_NAME_TEXT        (asm/ending/render_cast_name_text.asm)
 *   PRINT_CAST_NAME              (asm/ending/print_cast_name.asm)
 *   PRINT_CAST_NAME_ENTITY_VAR0  (asm/ending/print_cast_name_entity_var0.asm)
 *   PRINT_CAST_NAME_PARTY        (asm/ending/print_cast_name_party.asm)
 *   PREPARE_CAST_NAME_TILEMAP    (asm/ending/prepare_cast_name_tilemap.asm)
 *   COPY_CAST_NAME_TILEMAP       (asm/ending/copy_cast_name_tilemap.asm)
 *   PREPARE_DYNAMIC_CAST_NAME_TEXT (asm/ending/prepare_dynamic_cast_name_text.asm)
 *   UPLOAD_SPECIAL_CAST_PALETTE  (asm/ending/upload_special_cast_palette.asm)
 *   SET_CAST_SCROLL_THRESHOLD    (asm/ending/set_cast_scroll_threshold.asm)
 *   CHECK_CAST_SCROLL_THRESHOLD  (asm/ending/check_cast_scroll_threshold.asm)
 *   IS_ENTITY_STILL_ON_CAST_SCREEN (asm/ending/is_entity_still_on_cast_screen.asm)
 *   CREATE_ENTITY_AT_V01_PLUS_BG3Y (asm/ending/create_entity_at_v01_plus_bg3y.asm)
 *   CHANGE_VWF_2BPP_TO_3_COLOUR (asm/ending/change_vwf_2bpp_to_3_colour.asm)
 */

/* PLAY_CAST_SCENE (E1FBE4) — cast scene with entity wipe over battle BG. */
void play_cast_scene(void);

/* PLAY_CREDITS (E1FB03) — staff credits with photo slideshow. */
void play_credits(void);

/* Per-frame callback function pointer.
 * Defaults to process_overworld_tasks(); credits swaps to credits_scroll_frame(). */
typedef void (*frame_callback_fn)(void);
extern frame_callback_fn frame_callback;

/* WRAM 0xB4D1: CAST_TILE_OFFSET — tile base offset for cast name rendering. */
extern uint16_t cast_tile_offset;

/* CREDITS_SCROLL_FRAME — per-frame IRQ callback during credits. */
void credits_scroll_frame(void);

/* HANDLE_CAST_SCROLLING — tick callback for entity scripts during cast scene. */
void handle_cast_scrolling(uint16_t current_entity_slot);

/* UPLOAD_SPECIAL_CAST_PALETTE — callroutine: load palette from decompressed data. */
void upload_special_cast_palette(uint16_t palette_index);

/* SET_CAST_SCROLL_THRESHOLD — callroutine: set var0 = param*8 + BG3_Y_POS. */
void set_cast_scroll_threshold(uint16_t param, uint16_t current_entity_slot);

/* CHECK_CAST_SCROLL_THRESHOLD — callroutine: return 1 if BG3_Y >= var0. */
uint16_t check_cast_scroll_threshold(uint16_t current_entity_slot);

/* IS_ENTITY_STILL_ON_CAST_SCREEN — callroutine: return 1 if entity Y > BG3_Y - 8. */
uint16_t is_entity_still_on_cast_screen(uint16_t current_entity_slot);

/* CREATE_ENTITY_AT_V01_PLUS_BG3Y — callroutine wrapper. */
void create_entity_at_v01_plus_bg3y(uint16_t sprite_id, uint16_t script_id,
                                     uint16_t current_entity_slot);

/* PRINT_CAST_NAME — movement command: render cast name on BG3. */
void print_cast_name(uint16_t cast_index, uint16_t x_col, uint16_t y_row);

/* PRINT_CAST_NAME_ENTITY_VAR0 — movement command: render var0-based name. */
void print_cast_name_entity_var0(uint16_t cast_index, uint16_t x_col,
                                  uint16_t y_row, uint16_t current_entity_slot);

/* PRINT_CAST_NAME_PARTY — movement command: render party member name. */
void print_cast_name_party(uint16_t char_id, uint16_t x_col, uint16_t y_row);

#endif /* GAME_ENDING_H */
