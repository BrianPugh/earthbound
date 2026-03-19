#ifndef INTRO_FILE_SELECT_H
#define INTRO_FILE_SELECT_H

#include "core/types.h"

/* Run the complete file menu loop.
   Returns when user selects Start Game or completes New Game naming.
   Ported from FILE_MENU_LOOP in asm/intro/file_select_menu_loop.asm */
uint16_t file_menu_loop(void);

/* Shared keyboard input dialog for naming screens.
 * Port of TEXT_INPUT_DIALOG (asm/text/text_input_dialog.asm).
 * Creates keyboard window (0x1C), runs input loop, closes on done.
 *
 * name_buf:    output EB-encoded name buffer (at least max_len bytes)
 * max_len:     max characters to accept
 * naming_index: -1 for standalone (no Don't Care), >=0 for Don't Care
 * name_display_window_id: window where name VWF tiles are rendered
 * name_text_y: text row in that window for the name display (0-based)
 * existing_name: if non-NULL, pre-fill from this EB-encoded name
 *
 * Returns: 0 = confirmed, -1 = cancelled/back */
int text_input_dialog(uint8_t *name_buf, int max_len, int naming_index,
                      uint16_t name_display_window_id, int name_text_y,
                      const uint8_t *existing_name);

#endif /* INTRO_FILE_SELECT_H */
