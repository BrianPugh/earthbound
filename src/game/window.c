#include "game/window.h"
#include "game/text.h"
#include "game/display_text.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game/battle_bg.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include "game/overworld.h"
#include "entity/entity.h"
#include "snes/ppu.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "include/pad.h"
#include "include/constants.h"
#include "data/assets.h"
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "game_main.h"

/* Window configuration table: position/size for each window ID.
   Matches WINDOW_CONFIGURATION_TABLE from asm/data/text/window_configuration_table.asm (USA path).
   Format: {x, y, width, height} in tiles.
   These are the original SNES values (32-tile tilemap).  At non-native viewport
   widths the PPU renderer centers the tilemap so tiles 1..30 are visible at
   240px, preserving all original window positions without per-entry adaptation.

   WINDOW_X_NUDGE: tiles lost per side when the viewport is narrower than 256px
   (e.g. 1 tile at 240px).  Applied to edge-hugging windows so they keep a
   visible margin instead of being flush with (or clipped by) the screen edge. */
#define WINDOW_X_NUDGE_ ((SNES_WIDTH - VIEWPORT_WIDTH) / 2 / 8)
#define WINDOW_X_NUDGE (WINDOW_X_NUDGE_ > 0 ? WINDOW_X_NUDGE_ : 0)
static const uint16_t window_configs[][4] = {
    [0x00] = {  1 + WINDOW_X_NUDGE,  1, 13,  8 },  /* Command menu (Talk to, Goods, PSI, ...) */
    [0x01] = { 12,  1, 19,  8 },  /* Out-of-battle text / PSI ability list */
    [0x02] = {  7,  1, 24, 16 },  /* Main inventory window */
    [0x03] = {  1 + WINDOW_X_NUDGE,  1,  6, 10 },  /* Inventory menu */
    [0x04] = {  1,  3, 11,  6 },  /* PSI target/cost ("To one of us", "PP Cost:") */
    [0x05] = { 20,  1, 11, 16 },  /* Phone menu */
    [0x06] = {  8,  1, 20, 10 },  /* Equip menu */
    [0x07] = { 18,  1, 13, 16 },  /* Item list for equip */
    [0x08] = {  1,  1, 30, 18 },  /* Status menu */
    [0x09] = { 12,  1, 19, 18 },  /* Status stats panel */
    [0x0A] = {  1 + WINDOW_X_NUDGE, 10,  8,  4 },  /* Carried money */
    [0x0B] = {  1 + WINDOW_X_NUDGE, 15, 11,  4 },  /* Status equipment panel */
    [0x0C] = { 12,  1, 19, 16 },  /* Store item list */
    [0x0D] = {  7,  1, 24, 16 },  /* Escargo Express item list */
    [0x0E] = {  4,  1, 24,  6 },  /* In-battle text */
    [0x0F] = {  1 + WINDOW_X_NUDGE,  1, 21,  6 },  /* Normal battle menu */
    [0x10] = {  4,  1,  8,  8 },  /* Battle PSI category */
    [0x11] = { 12,  1, 12,  4 },  /* Battle PSI name */
    [0x12] = {  1 + WINDOW_X_NUDGE,  1, 14,  6 },  /* Jeff's Battle menu */
    [0x13] = {  1,  2, 30,  8 },  /* File Select */
    [0x14] = {  5,  9, 22,  4 },  /* Overworld Menu */
    [0x15] = { 10, 16, 12,  8 },  /* Copy Menu (2 choices) */
    [0x16] = { 10, 16, 12,  6 },  /* Copy Menu (1 choice) */
    [0x17] = {  6, 17, 21, 10 },  /* Delete confirmation */
    [0x18] = {  3, 14, 16, 10 },  /* Text Speed */
    [0x19] = {  8, 15, 18,  8 },  /* Music Mode */
    [0x1A] = {  5,  4,  8,  4 },  /* Naming Box */
    [0x1B] = { 13,  4, 17,  4 },  /* "Name This Friend" */
    [0x1C] = {  1,  9, 30, 16 },  /* Name input box */
    [0x1D] = {  7,  3,  7,  4 },  /* Ness's Name */
    [0x1E] = {  7,  7,  7,  4 },  /* Paula's Name */
    [0x1F] = {  7, 11,  7,  4 },  /* Jeff's Name */
    [0x20] = {  7, 15,  7,  4 },  /* Poo's Name */
    [0x21] = { 20,  3,  8,  4 },  /* King's Name */
    [0x22] = { 15,  7, 13,  6 },  /* Favourite Food */
    [0x23] = { 15, 13, 13,  6 },  /* Favourite Thing */
    [0x24] = {  4, 21, 24,  4 },  /* "Are you sure?" */
    [0x25] = { 18,  6, 13,  8 },  /* Store purchase info */
    [0x26] = { 12,  1, 12,  4 },  /* Battle action name */
    [0x27] = {  3,  3, 26,  6 },  /* Naming prompt */
    [0x28] = {  1,  1,  7,  4 },  /* Targeting prompt (Who?/Which?/Where?/Whom?) */
    [0x29] = { 16,  8, 15,  4 },  /* PSI level (short) */
    [0x2A] = { 10,  8, 21,  4 },  /* PSI level (medium) */
    [0x2B] = {  4,  8, 27,  4 },  /* PSI level (wide) */
    [0x2C] = {  8 - WINDOW_X_NUDGE,  2, 24, 16 },  /* Overworld character select */
    [0x2D] = {  3, 11, 15,  6 },  /* Equipment stats */
    [0x2E] = {  4,  1,  8, 10 },  /* Status PSI category */
    [0x2F] = {  1,  9, 30, 10 },  /* PSI description */
    [0x30] = {  1,  1, 28,  6 },  /* Battle menu (full) */
    [0x31] = { 10,  4, 20,  4 },  /* Battle target text */
    [0x32] = { 14, 11, 15, 16 },  /* Flavour selection */
    [0x33] = { 22,  8,  9,  4 },  /* Single character select */
    [0x34] = {  7,  9, 18, 18 },  /* Debug menu (US only) */
};
#define WINDOW_CONFIG_COUNT (sizeof(window_configs) / sizeof(window_configs[0]))

/* windows[], titled_windows[], and menu_backup_* now in WindowSystemState win. */
WindowSystemState win = {
    .current_focus_window = WINDOW_ID_NONE,
    .battle_menu_current_character_id = -1,
};

void window_system_init(void) {
    memset(win.windows, 0, sizeof(win.windows));
    memset(win.tilemap_pool, 0, sizeof(win.tilemap_pool));
    win.current_focus_window = WINDOW_ID_NONE;
    for (int i = 0; i < MAX_TITLED_WINDOWS; i++)
        win.titled_windows[i] = WINDOW_ID_NONE;
}

/* Allocate a contiguous block from the shared tilemap pool.
 * Simple first-fit scan over existing window allocations. */
static uint16_t *tilemap_pool_alloc(uint16_t count) {
    if (count == 0 || count > WINDOW_TILEMAP_POOL_SIZE) return NULL;

    /* Collect existing allocations as (offset, size) pairs, sorted by offset. */
    uint16_t offsets[MAX_WINDOWS], sizes[MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].content_tilemap && win.windows[i].content_tilemap_size > 0) {
            uint16_t off = (uint16_t)(win.windows[i].content_tilemap - win.tilemap_pool);
            /* Insertion sort by offset */
            int j = n;
            while (j > 0 && offsets[j - 1] > off) {
                offsets[j] = offsets[j - 1];
                sizes[j] = sizes[j - 1];
                j--;
            }
            offsets[j] = off;
            sizes[j] = win.windows[i].content_tilemap_size;
            n++;
        }
    }

    /* First-fit: scan gaps between allocations */
    uint16_t candidate = 0;
    for (int i = 0; i < n; i++) {
        if (candidate + count <= offsets[i])
            return &win.tilemap_pool[candidate];
        candidate = offsets[i] + sizes[i];
    }
    /* Check trailing space */
    if (candidate + count <= WINDOW_TILEMAP_POOL_SIZE)
        return &win.tilemap_pool[candidate];

    return NULL; /* pool exhausted */
}

static void tilemap_pool_free(WindowInfo *w) {
    w->content_tilemap = NULL;
    w->content_tilemap_size = 0;
}

WindowInfo *create_window(uint16_t window_id) {
    /* Find existing or free slot */
    int slot = -1;
    WindowInfo *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == window_id) {
            w = &win.windows[i];
            break;
        }
        if (!win.windows[i].active && slot < 0)
            slot = i;
    }

    if (!w) {
        /* New window — allocate slot and load configuration */
        if (slot < 0) return NULL; /* no free slots */

        w = &win.windows[slot];
        memset(w, 0, sizeof(*w));
        w->active = true;
        w->id = window_id;

        if (window_id < WINDOW_CONFIG_COUNT && window_configs[window_id][2] > 0) {
            w->x = window_configs[window_id][0];
            w->y = window_configs[window_id][1];
            w->width = window_configs[window_id][2];
            w->height = window_configs[window_id][3];
        } else {
            w->x = 2; w->y = 2; w->width = 28; w->height = 8;
        }
        w->content_x = w->x + 1;
        w->content_y = w->y + 1;
    }

    /* Reinitialize window state — shared path for both new and existing windows.
     * Assembly (create_window.asm @UNKNOWN8): always resets text cursor, font,
     * tile attributes, menu state, tilemap ert.buffer, and cursor_move_callback,
     * even when the window was already open. */
    win.current_focus_window = window_id;
    w->text_x = 0;
    w->text_y = 0;
    w->number_padding = 128;
    w->curr_tile_attributes = 0;
    w->font = 0;
    w->selected_option = WINDOW_ID_NONE;
    w->current_option = WINDOW_ID_NONE;
    w->menu_page_number = 1;
    w->cursor_move_callback = NULL;
    w->menu_count = 0;
    w->palette_index = 0;
    w->title[0] = '\0';

    /* Clear per-window content tilemap (assembly: free tiles via FREE_TILE_SAFE,
     * then fill with tile 64 / BLANK_TILE).
     * C port uses 0 since render_all_windows checks for zero and skips.
     * Must free old tiles on reopen to avoid BG2 tile allocation leak. */
    if (w->content_tilemap) {
        for (uint16_t i = 0; i < w->content_tilemap_size; i++)
            free_tile_safe(w->content_tilemap[i]);
    }

    /* (Re)allocate from shared pool — size may change if window was reopened
     * with different dimensions (unlikely but safe). */
    uint16_t needed = (w->width - 2) * (w->height - 2);
    if (needed > WINDOW_TILEMAP_MAX) needed = WINDOW_TILEMAP_MAX;
    if (!w->content_tilemap || w->content_tilemap_size != needed) {
        tilemap_pool_free(w);
        w->content_tilemap = tilemap_pool_alloc(needed);
        assert(w->content_tilemap && "tilemap pool exhausted — increase WINDOW_TILEMAP_POOL_SIZE");
        w->content_tilemap_size = needed;
    }
    memset(w->content_tilemap, 0, w->content_tilemap_size * sizeof(uint16_t));

    /* Assembly (create_window.asm:312-316) */
    vwf_init();
    ow.redraw_all_windows = 1;
    clear_party_sprite_hide_flags();

    return w;
}

void close_window(uint16_t window_id) {
    if (window_id == WINDOW_ID_NONE) return;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == window_id) {
            /* Clear current focus (assembly lines 26-30) */
            if (win.current_focus_window == window_id)
                win.current_focus_window = WINDOW_ID_NONE;

            /* Clear menu options (assembly line 33: CLEAR_WINDOW_MENU_OPTIONS) */
            win.windows[i].menu_count = 0;
            win.windows[i].selected_option = WINDOW_ID_NONE;
            win.windows[i].current_option = WINDOW_ID_NONE;
            win.windows[i].menu_page_number = 1;

            /* Free title slot (assembly close_window.asm:264-272) */
            if (win.windows[i].title_slot != 0) {
                uint8_t slot_idx = win.windows[i].title_slot - 1;
                if (slot_idx < MAX_TITLED_WINDOWS)
                    win.titled_windows[slot_idx] = WINDOW_ID_NONE;
                win.windows[i].title_slot = 0;
                win.windows[i].title_tile_count = 0;
            }

            /* Clear window area from BG3 tilemap (assembly lines 140-259).
             * MUST be done before setting active=false, because clear_window_text()
             * calls get_window() which checks w->active. */
            clear_window_text(window_id);

            /* Return tilemap allocation to shared pool */
            tilemap_pool_free(&win.windows[i]);

            win.windows[i].active = false;

            /* Set redraw flag (assembly line 280-281) */
            ow.redraw_all_windows = 1;

            /* Clear pagination window if this was it (assembly lines 283-287) */
            if (dt.pagination_window == window_id)
                dt.pagination_window = WINDOW_ID_NONE;

            /* Extra tick + clear instant printing (assembly lines 288-294).
             * Only when not called from close_all_windows (which sets
             * extra_tick_on_window_close=1 to skip this). The C port's
             * close_all_windows doesn't call close_window, so this always runs. */
            window_tick_without_instant_printing();
            clear_instant_printing();

            /* Clear VWF word-wrap indent (assembly line 298) */
            clear_vwf_indent_new_line();
            /* Reset VWF state (assembly line 261: JSL RESET_VWF_TEXT_STATE) */
            vwf_init();
            return;
        }
    }
}

void close_all_windows(void) {
    /* Assembly (C1008E.asm): sets EXTRA_TICK_ON_WINDOW_CLOSE=1, then
     * calls CLOSE_WINDOW for each window (which skips per-window tick
     * due to the flag). After all closed: CLEAR_INSTANT_PRINTING,
     * WINDOW_TICK, INIT_USED_BG2_TILE_MAP. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active) {
            /* Clear menu state */
            win.windows[i].menu_count = 0;
            win.windows[i].selected_option = WINDOW_ID_NONE;
            win.windows[i].current_option = WINDOW_ID_NONE;
            win.windows[i].menu_page_number = 1;

            /* Free title slot */
            if (win.windows[i].title_slot != 0) {
                uint8_t slot_idx = win.windows[i].title_slot - 1;
                if (slot_idx < MAX_TITLED_WINDOWS)
                    win.titled_windows[slot_idx] = WINDOW_ID_NONE;
                win.windows[i].title_slot = 0;
                win.windows[i].title_tile_count = 0;
            }

            clear_window_text(win.windows[i].id);
            tilemap_pool_free(&win.windows[i]);
            win.windows[i].active = false;
        }
    }
    win.current_focus_window = WINDOW_ID_NONE;
    dt.pagination_window = WINDOW_ID_NONE;
    ow.redraw_all_windows = 1;

    /* Post-close cleanup (assembly lines 25-31) */
    clear_instant_printing();
    window_tick();
    init_used_bg2_tile_map();

    clear_vwf_indent_new_line();
}

void close_focus_window(void) {
    if (win.current_focus_window != WINDOW_ID_NONE) {
        close_window(win.current_focus_window);
    }
}

bool any_window_open(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active)
            return true;
    }
    return false;
}

WindowInfo *get_window(uint16_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == window_id)
            return &win.windows[i];
    }
    return NULL;
}

/* SET_WINDOW_FOCUS — Port of asm/text/set_window_focus.asm. */
void set_window_focus(uint16_t window_id) {
    win.current_focus_window = window_id;
}

void set_focus_text_cursor(uint16_t x, uint16_t y) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (w) {
        /* Assembly: SET_FOCUS_TEXT_CURSOR → SET_WINDOW_TEXT_POSITION
         * → ADVANCE_VWF_TILE (USA only), then sets text_x/text_y. */
        advance_vwf_tile();
        w->text_x = x;
        w->text_y = y;
        w->cursor_pixel_x = x * 8;
    }
}

/*
 * SET_WINDOW_TITLE (asm/text/set_window_title.asm)
 *
 * Copies a title string into the window's title ert.buffer.
 * Assembly: A=window_id, X=max_chars (-1 for full string), PARAM00=title_ptr.
 * Title is rendered in TINY font on the top border by render_all_windows().
 */
