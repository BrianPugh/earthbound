#ifndef GAME_WINDOW_H
#define GAME_WINDOW_H

#include "core/types.h"

/* Sentinel value for "no window" / "no selection" (matches assembly's .LOWORD(-1)) */
#define WINDOW_ID_NONE  0xFF

/* Maximum number of open windows.
 * Reduced from 10 (original SNES BSS had 10 window slots).
 * Peak concurrent usage is 8 (file select confirmation screen:
 * 4 character names + king + food + thing + message).
 * HP/PP windows use a separate HPPPWindowBuffer, not these slots. */
#define MAX_WINDOWS 8

/* Per-window content tilemap max size.
 * Each entry is a 16-bit SNES tilemap word (tile index + attributes).
 * Content area = (width-2) columns × (height-2) tile rows.
 * Largest window (0x08 Status) = 28 × 16 = 448 entries. */
#define WINDOW_TILEMAP_MAX 450

/* Shared tilemap pool size (in uint16_t entries).
 * Empirical high-water mark is 936 entries. 1100 gives ~17% headroom. */
#define WINDOW_TILEMAP_POOL_SIZE 1100

/* Window IDs matching include/constants/windows.asm */
#define WINDOW_COMMAND_MENU           0x00
#define WINDOW_TEXT_STANDARD          0x01
#define WINDOW_INVENTORY              0x02
#define WINDOW_INVENTORY_MENU         0x03
#define WINDOW_PSI_TARGET_COST        0x04
#define WINDOW_PHONE_MENU             0x05
#define WINDOW_EQUIP_MENU             0x06
#define WINDOW_EQUIP_MENU_ITEMLIST    0x07
#define WINDOW_STATUS_MENU            0x08
#define WINDOW_STORE_ITEM_LIST        0x0C
#define WINDOW_ESCARGO_EXPRESS_ITEM   0x0D
#define WINDOW_TEXT_BATTLE            0x0E
#define WINDOW_PSI_CATEGORY           0x10
#define WINDOW_FILE_SELECT_MAIN       0x13
#define WINDOW_FILE_SELECT_MENU       0x14
#define WINDOW_FILE_SELECT_COPY_2     0x15
#define WINDOW_FILE_SELECT_COPY_1     0x16
#define WINDOW_FILE_SELECT_DELETE     0x17
#define WINDOW_FILE_SELECT_TEXT_SPEED 0x18
#define WINDOW_FILE_SELECT_MUSIC_MODE 0x19
#define WINDOW_FILE_SELECT_NAMING_BOX 0x1A
#define WINDOW_FILE_SELECT_NAMING_MSG 0x1B
#define WINDOW_FILE_SELECT_NAMING_KB  0x1C
#define WINDOW_FILE_SELECT_CONFIRM_NESS  0x1D
#define WINDOW_FILE_SELECT_CONFIRM_PAULA 0x1E
#define WINDOW_FILE_SELECT_CONFIRM_JEFF  0x1F
#define WINDOW_FILE_SELECT_CONFIRM_POO   0x20
#define WINDOW_FILE_SELECT_CONFIRM_KING  0x21
#define WINDOW_FILE_SELECT_CONFIRM_FOOD  0x22
#define WINDOW_FILE_SELECT_CONFIRM_THING 0x23
#define WINDOW_FILE_SELECT_CONFIRM_MSG   0x24
#define WINDOW_BATTLE_ACTION_NAME     0x26
#define WINDOW_TARGETING_PROMPT       0x28
#define WINDOW_EQUIPMENT_STATS        0x2D
#define WINDOW_STATUS_PSI_CATEGORY    0x2E
#define WINDOW_PSI_DESCRIPTION        0x2F
#define WINDOW_OVERWORLD_CHAR_SELECT  0x2C
#define WINDOW_BATTLE_TARGET_TEXT     0x31
#define WINDOW_FILE_SELECT_FLAVOUR    0x32
#define WINDOW_NAMING_PROMPT           0x27
#define WINDOW_SINGLE_CHARACTER_SELECT 0x33

