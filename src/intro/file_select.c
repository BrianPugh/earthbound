#include "intro/file_select.h"
#include "game/window.h"
#include "game/text.h"
#include "game/game_state.h"
#include "game/display_text.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/battle_bg.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include "core/memory.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "include/pad.h"
#include "game/overworld.h"
#include "game/inventory.h"
#include "game/battle.h"
#include "include/constants.h"
#include <string.h>
#include <stdio.h>
#include "game_main.h"
#include "data/text_refs.h"

/* Whether each save slot has data */
static uint8_t save_files_present[SAVE_COUNT];

/* current_save_slot is now a global in game_state.c (port of CURRENT_SAVE_SLOT BSS) */

/* Don't Care names — loaded from ROM via asset pipeline.
 * 49 entries (7 categories × 7 names), 6 bytes each, EB-encoded and null-padded. */
#define DONT_CARE_COUNT 7
#define DONT_CARE_NAME_SIZE 6
static const uint8_t *dont_care_names_data;

/* HP_METER_SPEEDS — loaded from ROM via asset pipeline.
 * 3 entries × 4 bytes (32-bit LE fixed-point rolling speeds). */
static const uint8_t *hp_meter_speeds_data;

/* FILE_SELECT_TEXT_PLEASE_NAME_THEM_STRINGS — loaded from ROM via asset pipeline.
 * 7 prompts × NAME_THEM_STRING_LENGTH bytes, EB-encoded and null-padded. */
#define NAMING_PROMPT_ENTRY_SIZE 40  /* NAME_THEM_STRING_LENGTH (US) */
static const uint8_t *naming_prompts_data;

/* INITIAL_STATS — loaded from ROM via asset pipeline.
 * 4 characters × 20 bytes (coords, money, level, EXP, items). */
#define INITIAL_STATS_ENTRY_SIZE 20
static const uint8_t *initial_stats_data;

/* Helper: get EB-encoded don't care name at [category][index] */
static const uint8_t *get_dont_care_name(int category, int index) {
    return dont_care_names_data + (category * DONT_CARE_COUNT + index) * DONT_CARE_NAME_SIZE;
}

/*
 * FILE_SELECT_INIT - Faithful port of asm/system/file_select_init.asm
 *
 * Sets up PPU, loads graphics, and prepares the file select screen.
 */
static void file_select_init(void) {
    /* Step 1: Force blank and wait vblank */
    ppu.inidisp = 0x80;

    /* Step 2: Initialize entity/graphics systems (simplified) */
    /* In the ROM: INIT_ENTITY_SYSTEM, OAM_CLEAR, UPDATE_SCREEN, etc. */
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = VIEWPORT_HEIGHT;
    }

    /* Step 3: Set BG mode $09 (Mode 1 + BG3 priority) */
    ppu.bgmode = 0x09;

    /* Step 4: Clear VRAM */
    memset(ppu.vram, 0, VRAM_SIZE);

    /* Step 5: Initialize text system (load fonts) */
    text_system_init();

    /* Step 6: Set up BG3 for text overlay */
    text_setup_bg3();

    /* Step 7: Upload tiny font tiles to VRAM as fallback fixed-width glyphs.
       Fills EB character positions 0x50-0xAF with font glyphs. */
    text_upload_font_tiles();

    /* Step 7b: Load window border graphics (TEXT_WINDOW_GFX).
       Must run AFTER font tile upload: the scattered mode-0 upload overwrites
       specific tile positions (including 0x51 = cursor lower-half) with the
       correct window graphics data. */
    text_load_window_gfx();

    /* Step 8: Initialize window system */
    window_system_init();

    /* Step 9: Clear BG3 tilemap
       Per assembly: writes 0 to VRAM at TEXT_LAYER_TILEMAP, $800 words */
    /* Clear BG3 tilemap: 32x32 entries = 1024 words = 2048 bytes */
    memset(win.bg2_buffer, 0, BG2_BUFFER_SIZE);
    uint32_t tilemap_offset = VRAM_TEXT_LAYER_TILEMAP * 2;
    uint32_t tilemap_size = 32 * 32 * 2;
    if (tilemap_offset + tilemap_size <= VRAM_SIZE) {
        memset(ppu.vram + tilemap_offset, 0, tilemap_size);
    }

    /* Step 10: Load flavour palette (default = plain/0) */
    text_load_flavour_palette(0);

    /* Step 11: Load battle background #230 (file select BG) */
    battle_bg_load(BATTLEBG_FILE_SELECT);

    /* Starfield BG2 fills viewport; BG3 text stays centered.
     * Clear fill[2] in case a previous screen left it set (e.g. logos). */
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_FILL;
    ppu.bg_viewport_fill[2] = BG_VIEWPORT_CENTER;

    /* Step 11b: Set OBSEL = $62 (OBJ tile base at VRAM word $4000, 8x8/16x16 sizes)
       From OVERWORLD_SETUP_VRAM → SET_OAM_SIZE #$0062 */
    ppu.obsel = 0x62;

    /* Step 11c: Load sprite group ert.palettes (8 × 32 bytes = 256 bytes) into OBJ palette area.
       Assembly: MEMCPY16 from SPRITE_GROUP_PALETTES to PALETTES + BPP4PALETTE_SIZE*8.
       PALETTES maps to ppu.cgram[]; BPP4PALETTE_SIZE*8 = 256 bytes = 128 colors.
       OBJ ert.palettes occupy CGRAM entries 128-255. */
    {
        for (int p = 0; p < 8; p++) {
            size_t pal_size;
            pal_size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_PALETTES(p));
            const uint8_t *pal_data = ASSET_DATA(ASSET_OVERWORLD_SPRITES_PALETTES(p));
            if (pal_data && pal_size >= 32) {
                memcpy(&ppu.cgram[128 + p * 16], pal_data, 32);
                memcpy(&ert.palettes[128 + p * 16], pal_data, 32);
            }
        }
    }

    /* Step 12: Set TM = $16 (BG2 + BG3 + OBJ)
       This is the correct setting - BG2 for battle BG, BG3 for text, OBJ for cursor */
    ppu.tm = 0x16;
    ppu.ts = 0x00;

    /* Sprites use SNES-native coordinates; offset them to match centered BG3 */
    ppu.sprite_x_offset = VIEWPORT_PAD_LEFT;
    ppu.sprite_y_offset = VIEWPORT_PAD_TOP;

    /* Step 13: Clear scroll positions */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    /* BG3 scroll already set in text_setup_bg3 */

    /* Step 14: Set background color */
    ppu.cgram[0] = BGR555(0, 0, 0); /* black backdrop */
}

/*
 * Display the file select menu showing 3 save slots.
 * Returns selected slot (1-3).
 *
 * Faithful port of FILE_SELECT_MENU (file_select_menu.asm):
 *   Pass 1: build menu item labels ("N: CharName" or "N: Start New Game")
 *   Pass 2: for occupied slots, print "Level:" and "Text Speed:" info
 */
static uint16_t file_select_menu(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MAIN);
    if (!w) return 1;

    static char slot_labels[3][32];

    /* Pass 1: Build menu items for each save slot.
     * Assembly (file_select_menu.asm:20-25): LOAD_GAME_SLOT then check
     * favourite_thing[1] != 0 to determine occupancy. */
    for (int slot = 0; slot < SAVE_COUNT; slot++) {
        bool occupied = load_game(slot) && game_state.favourite_thing[1] != 0;
        if (occupied) {
            char name[6] = {0};
            for (int i = 0; i < 5 && party_characters[0].name[i]; i++) {
                name[i] = eb_char_to_ascii(party_characters[0].name[i]);
            }
            snprintf(slot_labels[slot], sizeof(slot_labels[slot]), "%d: %s",
                     slot + 1, name);
            save_files_present[slot] = 1;
        } else {
            snprintf(slot_labels[slot], sizeof(slot_labels[slot]),
                     "%d: Start New Game", slot + 1);
            save_files_present[slot] = 0;
        }
        add_menu_item(slot_labels[slot], (uint16_t)(slot + 1), 0, (uint16_t)slot);
    }

    /* Assembly (file_select_menu.asm:111): OPEN_WINDOW_AND_PRINT_MENU(columns=1)
     * lays out and prints menu item labels BEFORE slot details. */
    open_window_and_print_menu(1, 0);

    /* Pass 2: Display per-slot details for occupied slots.
     * Assembly (file_select_menu.asm:115-214): after OPEN_WINDOW_AND_PRINT_MENU,
     * prints Level and Text Speed info at specific cursor positions. */
    static const char *speed_names[] = {"Fast", "Medium", "Slow"};
    for (int slot = 0; slot < SAVE_COUNT; slot++) {
        if (!save_files_present[slot]) continue;

        load_game(slot);

        /* "Level:" at cursor (9, slot) */
        set_focus_text_cursor(9, (uint16_t)slot);
        print_string("Level:");

        /* Level number at cursor (13, slot) */
        set_focus_text_cursor(13, (uint16_t)slot);
        print_number(party_characters[0].level, 2);

        /* "Text Speed: Fast/Medium/Slow" at cursor (16, slot) */
        set_focus_text_cursor(16, (uint16_t)slot);
        int speed_idx = game_state.text_speed - 1;
        if (speed_idx < 0 || speed_idx > 2) speed_idx = 0;
        char speed_buf[32];
        snprintf(speed_buf, sizeof(speed_buf), "Text Speed: %s",
                 speed_names[speed_idx]);
        print_string(speed_buf);
    }

    /* Assembly (file_select_menu.asm:271): SELECTION_MENU directly —
     * no RENDER_ALL_WINDOWS in between. SELECTION_MENU's internal
     * WINDOW_TICK (selection_menu.asm:186) handles the first render. */
    uint16_t result = selection_menu(0); /* no cancel */
    current_save_slot = (uint8_t)result;
    return result;
}