void set_window_title(uint16_t window_id, const char *title, int max_len) {
    WindowInfo *w = get_window(window_id);
    if (!w || !title) return;

    /* Assembly (render_window_title.asm:25-61): allocate a TITLED_WINDOWS slot
     * if one hasn't been allocated yet (title_slot == 0). */
    if (w->title_slot == 0) {
        for (int s = 0; s < MAX_TITLED_WINDOWS; s++) {
            if (win.titled_windows[s] == WINDOW_ID_NONE) {
                win.titled_windows[s] = window_id;
                w->title_slot = (uint8_t)(s + 1); /* 1-indexed */
                break;
            }
        }
    }

    int limit = (max_len > 0 && max_len < WINDOW_TITLE_SIZE)
                ? max_len : (WINDOW_TITLE_SIZE - 1);
    int i;
    for (i = 0; i < limit && title[i] != '\0'; i++) {
        w->title[i] = title[i];
    }
    w->title[i] = '\0';

    /* Render title glyphs to dedicated VRAM tiles once (matches assembly
     * RENDER_WINDOW_TITLE → RENDER_TINY_FONT_STRING). */
    if (w->title_slot > 0)
        w->title_tile_count = (uint8_t)render_title_to_vram(w->title, w->title_slot);
}

void add_menu_item(const char *label, uint16_t userdata, uint16_t text_x, uint16_t text_y) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count >= 24) return;

    MenuItem *item = &w->menu_items[w->menu_count];
    strncpy(item->label, label, MENU_LABEL_SIZE - 1);
    item->label[MENU_LABEL_SIZE - 1] = '\0';
    item->userdata = userdata;
    item->text_x = (uint8_t)text_x;
    item->text_y = (uint8_t)text_y;
    item->script = 0;
    w->menu_count++;
}

/*
 * ADD_MENU_ITEM_NO_POSITION — Port of asm/text/menu/add_menu_item_no_position.asm.
 *
 * Adds a menu item with auto-computed position. The actual text_x/text_y
 * are set later by open_window_and_print_menu() (LAYOUT_MENU_OPTIONS).
 * Sets type=2 (userdata return mode) matching assembly line 29-30.
 */
void add_menu_item_no_position(const char *label, uint16_t userdata) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count >= 24) return;

    MenuItem *item = &w->menu_items[w->menu_count];
    strncpy(item->label, label, MENU_LABEL_SIZE - 1);
    item->label[MENU_LABEL_SIZE - 1] = '\0';
    item->userdata = userdata;
    item->type = 2;   /* assembly: STA #2, menu_option::unknown0 */
    item->text_x = 0;
    item->text_y = 0;
    w->menu_count++;
}

/*
 * PRINT_MENU_ITEMS — Port of asm/text/print_menu_items.asm.
 *
 * Iterates the focus window's menu items and prints each label
 * at its pre-set text_x/text_y position. Used when items have explicit
 * positions (e.g., party_character_selector sets text_x = index * 6).
 *
 * Assembly: walks the linked-list menu option chain, sets the text cursor
 * for each option, and calls PRINT_TEXT_IN_WINDOW. C port: iterates the
 * menu_items array and calls print_string().
 */
/* CLEAR_FOCUS_WINDOW_MENU_OPTIONS — Port of asm/text/window/clear_focus_window_menu_options.asm.
 * Resets the menu item count of the focus window to 0. */
void clear_focus_window_menu_options(void) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (w) w->menu_count = 0;
}

/* GET_WINDOW_MENU_OPTION_COUNT — Port of asm/text/window/get_window_menu_option_count.asm.
 * Returns the number of menu items in the given window. */
uint16_t get_window_menu_option_count(uint16_t window_id) {
    WindowInfo *w = get_window(window_id);
    if (!w) return 0;
    return w->menu_count;
}

void print_menu_items(void) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) {
        /* Assembly: STA #$FF → EARLY_TICK_EXIT when no options */
        dt.early_tick_exit = 0xFF;
        return;
    }

    set_instant_printing();  /* assembly line 36 */

    /* Assembly (print_menu_items.asm lines 38-44): only print items on
     * the current page (page == menu_page_number) or always-visible
     * items (page == 0, i.e. the overflow indicator). */
    for (uint16_t i = 0; i < w->menu_count; i++) {
        MenuItem *item = &w->menu_items[i];
        if (item->page != w->menu_page_number && item->page != 0)
            continue;
        /* CLEAR_MENU_OPTION_TEXT: write marker tile at column 0 to clear stale VWF */
        set_focus_text_cursor(item->text_x, item->text_y);
        print_char_with_sound(47);
        advance_vwf_tile();
        set_focus_text_cursor(item->text_x + 1, item->text_y);
        w = get_window(win.current_focus_window);  /* re-fetch after cursor set */
        if (!w) return;
        print_string(item->label);
    }
}

/*
 * OPEN_WINDOW_AND_PRINT_MENU — Port of asm/text/window/open_window_and_print_menu.asm.
 *
 * Assembly calls LAYOUT_MENU_OPTIONS (C451FA.asm) which is a complex
 * layout engine. It arranges menu items into columns/rows based on
 * window dimensions and item count.
 *
 * Simplified C port: computes positions for items arranged in a grid
 * with the given number of columns, then prints each item's label
 * into the window using print_string.
 *
 * columns: number of columns for layout (1 = single column, 2 = two columns)
 * start_index: if non-zero, reassign userdata starting from this value
 */
void open_window_and_print_menu(uint16_t columns, uint16_t start_index) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) return;

    if (columns < 1) columns = 1;

    /* Calculate column width in tile units.
     * Interior width = window width - 2 (borders). Divide among columns. */
    uint16_t interior_w = w->width - 2;
    uint16_t col_width = interior_w / columns;

    /* Assembly LAYOUT_MENU_OPTIONS (line 155-156): max_rows = height / 2.
     * Uses raw height (LSR), not border-adjusted. */
    uint16_t max_rows = w->height / 2;
    uint16_t total_items = w->menu_count;
    uint16_t rows_needed = (total_items + columns - 1) / columns;
    bool needs_pagination = (rows_needed > max_rows);

    /* If paginated, assembly (layout_menu_options.asm lines 170-173) does
     * DEX;DEX — reduces rows_per_page by 2 from max_rows, reserving
     * the last row for the overflow indicator with one blank row gap. */
    uint16_t rows_per_page = needs_pagination ? (max_rows - 2) : max_rows;
    uint16_t items_per_page = rows_per_page * columns;

    /* Lay out items: row-major order with pagination.
     * Assembly @PAGE_LOOP: resets text_y for each new page, assigns
     * page numbers starting at 1. */
    uint16_t page = 1;
    uint16_t page_item = 0;

    for (uint16_t i = 0; i < total_items; i++) {
        uint16_t col = page_item % columns;
        uint16_t row = page_item / columns;
        w->menu_items[i].text_x = (uint8_t)(col * col_width);
        w->menu_items[i].text_y = (uint8_t)(w->text_y + row);
        w->menu_items[i].page = page;

        if (start_index) {
            w->menu_items[i].userdata = start_index + i;
        }

        page_item++;
        if (page_item >= items_per_page) {
            page++;
            page_item = 0;
        }
    }

    /* Assembly (layout_menu_options.asm lines 303-359): if items overflow,
     * add an overflow indicator at the last row with page=0 (always visible).
     * Selecting it cycles through pages. */
    if (needs_pagination && w->menu_count < 24) {
        MenuItem *oi = &w->menu_items[w->menu_count];
        strncpy(oi->label, "...", MENU_LABEL_SIZE - 1);
        oi->label[MENU_LABEL_SIZE - 1] = '\0';
        oi->userdata = 0;
        oi->type = 2;
        oi->text_x = 0;
        oi->text_y = (uint8_t)(w->text_y + max_rows - 1);
        oi->page = 0;  /* visible on all pages */
        oi->sound_effect = 0;
        oi->pixel_align = 0;
        oi->script = 0;
        w->menu_count++;
    }

    w->menu_page_number = 1;

    /* Assembly: LAYOUT_MENU_OPTIONS then PRINT_MENU_ITEMS */
    print_menu_items();
}

/* LAYOUT_AND_PRINT_MENU_AT_SELECTION — Port of asm/text/menu/layout_and_print_menu_at_selection.asm (59 lines).
 *
 * Like open_window_and_print_menu, but also sets the initial cursor
 * position to `initial_selection` (0-based index into menu items).
 * If initial_selection == (uint16_t)-1, no pre-selection is applied.
 *
 * Assembly: LAYOUT_MENU_OPTIONS(A=columns, X=0, Y=0) then walks the
 * menu option linked list to set selected_option and menu_page_number
 * to the page containing initial_selection. */
void layout_and_print_menu_at_selection(uint16_t columns, uint16_t start_index,
                                        uint16_t initial_selection) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) return;

    if (columns < 1) columns = 1;

    /* Layout items (same as open_window_and_print_menu) */
    uint16_t interior_w = w->width - 2;
    uint16_t col_width = interior_w / columns;

    for (uint16_t i = 0; i < w->menu_count; i++) {
        uint16_t col = i % columns;
        uint16_t row = i / columns;
        w->menu_items[i].text_x = (uint8_t)(col * col_width);
        w->menu_items[i].text_y = (uint8_t)(w->text_y + row);
        if (start_index)
            w->menu_items[i].userdata = start_index + i;
    }

    /* Set initial selection position (assembly lines 17-55) */
    if (initial_selection != (uint16_t)(-1) && initial_selection < w->menu_count) {
        w->selected_option = initial_selection;
    }

    print_menu_items();
}

/* menu_backup_* now in WindowSystemState win. */

/*
 * BACKUP_SELECTED_MENU_OPTION — Port of asm/misc/backup_selected_menu_option.asm.
 *
 * Saves the current focus window's cursor state so the menu can be
 * restored to the same position later. Used by the Goods menu to
 * remember which item was selected when returning from a sub-action.
 */
void backup_selected_menu_option(void) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    /* Assembly: absolute index = selected_option + current_option,
     * then look up that menu_option's text_x/text_y. */
    uint16_t abs_idx = w->selected_option + w->current_option;
    if (abs_idx < w->menu_count) {
        win.menu_backup_text_x = w->menu_items[abs_idx].text_x;
        win.menu_backup_text_y = w->menu_items[abs_idx].text_y;
    }
    win.menu_backup_current_option = w->current_option;
    win.menu_backup_selected_option = w->selected_option;
}

/*
 * DISPLAY_MENU_HEADER_TEXT — Port of asm/text/menu/display_menu_header_text.asm.
 *
 * Saves text attributes, creates WINDOW::TARGETING_PROMPT, prints
 * a targeting prompt from MISC_TARGET_TEXT, then restores attributes.
 *
 * Assembly: index into MISC_TARGET_TEXT (10 bytes per entry, EB-encoded).
 */
#define MISC_TARGET_TEXT_LENGTH 10
void display_menu_header_text(uint16_t text_index) {
    save_window_text_attributes();
    set_instant_printing();
    create_window(WINDOW_TARGETING_PROMPT);
    if (text_index < 5) {
        const uint8_t *data = ASSET_DATA(ASSET_US_DATA_MISC_TARGET_TEXT_BIN);
        print_eb_string(data + text_index * MISC_TARGET_TEXT_LENGTH, MISC_TARGET_TEXT_LENGTH);
    }
    clear_instant_printing();
    restore_window_text_attributes();
}

/*
 * CLOSE_MENU_HEADER_WINDOW — Port of asm/text/window/close_menu_header_window.asm.
 *
 * Closes the WINDOW::TARGETING_PROMPT targeting prompt window.
 */
void close_menu_header_window(void) {
    close_window(WINDOW_TARGETING_PROMPT);
}

/*
 * Render window borders and text content to the PPU.
 * Faithful port of RENDER_WINDOW_FRAME (asm/text/window/render_window_frame.asm).
 *
 * Uses BG3 tilemap at VRAM word $7C00 (TEXT_LAYER_TILEMAP).
 *
 * BG3 tilemap format (2bpp mode, 32x32 tiles):
 *   Each entry is 16 bits: vhopppcc cccccccc
 *   v=vflip, h=hflip, o=priority, p=palette(3), c=tile(10)
 *
 * Border tiles from TEXT_WINDOW_GFX:
 *   0x10 = corner, 0x11 = horizontal edge, 0x12 = vertical edge, 0x13 = junction
 *   All use palette 7 + priority.  Flips produce all 4 corners and edges.
 */

/* Full 16-bit tilemap entries for border pieces (from RENDER_WINDOW_FRAME) */
#define BORDER_TL     0x3C10  /* tile 0x10, pal 7, pri, no flip: top-left corner */
#define BORDER_TOP    0x3C11  /* tile 0x11, pal 7, pri, no flip: top edge */
#define BORDER_LEFT   0x3C12  /* tile 0x12, pal 7, pri, no flip: left edge */
#define BORDER_TR     0x7C10  /* tile 0x10, pal 7, pri, h-flip:  top-right corner */
#define BORDER_RIGHT  0x7C12  /* tile 0x12, pal 7, pri, h-flip:  right edge */
#define BORDER_BL     0xBC10  /* tile 0x10, pal 7, pri, v-flip:  bottom-left corner */
#define BORDER_BOTTOM 0xBC11  /* tile 0x11, pal 7, pri, v-flip:  bottom edge */
#define BORDER_BR     0xFC10  /* tile 0x10, pal 7, pri, hv-flip: bottom-right corner */
/* Junction (T-intersection) tiles for overlapping window corners */
#define JUNCTION_TL   0x3C13  /* tile 0x13, pal 7, pri, no flip */
#define JUNCTION_TR   0x7C13  /* tile 0x13, pal 7, pri, h-flip */
#define JUNCTION_BL   0xBC13  /* tile 0x13, pal 7, pri, v-flip */
#define JUNCTION_BR   0xFC13  /* tile 0x13, pal 7, pri, hv-flip */
/* Blank/interior tile index: tile 0x40 (64 decimal).
   Assembly FILL_WINDOW_ROW stores 0x0040 (no priority) in per-window content_tilemap,
   then RENDER_WINDOW_FRAME adds 0x2000 (priority) when writing to BG2_BUFFER.
   Since the C port writes directly to the screen tilemap here, include priority. */
#define BLANK_TILE    0x40
#define INTERIOR_FILL (0x2000 | BLANK_TILE)

void render_all_windows(void) {
    /* Assembly's RENDER_ALL_WINDOWS writes to BG2_BUFFER, then
     * UPLOAD_BATTLE_SCREEN_TO_VRAM copies BG2_BUFFER to VRAM.
     * Target win.bg2_buffer so all text layer writes go through one ert.buffer. */
    uint16_t *tilemap = (uint16_t *)win.bg2_buffer;

    /* Assembly (render_all_windows.asm lines 7-10): draw HPPP windows
     * into win.bg2_buffer first, if RENDER_HPPP_WINDOWS flag is set. */
    if (ow.render_hppp_windows & 0xFF) {
        draw_active_hppp_windows();
    }

    /* Render each active window */
    for (int wi = 0; wi < MAX_WINDOWS; wi++) {
        WindowInfo *w = &win.windows[wi];
        if (!w->active) continue;

        /* Draw window border using correct tile entries from RENDER_WINDOW_FRAME */
        for (uint16_t ty = 0; ty < w->height; ty++) {
            for (uint16_t tx = 0; tx < w->width; tx++) {
                uint16_t entry;
                bool is_top = (ty == 0);
                bool is_bot = (ty == w->height - 1);
                bool is_left = (tx == 0);
                bool is_right = (tx == w->width - 1);

                if (is_top && is_left)       entry = BORDER_TL;
                else if (is_top && is_right)  entry = BORDER_TR;
                else if (is_top)              entry = BORDER_TOP;
                else if (is_bot && is_left)   entry = BORDER_BL;
                else if (is_bot && is_right)  entry = BORDER_BR;
                else if (is_bot)              entry = BORDER_BOTTOM;
                else if (is_left)             entry = BORDER_LEFT;
                else if (is_right)            entry = BORDER_RIGHT;
                else                          entry = INTERIOR_FILL;

                uint16_t map_x = w->x + tx;
                uint16_t map_y = w->y + ty;
                if (map_x < 32 && map_y < 32) {
                    /* Assembly (render_window_frame.asm:37-55, 199-219, 276-314):
                     * At corners, check if another window already wrote a tile.
                     * If so, use junction tile (0x13) for T-intersection. */
                    if (is_top && is_left) {
                        uint16_t existing = tilemap[map_y * 32 + map_x];
                        if (existing != 0 && existing != BORDER_TL)
                            entry = JUNCTION_TL;
                    } else if (is_top && is_right) {
                        uint16_t existing = tilemap[map_y * 32 + map_x];
                        if (existing != 0 && existing != BORDER_TR)
                            entry = JUNCTION_TR;
                    } else if (is_bot && is_left) {
                        uint16_t existing = tilemap[map_y * 32 + map_x];
                        if (existing != 0 && existing != BORDER_BL)
                            entry = JUNCTION_BL;
                    } else if (is_bot && is_right) {
                        uint16_t existing = tilemap[map_y * 32 + map_x];
                        if (existing != 0 && existing != BORDER_BR)
                            entry = JUNCTION_BR;
                    }
                    tilemap[map_y * 32 + map_x] = entry;
                }
            }
        }

        /* Write pre-rendered title tile IDs to BG2_BUFFER (top border row).
           Assembly: RENDER_WINDOW_FRAME writes tile IDs for the title;
           actual glyph data was uploaded to VRAM once at set_window_title time. */
        if (w->title_slot > 0 && w->title_tile_count > 0) {
            uint16_t tile_base = 0x02E0 + (w->title_slot - 1) * 16;
            for (int t = 0; t < w->title_tile_count; t++) {
                uint16_t mx = w->content_x + t;
                if (mx < 32 && w->y < 32)
                    tilemap[w->y * 32 + mx] = (tile_base + t) | 0x2000;
            }
        }

        /* Copy per-window content_tilemap to win.bg2_buffer.
         * Port of RENDER_WINDOW_FRAME @WRITE_BODY_TILE loop.
         * Assembly adds $2000 (priority bit) via CLC; ADC #$2000 when
         * copying from per-window tilemap to BG2_BUFFER. */
        {
            uint16_t content_width = w->width - 2;
            uint16_t content_height = w->height - 2;  /* interior tile rows */

            for (uint16_t row = 0; row < content_height; row++) {
                for (uint16_t col = 0; col < content_width; col++) {
                    uint16_t src_idx = row * content_width + col;
                    if (src_idx >= w->content_tilemap_size) break;

                    uint16_t entry = w->content_tilemap[src_idx];
                    if (entry == 0) continue;  /* skip empty/transparent */

                    uint16_t screen_x = w->content_x + col;
                    uint16_t screen_y = w->content_y + row;
                    if (screen_x < 32 && screen_y < 32) {
                        /* Add priority bit (assembly: CLC; ADC #$2000) */
                        tilemap[screen_y * 32 + screen_x] = entry | 0x2000;
                    }
                }
            }
        }
    }
}

