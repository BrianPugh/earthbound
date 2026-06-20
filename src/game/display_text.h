/*
 * DISPLAY_TEXT — text bytecode interpreter.
 *
 * Port of asm/text/display_text.asm and asm/text/ccs/ control code handlers.
 *
 * The ROM's text system is a bytecode interpreter that drives all in-game
 * text, menus, and event scripting. Scripts are stored as binary bytecodes:
 *   - Bytes 0x20-0xFF: printable characters
 *   - Bytes 0x00-0x1F: control codes (CCs)
 *   - CC 0x02: END_BLOCK (end script)
 *   - CC 0x00-0x14: simple CCs (inline handlers)
 *   - CC 0x15-0x17: compression bank redirects
 *   - CC 0x18-0x1F: tree dispatchers (read next byte as sub-opcode)
 */
#ifndef GAME_DISPLAY_TEXT_H
#define GAME_DISPLAY_TEXT_H

#include "core/types.h"

/* Which embedded text asset a ScriptReader reads from. A stable, build-portable
 * tag (savestate-serializable) in place of a raw base pointer; text_source_base()
 * (display_text.c) maps it back to the live asset pointer on each read. */
typedef enum {
    TEXT_SRC_DIALOGUE = 0,    /* dialogue blob      (ASSET_DIALOGUE_DIALOGUE_BIN)   */
    TEXT_SRC_INLINE_STRINGS,  /* inline strings     (ASSET_TEXT_INLINE_STRINGS_BIN) */
    TEXT_SRC_STATUS_WINDOW,   /* status window text (ASSET_DATA_STATUS_WINDOW_TEXT_BIN) */
    TEXT_SRC_COUNT,
} TextSource;

/* Script reading cursor — offset-based so it holds no raw pointers and can live in
 * a serializable ModeState (savestate-anywhere migration). `source` selects the
 * asset; ptr_off/end_off are byte offsets within it. prefix_off is the CC 0x15-0x17
 * dictionary-substitution cursor (-1 = none); that CC is not yet wired up in the C
 * port, so prefix_off is currently always -1 (dormant). Defined here (not in the
 * internal header) so the GAME_MODE_DISPLAY_TEXT ModeState can embed it. */
typedef struct {
    uint8_t  source;       /* TextSource */
    uint32_t ptr_off;      /* current read offset within the source asset */
    uint32_t end_off;      /* end offset (exclusive) */
    int32_t  prefix_off;   /* dictionary substitution offset; -1 = none */
} ScriptReader;

/* Load the dialogue blob and inline string table. Call once at startup.
 * Returns true on success. */
bool display_text_init(void);

/* No-ops kept for API compatibility (all text lives in the dialogue blob). */
bool display_text_load_battle_text(void);
void display_text_free_battle_text(void);

/* Build a GAME_MODE_DISPLAY_TEXT init from a text address, for callers that
 * STEP_PUSH the mode (vs. running it inline). Returns false (the caller warns +
 * no-ops) if the address can't be resolved. Defined in display_text.c. */
union ModeState;  /* core/mode_stack.h */
bool dt_make_child_init(union ModeState *init, uint32_t addr);

/* No-yield runners for instant-print text (set_instant_printing() contexts).
 * Drive GAME_MODE_DISPLAY_TEXT to completion with NO host_process_frame() yield,
 * letting instant-print callers (menu hover text, the status window) avoid the
 * blocking pump_mode() bridge. Valid only because instant printing never enters
 * the typewriter DT_DELAY phase and never pushes a yielding child; if the script
 * parks anyway, they warn and force-unwind rather than hang. */
void display_text_inline(const uint8_t *script, size_t script_size);
void display_text_from_addr_inline(uint32_t addr);

/* Battle name buffer sizes (from bankconfig/common/ram.asm). */
#define BATTLE_NAME_ATTACKER_SIZE  30  /* sizeof(enemy_data::name) + 5 (USA) */
#define BATTLE_NAME_TARGET_SIZE    28  /* sizeof(enemy_data::name) + 3 (USA) */

/* Window text attributes backup (saved/restored by CC_18_02). */
typedef struct {
    uint8_t  id;
    uint8_t  text_x;
    uint8_t  text_y;
    uint16_t cursor_pixel_x;
    uint8_t  number_padding;
    uint16_t curr_tile_attributes;
    uint8_t  font;
    bool     valid;
} WindowTextAttributesCopy;