/* Menu option - matches menu_option from structs.asm (45 bytes in asm) */
#define MENU_LABEL_SIZE 26  /* asm: 25-byte label at offset 19, +1 for null */
typedef struct {
    char     label[MENU_LABEL_SIZE]; /* display text (copied inline, asm: 25-byte at offset 19) */
    uint16_t userdata;     /* value returned on selection (asm offset 12) */
    uint8_t  text_x;       /* x position in window tiles (asm offset 8) */
    uint8_t  text_y;       /* y position in window tiles (asm offset 10) */
    uint8_t  type;         /* asm offset 0: 1=counted, 2=userdata return */
    uint8_t  page;         /* asm offset 6: submenu page number */
    uint8_t  sound_effect; /* asm offset 14: SFX on select */
    uint8_t  pixel_align;  /* asm offset 44: USA only, pixel alignment */
    uint32_t script;       /* asm offset 15: SNES far ptr to text script, invoked on hover
                            * (selection_menu calls DISPLAY_TEXT when cursor lands on option) */
} MenuItem;

/* Window title size (asm: 22 bytes for US, 16 for JP) */
#define WINDOW_TITLE_SIZE 23  /* 22 chars + null terminator */

/* Window info - matches window_stats from structs.asm (82 bytes in asm) */
typedef struct {
    bool     active;
    uint8_t  id;                    /* asm offset 4 */
    uint8_t  x, y;                  /* position in tiles (asm offset 6/8) */
    uint8_t  content_x, content_y;  /* content origin = (x+1, y+1), inside border */
    uint8_t  width, height;         /* size in tiles (asm offset 10/12) */
    uint8_t  text_x, text_y;        /* text cursor position within window (asm offset 14/16) */
    uint16_t cursor_pixel_x;        /* VWF pixel X position (C port only, tracks assembly VWF_X) */
    uint8_t  number_padding;        /* asm offset 18: digit padding for numbers */
    uint16_t curr_tile_attributes;  /* asm offset 19: palette/priority for text tiles */
    uint8_t  font;                  /* asm offset 21: font ID per window */
    uint32_t working_memory;        /* asm offset 23: text system working memory */
    uint32_t argument_memory;       /* asm offset 27: text system argument memory */
    uint16_t secondary_memory;      /* asm offset 31: text system secondary memory */
    uint32_t working_memory_storage;  /* asm offset 33: backup of working_memory */
    uint32_t argument_memory_storage; /* asm offset 37: backup of argument_memory */
    uint16_t secondary_memory_storage;/* asm offset 41: backup of secondary_memory */
    uint8_t  selected_option;       /* asm offset 47: persists across menu re-entries */
    uint8_t  menu_page_number;      /* asm offset 51: current submenu page */
    uint16_t tilemap_address;       /* asm offset 53: per-window tilemap offset */
    void   (*cursor_move_callback)(uint16_t value); /* asm offset 55: called on cursor movement
                                                       asm passes userdata (type 2) or index+1 (type 1) */
    uint8_t  title_slot;              /* asm offset 59: TITLED_WINDOWS slot index (1-5), 0=none */
    uint8_t  title_tile_count;        /* number of VWF tile columns rendered for title (set once at set_window_title time) */
    char     title[WINDOW_TITLE_SIZE]; /* asm offset 60: window title text (tiny font) */
    uint8_t   palette_index;
    MenuItem  menu_items[24];
    uint8_t   menu_count;
    uint8_t   current_option;       /* asm offset 43 */
    uint16_t *content_tilemap;      /* pointer into shared tilemap pool */
    uint16_t  content_tilemap_size; /* allocated entries in pool */
} WindowInfo;

/* Forward declarations for struct fields. */
#define HPPP_DIGIT_COUNT  3   /* HPPP_WINDOW_WIDTH - 4 */
#define HPPP_MAX_PARTY    4   /* maximum party members with HPPP windows */