/*
 * Show the sub-menu for a valid file (Continue/Copy/Delete/Set Up).
 */
static uint16_t show_file_select_submenu(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MENU);
    if (!w) return 0;

    /* Horizontal layout matching assembly (C1F07E): all at text_y=0 */
    add_menu_item("Continue", 1, 0, 0);

    /* Copy is only shown if an empty slot exists to copy to (assembly condition:
       slot != current_save_slot AND SAVE_FILES_PRESENT[slot] == 0) */
    for (int i = 0; i < SAVE_COUNT; i++) {
        if (i != (current_save_slot - 1) && !save_files_present[i]) {
            add_menu_item("Copy", 2, 6, 0);
            break;
        }
    }

    add_menu_item("Delete", 3, 10, 0);
    add_menu_item("Set Up", 4, 15, 0);

    /* Assembly (show_file_select_menu.asm:58-62): PRINT_MENU_ITEMS then
     * SELECTION_MENU directly — no RENDER_ALL_WINDOWS in between. */
    print_menu_items();
    return selection_menu(1);
}

/*
 * Display-only version of FILE_SELECT_MENU — recreate the file select
 * window with slot labels and details, but no selection_menu() call.
 * Matches assembly's FILE_SELECT_MENU(1) path (file_select_menu.asm:
 * @VIRTUAL02 != 0 → skip SELECTION_MENU, just highlight selected slot).
 */
static void file_select_menu_display_only(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MAIN);
    if (!w) return;

    static char slot_labels[3][32];

    for (int slot = 0; slot < SAVE_COUNT; slot++) {
        bool occupied = load_game(slot) && game_state.favourite_thing[1] != 0;
        if (occupied) {
            char name[6] = {0};
            for (int i = 0; i < 5 && party_characters[0].name[i]; i++) {
                name[i] = eb_char_to_ascii(party_characters[0].name[i]);
            }
            snprintf(slot_labels[slot], sizeof(slot_labels[slot]), "%d: %s",
                     slot + 1, name);
            save_files_present[slot] = 1;
        } else {
            snprintf(slot_labels[slot], sizeof(slot_labels[slot]),
                     "%d: Start New Game", slot + 1);
            save_files_present[slot] = 0;
        }
        add_menu_item(slot_labels[slot], (uint16_t)(slot + 1), 0, (uint16_t)slot);
    }

    open_window_and_print_menu(1, 0);

    static const char *speed_names[] = {"Fast", "Medium", "Slow"};
    for (int slot = 0; slot < SAVE_COUNT; slot++) {
        if (!save_files_present[slot]) continue;

        load_game(slot);

        set_focus_text_cursor(9, (uint16_t)slot);
        print_string("Level:");

        set_focus_text_cursor(13, (uint16_t)slot);
        print_number(party_characters[0].level, 2);

        set_focus_text_cursor(16, (uint16_t)slot);
        int speed_idx = game_state.text_speed - 1;
        if (speed_idx < 0 || speed_idx > 2) speed_idx = 0;
        char speed_buf[32];
        snprintf(speed_buf, sizeof(speed_buf), "Text Speed: %s",
                 speed_names[speed_idx]);
        print_string(speed_buf);
    }

    /* Assembly (file_select_menu.asm:247-258): highlight selected slot with palette 6 */
    uint16_t highlight_idx = (current_save_slot > 0) ? current_save_slot - 1 : 0;
    highlight_menu_item(w, highlight_idx, 6, true);
}

/*
 * Display-only versions of setup menus — recreate the window with items
 * printed but no selection_menu() call.  Matches assembly's arg=1 path
 * (FILE_SELECT_TEXT_SPEED_MENU(1), FILE_SELECT_SOUND_MODE_MENU(1))
 * which highlights the previously selected option and returns immediately.
 */
static void text_speed_menu_display_only(uint8_t selected_speed) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_TEXT_SPEED);
    if (!w) return;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select text speed.");
    add_menu_item("Fast", 1, 0, 1);
    add_menu_item("Medium", 2, 0, 2);
    add_menu_item("Slow", 3, 0, 3);
    print_menu_items();

    /* Assembly (file_select_text_speed_menu.asm:49-68): highlight with palette 6 */
    if (selected_speed >= 1 && selected_speed <= 3)
        highlight_menu_item(w, selected_speed - 1, 6, true);
}

static void sound_mode_menu_display_only(uint8_t selected_sound) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MUSIC_MODE);
    if (!w) return;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select sound setting.");
    add_menu_item("Stereo", 1, 0, 1);
    add_menu_item("Mono", 2, 0, 2);
    print_menu_items();

    /* Assembly (file_select_sound_mode_menu.asm:50-69): highlight with palette 6.
     * C port stores sound_setting as 0-indexed (0=Stereo, 1=Mono). */
    if (selected_sound <= 1)
        highlight_menu_item(w, selected_sound, 6, true);
}

/*
 * Text speed selection menu.
 */
static uint16_t text_speed_menu(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_TEXT_SPEED);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Please select text speed.");

    add_menu_item("Fast", 1, 0, 1);
    add_menu_item("Medium", 2, 0, 2);
    add_menu_item("Slow", 3, 0, 3);

    print_menu_items();
    /* Assembly (file_select_text_speed_menu.asm:75) clears ENABLE_WORD_WRAP
     * here so selection_menu's confirm handler routes to APPLY_WINDOW_TEXT_ATTRIBUTES
     * instead of SET_FILE_SELECT_TEXT_HIGHLIGHT.  The C port gates highlight
     * by window ID instead (see selection_menu confirm handler), so this
     * clear is unnecessary and would break word-wrap if not restored. */
    uint16_t result = selection_menu(1);
    if (result > 0) {
        game_state.text_speed = (uint8_t)result;
        /* Compute dt.text_speed_based_wait for TEXT_SPEED_DELAY.
         * Assembly (file_select_menu_loop.asm:681-688):
         * text_speed==3 (Slow) → 0 (instant text, halt at end of line).
         * Otherwise → (text_speed - 1) * 30 frames per character. */
        if (result == 3) {
            dt.text_speed_based_wait = 0;
        } else {
            dt.text_speed_based_wait = result * 30;
        }
    }
    return result;
}

/*
 * Sound mode selection menu.
 */
static uint16_t sound_mode_menu(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MUSIC_MODE);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Please select sound setting.");

    add_menu_item("Stereo", 1, 0, 1);
    add_menu_item("Mono", 2, 0, 2);

    print_menu_items();
    uint16_t result = selection_menu(1);
    if (result > 0) {
        game_state.sound_setting = (uint8_t)(result - 1);
    }
    return result;
}

/*
 * Cursor move callback for flavour menu — previews window appearance as user
 * navigates.  Matches PREVIEW_WINDOW_FLAVOUR (C1EC8F.asm):
 *   1. Save original text_flavour
 *   2. Set text_flavour = new value
 *   3. LOAD_WINDOW_GFX (decompress + flavour-dependent tile generation)
 *   4. UPLOAD_TEXT_TILES_TO_VRAM(2) (re-upload scattered + bulk tiles)
 *   5. LOAD_CHARACTER_WINDOW_PALETTE
 *   6. Restore original text_flavour
 */
static void preview_flavour_callback(uint16_t value) {
    uint8_t orig = game_state.text_flavour;
    game_state.text_flavour = (uint8_t)value;  /* 1-indexed, matching assembly */
    text_load_window_gfx();
    load_character_window_palette();
    game_state.text_flavour = orig;
}

/*
 * Window flavour selection menu.
 */