/* Consolidated display text runtime state. */
typedef struct {
    /* Text speed delay counter. Set from game_state.text_speed during file select.
     * Used by CC_1F_60 (TEXT_SPEED_DELAY) for per-character typewriter delay. */
    uint16_t text_speed_based_wait;

    /* Text system state (from ram.asm, initialized by INITIALIZE_BATTLE_UI_STATE) */
    uint16_t text_prompt_waiting_for_input;
    uint16_t text_sound_mode;

    /* INSTANT_PRINTING flag (from ram.asm).
     * When non-zero, text prints instantly (no per-character delay or SFX). */
    uint8_t instant_printing;

    /* EARLY_TICK_EXIT flag (from ram.asm:1126).
     * When non-zero, window_tick exits immediately (after clearing it). */
    uint8_t early_tick_exit;

    /* Blinking triangle flag (from ram.asm, near BATTLE_MODE_FLAG). */
    uint16_t blinking_triangle_flag;

    /* Battle text name buffers */
    char battle_attacker_name[BATTLE_NAME_ATTACKER_SIZE];
    char battle_target_name[BATTLE_NAME_TARGET_SIZE];

    /* CNUM / CITEM: current action number and item for battle text */
    uint32_t cnum;
    uint8_t  citem;

    /* FORCE_LEFT_TEXT_ALIGNMENT: when non-zero, text renders left-aligned. */
    uint16_t force_left_text_alignment;

    /* PAGINATION_WINDOW: controls the "▶" pagination indicator. */
    uint8_t  pagination_window;

    /* PAGINATION_ANIMATION_FRAME: animation state for pagination arrow. */
    int16_t pagination_animation_frame;

    /* ENABLE_WORD_WRAP (ram.asm:995) — 0=off, 0xFF=on */
    uint16_t enable_word_wrap;

    /* ALLOW_TEXT_OVERFLOW (ram.asm:1624) */
    uint8_t allow_text_overflow;

    /* Last printed character code */
    uint8_t last_printed_character;

    /* Current interacting event flag */
    uint16_t current_interacting_event_flag;

    /* --- Promoted file-statics (saveable runtime state) --- */

    /* UPCOMING_WORD_LENGTH (ram.asm:1110) — word wrap state. */
    uint16_t upcoming_word_length;

    /* Text script pointers for battle character selection (4 per party member). */
    uint32_t party_member_selection_scripts[4];

    /* Enemy IDs for article printing. -1 = character (no article). */
    int16_t attacker_enemy_id;
    int16_t target_enemy_id;

    /* Whether article has been printed this message. */
    uint8_t print_attacker_article;
    uint8_t print_target_article;

    /* Text register backups (CC_1B_05/06). */
    uint32_t text_main_register_backup;
    uint32_t text_sub_register_backup;
    uint8_t  text_loop_register_backup;

    /* CC_18_02 save flag (display_text_state::unknown4). */
    uint8_t g_cc18_attrs_saved;

    /* Window text attributes backup (CC_18_02 save/restore). */
    WindowTextAttributesCopy window_text_attrs_backup;

    /* HPPP meter flipout mode HP/PP backups. */
    uint16_t hppp_meter_flipout_mode_hp_backups[4];
    uint16_t hppp_meter_flipout_mode_pp_backups[4];

    /* Last party member status change tracking. */
    uint16_t last_party_member_status_last_check;
} DisplayTextState;

extern DisplayTextState dt;

/* Teleport destination structure (8 bytes, matches assembly teleport_destination) */
typedef struct {
    uint16_t x_coord;
    uint16_t y_coord;
    uint8_t  direction;
    uint8_t  screen_transition;
    uint16_t unknown6;
} TeleportDestination;

/* Look up a teleport destination by index. Returns NULL if invalid. */
const TeleportDestination *get_teleport_dest(uint16_t index);

/* --- Text script memory registers ---
 * These operate on the current focus window's memory fields.
 * Used by text scripts for conditional logic and parameter passing.
 * Port of SET/GET_WORKING_MEMORY, SET/GET_ARGUMENT_MEMORY. */
uint32_t get_working_memory(void);
void set_working_memory(uint32_t val);
void set_working_memory_storage(uint32_t val);
uint32_t get_working_memory_storage(void);
uint32_t get_argument_memory(void);
void set_argument_memory(uint32_t val);

/* SET_BATTLE_ATTACKER_NAME: Port of asm/battle/ui/set_battle_attacker_name.asm.
 * Copies src string (length bytes) into battle_attacker_name buffer
 * and null-terminates. Clears attacker_enemy_id to -1. */
void set_battle_attacker_name(const char *src, uint16_t length);

/* SET_BATTLE_TARGET_NAME: Port of asm/battle/ui/set_battle_target_name.asm.
 * Copies src string (length bytes) into battle_target_name buffer
 * and null-terminates. Clears target_enemy_id to -1. */
void set_battle_target_name(const char *src, uint16_t length);