typedef struct {
    uint16_t hp1[HPPP_DIGIT_COUNT];  /* offset 0:  HP top-row tiles */
    uint16_t hp2[HPPP_DIGIT_COUNT];  /* offset 6:  HP bottom-row tiles */
    uint16_t pp1[HPPP_DIGIT_COUNT];  /* offset 12: PP top-row tiles */
    uint16_t pp2[HPPP_DIGIT_COUNT];  /* offset 18: PP bottom-row tiles */
} HPPPWindowBuffer;  /* 24 bytes, matches sizeof(hp_pp_window_buffer) */

/* BG2 tile usage tracking dimensions. */
#define USED_BG2_TILE_MAP_SIZE 32

/* BG2_BUFFER size. */
#define BG2_BUFFER_SIZE 0x800

/* Maximum titled windows (assembly: TITLED_WINDOWS, 5 slots for US). */
#define MAX_TITLED_WINDOWS 5

/* Consolidated window system runtime state. */
typedef struct {
    /* Focus window ID (WINDOW_ID_NONE = no focus). */
    uint8_t  current_focus_window;

    /* RAM shadow of BG3/text layer tilemap (32x32 = 0x800 bytes).
     * Accessed as uint16_t* for tilemap entry writes — must be 2-byte
     * aligned (Cortex-M0+ faults on unaligned halfword access). */
    uint8_t __attribute__((aligned(4))) bg2_buffer[BG2_BUFFER_SIZE];

    /* Which party member column has the battle menu indicator. -1 = none. */
    int16_t battle_menu_current_character_id;

    /* Set when HPPP tilemap in bg2_buffer needs re-upload to VRAM. */
    uint16_t hppp_meter_area_needs_update;

    /* HP/PP digit tile buffers for each party member. */
    HPPPWindowBuffer hppp_window_buffer[HPPP_MAX_PARTY];

    /* [hundreds, tens, ones] scratch buffer for digit separation. */
    uint8_t hppp_window_digit_buffer[3];

    /* When set, UPDATE_HPPP_METER_TILES skips individual VRAM writes. */
    uint8_t upload_hppp_meter_tiles;

    /* BG2 tile usage bitmask (32 x 16-bit). */
    uint16_t used_bg2_tile_map[USED_BG2_TILE_MAP_SIZE];

    /* Active windows array. */
    WindowInfo windows[MAX_WINDOWS];

    /* Shared tilemap pool for all window content_tilemap buffers.
     * Each window gets a slice via tilemap_pool_alloc/free. */
    uint16_t tilemap_pool[WINDOW_TILEMAP_POOL_SIZE];

    /* Title slot tracking: each element is a window ID, or WINDOW_ID_NONE for empty. */
    uint8_t  titled_windows[MAX_TITLED_WINDOWS];

    /* Menu selection backup state (for returning to previous menu position).
     * Mirrors WRAM: RESTORE_MENU_BACKUP, MENU_BACKUP_SELECTED_TEXT_X/Y,
     * MENU_BACKUP_CURRENT_OPTION, MENU_BACKUP_SELECTED_OPTION. */
    uint8_t  restore_menu_backup;       /* non-zero → selection_menu restores cursor */
    uint16_t menu_backup_text_x;
    uint16_t menu_backup_text_y;
    uint16_t menu_backup_current_option;
    uint16_t menu_backup_selected_option;
} WindowSystemState;

extern WindowSystemState win;

/* SET_WINDOW_FOCUS — Port of asm/text/set_window_focus.asm.
 * Sets the current focus window to the given window ID. */
void set_window_focus(uint16_t window_id);

/* Initialize the window system */
void window_system_init(void);

/* Create a window. Returns pointer to window info. */
WindowInfo *create_window(uint16_t window_id);

/* Close a window by ID */
void close_window(uint16_t window_id);

/* Close all open windows */
void close_all_windows(void);