/* ---- Tile-level rendering (WRITE_TILE_TO_WINDOW chain) ---- */

/*
 * WRITE_TILE_TO_WINDOW — Port of asm/text/window/write_tile_to_window.asm (150 lines).
 *
 * Writes a single tile code (16px tall = 2 stacked 8x8 tiles) to the
 * focus window's tilemap at the current text_x, text_y position, then
 * advances text_x.  Handles line wrapping (when text_x == width, advance
 * to next row or scroll up), blinking triangle logic, and computes the
 * SNES tilemap entry from the tile code.
 *
 * Tile mapping formula (assembly lines 100-113):
 *   vram_tile = ((code & 0xFFF0) << 1) + (code & 0x000F) + attribute
 *   Top tile entry = vram_tile
 *   Bottom tile entry = vram_tile + 16
 *
 * Parameters:
 *   window_id: which window to write to
 *   tile_code: encoded tile value (e.g., border tile 0x0130 → VRAM 0x0260)
 *   tile_attr: palette/priority bits (from curr_tile_attributes)
 */
static void write_tile_to_window(uint16_t window_id, uint16_t tile_code,
                                 uint16_t tile_attr) {
    WindowInfo *w = get_window(window_id);
    if (!w) return;

    uint16_t text_x = w->text_x;
    uint16_t text_y = w->text_y;
    uint16_t width  = w->width - 2;  /* Assembly stores content width (display - 2 for borders) */
    /* Line wrapping (assembly lines 29-50):
     * If text_x == width, advance to next row or scroll. */
    if (text_x == width) {
        uint16_t max_rows = (w->height - 2) / 2 - 1;
        if (text_y == max_rows) {
            scroll_window_up(w);
        } else {
            text_y++;
        }
        text_x = 0;
    }

    /* Blinking triangle handling (assembly lines 52-71):
     * If dt.blinking_triangle_flag != 0 and text_x == 0 and tile is 32 or 64:
     *   flag==1: skip drawing, just store position
     *   flag==2: replace tile with 32 (space)
     * Assembly: CPY #0 tests Y which holds the POST-WRAP text_x value.
     * If wrapping occurred, text_x was reset to 0. */
    if (dt.blinking_triangle_flag && text_x == 0 &&
        (tile_code == 32 || tile_code == BLANK_TILE)) {
        if (dt.blinking_triangle_flag == 1) {
            w->text_x = text_x;
            w->text_y = text_y;
            w->cursor_pixel_x = text_x * 8;
            return;
        }
        if (dt.blinking_triangle_flag == 2) {
            tile_code = 32;
        }
    }

    /* Special tile 34 (CHAR::EQUIPPED): force palette 3 (0x0C00) regardless of attribute */
    uint16_t attr = (tile_code == 34) ? 0x0C00 : tile_attr;

    /* Compute VRAM tile entry (assembly lines 100-113):
     * entry = ((code & 0xFFF0) << 1) + (code & 0x000F) + attribute */
    uint16_t entry = ((tile_code & 0xFFF0) << 1) + (tile_code & 0x000F) + attr;

    /* Compute content_tilemap position:
     * Each text line = 2 tile rows. Upper at row_base + text_x,
     * lower at row_base + content_width + text_x. */
    uint16_t row_base = text_y * width * 2;
    uint16_t upper_pos = row_base + text_x;
    uint16_t lower_pos = row_base + width + text_x;

    if (upper_pos < w->content_tilemap_size && lower_pos < w->content_tilemap_size) {
        /* Free old tiles at position */
        free_tile_safe(w->content_tilemap[upper_pos]);
        free_tile_safe(w->content_tilemap[lower_pos]);

        /* Write top tile (assembly line 118) */
        w->content_tilemap[upper_pos] = entry;

        /* Write bottom tile = entry + 16 (assembly lines 126-134) */
        w->content_tilemap[lower_pos] = entry + 16;
    }

    /* Advance text_x (assembly lines 135-137) */
    text_x++;

    /* Store updated text position (assembly lines 138-147) */
    w->text_x = text_x;
    w->text_y = text_y;
    w->cursor_pixel_x = text_x * 8;
}

/*
 * SET_WINDOW_TILE_ATTRIBUTE — Port of asm/text/window/set_window_tile_attribute.asm.
 *
 * Calls write_tile_to_window with the focus window's curr_tile_attributes.
 */
static void set_window_tile_attribute(uint16_t tile_code) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;
    write_tile_to_window(win.current_focus_window, tile_code, w->curr_tile_attributes);
}

/*
 * SET_TILE_ATTRIBUTE_AND_REDRAW — Port of asm/text/set_tile_attribute_and_redraw.asm.
 *
 * Calls set_window_tile_attribute, then flags ow.redraw_all_windows if the
 * focus window is not the topmost (tail) window.
 * Since the C port redraws all windows every frame via render_all_windows,
 * we always set the flag.
 */
static void set_tile_attribute_and_redraw(uint16_t tile_code) {
    set_window_tile_attribute(tile_code);
    ow.redraw_all_windows = 1;
}

/*
 * SET_WINDOW_PALETTE_INDEX — Port of asm/text/window/set_window_palette_index.asm.
 *
 * Sets curr_tile_attributes = palette_index * 1024 on the current focus window.
 * (1024 = 0x0400 = shift palette bits 10-12 by 10 positions in SNES tilemap).
 */
void set_window_palette_index(uint16_t palette_index) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (w)
        w->curr_tile_attributes = palette_index * 1024;
}

/*
 * SET_FILE_SELECT_TEXT_HIGHLIGHT — Port of asm/text/set_file_select_text_highlight.asm.
 *
 * Modifies tilemap attribute bits for a menu item's label text to apply
 * or clear a highlight palette. Assembly walks content_tilemap entries at
 * the item's (text_x+1, text_y) position and ORs in palette bits (SET)
 * or masks them off (CLEAR).
 *
 * Used by display-only file select menus to show the previously selected
 * option with a purple/highlight background (palette 6).
 */
void highlight_menu_item(WindowInfo *w, uint16_t item_index, uint16_t palette, bool set) {
    if (!w || item_index >= w->menu_count) return;

    MenuItem *item = &w->menu_items[item_index];
    uint16_t content_width = w->width - 2;
    uint16_t attr = palette * 1024;

    /* Assembly uses text_x + 1 to skip the cursor column */
    uint16_t tx = item->text_x + 1;
    uint16_t ty = item->text_y;

    /* Walk through each character of the label.
     * Assembly (set_file_select_text_highlight.asm line 78): stops when
     * it hits a blank tile (tile == 64).  In the C port, unwritten entries
     * are 0.  Skip any entry with a zero tile index to avoid painting
     * palette bits onto empty positions (which would make them visible). */
    for (int i = 0; item->label[i] != '\0'; i++) {
        uint16_t upper_pos = ty * content_width * 2 + tx;
        uint16_t lower_pos = upper_pos + content_width;

        if (upper_pos < w->content_tilemap_size && lower_pos < w->content_tilemap_size) {
            uint16_t upper_tile = w->content_tilemap[upper_pos] & 0x03FF;
            if (upper_tile == 0 || upper_tile == BLANK_TILE)
                break;  /* past end of rendered text */
            if (set) {
                w->content_tilemap[upper_pos] = upper_tile | attr;
                w->content_tilemap[lower_pos] = (w->content_tilemap[lower_pos] & 0x03FF) | attr;
            } else {
                w->content_tilemap[upper_pos] &= 0x03FF;
                w->content_tilemap[lower_pos] &= 0x03FF;
            }
        }
        tx++;
    }
}

/*
 * PRINT_CHAR_WITH_SOUND — Port of asm/text/print_char_with_sound.asm.
 *
 * Writes a single tile-level character to the focus window with optional
 * sound effect and text speed delay.
 *
 * Steps:
 *   1. Compute tilemap position from window text_x/text_y
 *   2. Free old VWF tiles at that position (top and bottom)
 *   3. If tile == 47 (specific char), clear vwf_indent_new_line
 *   4. Call SET_WINDOW_TILE_ATTRIBUTE to write the tile
 *   5. Set ow.redraw_all_windows = 1
 *   6. Play SFX::TEXT_PRINT (id=7) unless instant mode or silent
 *   7. Wait (text_speed+1) frames if not instant
 *
 * Parameters:
 *   tile_code: encoded tile value (same format as WRITE_TILE_TO_WINDOW)
 */
void print_char_with_sound(uint16_t tile_code) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    /* Free old tiles at current position from content_tilemap (assembly lines 15-55) */
    uint16_t content_w = w->width - 2;
    uint16_t row_base = w->text_y * content_w * 2;
    uint16_t upper_pos = row_base + w->text_x;
    uint16_t lower_pos = row_base + content_w + w->text_x;

    if (upper_pos < w->content_tilemap_size)
        free_tile_safe(w->content_tilemap[upper_pos]);
    if (lower_pos < w->content_tilemap_size)
        free_tile_safe(w->content_tilemap[lower_pos]);

    /* Tile 47: clear VWF indent flag (assembly line 57-60).
     * vwf_indent_new_line is in text.c as static; for tile-level rendering
     * this is mainly relevant to VWF mixed mode. Skip for now. */

    /* Write tile via SET_WINDOW_TILE_ATTRIBUTE (assembly line 64) */
    set_window_tile_attribute(tile_code);

    /* Flag redraw (assembly lines 65-73) */
    ow.redraw_all_windows = 1;

    /* Sound effect (assembly lines 76-102):
     * Play SFX::TEXT_PRINT (7) unless:
     *   - dt.text_sound_mode == 2 or 3 (those have special sound handling)
     *   - dt.instant_printing is set
     *   - tile_code == 32 (space)
     *   - dt.blinking_triangle_flag is set (when not in mode 2/3) */
    bool play_sound = true;
    if (dt.text_sound_mode == 2) {
        play_sound = true;
    } else if (dt.text_sound_mode == 3) {
        play_sound = false;
    } else {
        play_sound = (dt.blinking_triangle_flag == 0);
    }
    if (play_sound && !dt.instant_printing && tile_code != 32) {
        play_sfx(7);  /* SFX::TEXT_PRINT */
    }

    /* Text speed delay (assembly lines 104-117):
     * Assembly: LDX SELECTED_TEXT_SPEED; INX → delay = text_speed + 1. */
    if (!dt.instant_printing) {
        int delay = (game_state.text_speed & 0xFF) + 1;
        for (int i = 0; i < delay; i++) {
            window_tick();
        }
    }
}

/* ---- Window Border Animation ---- */

/* WINDOW_BORDER_ANIM_TILES data: loaded from "data/window_border_anim_tiles.bin".
 * ROW1 (offset 0, 20 bytes): 9 tiles + null terminator (0x130-0x138, 0x0000)
 * ROW2 (offset 20, 18 bytes): 9 tiles, no terminator (0x140-0x148) */
#define BORDER_ANIM_ROW1_OFFSET  0
#define BORDER_ANIM_ROW2_OFFSET  20  /* bytes */
#define BORDER_ANIM_ROW2_PART1   4   /* first 4 tiles in HPPP variant */
#define BORDER_ANIM_ROW2_PART2   5   /* remaining 5 tiles */
#define HPPP_UPDATE_COUNT        8   /* frames of HPPP meter updates between halves */

static const uint16_t *border_anim_tiles_row1;
static const uint16_t *border_anim_tiles_row2;

static void load_border_anim_tiles(void) {
    static const uint8_t *raw;
    if (!raw) {
        raw = ASSET_DATA(ASSET_DATA_WINDOW_BORDER_ANIM_TILES_BIN);
        if (raw) {
            border_anim_tiles_row1 = (const uint16_t *)(raw + BORDER_ANIM_ROW1_OFFSET);
            border_anim_tiles_row2 = (const uint16_t *)(raw + BORDER_ANIM_ROW2_OFFSET);
        }
    }
}

/*
 * ANIMATE_WINDOW_BORDER — Port of asm/text/window/animate_window_border.asm.
 *
 * Sets palette to 3 (highlight), steps through ROW1 border tiles one per frame,
 * writing each via SET_TILE_ATTRIBUTE_AND_REDRAW + WINDOW_TICK.
 * Restores palette to 0 when done.
 */
static void animate_window_border(void) {
    load_border_anim_tiles();
    if (!border_anim_tiles_row1) return;

    set_window_palette_index(3);

    const uint16_t *p = border_anim_tiles_row1;
    while (*p != 0) {
        set_tile_attribute_and_redraw(*p);
        p++;
        /* Assembly: WINDOW_TICK called once (loop count = 1) */
        window_tick();
    }

    set_window_palette_index(0);
}

/*
 * ANIMATE_WINDOW_BORDER_WITH_HPPP — Port of asm/text/hp_pp_window/animate_window_border_with_hppp.asm.
 *
 * Like animate_window_border but uses ROW2 tiles and inserts 8 frames
 * of UPDATE_HPPP_METER_AND_RENDER between the first 4 and last 5 tiles.
 * This allows the HP/PP rolling counter to update during the animation.
 */
static void animate_window_border_with_hppp(void) {
    load_border_anim_tiles();
    if (!border_anim_tiles_row2) return;

    set_window_palette_index(3);

    const uint16_t *p = border_anim_tiles_row2;

    /* First 4 tiles (assembly lines 13-36) */
    for (int i = 0; i < BORDER_ANIM_ROW2_PART1; i++) {
        set_tile_attribute_and_redraw(*p++);
        window_tick();
    }

    /* 8 frames of HPPP meter updates (assembly lines 37-48) */
    for (int i = 0; i < HPPP_UPDATE_COUNT; i++) {
        update_hppp_meter_and_render();
    }

    /* Remaining 5 tiles (assembly lines 49-74) */
    for (int i = 0; i < BORDER_ANIM_ROW2_PART2; i++) {
        set_tile_attribute_and_redraw(*p++);
        window_tick();
    }

    set_window_palette_index(0);
}

/*
 * DISPATCH_WINDOW_BORDER_ANIMATION — Port of asm/text/window/dispatch_window_border_animation.asm.
 *
 * Dispatches border animation based on argument:
 *   1 → animate_window_border (simple border animation)
 *   2 → animate_window_border_with_hppp (border + HP/PP meter update)
 */
