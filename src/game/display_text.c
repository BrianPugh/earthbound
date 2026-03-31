/*
 * DISPLAY_TEXT — text bytecode interpreter.
 *
 * Port of asm/text/display_text.asm.
 *
 * Implements the core bytecode loop, printable character rendering,
 * script flow control (JUMP/CALL/conditional branching), text memory
 * system, and all CC handlers (0x00-0x1F tree dispatchers).
 *
 * Argument byte counts verified against include/textmacros.asm.
 */

#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include "game/text.h"
#include "game/window.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "game/position_buffer.h"
#include "game/audio.h"
#include "game/battle.h"
#include "game/fade.h"
#include "intro/title_screen.h"
#include "intro/file_select.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/assets.h"
#include "include/binary.h"
#include "include/constants.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "core/math.h"
#include "core/decomp.h"
#include "game/battle_bg.h"
#include "game/door.h"
#include "game/flyover.h"
#include "game/ending.h"
#include "game/town_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
#include "game_main.h"

/* Consolidated display text runtime state. */
DisplayTextState dt = {
    .attacker_enemy_id = -1,
    .target_enemy_id = -1,
};

/* Interacting NPC state lives in dt.current_interacting_event_flag. */

/* --- PSI teleport destination table ---
 * Loaded from psi_teleport_dest_table.bin (extracted from ROM).
 * Each entry is 31 bytes: name[25] + event_flag[2] + x[2] + y[2].
 * Matches struct ow.psi_teleport_destination from include/structs.asm. */
/* --- PSI ability / name / suffix tables ---
 * PSI_ABILITY_TABLE: 54 entries x 15 bytes (sizeof psi_ability).
 * PSI_NAME_TABLE: 17 entries x 25 bytes (EB-encoded padded names).
 * PSI_SUFFIXES: 5 x 2-byte null-terminated EB-encoded strings (α β γ Σ Ω). */
#define PSI_NAME_SIZE            25
#define PSI_NAME_MAX_ENTRIES     17
#define PSI_SUFFIX_COUNT          5
#define PSI_SUFFIX_ENTRY_SIZE     2
#define psi_ability_data       ((const PsiAbility *)ASSET_DATA(ASSET_DATA_PSI_ABILITY_TABLE_BIN))
#define psi_ability_data_size  ASSET_SIZE(ASSET_DATA_PSI_ABILITY_TABLE_BIN)
#define psi_name_data          ASSET_DATA(ASSET_DATA_PSI_NAME_TABLE_BIN)
#define psi_name_data_size     ASSET_SIZE(ASSET_DATA_PSI_NAME_TABLE_BIN)
#define psi_suffix_data        ASSET_DATA(ASSET_DATA_PSI_SUFFIXES_BIN)
#define psi_suffix_data_size   ASSET_SIZE(ASSET_DATA_PSI_SUFFIXES_BIN)

/* --- Compressed text dictionary (CC 0x15/0x16/0x17) ---
 * 767 null-terminated EB-encoded string fragments (e.g. " in the ", " that ").
 * The pointer table has 768 x 4-byte offset entries; each is a byte offset
 * into the data blob.  CC 0x15 uses entries 0-255, CC 0x16 uses 256-511,
 * CC 0x17 uses 512-767. */
#define COMPRESSED_TEXT_ENTRY_COUNT 768
#define compressed_text_data       ASSET_DATA(ASSET_DATA_COMPRESSED_TEXT_DATA_BIN)
#define compressed_text_data_size  ASSET_SIZE(ASSET_DATA_COMPRESSED_TEXT_DATA_BIN)
#define compressed_text_ptrs       ASSET_DATA(ASSET_DATA_COMPRESSED_TEXT_PTRS_BIN)
#define compressed_text_ptrs_size  ASSET_SIZE(ASSET_DATA_COMPRESSED_TEXT_PTRS_BIN)

/* New inline string table (from ebtools pack-all text round-trip tooling) */
static const uint8_t *inline_string_table = NULL;
static size_t inline_string_table_size = 0;

/* Flat dialogue blob — all compiled dialogue concatenated into one blob.
 * See resolve_text_addr() for the address range layout. */
#define DIALOGUE_BLOB_BASE 0x100000
static const uint8_t *dialogue_blob = NULL;
static size_t dialogue_blob_size = 0;

/* SNES address → dialogue blob offset remap table.
 * Sorted array of (snes_addr_u32, blob_offset_u32) pairs, binary-searched
 * at runtime. Used by callroutine_movement CC_0F which constructs SNES
 * addresses from binary ROM movement script data. */
static const uint8_t *addr_remap_data = NULL;
static size_t addr_remap_count = 0;