/* Close the currently focused window */
void close_focus_window(void);

/* Get window by ID (NULL if not open) */
WindowInfo *get_window(uint16_t window_id);

/* Set text cursor position within focus window */
void set_focus_text_cursor(uint16_t x, uint16_t y);

/* SET_WINDOW_TITLE: Port of asm/text/set_window_title.asm.
 * Copies a title string (up to max_len chars) into the window's title buffer.
 * The title is rendered in tiny font on the window's top border row.
 * Pass max_len == -1 to copy the full string (up to WINDOW_TITLE_SIZE-1).
 * Assembly: A=window_id, X=max_chars, PARAM00=title_pointer. */
void set_window_title(uint16_t window_id, const char *title, int max_len);

/* SET_FILE_SELECT_TEXT_HIGHLIGHT: apply/clear highlight palette on a menu item's
 * tilemap entries. Used by display-only file select menus. */
void highlight_menu_item(WindowInfo *w, uint16_t item_index, uint16_t palette, bool set);

/* Add a menu item to the focus window */
void add_menu_item(const char *label, uint16_t userdata, uint16_t text_x, uint16_t text_y);

/* PRINT_MENU_ITEMS — Port of asm/text/print_menu_items.asm.
 * Iterates the focus window's menu items and prints each label
 * at its text_x/text_y position. Called after add_menu_item() when
 * items already have explicit positions set (unlike open_window_and_print_menu
 * which auto-layouts first). */
void print_menu_items(void);

/* Run a selection menu in the focus window.
   allow_cancel: if non-zero, B button returns 0.
   Returns selected menu item's userdata, or 0 if cancelled. */
uint16_t selection_menu(uint16_t allow_cancel);

/* CHAR_SELECT_PROMPT — Port of asm/text/character_select_prompt.asm.
 * Creates a window listing party member names and runs selection_menu.
 * Returns 1-based character ID of the selected party member, or 0 if cancelled.
 *
 * mode: 1 = overworld-style (window with names), other = battle-style (HPPP).
 * allow_cancel: 1 = B/SELECT cancels.
 *
 * on_change: called with party member ID when selection changes. NULL = none.
 * check_valid: called with party member ID; returns non-zero if valid. NULL = all valid. */
uint16_t char_select_prompt(uint16_t mode, uint16_t allow_cancel,
                            void (*on_change)(uint16_t),
                            uint16_t (*check_valid)(uint16_t));

/* CLEAR_FOCUS_WINDOW_MENU_OPTIONS: Port of asm/text/window/clear_focus_window_menu_options.asm.
 * Resets the menu item count of the focus window to 0. */
void clear_focus_window_menu_options(void);

/* GET_WINDOW_MENU_OPTION_COUNT: Port of asm/text/window/get_window_menu_option_count.asm.
 * Returns the number of menu items in the specified window. */
uint16_t get_window_menu_option_count(uint16_t window_id);

/* Check if any window is currently open (WINDOW_HEAD != -1) */
bool any_window_open(void);

/* Render all open windows to the PPU BG2 layer */
void render_all_windows(void);

/* ACTIVE_HPPP_WINDOW_Y_OFFSET: tilemap row where the HP/PP window starts.
 * From include/config.asm: .DEFINE ACTIVE_HPPP_WINDOW_Y_OFFSET 18 */
#define ACTIVE_HPPP_WINDOW_Y_OFFSET 18

/* NORMAL_HPPP_WINDOW_Y_OFFSET: tilemap row for non-active character HP/PP windows.
 * From include/config.asm: .DEFINE NORMAL_HPPP_WINDOW_Y_OFFSET 19 */
#define NORMAL_HPPP_WINDOW_Y_OFFSET 19

/* CLEAR_HPPP_WINDOW_HEADER: Port of asm/battle/clear_hppp_window_header.asm.
 * Clears 4 tilemap entries in BG2_BUFFER at the HPPP window header position. */
