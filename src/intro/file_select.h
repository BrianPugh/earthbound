#ifndef INTRO_FILE_SELECT_H
#define INTRO_FILE_SELECT_H

#include "core/types.h"

/* Run the complete file menu loop.
   Returns when user selects Start Game or completes New Game naming.
   Ported from FILE_MENU_LOOP in asm/intro/file_select_menu_loop.asm */
uint16_t file_menu_loop(void);

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

/* Shared keyboard input dialog for naming screens.
 * Port of TEXT_INPUT_DIALOG (asm/text/text_input_dialog.asm).
 * Creates keyboard window (0x1C), runs input loop, closes on done.
 *
 * name_target: NameTargetId selecting the output buffer (written on confirm)
 * max_len:     max characters to accept
 * naming_index: -1 for standalone (no Don't Care), >=0 for Don't Care
 * name_display_window_id: window where name VWF tiles are rendered
 * name_text_y: text row in that window for the name display (0-based)
 * existing_name: if non-NULL, pre-fill from this EB-encoded name
 *
 * Returns: 0 = confirmed, -1 = cancelled/back */
int text_input_dialog(int name_target, int max_len, int naming_index,
                      uint16_t name_display_window_id, int name_text_y,
                      const uint8_t *existing_name);

#endif /* INTRO_FILE_SELECT_H */