void dispatch_window_border_animation(uint16_t mode) {
    switch (mode) {
    case 1: animate_window_border(); break;
    case 2: animate_window_border_with_hppp(); break;
    default: break;
    }
}

/*
 * Cursor arrow tilemap entries from asm/data/unknown/C3E3F8.asm.
 * Two animation frames (small triangle / big triangle), each 16px tall
 * (upper 8x8 + lower 8x8 tile).  Tiles come from TEXT_WINDOW_GFX asset.
 *
 * Format: vhopppcc cccccccc  (palette 1, priority set)
 */
#define CURSOR_FRAME0_UPPER 0x2441  /* tile 0x41, pal 1, pri */
#define CURSOR_FRAME0_LOWER 0x2451  /* tile 0x51, pal 1, pri */
#define CURSOR_FRAME1_UPPER 0x268D  /* tile 0x28D, pal 1, pri */
#define CURSOR_FRAME1_LOWER 0x269D  /* tile 0x29D, pal 1, pri */

/* Assembly toggles cursor frame every ~10 vblank waits */
#define CURSOR_TOGGLE_FRAMES 10

/*
 * has_menu_item_at — Check if a selectable menu item exists at (x, y)
 * on the current page.  Replaces GET_MENU_TILE_AT_POSITION tilemap scan.
 */
static bool has_menu_item_at(WindowInfo *w, int16_t x, int16_t y) {
    for (uint16_t i = 0; i < w->menu_count; i++) {
        if (w->menu_items[i].text_x == x && w->menu_items[i].text_y == y) {
            uint16_t page = w->menu_items[i].page;
            if (page == w->menu_page_number || page == 0)
                return true;
        }
    }
    return false;
}

/*
 * find_next_menu_option — Port of FIND_NEXT_MENU_OPTION
 * (asm/battle/find_next_menu_option.asm).
 *
 * Two search modes based on delta_y:
 *   Vertical   (delta_y != 0): scan column by delta_y, spiral left then right
 *   Horizontal (delta_y == 0): scan row by direction, spiral up then down
 *
 * Returns packed (y << 8) | x, or -1 if not found.
 * Bounds use unsigned comparison (BCC in assembly) so negative coords
 * wrap past the limit and terminate the loop.
 */
static int16_t find_next_menu_option(WindowInfo *w, int16_t start_x, int16_t start_y,
                                     int16_t delta_y, int16_t direction) {
    int16_t content_w = (int16_t)(w->width - 2);
    int16_t max_rows  = (int16_t)((w->height - 2) / 2);

    if (delta_y == 0) {
        /* --- Horizontal search (LEFT / RIGHT) --- */
        /* Phase 1: same row */
        int16_t cx = start_x + direction;
        while ((uint16_t)cx < (uint16_t)content_w) {
            if (has_menu_item_at(w, cx, start_y))
                return (start_y << 8) | (cx & 0xFF);
            cx += direction;
        }
        /* Phase 2: spiral upward from each column */
        cx = start_x + direction;
        while ((uint16_t)cx < (uint16_t)content_w) {
            int16_t cy = start_y - 1;
            while ((uint16_t)cy < (uint16_t)max_rows) {
                if (has_menu_item_at(w, cx, cy))
                    return (cy << 8) | (cx & 0xFF);
                cy--;
            }
            cx += direction;
        }
        /* Phase 3: spiral downward from each column */
        cx = start_x + direction;
        while ((uint16_t)cx < (uint16_t)content_w) {
            int16_t cy = start_y + 1;
            while ((uint16_t)cy < (uint16_t)max_rows) {
                if (has_menu_item_at(w, cx, cy))
                    return (cy << 8) | (cx & 0xFF);
                cy++;
            }
            cx += direction;
        }
    } else {
        /* --- Vertical search (UP / DOWN) --- */
        /* Phase 1: same column */
        int16_t cy = start_y + delta_y;
        while ((uint16_t)cy < (uint16_t)max_rows) {
            if (has_menu_item_at(w, start_x, cy))
                return (cy << 8) | (start_x & 0xFF);
            cy += delta_y;
        }
        /* Phase 2: spiral leftward from each row */
        cy = start_y + delta_y;
        while ((uint16_t)cy < (uint16_t)max_rows) {
            int16_t cx = start_x - 1;
            while ((uint16_t)cx < (uint16_t)content_w) {
                if (has_menu_item_at(w, cx, cy))
                    return (cy << 8) | (cx & 0xFF);
                cx--;
            }
            cy += delta_y;
        }
        /* Phase 3: spiral rightward from each row */
        cy = start_y + delta_y;
        while ((uint16_t)cy < (uint16_t)max_rows) {
            int16_t cx = start_x + 1;
            while ((uint16_t)cx < (uint16_t)content_w) {
                if (has_menu_item_at(w, cx, cy))
                    return (cy << 8) | (cx & 0xFF);
                cx++;
            }
            cy += delta_y;
        }
    }
    return -1;
}

/*
 * move_cursor — Port of MOVE_CURSOR (asm/text/move_cursor.asm).
 * Tries find_next_menu_option from current position; on failure retries
 * from wrap coordinates and validates the result stays on the same
 * row (horizontal) or column (vertical).
 */
static int16_t move_cursor(WindowInfo *w,
                           int16_t start_x, int16_t start_y,
                           int16_t delta_y, int16_t direction,
                           int16_t wrap_x, int16_t wrap_y,
                           uint16_t sfx) {
    int16_t result = find_next_menu_option(w, start_x, start_y, delta_y, direction);
    if (result != -1) {
        play_sfx(sfx);
        return result;
    }
    /* Retry from wrap position */
    result = find_next_menu_option(w, wrap_x, wrap_y, delta_y, direction);
    if (result == -1)
        return -1;
    /* Validate: horizontal moves must stay on same row, vertical on same column */
    if (delta_y == 0) {
        int16_t result_y = (result >> 8) & 0xFF;
        if (result_y != start_y) return -1;
    } else {
        int16_t result_x = result & 0xFF;
        if (result_x != start_x) return -1;
    }
    play_sfx(sfx);
    return result;
}

/*
 * resolve_cursor_move — Map a packed (y << 8 | x) position back to a
 * menu_items[] index on the current page.  Port of @PROCESS_CURSOR_MOVE
 * in selection_menu.asm (lines 573-634).
 */
static int16_t resolve_cursor_move(WindowInfo *w, int16_t packed) {
    if (packed == -1) return -1;
    int16_t target_x = packed & 0xFF;
    int16_t target_y = (packed >> 8) & 0xFF;
    for (uint16_t i = 0; i < w->menu_count; i++) {
        if (w->menu_items[i].text_x == target_x &&
            w->menu_items[i].text_y == target_y) {
            uint16_t page = w->menu_items[i].page;
            if (page == w->menu_page_number || page == 0)
                return (int16_t)i;
        }
    }
    return -1;
}

/*
 * Run a selection menu in the focus window.
 * Draws cursor, handles input, returns selected userdata.
 */
uint16_t selection_menu(uint16_t allow_cancel) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) return 0;

    /* Restore persisted selection if valid, otherwise start at 0.
     * Assembly (selection_menu.asm lines 38-46, USA only): if RESTORE_MENU_BACKUP
     * is set, override current_option/selected_option from backup variables,
     * then fall through to the normal initialization below.
     * Assembly (lines 49-56): checks selected_option != -1, uses it as
     * current_option. Otherwise starts at 0. */
    if (win.restore_menu_backup) {
        /* Restore cursor indices saved by backup_selected_menu_option().
         * Mirrors selection_menu.asm lines 41-46. */
        if (win.menu_backup_current_option < w->menu_count)
            w->current_option = win.menu_backup_current_option;
        if (win.menu_backup_selected_option < w->menu_count)
            w->selected_option = win.menu_backup_selected_option;
        /* Restore text_x/text_y for the current item (lines 162-170).
         * SETUP_MENU_OPTION_VWF clears restore_menu_backup (line 44 in
         * setup_menu_option_vwf.asm) — mirror that by clearing it here. */
        uint16_t cur = w->current_option;
        if (cur < w->menu_count) {
            w->menu_items[cur].text_x = (uint8_t)win.menu_backup_text_x;
            w->menu_items[cur].text_y = (uint8_t)win.menu_backup_text_y;
        }
        win.restore_menu_backup = 0;
    }
    if (w->selected_option < w->menu_count) {
        /* Assembly (lines 75-92): clear highlight from previously selected
         * option before starting new selection loop. X=0 means CLEAR. */
        set_instant_printing();
        if (w->id == WINDOW_FILE_SELECT_MAIN
            || w->id == WINDOW_FILE_SELECT_MENU
            || w->id == WINDOW_FILE_SELECT_TEXT_SPEED
            || w->id == WINDOW_FILE_SELECT_MUSIC_MODE
            || w->id == WINDOW_FILE_SELECT_CONFIRM_MSG)
            highlight_menu_item(w, w->selected_option, 0, false);
        w->current_option = w->selected_option;
    } else {
        w->current_option = 0;
    }

    /* Assembly @SETUP_CURRENT_OPTION (lines 102-122): on initial display,
     * if the current option has a non-NULL script, run it as hover text. */
    if (w->current_option < w->menu_count &&
        w->menu_items[w->current_option].script != 0) {
        set_instant_printing();
        display_text_from_addr(w->menu_items[w->current_option].script);
    }

    /* Assembly lines 123-158: invoke cursor_move_callback for initial option */
    if (w->cursor_move_callback && w->current_option < w->menu_count) {
        MenuItem *ci = &w->menu_items[w->current_option];
        uint16_t val = (ci->type == 1) ? (w->current_option + 1) : ci->userdata;
        w->cursor_move_callback(val);
        set_window_focus(w->id);
    }

    /* Assembly line 186: JSL WINDOW_TICK — render all windows, upload
     * win.bg2_buffer to VRAM, run one full frame (OAM_CLEAR + actionscript +
     * UPDATE_SCREEN + WAIT). This is the ONLY place windows are rendered
     * in the selection loop; the per-frame loop just does HP/PP + frame tick. */
    clear_instant_printing();
    window_tick();

    /* Assembly line 188: cursor_frame starts at 1, then immediately
     * toggles to 0 at @CURSOR_BLINK_LOOP. */
    uint16_t cursor_frame = 1;
    bool redraw_cursor = true;
    uint16_t frame_counter = 0;

    while (!platform_input_quit_requested()) {
        /* --- Cursor blink (assembly @CURSOR_BLINK_LOOP, lines 189-250) ---
         * Every CURSOR_TOGGLE_FRAMES, toggle cursor_frame and write cursor
         * tiles directly to VRAM (matching assembly's PREPARE_VRAM_COPY). */
        if (redraw_cursor) {
            cursor_frame ^= 1;
            redraw_cursor = false;
            frame_counter = 0;

            /* Write cursor tiles directly to ppu.vram at the correct tilemap
             * position, matching assembly's PREPARE_VRAM_COPY (lines 220-249).
             *
             * VRAM word address = TEXT_LAYER_TILEMAP + TILEMAP_COORDS(0,1)
             *   + (text_y*2 + window_y) * 32 + (window_x + text_x)
             *
             * Note: cursor is placed at window_x + text_x (ON the border
             * column when text_x==0), not content_x. The +1 row from
             * TILEMAP_COORDS(0,1) accounts for the top border row. */
            if (w->current_option < w->menu_count) {
                MenuItem *item = &w->menu_items[w->current_option];
                /* Assembly: SETUP_MENU_OPTION_VWF sets window_stats::text_x
                 * to menu_option::text_x, then SET_TILE_ATTRIBUTE_AND_REDRAW(33)
                 * calls WRITE_TILE_TO_WINDOW which writes a marker tile and
                 * advances text_x by 1.  The cursor blink reads the advanced
                 * text_x from window_stats, so the cursor sits one column
                 * right of the marker dot (i.e., at text_x + 1). */
                uint16_t vram_word = VRAM_TEXT_LAYER_TILEMAP + 32
                    + (item->text_y * 2 + w->y) * 32
                    + w->x + item->text_x + 1;
                uint32_t vram_byte = (uint32_t)vram_word * 2;

                uint16_t upper = cursor_frame ? CURSOR_FRAME1_UPPER : CURSOR_FRAME0_UPPER;
                uint16_t lower = cursor_frame ? CURSOR_FRAME1_LOWER : CURSOR_FRAME0_LOWER;

                /* Upper cursor tile */
                ppu.vram[vram_byte]     = (uint8_t)(upper & 0xFF);
                ppu.vram[vram_byte + 1] = (uint8_t)(upper >> 8);
                /* Lower cursor tile (next row = +32 words = +64 bytes) */
                ppu.vram[vram_byte + 64] = (uint8_t)(lower & 0xFF);
                ppu.vram[vram_byte + 65] = (uint8_t)(lower >> 8);
            }
        }

        /* --- Per-frame tick (assembly @INPUT_TICK, line 255) ---
         * Assembly: JSL UPDATE_HPPP_METER_AND_RENDER
         *   = HP_PP_ROLLER + optionally COPY_HPPP_WINDOW_TO_VRAM
         *     + UPDATE_HPPP_METER_TILES + RENDER_FRAME_TICK
         *   (RENDER_FRAME_TICK = OAM_CLEAR + RUN_ACTIONSCRIPT_FRAME
         *     + UPDATE_SCREEN + WAIT_UNTIL_NEXT_FRAME) */
        update_hppp_meter_and_render();

        /* --- Directional input (pressed and held) ---
           Assembly checks PAD_PRESS first (with MOVE_CURSOR for wrap-around),
           then PAD_HELD (with FIND_NEXT_MENU_OPTION directly, no wrap). */
        uint16_t pressed = platform_input_get_pad_new();
        int16_t move_result = -1;
        int16_t text_x = (w->current_option < w->menu_count)
                         ? w->menu_items[w->current_option].text_x : 0;
        int16_t text_y = (w->current_option < w->menu_count)
                         ? w->menu_items[w->current_option].text_y : 0;
        int16_t content_w = (int16_t)(w->width - 2);
        int16_t max_rows  = (int16_t)((w->height - 2) / 2);

        /* Pressed buttons: MOVE_CURSOR (with wrap-around) */
        if (pressed & PAD_UP) {
            move_result = move_cursor(w, text_x, text_y,
                                      /*delta_y=*/-1, /*direction=*/0,
                                      /*wrap_x=*/text_x, /*wrap_y=*/max_rows,
                                      /*sfx=*/3);  /* SFX::CURSOR3 */
        } else if (pressed & PAD_LEFT) {
            move_result = move_cursor(w, text_x, text_y,
                                      /*delta_y=*/0, /*direction=*/-1,
                                      /*wrap_x=*/content_w, /*wrap_y=*/text_y,
                                      /*sfx=*/2);  /* SFX::CURSOR2 */
        } else if (pressed & PAD_DOWN) {
            move_result = move_cursor(w, text_x, text_y,
                                      /*delta_y=*/1, /*direction=*/0,
                                      /*wrap_x=*/text_x, /*wrap_y=*/-1,
                                      /*sfx=*/3);  /* SFX::CURSOR3 */
        } else if (pressed & PAD_RIGHT) {
            move_result = move_cursor(w, text_x, text_y,
                                      /*delta_y=*/0, /*direction=*/1,
                                      /*wrap_x=*/-1, /*wrap_y=*/text_y,
                                      /*sfx=*/2);  /* SFX::CURSOR2 */
        }

        /* Held buttons: FIND_NEXT_MENU_OPTION directly (no wrap) */
        if (move_result == -1 && !pressed) {
            uint16_t held = core.pad1_autorepeat;
            uint16_t held_sfx = 0;
            if (held & PAD_UP) {
                move_result = find_next_menu_option(w, text_x, text_y, -1, 0);
                held_sfx = 3;  /* SFX::CURSOR3 */
            } else if (held & PAD_LEFT) {
                move_result = find_next_menu_option(w, text_x, text_y, 0, -1);
                held_sfx = 2;  /* SFX::CURSOR2 */
            } else if (held & PAD_DOWN) {
                move_result = find_next_menu_option(w, text_x, text_y, 1, 0);
                held_sfx = 3;  /* SFX::CURSOR3 */
            } else if (held & PAD_RIGHT) {
                move_result = find_next_menu_option(w, text_x, text_y, 0, 1);
                held_sfx = 2;  /* SFX::CURSOR2 */
            }
            if (move_result != -1)
                play_sfx(held_sfx);
        }

        /* Resolve move result to menu_items[] index.
         * Assembly (@PROCESS_CURSOR_MOVE): on cursor move, traverses
         * linked list to find new option, calls cursor_move_callback,
         * then jumps to @SETUP_CURRENT_OPTION → WINDOW_TICK →
         * @CURSOR_BLINK_LOOP to re-render and redraw cursor. */
        if (move_result != -1) {
            int16_t new_option = resolve_cursor_move(w, move_result);
            if (new_option >= 0) {
                w->current_option = (uint16_t)new_option;
                if (w->cursor_move_callback) {
                    MenuItem *ci = &w->menu_items[w->current_option];
                    uint16_t val = (ci->type == 1) ? (w->current_option + 1) : ci->userdata;
                    w->cursor_move_callback(val);
                    /* Assembly line 157-158: restore focus to this window
                     * after callback might have changed it. */
                    set_window_focus(w->id);
                }
                /* Assembly @SETUP_CURRENT_OPTION (lines 102-122):
                 * If menu_option::script is non-NULL, set instant printing
                 * and call DISPLAY_TEXT on the script (hover preview text).
                 * Then (line 160): CLEAR_INSTANT_PRINTING, WINDOW_TICK. */
                if (w->menu_items[w->current_option].script != 0) {
                    set_instant_printing();
                    display_text_from_addr(w->menu_items[w->current_option].script);
                }
                clear_instant_printing();
                window_tick();
                redraw_cursor = true;
                continue;
            }
        }

        if (pressed & PAD_CONFIRM) {
            MenuItem *sel = &w->menu_items[w->current_option];

            /* Assembly (selection_menu.asm lines 430-537): if the selected
             * item has page==0, it's the overflow indicator — flip to the
             * next page instead of returning a selection.
             * In assembly, LAYOUT_MENU_OPTIONS always sets page>=1 for real
             * items before SELECTION_MENU runs, so page==0 unambiguously
             * identifies the overflow indicator.  Detect active pagination
             * by checking whether any item has page>1. */
            bool is_overflow = false;
            if (sel->page == 0) {
                for (uint16_t i = 0; i < w->menu_count; i++) {
                    if (w->menu_items[i].page > 1) {
                        is_overflow = true;
                        break;
                    }
                }
            }

            if (is_overflow) {
                play_sfx(2);  /* SFX::CURSOR2 */

                /* Find the last real item's page number to know when to wrap.
                 * Assembly checks the item before the overflow indicator. */
                uint16_t last_page = 1;
                for (uint16_t i = 0; i < w->menu_count; i++) {
                    if (w->menu_items[i].page > last_page)
                        last_page = w->menu_items[i].page;
                }

                /* Cycle: if on last page, wrap to 1; else increment */
                if (w->menu_page_number >= last_page)
                    w->menu_page_number = 1;
                else
                    w->menu_page_number++;

                /* Re-render: clear window content, redraw items for new page */
                clear_instant_printing();
                clear_window_tilemap(w->id);
                window_tick();
                print_menu_items();
                set_instant_printing();

                /* Reset cursor to first item on the new page */
                for (uint16_t i = 0; i < w->menu_count; i++) {
                    if (w->menu_items[i].page == w->menu_page_number) {
                        w->current_option = i;
                        break;
                    }
                }
                clear_instant_printing();
                window_tick();
                redraw_cursor = true;
                continue;
            }

            /* Normal confirm: play item's sound_effect (line 433-435) */
            if (sel->sound_effect)
                play_sfx(sel->sound_effect);
            /* Assembly lines 428-485: highlight confirmed option with
             * palette 6.  SET_FILE_SELECT_TEXT_HIGHLIGHT (lines 24-36)
             * only operates on 4 specific file-select windows and is a
             * no-op for all others.  Restrict highlight to those windows. */
            set_instant_printing();
            if (w->id == WINDOW_FILE_SELECT_MAIN
                || w->id == WINDOW_FILE_SELECT_MENU
                || w->id == WINDOW_FILE_SELECT_TEXT_SPEED
                || w->id == WINDOW_FILE_SELECT_MUSIC_MODE
                || w->id == WINDOW_FILE_SELECT_CONFIRM_MSG)
                highlight_menu_item(w, w->current_option, 6, true);
            w->selected_option = w->current_option;
            clear_instant_printing();
            return sel->userdata;
        }
        /* Assembly checks B_BUTTON | SELECT_BUTTON for cancel (line 545-546) */
        if (allow_cancel && (pressed & PAD_CANCEL)) {
            play_sfx(2);  /* SFX::CURSOR2 */
            return 0;
        }

        /* Assembly @FRAME_WAIT_CHECK (line 569-572): increment frame counter,
         * if < 10 continue inner loop, otherwise toggle cursor (outer loop). */
        frame_counter++;
        if (frame_counter >= CURSOR_TOGGLE_FRAMES) {
            redraw_cursor = true;
        }
    }
    return 0;
}