static uint16_t flavour_menu(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_FLAVOUR);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Which style of windows do you prefer?");

    add_menu_item("Plain flavor", 1, 0, 2);
    add_menu_item("Mint flavor", 2, 0, 3);
    add_menu_item("Strawberry flavor", 3, 0, 4);
    add_menu_item("Banana flavor", 4, 0, 5);
    add_menu_item("Peanut flavor", 5, 0, 6);

    /* Set cursor move callback to preview palette (asm: SET_CURSOR_MOVE_CALLBACK) */
    w->cursor_move_callback = preview_flavour_callback;

    print_menu_items();
    uint16_t result = selection_menu(1);
    if (result > 0) {
        game_state.text_flavour = (uint8_t)result;  /* 1-indexed, matching assembly */
        text_load_flavour_palette(game_state.text_flavour - 1);
    }
    return result;
}

/*
 * Naming screen — faithful port of text_input_dialog.asm + name_a_character.asm.
 *
 * The keyboard window (0x1C: x=1, y=9, w=30, h=16) renders EB char tiles.
 * Each selectable position has a $2F marker tile at an even text_x, with the
 * character tile at text_x+1.  The cursor can only land on positions with
 * $2F tiles.
 *
 * Grid layout (text_y 0-4, plus row 6 for buttons):
 *   Rows 0-2: 9 letters + gap + 2 punctuation  (11 selectable per row)
 *   Row 3:    10 digits + gap + 2 punctuation   (12 selectable)
 *   Row 4:    CAPITAL + small + gap + 2 punctuation
 *   Row 5:    (empty)
 *   Row 6:    Don't Care + Backspace + OK
 *
 * Layouts 0/2 = uppercase, 1/3 = lowercase.  Layout 4/5 = button labels.
 *
 * Font tile VRAM placement:
 *   The original assembly renders keyboard characters through DISPLAY_TEXT,
 *   which uses the normal VWF → FLUSH_VWF_TILES_TO_VRAM path.  Each character
 *   gets a dynamically allocated tile from ALLOC_BG2_TILEMAP_ENTRY (bitmap
 *   allocator).  The C port matches this by rendering each grid character
 *   through vwf_render_eb_string_at(), using the same per-frame VWF tile
 *   allocation pool as all other text.
 */

/* Keyboard grid dimensions for character rows (text_y 0-3) */
#define KB_GRID_ROWS   5   /* rows 0-4 */
#define KB_GRID_COLS  14   /* max columns per row (from offset table) */
#define KB_TOTAL_ROWS  8   /* total text_y lines (0-7, height/2) */

/* EB character code for each grid position.
   Row index = text_y (0-4), column index = grid column (0-13).
   0xFF = no character at this position. */
static const uint8_t kb_upper_grid[KB_GRID_ROWS][KB_GRID_COLS] = {
    /* Row 0: A-I, gap, -, {        */
    { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0xFF, 0xFF, 0x5D, 0x53, 0xFF },
    /* Row 1: J-R, gap, ', ♪($AE)   */
    { 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0xFF, 0xFF, 0x57, 0xAE, 0xFF },
    /* Row 2: S-Z, space, gap, ., /($5F) */
    { 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x50, 0xFF, 0xFF, 0x5E, 0x5F, 0xFF },
    /* Row 3: 0-9, gap, !, |($AC)    */
    { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xFF, 0x51, 0xAC, 0xFF },
    /* Row 4: no chars (CAPITAL/small buttons handled separately) + gap + ?, ♫($AF) */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x6F, 0xAF, 0xFF },
};

static const uint8_t kb_lower_grid[KB_GRID_ROWS][KB_GRID_COLS] = {
    /* Row 0: a-i, gap, -, {        */
    { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0xFF, 0xFF, 0x5D, 0x53, 0xFF },
    /* Row 1: j-r, gap, ', ♪($AE)   */
    { 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0x57, 0xAE, 0xFF },
    /* Row 2: s-z, space, gap, ., /($5F) */
    { 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0x50, 0xFF, 0xFF, 0x5E, 0x5F, 0xFF },
    /* Row 3: 0-9, gap, !, |($AC)    */
    { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xFF, 0x51, 0xAC, 0xFF },
    /* Row 4: no chars + gap + ?, ♫($AF) */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x6F, 0xAF, 0xFF },
};

/* Valid cursor stop positions per row.  Each entry is a text_x where a $2F
   marker tile should be placed.  -1 terminates the list. */
static const int kb_row_stops[KB_TOTAL_ROWS][15] = {
    /* text_y=0 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=1 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=2 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=3 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18,  22, 24,  -1 },
    /* text_y=4 */ { 0, 7,  22, 24,  -1 },
    /* text_y=5 */ { -1 },  /* empty row */
    /* text_y=6 */ { 0, 17, 25,  -1 },
    /* text_y=7 */ { -1 },
};

/* Map from text_x of a cursor stop to the grid column index (for rows 0-3).
   Returns -1 if no grid lookup (handled as button). */
static int kb_text_x_to_grid_col(int text_x) {
    if (text_x >= 0 && text_x <= 18 && (text_x % 2) == 0)
        return text_x / 2;
    if (text_x == 22) return 11;
    if (text_x == 24) return 12;
    return -1;
}

/* Check if a position has a valid cursor stop.
   Uses the static kb_row_stops[] table rather than scanning the tilemap. */
static bool kb_is_valid_stop(WindowInfo *kw, int text_x, int text_y) {
    (void)kw;
    if (text_y < 0 || text_y >= KB_TOTAL_ROWS) return false;
    if (text_x < 0) return false;
    for (int i = 0; kb_row_stops[text_y][i] >= 0; i++) {
        if (kb_row_stops[text_y][i] == text_x) return true;
    }
    return false;
}

/* Find next valid cursor position scanning in (dx, dy) direction.
   Matches FIND_NEXT_MENU_OPTION scanning logic:
   - Primary: scan in dy direction, keeping text_x fixed
   - Secondary: scan in dx direction at each row
   Returns packed (text_y << 8 | text_x), or -1 if none found. */
static int kb_find_next(WindowInfo *kw, int cur_x, int cur_y, int dx, int dy) {
    int half_height = (int)kw->height / 2;
    int width = (int)kw->width;

    if (dy != 0) {
        /* Vertical movement: scan rows in dy direction, try to keep text_x */
        int y = cur_y + dy;
        for (int steps = 0; steps < half_height; steps++) {
            if (y < 0) y = half_height - 1;
            if (y >= half_height) y = 0;
            if (kb_is_valid_stop(kw, cur_x, y))
                return (y << 8) | cur_x;
            /* Try scanning left from cur_x */
            for (int x = cur_x - 1; x >= 0; x--) {
                if (kb_is_valid_stop(kw, x, y))
                    return (y << 8) | x;
            }
            /* Try scanning right from cur_x */
            for (int x = cur_x + 1; x < width; x++) {
                if (kb_is_valid_stop(kw, x, y))
                    return (y << 8) | x;
            }
            y += dy;
        }
    } else if (dx != 0) {
        /* Horizontal movement: scan in dx direction on same row */
        int x = cur_x + dx;
        for (int steps = 0; steps < width; steps++) {
            if (x < 0) x = width - 1;
            if (x >= width) x = 0;
            if (kb_is_valid_stop(kw, x, cur_y))
                return (cur_y << 8) | x;
            x += dx;
        }
    }
    return -1;
}

/* Render a single keyboard character at a window position using VWF.
   Matches assembly: DISPLAY_TEXT renders keyboard text through VWF, with each
   character dynamically allocated from the per-frame VWF tile pool. */
static void kb_write_char(WindowInfo *kw, int text_x, int text_y, uint8_t eb_char) {
    uint16_t mx = kw->content_x + (uint16_t)text_x;
    uint16_t my = kw->content_y + (uint16_t)(text_y * 2);
    uint8_t eb_str[2] = { eb_char, 0 };
    vwf_render_eb_string_at(eb_str, 1, mx, my, FONT_ID_NORMAL, 0);
}

/* Render the selectable character tiles in the keyboard grid via VWF.
   Each character is rendered through the same VWF tile allocation pool used
   by all other text, matching the assembly's DISPLAY_TEXT → VWF path. */
static void kb_render_grid(WindowInfo *kw, bool is_lowercase) {
    const uint8_t (*grid)[KB_GRID_COLS] = is_lowercase ? kb_lower_grid : kb_upper_grid;

    /* Place character tiles for rows 0-4.
       Each cursor stop at text_x has its character at text_x+1. */
    for (int row = 0; row < KB_GRID_ROWS; row++) {
        for (int si = 0; kb_row_stops[row][si] >= 0; si++) {
            int tx = kb_row_stops[row][si];

            int col = kb_text_x_to_grid_col(tx);
            if (col >= 0 && grid[row][col] != 0xFF) {
                kb_write_char(kw, tx + 1, row, grid[row][col]);
            }
        }
    }
}

/* Render label text (CAPITAL, small, Don't Care, Backspace, OK) via VWF
   for proper character packing (no gaps between letters). */
