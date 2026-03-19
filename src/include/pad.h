#ifndef PAD_H
#define PAD_H

#include <stdint.h>

/* SNES joypad bitmask — matches PAD enum from enums.asm.
 *
 *   Bit 15: B      Bit 14: Y      Bit 13: Select  Bit 12: Start
 *   Bit 11: Up     Bit 10: Down   Bit  9: Left    Bit  8: Right
 *   Bit  7: A      Bit  6: X      Bit  5: L       Bit  4: R
 */
#define PAD_B      (1 << 15)
#define PAD_Y      (1 << 14)
#define PAD_SELECT (1 << 13)
#define PAD_START  (1 << 12)
#define PAD_UP     (1 << 11)
#define PAD_DOWN   (1 << 10)
#define PAD_LEFT   (1 <<  9)
#define PAD_RIGHT  (1 <<  8)
#define PAD_A      (1 <<  7)
#define PAD_X      (1 <<  6)
#define PAD_L      (1 <<  5)
#define PAD_R      (1 <<  4)

/* Functional button groups — used throughout game logic.
 * Ports with fewer buttons should ensure at minimum:
 *   - PAD_A or PAD_L is mapped (confirm/interact)
 *   - PAD_B or PAD_SELECT is mapped (cancel/HP-PP display)
 *   - PAD_X is mapped if town map is desired
 *   - PAD_R is optional (bicycle bell only)
 *   - PAD_Y is unused by the game
 *   - PAD_START is only needed for title screen
 */
#define PAD_CONFIRM      (PAD_A | PAD_L)                       /* confirm selection, interact */
#define PAD_CANCEL       (PAD_B | PAD_SELECT)                  /* cancel, back, HP/PP display */
#define PAD_ANY_BUTTON   (PAD_A | PAD_B | PAD_L | PAD_START)   /* skip attract mode, title screen, etc. */
#define PAD_TEXT_ADVANCE  (PAD_A | PAD_B | PAD_SELECT | PAD_L) /* advance/dismiss text */
#define PAD_DPAD         (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)

#endif /* PAD_H */