void clear_hppp_window_header(void);

/* RENDER_HPPP_WINDOW_HEADER: Port of asm/battle/render_hppp_window_header.asm.
 * Copies 4 header tile entries into BG2_BUFFER at the HPPP window header position. */
void render_hppp_window_header(void);

/* COPY_HPPP_WINDOW_TO_VRAM: Port of asm/text/hp_pp_window/copy_hppp_window_to_vram.asm.
 * Copies the HPPP window rows from BG2_BUFFER to VRAM at TEXT_LAYER_TILEMAP. */
void copy_hppp_window_to_vram(void);

/* UPLOAD_BATTLE_SCREEN_TO_VRAM: Port of asm/battle/upload_battle_screen_to_vram.asm.
 * Copies the first 0x700 bytes of BG2_BUFFER (28 rows) to VRAM at TEXT_LAYER_TILEMAP,
 * then fills 0x40 bytes (1 row) with zeros (blank tiles). */
void upload_battle_screen_to_vram(void);

/* HPPP window dimensions from include/config.asm */
#define HPPP_WINDOW_HEIGHT 8
#define HPPP_WINDOW_WIDTH  7

/* HPPP horizontal centering tile: center of the 32-tile BG3 tilemap (16),
 * shifted right by VIEWPORT_PAD_LEFT/8 so the windows appear more centered
 * in the expanded viewport during battle. At native 256px this is 16.
 * Clamped to 18 so 4 party members (28 tiles) still fit in the 32-tile tilemap. */
#define HPPP_CENTER_TILE_IDEAL (16 + VIEWPORT_PAD_LEFT / 8)
#define HPPP_CENTER_TILE_MAX   (32 - (4 * HPPP_WINDOW_WIDTH) / 2)
#define HPPP_CENTER_TILE (HPPP_CENTER_TILE_IDEAL < HPPP_CENTER_TILE_MAX \
                          ? HPPP_CENTER_TILE_IDEAL : HPPP_CENTER_TILE_MAX)

/* SELECT_BATTLE_MENU_CHARACTER: Port of asm/battle/select_battle_menu_character.asm.
 * Shows the battle menu indicator below the HPPP window for the given party slot.
 * Clears the previous indicator if one was active. */
void select_battle_menu_character(uint16_t party_slot);

/* CLEAR_BATTLE_MENU_CHARACTER_INDICATOR: Port of asm/text/clear_battle_menu_character_indicator.asm.
 * Clears the battle menu indicator at the top of the HPPP window for the
 * currently selected character, then resets battle_menu_current_character_id to -1. */
void clear_battle_menu_character_indicator(void);

/* UNDRAW_HP_PP_WINDOW: Port of asm/text/hp_pp_window/undraw.asm.
 * Clears the HP/PP window tiles for the given party slot from bg2_buffer.
 * Clears the corresponding bit in currently_drawn_hppp_windows. */
void undraw_hp_pp_window(uint16_t char_id);

/* GET_EQUIP_WINDOW_TEXT: Port of asm/battle/get_equip_window_text.asm.
 * Scans afflictions[7] for the first non-zero entry (priority: slot 0, slot 3,
 * then slots 1-6). Looks up the tile index from STATUS_EQUIP_WINDOW_TEXT tables.
 *   mode=1: uses TEXT_1 table (status icon tile for HP/PP window)
 *   mode=0: uses TEXT_2 table (status icon tile for other contexts)
 * Returns default (7 for mode!=0, 32 for mode==0) if no affliction found. */
uint16_t get_equip_window_text(const uint8_t *afflictions, int mode);

/* GET_EQUIP_WINDOW_TEXT_ALT: Port of asm/battle/get_equip_window_text_alt.asm.
 * Same scan as GET_EQUIP_WINDOW_TEXT. Returns a palette multiplier from TEXT_3.
 * Returns 4 if no affliction found (4 * 0x0400 = 0x1000 = palette 4). */