static void kb_render_labels(WindowInfo *kw, bool has_dont_care) {
    uint16_t bx = kw->content_x;  /* base tilemap x (past border) */
    uint16_t by = kw->content_y;  /* base tilemap y (past border) */

    /* Row 4: CAPITAL at text_x=1, small at text_x=8 */
    vwf_render_string_at("CAPITAL", bx + 1, by + 4 * 2, FONT_ID_NORMAL);
    vwf_render_string_at("small",   bx + 8, by + 4 * 2, FONT_ID_NORMAL);

    /* Row 6: buttons */
    if (has_dont_care)
        vwf_render_string_at("Don't Care", bx + 1,  by + 6 * 2, FONT_ID_NORMAL);
    vwf_render_string_at("Backspace",      bx + 18, by + 6 * 2, FONT_ID_NORMAL);
    vwf_render_string_at("OK",             bx + 26, by + 6 * 2, FONT_ID_NORMAL);
}

/* VRAM tile index for name display, matching assembly's
   VRAM::TEXT_LAYER_TILES + $1700 = tile index 0x2E0 relative to BG3 tile base.
   Tiles are uploaded once on change; tilemap entries are written every frame
   since render_all_windows() clears the tilemap. */
#define NAME_VRAM_TILE_BASE 0x2E0

/* Build the EB-encoded display string for the name box.
   Entered chars + bullet at insertion point + dashes for empty slots. */
static void kb_build_name_display(uint8_t *display, const uint8_t *eb_name,
                                   int name_pos, int max_len) {
    for (int i = 0; i < max_len; i++) {
        if (i < name_pos && eb_name[i] != 0)
            display[i] = eb_name[i];   /* entered character */
        else if (i == name_pos)
            display[i] = 0x70;         /* CHAR::BULLET — centered dot at insertion point */
        else
            display[i] = 0x53;         /* placeholder dash (EB 0x53) */
    }
    display[max_len] = 0;
}

/* Upload name display VWF tiles to the dedicated VRAM range.
   Only called when the name changes (matching assembly's
   RENDER_KEYBOARD_INPUT_CHARACTER which is called per-change, not per-frame).
   Returns the number of tile columns rendered. */
static int kb_render_name_tiles(const uint8_t *display, int max_len) {
    return vwf_render_to_fixed_tiles(display, max_len, FONT_ID_NORMAL, NAME_VRAM_TILE_BASE);
}

/* Write tilemap entries for the name display at a specific text row.
   Called every frame since render_all_windows() clears the tilemap.
   Matches the @WRITE_CHAR_TO_TILEMAP loop in RENDER_KEYBOARD_INPUT_CHARACTER. */
static void kb_write_name_tilemap_at(WindowInfo *nw, int num_cols, int text_y) {
    uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
    uint16_t mx = nw->content_x;
    uint16_t my = nw->content_y + (uint16_t)(text_y * 2);
    for (int i = 0; i < num_cols; i++) {
        if (mx + i >= 32) break;
        uint16_t upper_tile = NAME_VRAM_TILE_BASE + i * 2;
        uint16_t lower_tile = NAME_VRAM_TILE_BASE + i * 2 + 1;
        tilemap[my * 32 + mx + i] = 0x2000 | upper_tile;
        if (my + 1 < 32)
            tilemap[(my + 1) * 32 + mx + i] = 0x2000 | lower_tile;
    }
}

/* Convenience wrapper: name display at text row 0 (for file select name box). */
static void kb_write_name_tilemap(WindowInfo *nw, int num_cols) {
    kb_write_name_tilemap_at(nw, num_cols, 0);
}

/*
 * text_input_dialog — shared keyboard input loop for naming screens.
 * Port of TEXT_INPUT_DIALOG (asm/text/text_input_dialog.asm).
 *
 * Creates the keyboard window (0x1C), runs the input loop, closes on done.
 * Name display tiles are rendered in name_display_window_id at text row
 * name_text_y (0 for file select name box, 1 for NAMING_PROMPT where row 0
 * shows the prompt text).
 *
 * name_buf:    output EB-encoded name (must be at least max_len bytes)
 * max_len:     maximum characters to accept
 * naming_index: -1 for standalone (no Don't Care), >=0 for Don't Care cycling
 * name_display_window_id: window where name VWF tiles are shown
 * name_text_y: text row within that window for the name display
 * existing_name: if non-NULL, pre-fill with this EB-encoded name (up to max_len)
 *
 * Returns: 0 = confirmed (name written to name_buf), -1 = cancelled/back
 */