/* ---- BG2 Buffer (Text Layer Tilemap Shadow) ---- */

/* win.bg2_buffer, win.hppp_window_buffer, win.hppp_window_digit_buffer,
 * win.hppp_meter_area_needs_update, win.upload_hppp_meter_tiles
 * now live in WindowSystemState win (see window.h). */

/*
 * CLEAR_HPPP_WINDOW_HEADER — Port of asm/battle/clear_hppp_window_header.asm.
 * Clears 4 × 16-bit tilemap entries at the HPPP window header position in BG2_BUFFER.
 * Offset formula: ((ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) - 6) * 2
 *   = ((18 * 32) - 6) * 2 = (576 - 6) * 2 = 570 * 2 = 0x474
 */
void clear_hppp_window_header(void) {
    uint16_t offset = ((ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) - 6) * 2;
    memset(&win.bg2_buffer[offset], 0, 4 * 2);
}

/*
 * RENDER_HPPP_WINDOW_HEADER — Port of asm/battle/render_hppp_window_header.asm.
 * Copies 4 header tile entries from HPPP_WINDOW_HEADER_TILES into BG2_BUFFER.
 * Data from asm/data/unknown/C3E40E.asm: $3A69, $3A6A, $3A6B, $3A6C
 */
void render_hppp_window_header(void) {
    static const uint16_t header_tiles[4] = { 0x3A69, 0x3A6A, 0x3A6B, 0x3A6C };
    uint16_t offset = ((ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) - 6) * 2;
    memcpy(&win.bg2_buffer[offset], header_tiles, sizeof(header_tiles));
}

/*
 * COPY_HPPP_WINDOW_TO_VRAM — Port of asm/text/hp_pp_window/copy_hppp_window_to_vram.asm.
 * Transfers the HPPP window rows from BG2_BUFFER to VRAM.
 * Assembly: COPY_TO_VRAM2 BG2_BUFFER + (Y_OFFSET * 32) * 2,
 *           VRAM::TEXT_LAYER_TILEMAP + TILEMAP_COORDS(0, Y_OFFSET), $240, 0
 * Source offset in BG2_BUFFER: (18 * 32) * 2 = 0x480
 * VRAM destination: (0x7C00 + 18 * 32) words = 0x7E40 words = 0xFC80 bytes
 * Transfer size: 0x240 bytes (18 rows × 32 entries? Actually 0x240 = 576 bytes = 288 words = 9 rows)
 */
void copy_hppp_window_to_vram(void) {
    uint16_t bg2_offset = (ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) * 2;
    uint32_t vram_byte_addr = (VRAM_TEXT_LAYER_TILEMAP + ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) * 2;
    memcpy(&ppu.vram[vram_byte_addr], &win.bg2_buffer[bg2_offset], 0x240);
}

/*
 * UPLOAD_BATTLE_SCREEN_TO_VRAM — Port of asm/battle/upload_battle_screen_to_vram.asm.
 *
 * Assembly:
 *   COPY_TO_VRAM2 BG2_BUFFER, $7C00, $700, $00
 *   COPY_TO_VRAM1 BLANK_TILE_DATA, $7F80, $40, $00
 *
 * Copies the first 0x700 bytes (28 rows) of BG2_BUFFER to VRAM at
 * TEXT_LAYER_TILEMAP, then writes 0x40 bytes of zeros at $7F80.
 */
void upload_battle_screen_to_vram(void) {
    /* Copy 0x700 bytes of BG2_BUFFER to VRAM at word address $7C00 */
    uint32_t vram_dest = VRAM_TEXT_LAYER_TILEMAP * 2;  /* $7C00 * 2 = $F800 */
    memcpy(&ppu.vram[vram_dest], win.bg2_buffer, 0x700);

    /* Clear 0x40 bytes at VRAM word address $7F80 (blank tile) */
    uint32_t blank_dest = 0x7F80 * 2;  /* $FF00 */
    memset(&ppu.vram[blank_dest], 0, 0x40);
}

/*
 * Compute the BG2_BUFFER byte offset for a character indicator column.
 *
 * Used by both SELECT_BATTLE_MENU_CHARACTER and CLEAR_BATTLE_MENU_CHARACTER_INDICATOR.
 * The character columns are centered within the tilemap: each character occupies
 * HPPP_WINDOW_WIDTH (7) tilemap entries, and the group is centered at HPPP_CENTER_TILE.
 *
 * Assembly pattern (both functions):
 *   char_id * 7 → char_offset
 *   (party_count * 7) / 2 → center_half
 *   x = HPPP_CENTER_TILE - center_half + char_offset
 *   byte_offset = tilemap_row_offset + x * 2
 */
static uint16_t calc_character_indicator_x_offset(int16_t char_id) {
    uint16_t char_offset = (uint16_t)char_id * HPPP_WINDOW_WIDTH;
    uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
    uint16_t total_width = party_count * HPPP_WINDOW_WIDTH;
    uint16_t center_half = total_width / 2;
    uint16_t x = HPPP_CENTER_TILE - center_half + char_offset;
    return x * 2;  /* byte offset within the row */
}

/*
 * SELECT_BATTLE_MENU_CHARACTER — Port of asm/battle/select_battle_menu_character.asm.
 *
 * Clears the previous character indicator (if any), then writes 7 zero
 * tilemap entries on the row below the HPPP window (row Y_OFFSET + HEIGHT)
 * at the new character's column.  Sets REDRAW_ALL_WINDOWS = 1.
 *
 * Assembly (US retail):
 *   If BATTLE_MENU_CURRENT_CHARACTER_ID != -1, JSR CLEAR_BATTLE_MENU_CHARACTER_INDICATOR
 *   Store param → BATTLE_MENU_CURRENT_CHARACTER_ID
 *   JSL WAIT_UNTIL_NEXT_FRAME (US only)
 *   Compute x offset, write 7 zeros at ((Y_OFFSET + HEIGHT) * 32) * 2
 *   STA #1 → REDRAW_ALL_WINDOWS
 */
void select_battle_menu_character(uint16_t party_slot) {
    /* Clear previous indicator if one is active */
    if (win.battle_menu_current_character_id != -1) {
        clear_battle_menu_character_indicator();
    }

    win.battle_menu_current_character_id = (int16_t)party_slot;

    /* US retail: wait one frame before drawing */
    render_frame_tick();

    uint16_t row_offset = ((ACTIVE_HPPP_WINDOW_Y_OFFSET + HPPP_WINDOW_HEIGHT) * 32) * 2;
    uint16_t x_offset = calc_character_indicator_x_offset(win.battle_menu_current_character_id);
    uint16_t offset = row_offset + x_offset;

    /* Clear 7 tilemap entries (each 2 bytes) */
    memset(&win.bg2_buffer[offset], 0, HPPP_WINDOW_WIDTH * 2);
    ow.redraw_all_windows = 1;
}

/*
 * CLEAR_BATTLE_MENU_CHARACTER_INDICATOR — Port of asm/text/clear_battle_menu_character_indicator.asm.
 *
 * If a character indicator is active, waits one frame, then clears 7
 * tilemap entries at the top of the HPPP window (row Y_OFFSET) at the
 * current character's column.  Resets win.battle_menu_current_character_id to -1.
 *
 * Assembly:
 *   If BATTLE_MENU_CURRENT_CHARACTER_ID == $FFFF, return
 *   JSL WAIT_UNTIL_NEXT_FRAME
 *   Compute x offset, write 7 zeros at (Y_OFFSET * 32) * 2
 *   STA #$FFFF → BATTLE_MENU_CURRENT_CHARACTER_ID
 *   STA #1 → REDRAW_ALL_WINDOWS
 */
void clear_battle_menu_character_indicator(void) {
    if (win.battle_menu_current_character_id == -1) {
        return;
    }

    render_frame_tick();

    uint16_t row_offset = (ACTIVE_HPPP_WINDOW_Y_OFFSET * 32) * 2;
    uint16_t x_offset = calc_character_indicator_x_offset(win.battle_menu_current_character_id);
    uint16_t offset = row_offset + x_offset;

    /* Clear 7 tilemap entries (each 2 bytes) */
    memset(&win.bg2_buffer[offset], 0, HPPP_WINDOW_WIDTH * 2);

    win.battle_menu_current_character_id = -1;
    ow.redraw_all_windows = 1;
}

/* ---- HPPP Window Drawing ---- */

/*
 * UNDRAW_HP_PP_WINDOW — Port of asm/text/hp_pp_window/undraw.asm.
 *
 * Clears the HP/PP window tilemap entries from win.bg2_buffer for one party slot.
 *   1. Sets win.hppp_meter_area_needs_update = 1
 *   2. Clears the bit in ow.currently_drawn_hppp_windows
 *   3. Computes the window position (centered, active vs normal row)
 *   4. Clears HPPP_WINDOW_HEIGHT (8) rows × HPPP_WINDOW_WIDTH (7) columns
 */
void undraw_hp_pp_window(uint16_t char_id) {
    win.hppp_meter_area_needs_update = 1;

    /* Clear the drawn bit for this character */
    ow.currently_drawn_hppp_windows &= ~(1 << char_id);

    /* Determine Y row offset: active character is one row higher */
    uint16_t y_offset;
    if (char_id == (uint16_t)win.battle_menu_current_character_id) {
        y_offset = ACTIVE_HPPP_WINDOW_Y_OFFSET;   /* 18 */
    } else {
        y_offset = NORMAL_HPPP_WINDOW_Y_OFFSET;   /* 19 */
    }

    /* Compute position in win.bg2_buffer using same centering as assembly */
    uint16_t x_byte_offset = calc_character_indicator_x_offset(char_id);
    uint16_t buf_offset = y_offset * 64 + x_byte_offset;

    /* Clear HPPP_WINDOW_HEIGHT rows × HPPP_WINDOW_WIDTH tiles (2 bytes each) */
    for (int row = 0; row < HPPP_WINDOW_HEIGHT; row++) {
        memset(&win.bg2_buffer[buf_offset], 0, HPPP_WINDOW_WIDTH * 2);
        buf_offset += 64;  /* tilemap row stride = 32 tiles × 2 bytes */
    }
}

/*
 * Compute the VRAM tile offset for a digit value (0-9).
 *
 * Digit tiles are arranged in groups of 4 across 16-tile rows, so the full
 * formula is d*4 + (d/4)*16: d*4 for the within-group offset, plus (d/4)*16
 * for the row offset (one 16-tile row per 4 digits).
 */
static inline uint16_t digit_tile_offset(uint8_t d) {
    return (uint16_t)(d * 4 + (d >> 2) * 16);
}

/*
 * SEPARATE_DECIMAL_DIGITS — Port of asm/text/hp_pp_window/separate_decimal_digits.asm.
 *
 * Decomposes a value into its decimal digits and stores them in
 * win.hppp_window_digit_buffer: [0]=hundreds, [1]=tens, [2]=ones.
 *
 * Assembly uses MODULUS16 and DIVISION16S_DIVISOR_POSITIVE.
 */
void separate_decimal_digits(uint16_t value) {
    win.hppp_window_digit_buffer[2] = value % 10;         /* ones */
    uint16_t q = value / 10;
    win.hppp_window_digit_buffer[1] = q % 10;             /* tens */
    win.hppp_window_digit_buffer[0] = (uint8_t)(q / 10);  /* hundreds */
}

/*
 * FILL_HP_PP_TILE_BUFFER — Port of asm/text/hp_pp_window/fill_tile_buffer.asm (181 lines).
 *
 * Computes tilemap entries (tile index + palette/priority attributes) for a
 * 3-digit HP or PP display.  Reads from win.hppp_window_digit_buffer and writes
 * to win.hppp_window_buffer[id].
 *
 * Parameters:
 *   id       — party slot (0-3)
 *   mode     — 0 for HP, 1 for PP
 *   fraction — roller animation fraction (16-bit); values >= 0x3000 produce
 *              a carry that shifts digit tiles to animate rolling
 *
 * Tilemap entry format (16-bit SNES BG):
 *   Ones base  = 0x2600 (tile 0x200, palette 1, priority 1)
 *   Tens/Hund  = 0x2400 (tile 0x000, palette 1, priority 1) + 0x0200 (visible)
 *   Blank      = 0x2400 + 0x0248 (suppressed leading zero tile)
 *   Bottom row = top tile + 0x0010 (next VRAM tile row)
 */