uint16_t get_equip_window_text_alt(const uint8_t *afflictions);

/* SET_WINDOW_PALETTE_INDEX: Port of asm/text/window/set_window_palette_index.asm.
 * Sets curr_tile_attributes = palette_index * 1024 on the focus window. */
void set_window_palette_index(uint16_t palette_index);

/* PRINT_CHAR_WITH_SOUND: Port of asm/text/print_char_with_sound.asm.
 * Writes a tile-level character to the focus window, plays SFX, and delays. */
void print_char_with_sound(uint16_t tile_code);

/* DISPATCH_WINDOW_BORDER_ANIMATION: Port of asm/text/window/dispatch_window_border_animation.asm.
 * mode=1: animate border tiles (ROW1), mode=2: animate with HPPP updates. */
void dispatch_window_border_animation(uint16_t mode);

/* DRAW_HP_PP_WINDOW: Port of asm/text/hp_pp_window/draw.asm.
 * Draws the full HP/PP window (border, name, HP digits, PP digits) for
 * the given party slot into bg2_buffer. Near function (called from bank C2). */
void draw_hp_pp_window(uint16_t char_id);

/* DRAW_AND_MARK_HPPP_WINDOW: Port of asm/battle/draw_and_mark_hppp_window.asm.
 * Sets the drawn bit in currently_drawn_hppp_windows, calls draw_hp_pp_window,
 * then sets hppp_meter_area_needs_update. */
void draw_and_mark_hppp_window(uint16_t char_id);

/* DRAW_ACTIVE_HPPP_WINDOWS: Port of asm/battle/draw_active_hppp_windows.asm.
 * Iterates through party members and calls draw_hp_pp_window for each one
 * that has its bit set in currently_drawn_hppp_windows. */
void draw_active_hppp_windows(void);

/* HIDE_HPPP_WINDOWS: Port of asm/text/hide_hppp_windows.asm.
 * Clears the battle menu indicator, disables HPPP rendering, undraws all
 * windows, and snaps HP/PP to their target values if not in battle mode. */
void hide_hppp_windows(void);

/* SEPARATE_DECIMAL_DIGITS: Port of asm/text/hp_pp_window/separate_decimal_digits.asm.
 * Splits a 16-bit value into hundreds/tens/ones in hppp_window_digit_buffer. */
void separate_decimal_digits(uint16_t value);

/* FILL_HP_PP_TILE_BUFFER: Port of asm/text/hp_pp_window/fill_tile_buffer.asm.
 * Computes tilemap entries for a digit display.  Reads digits from
 * hppp_window_digit_buffer and writes to hppp_window_buffer[id].
 * mode: 0=HP, 1=PP.  fraction: roller animation value (16.16 low word). */
void fill_hp_pp_tile_buffer(uint16_t id, uint16_t mode, uint16_t fraction);

/* FILL_HP_PP_TILE_BUFFER_X: Port of asm/text/hp_pp_window/fill_tile_buffer_x.asm.
 * Writes "X" placeholder tiles into PP slots for a concentrated character. */
void fill_hp_pp_tile_buffer_x(uint16_t id);

/* FILL_CHARACTER_HP_TILE_BUFFER: Port of asm/text/hp_pp_window/fill_character_hp_tile_buffer.asm.
 * Wrapper: splits integer into digits, then fills HP tile buffer. */
void fill_character_hp_tile_buffer(uint16_t id, uint16_t integer, uint16_t fraction);

/* FILL_CHARACTER_PP_TILE_BUFFER: Port of asm/text/hp_pp_window/fill_character_pp_tile_buffer.asm.
 * Checks concentration status; if concentrated, fills with X tiles.
 * Otherwise splits integer into digits and fills PP tile buffer. */
void fill_character_pp_tile_buffer(uint16_t id, uint8_t *afflictions,
                                   uint16_t integer, uint16_t fraction);

