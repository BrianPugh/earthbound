#ifndef INTRO_FILE_SELECT_H
#define INTRO_FILE_SELECT_H

#include "core/types.h"

typedef union ModeState ModeState;

/* One-shot setup for the file menu (the yield-free front half of FILE_MENU_LOOP,
 * asm/intro/file_select_menu_loop.asm): loads the file-select asset data, runs
 * FILE_SELECT_INIT, and starts the fade-in. The init_intro parent calls this,
 * then STEP_PUSHes GAME_MODE_FILE_MENU (phase FM_FADEIN_WAIT) to run the cascade. */
void file_menu_setup(void);

/* Identifies the (stable global) buffer a naming dialog writes its result into.
 * The output buffer was a uint8_t* parameter; it is replaced by this ID so the
 * keyboard mode (GAME_MODE_TEXT_INPUT) can hoist its state into a POD ModeState
 * and resolve the buffer only on confirm. Values 0..6 deliberately match the
 * new-game naming index. */
typedef enum {
    NAME_TARGET_PARTY0 = 0,   /* party_characters[0].name */
    NAME_TARGET_PARTY1,       /* party_characters[1].name */
    NAME_TARGET_PARTY2,       /* party_characters[2].name */
    NAME_TARGET_PARTY3,       /* party_characters[3].name */
    NAME_TARGET_PET,          /* game_state.pet_name */
    NAME_TARGET_FOOD,         /* game_state.favourite_food */
    NAME_TARGET_THING,        /* game_state.favourite_thing + 4 */
    NAME_TARGET_M2_PLAYER,    /* game_state.mother2_playername */
    NAME_TARGET_EB_PLAYER,    /* game_state.earthbound_playername */
} NameTargetId;

/* Create the keyboard window and seed a TextInputState for GAME_MODE_TEXT_INPUT.
 * The setup half of the on-screen naming keyboard (port of TEXT_INPUT_DIALOG,
 * asm/text/text_input_dialog.asm): creates the keyboard window (0x1C); the modes
 * that STEP_PUSH GAME_MODE_TEXT_INPUT (GAME_MODE_NEW_GAME_NAMING,
 * GAME_MODE_ENTER_NAME) call this then push the mode, which runs the input loop
 * and closes the window on done.
 *
 * name_target: NameTargetId selecting the output buffer (written on confirm)
 * max_len:     max characters to accept
 * naming_index: -1 for standalone (no Don't Care), >=0 for Don't Care
 * name_display_window_id: window where name VWF tiles are rendered
 * name_text_y: text row in that window for the name display (0-based)
 * existing_name: if non-NULL, pre-fill from this EB-encoded name (folded into the
 *                seed so the pointer never enters the POD ModeState)
 *
 * The mode pops 0 = confirmed, -1 = cancelled/back. */
void text_input_prepare(ModeState *init, int name_target, int max_len,
                        int naming_index, uint16_t name_display_window_id,
                        int name_text_y, const uint8_t *existing_name);

#endif /* INTRO_FILE_SELECT_H */