void fill_hp_pp_tile_buffer(uint16_t id, uint16_t mode, uint16_t fraction) {
    /* Compute carry from roller fraction overflow.
     * Assembly: if fraction >= $3000, carry = (fraction - $3000) / $3400.
     * $3400 = 13312 (comment: "13312/65536 is close to 1/5"). */
    int carry;
    if (fraction >= 0x3000) {
        carry = (int)((uint32_t)(fraction - 0x3000) / 0x3400);
    } else {
        carry = 0;
    }

    /* Select the row pair: mode 0 = hp1/hp2, mode 1 = pp1/pp2 */
    uint16_t *row1, *row2;
    if (mode == 0) {
        row1 = win.hppp_window_buffer[id].hp1;
        row2 = win.hppp_window_buffer[id].hp2;
    } else {
        row1 = win.hppp_window_buffer[id].pp1;
        row2 = win.hppp_window_buffer[id].pp2;
    }

    uint8_t ones     = win.hppp_window_digit_buffer[2];
    uint8_t tens     = win.hppp_window_digit_buffer[1];
    uint8_t hundreds = win.hppp_window_digit_buffer[0];

    /* --- Ones digit (rightmost, index 2) ---
     * Base tile 0x2600 (VRAM tile 0x200, palette 1, priority 1). */
    uint16_t tile = 0x2600 + digit_tile_offset(ones) + carry;
    row1[2] = tile;
    row2[2] = tile + 0x0010;

    /* Carry propagation: ones == 9 means tens might roll over */
    if (!(ones == 9 && carry != 0))
        carry = 0;

    /* --- Tens digit (middle, index 1) ---
     * Base tile 0x2400.  Add 0x0200 for a visible digit, 0x0248 for blank
     * (leading zero suppression: blank if tens==0 AND hundreds==0). */
    uint16_t tens_base = (tens == 0 && hundreds == 0) ? 0x0248 : 0x0200;
    tile = 0x2400 + tens_base + digit_tile_offset(tens) + carry;
    row1[1] = tile;
    row2[1] = tile + 0x0010;

    /* Carry propagation: tens == 9 means hundreds might roll over */
    if (!(tens == 9 && carry != 0))
        carry = 0;

    /* --- Hundreds digit (leftmost, index 0) ---
     * Same base 0x2400.  Blank if hundreds==0. */
    uint16_t hund_base = (hundreds == 0) ? 0x0248 : 0x0200;
    tile = 0x2400 + hund_base + digit_tile_offset(hundreds) + carry;
    row1[0] = tile;
    row2[0] = tile + 0x0010;
}

/*
 * FILL_HP_PP_TILE_BUFFER_X — Port of asm/text/hp_pp_window/fill_tile_buffer_x.asm (32 lines).
 *
 * Writes "X" placeholder tiles into the PP row of win.hppp_window_buffer[id].
 * Used when a character is concentrating (can't use PP).
 *
 * Assembly tile constants:
 *   Top row:    0x264C + i  (i = 0, 1, 2)
 *   Bottom row: 0x265C + i
 */
void fill_hp_pp_tile_buffer_x(uint16_t id) {
    for (int i = 0; i < HPPP_DIGIT_COUNT; i++) {
        win.hppp_window_buffer[id].pp1[i] = 0x264C + i;
        win.hppp_window_buffer[id].pp2[i] = 0x265C + i;
    }
}

/*
 * FILL_CHARACTER_HP_TILE_BUFFER — Port of asm/text/hp_pp_window/fill_character_hp_tile_buffer.asm.
 *
 * Wrapper: decomposes the HP integer into digits, then fills the HP tile ert.buffer.
 *
 * Assembly:
 *   A = id, X = integer, Y = fraction
 *   JSR SEPARATE_DECIMAL_DIGITS  (with X = integer)
 *   JSR FILL_HP_PP_TILE_BUFFER   (A = id, X = 0, Y = fraction)
 */
void fill_character_hp_tile_buffer(uint16_t id, uint16_t integer, uint16_t fraction) {
    separate_decimal_digits(integer);
    fill_hp_pp_tile_buffer(id, 0, fraction);
}

/*
 * FILL_CHARACTER_PP_TILE_BUFFER — Port of asm/text/hp_pp_window/fill_character_pp_tile_buffer.asm.
 *
 * If the character is concentrating (afflictions[CONCENTRATION] != 0),
 * fills the PP display with "X" tiles.  Otherwise, decomposes the PP
 * integer into digits and fills normally.
 *
 * Assembly parameters: A = id, X = afflictions ptr, Y = integer, stack = fraction
 */
void fill_character_pp_tile_buffer(uint16_t id, uint8_t *afflictions,
                                   uint16_t integer, uint16_t fraction) {
    if (afflictions[STATUS_GROUP_CONCENTRATION] != 0) {
        fill_hp_pp_tile_buffer_x(id);
    } else {
        separate_decimal_digits(integer);
        fill_hp_pp_tile_buffer(id, 1, fraction);
    }
}

/*
 * STATUS_EQUIP_WINDOW_TEXT tile lookup tables.
 * Loaded from "data/status_equip_tile_tables.bin" (294 bytes):
 *   Offset   0: TEXT_1 (98 bytes) — mode=1 tile indices
 *   Offset  98: TEXT_2 (98 bytes) — mode=0 tile indices
 *   Offset 196: TEXT_3 (98 bytes) — palette multipliers
 * Each table: 7 rows (affliction slots) × 7 columns (affliction values) × 2 bytes.
 */
static const uint16_t *status_tile_table_1;  /* TEXT_1: mode=1 tiles */
static const uint16_t *status_tile_table_2;  /* TEXT_2: mode=0 tiles */
static const uint16_t *status_tile_table_3;  /* TEXT_3: palette multipliers */

#define STATUS_TABLE_COLS  7  /* max affliction values per slot */
#define STATUS_TABLE_SIZE  (AFFLICTION_GROUP_COUNT * STATUS_TABLE_COLS)  /* 49 words */

static void load_status_tile_tables(void) {
    static const uint8_t *raw;
    if (!raw) {
        raw = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_TILE_TABLES_BIN);
        status_tile_table_1 = (const uint16_t *)(raw);
        status_tile_table_2 = (const uint16_t *)(raw + STATUS_TABLE_SIZE * 2);
        status_tile_table_3 = (const uint16_t *)(raw + STATUS_TABLE_SIZE * 4);
    }
}

/*
 * Scan an afflictions[7] array for the first non-zero entry.
 * Priority: slot 0, then slot 3, then slots 1-6.
 * Returns: found slot index in *out_slot, affliction value in *out_value.
 * Returns false if no affliction found.
 */
static bool scan_afflictions(const uint8_t *afflictions,
                             int *out_slot, uint8_t *out_value) {
    /* Check slot 0 first (consciousness group — unconscious, diamondized, etc.) */
    if (afflictions[0] != 0) {
        *out_slot = 0;
        *out_value = afflictions[0];
        return true;
    }
    /* Check slot 3 next */
    if (afflictions[3] != 0) {
        *out_slot = 3;
        *out_value = afflictions[3];
        return true;
    }
    /* Scan slots 1-6 */
    for (int i = 1; i < AFFLICTION_GROUP_COUNT; i++) {
        if (afflictions[i] != 0) {
            *out_slot = i;
            *out_value = afflictions[i];
            return true;
        }
    }
    return false;
}

/* GET_EQUIP_WINDOW_TEXT — Port of asm/battle/get_equip_window_text.asm. */
uint16_t get_equip_window_text(const uint8_t *afflictions, int mode) {
    load_status_tile_tables();
    int slot;
    uint8_t value;
    if (!scan_afflictions(afflictions, &slot, &value)) {
        return (mode != 0) ? 7 : 32;
    }
    const uint16_t *table = (mode != 0) ? status_tile_table_1 : status_tile_table_2;
    return table[slot * STATUS_TABLE_COLS + (value - 1)];
}

/* GET_EQUIP_WINDOW_TEXT_ALT — Port of asm/battle/get_equip_window_text_alt.asm. */
uint16_t get_equip_window_text_alt(const uint8_t *afflictions) {
    load_status_tile_tables();
    int slot;
    uint8_t value;
    if (!scan_afflictions(afflictions, &slot, &value)) {
        return 4;
    }
    return status_tile_table_3[slot * STATUS_TABLE_COLS + (value - 1)];
}

/*
 * DRAW_HP_PP_WINDOW — Port of asm/text/hp_pp_window/draw.asm (665 lines).
 *
 * Draws the full HP/PP window (border, character name, HP digits, PP digits)
 * for one party slot into win.bg2_buffer.
 *
 * The window is HPPP_WINDOW_WIDTH (7) tiles wide × HPPP_WINDOW_HEIGHT (8) rows:
 *   Row 0: top border (corners + edge fill)
 *   Row 1: left edge + 4 name tiles (row 1) + status tile + right edge
 *   Row 2: left edge + 4 name tiles (row 2) + status tile + right edge
 *   Row 3: left edge + 2 HP header + 3 HP digits (top) + right edge
 *   Row 4: left edge + 2 HP header + 3 HP digits (bottom) + right edge
 *   Row 5: left edge + 2 PP header + 3 PP digits (top) + right edge
 *   Row 6: left edge + 2 PP header + 3 PP digits (bottom) + right edge
 *   Row 7: bottom border (corners + edge fill)
 *
 * SNES tilemap entry format (16-bit):
 *   bit 15: V-flip, bit 14: H-flip, bit 13: priority
 *   bits 12-10: palette (0-7), bits 9-0: tile number (0-1023)
 *
 * Border tiles: 4=corners, 5=horizontal edge, 6=vertical edge, 7=blank interior.
 * Name tiles: 0x2A0 + (member-1)*4 (row 1), 0x2B0 + (member-1)*4 (row 2).
 *   Rendered by LOAD_WINDOW_GFX using BATTLE font into VRAM.
 *
 * HP/PP header tiles from HPPP_WINDOW_TILE_DATA (asm/data/unknown/C3E3F8.asm):
 *   HP row 1: tiles 0x08, 0x09; HP row 2: tiles 0x18, 0x19
 *   PP row 1: tiles 0x0A, 0x09; PP row 2: tiles 0x1A, 0x19
 *
 * Status affliction lookup (GET_EQUIP_WINDOW_TEXT / _ALT) determines:
 *   - name_palette: palette bits for name tiles (normally 0x1000 = palette 4)
 *   - header_palette: palette bits for HP/PP label tiles
 *   - digit_offset: added to digit tile entries (0 normal, 0x0800 greyed)
 *   - status_tile: affliction indicator tile (7 = none/blank)
 */
void draw_hp_pp_window(uint16_t char_id) {
    if (char_id >= HPPP_MAX_PARTY) return;

    /* Get party member and character struct (assembly lines 26-42) */
    uint8_t member_id = game_state.party_members[char_id];
    if (member_id == 0 || member_id > TOTAL_PARTY_COUNT)
        return;
    CharStruct *ch = &party_characters[member_id - 1];

    /* Status/affliction tile lookup (assembly lines 43-89).
     * GET_EQUIP_WINDOW_TEXT with mode=1 returns a tile index; it's called twice
     * and combined: LOCAL07 = (result & 0xFFF0) + result. This maps the table
     * entry (e.g. 0x0160) to a VRAM tile (e.g. 0x02C0) for the status icon.
     * GET_EQUIP_WINDOW_TEXT_ALT returns a palette multiplier (× 0x0400).
     * For greyed (0x0C00) characters, the functions are skipped entirely. */
    uint16_t palette_bits = ch->hp_pp_window_options;  /* @LOCAL06 / @VIRTUAL04 */
    uint16_t name_palette;    /* @LOCAL05 */
    uint16_t header_palette;  /* @LOCAL08 */
    uint16_t digit_offset;    /* @LOCAL04 */
    uint16_t status_tile;     /* @LOCAL07 */

    /* Status icon tile: assembly calls GET_EQUIP_WINDOW_TEXT(affls, 1) twice,
     * then: LOCAL07 = (first & 0xFFF0) + second (lines 46-64) */
    uint16_t ewt_first = get_equip_window_text(ch->afflictions, 1);
    uint16_t ewt_second = get_equip_window_text(ch->afflictions, 1);
    status_tile = (ewt_first & 0xFFF0) + ewt_second;

    if (palette_bits == 0x0C00) {
        /* Greyed out / unusable (assembly lines 71-79) */
        name_palette = 0x0C00;
        header_palette = 0x0C00;
        digit_offset = 0x0800;
    } else {
        /* Normal display (assembly lines 80-89) */
        uint16_t alt = get_equip_window_text_alt(ch->afflictions);
        name_palette = alt * 0x0400;
        header_palette = 0x1000;
        digit_offset = 0;
    }

    /* Determine Y row (assembly lines 91-99) */
    uint16_t y_row;
    if (win.battle_menu_current_character_id == (int16_t)char_id)
        y_row = ACTIVE_HPPP_WINDOW_Y_OFFSET;
    else
        y_row = NORMAL_HPPP_WINDOW_Y_OFFSET;

    /* Compute BG2_BUFFER position (assembly lines 101-127).
     * Each party member's window is 7 tiles wide, centered at HPPP_CENTER_TILE.
     * x_start = HPPP_CENTER_TILE - (party_count * 7) / 2 + char_id * 7
     * Byte offset = y_row * 64 + x_start * 2 */
    uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
    uint16_t half_width = (party_count * HPPP_WINDOW_WIDTH) / 2;
    uint16_t x_start = HPPP_CENTER_TILE - half_width + char_id * HPPP_WINDOW_WIDTH;
    uint16_t off = y_row * 64 + x_start * 2;

    /* Row stride in bytes (32 tiles * 2 bytes - window width * 2 bytes) */
    #define ROW_ADVANCE (64 - HPPP_WINDOW_WIDTH * 2)

    /* Helper: write a 16-bit tilemap entry to win.bg2_buffer */
    #define BG2_WRITE(offset, val) do { \
        win.bg2_buffer[(offset)]     = (uint8_t)((val) & 0xFF); \
        win.bg2_buffer[(offset) + 1] = (uint8_t)((val) >> 8); \
    } while (0)

    /* --- Row 0: Top border (assembly lines 128-167/264-282) ---
     * Left corner (tile 4), 5 fill tiles (tile 5), right corner (tile 4 H-flipped) */
    BG2_WRITE(off, palette_bits + 0x2004); off += 2;
    for (int i = 0; i < HPPP_WINDOW_WIDTH - 2; i++) {
        BG2_WRITE(off, palette_bits + 0x2005); off += 2;
    }
    BG2_WRITE(off, palette_bits + 0x6004); off += 2;
    off += ROW_ADVANCE;

    /* --- Row 1: Name row 1 (assembly lines 276-337 US path) ---
     * Left edge (tile 6) + 4 name tiles + status tile + right edge (tile 6 H-flipped) */
    BG2_WRITE(off, palette_bits + 0x2006); off += 2;

    /* Character name tiles (row 1): base = 0x22A0 + (member_id-1)*4.
     * 0x2000 = priority bit, 0x02A0 = tile 672 base for character 0.
     * US: STRLEN computes name width = (strlen*6+9)/8 tiles. */
    uint16_t name_base1 = 0x22A0 + (uint16_t)(member_id - 1) * 4;
    int name_len = 0;
    for (int i = 0; i < 5 && ch->name[i] != 0; i++) name_len++;
    int name_tile_count = (name_len * 6 + 9) / 8;  /* assembly: STRLEN*6+9 >> 3 */
    int ntc = name_tile_count;

    for (int i = 0; i < HPPP_WINDOW_WIDTH - 3; i++) {
        if (ntc > 0) {
            BG2_WRITE(off, name_base1 + name_palette);
            name_base1++;
            ntc--;
        } else {
            BG2_WRITE(off, name_palette + 0x2007);  /* blank interior */
        }
        off += 2;
    }
    /* Status indicator tile (row 1): status_tile + name_palette + priority */
    BG2_WRITE(off, name_palette + status_tile + 0x2000); off += 2;
    /* Right edge */
    BG2_WRITE(off, palette_bits + 0x6006); off += 2;
    off += ROW_ADVANCE;

    /* --- Row 2: Name row 2 (assembly lines 390-457 US path) ---
     * Same structure as row 1, with row-2 name tiles at 0x22B0 base
     * and blank tile 0x2017 (tile 23 = row-2 blank). */
    BG2_WRITE(off, palette_bits + 0x2006); off += 2;

    uint16_t name_base2 = 0x22B0 + (uint16_t)(member_id - 1) * 4;
    ntc = name_tile_count;

    for (int i = 0; i < HPPP_WINDOW_WIDTH - 3; i++) {
        if (ntc > 0) {
            BG2_WRITE(off, name_base2 + name_palette);
            name_base2++;
            ntc--;
        } else {
            BG2_WRITE(off, name_palette + 0x2017);  /* blank row 2 */
        }
        off += 2;
    }
    /* Status indicator tile (row 2): status_tile + name_palette + 0x2010 */
    BG2_WRITE(off, name_palette + status_tile + 0x2010); off += 2;
    /* Right edge */
    BG2_WRITE(off, palette_bits + 0x6006); off += 2;
    off += ROW_ADVANCE;

    /* --- Rows 3-4: HP section (assembly lines 462-556) ---
     * Fill HP digit ert.buffer, then write 2 rows of: edge + 2 header + 3 digits + edge.
     *
     * HPPP_WINDOW_TILE_DATA (asm/data/unknown/C3E3F8.asm):
     *   HP row 1: 0x08, 0x09; HP row 2: 0x18, 0x19
     *   PP row 1: 0x0A, 0x09; PP row 2: 0x1A, 0x19 */
    static const uint8_t hppp_header_tiles[8] = {
        0x08, 0x09,  /* HP top */
        0x18, 0x19,  /* HP bottom */
        0x0A, 0x09,  /* PP top */
        0x1A, 0x19   /* PP bottom */
    };

    fill_character_hp_tile_buffer(char_id, ch->current_hp, ch->current_hp_fraction);

    int hdr_idx = 0;
    uint16_t *digit_buf = win.hppp_window_buffer[char_id].hp1;
    for (int row = 0; row < 2; row++) {
        /* Left edge */
        BG2_WRITE(off, palette_bits + 0x2006); off += 2;
        /* 2 HP header label tiles */
        for (int h = 0; h < 2; h++) {
            uint16_t tile = (uint16_t)hppp_header_tiles[hdr_idx++]
                          + header_palette + 0x2000;
            BG2_WRITE(off, tile); off += 2;
        }
        /* 3 digit tiles from ert.buffer */
        for (int d = 0; d < HPPP_DIGIT_COUNT; d++) {
            BG2_WRITE(off, digit_buf[d] + digit_offset); off += 2;
        }
        /* Right edge */
        BG2_WRITE(off, palette_bits + 0x6006); off += 2;
        off += ROW_ADVANCE;
        digit_buf = win.hppp_window_buffer[char_id].hp2;
    }

    /* --- Rows 5-6: PP section (assembly lines 557-639) ---
     * Same structure as HP but with PP header tiles and PP digit ert.buffer. */
    fill_character_pp_tile_buffer(char_id, ch->afflictions,
                                  ch->current_pp, ch->current_pp_fraction);

    digit_buf = win.hppp_window_buffer[char_id].pp1;
    for (int row = 0; row < 2; row++) {
        /* Left edge */
        BG2_WRITE(off, palette_bits + 0x2006); off += 2;
        /* 2 PP header label tiles */
        for (int h = 0; h < 2; h++) {
            uint16_t tile = (uint16_t)hppp_header_tiles[hdr_idx++]
                          + header_palette + 0x2000;
            BG2_WRITE(off, tile); off += 2;
        }
        /* 3 digit tiles from ert.buffer */
        for (int d = 0; d < HPPP_DIGIT_COUNT; d++) {
            BG2_WRITE(off, digit_buf[d] + digit_offset); off += 2;
        }
        /* Right edge */
        BG2_WRITE(off, palette_bits + 0x6006); off += 2;
        off += ROW_ADVANCE;
        digit_buf = win.hppp_window_buffer[char_id].pp2;
    }

    /* --- Row 7: Bottom border (assembly lines 640-665) ---
     * Left corner (tile 4 V-flipped), 5 fill (tile 5 V-flipped),
     * right corner (tile 4 V+H-flipped) */
    BG2_WRITE(off, palette_bits + 0xA004); off += 2;
    for (int i = 0; i < HPPP_WINDOW_WIDTH - 2; i++) {
        BG2_WRITE(off, palette_bits + 0xA005); off += 2;
    }
    BG2_WRITE(off, palette_bits + 0xE004);

    #undef BG2_WRITE
    #undef ROW_ADVANCE
}