/* UPDATE_HPPP_METER_AND_RENDER: Port of asm/text/hp_pp_window/update_hppp_meter_and_render.asm (16 lines).
 * Per-frame HPPP update orchestrator: runs HP_PP_ROLLER, conditionally copies
 * bg2_buffer to VRAM, runs UPDATE_HPPP_METER_TILES, then RENDER_FRAME_TICK. */
void update_hppp_meter_and_render(void);

/* UPDATE_HPPP_METER_TILES: Port of asm/text/update_hppp_meter_tiles.asm (325 lines).
 * Each frame, selects one party member (FRAME_COUNTER & 3), recomputes their
 * HP/PP digit tiles via fill_character_hp/pp_tile_buffer, and writes the
 * resulting tilemap entries to bg2_buffer (and optionally to VRAM directly). */
void update_hppp_meter_tiles(void);

/* INIT_USED_BG2_TILE_MAP: Port of asm/system/dma/init_used_bg2_tile_map.asm.
 * Resets the BG2 tile usage map to its default state.
 * Called at boot (main.asm) and when reinitializing the text system. */
void init_used_bg2_tile_map(void);

/* ALLOC_BG2_TILEMAP_ENTRY: Port of asm/system/dma/alloc_bg2_tilemap_entry.asm.
 * Allocates a free BG2 tile from used_bg2_tile_map.
 * Returns tile index (0-511). On exhaustion: emergency recovery (close all windows). */
uint16_t alloc_bg2_tilemap_entry(void);

/* FREE_TILE: Port of asm/text/free_tile.asm.
 * Marks a BG2 tile as free in used_bg2_tile_map (clears the bit).
 * Skips locked tiles (system tiles like borders, cursors, fonts). */
void free_tile(uint16_t tile_entry);

/* FREE_TILE_SAFE: Port of asm/text/free_tile_safe.asm.
 * Like free_tile but skips tile 0 (transparent) and tile 64 (blank fill). */
void free_tile_safe(uint16_t tile_entry);

/* SCROLL_WINDOW_UP: Port of asm/text/window/scroll_window_up.asm.
 * Scrolls window content up by one text line (2 tile rows / 16px).
 * Frees top-row tiles, shifts content_tilemap entries up, fills bottom row.
 * Called by print_newline() when text_y reaches the last interior line. */
void scroll_window_up(WindowInfo *w);

/* FREE_WINDOW_TEXT_ROW: Port of asm/text/window/free_window_text_row.asm.
 * Frees tiles on the current text row and fills with blank tiles (0). */
void free_window_text_row(WindowInfo *w);

/* ADD_MENU_ITEM_NO_POSITION: Port of asm/text/menu/add_menu_item_no_position.asm.
 * Wraps add_menu_item() with auto-computed positions.
 * Position is set by open_window_and_print_menu() later.
 * Sets type=2 (userdata return). */
void add_menu_item_no_position(const char *label, uint16_t userdata);

/* OPEN_WINDOW_AND_PRINT_MENU: Port of asm/text/window/open_window_and_print_menu.asm.
 * Calls LAYOUT_MENU_OPTIONS to arrange items in columns, then prints them.
 * columns: number of columns for layout.
 * start_index: 1-based starting index for userdata assignment (0 = don't reassign). */
void open_window_and_print_menu(uint16_t columns, uint16_t start_index);

/* LAYOUT_AND_PRINT_MENU_AT_SELECTION: Port of asm/text/menu/layout_and_print_menu_at_selection.asm.
 * Like open_window_and_print_menu, but also sets initial cursor position.
 * initial_selection: 0-based index, or (uint16_t)-1 for no pre-selection. */
void layout_and_print_menu_at_selection(uint16_t columns, uint16_t start_index,
                                        uint16_t initial_selection);

/* BACKUP_SELECTED_MENU_OPTION: Port of asm/misc/backup_selected_menu_option.asm.
 * Saves the current focus window's cursor/selection state to global
 * backup variables. Used by Goods menu to remember position. */
void backup_selected_menu_option(void);

