#ifndef INTRO_TITLE_SCREEN_H
#define INTRO_TITLE_SCREEN_H

#include "core/types.h"

/* Load title screen graphics into VRAM/CGRAM.
   Ported from LOAD_TITLE_SCREEN_GRAPHICS (C0EBE0.asm) +
   LOAD_TITLE_SCREEN_PALETTE (C0ECB7.asm) */
void load_title_screen_graphics(void);

/* Display the title screen, wait for user input.
   quick_mode: if non-zero, skip the slow palette fade.
   Returns: 0 = timed out (attract mode), 1 = user pressed Start/A/B.
   Ported from SHOW_TITLE_SCREEN (show_title_screen.asm) */
uint16_t show_title_screen(uint16_t quick_mode);

#endif /* INTRO_TITLE_SCREEN_H */