int text_input_dialog(uint8_t *name_buf, int max_len, int naming_index,
                      uint16_t name_display_window_id, int name_text_y,
                      const uint8_t *existing_name)
{
    bool has_dont_care = (naming_index >= 0);

    /* Create keyboard window (assembly: TEXT_INPUT_DIALOG line 43) */
    create_window(WINDOW_FILE_SELECT_NAMING_KB);
    win.current_focus_window = WINDOW_FILE_SELECT_NAMING_KB;

    /* Don't Care row counter — starts at -1, incremented to 0 on first press,
       then cycles 0→1→2→3→4→5→6→0→... (assembly: @LOCAL0A) */
    int dont_care_row = -1;

    /* Name ert.buffer in EB encoding */
    uint8_t eb_name[32];
    memset(eb_name, 0, sizeof(eb_name));
    int name_pos = 0;

    /* Pre-fill from existing name if provided */
    if (existing_name) {
        for (int i = 0; i < max_len && existing_name[i] != 0; i++) {
            eb_name[i] = existing_name[i];
            name_pos++;
        }
    }

    int name_tile_cols = 0;

    /* Cursor position in window text coordinates */
    int cur_x = 0, cur_y = 0;
    bool is_lowercase = false;
    int frame_counter = 0;

    while (!platform_input_quit_requested()) {
        /* Reset VWF VRAM tile allocation pointer each frame — without this,
           kb_render_grid() and kb_render_labels() advance vwf_vram_next every
           frame, eventually overwriting sprite tile data in VRAM. */
        vwf_frame_reset();
        /* Reserve VRAM tiles for the name display (at 0x2E0+) so the keyboard
           grid VWF rendering doesn't allocate into the same range.  Each name
           character uses 2 VRAM tiles (upper+lower for 16px font). */
        vwf_reserve_tiles(max_len * 2);

        WindowInfo *kw = get_window(WINDOW_FILE_SELECT_NAMING_KB);
        WindowInfo *nw = get_window(name_display_window_id);
        if (!kw || !nw) break;

        /* Render all windows (borders + any VWF text for prompt).
           This clears the tilemap, so we must re-render keyboard tiles after. */
        render_all_windows();

        /* Render keyboard grid character tiles */
        kb_render_grid(kw, is_lowercase);

        /* Render label text via VWF (CAPITAL, small, Don't Care, etc.) */
        kb_render_labels(kw, has_dont_care);

        /* Re-render name VWF tiles every frame.  Even though the name only
           changes on input, the keyboard grid VWF rendering writes to the same
           VRAM range (0x2E0+) as the name display tiles, so we must re-upload
           the name tiles after the grid to keep them visible. */
        {
            uint8_t display[32];
            kb_build_name_display(display, eb_name, name_pos, max_len);
            name_tile_cols = kb_render_name_tiles(display, max_len);
        }
        kb_write_name_tilemap_at(nw, name_tile_cols, name_text_y);

        /* Draw blinking cursor matching selection_menu() two-frame animation:
           Frame 0 (big):  tiles 0x41/0x51  — CURSOR_FRAME0 from TEXT_WINDOW_GFX
           Frame 1 (small): tiles 0x28D/0x29D — CURSOR_FRAME1 from TEXT_WINDOW_GFX
           Toggle every 10 frames; always draw one frame (never skip). */
        {
            bool cursor_frame = ((frame_counter / 10) % 2 != 0);
            uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
            uint16_t mx = kw->content_x + (uint16_t)cur_x;
            uint16_t my = kw->content_y + (uint16_t)(cur_y * 2);
            if (mx < 32 && my + 1 < 32) {
                if (!cursor_frame) {
                    tilemap[my * 32 + mx]       = 0x2441; /* tile 0x41, pal 1, pri */
                    tilemap[(my + 1) * 32 + mx] = 0x2451; /* tile 0x51, pal 1, pri */
                } else {
                    tilemap[my * 32 + mx]       = 0x268D; /* tile 0x28D, pal 1, pri */
                    tilemap[(my + 1) * 32 + mx] = 0x269D; /* tile 0x29D, pal 1, pri */
                }
            }
        }
        frame_counter++;

        /* Sync win.bg2_buffer to VRAM (NMI DMA equivalent) */
        upload_battle_screen_to_vram();

        /* Run entity scripts for naming screen animations */
        oam_clear();
        run_actionscript_frame();
        render_all_priority_sprites();

        /* Sync palette mirror to CGRAM (NMI handler does this in ROM) */
        sync_palettes_to_cgram();

        /* Update battle BG animation */
        battle_bg_update();

        wait_for_vblank();

        uint16_t pressed = platform_input_get_pad_new();

        /* Directional input — assembly: MOVE_CURSOR plays SFX only on
           successful cursor movement (SFX 124 for UP/DOWN, 123 for LEFT/RIGHT) */
        if (pressed & PAD_UP) {
            int found = kb_find_next(kw, cur_x, cur_y, 0, -1);
            if (found >= 0) {
                cur_y = (found >> 8) & 0xFF;
                cur_x = found & 0xFF;
                play_sfx(124);
            }
        }
        if (pressed & PAD_DOWN) {
            int found = kb_find_next(kw, cur_x, cur_y, 0, 1);
            if (found >= 0) {
                cur_y = (found >> 8) & 0xFF;
                cur_x = found & 0xFF;
                play_sfx(124);
            }
        }
        if (pressed & PAD_LEFT) {
            int found = kb_find_next(kw, cur_x, cur_y, -1, 0);
            if (found >= 0) {
                cur_y = (found >> 8) & 0xFF;
                cur_x = found & 0xFF;
                play_sfx(123);
            }
        }
        if (pressed & PAD_RIGHT) {
            int found = kb_find_next(kw, cur_x, cur_y, 1, 0);
            if (found >= 0) {
                cur_y = (found >> 8) & 0xFF;
                cur_x = found & 0xFF;
                play_sfx(123);
            }
        }

        /* A/L button: select */
        if (pressed & PAD_CONFIRM) {
            if (cur_y == 6) {
                /* Row 6: buttons */
                if (cur_x == 0 && has_dont_care) {
                    /* Don't Care — cycle through preset names (assembly: @DONTCARE_SELECTED).
                       Counter starts at -1; on each press: if >= 6 reset to 0, else increment.
                       Then load dont_care_names[naming_index][row] into eb_name. */
                    play_sfx(122); /* SFX::TEXT_INPUT */
                    if (dont_care_row >= 6)
                        dont_care_row = 0;
                    else
                        dont_care_row++;
                    const uint8_t *eb_src = get_dont_care_name(naming_index, dont_care_row);
                    memset(eb_name, 0, sizeof(eb_name));
                    name_pos = 0;
                    for (int i = 0; i < DONT_CARE_NAME_SIZE && i < max_len && eb_src[i] != 0; i++) {
                        eb_name[i] = eb_src[i];
                        name_pos++;
                    }
                    /* Continue loop — assembly jumps back to @RENDER_CURSOR */
                } else if (cur_x == 17) {
                    /* Backspace */
                    play_sfx(122); /* SFX::TEXT_INPUT */
                    if (name_pos > 0) {
                        name_pos--;
                        eb_name[name_pos] = 0;

                    }
                } else if (cur_x == 25) {
                    /* OK */
                    play_sfx(94); /* SFX::NAMING_CONFIRM */
                    goto confirm_name;
                }
            } else if (cur_y == 4) {
                /* Row 4: CAPITAL/small toggle or punctuation */
                play_sfx(122); /* SFX::TEXT_INPUT */
                if (cur_x == 0) {
                    is_lowercase = false;
                } else if (cur_x == 7) {
                    is_lowercase = true;
                } else {
                    /* Punctuation at column 11/12 */
                    int col = kb_text_x_to_grid_col(cur_x);
                    const uint8_t (*grid)[KB_GRID_COLS] = is_lowercase ? kb_lower_grid : kb_upper_grid;
                    if (col >= 0 && grid[4][col] != 0xFF && name_pos < max_len) {
                        eb_name[name_pos] = grid[4][col];
                        name_pos++;

                    }
                }
            } else if (cur_y >= 0 && cur_y <= 3) {
                /* Character rows 0-3 */
                play_sfx(122); /* SFX::TEXT_INPUT */
                int col = kb_text_x_to_grid_col(cur_x);
                const uint8_t (*grid)[KB_GRID_COLS] = is_lowercase ? kb_lower_grid : kb_upper_grid;
                if (col >= 0 && grid[cur_y][col] != 0xFF && name_pos < max_len) {
                    eb_name[name_pos] = grid[cur_y][col];
                    name_pos++;
                }
            }
        }

        /* B/SELECT: backspace (assembly: @CHECK_B_SELECT) */
        if (pressed & PAD_CANCEL) {
            play_sfx(125); /* SFX::NAMING_BACKSPACE */
            if (name_pos > 0) {
                name_pos--;
                eb_name[name_pos] = 0;
            } else if (naming_index != -1) {
                /* No characters and not standalone — go back to previous screen.
                   Assembly only closes the keyboard window; name box and message
                   are left for the caller. */
                close_window(WINDOW_FILE_SELECT_NAMING_KB);
                return -1;
            }
        }

        /* START: confirm */
        if (pressed & PAD_START) {
            play_sfx(126); /* SFX::NAMING_CONFIRM_ALT */
            goto confirm_name;
        }

        continue;

    confirm_name:
        /* Only confirm if at least one character entered
           (assembly: @CONFIRM_NAME checks STRLEN of KEYBOARD_INPUT_CHARACTERS) */
        if (name_pos > 0) {
            memset(name_buf, 0, (size_t)max_len);
            for (int i = 0; i < name_pos && i < max_len; i++)
                name_buf[i] = eb_name[i];
            close_window(WINDOW_FILE_SELECT_NAMING_KB);
            return 0;
        }
    }

    close_window(WINDOW_FILE_SELECT_NAMING_KB);
    return -1;
}

static int name_a_character(uint8_t *name_buf, int max_len, const uint8_t *eb_prompt,
                            int eb_prompt_len, int naming_index) {
    /* Assembly order (name_a_character.asm):
       1. CREATE_WINDOW #FILE_SELECT_NAMING_NAME_BOX
       2. Initialize name display (INITIALIZE_KEYBOARD_INPUT_VWF)
       3. CREATE_WINDOW #FILE_SELECT_NAMING_MESSAGE + display prompt
       4. Wait for button press
       5. TEXT_INPUT_DIALOG creates the keyboard window */

    /* Step 1: Create name box window first (assembly line 31) */
    create_window(WINDOW_FILE_SELECT_NAMING_BOX);

    /* Step 2: Create message window and display prompt (assembly lines 119-125) */
    WindowInfo *msg_w = create_window(WINDOW_FILE_SELECT_NAMING_MSG);
    if (msg_w) {
        set_focus_text_cursor(0, 0);
        print_eb_string(eb_prompt, eb_prompt_len);
    }

    /* Render initial name display (bullet + dashes) during prompt wait.
       Assembly does this via INITIALIZE_KEYBOARD_INPUT_VWF before message window. */
    int name_tile_cols;
    {
        uint8_t empty_name[16];
        uint8_t display[16];
        memset(empty_name, 0, sizeof(empty_name));
        kb_build_name_display(display, empty_name, 0, max_len);
        name_tile_cols = kb_render_name_tiles(display, max_len);
    }
    while (!platform_input_quit_requested()) {
        vwf_frame_reset();
        render_all_windows();
        {
            WindowInfo *nw = get_window(WINDOW_FILE_SELECT_NAMING_BOX);
            if (nw) kb_write_name_tilemap(nw, name_tile_cols);
        }
        upload_battle_screen_to_vram();
        oam_clear();
        run_actionscript_frame();
        render_all_priority_sprites();
        sync_palettes_to_cgram();
        battle_bg_update();
        wait_for_vblank();
        if (platform_input_get_pad_new() & PAD_ANY_BUTTON)
            break;
    }

    /* Step 5: Keyboard input via shared text_input_dialog.
       Name display goes in the name box window at text row 0. */
    return text_input_dialog(name_buf, max_len, naming_index,
                             WINDOW_FILE_SELECT_NAMING_BOX, 0, NULL);
}

/*
 * RENDER_FRAME_TICK (C1004E.asm) — overworld path.
 * Executes one frame: OAM clear, run scripts, draw, sync, vblank.
 * Used during naming screen wait loops (init_naming_screen_events).
 */
static void render_frame_tick_naming(void) {
    /* OAM_CLEAR — hide all sprites and reset priority queues */
    oam_clear();

    /* RUN_ACTIONSCRIPT_FRAME (entity script execution + draw list) */
    run_actionscript_frame();

    /* Flush queued sprites to OAM */
    render_all_priority_sprites();

    /* NMI equivalent: win.bg2_buffer sync + palette sync + BG animation + vblank */
    upload_battle_screen_to_vram();
    sync_palettes_to_cgram();
    battle_bg_update();
    wait_for_vblank();
}

/*
 * INIT_NAMING_SCREEN_EVENTS (C4D830.asm)
 *
 * After a character has been named, assigns return animation scripts to the
 * walk-in entities and waits for all entity scripts to complete.
 *
 * 1. Wait for ert.wait_for_naming_screen_actionscript == 0
 * 2. Read return animation entity list (entry 7 + naming_index)
 * 3. For each entity: find by sprite_id, reassign to return animation script
 * 4. Wait for all entity scripts (slots 0 to PARTY_LEADER_ENTITY_INDEX-2) to finish
 */
