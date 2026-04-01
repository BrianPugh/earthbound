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

/* Script reading cursor */
typedef struct {
    const uint8_t *base;
    const uint8_t *ptr;
    const uint8_t *end;
    const uint8_t *prefix_ptr;  /* dictionary substitution buffer (CC 0x15-0x17) */
} ScriptReader;

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

/* ---- Window helpers (display_text.c) ---- */
WindowInfo *get_focus_window_info(void);
uint16_t party_character_selector(uint32_t *script_ptrs, uint16_t mode,
                                  uint16_t allow_cancel);

/* ---- CC dispatch handlers (display_text_cc.c) ---- */
void cc_halt(int show_triangle, int skip_text_speed);
void cc_set_event_flag(ScriptReader *r);
void cc_clear_event_flag(ScriptReader *r);
void cc_pause(ScriptReader *r);
void cc_18_dispatch(ScriptReader *r);
void cc_19_dispatch(ScriptReader *r);
void cc_1a_dispatch(ScriptReader *r);
void cc_1b_dispatch(ScriptReader *r);
void cc_1c_dispatch(ScriptReader *r);
void cc_1d_dispatch(ScriptReader *r);
void cc_1e_dispatch(ScriptReader *r);
void cc_1f_dispatch(ScriptReader *r);

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