/* DISPLAY_MENU_HEADER_TEXT: Port of asm/text/menu/display_menu_header_text.asm.
 * Creates WINDOW::TARGETING_PROMPT and prints a targeting prompt.
 * text_index: 0="Who?", 1="Which?", 2="Where?", 3="Whom?", 4="Where?" */
void display_menu_header_text(uint16_t text_index);

/* CLOSE_MENU_HEADER_WINDOW: Port of asm/text/window/close_menu_header_window.asm.
 * Closes the WINDOW::TARGETING_PROMPT window. */
void close_menu_header_window(void);

/* SAVE_WINDOW_TEXT_ATTRIBUTES / RESTORE_WINDOW_TEXT_ATTRIBUTES:
 * Ports of asm/battle/save_window_text_attributes.asm and C20ABC.asm.
 * Saves/restores the current focus window's text cursor state. */
void save_window_text_attributes(void);
void restore_window_text_attributes(void);

/* SET_CURSOR_MOVE_CALLBACK: Port of asm/text/menu/set_cursor_move_callback.asm.
 * Sets the cursor_move_callback for the current focus window.
 * The callback receives the selected item's userdata (type 2) or
 * index+1 (type 1) when the cursor moves. */
void set_cursor_move_callback(void (*cb)(uint16_t));

/* CLEAR_CURSOR_MOVE_CALLBACK: Port of asm/text/menu/clear_cursor_move_callback.asm.
 * Clears the cursor_move_callback for the current focus window. */
void clear_cursor_move_callback(void);

/* CLEAR_WINDOW_TILEMAP: Port of asm/text/clear_window_tilemap.asm.
 * Clears the window's tilemap area, freeing tiles and filling with blank.
 * Sets redraw_all_windows=1 and calls clear_party_sprite_hide_flags(). */
void clear_window_tilemap(uint16_t window_id);

/* INITIALIZE_WINDOW_FLAVOUR_PALETTE: Port of asm/text/window/initialize_window_flavour_palette.asm.
 * Resets all party characters' hp_pp_window_options to 0x0400 (normal),
 * then loads sub-palette 5 (offset +40) from the current text flavour's
 * palette data into palette sub-palette 3 (colors 12-15). */
void initialize_window_flavour_palette(void);

/* RESET_HPPP_OPTIONS_AND_LOAD_PALETTE: Port of asm/text/hp_pp_window/reset_hppp_options_and_load_palette.asm.
 * Same as initialize_window_flavour_palette but loads sub-palette 3
 * (offset +24) instead of sub-palette 5 (offset +40). */
void reset_hppp_options_and_load_palette(void);

/* SET_HPPP_WINDOW_MODE_ITEM: Port of asm/text/set_hppp_window_mode_item.asm.
 * For each party character, checks if the given item is usable/equippable,
 * compares stats against currently equipped item, and sets
 * hp_pp_window_options to reflect the comparison:
 *   0x0400 = normal, 0x0C00 = greyed/unusable, 0x1400 = stat improvement. */
void set_hppp_window_mode_item(uint16_t item_id);

/* INVENTORY_GET_ITEM_NAME: Port of asm/misc/inventory_get_item_name.asm.
 * Populates a window with the character's inventory items as menu options.
 * Creates the given window, adds each non-empty inventory slot as a menu item,
 * with an equipped marker prefix for equipped items.
 * char_id: 1-indexed character ID.
 * window_type: window ID to create (e.g., WINDOW_INVENTORY = 0x02). */
void inventory_get_item_name(uint16_t char_id, uint16_t window_type);

/* PRINT_MONEY_IN_WINDOW: Port of asm/text/window/print_money_in_window.asm.
 * Prints a right-aligned dollar amount ("$NNN") in the current focus window.
 * The dollar sign and digits are positioned to right-align against the
 * window's right edge. */
void print_money_in_window(uint32_t amount);

#endif /* GAME_WINDOW_H */
