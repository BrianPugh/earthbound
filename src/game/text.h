#ifndef GAME_TEXT_H
#define GAME_TEXT_H

#include "core/types.h"

/* VWF buffer: 52 tiles x 32 bytes = 1664 bytes */
#define VWF_BUFFER_TILES 52
#define VWF_TILE_BYTES   32  /* 2bpp: 16 bytes upper + 16 bytes lower (8x16 px) */
#define VWF_BUFFER_SIZE  (VWF_BUFFER_TILES * VWF_TILE_BYTES)

/* Font IDs matching FONT enum from assembly */
#define FONT_ID_NORMAL    0
#define FONT_ID_MRSATURN  1
#define FONT_ID_BATTLE    2
#define FONT_ID_TINY      3
#define FONT_ID_LARGE     4

/* Number of characters in a font table */
#define FONT_CHAR_COUNT   96

/* Special EB text character codes (from CHAR enum in include/macros.asm) */
#define EB_CHAR_EQUIPPED  0x22  /* CHAR::EQUIPPED — purple oval with "E" */

/* Initialize the text rendering system.
   Loads fonts from extracted assets and sets up VRAM for text display. */
void text_system_init(void);

/* Load window border graphics from TEXT_WINDOW_GFX asset.
   Decompresses and uploads to VRAM at BG3 tile data area. */
void text_load_window_gfx(void);

/* Upload tiny font glyphs to VRAM at EB character tile positions (0x50-0xAF).
   Call after text_load_window_gfx() to fill in missing character tiles. */
void text_upload_font_tiles(void);

/* Load the flavour palette for window borders.
   flavour: 0-based index (0=plain, 1=mint, etc.) */
void text_load_flavour_palette(uint8_t flavour);

/* Load the character window palette based on party status and flavour.
 * Port of LOAD_CHARACTER_WINDOW_PALETTE (C47F87).
 * If the last party member is unconscious/diamondized (and transitions enabled),
 * loads a special "death" palette; otherwise loads the current text_flavour. */
void load_character_window_palette(void);

/* Animate the text window border palette (HP/PP windows).
 * Port of UPDATE_TEXT_WINDOW_PALETTE (C3E450).
 * Alternates between two sub-palette offsets based on frame_counter. */
void update_text_window_palette(void);

/* Show the HP/PP windows for all party members.
 * Port of SHOW_HPPP_WINDOWS (asm/text/show_hppp_windows.asm).
 * Sets render_hppp_windows=1 and marks all windows for redraw. */
void show_hppp_windows(void);


/* Display the money window (saves/restores text attributes).
 * Port of DISPLAY_MONEY_WINDOW (asm/text/window/display_money_window.asm). */
void display_money_window(void);

/* Open the HP/PP and money display (B/Select button handler).
 * Port of OPEN_HPPP_DISPLAY (asm/text/open_hppp_display.asm).
 * Shows HP/PP windows + money, waits for button press to dismiss.
 * A/L within this display calls open_menu_button(). */
void open_hppp_display(void);

/* OPEN_MENU_BUTTON — Full pause menu (Talk to, Goods, PSI, Equip, Check, Status).
 * Port of asm/overworld/open_menu.asm.
 * Called on A button in overworld, or A/L from HPPP display.
 * Handles all cleanup (close windows, enable entities) before returning. */
void open_menu_button(void);

/* OPEN_MENU_BUTTON_CHECKTALK — Quick talk/check (L button).
 * Port of asm/overworld/open_menu.asm lines 616-644.
 * Tries talk_to first, then check, falls back to "Nothing problem here."
 * Handles all cleanup before returning. */
void open_menu_button_checktalk(void);

/* Set up BG3 as a text overlay layer. */
void text_setup_bg3(void);

/* Print a null-terminated ASCII string at the current text cursor position
   within the current focus window. Stores text for VWF rendering.
   Converts ASCII to EB codes internally before storing. */
void print_string(const char *str);

/* Print raw EB-encoded characters at the current text cursor position.
   Renders immediately via VWF → VRAM → per-window content_tilemap.
   Used by display_text() for script text where bytes are already EB-encoded. */
void print_eb_string(const uint8_t *eb_str, int len);

/* Print an integer value at the current cursor position */
void print_number(int value, int min_digits);

/* Clear the text area of a window */
void clear_window_text(uint16_t window_id);

/* Convert an EarthBound internal character code to ASCII. */
char eb_char_to_ascii(uint8_t eb_char);

/* Convert ASCII to EarthBound internal character code */
uint8_t ascii_to_eb_char(char ascii);

/* --- VWF internal state (public for ending/cast name rendering) --- */
extern uint8_t vwf_buffer[VWF_BUFFER_SIZE];
extern uint16_t vwf_x;
extern uint16_t vwf_tile;
extern uint8_t character_padding;

/* Blit a single VWF glyph into the VWF buffer.
 * glyph_data: 1bpp glyph, height: pixel rows, width: pixel advance. */
void blit_vwf_glyph(const uint8_t *glyph_data, uint8_t height, uint8_t width);

/* --- VWF (Variable Width Font) API --- */

/* Initialize VWF state (clear buffer, reset position) */
void vwf_init(void);

/* Reset VWF VRAM tile allocation for a new frame.
   Call at the start of render_all_windows(). */
void vwf_frame_reset(void);

