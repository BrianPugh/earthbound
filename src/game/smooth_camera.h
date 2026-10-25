#ifndef GAME_SMOOTH_CAMERA_H
#define GAME_SMOOTH_CAMERA_H

/*
 * Smooth camera (desktop presentation option, off by default).
 *
 * The leader's position is 16.16 fixed point, but the BG scroll is the integer
 * leader coordinate minus the viewport centre, so while walking the scenery
 * steps an uneven 0/1/2 px per frame around a fixed Ness. That judder is in the
 * original game too. With the option on, the port renders a second frame with
 * the party's sprites hidden and the host draws that scenery shifted by the
 * leader's sub-pixel fraction (at window scale 2 or more), then draws the
 * party's pixels unshifted on top.
 *
 * This is presentation only: game logic, positions, collision, OAM contents
 * seen by the game and savestates are untouched. The whole module compiles
 * out unless the build defines EB_SMOOTH_CAMERA (only the SDL2 desktop port
 * does), so embedded ports pay no RAM for it.
 */

#include "core/types.h"

#ifdef EB_SMOOTH_CAMERA

/* Runtime switch (--smooth-camera / EB_SMOOTH_CAMERA=1 on the unix port). */
extern bool smooth_camera_enabled;

/* Entity slot + 1 that queued each OAM entry this frame (0 = not an entity),
 * filled by render_all_priority_sprites() while smooth_camera_enabled. */
extern uint8_t smooth_camera_oam_owner[128];

/* Entity slot + 1 of the entity whose draw callback is running (0 = none);
 * queue_sprite_draw() records it per queued spritemap. */
extern uint8_t smooth_camera_draw_owner;

typedef struct {
    uint16_t frac_x;        /* leader sub-pixel fraction, 0..65535 = 0..1 px */
    uint16_t frac_y;
    uint8_t  party_oam[128]; /* nonzero = OAM entry belongs to a party member */
} SmoothCameraFrame;

/* Decide whether this frame can be presented with the smooth camera (plain
 * overworld walking only) and fill *f. Returns false when the frame must use
 * the normal single-pass path. Reads game state only. */
bool smooth_camera_prepare(SmoothCameraFrame *f);

#endif /* EB_SMOOTH_CAMERA */

#endif /* GAME_SMOOTH_CAMERA_H */