static void init_naming_screen_events(uint16_t naming_index) {
    /* Wait for any pending actionscript to complete (assembly lines 12-16) */
    while (ert.wait_for_naming_screen_actionscript != 0) {
        render_frame_tick_naming();
    }

    /* Read the return animation entity list (entries 7 + naming_index).
     * Same data format as display_animated_naming_sprite, but entries 7-13
     * contain (sprite_id, script_id) pairs for the walk-out animations. */
    uint16_t entry = 7 + naming_index;
    if (!naming_entities_data || entry >= NAMING_SCREEN_ENTITY_COUNT)
        return;

    uint32_t ptr_off = NAMING_ENTITIES_PTR_TABLE_OFF + (uint32_t)entry * 4;
    if (ptr_off + 4 > naming_entities_data_size)
        return;

    uint32_t ptr = read_u32_le(&naming_entities_data[ptr_off]);

    uint16_t within_bank = (uint16_t)(ptr & 0xFFFF);
    uint16_t buf_off = within_bank - NAMING_ENTITIES_ROM_BASE;

    /* Walk entity list: .WORD sprite_id, .WORD script_id, terminated by 0.
     * For each entity: find by sprite_id, reassign to return animation script.
     * (Assembly lines 26-62) */
    while (buf_off + 2 <= naming_entities_data_size) {
        uint16_t sprite_id = read_u16_le(&naming_entities_data[buf_off]);
        if (sprite_id == 0)
            break;  /* Terminator */

        uint16_t script_id = read_u16_le(&naming_entities_data[buf_off + 2]);

        int16_t slot = find_entity_by_sprite_id(sprite_id);
        if (slot >= 0) {
            reassign_entity_script(slot, script_id);
        }

        buf_off += 4;
    }

    /* Wait for all entity scripts (slots 0 to PARTY_LEADER_ENTITY_INDEX-2)
     * to complete (script_table == -1 for all).
     * Assembly (lines 63-84): AND all script_table values; when ALL are -1,
     * the AND result is -1, and the loop exits. */
    while (1) {
        int16_t result = -1;
        for (int slot = 0; slot < PARTY_LEADER_ENTITY_INDEX - 1; slot++) {
            result &= entities.script_table[ENT(slot)];
        }
        render_frame_tick_naming();
        if (result == -1)
            break;
    }

    /* All entities are now deactivated.  The assembly achieves a clean VRAM
     * and spritemap state via per-entity DEALLOCATE_ENTITY_SPRITE calls in
     * the walk-out scripts.  As a safety net, do a bulk clear here so the
     * next naming screen starts with a clean slate. */
    memset(sprite_vram_table, 0, sizeof(sprite_vram_table));
    clear_overworld_spritemaps();
}

/*
 * Create animated party sprites on the confirmation screen.
 * Port of CREATE_FILE_SELECT_PARTY_SPRITES (C4D8FA.asm).
 * Spawns 5 entities (Ness, Paula, Jeff, Poo, King) next to their name windows.
 */
static void create_file_select_party_sprites(void) {
    static const struct {
        uint16_t sprite;
        uint16_t script;
        int16_t x, y;
    } configs[] = {
        {   1, 861,  40,  44 },  /* Ness */
        {   2, 861,  40,  76 },  /* Paula */
        {   3, 861,  40, 108 },  /* Jeff */
        {   4, 861,  40, 140 },  /* Poo */
        { 359, 534, 136,  40 },  /* King (sleeping dog) */
    };
    for (int i = 0; i < 5; i++) {
        int16_t ent = create_entity(configs[i].sprite, configs[i].script, -1,
                                     configs[i].x, configs[i].y);
        if (ent >= 0)
            entities.directions[ENT(ent)] = 4; /* DIRECTION::DOWN */
    }
}

/*
 * Compute the pixel width of an EB-encoded text string.
 * Port of GET_TEXT_PIXEL_WIDTH (C44FF3.asm).
 * Sums (char_width + CHARACTER_PADDING) for each character.
 */
static uint16_t get_text_pixel_width(uint8_t font_id, const uint8_t *eb_text, int len) {
    uint16_t total = 0;
    for (int i = 0; i < len; i++) {
        uint8_t char_index = (eb_text[i] - 0x50) & 0x7F;
        total += font_get_width(font_id, char_index) + 1; /* +1 = CHARACTER_PADDING */
    }
    return total;
}

/*
 * Print an EB-encoded name right-justified on row 1 of the focus window.
 * Port of the right-justification algorithm in file_select_menu_loop.asm (lines 338-382).
 *
 * Algorithm:
 *   1. pixel_width = GET_TEXT_PIXEL_WIDTH(font=0, eb_text, strlen)
 *   2. width_tiles = pixel_width / 8
 *   3. remainder = pixel_width % 8
 *   4. If remainder != 0 OR width_tiles == 6: width_tiles += 1
 *   5. cursor_x = window_width - width_tiles
 *   6. SET_FOCUS_TEXT_CURSOR(cursor_x, 1)
 */
static void print_right_justified_name(const uint8_t *eb_name, int len,
                                        uint16_t window_id) {
    /* Find string length (stop at null) */
    int slen = 0;
    while (slen < len && eb_name[slen] != 0) slen++;

    uint16_t pixel_width = get_text_pixel_width(FONT_ID_NORMAL, eb_name, slen);
    uint16_t width_tiles = pixel_width / 8;
    uint16_t remainder = pixel_width % 8;

    if (remainder != 0 || width_tiles == 6)
        width_tiles++;

    WindowInfo *w = get_window(window_id);
    if (!w) return;

    /* Assembly (create_window.asm) subtracts 2 from config width for borders.
       w->width in C is the raw config value; subtract 2 for interior width. */
    uint16_t interior_width = w->width - 2;
    uint16_t cursor_x = interior_width - width_tiles;
    set_focus_text_cursor(cursor_x, 1);

    /* Print the EB-encoded name directly (no lossy ASCII round-trip) */
    int print_len = slen < 15 ? slen : 15;
    print_eb_string(eb_name, print_len);
}

/*
 * Run the new game naming flow.
 */