/* Advance the VWF VRAM tile allocation pointer by count tiles,
   reserving that VRAM range so subsequent VWF rendering won't use it. */
void vwf_reserve_tiles(uint16_t count);

/* Render a single character using VWF.
   eb_char: EarthBound character code (0x50+)
   font_id: which font to use */
void vwf_render_character(uint8_t eb_char, uint8_t font_id);

/* VWF_FLUSH_TILES_TO_VRAM: Port of asm/text/vwf/flush_vwf_tiles_to_vram.asm.
 * Called after each vwf_render_character(). Allocates VRAM tiles for new
 * VWF tile columns, uploads tile data, writes to per-window content_tilemap. */
void vwf_flush_tiles_to_vram(void);

/* ADVANCE_VWF_TILE: Port of asm/text/vwf/advance_vwf_tile.asm.
 * Advances VWF state to the next tile boundary.  Clears
 * TEXT_RENDER_STATE::upper_vram_position so the next flush allocates fresh.
 * Called by set_focus_text_cursor() / SET_WINDOW_TEXT_POSITION. */
void advance_vwf_tile(void);

/* Render a null-terminated ASCII string using VWF at a tilemap position.
   Handles newlines (\n) with 2-tile-row line spacing for 16px fonts.
   Allocates VRAM tiles, uploads tile data, writes tilemap entries. */
void vwf_render_string_at(const char *text, uint16_t x_tile, uint16_t y_tile,
                           uint8_t font_id);

/* Render an EB-encoded character array using VWF at a tilemap position.
   Like vwf_render_string_at() but takes raw EB character codes (0x50+),
   avoiding the lossy ASCII round-trip for characters without ASCII equivalents.
   Stops at null (0x00) or after len characters.
   pixel_offset: sub-tile pixel offset (0-7) to start rendering within the
   first tile column.  Used when text doesn't start on a tile boundary. */
void vwf_render_eb_string_at(const uint8_t *eb_str, int len, uint16_t x_tile,
                              uint16_t y_tile, uint8_t font_id,
                              uint8_t pixel_offset);

/* Render an EB-encoded string using VWF and upload tiles to a specific
   VRAM tile range, bypassing the per-frame VWF allocation pool.
   Matches RENDER_KEYBOARD_INPUT_CHARACTER (C442AC.asm) which uploads
   VWF tiles at VRAM::TEXT_LAYER_TILES + $1700 (tile index 0x2E0).
   Returns the number of VWF tile columns used (each column = 2 VRAM
   tiles for 16px fonts, 1 tile for 8px fonts).
   Does NOT write tilemap entries — caller must do that separately. */
int vwf_render_to_fixed_tiles(const uint8_t *eb_str, int len, uint8_t font_id,
                               uint16_t vram_tile_base);

/* Render a window title string to dedicated VRAM tiles (once, at set_window_title time).
   Matches assembly's RENDER_WINDOW_TITLE → RENDER_TINY_FONT_STRING flow.
   title: ASCII title string.  title_slot: 1-based TITLED_WINDOWS slot.
   Returns the number of VWF tile columns rendered. */
int render_title_to_vram(const char *title, uint8_t title_slot);

/* Set VWF cursor position (in pixels) */
void vwf_set_position(uint16_t pixel_x);

/* VWF word-wrap indent flag — set when wrapping inserts an auto-newline.
 * Port of VWF_INDENT_NEW_LINE BSS variable. */
extern uint8_t vwf_indent_new_line;

/* Clear VWF word-wrap indent flag (called by close_window) */
void clear_vwf_indent_new_line(void);

/* Get pointer to font glyph data (1bpp, variable height) */
const uint8_t *font_get_glyph(uint8_t font_id, uint8_t char_index);

/* Get pixel width of a character in a given font */
uint8_t font_get_width(uint8_t font_id, uint8_t char_index);

/* Get height (in pixel rows) of a font */
uint8_t font_get_height(uint8_t font_id);

/* Print newline in the current focus window.
 * Port of PRINT_NEWLINE (asm/text/print_newline.asm). */
void print_newline(void);

/* Check if a character will fit on the current window line; if not, insert a newline.
 * Port of CHECK_TEXT_FITS_IN_WINDOW (asm/text/check_text_fits_in_window.asm). */
void check_text_fits_in_window(uint16_t eb_char);

/* Compute total pixel width of an EB-encoded string.
 * Port of GET_STRING_PIXEL_WIDTH (asm/text/get_string_pixel_width.asm).
 * max_len: max chars to measure, or -1 for null-terminated. */
uint16_t get_string_pixel_width(const uint8_t *str, int16_t max_len);

/* Print an EB-encoded string with word-wrap awareness.
 * If the string won't fit on the current line, inserts a newline first.
 * Port of PRINT_STRING_WITH_WORDWRAP (asm/text/print_string_with_wordwrap.asm).
 * max_len: max chars to print, or -1 for null-terminated. */
void print_string_with_wordwrap(const uint8_t *str, int16_t max_len);

/* Print an EB-encoded string, splitting at spaces and wrapping each word.
 * Port of PRINT_TEXT_WITH_WORD_SPLITTING (asm/text/print_text_with_word_splitting.asm).
 * max_len: max chars to process, or -1 for null-terminated. */
void print_text_with_word_splitting(const uint8_t *str, int16_t max_len);

#endif /* GAME_TEXT_H */