/* Setters for battle article/enemy ID state used by FIX_ATTACKER_NAME/FIX_TARGET_NAME */
void set_attacker_enemy_id(int16_t id);
void set_target_enemy_id(int16_t id);
void set_print_attacker_article(uint8_t val);
void set_print_target_article(uint8_t val);

/* SET_CNUM: Port of asm/text/set_cnum.asm. */
void set_cnum(uint32_t value);

/* GET_CNUM: Port of asm/text/get_cnum.asm. */
uint32_t get_cnum(void);

/* GET/SET secondary memory */
uint16_t get_secondary_memory(void);
void set_secondary_memory(uint16_t val);
void increment_secondary_memory(void);

/* PRINT_PSI_NAME / GET_PSI_NAME: Port of asm/text/get_psi_name.asm.
 * Prints just the base PSI name (e.g. "Fire", "Rockin") without level suffix. */
void print_psi_name(uint16_t psi_name_id);

/* PRINT_PSI_ABILITY_NAME: Port of asm/text/print_psi_ability_name.asm.
 * Prints a PSI ability name + level suffix (e.g. "PSI Fire α") into
 * the current focus window at the current text cursor position. */
void print_psi_ability_name(uint16_t ability_id);

/* Get the PSI level suffix (α/β/γ/Σ/Ω) as an ASCII label string.
 * Writes a 1-char null-terminated string to out (e.g. "~" for α). */
void get_psi_suffix_label(uint16_t ability_id, char *out, size_t out_size);

/* EB_TO_ASCII_BUF: Convert an EB-encoded byte string to ASCII.
 * src: EB-encoded bytes from ROM data. max_len: max source bytes to read.
 * dst: output buffer (must be at least max_len+1 bytes).
 * Returns length of resulting ASCII string (excluding null terminator). */
int eb_to_ascii_buf(const uint8_t *src, int max_len, char *dst);

/* DISPLAY_STATUS_WINDOW: Port of asm/text/window/display_status_window.asm.
 * Opens the Status window for a character, displaying stats, afflictions,
 * and PSI info prompt. char_id: 1-based. */
void display_status_window(uint16_t char_id);

/* SET_INSTANT_PRINTING: Port of asm/text/set_instant_printing.asm.
 * Sets instant_printing = 1 (text prints without delay). */
void set_instant_printing(void);

/* CLEAR_INSTANT_PRINTING: Port of asm/text/clear_instant_printing.asm.
 * Sets instant_printing = 0 (text prints with normal delay). */
void clear_instant_printing(void);

/* RENDER_PAGINATION_ARROWS: Renders the pagination arrow animation tiles
 * at the right edge of pagination_window. Called during character selection loops. */
void render_pagination_arrows(void);

/* WINDOW_TICK: Port of asm/text/window_tick.asm.
 * Per-frame orchestrator: renders windows to bg2_buffer, updates HPPP,
 * syncs bg2_buffer to VRAM via upload_battle_screen_to_vram, then
 * calls render_frame_tick. */
void window_tick(void);

/* Run-to-completion form of window_tick(): same work, but the frame render uses
 * render_frame_tick_work() (no internal wait_for_vblank) so the caller owns the
 * yield. Used by mode-stack step functions during the savestate migration. */
void window_tick_work(void);

/* Park-propagating split of window_tick_work() (savestate D4b): a parked
 * actionscript callroutine becomes a STEP_PUSH instead of a nested pump_mode.
 * _step() returns true iff a frame parked — the caller must push
 * GAME_MODE_ACTIONSCRIPT_FRAME (actionscript_frame_take_push) and call _flush() at
 * its resume phase; on no park the tick finishes inline and _step() returns false. */
bool window_tick_work_step(void);
void window_tick_work_flush(void);

/* WINDOW_TICK_WITHOUT_INSTANT_PRINTING: Port of
 * asm/text/window_tick_without_instant_printing.asm.
 * Temporarily clears instant_printing, calls window_tick, re-enables it. */
void window_tick_without_instant_printing(void);

/* window_tick's window render + VRAM upload without advancing entity action
 * scripts or yielding (the "show window" beat). See display_text.c. */
void window_tick_no_actionscript(void);

/* SET_WINDOW_NUMBER_PADDING: Port of asm/text/window/set_window_number_padding.asm.
 * Sets the number_padding field on the focus window. */
void set_window_number_padding(uint16_t padding);

/* SET_TEXT_PIXEL_POSITION: Port of asm/text/set_text_pixel_position.asm.
 * Sets the text cursor to a specific pixel position within the focus window.
 * row = text_y, pixel_pos = horizontal pixel position. */
void set_text_pixel_position(uint16_t row, uint16_t pixel_pos);

#endif /* GAME_DISPLAY_TEXT_H */