static bool new_game_naming(void) {
    /* Matching assembly @CHANGE_TO_NAMING_SCREEN_MUSIC → @UNKNOWN18:
       1. Change music
       2. Close all windows (file select, flavour, etc.)
       3. Enter naming loop with DISPLAY_ANIMATED_NAMING_SPRITE per character */
    change_music(2); /* MUSIC::NAMING_SCREEN */
    close_all_windows();

    /* Load event script data for naming screen animations (EVENT_502-533).
     * This loads the EVENT_SCRIPT_POINTERS table, script bytecode banks
     * for C3/C4, and the naming screen entity table. */
    load_event_script_data();
    load_sprite_data();

    /* Naming prompts loaded from ROM asset (EB-encoded, NAMING_PROMPT_ENTRY_SIZE per entry) */

    struct {
        uint8_t *buf;
        int max_len;
    } name_targets[THINGS_NAMED_COUNT];

    name_targets[0] = (typeof(name_targets[0])){party_characters[0].name, 5};
    name_targets[1] = (typeof(name_targets[0])){party_characters[1].name, 5};
    name_targets[2] = (typeof(name_targets[0])){party_characters[2].name, 5};
    name_targets[3] = (typeof(name_targets[0])){party_characters[3].name, 5};
    name_targets[4] = (typeof(name_targets[0])){game_state.pet_name, 6};
    name_targets[5] = (typeof(name_targets[0])){game_state.favourite_food, 6};
    name_targets[6] = (typeof(name_targets[0])){game_state.favourite_thing + 4, 6};

    /* Initialize entity system once before the naming loop.
     * Assembly: INIT_ENTITY_SYSTEM runs at FILE_SELECT_INIT time;
     * entities then accumulate and deactivate naturally via scripts. */
    entity_system_init();
    clear_overworld_spritemaps();
    memset(sprite_vram_table, 0, sizeof(sprite_vram_table));

    int i = 0;
    while (i < THINGS_NAMED_COUNT) {
        if (platform_input_quit_requested()) return false;

        /* Create animated walk-in entities for this naming screen.
         * Assembly: DISPLAY_ANIMATED_NAMING_SPRITE creates entities from
         * NAMING_SCREEN_ENTITIES table — each character has sprite/script pairs
         * that animate them walking onto the screen. */
        display_animated_naming_sprite((uint16_t)i);

        const uint8_t *eb_prompt = naming_prompts_data + i * NAMING_PROMPT_ENTRY_SIZE;
        int result = name_a_character(name_targets[i].buf, name_targets[i].max_len,
                                      eb_prompt, NAMING_PROMPT_ENTRY_SIZE, i);

        /* Assembly (file_select_menu_loop.asm @NAMING_ADVANCE):
         * ALWAYS call INIT_NAMING_SCREEN_EVENTS with the current index
         * BEFORE advancing. This runs the return animation (entities walk
         * off screen) and waits for all entity scripts to complete. */
        init_naming_screen_events((uint16_t)i);

        if (result == 0) {
            /* Confirmed — advance to next character */
            i++;
        } else if (result == -1) {
            /* Cancelled — go back to previous character */
            if (i > 0) {
                i--;
            } else {
                /* Going back past the first character — assembly
                   (@NAMING_NEXT_CHARACTER, index == $FFFF) calls
                   CLOSE_ALL_WINDOWS and returns to flavour selection. */
                close_all_windows();
                return false;
            }
        }
    }

    /* Set default names for any empty slots (first name in each group).
     * Names are already EB-encoded in the ROM data — copy directly. */
    for (int c = 0; c < 4; c++) {
        if (party_characters[c].name[0] == 0) {
            const uint8_t *eb_src = get_dont_care_name(c, 0);
            for (int j = 0; j < 5 && eb_src[j] != 0; j++)
                party_characters[c].name[j] = eb_src[j];
        }
    }
    if (game_state.pet_name[0] == 0) {
        const uint8_t *eb_src = get_dont_care_name(4, 0);
        for (int j = 0; j < 6 && eb_src[j] != 0; j++)
            game_state.pet_name[j] = eb_src[j];
    }
    if (game_state.favourite_food[0] == 0) {
        const uint8_t *eb_src = get_dont_care_name(5, 0);
        for (int j = 0; j < 6 && eb_src[j] != 0; j++)
            game_state.favourite_food[j] = eb_src[j];
    }
    if (game_state.favourite_thing[4] == 0) {
        const uint8_t *eb_src = get_dont_care_name(6, 0);
        for (int j = 0; j < 6 && eb_src[j] != 0; j++)
            game_state.favourite_thing[4 + j] = eb_src[j];
    }

    /* Set "PSI " prefix on favourite_thing */
    game_state.favourite_thing[0] = ascii_to_eb_char('P');
    game_state.favourite_thing[1] = ascii_to_eb_char('S');
    game_state.favourite_thing[2] = ascii_to_eb_char('I');
    game_state.favourite_thing[3] = ascii_to_eb_char(' ');

    /* Show confirmation.
     * Assembly (file_select_menu_loop.asm line 307): CLOSE_ALL_WINDOWS,
     * then SET_INSTANT_PRINTING, then creates confirmation windows.
     * All naming entities are already deactivated by init_naming_screen_events. */
    close_all_windows();
    set_instant_printing();

    /* Display all names for confirmation (print EB-encoded directly) */
    for (int c = 0; c < 4; c++) {
        WindowInfo *cw = create_window((uint16_t)(WINDOW_FILE_SELECT_CONFIRM_NESS + c));
        if (cw) {
            set_focus_text_cursor(0, 0);
            int len = 0;
            while (len < 5 && party_characters[c].name[len] != 0x00) len++;
            print_eb_string(party_characters[c].name, len);
        }
    }

    /* Dog name */
    WindowInfo *kw = create_window(WINDOW_FILE_SELECT_CONFIRM_KING);
    if (kw) {
        set_focus_text_cursor(0, 0);
        int len = 0;
        while (len < 6 && game_state.pet_name[len] != 0x00) len++;
        print_eb_string(game_state.pet_name, len);
    }

    /* Favorite food — assembly prints label on row 0, right-justifies value on row 1 */
    create_window(WINDOW_FILE_SELECT_CONFIRM_FOOD);
    set_focus_text_cursor(0, 0);
    print_string("Favorite food:");
    print_right_justified_name(game_state.favourite_food, 6, WINDOW_FILE_SELECT_CONFIRM_FOOD);

    /* Favorite thing — same layout as food */
    create_window(WINDOW_FILE_SELECT_CONFIRM_THING);
    set_focus_text_cursor(0, 0);
    print_string("Coolest thing:");
    print_right_justified_name(game_state.favourite_thing + 4, 6, WINDOW_FILE_SELECT_CONFIRM_THING);

    /* Are you sure? */
    WindowInfo *mw = create_window(WINDOW_FILE_SELECT_CONFIRM_MSG);
    if (mw) {
        set_focus_text_cursor(0, 0);
        print_string("Are you sure?");
        /* Horizontal layout matching assembly (file_select_menu_loop.asm):
           Yep at text_x=14, Nope at text_x=18, both text_y=0 */
        add_menu_item("Yep", 1, 14, 0);
        add_menu_item("Nope", 0, 18, 0);
    }

    /* Assembly (file_select_menu_loop.asm:470-471): PRINT_MENU_ITEMS then
     * CREATE_FILE_SELECT_PARTY_SPRITES — no RENDER_ALL_WINDOWS in between. */
    print_menu_items();

    /* Spawn animated party sprites next to name windows (assembly line 471) */
    create_file_select_party_sprites();

    /* Assembly (file_select_menu_loop.asm:474): LDA #1; JSR SELECTION_MENU
     * — B button enabled, returns 0 (same as "Nope"). */
    uint16_t confirm = selection_menu(1);

    if (confirm == 0) {
        /* Nope — clear entities and redo naming (assembly line 478) */
        entity_system_init();
        close_all_windows();
        return new_game_naming();
    }

    /* Yep - initialize new game */
    change_music(158); /* MUSIC::NAME_CONFIRMATION */

    /* Wait for confirmation animation (assembly lines 487-497: 180-frame loop) */
    wait_frames_or_button(180, 0);

    /* Clear map entities after animation (assembly line 498) */
    entity_system_init();

    /* Initialize character stats from INITIAL_STATS (asm/data/initial_stats.asm).
     * Assembly: file_select_menu_loop.asm lines 501-576.
     * For each character:
     *   1. RESET_CHAR_LEVEL_ONE(char_id, level, 0) — sets base stats and levels up
     *   2. GAIN_EXP if initial EXP > 0
     *   3. Copy max HP/PP → current (sync after level-ups)
     *   4. Clear items and set from INITIAL_STATS */

    /* Initialize all 4 characters from INITIAL_STATS ROM data.
     * Per-entry layout: coords(4), money(2), level(2), EXP(2), items(10) = 20 bytes. */
    for (int c = 0; c < 4; c++) {
        const uint8_t *entry = initial_stats_data + c * INITIAL_STATS_ENTRY_SIZE;
        uint16_t level = read_u16_le(entry + 6);
        uint16_t exp = read_u16_le(entry + 8);

        reset_char_level_one(c + 1, level, 0);
        if (exp > 0) gain_exp(0, c + 1, exp);

        memset(party_characters[c].items, ITEM_NONE, sizeof(party_characters[c].items));
        for (int j = 0; j < 10; j++) {
            uint8_t item = entry[10 + j];
            if (item != 0) party_characters[c].items[j] = item;
        }
    }

    /* Sync current HP/PP = max for all characters (assembly lines 538-555) */
    for (int i = 0; i < 4; i++) {
        party_characters[i].current_hp = party_characters[i].max_hp;
        party_characters[i].current_hp_target = party_characters[i].max_hp;
        party_characters[i].current_pp = party_characters[i].max_pp;
        party_characters[i].current_pp_target = party_characters[i].max_pp;
    }

    /* Ness's starting money from INITIAL_STATS[0] */
    game_state.money_carried = read_u16_le(initial_stats_data + 4);
    game_state.party_count = 1;
    game_state.player_controlled_party_count = 1;
    game_state.current_party_members = 1;

    /* Set starting position from INITIAL_STATS[0]::coords × 8
       (file_select_menu_loop.asm lines 593-603 → PLACE_LEADER_AT_POSITION) */
    {
        uint16_t start_x = read_u16_le(initial_stats_data + 0);
        uint16_t start_y = read_u16_le(initial_stats_data + 2);
        place_leader_at_position(start_x * 8, start_y * 8);
    }

    /* Assembly lines 604-612: Prepend "PSI " to favourite_thing.
       The naming screen fills favourite_thing+4 onward; we set the prefix here. */
    game_state.favourite_thing[0] = 0x80; /* CHAR::P */
    game_state.favourite_thing[1] = 0x83; /* CHAR::S_ */
    game_state.favourite_thing[2] = 0x79; /* CHAR::I */
    game_state.favourite_thing[3] = 0x50; /* CHAR::SPACE */

    /* Assembly line 644-645: unknownC3 = 1 */
    game_state.unknownC3 = 1;

    /* Assembly lines 647-650: Save leader position as respawn point */
    ow.respawn_x = game_state.leader_x_coord;
    ow.respawn_y = game_state.leader_y_coord;

    /* Assembly line 651: clear any pending NPC/door interactions */
    reset_queued_interactions();

    /* Assembly lines 652-654: Move leader to prologue/meteorite scene location */
    place_leader_at_position(2112, 1768);

    /* Assembly lines 655-656: Freeze entities and queue prologue text.
     * MSG_EVT_PROLOGUE_NEW = $C5E70B */
    freeze_and_queue_text_interaction(MSG_EVT0_PROLOGUE_METEORITE_FALL);

    /* Assembly lines 657-659: Disable monster spawns for prologue */
    event_flag_set(EVENT_FLAG_MONSTER_OFF);

    /* Assembly lines 660-661: Enable NPC display */
    ow.show_npc_flag = 1;

    /* NOTE: No save here. The assembly does NOT call SAVE_GAME_SLOT after
     * naming/initialization. The first real save happens later when the
     * player calls Dad on the phone (via CC_1F SAVE_GAME in the event
     * script). The incremental saves during text speed/sound/flavour
     * selection are handled by those individual menu functions. */

    close_all_windows();

    return true;
}

