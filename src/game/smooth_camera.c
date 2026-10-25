/*
 * Smooth camera eligibility and party-sprite identification (see
 * smooth_camera.h). Presentation only; nothing here writes game state.
 */
#include "game/smooth_camera.h"

#ifdef EB_SMOOTH_CAMERA

#include "core/mode_stack.h"
#include "game/game_state.h"
#include "game/window.h"
#include "include/binary.h"
#include "include/constants.h"
#include "snes/ppu.h"

#include <string.h>

bool smooth_camera_enabled;
uint8_t smooth_camera_oam_owner[128];
uint8_t smooth_camera_draw_owner;

bool smooth_camera_prepare(SmoothCameraFrame *f) {
    if (!smooth_camera_enabled)
        return false;

    /* Plain overworld walking: the root mode alone, past boot, nothing modal. */
    if (g_mode_stack.depth != 1 || g_mode_stack.mode[0] != GAME_MODE_OVERWORLD)
        return false;
    if (g_mode_stack.state[0].overworld.phase >= OWP_BOOT_SETUP)
        return false;
    if (any_window_open())
        return false;
    /* camera_mode 0 = the camera follows the leader (no scripted pan/shake). */
    if (game_state.camera_mode != 0)
        return false;

    /* A plain, fully lit mode-1 overworld frame whose BG1 scroll is the
     * leader-derived camera. Anything else (fades, flashes, mosaic, scripted
     * scroll, battle swirls) keeps the stock integer camera for that frame. */
    if ((ppu.inidisp & 0x80) || (ppu.inidisp & 0x0F) != 0x0F)
        return false;
    if ((ppu.bgmode & BGMODE_MODE_MASK) != 1 || (ppu.mosaic >> MOSAIC_SIZE_SHIFT))
        return false;
    if (ppu.bg_hofs[0] != (uint16_t)(game_state.leader_x_coord - EB_VIEWPORT_CENTER_X) ||
        ppu.bg_vofs[0] != (uint16_t)(game_state.leader_y_coord - EB_VIEWPORT_CENTER_Y))
        return false;

    f->frac_x = game_state.leader_x_frac;
    f->frac_y = game_state.leader_y_frac;
    if (f->frac_x == 0 && f->frac_y == 0)
        return false; /* nothing to shift: the normal pass is already exact */

    /* Party members (followers included) stay fixed on screen; everything
     * else (NPCs, enemies, objects) belongs to the shifted scenery. */
    bool is_party[MAX_ENTITIES];
    memset(is_party, 0, sizeof(is_party));
    int count = game_state.party_count;
    if (count > TOTAL_PARTY_COUNT)
        count = TOTAL_PARTY_COUNT;
    for (int i = 0; i < count; i++) {
        uint16_t slot = read_u16_le(&game_state.party_entity_slots[i * 2]);
        if (slot < MAX_ENTITIES)
            is_party[slot] = true;
    }
    bool any = false;
    for (int i = 0; i < 128; i++) {
        uint8_t owner = smooth_camera_oam_owner[i];
        bool party = owner != 0 && is_party[owner - 1];
        f->party_oam[i] = party;
        any |= party;
    }
    return any;
}

#endif /* EB_SMOOTH_CAMERA */
