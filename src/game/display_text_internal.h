/*
 * Display text system internal header.
 *
 * Shared declarations for display_text sub-files.
 * NOT for external consumers — use display_text.h instead.
 */
#ifndef GAME_DISPLAY_TEXT_INTERNAL_H
#define GAME_DISPLAY_TEXT_INTERNAL_H

#include "game/display_text.h"
#include "game/window.h"  /* for WindowInfo */
#include "data/assets.h"
#include "core/mode_stack.h"  /* ModeState/GameMode for cc_1f_dispatch's push-request */

/* TextSource and ScriptReader are defined in game/display_text.h (included above)
 * so the GAME_MODE_DISPLAY_TEXT ModeState in mode_stack.h can embed the reader. */

/* ---- Script reader helpers (display_text.c) ---- */
uint8_t script_read_byte(ScriptReader *r);
uint16_t script_read_word(ScriptReader *r);
uint32_t script_read_dword(ScriptReader *r);
void script_skip(ScriptReader *r, int n);
void resolve_text_jump(ScriptReader *r, uint32_t addr);

/* ---- Data helpers (display_text.c) ---- */
void toggle_hppp_flipout_mode(uint16_t enable);
uint16_t is_escargo_express_full(void);
uint16_t get_item_subtype_2(uint16_t item_id);
void check_text_word_wrap(ScriptReader *reader);
void cc_skip_args(ScriptReader *r, uint8_t cc);

/* ---- CC table constants ---- */
#define CC_TABLE_TYPE_STRING  0
#define CC_TABLE_TYPE_INT     1

/* ---- CC table stat printing (display_text.c) ---- */
uintptr_t resolve_cc_table_data(uint16_t index, int *out_type, int *out_str_len);
uint8_t get_cc_table_entry_size(uint16_t index);
void print_cc_table_value(uint16_t index);
void print_enemy_article(uint16_t mode);

/* ---- PSI teleport destination table constants ---- */
#define PSI_TELEPORT_DEST_NAME_LEN    25
#define PSI_TELEPORT_DEST_ENTRY_SIZE  31
#define PSI_TELEPORT_DEST_MAX_ENTRIES 17

/* ---- Wallet / ATM constants ---- */
#define WALLET_LIMIT  99999u
#define ATM_LIMIT     9999999u

/* ---- PSI teleport data (compile-time linked) ---- */
#define psi_teleport_dest_data  ASSET_DATA(ASSET_DATA_PSI_TELEPORT_DEST_TABLE_BIN)
#define psi_teleport_dest_size  ASSET_SIZE(ASSET_DATA_PSI_TELEPORT_DEST_TABLE_BIN)

/* Build a child DISPLAY_TEXT init from a CALL_TEXT/gosub target address (mirrors
 * display_text_from_addr -> display_text). Returns false if unresolvable. Used by
 * CC_08 (display_text.c) and CC_1F_C0 (display_text_cc.c) to STEP_PUSH a nested
 * GAME_MODE_DISPLAY_TEXT child instead of recursing on the C stack. */
bool dt_make_child_init(ModeState *init, uint32_t addr);

/* ---- Window helpers (display_text.c) ---- */
WindowInfo *get_focus_window_info(void);
/* party_character_selector: now BATTLE-path only (mode != 1) — the HPPP column
 * selector that STEP_PUSHes GAME_MODE_CHAR_SELECT. Still inline-blocking (its
 * char_select on_change cascades into display_text_from_addr; Phase B). */
uint16_t party_character_selector(uint32_t *script_ptrs, uint16_t mode,
                                  uint16_t allow_cancel);
/* Overworld party-member selection (former party_character_selector mode==1):
 * builds the selection window + menu items, fills the SELECTION_MENU child init for
 * a STEP_PUSH, and returns the saved argument_memory to restore on resume.
 * *out_window_id receives the created window to close on resume. The result-store
 * and window/attr cleanup run in the DT_RESUME_CC1A_PARTY_SEL handler on POP. */
uint32_t party_selector_overworld_prepare(uint16_t allow_cancel, ModeState *out_init,
                                          uint16_t *out_window_id);

/* ---- CC dispatch handlers (display_text_cc.c) ---- */
void cc_set_event_flag(ScriptReader *r);
void cc_clear_event_flag(ScriptReader *r);
void cc_18_dispatch(ScriptReader *r);
void cc_19_dispatch(ScriptReader *r);
/* cc_1a_dispatch: most sub-ops run inline and return false. Sub 0x00/0x01
 * (PARTY_MEMBER_SELECTION_MENU) in OVERWORLD mode (mode byte == 1) instead fills
 * out_init/out_mode (GAME_MODE_SELECTION_MENU to STEP_PUSH), out_resume
 * (DT_RESUME_CC1A_PARTY_SEL), *out_window_id (window to close on POP) and
 * *out_saved_argmem (argument_memory to restore on POP), and returns true. The
 * battle path (mode != 1) stays inline-blocking (Phase B). The caller
 * (mode_step_display_text) zeroes *out_init before the call. */
bool cc_1a_dispatch(ScriptReader *r, ModeState *out_init, GameMode *out_mode,
                    uint8_t *out_resume, uint16_t *out_window_id,
                    uint32_t *out_saved_argmem);
void cc_1b_dispatch(ScriptReader *r);
void cc_1c_dispatch(ScriptReader *r);
void cc_1d_dispatch(ScriptReader *r);
void cc_1e_dispatch(ScriptReader *r);
/* cc_1f_dispatch: most sub-ops run inline and return false. The three yielding
 * sub-ops (0x52 number-select, 0x60 text-speed delay, 0x61 wait-for-actionscript)
 * instead fill out_init/out_mode (the child to STEP_PUSH) and out_resume (the
 * DisplayTextResume post-work the parent owes on POP) and return true. The caller
 * (mode_step_display_text) zeroes *out_init before the call. */
bool cc_1f_dispatch(ScriptReader *r, ModeState *out_init, GameMode *out_mode,
                    uint8_t *out_resume);

/* ---- Menu functions (display_text_menus.c) ---- */
uint16_t enter_your_name_please(uint16_t param);
uint16_t use_sound_stone(uint16_t cancellable);
uint16_t dispatch_special_event(uint16_t event_id);
void show_character_inventory(uint16_t window_id, uint16_t char_source);
uint16_t open_store_menu(uint16_t shop_id);
uint16_t select_escargo_express_item(void);
uint16_t open_telephone_menu(void);
uint16_t display_telephone_contact_text(void);

#endif /* GAME_DISPLAY_TEXT_INTERNAL_H */