/* Binary search the remap table. Returns dialogue blob offset, or 0 on miss. */
static uint32_t remap_snes_to_blob(uint32_t snes_addr) {
    if (!addr_remap_data || addr_remap_count == 0) return 0;
    size_t lo = 0, hi = addr_remap_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t key = read_u32_le(&addr_remap_data[mid * 8]);
        if (key == snes_addr)
            return read_u32_le(&addr_remap_data[mid * 8 + 4]);
        if (key < snes_addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* HP/PP modification functions: now in game_state.c, declared in game_state.h.
 * heal_character_hp/pp, reduce_hp/pp_target, recover/reduce_hp/pp_amtpercent. */

/* dt.attacker_enemy_id, dt.target_enemy_id, print_attacker/target_article,
 * text register backups, dt.g_cc18_attrs_saved, dt.window_text_attrs_backup
 * all now in DisplayTextState dt. */

/* Save current focus window's text attributes to backup.
 * Port of SAVE_WINDOW_TEXT_ATTRIBUTES (asm/battle/save_window_text_attributes.asm). */
void save_window_text_attributes(void) {
    dt.window_text_attrs_backup.id = win.current_focus_window;
    if (win.current_focus_window == WINDOW_ID_NONE) {
        dt.window_text_attrs_backup.valid = true;
        return;
    }
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w) {
        dt.window_text_attrs_backup.valid = true;
        return;
    }
    dt.window_text_attrs_backup.text_x = w->text_x;
    dt.window_text_attrs_backup.text_y = w->text_y;
    dt.window_text_attrs_backup.cursor_pixel_x = w->cursor_pixel_x;
    dt.window_text_attrs_backup.number_padding = w->number_padding;
    dt.window_text_attrs_backup.curr_tile_attributes = w->curr_tile_attributes;
    dt.window_text_attrs_backup.font = w->font;
    dt.window_text_attrs_backup.valid = true;
}

/* Restore focus window's text attributes from backup.
 * Port of RESTORE_WINDOW_TEXT_ATTRIBUTES (asm/battle/restore_window_text_attributes.asm). */
void restore_window_text_attributes(void) {
    if (!dt.window_text_attrs_backup.valid)
        return;
    uint16_t saved_id = dt.window_text_attrs_backup.id;
    if (saved_id == WINDOW_ID_NONE)
        return;
    WindowInfo *w = get_window(saved_id);
    if (!w)
        return;
    win.current_focus_window = saved_id;
    w->text_x = dt.window_text_attrs_backup.text_x;
    w->text_y = dt.window_text_attrs_backup.text_y;
    w->cursor_pixel_x = dt.window_text_attrs_backup.cursor_pixel_x;
    w->number_padding = dt.window_text_attrs_backup.number_padding;
    w->curr_tile_attributes = dt.window_text_attrs_backup.curr_tile_attributes;
    w->font = dt.window_text_attrs_backup.font;
}

/* --- Text memory helpers ---
 * Each window has its own working_memory, argument_memory, and secondary_memory.
 * These helper functions access the focused window's memory.
 *
 * DUMMY_WINDOW: When no window is open, the assembly's GET_ACTIVE_WINDOW_ADDRESS
 * returns a pointer to DUMMY_WINDOW — a full window_stats structure in RAM that
 * always exists. This allows CC codes like CHECK_EVENT_FLAG + JUMP_IF_FALSE to
 * work in windowless text scripts (e.g., TEXT_DOOR_043 which never opens a window).
 * Without this fallback, set_working_memory() silently drops the value and
 * get_working_memory() always returns 0, causing JUMP_IF_FALSE to always jump. */
static WindowInfo dummy_window;

WindowInfo *get_focus_window_info(void) {
    if (win.current_focus_window == WINDOW_ID_NONE) return &dummy_window;
    return get_window(win.current_focus_window);
}

uint32_t get_working_memory(void) {
    WindowInfo *w = get_focus_window_info();
    return w ? w->working_memory : 0;
}

void set_working_memory(uint32_t val) {
    WindowInfo *w = get_focus_window_info();
    if (w) w->working_memory = val;
}

uint32_t get_argument_memory(void) {
    WindowInfo *w = get_focus_window_info();
    return w ? w->argument_memory : 0;
}

void set_argument_memory(uint32_t val) {
    WindowInfo *w = get_focus_window_info();
    if (w) w->argument_memory = val;
}

uint16_t get_secondary_memory(void) {
    WindowInfo *w = get_focus_window_info();
    return w ? w->secondary_memory : 0;
}

void set_secondary_memory(uint16_t val) {
    WindowInfo *w = get_focus_window_info();
    if (w) w->secondary_memory = val;
}

void increment_secondary_memory(void) {
    WindowInfo *w = get_focus_window_info();
    if (w) w->secondary_memory++;
}

/* --- Text utility functions --- */

void set_instant_printing(void) {
    /* Port of SET_INSTANT_PRINTING (asm/text/set_instant_printing.asm). */
    dt.instant_printing = 1;
}

void clear_instant_printing(void) {
    /* Port of CLEAR_INSTANT_PRINTING (asm/text/clear_instant_printing.asm). */
    dt.instant_printing = 0;
}

/* Pagination arrow animation tilemap entries (from asm/data/unknown/C3E41C.asm).
 * 4 frames × 4 tilemap words. Rendered at the right edge of dt.pagination_window. */
static const uint16_t pagination_tiles[4][4] = {
    { 0x3C16, 0x2E6D, 0x2E6E, 0x7C16 },  /* frame 0: both arrows */
    { 0x3C16, 0x2E7D, 0x2E7E, 0x7C16 },  /* frame 1: both arrows (alt) */
    { 0x3C16, 0x2E6D, 0x2C40, 0x7C16 },  /* frame 2: left arrow only */
    { 0x3C16, 0x2C40, 0x2E6E, 0x7C16 },  /* frame 3: right arrow only */
};

void render_pagination_arrows(void) {
    /* Port of assembly lines 173-228 of character_select_prompt.asm.
     * Writes 4 tilemap entries at the right edge of dt.pagination_window.
     *
     * Assembly uses PREPARE_VRAM_COPY (line 227) to write directly to VRAM
     * after window_tick has already uploaded bg2_buffer. We match this by
     * writing to ppu.vram — bg2_buffer would be overwritten by
     * render_all_windows() before the next upload. */
    if (dt.pagination_window == WINDOW_ID_NONE) return;

    WindowInfo *w = get_window(dt.pagination_window);
    if (!w) return;

    int frame = dt.pagination_animation_frame;
    if (frame < 0 || frame > 3) return;

    /* Calculate tilemap word offset (assembly lines 207-222):
     * window_y * 32 + window_x + width - 3 + VRAM::TEXT_LAYER_TILEMAP
     *
     * Assembly's window_stats::width = config_width - 2 (create_window.asm
     * lines 158-160 DEC twice), so subtract 2 from C port's w->width. */
    uint16_t tile_offset = (w->y * 32) + w->x + (w->width - 2) - 3;
    uint32_t vram_word_addr = VRAM_TEXT_LAYER_TILEMAP + tile_offset;

    /* Write 4 tilemap entries (8 bytes) directly to ppu.vram. */
    uint16_t *vram = (uint16_t *)&ppu.vram[vram_word_addr * 2];
    for (int i = 0; i < 4; i++) {
        vram[i] = pagination_tiles[frame][i];
    }
}

/* Port of CHECK_LAST_MEMBER_STATUS_CHANGE (asm/text/check_last_member_status_change.asm).
 * Returns 1 if the last party member's incapacitated state changed since last check.
 * Tracks state in dt.last_party_member_status_last_check. */
/* dt.last_party_member_status_last_check now in DisplayTextState dt. */

static uint16_t check_last_member_status_change(void) {
    uint8_t count = game_state.player_controlled_party_count;
    if (count == 0) return 0;

    /* Assembly: get last party member's char_id, check afflictions[0] */
    uint8_t char_id = game_state.player_controlled_party_members[count - 1];
    uint8_t status = party_characters[char_id].afflictions[0]; /* PERSISTENT_EASYHEAL */

    uint16_t incapacitated = 0;
    if (status == 1 /* UNCONSCIOUS */ || status == 2 /* DIAMONDIZED */)
        incapacitated = 1;

    uint16_t changed = (incapacitated != dt.last_party_member_status_last_check) ? 1 : 0;
    dt.last_party_member_status_last_check = incapacitated;
    return changed;
}

/* Port of WINDOW_TICK (asm/text/window_tick.asm).
 *
 * Per-frame orchestrator for the text/window system:
 *   1. RAND (advance PRNG)
 *   2. If dt.early_tick_exit, clear it and exit (assembly lines 5-10)
 *   3. If dt.instant_printing, exit (assembly lines 13-15)
 *   4. If ow.redraw_all_windows, render all; else if windows open, render tail
 *   5. HP/PP roller + meter tile updates
 *   6. If !ow.disabled_transitions && status changed, reload palette
 *   7. upload_battle_screen_to_vram() — sync win.bg2_buffer to VRAM
 *   8. render_frame_tick() — process one frame
 */
void window_tick(void) {
    rng_next_byte();  /* Assembly line 4: JSL RAND (advance PRNG) */

    /* Assembly lines 5-10: early exit if flagged (set by print_menu_items) */
    if (dt.early_tick_exit) {
        dt.early_tick_exit = 0;
        return;
    }

    /* Assembly lines 13-15: skip rendering if dt.instant_printing */
    if (dt.instant_printing)
        return;

    /* Assembly lines 16-28: render windows into win.bg2_buffer.
     * Assembly: if ow.redraw_all_windows → RENDER_ALL_WINDOWS;
     *           else if WINDOW_HEAD != -1 → RENDER_WINDOW_FRAME(tail);
     *           else → skip to HP/PP.
     * C port: both paths call render_all_windows() which copies
     * per-window content_tilemap to win.bg2_buffer. Functionally equivalent
     * since VWF tiles are persistent in VRAM and content_tilemap. */
    if (ow.redraw_all_windows) {
        render_all_windows();
        ow.redraw_all_windows = 0;
    } else if (any_window_open()) {
        render_all_windows();
    }

    /* Assembly lines 29-34: HP/PP updates */
    hp_pp_roller();
    win.upload_hppp_meter_tiles = 1;
    update_hppp_meter_tiles();

    /* Assembly lines 35-41: only reload palette if last member's
     * incapacitated state actually changed (avoids redundant loads). */
    if (!ow.disabled_transitions) {
        if (check_last_member_status_change()) {
            load_character_window_palette();
        }
    }

    /* Assembly lines 42-45: sync win.bg2_buffer to VRAM, then render frame.
     * Pagination arrows are applied between upload and render to match SNES
     * DMA ordering: WINDOW_TICK's upload runs first, then PREPARE_VRAM_COPY
     * for pagination arrows executes during the same VBlank. */
    win.hppp_meter_area_needs_update = 0;
    upload_battle_screen_to_vram();
    render_pagination_arrows();
    render_frame_tick();
}

void window_tick_without_instant_printing(void) {
    /* Port of WINDOW_TICK_WITHOUT_INSTANT_PRINTING
     * (asm/text/window_tick_without_instant_printing.asm).
     * Temporarily disables instant printing, calls full WINDOW_TICK,
     * then re-enables instant printing. */
    dt.instant_printing = 0;
    window_tick();
    dt.instant_printing = 1;
}

void set_window_number_padding(uint16_t padding) {
    /* Port of SET_WINDOW_NUMBER_PADDING (asm/text/window/set_window_number_padding.asm).
     * Stores padding value (8-bit) to the focus window's number_padding field. */
    WindowInfo *w = get_focus_window_info();
    if (w) w->number_padding = (uint8_t)padding;
}

void set_text_pixel_position(uint16_t row, uint16_t pixel_pos) {
    /* Port of SET_TEXT_PIXEL_POSITION (asm/text/set_text_pixel_position.asm)
     * and SET_TEXT_CURSOR_WITH_PIXEL_OFFSET (asm/text/set_text_cursor_with_pixel_offset.asm).
     *
     * Assembly chain:
     *   1. NEW_TEXT_PIXEL_OFFSET = pixel_pos & 7
     *   2. tile_column = pixel_pos >> 3
     *   3. SET_TEXT_CURSOR_WITH_PIXEL_OFFSET(tile_column, row):
     *      a. SET_FOCUS_TEXT_CURSOR(tile_column, row) → ADVANCE_VWF_TILE + set text_x/y
     *      b. If pixel_offset != 0: VWF_X += offset, clear VWF ert.buffer at vwf_tile */
    uint8_t pixel_offset = pixel_pos & 7;
    uint16_t tile_column = pixel_pos >> 3;

    /* set_focus_text_cursor calls advance_vwf_tile() internally */
    set_focus_text_cursor(tile_column, row);

    /* Handle sub-tile pixel offset (SET_TEXT_CURSOR_WITH_PIXEL_OFFSET lines 16-32) */
    if (pixel_offset) {
        vwf_x += pixel_offset;
        /* Clear VWF ert.buffer at current tile (fill with 0xFF) */
        uint16_t buf_offset = vwf_tile * VWF_TILE_BYTES;
        memset(vwf_buffer + buf_offset, 0xFF, VWF_TILE_BYTES);
    }

    /* Override cursor_pixel_x to actual pixel position */
    WindowInfo *w = get_focus_window_info();
    if (w) w->cursor_pixel_x = pixel_pos;
}

/* --- Battle text accessors --- */

void set_battle_attacker_name(const char *src, uint16_t length) {
    /* Port of SET_BATTLE_ATTACKER_NAME (asm/battle/ui/set_battle_attacker_name.asm).
     * Copies length bytes from src, null-terminates, clears dt.attacker_enemy_id. */
    if (length >= BATTLE_NAME_ATTACKER_SIZE)
        length = BATTLE_NAME_ATTACKER_SIZE - 1;
    memcpy(dt.battle_attacker_name, src, length);
    dt.battle_attacker_name[length] = '\0';
    dt.attacker_enemy_id = -1;
}

void set_battle_target_name(const char *src, uint16_t length) {
    /* Port of SET_BATTLE_TARGET_NAME (asm/battle/ui/set_battle_target_name.asm). */
    if (length >= BATTLE_NAME_TARGET_SIZE)
        length = BATTLE_NAME_TARGET_SIZE - 1;
    memcpy(dt.battle_target_name, src, length);
    dt.battle_target_name[length] = '\0';
    dt.target_enemy_id = -1;
}

void set_attacker_enemy_id(int16_t id) { dt.attacker_enemy_id = id; }
void set_target_enemy_id(int16_t id) { dt.target_enemy_id = id; }
void set_print_attacker_article(uint8_t val) { dt.print_attacker_article = val; }
void set_print_target_article(uint8_t val) { dt.print_target_article = val; }

void set_cnum(uint32_t value) {
    /* Port of SET_CNUM (asm/text/set_cnum.asm). */
    dt.cnum = value;
}

uint32_t get_cnum(void) {
    /* Port of GET_CNUM (asm/text/get_cnum.asm). */
    return dt.cnum;
}

/* TextBlock — lightweight struct used by resolve_text_addr() to return
 * a data pointer + size to callers that need to compute remaining bytes. */
typedef struct {
    const uint8_t *data;
    size_t   size;
} TextBlock;


/* Resolve a text address to a byte pointer.
 *
 * Address ranges:
 *   0x000000 .. 0x0FFFFF  — inline string table (item/enemy descriptions)
 *   0x100000 .. 0xBFFFFF  — dialogue blob (compiled from YAML)
 *   0xC00000 .. 0xFFFFFF  — legacy SNES ROM address (remapped via addr_remap.bin)
 *
 * *out_block receives the data source for size-remaining calculation.
 * Returns NULL if the address cannot be resolved. */
static const uint8_t *resolve_text_addr(uint32_t addr, TextBlock **out_block) {
    static TextBlock dialogue_block = {0};
    static TextBlock string_table_block = {0};

    /* Legacy SNES address: remap to dialogue blob offset first */
    if (addr >= 0xC00000) {
        uint32_t remapped = remap_snes_to_blob(addr);
        if (remapped == 0) {
            fprintf(stderr, "WARN: resolve_text_addr: addr $%06X not in remap table\n", addr);
            return NULL;
        }
        addr = remapped;
    }

    /* Dialogue blob */
    if (addr >= DIALOGUE_BLOB_BASE && dialogue_blob != NULL) {
        uint32_t offset = addr - DIALOGUE_BLOB_BASE;
        if (offset < dialogue_blob_size) {
            dialogue_block.data = dialogue_blob;
            dialogue_block.size = dialogue_blob_size;
            if (out_block) *out_block = &dialogue_block;
            return dialogue_blob + offset;
        }
    }

    /* Inline string table */
    if (addr < DIALOGUE_BLOB_BASE && inline_string_table != NULL && addr < inline_string_table_size) {
        string_table_block.data = inline_string_table;
        string_table_block.size = inline_string_table_size;
        if (out_block) *out_block = &string_table_block;
        return inline_string_table + addr;
    }

    return NULL;
}

/* Print a PSI name given a PSI name ID.
 * Port of GET_PSI_NAME (asm/text/get_psi_name.asm).
 * psi_name_id==1 (ROCKIN) → uses favourite_thing from game_state.
 * Otherwise indexes PSI_NAME_TABLE by (psi_name_id - 1). */
void print_psi_name(uint16_t psi_name_id) {

    if (psi_name_id == 1) {
        /* PSI Rockin — print favourite_thing (EB-encoded in game_state, null-terminated) */
        int len = 0;
        while (len < (int)sizeof(game_state.favourite_thing) && game_state.favourite_thing[len] != 0x00) len++;
        print_eb_string(game_state.favourite_thing, len);
    } else if (psi_name_id > 1 && (psi_name_id - 1) < PSI_NAME_MAX_ENTRIES) {
        const uint8_t *entry = psi_name_data + (uint32_t)(psi_name_id - 1) * PSI_NAME_SIZE;
        int len = 0;
        while (len < PSI_NAME_SIZE && entry[len] != 0x00) len++;
        print_eb_string(entry, len);
    }
}

/* Print a full PSI ability name (name + suffix like "α").
 * Port of PRINT_PSI_ABILITY_NAME (asm/text/print_psi_ability_name.asm).
 * Looks up the ability in PSI_ABILITY_TABLE, prints the name via
 * print_psi_name, then prints the level suffix from PSI_SUFFIXES. */
void print_psi_ability_name(uint16_t ability_id) {

    if (ability_id >= PSI_MAX_ENTRIES)
        return;

    const PsiAbility *psi = &psi_ability_data[ability_id];
    uint8_t psi_name_id = psi->name;
    uint8_t psi_level = psi->level;

    /* Print the PSI name (e.g. "PSI Fire ") */
    print_psi_name(psi_name_id);

    /* Print the level suffix (α/β/γ/Σ/Ω) */
    if (psi_level >= 1 && psi_level <= PSI_SUFFIX_COUNT) {
        uint16_t suffix_offset = (uint16_t)(psi_level - 1) * PSI_SUFFIX_ENTRY_SIZE;
        /* Each suffix is a null-terminated EB-encoded string */
        const uint8_t *suffix = psi_suffix_data + suffix_offset;
        int len = 0;
        while (len < PSI_SUFFIX_ENTRY_SIZE && suffix[len] != 0x00) len++;
        print_eb_string(suffix, len);
    }
}

void get_psi_suffix_label(uint16_t ability_id, char *out, size_t out_size) {
    out[0] = '\0';

    if (ability_id >= PSI_MAX_ENTRIES || out_size < 2)
        return;
    const PsiAbility *psi = &psi_ability_data[ability_id];
    uint8_t level = psi->level;
    if (level >= 1 && level <= PSI_SUFFIX_COUNT) {
        uint8_t eb_char = psi_suffix_data[(level - 1) * PSI_SUFFIX_ENTRY_SIZE];
        if (eb_char != 0x00) {
            out[0] = eb_char_to_ascii(eb_char);
            out[1] = '\0';
            return;
        }
    }
}

void display_text_from_addr(uint32_t addr) {
    TextBlock *blk = NULL;
    const uint8_t *ptr = resolve_text_addr(addr, &blk);
    if (ptr && blk) {
        size_t remaining = blk->size - (size_t)(ptr - blk->data);
        display_text(ptr, remaining);
    } else {
        fprintf(stderr, "WARNING: resolve_text_addr(0x%06X) returned NULL\n", addr);
    }
}


static bool display_text_load_inline_strings(void) {
#ifdef ASSET_TEXT_INLINE_STRINGS_BIN_DATA
    inline_string_table = ASSET_TEXT_INLINE_STRINGS_BIN_DATA;
    inline_string_table_size = ASSET_TEXT_INLINE_STRINGS_BIN_SIZE;
    return inline_string_table != NULL;
#else
    return true;  /* No string table in this build */
#endif
}

static bool display_text_load_dialogue_blob(void) {
#ifdef ASSET_DIALOGUE_DIALOGUE_BIN_DATA
    dialogue_blob = ASSET_DIALOGUE_DIALOGUE_BIN_DATA;
    dialogue_blob_size = ASSET_DIALOGUE_DIALOGUE_BIN_SIZE;
#endif
#ifdef ASSET_DIALOGUE_ADDR_REMAP_BIN_DATA
    addr_remap_data = ASSET_DIALOGUE_ADDR_REMAP_BIN_DATA;
    addr_remap_count = ASSET_DIALOGUE_ADDR_REMAP_BIN_SIZE / 8;
#endif
    return dialogue_blob != NULL;
}

bool display_text_load_eevent0(void) {
    /* All text data lives in the dialogue blob + inline string table.
     * Load both at startup. */
    bool ok = true;
    if (!display_text_load_inline_strings()) ok = false;
    if (!display_text_load_dialogue_blob()) ok = false;
    return ok;
}

/* Battle text is part of the dialogue blob — no separate loading needed. */
bool display_text_load_battle_text(void) { return true; }

/* No-ops: dialogue blob is always loaded, nothing to free. */
void display_text_free_eevent0(void) {}
void display_text_free_battle_text(void) {}



/* ScriptReader struct moved to display_text_internal.h */

uint8_t script_read_byte(ScriptReader *r) {
    /* Read from prefix ert.buffer first (compressed text dictionary insertion).
     * When a null terminator is hit, switch back to main stream.
     * Port of @LOCAL04 / @READ_NEXT_BYTE in display_text.asm. */
    if (r->prefix_ptr) {
        uint8_t b = *r->prefix_ptr;
        if (b != 0x00) {
            r->prefix_ptr++;
            return b;
        }
        r->prefix_ptr = NULL; /* prefix exhausted */
    }
    if (r->ptr >= r->end) return 0x02; /* END_BLOCK if past end */
    return *r->ptr++;
}

uint16_t script_read_word(ScriptReader *r) {
    uint16_t lo = script_read_byte(r);
    uint16_t hi = script_read_byte(r);
    return lo | (hi << 8);
}

uint32_t script_read_dword(ScriptReader *r) {
    uint32_t b0 = script_read_byte(r);
    uint32_t b1 = script_read_byte(r);
    uint32_t b2 = script_read_byte(r);
    uint32_t b3 = script_read_byte(r);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void script_skip(ScriptReader *r, int n) {
    for (int i = 0; i < n; i++) script_read_byte(r);
}

/* Resolve a text address and update the reader's stream pointers.
 * Used by CC_0A (JUMP), CC_06 (JUMP_IF_FLAG_SET), CC_09 (JUMP_MULTI),
 * CC_08 (CALL_TEXT), CC_1B_02/03 (conditional jumps). */
void resolve_text_jump(ScriptReader *r, uint32_t addr) {
    TextBlock *blk = NULL;
    const uint8_t *ptr = resolve_text_addr(addr, &blk);
    if (ptr && blk) {
        r->ptr = ptr;
        r->base = blk->data;
        r->end = blk->data + blk->size;
    } else {
        fprintf(stderr, "WARNING: resolve_text_addr(0x%06X) returned NULL\n", addr);
    }
}

/* --- Teleport destination loading --- */

#define teleport_table_data  ASSET_DATA(ASSET_DATA_TELEPORT_DESTINATION_TABLE_BIN)
#define teleport_table_size  ASSET_SIZE(ASSET_DATA_TELEPORT_DESTINATION_TABLE_BIN)

const TeleportDestination *get_teleport_dest(uint16_t index) {
    size_t offset = (size_t)index * sizeof(TeleportDestination);
    if (offset + sizeof(TeleportDestination) > teleport_table_size) return NULL;
    return (const TeleportDestination *)(teleport_table_data + offset);
}

/* --- Compressed text dictionary loading ---
 * Used by CC 0x15/0x16/0x17 to inline common text fragments. */

/* Look up a compressed text dictionary entry.
 * bank_offset: 0 for CC 0x15, 256 for CC 0x16, 512 for CC 0x17.
 * index: the byte read after the CC (0-255).
 * Returns pointer into compressed_text_data, or NULL on failure. */
const uint8_t *compressed_text_lookup(uint16_t bank_offset, uint8_t index) {
    uint16_t effective_index = bank_offset + index;
    if (effective_index >= COMPRESSED_TEXT_ENTRY_COUNT) return NULL;
    /* Read 4-byte offset from pointer table (byte offset into compressed_text_data) */
    uint32_t offset = read_u32_le(&compressed_text_ptrs[effective_index * 4]);
    if (offset >= compressed_text_data_size) return NULL;
    return compressed_text_data + offset;
}

/* Item configuration table is now in inventory.c / inventory.h.
 * Functions used: ensure_item_config(), get_item_entry(), read_u16_le().
 * Constants: ITEM_NAME_LEN, ItemConfig struct fields, etc. */

/* GET_ITEM_TYPE: now in inventory.c, declared in inventory.h */

/* CHECK_ITEM_USABLE_BY: now shared via inventory.h — see inventory.c. */

/* IS_ESCARGO_EXPRESS_FULL: Port of asm/text/is_escargo_express_full.asm.
 * Returns 0 if there is at least one empty slot, 1 if full.
 * Capacity = 36 minus count of non-zero unknownB6[0..2] entries. */
uint16_t is_escargo_express_full(void) {
    int capacity = 36;
    for (int i = 0; i < 3; i++) {
        if (game_state.unknownB6[i] != 0)
            capacity--;
    }
    for (int i = 0; i < capacity; i++) {
        if (game_state.escargo_express_items[i] == 0)
            return 0;
    }
    return 1;
}

/* CHECK_CHARACTER_IN_PARTY and CHECK_STATUS_GROUP: now in game_state.c,
 * declared in game_state.h. */

/* GET_ITEM_SUBTYPE_2: Port of src/inventory/get_item_subtype2.asm.
 * Reads item.type bits 2-3 (masked with 0x0C), maps:
 *   0x00 → 1, 0x0C → 1, 0x04 → 2, 0x08 → 3, other → 0. */
uint16_t get_item_subtype_2(uint16_t item_id) {
    const ItemConfig *entry = get_item_entry(item_id);
    if (!entry) return 0;
    uint8_t masked = entry->type & 0x0C;
    switch (masked) {
    case 0x00: return 1;
    case 0x0C: return 1;
    case 0x04: return 2;
    case 0x08: return 3;
    default:   return 0;
    }
}

/*
 * HP/PP flipout mode — backup arrays from ram.asm:1141-1144.
 *
 * When enabled (e.g., during Giygas battle), sets all PCs' HP to 999 and PP to 0,
 * backing up the originals. When disabled, restores the backups.
 * bt.hppp_meter_flipout_mode itself is defined in battle.c / battle.h.
 */
#define PLAYER_CHAR_COUNT 4
/* hppp_meter_flipout_mode backups now in DisplayTextState dt. */

/* HP/PP meter speed flags — now in BattleState (bt.half_hppp_meter_speed, bt.disable_hppp_rolling). */

/*
 * toggle_hppp_flipout_mode — Port of TOGGLE_HPPP_FLIPOUT_MODE (C12D17).
 *
 * When enabling (param != 0, currently off):
 *   Back up each PC's hp_target/pp_target, then set hp = hp_target = 999, pp = pp_target = 0.
 * When disabling (param == 0, currently on):
 *   Restore hp_target/pp_target from backups.
 * Always: update flipout state and clear meter speed flags (RESUME_MUSIC).
 */
void toggle_hppp_flipout_mode(uint16_t enable) {
    if (bt.hppp_meter_flipout_mode == 0 && enable != 0) {
        /* Save current hp_target/pp_target and override */
        for (int i = 0; i < PLAYER_CHAR_COUNT; i++) {
            dt.hppp_meter_flipout_mode_hp_backups[i] = party_characters[i].current_hp_target;
            party_characters[i].current_hp_target = 999;
            party_characters[i].current_hp = 999;
            dt.hppp_meter_flipout_mode_pp_backups[i] = party_characters[i].current_pp_target;
            party_characters[i].current_pp_target = 0;
            party_characters[i].current_pp = 0;
        }
    } else if (bt.hppp_meter_flipout_mode != 0 && enable == 0) {
        /* Restore hp_target/pp_target from backups */
        for (int i = 0; i < PLAYER_CHAR_COUNT; i++) {
            party_characters[i].current_hp_target = dt.hppp_meter_flipout_mode_hp_backups[i];
            party_characters[i].current_pp_target = dt.hppp_meter_flipout_mode_pp_backups[i];
        }
    }

    bt.hppp_meter_flipout_mode = enable;

    /* RESUME_MUSIC (asm/audio/resume_music.asm): clears meter speed flags */
    bt.half_hppp_meter_speed = 0;
    bt.disable_hppp_rolling = 0;
}

/* --- DISPLAY_STATUS_WINDOW ---
 * Port of asm/text/window/display_status_window.asm (292 lines).
 * Opens the Status window for a character, displaying stats, afflictions,
 * and a PSI info prompt.  Loaded text assets:
 *   - status_window_text.bin  : label template processed by display_text()
 *   - status_equip_text.bin   : affliction names (TEXT_5/6) + PSI info (TEXT_4)
 *
 * char_id is 1-based (1 = Ness, 2 = Paula, 3 = Jeff, 4 = Poo). */
#define WINDOW_STATUS_MENU 0x08
#define EXP_LIMIT          9999999u
#define AFFLICTION_TEXT_ENTRY_SIZE  16 /* bytes per PADDEDEBTEXT entry */
#define STATUS_TEXT4_OFFSET         0 /* PSI info prompt within status_equip_text */
#define STATUS_TEXT5_OFFSET        35 /* affliction names (9 entries × 16 bytes) */
#define STATUS_TEXT6_OFFSET       179 /* homesick text (16 bytes) */

static const uint8_t *status_window_text_data;
static size_t   status_window_text_size;
static const uint8_t *status_equip_text_data;

void display_status_window(uint16_t char_id) {
    uint16_t char_index = char_id - 1;
    if (char_index >= TOTAL_PARTY_COUNT) return;
    CharStruct *c = &party_characters[char_index];

    /* Lazy-load text assets */
    if (!status_window_text_data) {
        status_window_text_size = ASSET_SIZE(ASSET_DATA_STATUS_WINDOW_TEXT_BIN);
        status_window_text_data = ASSET_DATA(ASSET_DATA_STATUS_WINDOW_TEXT_BIN);
    }
    if (!status_equip_text_data)
        status_equip_text_data = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_TEXT_BIN);

    /* Assembly line 13: SET_INSTANT_PRINTING */
    set_instant_printing();

    /* Assembly line 14: CREATE_WINDOW_NEAR #WINDOW::STATUS_MENU */
    create_window(WINDOW_STATUS_MENU);

    /* Assembly line 15: WINDOW_TICK_WITHOUT_INSTANT_PRINTING */
    window_tick_without_instant_printing();

    /* Assembly lines 17-19: FORCE_LEFT_TEXT_ALIGNMENT = 1, then
     * DISPLAY_TEXT_PTR STATUS_WINDOW_TEXT → renders label strings,
     * then FORCE_LEFT_TEXT_ALIGNMENT = 0. */
    dt.force_left_text_alignment = 1;
    if (status_window_text_data)
        display_text(status_window_text_data, status_window_text_size);
    dt.force_left_text_alignment = 0;

    /* Assembly lines 20-26: pagination for multi-member party */
    if ((game_state.player_controlled_party_count & 0xFF) != 1)
        dt.pagination_window = WINDOW_STATUS_MENU;

    /* Assembly lines 27-40: set window title to character name */
    {
        char name_buf[6];
        int i;
        for (i = 0; i < 5 && c->name[i] != 0x00; i++)
            name_buf[i] = eb_char_to_ascii(c->name[i]);
        name_buf[i] = '\0';
        set_window_title(WINDOW_STATUS_MENU, name_buf, 5);
    }

    dt.force_left_text_alignment = 1;

    /* Assembly lines 43-53: Level (row 0, pixel 38, padding 1) */
    set_window_number_padding(1);
    set_text_pixel_position(0, 38);
    print_number((int)c->level, 1);

    /* Assembly lines 54-97: HP / PP with slash separator (padding 2) */
    set_window_number_padding(2);

    /* Current HP */
    set_text_pixel_position(3, 94);
    print_number((int)c->current_hp, 2);
    /* Slash (EB char 0x5F = '/') */
    set_text_pixel_position(3, 114);
    print_string("/");
    /* Max HP */
    set_text_pixel_position(3, 121);
    print_number((int)c->max_hp, 2);

    /* Current PP */
    set_text_pixel_position(4, 94);
    print_number((int)c->current_pp, 2);
    /* Slash (EB char 0x5F = '/') */
    set_text_pixel_position(4, 114);
    print_string("/");
    /* Max PP */
    set_text_pixel_position(4, 121);
    print_number((int)c->max_pp, 2);

    /* Assembly lines 98-167: combat stats (rows 0-6 at pixel 199, padding 2) */
    set_text_pixel_position(0, 199);
    print_number((int)c->offense, 1);
    set_text_pixel_position(1, 199);
    print_number((int)c->defense, 1);
    set_text_pixel_position(2, 199);
    print_number((int)c->speed, 1);
    set_text_pixel_position(3, 199);
    print_number((int)c->guts, 1);
    set_text_pixel_position(4, 199);
    print_number((int)c->vitality, 1);
    set_text_pixel_position(5, 199);
    print_number((int)c->iq, 1);
    set_text_pixel_position(6, 199);
    print_number((int)c->luck, 1);

    /* Assembly lines 168-197: EXP + required EXP (padding 6) */
    set_window_number_padding(6);
    set_text_pixel_position(5, 97);
    {
        uint32_t exp = c->exp;
        if (exp > EXP_LIMIT) exp = EXP_LIMIT;
        print_number((int)exp, 6);
    }
    set_text_pixel_position(6, 10);
    print_number((int)get_required_exp(char_id), 6);

    dt.force_left_text_alignment = 0;

    /* Assembly lines 198-263: affliction display.
     * Scans afflictions[0..6]; prints the first non-zero one found.
     * slot 0: TEXT_5[(value-1)*16]   (major afflictions)
     * slot 1: TEXT_5[value*16 + 96]  (secondary: cold/mushroom/possessed)
     * slot 5: TEXT_6                 (homesick)
     * slots 2-4,6: break immediately if non-zero */
    if (status_equip_text_data) {
        for (int slot = 0; slot < AFFLICTION_GROUP_COUNT; slot++) {
            uint8_t val = c->afflictions[slot];
            if (val == 0) continue;

            const uint8_t *text_ptr = NULL;
            if (slot == 0 && val >= 1) {
                text_ptr = status_equip_text_data + STATUS_TEXT5_OFFSET
                         + (int)(val - 1) * AFFLICTION_TEXT_ENTRY_SIZE;
            } else if (slot == 1) {
                text_ptr = status_equip_text_data + STATUS_TEXT5_OFFSET
                         + (int)val * AFFLICTION_TEXT_ENTRY_SIZE + 96;
            } else if (slot == 5) {
                text_ptr = status_equip_text_data + STATUS_TEXT6_OFFSET;
            } else {
                break; /* slots 2-4,6: unknown affliction, stop */
            }

            if (text_ptr) {
                /* Assembly lines 250-252: SET_FOCUS_TEXT_CURSOR(A=1, X=1) */
                set_focus_text_cursor(1, 1);
                int len = 0;
                while (len < AFFLICTION_TEXT_ENTRY_SIZE && text_ptr[len] != 0x00) len++;
                print_eb_string(text_ptr, len);
            }
            break; /* only print first found affliction */
        }
    }

    /* Assembly lines 264-275: equipment/affliction display tile.
     * GET_EQUIP_WINDOW_TEXT(afflictions, mode=0) → tile code from TEXT_2 table.
     * PRINT_CHAR_WITH_SOUND writes the tile-level character to the window. */
    set_focus_text_cursor(11, 1);
    {
        uint16_t equip_tile = get_equip_window_text(c->afflictions, 0);
        print_char_with_sound(equip_tile);
    }

    /* Assembly lines 276-287: PSI info text (skip for Jeff, char_index == 2) */
    if (char_index != 2 && status_equip_text_data) {
        dt.force_left_text_alignment = 1;
        set_text_pixel_position(7, 36);
        const uint8_t *psi_text = status_equip_text_data + STATUS_TEXT4_OFFSET;
        int len = 0;
        while (len < 35 && psi_text[len] != 0x00) len++;
        print_eb_string(psi_text, len);
        dt.force_left_text_alignment = 0;
    }

    /* Assembly line 289: CLEAR_INSTANT_PRINTING */
    clear_instant_printing();
}

/* --- CC 0x1A tree: menus/telephone ---
 * Byte counts from include/textmacros.asm.
 *
 * All CC 0x1A handlers are now ported. */

/* Convert an EB-encoded string from a binary asset to an ASCII C string.
 * src: EB-encoded bytes from ROM data. max_len: maximum source bytes to read.
 * dst: output ASCII ert.buffer (must be at least max_len+1 bytes).
 * Returns length of resulting ASCII string. */
int eb_to_ascii_buf(const uint8_t *src, int max_len, char *dst) {
    int j = 0;
    for (int i = 0; i < max_len && src[i] != 0x00; i++) {
        dst[j++] = eb_char_to_ascii(src[i]);
    }
    dst[j] = '\0';
    return j;
}

/*
 * PARTY_CHARACTER_SELECTOR — Port of asm/text/party_character_selector.asm.
 *
 * Displays a party member selection menu and returns the selected character ID
 * (PARTY_MEMBER enum: 1=Ness, 2=Paula, 3=Jeff, 4=Poo), or 0 if cancelled.
 *
 * Two modes controlled by the mode parameter:
 *   mode == 0: Overworld mode — creates a text window with party member names,
 *              runs selection_menu, returns selected character's party_members[] ID.
 *   mode == 1: Battle mode — uses HPPP window column selection with LEFT/RIGHT
 *              navigation, A/L=select, B=cancel.
 *
 * script_ptrs: array of 4 x uint32_t text script pointers (one per possible party
 *              member). In overworld mode these are stored but not executed. In
 *              battle mode they are displayed when navigating between characters.
 * allow_cancel: 0 = cannot cancel, 1 = B button cancels (returns 0).
 *
 * Window selection (overworld):
 *   party_count == 1 → WINDOW_SINGLE_CHARACTER_SELECT (0x33)
 *   party_count >= 2 → WINDOW_TARGETING_PROMPT + party_count - 1 (0x29..0x2B)
 *
 * Assembly: called from CC_1A_00 (uncancellable, Y=0) and CC_1A_01 (cancellable, Y=1).
 * Parameters: A=script_ptrs_base, X=mode, Y=allow_cancel.
 */
uint16_t party_character_selector(uint32_t *script_ptrs, uint16_t mode,
                                         uint16_t allow_cancel) {
    /* Save/restore the calling window's argument_memory.
     * Assembly lines 24-30: saves window_stats::argument_memory to LOCAL06,
     * lines 370-378: restores it on return. */
    uint32_t saved_argument_memory = get_argument_memory();
    uint16_t result = 0;

    if (mode == 1) {
        /* --- Overworld mode (assembly lines 34-111) ---
         * Mode == 1 → overworld selection menu (falls through in assembly).
         * Assembly: CMP #1; BNEL @BATTLE_MODE_PATH — branches AWAY on mode != 1. */
        save_window_text_attributes();

        /* Pick window based on party count.
         * Assembly line 36-46: party_count==1 → SINGLE_CHARACTER_SELECT,
         * else → TARGETING_PROMPT + party_count - 1 */
        uint8_t party_count = game_state.player_controlled_party_count;
        uint16_t window_id;
        if (party_count == 1) {
            window_id = WINDOW_SINGLE_CHARACTER_SELECT;
        } else {
            window_id = WINDOW_TARGETING_PROMPT + party_count - 1;
        }
        create_window(window_id);

        /* Build menu items: one per party member.
         * Assembly lines 54-100: loops party_count times, gets each character's name
         * from GET_PARTY_CHARACTER_NAME, copies 5 chars + null, adds menu item
         * with text_x = index * 6, text_y = 0, userdata = party_member_id. */
        for (int i = 0; i < party_count; i++) {
            uint8_t member_id = game_state.party_members[i];

            char name_buf[7];
            if (member_id >= 1 && member_id <= 4) {
                eb_to_ascii_buf(party_characters[member_id - 1].name, 5, name_buf);
            } else if (member_id == PARTY_MEMBER_KING) {
                eb_to_ascii_buf(game_state.pet_name, 6, name_buf);
                name_buf[5] = '\0';
            } else {
                name_buf[0] = '\0';
            }

            add_menu_item(name_buf, member_id, (uint16_t)(i * 6), 0);
        }

        print_menu_items();
        result = selection_menu(allow_cancel);
        close_window(window_id);
        restore_window_text_attributes();
    } else {
        /* --- Battle mode (assembly lines 112-369) ---
         * Mode != 1 → HPPP column selection with LEFT/RIGHT navigation.
         * Uses BATTLE_MENU_CURRENT_CHARACTER_ID for initial cursor position.
         * Displays per-character text scripts when switching characters. */

        /* Lines 112-132: Copy 4 script pointers to global array.
         * Assembly: loop X=0..3, copy script_ptrs[X] → PARTY_MEMBER_SELECTION_SCRIPTS[X]. */
        for (int i = 0; i < 4; i++) {
            dt.party_member_selection_scripts[i] = script_ptrs[i];
        }

        /* Lines 133-140: Determine initial character index.
         * If BATTLE_MENU_CURRENT_CHARACTER_ID == -1 → start at 0, else use it. */
        uint16_t current_index;
        if (win.battle_menu_current_character_id == -1) {
            current_index = 0;
        } else {
            current_index = (uint16_t)win.battle_menu_current_character_id;
        }

        /* Lines 141-156: Display initial character's text script.
         * Looks up party_members[current_index], gets 0-based char ID,
         * loads script_ptrs[char_id], calls DISPLAY_TEXT if non-null. */
        {
            uint8_t member_id = game_state.party_members[current_index];
            if (member_id > 0 && member_id <= 4) {
                uint32_t script_addr = dt.party_member_selection_scripts[member_id - 1];
                if (script_addr != 0) {
                    display_text_from_addr(script_addr);
                }
            }
        }

        /* Lines 157-160: Initialize animation state.
         * PAGINATION_ANIMATION_FRAME = 0, delay = 10. */
        dt.pagination_animation_frame = 0;
        uint16_t delay = 10;

        /* Main loop: handle character indicator, animation, input.
         * Assembly: @MULTI_PARTY_WINDOW4 through @CHAR_LOOP_CHECK2. */
        for (;;) {
            /* Lines 161-166: If mode == 0, highlight current character's HPPP column.
             * Assembly: LDA @LOCAL09; BNE skip; JSL SELECT_BATTLE_MENU_CHARACTER */
            if (mode == 0) {
                select_battle_menu_character(current_index);
            }

            /* Lines 167-168: CLEAR_INSTANT_PRINTING, WINDOW_TICK */
            clear_instant_printing();
            window_tick();

            /* Lines 171-229: Pagination arrow animation. */
            render_pagination_arrows();

            /* Lines 230-309: Input polling loop.
             * Runs UPDATE_HPPP_METER_AND_RENDER each frame, checks buttons.
             * Counter increments until >= delay, then toggles animation. */
            uint16_t counter = 0;
            int input_handled = 0;

            while (counter < delay) {
                update_hppp_meter_and_render();

                if (core.pad1_pressed & PAD_LEFT) {
                    /* Lines 236-251: LEFT pressed — move to previous character.
                     * SFX depends on mode: 0 → MENU_OPEN_CLOSE (27), else → CURSOR2 (2). */
                    int16_t new_index = (int16_t)current_index - 1;
                    uint16_t sfx = (mode == 0) ? 27 : 2;
                    dt.pagination_animation_frame = 2;

                    /* Lines 322-342: Boundary check with wrap-around.
                     * new_index < 0 → wrap to party_count - 1.
                     * new_index >= party_count → wrap to 0. */
                    uint8_t pcount = game_state.player_controlled_party_count;
                    if (new_index < 0) {
                        new_index = (int16_t)(pcount - 1);
                    } else if ((uint16_t)new_index >= pcount) {
                        new_index = 0;
                    }

                    /* Lines 344-364: If changed, play SFX and display new text. */
                    if ((uint16_t)new_index != current_index) {
                        play_sfx(sfx);
                        current_index = (uint16_t)new_index;
                        uint8_t mid = game_state.party_members[current_index];
                        if (mid > 0 && mid <= 4) {
                            uint32_t sa = dt.party_member_selection_scripts[mid - 1];
                            if (sa != 0) {
                                display_text_from_addr(sa);
                            }
                        }
                    }

                    /* Line 366-368: Shorter delay after character change. */
                    delay = 4;
                    input_handled = 1;
                    break;
                }

                if (core.pad1_pressed & PAD_RIGHT) {
                    /* Lines 252-268: RIGHT pressed — move to next character. */
                    int16_t new_index = (int16_t)current_index + 1;
                    uint16_t sfx = (mode == 0) ? 27 : 2;
                    dt.pagination_animation_frame = 3;

                    uint8_t pcount = game_state.player_controlled_party_count;
                    if ((uint16_t)new_index >= pcount) {
                        new_index = 0;
                    } else if (new_index < 0) {
                        new_index = (int16_t)(pcount - 1);
                    }

                    if ((uint16_t)new_index != current_index) {
                        play_sfx(sfx);
                        current_index = (uint16_t)new_index;
                        uint8_t mid = game_state.party_members[current_index];
                        if (mid > 0 && mid <= 4) {
                            uint32_t sa = dt.party_member_selection_scripts[mid - 1];
                            if (sa != 0) {
                                display_text_from_addr(sa);
                            }
                        }
                    }

                    delay = 4;
                    input_handled = 1;
                    break;
                }

                if (core.pad1_pressed & PAD_CONFIRM) {
                    /* Lines 269-280: A/L pressed — confirm selection.
                     * Returns party_members[current_index] (1-based character ID). */
                    uint8_t member_id = game_state.party_members[current_index];
                    result = (uint16_t)(member_id & 0xFF);
                    play_sfx(1);  /* SFX::CURSOR1 */
                    input_handled = 2;  /* done */
                    break;
                }

                if (core.pad1_pressed & PAD_CANCEL) {
                    /* Lines 281-300: B/SELECT pressed — cancel (if allowed).
                     * Returns 0. Clears battle menu character indicator. */
                    if (allow_cancel == 1) {
                        result = 0;
                        uint16_t sfx = (mode == 0) ? 27 : 2;
                        play_sfx(sfx);
                        clear_battle_menu_character_indicator();
                        input_handled = 2;  /* done */
                        break;
                    }
                }

                /* Lines 301-304: No meaningful input — increment counter. */
                counter++;
            }

            if (input_handled == 2) {
                break;  /* A/L confirm or B/SELECT cancel — exit outer loop */
            }

            if (input_handled == 1) {
                continue;  /* LEFT/RIGHT — restart outer loop with new character */
            }

            /* Lines 310-321: Counter expired — toggle animation frame (0↔1).
             * Reset delay to 10. Assembly: @MULTI_PARTY_WINDOW6 re-entry. */
            if (dt.pagination_animation_frame != 0) {
                dt.pagination_animation_frame = 0;
            } else {
                dt.pagination_animation_frame = 1;
            }
            delay = 10;
        }
    }

    /* Lines 370-378: Cleanup — restore pagination and argument_memory. */
    dt.pagination_animation_frame = -1;
    set_argument_memory(saved_argument_memory);

    return result;
}


/* --- CC_1C_01_TABLE resolve system ---
 *
 * Port of CC_1C_01_TABLE (asm/data/text/CC_1C_01_data.asm) and
 * RESOLVE_CC_TABLE_DATA (asm/battle/resolve_cc_table_data.asm).
 *
 * The assembly table maps 96 indices to game_state/char_struct fields:
 *   0: null, 1-7: global game data, 8-29: char 0, 30-51: char 1,
 *   52-73: char 2, 74-95: char 3 (22 entries per character).
 *
 * This is our own C-native implementation of the same lookup, not binary data. */

/* Per-character field sub-index (0-21 within a character block) */
typedef enum {
    CC_CHAR_NAME = 0,
    CC_CHAR_LEVEL,         /* 1 */
    CC_CHAR_EXP,           /* 2 */
    CC_CHAR_CURRENT_HP,    /* 3 */
    CC_CHAR_HP_TARGET,     /* 4 */
    CC_CHAR_MAX_HP,        /* 5 */
    CC_CHAR_CURRENT_PP,    /* 6 */
    CC_CHAR_PP_TARGET,     /* 7 */
    CC_CHAR_MAX_PP,        /* 8 */
    CC_CHAR_OFFENSE,       /* 9 */
    CC_CHAR_DEFENSE,       /* 10 */
    CC_CHAR_SPEED,         /* 11 */
    CC_CHAR_GUTS,          /* 12 */
    CC_CHAR_LUCK,          /* 13 */
    CC_CHAR_VITALITY,      /* 14 */
    CC_CHAR_IQ,            /* 15 */
    CC_CHAR_BASE_IQ,       /* 16 */
    CC_CHAR_BASE_OFFENSE,  /* 17 */
    CC_CHAR_BASE_DEFENSE,  /* 18 */
    CC_CHAR_BASE_SPEED,    /* 19 */
    CC_CHAR_BASE_GUTS,     /* 20 */
    CC_CHAR_BASE_LUCK,     /* 21 */
    CC_CHAR_FIELD_COUNT    /* 22 */
} CcCharField;

#define CC_GLOBAL_COUNT 8   /* entries 0-7: null + 7 global fields */
#define CC_TABLE_TOTAL (CC_GLOBAL_COUNT + CC_CHAR_FIELD_COUNT * 4)  /* 96 */

/* Resolve a CC_1C_01_TABLE entry.
 * Returns: for integers, the value itself (zero-extended).
 *          for strings, returns a pointer to the EB-encoded string cast to uintptr_t.
 * Sets *out_type to CC_TABLE_TYPE_STRING or CC_TABLE_TYPE_INT.
 * Sets *out_str_len to string length (for string type entries).
 * Port of RESOLVE_CC_TABLE_DATA (C3EE7A). */
uintptr_t resolve_cc_table_data(uint16_t index, int *out_type, int *out_str_len) {
    *out_type = CC_TABLE_TYPE_INT;
    *out_str_len = 0;

    if (index == 0) return 0;  /* null entry */

    /* Global entries 1-7 */
    if (index < CC_GLOBAL_COUNT) {
        switch (index) {
        case 1:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(game_state.mother2_playername);
            return (uintptr_t)game_state.mother2_playername;
        case 2:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(game_state.earthbound_playername);
            return (uintptr_t)game_state.earthbound_playername;
        case 3:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(game_state.pet_name);
            return (uintptr_t)game_state.pet_name;
        case 4:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(game_state.favourite_food);
            return (uintptr_t)game_state.favourite_food;
        case 5:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(game_state.favourite_thing);
            return (uintptr_t)game_state.favourite_thing;
        case 6:
            return game_state.money_carried;
        case 7:
            return game_state.bank_balance;
        }
    }

    /* Character entries: index 8-95 → char_index 0-3, field 0-21 */
    if (index >= CC_GLOBAL_COUNT && index < CC_TABLE_TOTAL) {
        uint16_t rel = index - CC_GLOBAL_COUNT;
        uint16_t char_index = rel / CC_CHAR_FIELD_COUNT;
        uint16_t field = rel % CC_CHAR_FIELD_COUNT;

        if (char_index >= 4) return 0;
        CharStruct *c = &party_characters[char_index];

        switch ((CcCharField)field) {
        case CC_CHAR_NAME:
            *out_type = CC_TABLE_TYPE_STRING;
            *out_str_len = (int)sizeof(c->name);
            return (uintptr_t)c->name;
        case CC_CHAR_LEVEL:       return c->level;
        case CC_CHAR_EXP:         return c->exp;
        case CC_CHAR_CURRENT_HP:  return c->current_hp;
        case CC_CHAR_HP_TARGET:   return c->current_hp_target;
        case CC_CHAR_MAX_HP:      return c->max_hp;
        case CC_CHAR_CURRENT_PP:  return c->current_pp;
        case CC_CHAR_PP_TARGET:   return c->current_pp_target;
        case CC_CHAR_MAX_PP:      return c->max_pp;
        case CC_CHAR_OFFENSE:     return c->offense;
        case CC_CHAR_DEFENSE:     return c->defense;
        case CC_CHAR_SPEED:       return c->speed;
        case CC_CHAR_GUTS:        return c->guts;
        case CC_CHAR_LUCK:        return c->luck;
        case CC_CHAR_VITALITY:    return c->vitality;
        case CC_CHAR_IQ:          return c->iq;
        case CC_CHAR_BASE_IQ:     return c->base_iq;
        case CC_CHAR_BASE_OFFENSE: return c->base_offense;
        case CC_CHAR_BASE_DEFENSE: return c->base_defense;
        case CC_CHAR_BASE_SPEED:  return c->base_speed;
        case CC_CHAR_BASE_GUTS:   return c->base_guts;
        case CC_CHAR_BASE_LUCK:   return c->base_luck;
        default: return 0;
        }
    }

    return 0;
}

/* Get the "string length" of a CC table entry (byte 0 of the 3-byte entry).
 * For string entries, this is the max length of the string field.
 * For integer entries, this is the size in bytes (1, 2, or 4).
 * Used by CC_19_28 to bounds-check character extraction. */
uint8_t get_cc_table_entry_size(uint16_t index) {
    if (index == 0) return 0;

    /* Global entries */
    if (index < CC_GLOBAL_COUNT) {
        switch (index) {
        case 1: return (uint8_t)sizeof(game_state.mother2_playername);
        case 2: return (uint8_t)sizeof(game_state.earthbound_playername);
        case 3: return (uint8_t)sizeof(game_state.pet_name);
        case 4: return (uint8_t)sizeof(game_state.favourite_food);
        case 5: return (uint8_t)sizeof(game_state.favourite_thing);
        case 6: return (uint8_t)sizeof(game_state.money_carried);
        case 7: return (uint8_t)sizeof(game_state.bank_balance);
        }
    }

    /* Character entries */
    if (index >= CC_GLOBAL_COUNT && index < CC_TABLE_TOTAL) {
        uint16_t field = (index - CC_GLOBAL_COUNT) % CC_CHAR_FIELD_COUNT;
        switch ((CcCharField)field) {
        case CC_CHAR_NAME:        return (uint8_t)sizeof(((CharStruct *)0)->name);
        case CC_CHAR_LEVEL:       return 1;
        case CC_CHAR_EXP:         return 4;
        case CC_CHAR_CURRENT_HP:  return 2;
        case CC_CHAR_HP_TARGET:   return 2;
        case CC_CHAR_MAX_HP:      return 2;
        case CC_CHAR_CURRENT_PP:  return 2;
        case CC_CHAR_PP_TARGET:   return 2;
        case CC_CHAR_MAX_PP:      return 2;
        case CC_CHAR_OFFENSE:     return 1;
        case CC_CHAR_DEFENSE:     return 1;
        case CC_CHAR_SPEED:       return 1;
        case CC_CHAR_GUTS:        return 1;
        case CC_CHAR_LUCK:        return 1;
        case CC_CHAR_VITALITY:    return 1;
        case CC_CHAR_IQ:          return 1;
        case CC_CHAR_BASE_IQ:     return 1;
        case CC_CHAR_BASE_OFFENSE: return 1;
        case CC_CHAR_BASE_DEFENSE: return 1;
        case CC_CHAR_BASE_SPEED:  return 1;
        case CC_CHAR_BASE_GUTS:   return 1;
        case CC_CHAR_BASE_LUCK:   return 1;
        default: return 0;
        }
    }

    return 0;
}

/* PRINT_CC_TABLE_VALUE: Port of asm/text/print_cc_table_value.asm.
 * Looks up a stat/value by index from the CC_1C_01_TABLE and prints it.
 * Integer entries → print_number. String entries → print as EB text. */
void print_cc_table_value(uint16_t index) {
    int type = CC_TABLE_TYPE_INT;
    int str_len = 0;
    uintptr_t value = resolve_cc_table_data(index, &type, &str_len);

    if (type == CC_TABLE_TYPE_STRING) {
        /* Print EB-encoded string, converting to ASCII */
        const uint8_t *data = (const uint8_t *)value;
        if (data) {
            int len = 0;
            while (len < str_len && len < 31 && data[len] != 0x00) len++;
            print_eb_string(data, len);
        }
    } else {
        /* Print integer — delegate to print_number() like the assembly does
         * (JSR PRINT_NUMBER in print_cc_table_value.asm lines 43/56/68). */
        print_number((int)value, 0);
    }
}

/* --- CC 0x1C tree: text printing/display ---
 * Byte counts from include/textmacros.asm.
 *
 *   0x00: TEXT_COLOUR_EFFECTS(flag) — toggle text color/style
 *   0x01-0x03: Print character stat/name/character by ID
 *   0x04: OPEN_HP_PP_WINDOWS — show HP/PP status windows
 *   0x05-0x08: Print item/teleport/text string/special GFX by ID
 *   0x0A-0x0B: Print number/money from working memory
 *   0x0C: PRINT_VERTICAL_TEXT_STRING — vertical text layout
 *   0x0D-0x0F: Print battle action names/amounts (user, target, damage)
 *   0x12: PRINT_PSI_NAME(id) — render PSI ability name
 *   0x13: DISPLAY_PSI_ANIMATION(id, target) — play PSI visual effect
 *   0x14-0x15: LOAD_SPECIAL — query special values for branching */

/* PRINT_ENEMY_ARTICLE: Port of asm/battle/print_enemy_article.asm (USA only).
 * Conditionally prints "The " or "the " before an enemy name.
 * mode: 0 = attacker, non-zero = target.
 *
 * Logic:
 *   1. If enemy_id == -1 (character, not enemy): clear article flag, return.
 *   2. If article already printed (flag != 0): return.
 *   3. Look up enemy_data.the_flag from ENEMY_CONFIGURATION_TABLE.
 *      If the_flag == 0: return (enemy doesn't use article).
 *   4. If dt.last_printed_character == CHAR::BULLET (0x70, start of line): print "The ".
 *      Otherwise: print "the ". */
void print_enemy_article(uint16_t mode) {
    if (mode == 0) {
        /* Attacker */
        if (dt.attacker_enemy_id == -1) {
            dt.print_attacker_article = 0;
            return;
        }
        if (dt.print_attacker_article != 0)
            return;
        if (!enemy_config_table)
            return;
        if (enemy_config_table[dt.attacker_enemy_id].the_flag == 0)
            return;
    } else {
        /* Target */
        if (dt.target_enemy_id == -1) {
            dt.print_target_article = 0;
            return;
        }
        if (dt.print_target_article != 0)
            return;
        if (!enemy_config_table)
            return;
        if (enemy_config_table[dt.target_enemy_id].the_flag == 0)
            return;
    }
    /* Print "The " at start of line, "the " otherwise */
    if (dt.last_printed_character == 0x70)
        print_string("The ");
    else
        print_string("the ");
}

/* --- Simple CC argument skip table ---
 * Byte counts from include/textmacros.asm.
 * Safety fallback for any CC code that falls through the main switch's default case.
 * All CCs 0x00-0x1F are handled by explicit case statements above, so this function
 * is only reached for truly unknown/invalid CC codes in corrupted script data. */
void cc_skip_args(ScriptReader *r, uint8_t cc) {
    switch (cc) {
    case 0x00: break;                                     /* LINE_BREAK: 0 args */
    case 0x01: break;                                     /* START_NEW_LINE: 0 args */
    /* 0x02 = END_BLOCK, handled separately */
    case 0x03: break;                                     /* HALT_WITH_PROMPT: 0 args */
    case 0x04: script_read_word(r); break;                /* SET_EVENT_FLAG: 2 args */
    case 0x05: script_read_word(r); break;                /* CLEAR_EVENT_FLAG: 2 args */
    case 0x06: script_read_word(r); script_read_dword(r); break; /* JUMP_IF_FLAG_SET: 6 args (word + dword) */
    case 0x07: script_read_word(r); break;                /* CHECK_EVENT_FLAG: 2 args */
    case 0x08: script_read_dword(r); break;               /* CALL_TEXT: 4 args (dword) */
    case 0x09: {
        /* JUMP_MULTI: 1 byte count + N * 4-byte destinations */
        uint8_t count = script_read_byte(r);
        for (int i = 0; i < count; i++) script_read_dword(r);
        break;
    }
    case 0x0A: script_read_dword(r); break;               /* JUMP: 4 args (dword) */
    case 0x0B: script_read_byte(r); break;                /* TEST_IF_WORKMEM_TRUE: 1 arg */
    case 0x0C: script_read_byte(r); break;                /* TEST_IF_WORKMEM_FALSE: 1 arg */
    case 0x0D: script_read_byte(r); break;                /* COPY_TO_ARGMEM: 1 arg */
    case 0x0E: script_read_byte(r); break;                /* STORE_TO_ARGMEM: 1 arg */
    case 0x0F: break;                                     /* INCREMENT_SECONDARY_MEMORY: 0 args */
    case 0x10: script_read_byte(r); break;                /* PAUSE: 1 arg */
    case 0x11: break;                                     /* CREATE_SELECTION_MENU: 0 args */
    case 0x12: break;                                     /* CLEAR_TEXT_LINE: 0 args */
    case 0x13: break;                                     /* HALT_WITHOUT_PROMPT: 0 args */
    case 0x14: break;                                     /* HALT_WITH_PROMPT_ALWAYS: 0 args */
    default: break;
    }
}

/* --- Word-level wrap lookahead ---
 * Port of CHECK_TEXT_WORD_WRAP (asm/text/check_text_word_wrap.asm).
 * Called from the display_text main loop when ENABLE_WORD_WRAP is set
 * and UPCOMING_WORD_LENGTH has reached 0 (= word boundary).
 *
 * Peeks ahead in the script stream (without consuming) to measure the
 * upcoming word's pixel width.  If the word won't fit on the current
 * line, inserts a newline before the word starts.  Sets
 * dt.upcoming_word_length so the check skips until the next word boundary.
 *
 * Word boundary = space (EB 0x50) or control code (< 0x20). */
void check_text_word_wrap(ScriptReader *reader) {
    WindowInfo *w = get_focus_window_info();
    if (!w || win.current_focus_window == WINDOW_ID_NONE) return;

    /* Make a copy of the reader for lookahead (don't consume from real stream) */
    ScriptReader peek = *reader;

    uint16_t accumulated_width = 0;
    dt.upcoming_word_length = 0;

    while (1) {
        /* Read next byte from peek copy */
        uint8_t ch;
        if (peek.prefix_ptr) {
            ch = *peek.prefix_ptr;
            if (ch == 0x00) {
                /* End of prefix ert.buffer → switch to main stream */
                peek.prefix_ptr = NULL;
                continue;
            }
            peek.prefix_ptr++;
        } else {
            if (peek.ptr >= peek.end) break;
            ch = *peek.ptr++;
        }

        /* Handle dictionary codes (0x15/0x16/0x17) — expand inline.
         * Port of @DICT1/@DICT2/@DICT3 in check_text_word_wrap.asm. */
        if (ch >= 0x15 && ch <= 0x17) {
            uint16_t bank_offset = (ch - 0x15) * 256;
            /* Read index byte */
            uint8_t index;
            if (peek.prefix_ptr) {
                index = *peek.prefix_ptr;
                if (index == 0x00) { peek.prefix_ptr = NULL; continue; }
                peek.prefix_ptr++;
            } else {
                if (peek.ptr >= peek.end) break;
                index = *peek.ptr++;
            }
            const uint8_t *entry = compressed_text_lookup(bank_offset, index);
            if (entry) {
                /* Read first char from dictionary entry, set rest as prefix */
                ch = *entry;
                if (ch == 0x00) continue;
                peek.prefix_ptr = entry + 1;
            } else {
                continue;
            }
        }

        /* Word boundary: space (0x50) or control code (< 0x20) */
        if (ch == 0x50) break;
        if (ch < 0x20) break;

        /* Count this character as part of the word */
        dt.upcoming_word_length++;

        /* Measure character width */
        uint16_t char_width;
        if (ch == 0x2F) {
            /* Assembly hardcodes width 8 for character 0x2F */
            char_width = 8;
        } else {
            uint8_t glyph_index = (ch - 0x50) & 0x7F;
            char_width = font_get_width(w->font, glyph_index) + character_padding;
        }
        accumulated_width += char_width;
    }

    /* Check if the word fits on the current line.
     * Assembly: if text_x > 0, pos = (text_x-1)*8 + (VWF_X & 7)
     *           if text_x == 0, pos = VWF_X & 7 */
    uint16_t current_pos;
    if (w->text_x > 0)
        current_pos = (uint16_t)((w->text_x - 1) * 8 + (vwf_x & 7));
    else
        current_pos = vwf_x & 7;
    uint16_t total = current_pos + accumulated_width;
    uint16_t max_pixels = (w->width - 2) * 8;  /* content width in pixels (asm width already has -2 baked in) */

    if (total > max_pixels) {
        print_newline();
        vwf_indent_new_line = 1;
    }
}

/* --- Main interpreter loop ---
 * Port of DISPLAY_TEXT (asm/text/display_text.asm). */
void display_text(const uint8_t *script, size_t script_size) {
    if (!script || script_size == 0) return;

    /* Save/restore dt.g_cc18_attrs_saved per call level, mirroring PUSH_TEXT_STACK_FRAME /
     * POP_TEXT_STACK_FRAME which give each display_text invocation its own
     * display_text_state::unknown4 field. Each nested CC_08 (CALL_TEXT) call gets
     * a clean flag; the parent's flag is restored when the nested call returns. */
    uint8_t saved_cc18_attrs = dt.g_cc18_attrs_saved;
    dt.g_cc18_attrs_saved = 0;

    ScriptReader reader;
    reader.base = script;
    reader.ptr = script;
    reader.end = script + script_size;
    reader.prefix_ptr = NULL;
    dt.upcoming_word_length = 0;

    while (reader.ptr < reader.end || reader.prefix_ptr) {
        /* Word-wrap lookahead — port of display_text.asm @MAIN_LOOP lines 49-61.
         * When ENABLE_WORD_WRAP is active, before each character we check if
         * we're at a word boundary (dt.upcoming_word_length == 0).  If so, peek
         * ahead to measure the next word and insert a newline if it won't fit.
         * Otherwise just decrement the counter (still inside measured word). */
        if (dt.enable_word_wrap) {
            if (dt.upcoming_word_length > 0) {
                dt.upcoming_word_length--;
            } else {
                check_text_word_wrap(&reader);
                /* After check, dt.upcoming_word_length is set for the word.
                 * Assembly does BRA @READ_NEXT_BYTE (skips DEC for first char).
                 * This is handled naturally: we don't decrement here, so the
                 * first char of the word gets a "free pass". */
            }
        }

        uint8_t byte = script_read_byte(&reader);

        /* Printable character (>= 0x20) — port of PRINT_LETTER
         * (asm/text/print_letter.asm).
         *
         * Assembly processes one character at a time:
         *   1. RENDER_VWF_CHARACTER (store for VWF rendering)
         *   2. Play SFX::TEXT_PRINT (conditional on mode/settings)
         *   3. Delay SELECTED_TEXT_SPEED+1 frames via WINDOW_TICK
         *
         * The per-character delay creates the typewriter effect. */
        if (byte >= 0x20) {
            uint8_t eb_buf[2] = { byte, 0x00 };
            print_eb_string(eb_buf, 1);

            /* Sound + delay only when not instant printing
             * (assembly lines 68-70: skip to @RETURN if instant) */
            if (!dt.instant_printing) {
                /* Sound logic (assembly lines 36-66):
                 * Mode 2: always play. Mode 3: never play.
                 * Default: play if dt.blinking_triangle_flag == 0.
                 * Skip for byte 0x20 and CHAR::SPACE (0x50). */
                bool play_sound;
                if (dt.text_sound_mode == 2) {
                    play_sound = true;
                } else if (dt.text_sound_mode == 3) {
                    play_sound = false;
                } else {
                    play_sound = (dt.blinking_triangle_flag == 0);
                }
                if (play_sound && byte != 0x20 && byte != 0x50) {
                    play_sfx(7);  /* SFX::TEXT_PRINT */
                }

                /* Delay loop (assembly lines 71-81):
                 * SELECTED_TEXT_SPEED + 1 iterations of WINDOW_TICK.
                 * game_state.text_speed = SELECTED_TEXT_SPEED. */
                int delay = (game_state.text_speed & 0xFF) + 1;
                for (int i = 0; i < delay; i++) {
                    window_tick();
                }
            }
            continue;
        }

        /* END_BLOCK (CC_02): Script terminator.
         * Assembly (display_text.asm @CC_02, lines 371-381):
         *   RESTORE_TEXT_STATE_ATTRIBUTES (conditional on display_text_state::unknown4)
         *   then POP_TEXT_STACK_FRAME.
         * unknown4 starts as 0 (cleared by INIT_TEXT_STATE), set to 1 by CC_18_02.
         * In C: dt.g_cc18_attrs_saved tracks this per display_text call level. */
        if (byte == 0x02) {
            if (dt.g_cc18_attrs_saved) {
                restore_window_text_attributes();
            }
            dt.g_cc18_attrs_saved = saved_cc18_attrs;
            return;
        }

        /* Control codes */
        switch (byte) {
        case 0x00: {
            /* LINE_BREAK: 0 args. Port of CC_00 → PRINT_NEWLINE.
             * Advances text cursor to next line (text_x=0, text_y++). */
            print_newline();
            break;
        }
        case 0x01: {
            /* START_NEW_LINE: 0 args. Port of CC_01.
             * Like LINE_BREAK but only if not already at start of line. */
            WindowInfo *w = get_focus_window_info();
            if (w && w->text_x > 0) {
                print_newline();
            }
            break;
        }
        case 0x03:
            /* HALT_WITH_PROMPT: 0 args. Port of CC_03.
             * Assembly: calls CC_13_14 with A=1, X=0 → show triangle, allow text speed delay. */
            cc_halt(1, 0);
            break;

        /* Implemented CCs */
        case 0x04: cc_set_event_flag(&reader); break;
        case 0x05: cc_clear_event_flag(&reader); break;
        case 0x07: {
            /* CHECK_EVENT_FLAG: 2 args (word) → store flag value to working memory.
             * Port of CC_07 (asm/text/ccs/get_event_flag.asm). */
            uint16_t flag = script_read_word(&reader);
            int16_t val = event_flag_get(flag);
            set_working_memory((uint32_t)(int32_t)val);
            break;
        }
        case 0x0B: {
            /* TEST_IF_WORKMEM_TRUE: 1 arg. If working_memory == arg, set 1; else 0.
             * Port of CC_0B (asm/text/ccs/check_equal.asm). */
            uint8_t val = script_read_byte(&reader);
            uint32_t wm = get_working_memory();
            set_working_memory((uint32_t)((uint16_t)wm == (uint16_t)val ? 1 : 0));
            break;
        }
        case 0x0C: {
            /* TEST_IF_WORKMEM_FALSE: 1 arg. If working_memory != arg, set 1; else 0.
             * Port of CC_0C (asm/text/ccs/check_not_equal.asm). */
            uint8_t val = script_read_byte(&reader);
            uint32_t wm = get_working_memory();
            set_working_memory((uint32_t)((uint16_t)wm != (uint16_t)val ? 1 : 0));
            break;
        }
        case 0x0D: {
            /* COPY_TO_ARGMEM: 1 arg. If arg != 0, copy secondary → argument.
             * If arg == 0, copy working → argument.
             * Port of CC_0D (asm/text/ccs/copy_to_argmem.asm). */
            uint8_t val = script_read_byte(&reader);
            if (val != 0) {
                set_argument_memory((uint32_t)get_secondary_memory());
            } else {
                set_argument_memory(get_working_memory());
            }
            break;
        }
        case 0x0E: {
            /* STORE_TO_ARGMEM: 1 arg. If arg != 0, set secondary = arg.
             * If arg == 0, set secondary = low byte of argument_memory.
             * Port of CC_0E (asm/text/ccs/set_secmem.asm). */
            uint8_t val = script_read_byte(&reader);
            if (val != 0) {
                set_secondary_memory(val);
            } else {
                set_secondary_memory((uint16_t)(get_argument_memory() & 0xFF));
            }
            break;
        }
        case 0x0F:
            /* INCREMENT_SECONDARY_MEMORY: 0 args.
             * Port of CC_0F → INCREMENT_SECONDARY_MEMORY (asm/text/increment_secondary_memory.asm). */
            increment_secondary_memory();
            break;
        case 0x10: cc_pause(&reader); break;

        /* Jump CCs */
        case 0x06: {
            /* JUMP_IF_FLAG_SET: 2+4 args (word flag + dword address).
             * Port of CC_06. If event flag is set, jump to target; else skip. */
            uint16_t flag = script_read_word(&reader);
            uint32_t target = script_read_dword(&reader);
            if (event_flag_get(flag)) {
                resolve_text_jump(&reader, target);
            }
            break;
        }
        case 0x08: {
            /* CALL_TEXT: 4 args (dword address).
             * Port of CC_08. Recursive call; returns after END_BLOCK. */
            uint32_t target = script_read_dword(&reader);
            display_text_from_addr(target);
            break;
        }
        case 0x09: {
            /* JUMP_MULTI: 1 byte count + count*4 byte addresses.
             * Port of CC_09 (asm/text/ccs/jump_multi.asm).
             * Uses working_memory as 1-indexed selector into address table. */
            uint8_t count = script_read_byte(&reader);
            uint32_t wm = get_working_memory();
            if (wm > 0 && wm <= count) {
                script_skip(&reader, (wm - 1) * 4);
                uint32_t target = script_read_dword(&reader);
                resolve_text_jump(&reader, target);
            } else {
                /* Out of range or 0 — skip all destinations */
                script_skip(&reader, count * 4);
            }
            break;
        }
        case 0x0A: {
            /* JUMP: 4 args (dword address).
             * Port of CC_0A. Unconditional jump. */
            uint32_t target = script_read_dword(&reader);
            resolve_text_jump(&reader, target);
            break;
        }

        case 0x12: {
            /* CLEAR_TEXT_LINE: 0 args. Port of CC_12 (clear_line.asm).
             * Assembly: FREE_WINDOW_TEXT_ROW, then SET_FOCUS_TEXT_CURSOR(x=0, y=text_y).
             * SET_FOCUS_TEXT_CURSOR calls ADVANCE_VWF_TILE (lightweight advance
             * to next tile boundary), NOT RESET_VWF_TEXT_STATE (full VWF reset). */
            WindowInfo *w = get_focus_window_info();
            if (w) {
                free_window_text_row(w);
                set_focus_text_cursor(0, w->text_y);
            }
            break;
        }
        case 0x11: {
            /* CREATE_SELECTION_MENU: 0 args. Port of CC_11.
             * Runs the selection menu for the focus window and stores
             * result in working_memory. Allow cancel (B button = 0).
             * Assembly: JSR SELECTION_MENU; SET_WORKING_MEMORY;
             *           JSR CLEAR_FOCUS_WINDOW_MENU_OPTIONS */
            uint16_t result = selection_menu(1);
            set_working_memory((uint32_t)result);
            clear_focus_window_menu_options();
            break;
        }
        case 0x13:
            /* HALT_WITHOUT_PROMPT: 0 args. Port of CC_13.
             * Assembly: calls CC_13_14 with A=0, X=0 → no triangle. */
            cc_halt(0, 0);
            break;
        case 0x14:
            /* HALT_WITH_PROMPT_ALWAYS: 0 args. Port of CC_14.
             * Assembly: calls CC_13_14 with A=1, X=1 → show triangle, skip text speed delay. */
            cc_halt(1, 1);
            break;

        /* Tree dispatchers */
        case 0x18: cc_18_dispatch(&reader); break;
        case 0x19: cc_19_dispatch(&reader); break;
        case 0x1A: cc_1a_dispatch(&reader); break;
        case 0x1B: cc_1b_dispatch(&reader); break;
        case 0x1C: cc_1c_dispatch(&reader); break;
        case 0x1D: cc_1d_dispatch(&reader); break;
        case 0x1E: cc_1e_dispatch(&reader); break;
        case 0x1F: cc_1f_dispatch(&reader); break;

        /* Compression banks (0x15-0x17): dictionary text substitution.
         * Reads an index byte, looks up a common string fragment, and inserts
         * it into the text stream via the prefix ert.buffer.
         * Port of @COMPRESSION_BANK_ONE/TWO/THREE in display_text.asm. */
        case 0x15:
        case 0x16:
        case 0x17: {
            uint16_t bank_offset = (byte - 0x15) * 256;
            uint8_t index = script_read_byte(&reader);
            const uint8_t *entry = compressed_text_lookup(bank_offset, index);
            if (entry) {
                reader.prefix_ptr = entry;
            }
            break;
        }

        /* All other simple CCs — skip arguments */
        default:
            cc_skip_args(&reader, byte);
            break;
        }

        if (platform_input_quit_requested()) {
            dt.g_cc18_attrs_saved = saved_cc18_attrs;
            return;
        }
    }
    dt.g_cc18_attrs_saved = saved_cc18_attrs;
}