/*
 * DRAW_AND_MARK_HPPP_WINDOW — Port of asm/battle/draw_and_mark_hppp_window.asm.
 *
 * Assembly:
 *   SEP #$30
 *   TAY             ; Y = char_id (8-bit)
 *   REP #$20
 *   LDA #1
 *   JSL ASL16_ENTRY2  ; A = 1 << char_id
 *   ORA CURRENTLY_DRAWN_HPPP_WINDOWS
 *   STA CURRENTLY_DRAWN_HPPP_WINDOWS
 *   LDA @LOCAL00    ; char_id
 *   JSR DRAW_HP_PP_WINDOW
 *   LDA #1
 *   STA HPPP_METER_AREA_NEEDS_UPDATE
 */
void draw_and_mark_hppp_window(uint16_t char_id) {
    ow.currently_drawn_hppp_windows |= (1 << (char_id & 0xFF));
    draw_hp_pp_window(char_id);
    win.hppp_meter_area_needs_update = 1;
}

/*
 * DRAW_ACTIVE_HPPP_WINDOWS — Port of asm/battle/draw_active_hppp_windows.asm.
 *
 * Iterates through party members. For each one whose bit is set in
 * ow.currently_drawn_hppp_windows, calls draw_hp_pp_window.
 */
void draw_active_hppp_windows(void) {
    uint16_t drawn = ow.currently_drawn_hppp_windows;
    uint16_t party_count = game_state.player_controlled_party_count & 0xFF;

    for (uint16_t i = 0; i < party_count; i++) {
        if (drawn & 1) {
            draw_hp_pp_window(i);
        }
        drawn >>= 1;
    }
}

/*
 * HIDE_HPPP_WINDOWS — Port of asm/text/hide_hppp_windows.asm.
 *
 * Clears the battle menu character indicator, disables HPPP window rendering,
 * then (if not in battle mode) loops through all party members to:
 *   - Undraw each HP/PP window
 *   - Snap current_hp/pp to their target values
 *   - Clear hp/pp fractions
 * Finally sets ow.redraw_all_windows = 1.
 */
void hide_hppp_windows(void) {
    clear_battle_menu_character_indicator();
    ow.render_hppp_windows = 0;

    if (!bt.battle_mode_flag) {
        uint16_t party_count = game_state.player_controlled_party_count & 0xFF;

        for (uint16_t i = 0; i < party_count; i++) {
            undraw_hp_pp_window(i);

            /* Snap HP/PP to target values */
            uint8_t member = game_state.party_members[i];
            if (member == 0) continue;
            CharStruct *cs = &party_characters[member - 1];
            cs->current_hp = cs->current_hp_target;
            cs->current_pp = cs->current_pp_target;
            cs->current_hp_fraction = 0;
            cs->current_pp_fraction = 0;
        }
    }

    ow.redraw_all_windows = 1;
}

/*
 * UPDATE_HPPP_METER_AND_RENDER — Port of asm/text/hp_pp_window/update_hppp_meter_and_render.asm (16 lines).
 *
 * Per-frame orchestrator:
 *   1. HP_PP_ROLLER — animate HP/PP values toward targets
 *   2. If win.bg2_buffer was modified (win.hppp_meter_area_needs_update), bulk-copy
 *      to VRAM and set win.upload_hppp_meter_tiles flag
 *   3. UPDATE_HPPP_METER_TILES — update individual digit tiles
 *   4. RENDER_FRAME_TICK — process one frame
 */
void update_hppp_meter_and_render(void) {
    hp_pp_roller();

    if (win.hppp_meter_area_needs_update) {
        copy_hppp_window_to_vram();
        win.hppp_meter_area_needs_update = 0;
        win.upload_hppp_meter_tiles = 1;
    }

    update_hppp_meter_tiles();
    render_frame_tick();
}

/*
 * UPDATE_HPPP_METER_TILES — Port of asm/text/update_hppp_meter_tiles.asm (325 lines).
 *
 * Each frame, selects one of 4 party members (FRAME_COUNTER & 3) and updates
 * their HP/PP digit tiles in win.bg2_buffer.  When the roller animation is active
 * (fraction & 1), recomputes the digit tile ert.buffer and copies the tilemap
 * entries into the appropriate BG2_BUFFER positions.
 *
 * When win.upload_hppp_meter_tiles is clear (0), also writes the changed tiles
 * directly to ppu.vram[] for immediate display.  When set (1), skips the
 * VRAM writes (a bulk copy just happened) and clears the flag.
 */
void update_hppp_meter_tiles(void) {
    if (!(ow.render_hppp_windows & 0xFF))
        return;

    /* Select one party member per frame (round-robin over 4 slots) */
    uint16_t member_index = core.frame_counter & 3;

    /* Validate: member must exist and be a valid party member (1-4) */
    uint8_t member_id = game_state.party_members[member_index];
    if (member_id == 0)
        return;
    if (member_id > 4)
        return;

    /* Check if this member's HPPP window is currently drawn */
    if (!((ow.currently_drawn_hppp_windows >> member_index) & 1))
        return;

    /* Determine Y row: active character uses row 18, others use row 19 */
    uint16_t base_y;
    if (win.battle_menu_current_character_id == (int16_t)member_index)
        base_y = ACTIVE_HPPP_WINDOW_Y_OFFSET;
    else
        base_y = NORMAL_HPPP_WINDOW_Y_OFFSET;

    /* Calculate tilemap word offset for the HP digit row.
     * base_offset = row * 32 + 96  (96 = 3 rows below header = digit area)
     * Center horizontally: x_start = HPPP_CENTER_TILE - floor(party_count * 7 / 2)
     * Skip 3 entries (border + name area) */
    uint16_t base_offset = base_y * 32 + 96;
    uint16_t party_count = game_state.player_controlled_party_count & 0xFF;
    uint16_t half_width = (party_count * 7) / 2;
    uint16_t tile_start = base_offset + HPPP_CENTER_TILE - half_width + 3;

    /* Offset for this specific party member (7 tiles per window) */
    uint16_t tile_pos = tile_start + member_index * 7;

    /* Byte offset into win.bg2_buffer */
    uint16_t bg2_off = tile_pos * 2;

    /* VRAM word address for this tilemap entry */
    uint16_t vram_addr = tile_pos + VRAM_TEXT_LAYER_TILEMAP;

    /* Get the party character struct */
    CharStruct *ch = &party_characters[member_id - 1];

    /* ---- HP digit update ---- */
    uint16_t hp_fraction = ch->current_hp_fraction;
    if (hp_fraction & 1) {
        /* Roller is active — recompute HP digit tiles */
        fill_character_hp_tile_buffer(member_index, ch->current_hp, hp_fraction);

        /* Set up VRAM copies if not already bulk-uploaded */
        if (!win.upload_hppp_meter_tiles) {
            /* Copy HP top row (3 entries = 6 bytes) to VRAM */
            memcpy(&ppu.vram[vram_addr * 2],
                   win.hppp_window_buffer[member_index].hp1,
                   HPPP_DIGIT_COUNT * 2);
            /* Copy HP bottom row to VRAM (next tilemap row = +32 entries) */
            memcpy(&ppu.vram[(vram_addr + 32) * 2],
                   win.hppp_window_buffer[member_index].hp2,
                   HPPP_DIGIT_COUNT * 2);
        }

        /* Copy HP top tiles to win.bg2_buffer */
        for (int i = 0; i < HPPP_DIGIT_COUNT; i++) {
            uint16_t tile = win.hppp_window_buffer[member_index].hp1[i];
            win.bg2_buffer[bg2_off + i * 2]     = (uint8_t)(tile & 0xFF);
            win.bg2_buffer[bg2_off + i * 2 + 1] = (uint8_t)(tile >> 8);
        }

        /* Copy HP bottom tiles to win.bg2_buffer (next row = +64 bytes) */
        uint16_t bg2_bottom = bg2_off + 64;
        for (int i = 0; i < HPPP_DIGIT_COUNT; i++) {
            uint16_t tile = win.hppp_window_buffer[member_index].hp2[i];
            win.bg2_buffer[bg2_bottom + i * 2]     = (uint8_t)(tile & 0xFF);
            win.bg2_buffer[bg2_bottom + i * 2 + 1] = (uint8_t)(tile >> 8);
        }

        /* Advance to PP area (2 rows below HP top = +128 bytes) */
        bg2_off += 128;
    } else {
        /* HP not rolling — skip 2 rows to PP area */
        bg2_off += 128;
    }

    /* ---- PP digit update ---- */
    uint16_t pp_fraction = ch->current_pp_fraction;
    if (pp_fraction & 1) {
        /* Roller is active — recompute PP digit tiles */
        fill_character_pp_tile_buffer(member_index,
                                      ch->afflictions,
                                      ch->current_pp,
                                      pp_fraction);

        /* Set up VRAM copies if not already bulk-uploaded */
        if (!win.upload_hppp_meter_tiles) {
            /* Copy PP top row to VRAM (+64 entries from HP top = 2 rows down) */
            memcpy(&ppu.vram[(vram_addr + 64) * 2],
                   win.hppp_window_buffer[member_index].pp1,
                   HPPP_DIGIT_COUNT * 2);
            /* Copy PP bottom row to VRAM (+96 entries = 3 rows down) */
            memcpy(&ppu.vram[(vram_addr + 96) * 2],
                   win.hppp_window_buffer[member_index].pp2,
                   HPPP_DIGIT_COUNT * 2);
        }

        /* Copy PP top tiles to win.bg2_buffer */
        for (int i = 0; i < HPPP_DIGIT_COUNT; i++) {
            uint16_t tile = win.hppp_window_buffer[member_index].pp1[i];
            win.bg2_buffer[bg2_off + i * 2]     = (uint8_t)(tile & 0xFF);
            win.bg2_buffer[bg2_off + i * 2 + 1] = (uint8_t)(tile >> 8);
        }

        /* Copy PP bottom tiles to win.bg2_buffer (next row = +64 bytes) */
        uint16_t pp_bottom = bg2_off + 64;
        for (int i = 0; i < HPPP_DIGIT_COUNT; i++) {
            uint16_t tile = win.hppp_window_buffer[member_index].pp2[i];
            win.bg2_buffer[pp_bottom + i * 2]     = (uint8_t)(tile & 0xFF);
            win.bg2_buffer[pp_bottom + i * 2 + 1] = (uint8_t)(tile >> 8);
        }
    }

    /* If upload flag was set, clear it after processing */
    if (win.upload_hppp_meter_tiles) {
        win.upload_hppp_meter_tiles = 0;
    }
}

/* ---- Window Text Scrolling ---- */

/*
 * SCROLL_WINDOW_UP: Port of asm/text/window/scroll_window_up.asm.
 *
 * Operates on per-window content_tilemap:
 *   1. Frees top-row tiles (2 tile rows × content_width entries)
 *   2. Shifts all content_tilemap entries up by one text line (2 tile rows)
 *   3. Fills the bottom text line (2 tile rows) with 0
 */
void scroll_window_up(WindowInfo *w) {
    if (!w) return;

    uint16_t content_width = w->width - 2;
    uint16_t interior_tile_rows = w->height - 2;
    uint16_t entries_per_text_line = content_width * 2;  /* 2 tile rows per text line */
    uint16_t total_entries = content_width * interior_tile_rows;

    /* 1. Free top text line tiles (first 2 tile rows) */
    for (uint16_t i = 0; i < entries_per_text_line && i < w->content_tilemap_size; i++) {
        free_tile(w->content_tilemap[i]);
    }

    /* 2. Shift entries up by one text line (entries_per_text_line entries) */
    if (total_entries > entries_per_text_line) {
        uint16_t shift_count = total_entries - entries_per_text_line;
        if (shift_count > w->content_tilemap_size) shift_count = w->content_tilemap_size;
        memmove(w->content_tilemap,
                w->content_tilemap + entries_per_text_line,
                shift_count * sizeof(uint16_t));
    }

    /* 3. Fill bottom text line with 0 (empty) */
    uint16_t bottom_start = total_entries - entries_per_text_line;
    if (bottom_start < w->content_tilemap_size) {
        uint16_t fill_count = entries_per_text_line;
        if (bottom_start + fill_count > w->content_tilemap_size)
            fill_count = w->content_tilemap_size - bottom_start;
        memset(w->content_tilemap + bottom_start, 0, fill_count * sizeof(uint16_t));
    }
}

