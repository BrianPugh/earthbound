#ifndef INTRO_LOGO_SCREEN_H
#define INTRO_LOGO_SCREEN_H

#include "core/types.h"

/* Logo IDs */
#define LOGO_NINTENDO  0
#define LOGO_APE       1
#define LOGO_HALKEN    2

/* Display the logo sequence (Nintendo -> APE -> HAL).
   Returns non-zero if the user pressed a button to skip. */
uint16_t logo_screen(void);

/* Load and decompress a single logo's assets.
   logo_id: LOGO_NINTENDO, LOGO_APE, or LOGO_HALKEN */
void logo_screen_load(uint16_t logo_id);

#endif /* INTRO_LOGO_SCREEN_H */