/*
 * Main file select loop.
 * Ported from FILE_SELECT_INIT + RUN_FILE_MENU in assembly.
 */
uint16_t file_menu_loop(void) {
    /* Load asset data for file select screen */
    dont_care_names_data = ASSET_DATA(ASSET_US_DATA_DONT_CARE_NAMES_BIN);
    hp_meter_speeds_data = ASSET_DATA(ASSET_DATA_HP_METER_SPEEDS_BIN);
    naming_prompts_data = ASSET_DATA(ASSET_US_DATA_NAMING_PROMPTS_BIN);
    initial_stats_data = ASSET_DATA(ASSET_DATA_INITIAL_STATS_BIN);

    /* Run the faithful FILE_SELECT_INIT sequence */
    file_select_init();

    /* Fade in (matching assembly: FADE_IN with X=1) */
    fade_in(1, 1);

    /* Wait for fade to complete while updating BG animation */
    while (fade_active()) {
        battle_bg_update();
        fade_update();
        wait_for_vblank();
    }

    while (!platform_input_quit_requested()) {
        /* Update battle BG animation each frame */
        battle_bg_update();

        /* Show file select menu */
        uint16_t selected = file_select_menu();
        if (selected == 0) continue;

        int slot = selected - 1;

        if (save_files_present[slot]) {
            /* Existing save — assembly @VALID_FILE_SELECTED (line 37):
             * show submenu, dispatch action, loop back here on cancel
             * from Setup or after Copy/Delete complete. */
        valid_file_selected: {
            uint16_t action = show_file_select_submenu();

            switch (action) {
            case 0: /* B pressed — assembly @MENU_B_PRESSED (line 50-52) */
                close_focus_window();
                continue;

            case 1: /* Continue — assembly @MENU_STARTGAME_SELECTED (lines 53-60) */
                load_game(slot);
                reset_queued_interactions();
                reload_hotspots();
                /* Assembly lines 56-59: Save leader position as respawn point */
                ow.respawn_x = game_state.leader_x_coord;
                ow.respawn_y = game_state.leader_y_coord;
                if (game_state.text_speed == 3)
                    dt.text_speed_based_wait = 0;
                else
                    dt.text_speed_based_wait = (uint16_t)game_state.text_speed * 30;
                /* Assembly lines 665-678: set HP meter rolling speed from
                 * HP_METER_SPEEDS table based on text_speed. */
                {
                    int idx = (game_state.text_speed & 0xFF) - 1;
                    if (idx < 0) idx = 0;
                    if (idx > 2) idx = 2;
                    bt.hp_meter_speed = (int32_t)read_u32_le(hp_meter_speeds_data + idx * 4);
                }
                close_all_windows();
                return 1;

            case 2: /* Copy — assembly @MENU_COPY_SELECTED (lines 61-65) */
                close_all_windows();
                continue;

            case 3: /* Delete — faithful port of CONFIRM_FILE_DELETE (C1F2A8) */
                {
                    close_focus_window();
                    WindowInfo *dw = create_window(WINDOW_FILE_SELECT_DELETE);
                    if (dw) {
                        /* Row 0: confirmation prompt */
                        set_focus_text_cursor(0, 0);
                        print_string("Are you sure you want to delete?");

                        /* Row 1: slot number + character name + level */
                        set_focus_text_cursor(0, 1);
                        print_number(current_save_slot, 1);

                        set_focus_text_cursor(2, 1);
                        {
                            int len = 0;
                            while (len < 5 && party_characters[0].name[len] != 0x00) len++;
                            print_eb_string(party_characters[0].name, len);
                        }

                        set_focus_text_cursor(8, 1);
                        print_string("Level:");

                        set_focus_text_cursor(12, 1);
                        print_number(party_characters[0].level, 2);

                        /* Menu items: No at (0,2), Yes at (0,3) */
                        add_menu_item("No", 0, 0, 2);
                        add_menu_item("Yes", 1, 0, 3);
                    }
                    /* Assembly (confirm_file_delete.asm:65-67): PRINT_MENU_ITEMS
                     * then SELECTION_MENU directly — no RENDER_ALL_WINDOWS. */
                    print_menu_items();
                    uint16_t del_confirm = selection_menu(1);
                    if (del_confirm == 1) {
                        /* Assembly (confirm_file_delete.asm:71-74): ERASE_SAVE
                         * zeroes both copies of the save block in SRAM. */
                        save_files_present[slot] = 0;
                        erase_save(slot);
                    }
                    close_all_windows();
                }
                continue;

            case 4: /* Set Up — asm lines 71-99: windows stack on confirm,
                     * B-press closes only current window and goes back. */
                {
                    load_game(slot);

                setup_text_speed: {
                    uint16_t ts = text_speed_menu();
                    if (ts == 0) {
                        close_window(WINDOW_FILE_SELECT_TEXT_SPEED);
                        /* Assembly line 80: BRA @VALID_FILE_SELECTED —
                         * return to submenu, not to slot selection. */
                        goto valid_file_selected;
                    }
                }

                setup_sound_mode: {
                    uint16_t sm = sound_mode_menu();
                    if (sm == 0) {
                        close_window(WINDOW_FILE_SELECT_MUSIC_MODE);
                        goto setup_text_speed;
                    }
                }

                {
                    uint16_t fl = flavour_menu();
                    if (fl == 0) {
                        close_window(WINDOW_FILE_SELECT_FLAVOUR);
                        goto setup_sound_mode;
                    }
                }

                    /* Assembly @MENU_OTHER_SELECTED (line 98-100) */
                    save_game(slot);
                    close_all_windows();
                }
                continue;
            }
        }
        } else {
            /* New game — asm lines 101-132: windows stack/persist as the
             * user advances.  B-press on a menu closes only that window
             * and returns to the previous menu.  CLOSE_ALL_WINDOWS only
             * happens when entering the naming screen. */
            game_state_init();

        new_game_text_speed: {
            uint16_t ts = text_speed_menu();
            if (ts == 0) {
                close_window(WINDOW_FILE_SELECT_TEXT_SPEED);
                continue; /* back to file select */
            }
        }

        new_game_sound_mode: {
            uint16_t sm = sound_mode_menu();
            if (sm == 0) {
                close_window(WINDOW_FILE_SELECT_MUSIC_MODE);
                goto new_game_text_speed;
            }
        }

        new_game_flavour: {
            uint16_t fl = flavour_menu();
            if (fl == 0) {
                close_window(WINDOW_FILE_SELECT_FLAVOUR);
                goto new_game_sound_mode;
            }
        }

            /* Naming screen */
            current_save_slot = (uint8_t)selected;
            if (!new_game_naming()) {
                /* Assembly (file_select_menu_loop.asm lines 141-150):
                 * When backing out of naming, re-display all setup windows
                 * as backdrops (display-only with no selection_menu call),
                 * change music back to setup screen, then re-enter flavour
                 * selection for user input.
                 * Assembly calls: CLOSE_ALL_WINDOWS, FILE_SELECT_MENU(1),
                 *   FILE_SELECT_TEXT_SPEED_MENU(1), FILE_SELECT_SOUND_MODE_MENU(1),
                 *   CHANGE_MUSIC(SETUP_SCREEN), BRA @NEW_GAME_OPEN_FLAVOUR */
                /* Save user's choices before file_select_menu_display_only
                 * clobbers game_state via load_game() calls. */
                uint8_t saved_text_speed = game_state.text_speed;
                uint8_t saved_sound_setting = game_state.sound_setting;
                close_all_windows();
                file_select_menu_display_only();
                text_speed_menu_display_only(saved_text_speed);
                sound_mode_menu_display_only(saved_sound_setting);
                change_music(3);  /* MUSIC::SETUP_SCREEN */
                goto new_game_flavour;
            }
            /* Assembly lines 665-678: set HP meter rolling speed */
            {
                int idx = (game_state.text_speed & 0xFF) - 1;
                if (idx < 0) idx = 0;
                if (idx > 2) idx = 2;
                bt.hp_meter_speed = (int32_t)read_u32_le(hp_meter_speeds_data + idx * 4);
            }
            return 1;
        }
    }

    return 0;
}