/*
 * FREE_WINDOW_TEXT_ROW: Port of asm/text/window/free_window_text_row.asm.
 *
 * Frees tiles at the current text_y row in content_tilemap and fills with 0.
 * Each text line = 2 tile rows (upper + lower for 16px font).
 */
void free_window_text_row(WindowInfo *w) {
    if (!w) return;

    uint16_t content_width = w->width - 2;
    uint16_t row_base = w->text_y * content_width * 2;

    /* Free tiles and clear both tile rows for this text line */
    for (uint16_t i = 0; i < content_width * 2; i++) {
        uint16_t idx = row_base + i;
        if (idx >= w->content_tilemap_size) break;
        free_tile(w->content_tilemap[idx]);
        w->content_tilemap[idx] = 0;
    }
}

/* ---- BG2 Tile Usage Tracking ---- */

/* win.used_bg2_tile_map now lives in WindowSystemState win (see window.h). */

/* LOCKED_TILES: 512-byte table from asm/data/text/locked_tiles.asm.
 * $01 = locked (cannot be freed by FREE_TILE), $00 = freeable.
 * First ~160 tiles are locked (system tiles: window borders, cursors, fonts).
 * Exact byte-for-byte copy of the assembly data. */
static const uint8_t locked_tiles[512] = {
    /* Tiles 0x00-0x3F: all locked ($01) */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* Tiles 0x40-0x4F: 0x40-0x4B locked, 0x4C-0x4E free, 0x4F locked */
    1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,
    /* Tiles 0x50-0x5F: 0x50-0x54 locked, 0x55-0x5E free, 0x5F locked */
    1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,1,
    /* Tiles 0x60-0x6F: 0x60-0x69 locked, 0x6A-0x6F free */
    1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
    /* Tiles 0x70-0x7F: 0x70-0x79 locked, 0x7A-0x7F free */
    1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
    /* Tiles 0x80-0x8F: 0x80 locked, rest free */
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* Tiles 0x90-0x9F: 0x90 locked, rest free */
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* Tiles 0xA0-0x1FF: all free ($00) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/*
 * FREE_TILE: Port of asm/text/free_tile.asm.
 *
 * Marks a tile as free in win.used_bg2_tile_map (clears the corresponding bit).
 * Skips tiles that are marked as locked in locked_tiles[].
 *
 * Assembly: AND #$03FF → tile index. Check LOCKED_TILES[index].
 *           word_index = tile >> 4.  bit_pos = tile & 0xF.
 *           win.used_bg2_tile_map[word_index] &= ~(1 << bit_pos).
 */
void free_tile(uint16_t tile_entry) {
    uint16_t tile = tile_entry & 0x03FF;
    if (tile >= 512) return;
    if (locked_tiles[tile]) return;

    uint16_t word_index = tile >> 4;
    uint16_t bit_pos = tile & 0x0F;
    win.used_bg2_tile_map[word_index] &= ~(1 << bit_pos);
}

/*
 * FREE_TILE_SAFE: Port of asm/text/free_tile_safe.asm.
 *
 * Wrapper around free_tile that skips tile 0 and tile 64 (blank tile).
 * These are never freed because tile 0 is the universal transparent tile
 * and tile 64 is the window interior fill tile.
 */
void free_tile_safe(uint16_t tile_entry) {
    uint16_t tile = tile_entry & 0x03FF;
    if (tile == BLANK_TILE || tile == 0) return;
    free_tile(tile_entry);
}

/*
 * INIT_USED_BG2_TILE_MAP (asm/system/dma/init_used_bg2_tile_map.asm)
 *
 * Copies the default BG2 tile usage bitmasks from ROM data.
 * The defaults mark system-reserved tiles (window frames, cursors, etc.)
 * as in-use so the dynamic allocator won't overwrite them.
 *
 * Default data from asm/data/unknown/C20958.asm:
 *   Entries 0-3: $FFFF (all tiles reserved)
 *   Entry 4: $8FFF, Entry 5: $801F
 *   Entries 6-7: $03FF, Entries 8-9: $0001
 *   Entries 10-31: $0000 (all tiles free)
 */
/*
 * ALLOC_BG2_TILEMAP_ENTRY — Port of asm/system/dma/alloc_bg2_tilemap_entry.asm.
 *
 * Scans win.used_bg2_tile_map[] from word 4 (tile 64+) to word 31 (tile 511)
 * to find the first free bit. Sets the bit and returns the tile index.
 *
 * Assembly algorithm:
 *   1. Start at Y=8 (word index 4, since Y is byte offset = index*2)
 *   2. Find first word != 0xFFFF (has a free bit)
 *   3. From bit 15 downward, find the highest free (0) bit
 *   4. Set the bit, return: word_index * 16 + bit_position
 *   5. On exhaustion: emergency recovery (close all windows, longjmp)
 */
uint16_t alloc_bg2_tilemap_entry(void) {
    for (int word = 4; word < USED_BG2_TILE_MAP_SIZE; word++) {
        if (win.used_bg2_tile_map[word] == 0xFFFF)
            continue;  /* all bits set = all tiles in use */

        /* Find highest free bit (assembly scans from bit 15 downward) */
        uint16_t val = win.used_bg2_tile_map[word];
        for (int bit = 15; bit >= 0; bit--) {
            if (!(val & (1 << bit))) {
                /* Found free tile — mark as in-use */
                win.used_bg2_tile_map[word] |= (1 << bit);
                return (uint16_t)(word * 16 + bit);
            }
        }
    }

    /* Tile exhaustion — emergency recovery.
     * Assembly: HIDE_HPPP_WINDOWS_LONG, CLOSE_ALL_WINDOWS_FAR,
     * ENABLE_ALL_ENTITIES, LONGJMP(JMP_BUF2).
     * C port: close all windows and return tile 0 as fallback. */
    fprintf(stderr, "WARNING: alloc_bg2_tilemap_entry: tile exhaustion!\n");
    close_all_windows();
    return 0;
}

void init_used_bg2_tile_map(void) {
    static const uint16_t defaults[USED_BG2_TILE_MAP_SIZE] = {
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0x8FFF, 0x801F, 0x03FF, 0x03FF,
        0x0001, 0x0001, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
    };
    memcpy(win.used_bg2_tile_map, defaults, sizeof(win.used_bg2_tile_map));
}

/*
 * SET_CURSOR_MOVE_CALLBACK — Port of asm/text/menu/set_cursor_move_callback.asm.
 *
 * Sets the cursor_move_callback for the current focus window.
 * The callback is invoked by selection_menu() when the cursor moves,
 * receiving the selected item's userdata (type 2) or index+1 (type 1).
 */
void set_cursor_move_callback(void (*cb)(uint16_t)) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (w)
        w->cursor_move_callback = cb;
}

/*
 * CLEAR_CURSOR_MOVE_CALLBACK — Port of asm/text/menu/clear_cursor_move_callback.asm.
 *
 * Clears the cursor_move_callback for the current focus window.
 */
void clear_cursor_move_callback(void) {
    WindowInfo *w = get_window(win.current_focus_window);
    if (w)
        w->cursor_move_callback = NULL;
}

/*
 * CLEAR_WINDOW_TILEMAP — Port of asm/text/clear_window_tilemap.asm.
 *
 * Assembly: iterates the window's tilemap ert.buffer, frees each tile via
 * FREE_TILE_SAFE, fills with tile 64 (blank). Sets REDRAW_ALL_WINDOWS=1
 * and calls CLEAR_PARTY_SPRITE_HIDE_FLAGS.
 */
void clear_window_tilemap(uint16_t window_id) {
    WindowInfo *w = get_window(window_id);
    if (!w) return;

    /* Free all tiles in content_tilemap and fill with 0.
     * Assembly (clear_window_tilemap.asm): iterates width*height, frees via
     * FREE_TILE_SAFE, fills with tile 64. Does NOT reset text_x/text_y/menu. */
    uint16_t content_width = w->width - 2;
    uint16_t interior_tile_rows = w->height - 2;
    uint16_t total = content_width * interior_tile_rows;
    if (total > w->content_tilemap_size) total = w->content_tilemap_size;

    for (uint16_t i = 0; i < total; i++) {
        free_tile_safe(w->content_tilemap[i]);
        w->content_tilemap[i] = 0;
    }

    ow.redraw_all_windows = 1;
    clear_party_sprite_hide_flags();
}

/*
 * SET_HPPP_WINDOW_MODE_ITEM — Port of asm/text/set_hppp_window_mode_item.asm (210 lines).
 *
 * For each party character, checks if the given item is usable/equippable,
 * and if equippable, compares the item's strength stat against the currently
 * equipped item in the same slot. Sets hp_pp_window_options to:
 *   0x0C00 = greyed out (item not usable by this character)
 *   0x0400 = normal (usable but not equippable, or equippable but not better)
 *   0x1400 = highlighted (equippable and stat is an improvement)
 *
 * Poo special case: compares EPI (item_parameters.epi, offset +1) instead
 * of strength (offset +0) because Poo's offense scales with IQ.
 *
 * Equipment[] stores 1-based inventory slot indices (not item IDs).
 * To get the equipped item_id: items[equipment[slot] - 1].
 */
void set_hppp_window_mode_item(uint16_t item_id) {
    const ItemConfig *new_item = get_item_entry(item_id);

    for (int i = 0; i < 4; i++) {
        /* Assembly line 26-30: CHECK_ITEM_USABLE_BY(char_id=i+1, item=item_id) */
        if (!check_item_usable_by((uint16_t)(i + 1), item_id)) {
            party_characters[i].hp_pp_window_options = 0x0C00;
            continue;
        }

        /* Assembly line 35-41: GET_ITEM_TYPE, check if equippable (type==2) */
        if (get_item_type(item_id) != 2) {
            party_characters[i].hp_pp_window_options = 0x0400;
            continue;
        }

        if (!new_item) {
            party_characters[i].hp_pp_window_options = 0x0400;
            continue;
        }

        /* Assembly lines 43-101: determine equipment slot from item.type bits 2-3 */
        uint8_t equip_type = new_item->type & 0x0C;
        int slot;
        switch (equip_type) {
        case 0x00: slot = EQUIP_WEAPON; break; /* weapon */
        case 0x04: slot = EQUIP_BODY;   break; /* body */
        case 0x08: slot = EQUIP_ARMS;   break; /* arms */
        case 0x0C: slot = EQUIP_OTHER;  break; /* other */
        default:
            party_characters[i].hp_pp_window_options = 0x0400;
            continue;
        }

        /* Poo (index 3) uses EPI instead of strength for stat comparison.
         * Assembly lines 106-112: if char_index == 3, param_offset = 1 */
        int param_offset = (i == 3) ? 1 : 0;

        /* Assembly lines 59-101: read currently equipped item from equipment slot.
         * equipment[] stores 1-based inventory slot index (0 = nothing equipped). */
        uint8_t equipped_slot = party_characters[i].equipment[slot];

        int16_t old_stat = 0;
        if (equipped_slot != 0) {
            /* Assembly lines 114-149: read equipped item's strength.
             * items[equipped_slot - 1] → item_id → item config → params.strength */
            uint8_t equipped_item_id = party_characters[i].items[equipped_slot - 1];
            const ItemConfig *eq_entry = get_item_entry(equipped_item_id);
            if (eq_entry) {
                /* Sign-extend byte to int16: assembly SEC; SBC $0080; EOR $FF80 */
                old_stat = (int16_t)(int8_t)eq_entry->params[ITEM_PARAM_STRENGTH + param_offset];
            }
        }

        /* Assembly lines 153-180: read new item's strength (same param_offset) */
        int16_t new_stat = (int16_t)(int8_t)new_item->params[ITEM_PARAM_STRENGTH + param_offset];

        /* Assembly lines 181-189: CLC; SBC old_stat → BRANCHLTEQS
         * This checks new_stat > old_stat (strictly greater) */
        if (new_stat > old_stat) {
            party_characters[i].hp_pp_window_options = 0x1400; /* better */
        } else {
            party_characters[i].hp_pp_window_options = 0x0400; /* normal */
        }
    }

    ow.redraw_all_windows = 1;
}

/*
 * PRINT_MONEY_IN_WINDOW — Port of asm/text/window/print_money_in_window.asm.
 *
 * Prints a right-aligned dollar amount ("$NNN") in the current focus window.
 * Saves/restores cursor position and VWF_INDENT_NEW_LINE.
 * Places tile 36 at the rightmost column as a visual separator.
 */
void print_money_in_window(uint32_t amount) {
    if (win.current_focus_window == WINDOW_ID_NONE) return;

    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) return;

    /* Save VWF_INDENT_NEW_LINE (asm lines 22-24) */
    uint8_t saved_vwf_indent = vwf_indent_new_line;
    vwf_indent_new_line = 0;

    /* Save current text position (asm lines 48-51) */
    uint16_t saved_text_x = w->text_x;
    uint16_t saved_text_y = w->text_y;

    /* Format the money string */
    char money_buf[16];
    snprintf(money_buf, sizeof(money_buf), "$%u", (unsigned)amount);

    /* Calculate total pixel width (asm lines 52-120):
     * Sum of (char_width + padding) for each character, plus extra trailing padding. */
    uint16_t total_pixel_width = 0;
    for (int c = 0; money_buf[c]; c++) {
        uint8_t eb = ascii_to_eb_char(money_buf[c]);
        uint8_t glyph_idx = (eb - 0x50) & 0x7F;
        total_pixel_width += font_get_width(w->font, glyph_idx) + character_padding;
    }
    total_pixel_width += character_padding;  /* Extra trailing padding (asm lines 114-120) */

    /* Right-align: pixel_pos = (content_width - 1) * 8 - total_pixel_width (asm lines 130-137)
     * w->width is outer width (includes 2 border tiles); asm stores content width (outer - 2). */
    uint16_t right_edge = (w->width - 3) * 8;
    uint16_t pixel_pos = (right_edge > total_pixel_width) ? (right_edge - total_pixel_width) : 0;

    /* Set force left alignment and position cursor (asm lines 122-137) */
    dt.force_left_text_alignment = 1;
    set_text_pixel_position(w->text_y, pixel_pos);

    /* Print the money string character by character (asm lines 138-153).
     * Assembly uses REDIRECT_PRINT_LETTER for each character, which includes
     * VWF rendering, sound, and per-character delay via WINDOW_TICK. */
    for (int c = 0; money_buf[c]; c++) {
        uint8_t eb = ascii_to_eb_char(money_buf[c]);
        uint8_t eb_buf[2] = { eb, 0x00 };
        print_eb_string(eb_buf, 1);

        /* PRINT_LETTER (asm lines 25-33): set REDRAW_ALL_WINDOWS if non-tail window */
        ow.redraw_all_windows = 1;

        /* Sound + delay (asm lines 36-81): same as display_text's PRINT_LETTER */
        if (!dt.instant_printing) {
            bool play_sound;
            if (dt.text_sound_mode == 2) {
                play_sound = true;
            } else if (dt.text_sound_mode == 3) {
                play_sound = false;
            } else {
                play_sound = (dt.blinking_triangle_flag == 0);
            }
            if (play_sound && eb != 0x20 && eb != 0x50) {
                play_sfx(7);  /* SFX::TEXT_PRINT */
            }

            int delay = (game_state.text_speed & 0xFF) + 1;
            for (int d = 0; d < delay; d++) {
                window_tick();
            }
        }
    }

    dt.force_left_text_alignment = 0;

    /* Place tile 36 at rightmost column (asm lines 156-164) */
    w = get_window(win.current_focus_window);
    set_focus_text_cursor(w->width - 3, w->text_y);
    print_char_with_sound(36);

    /* Restore cursor position (asm lines 165-167) */
    set_focus_text_cursor(saved_text_x, saved_text_y);

    /* Restore VWF_INDENT_NEW_LINE (asm lines 168-170) */
    vwf_indent_new_line = saved_vwf_indent;
}
