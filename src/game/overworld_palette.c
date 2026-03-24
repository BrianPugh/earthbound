/*
 * Overworld palette, damage, and game-over/comeback functions.
 *
 * Ported from:
 *   ADJUST_SINGLE_COLOUR            — asm/overworld/adjust_single_colour.asm
 *   UPDATE_OVERWORLD_DAMAGE         — asm/overworld/update_overworld_damage.asm
 *   SPAWN                           — asm/overworld/spawn.asm
 *   INITIALIZE_GAME_OVER_SCREEN     — asm/misc/initialize_game_over_screen.asm
 *   PLAY_COMEBACK_SEQUENCE          — asm/misc/play_comeback_sequence.asm
 *   SKIPPABLE_PAUSE                 — asm/text/skippable_pause.asm
 *   LOAD_MAP_PALETTE_ANIMATION_FRAME — asm/system/palette/load_map_palette_animation_frame.asm
 *   INITIALIZE_MAP_PALETTE_FADE     — asm/overworld/initialize_map_palette_fade.asm
 *   UPDATE_MAP_PALETTE_FADE         — asm/system/palette/update_map_palette_fade.asm
 *   ANIMATE_MAP_PALETTE_CHANGE      — asm/system/palette/animate_map_palette_change.asm
 *   FADE_PALETTE_TO_WHITE           — asm/system/palette/fade_palette_to_white.asm
 *   ANIMATE_PALETTE_FADE_WITH_RENDERING — asm/system/palette/animate_palette_fade_with_rendering.asm
 */

#include "game/overworld_internal.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/battle.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/display_text.h"
#include "game/text.h"
#include "game/door.h"
#include "game/inventory.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "entity/sprite.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "core/decomp.h"
#include "game_main.h"
#include <string.h>

/* ---- ADJUST_SINGLE_COLOUR (port of asm/overworld/adjust_single_colour.asm) ----
 *
 * Adjusts a single colour channel toward a target, clamped to +/-6 range.
 * Returns:
 *   - colour2 if colour1 == colour2 (already matched)
 *   - colour1 if |colour2 - colour1| > 6 on the "bigger" side
 *   - colour2 +/- 6 if |colour2 - colour1| > 6 on the "smaller" side
 *   - colour2 if |colour2 - colour1| <= 6
 * colour1 = current value (A), colour2 = target (X). */
uint16_t adjust_single_colour(uint16_t colour1, uint16_t colour2) {
    if (colour1 == colour2)
        return colour2;

    if (colour1 > colour2) {
        /* colour1 > colour2: approaching from above */
        uint16_t diff = colour1 - colour2;
        if (diff <= 6) {
            return colour2;  /* Close enough — snap to target */
        }
        /* Too far — return colour1 - 6 (clamp approach) */
        return colour1 - 6;
    } else {
        /* colour1 < colour2: approaching from below */
        uint16_t diff = colour2 - colour1;
        if (diff <= 6) {
            return colour2;  /* Close enough — snap to target */
        }
        /* Too far — return colour1 + 6 (clamp approach) */
        return colour1 + 6;
    }
}

/* ---- Enemy touch flash helpers ---- */
static uint16_t background_colour_backup_ow;
static uint8_t tm_backup_ow;

void restore_bg_palette_callback(void) {
    ert.palettes[0] = background_colour_backup_ow;
    ppu.tm = tm_backup_ow;
}

void start_enemy_touch_flash(void) {
    if (ow.battle_swirl_countdown) return;
    if (ow.enemy_has_been_touched) return;
    background_colour_backup_ow = ert.palettes[0];
    ert.palettes[0] = (31 << 0) | (0 << 5) | (0 << 10);  /* RGB(31,0,0) = pure red */
    tm_backup_ow = ppu.tm;
    ppu.tm = 0;  /* TM_MIRROR = 0: hide all layers */
    schedule_overworld_task(restore_bg_palette_callback, 1);
}

/* ---- CHECK_LOW_HP_ALERT (port of asm/overworld/check_low_hp_alert.asm) ----
 *
 * Checks if a party member's HP has fallen below 20% of max.
 * If so and the alert hasn't been shown yet, shows a warning message.
 *
 * party_index: 0-based index into party_order / player_controlled_party_members.
 */
void check_low_hp_alert(uint16_t party_index) {
    uint8_t char_id = game_state.player_controlled_party_members[party_index];
    CharStruct *cs = &party_characters[char_id];

    /* Threshold = 20% of max HP = max_hp * 20 / 100 */
    uint16_t threshold = (uint16_t)((uint32_t)cs->max_hp * 20 / 100);
    if (cs->current_hp < threshold) {
        /* HP is low — show alert if not already shown */
        if (!ow.hp_alert_shown[party_index]) {
            /* Port of SHOW_HP_ALERT (asm/overworld/show_hp_alert.asm).
             * Opens a text window with "[name]'s HP is very low!" message.
             * Assembly: disable entities -> open window -> set attacker name
             *           -> DISPLAY_TEXT_PTR MSG_SYS_MAP_CRITICAL_SITUATION
             *           -> close window -> WINDOW_TICK -> enable entities. */
            disable_all_entities();
            create_window(0x01);  /* WINDOW::TEXT_STANDARD */
            set_battle_attacker_name((const char *)cs->name,
                                    sizeof(cs->name));
            display_text_from_snes_addr(0xC7C7AFu);  /* MSG_SYS_MAP_CRITICAL_SITUATION */
            close_focus_window();
            window_tick();
            enable_all_entities();
            ow.hp_alert_shown[party_index] = 1;
        }
    } else {
        /* HP is OK — clear the alert flag */
        ow.hp_alert_shown[party_index] = 0;
    }
}

/* ---- UPDATE_OVERWORLD_DAMAGE (port of asm/overworld/update_overworld_damage.asm) ----
 *
 * Called every main loop frame. Applies per-frame damage from:
 *   - Affliction 5 (poison): 10 HP every 120 frames
 *   - Affliction 4 (sunstroke): 10 HP every 120 frames
 *   - Afflictions 6-7 (environmental): 2 HP every 240 frames
 *   - Hot tile (trodden_tile_type & 0x0C == 12): 2 HP every 240 frames
 *
 * If a character's HP drops to 0, sets affliction to unconscious (1),
 * clears other afflictions, sets entity script var3 = 16 (KO trigger).
 *
 * After processing all party members:
 *   - If damage occurred, flashes screen red (START_ENEMY_TOUCH_FLASH).
 *   - If any KO occurred, calls UPDATE_PARTY + REFRESH_PARTY_ENTITIES.
 *
 * Returns total remaining HP across living party members.
 * A return value of 0 indicates a potential game-over (all KO'd -> SPAWN).
 */
uint16_t update_overworld_damage(void) {
    /* Early exit: special camera mode or status suppressed */
    if (game_state.camera_mode == 2) return 1;
    if (ow.overworld_status_suppression) return 1;

    uint16_t ko_count = 0;       /* @LOCAL04: number of members KO'd this frame */
    uint16_t total_hp = 0;       /* @LOCAL03: total remaining HP */
    uint16_t damage_events = 0;  /* @VIRTUAL04: number of damage ticks applied */

    for (uint16_t i = 0; i < 6; i++) {
        /* Read party_order to check if slot is occupied.
         * party_order stores character IDs: 1-4 = chosen four, 0 = empty. */
        uint8_t order_id = game_state.party_order[i];
        if (order_id == 0) break;        /* No more party members */
        if (order_id > 4) break;         /* Only process chosen four (IDs 1-4) */

        /* Get character struct via player_controlled_party_members mapping.
         * Values are 0-based: Ness=0, Paula=1, Jeff=2, Poo=3. */
        uint8_t char_id = game_state.player_controlled_party_members[i];
        CharStruct *cs = &party_characters[char_id];

        uint8_t affliction = cs->afflictions[0];

        /* Skip unconscious (1) and affliction 2 */
        if (affliction == 1) goto next_member;
        if (affliction == 2) goto next_member;

        if (affliction == 5) {
            /* --- Poison damage: 10 HP every 120 frames --- */
            if (ow.overworld_damage_countdown_frames[i] > 0) {
                ow.overworld_damage_countdown_frames[i]--;
                if (ow.overworld_damage_countdown_frames[i] != 0)
                    goto after_damage;
                /* Timer expired — apply damage */
                damage_events++;
                cs->current_hp -= 10;
                cs->current_hp_target -= 10;
                check_low_hp_alert(i);
            } else {
                /* Reset timer */
                ow.overworld_damage_countdown_frames[i] = 120;
            }
            goto after_damage;
        }

        /* --- Environmental/sunstroke damage --- */
        {
            bool apply_env = false;
            if (affliction >= 4 && affliction <= 7) {
                apply_env = true;
            } else if ((game_state.trodden_tile_type & 0x000C) == 12) {
                /* Hot tile damage (no affliction required) */
                apply_env = true;
            }

            if (apply_env) {
                if (ow.overworld_damage_countdown_frames[i] > 0) {
                    ow.overworld_damage_countdown_frames[i]--;
                    if (ow.overworld_damage_countdown_frames[i] != 0)
                        goto after_damage;
                    /* Timer expired — apply damage */
                    damage_events++;
                    if (affliction == 4) {
                        /* Sunstroke: 10 HP damage */
                        cs->current_hp -= 10;
                        cs->current_hp_target -= 10;
                    } else {
                        /* Other environmental: 2 HP damage */
                        cs->current_hp -= 2;
                        cs->current_hp_target -= 2;
                    }
                    check_low_hp_alert(i);
                } else {
                    /* Reset timer: 120 frames for sunstroke, 240 for others */
                    ow.overworld_damage_countdown_frames[i] = (affliction == 4) ? 120 : 240;
                }
            }
        }

after_damage:
        /* Check if HP has been depleted (negative or zero) */
        if (cs->current_hp == 0 || cs->current_hp > 0x8000) {
            /* Already unconscious? Skip */
            if (affliction == 1) goto next_member;

            /* Clear affliction bytes 0-4 only (assembly loop: counter < 5, i.e. i=0..4).
             * afflictions[5] (SHIELDS/PSI status) and afflictions[6] are NOT cleared. */
            memset(cs->afflictions, 0, 5);
            cs->afflictions[0] = 1;  /* Unconscious */
            cs->current_hp_target = 0;
            cs->current_hp = 0;

            /* Set entity script var3 = 16 to trigger KO animation.
             * char_struct.unknown59 = entity slot for this character. */
            uint16_t entity_slot = cs->unknown59;
            entities.var[3][entity_slot] = 16;
            ko_count++;
            goto next_member;
        }

        /* Accumulate HP for surviving, non-affliction-2 members */
        if (affliction != 2) {
            total_hp += cs->current_hp;
        }

next_member:
        (void)0;
    }

    /* If any damage ticked, flash the screen red */
    if (damage_events > 0) {
        start_enemy_touch_flash();
    }

    /* If any party member was KO'd, update party state */
    if (ko_count > 0) {
        bt.party_members_alive_overworld = 0;
        update_party();
        refresh_party_entities();
        enable_all_entities();
    }

    return total_hp;
}

/* ====================================================================
 * SPAWN system — Game Over / Comeback sequence
 * Port of:
 *   SPAWN                              — asm/overworld/spawn.asm
 *   INITIALIZE_GAME_OVER_SCREEN        — asm/misc/initialize_game_over_screen.asm
 *   PLAY_COMEBACK_SEQUENCE             — asm/misc/play_comeback_sequence.asm
 *   SKIPPABLE_PAUSE                    — asm/text/skippable_pause.asm
 *   LOAD_MAP_PALETTE_ANIMATION_FRAME   — asm/system/palette/load_map_palette_animation_frame.asm
 *   INITIALIZE_MAP_PALETTE_FADE        — asm/overworld/initialize_map_palette_fade.asm
 *   UPDATE_MAP_PALETTE_FADE            — asm/system/palette/update_map_palette_fade.asm
 *   ANIMATE_MAP_PALETTE_CHANGE         — asm/system/palette/animate_map_palette_change.asm
 *   FADE_PALETTE_TO_WHITE              — asm/system/palette/fade_palette_to_white.asm
 *   ANIMATE_PALETTE_FADE_WITH_RENDERING — asm/system/palette/animate_palette_fade_with_rendering.asm
 * ==================================================================== */

/* VRAM word addresses for game over screen (from include/enums.asm) */
#define VRAM_GAME_OVER_L1_TILES    0x0000
#define VRAM_GAME_OVER_L1_TILEMAP  0x5800
#define VRAM_GAME_OVER_L2_TILES    0x6000
#define VRAM_GAME_OVER_L2_TILEMAP  0x7C00

/* Music track: "You Lose" (from include/constants/music.asm) */
#define MUSIC_YOU_LOSE  7

/* Event flag for "No Continue" selected (flag 475 = FLG_SYS_COMEBACK) */
#define EVENT_FLAG_NOCONTINUE_SELECTED  475

/* Comeback message text ROM address */
#define MSG_SYS_COMEBACK  0xC7DE7Du

/* Buffer offsets for INITIALIZE_MAP_PALETTE_FADE / UPDATE_MAP_PALETTE_FADE.
 * These use the ert.buffer[] at offsets $7800-$7EFF for palette staging and
 * per-channel 8.8 fixed-point accumulators/increments.
 * 96 colors x 2 bytes each = 192 bytes per array. */
#define COLORS_PER_GROUP  (BPP4PALETTE_SIZE / COLOUR_SIZE)  /* 16 */
#define PALETTE_GROUP(n)  ((n) * COLORS_PER_GROUP)

/* Map palette fade constants from buffer_layout.h (BUF_MAP_FADE_*) */

/* ---- Helper: 8.8 fixed-point fade slope per color channel ---- */
static int16_t get_map_colour_fade_slope(int16_t current, int16_t target,
                                         int16_t frames) {
    if (frames <= 0) return 0;
    int32_t diff = (int32_t)(target - current) << 8;
    return (int16_t)(diff / frames);
}

/* ---- SKIPPABLE_PAUSE (port of asm/text/skippable_pause.asm) ----
 * Waits for 'frames' vblanks. Returns -1 if any button pressed, 0 when done. */
int16_t skippable_pause(uint16_t frames) {
    while (frames != 0) {
        if (core.pad1_pressed)
            return -1;
        wait_for_vblank();
        frames--;
    }
    return 0;
}

/* ---- LOAD_MAP_PALETTE_ANIMATION_FRAME (port of asm/system/palette/load_map_palette_animation_frame.asm) ----
 * Copies current palette groups 2-7 into mf->target as a base,
 * then swaps two sub-palettes within the staging area based on frame_index.
 *
 * Assembly logic:
 *   1. Copy PALETTES[32..127] (groups 2-7, 96 colors) to BUFFER+$7800
 *   2. Copy live palette group 7 (ert.palettes[112]) to buffer at frame_index position
 *   3. Copy live palette group 6 (ert.palettes[96]) to buffer at (frame_index-1) position
 */
void load_map_palette_animation_frame(uint16_t frame_index) {
    MapPaletteFadeBuffer *mf = buf_map_palette_fade(ert.buffer);

    /* Step 1: Copy palette groups 2-7 to staging buffer */
    memcpy(mf->target,
           &ert.palettes[PALETTE_GROUP(2)],
           BPP4PALETTE_SIZE * 6);

    /* Step 2: Copy live palette group 7 to buffer at position frame_index */
    memcpy(&mf->target[frame_index * COLORS_PER_GROUP],
           &ert.palettes[PALETTE_GROUP(7)],
           BPP4PALETTE_SIZE);

    /* Step 3: Copy live palette group 6 to buffer at position (frame_index-1) */
    memcpy(&mf->target[(frame_index - 1) * COLORS_PER_GROUP],
           &ert.palettes[PALETTE_GROUP(6)],
           BPP4PALETTE_SIZE);
}

/* ---- INITIALIZE_MAP_PALETTE_FADE (port of asm/overworld/initialize_map_palette_fade.asm) ----
 * For each of 96 colors in palette groups 2-7, compute per-channel 8.8 fixed-point
 * accumulators and increments for fading from current ert.palettes[] toward ert.buffer[BUF_MAP_FADE_TARGET].
 */
void initialize_map_palette_fade(uint16_t frames) {
    MapPaletteFadeBuffer *mf = buf_map_palette_fade(ert.buffer);

    for (int i = 0; i < BUF_MAP_FADE_COLOR_COUNT; i++) {
        /* Current palette color (groups 2-7 start at palette group 2) */
        uint16_t cur = ert.palettes[PALETTE_GROUP(2) + i];
        /* Target color from staging buffer */
        uint16_t tgt = mf->target[i];

        int16_t cur_r = cur & 0x1F;
        int16_t cur_g = (cur >> 5) & 0x1F;
        int16_t cur_b = (cur >> 10) & 0x1F;

        int16_t tgt_r = tgt & 0x1F;
        int16_t tgt_g = (tgt >> 5) & 0x1F;
        int16_t tgt_b = (tgt >> 10) & 0x1F;

        /* Compute 8.8 fixed-point increments */
        int16_t slope_r = get_map_colour_fade_slope(cur_r, tgt_r, (int16_t)frames);
        int16_t slope_g = get_map_colour_fade_slope(cur_g, tgt_g, (int16_t)frames);
        int16_t slope_b = get_map_colour_fade_slope(cur_b, tgt_b, (int16_t)frames);

        /* Store slopes */
        mf->slope_r[i] = slope_r;
        mf->slope_g[i] = slope_g;
        mf->slope_b[i] = slope_b;

        /* Initialize 8.8 accumulators from current channel values */
        int16_t acc_r = cur_r << 8;
        int16_t acc_g = cur_g << 8;
        int16_t acc_b = cur_b << 8;

        mf->accum_r[i] = acc_r;
        mf->accum_g[i] = acc_g;
        mf->accum_b[i] = acc_b;
    }
}

/* ---- UPDATE_MAP_PALETTE_FADE (port of asm/system/palette/update_map_palette_fade.asm) ----
 * Applies one frame of the per-channel palette fade. Adds slopes to accumulators,
 * extracts the high bytes as 5-bit channel values, reconstructs BGR555 colors,
 * and writes to ert.palettes[32..127]. Sets ert.palette_upload_mode = 8. */
void update_map_palette_fade(void) {
    MapPaletteFadeBuffer *mf = buf_map_palette_fade(ert.buffer);

    for (int i = 0; i < BUF_MAP_FADE_COLOR_COUNT; i++) {
        /* Read and accumulate each channel */
        int16_t acc_r = mf->accum_r[i];
        acc_r += mf->slope_r[i];
        mf->accum_r[i] = acc_r;

        int16_t acc_g = mf->accum_g[i];
        acc_g += mf->slope_g[i];
        mf->accum_g[i] = acc_g;

        int16_t acc_b = mf->accum_b[i];
        acc_b += mf->slope_b[i];
        mf->accum_b[i] = acc_b;

        /* Extract high byte of each accumulator, mask to 5 bits */
        uint16_t r = (uint16_t)((acc_r >> 8) & 0x1F);
        uint16_t g = (uint16_t)((acc_g >> 8) & 0x1F);
        uint16_t b = (uint16_t)((acc_b >> 8) & 0x1F);

        /* Reconstruct BGR555 and write to ert.palettes */
        ert.palettes[32 + i] = r | (g << 5) | (b << 10);
    }

    ert.palette_upload_mode = PALETTE_UPLOAD_BG_ONLY;
}

/* ---- ANIMATE_MAP_PALETTE_CHANGE (port of asm/system/palette/animate_map_palette_change.asm) ----
 * Stages palette data, runs a fade over 'frames' vblanks (skippable by button press),
 * then copies the final staged palette back to live ert.palettes.
 * Returns -1 if button pressed (skipped), 0 if completed normally. */
int16_t animate_map_palette_change(uint16_t frame_index, uint16_t frames) {
    load_map_palette_animation_frame(frame_index);
    initialize_map_palette_fade(frames);

    for (uint16_t f = frames; f != 0; f--) {
        if (core.pad1_pressed)
            return -1;
        update_map_palette_fade();
        wait_for_vblank();
    }

    /* Copy staged palette (mf->target, 6 sub-palettes) back to
     * live ert.palettes groups 2-7. Assembly: MEMCPY16(BUFFER+$7800, PALETTES+64, 192) */
    MapPaletteFadeBuffer *mf = buf_map_palette_fade(ert.buffer);
    memcpy(&ert.palettes[PALETTE_GROUP(2)], mf->target, BPP4PALETTE_SIZE * 6);
    return 0;
}

/* ---- FADE_PALETTE_TO_WHITE (port of asm/system/palette/fade_palette_to_white.asm) ----
 * Fades all 256 palette entries toward white over 'frames' vblanks,
 * then fills the entire palette with $FFFF (white). */
void fade_palette_to_white(uint16_t frames) {
    /* Load current ert.palettes to fade ert.buffer (level=100 = full brightness).
     * Assembly: LOAD_PALETTE_TO_FADE_BUFFER(100) */
    load_palette_to_fade_buffer(100);

    /* Prepare slopes for all palette groups (mask $FFFF = all groups).
     * Assembly: PREPARE_PALETTE_FADE(frames, $FFFF) */
    prepare_palette_fade_slopes((int16_t)frames, 0xFFFF);

    /* Run the fade */
    for (uint16_t f = 0; f < frames; f++) {
        update_map_palette_animation();
        wait_for_vblank();
    }

    /* Fill all ert.palettes with white ($FFFF).
     * Assembly: SEP; LDA #$FF; MEMSET16(PALETTES, BPP4PALETTE_SIZE * 16) */
    memset(ert.palettes, 0xFF, 256 * sizeof(uint16_t));

    /* Set palette upload mode = 24 (full) */
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    wait_for_vblank();
}

/* ---- ANIMATE_PALETTE_FADE_WITH_RENDERING (port of asm/system/palette/animate_palette_fade_with_rendering.asm) ----
 * Runs a palette fade over 'frames' vblanks with full entity rendering each frame.
 * Assembly: PREPARE_PALETTE_FADE(frames, $FFFF), then loop with
 *           UPDATE_MAP_PALETTE_ANIMATION + OAM_CLEAR + RUN_ACTIONSCRIPT_FRAME +
 *           UPDATE_SCREEN + WAIT_UNTIL_NEXT_FRAME, then FINALIZE_PALETTE_FADE. */
void animate_palette_fade_with_rendering(uint16_t frames) {
    prepare_palette_fade_slopes((int16_t)frames, 0xFFFF);

    for (uint16_t f = 0; f < frames; f++) {
        update_map_palette_animation();
        oam_clear();
        run_actionscript_frame();
        update_screen();
        wait_for_vblank();
    }

    finalize_palette_fade();
}

/* ---- INITIALIZE_GAME_OVER_SCREEN (port of asm/misc/initialize_game_over_screen.asm) ----
 * Displays the "You Lose" screen. Plays game-over music if all party members
 * are dead, decompresses game-over graphics/tilemap/palette, sets up VRAM,
 * loads UI state, fades in. */
void initialize_game_over_screen(void) {
    if (!bt.party_members_alive_overworld) {
        /* Play "You Lose" music and fade out with mosaic */
        change_music(MUSIC_YOU_LOSE);

        /* FADE_OUT_WITH_MOSAIC(step=1, delay=1, mosaic_enable=0)
         * Assembly: LDY #0; LDX #1; TXA; JSL FADE_OUT_WITH_MOSAIC */
        fade_out(1, 1);
        wait_for_fade_complete();
    }

    /* Clear overworld state */
    ml.loaded_animated_tile_count = 0;
    ml.map_palette_animation_loaded = 0;
    reset_item_transformations();
    ppu_clear_effects();

    /* SET_BGMODE(9): mode 1, BG3 priority */
    ppu.bgmode = 0x09;

    /* SET_BG1_VRAM_LOCATION(tiles=$0000, tilemap=$5800, size=NORMAL)
     * bg_sc[0] = (tilemap >> 8) | size = $58 | 0 = $58
     * bg_nba[0] low nibble = tile_base >> 12 = 0 */
    ppu.bg_sc[0] = (uint8_t)(VRAM_GAME_OVER_L1_TILEMAP >> 8);
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | (uint8_t)(VRAM_GAME_OVER_L1_TILES >> 12);

    /* SET_BG3_VRAM_LOCATION(tiles=$6000, tilemap=$7C00, size=NORMAL) */
    ppu.bg_sc[2] = (uint8_t)(VRAM_GAME_OVER_L2_TILEMAP >> 8);
    ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | (uint8_t)(VRAM_GAME_OVER_L2_TILES >> 12);

    /* Decompress game-over graphics (BINARY E1CFAF.gfx.lzhal) directly to VRAM.
     * The asset contains two 32KB images: normal (offset 0) and
     * Paula-leader variant (offset $8000).
     *
     * Intentional divergence from assembly: the SNES stages through BUFFER
     * then DMAs to VRAM. We decompress directly to ppu.vram (a byte array),
     * eliminating the 64KB intermediate buffer requirement. */
    {
        size_t comp_size = ASSET_SIZE(ASSET_E1CFAF_GFX_LZHAL);
        const uint8_t *comp_data = ASSET_DATA(ASSET_E1CFAF_GFX_LZHAL);
        if (comp_data) {
            uint8_t *vram_dst = &ppu.vram[VRAM_GAME_OVER_L1_TILES * 2];
            decomp(comp_data, comp_size, vram_dst, 0x10000);

            /* Choose graphics variant based on party leader.
             * Assembly: if party_members[0] == 3 (Paula), use offset $8000.
             * If Paula variant, move it down to the tile base. */
            if ((game_state.party_members[0] & 0xFF) == 3)
                memmove(vram_dst, vram_dst + 0x8000, 0x8000);
        }
    }

    /* Decompress tilemap (BINARY E1D5E8.arr.lzhal) directly to VRAM.
     * Assembly: COPY_TO_VRAM1P $06, GAME_OVER_LAYER_1_TILEMAP, 2048, 0 */
    {
        size_t comp_size = ASSET_SIZE(ASSET_E1D5E8_ARR_LZHAL);
        const uint8_t *comp_data = ASSET_DATA(ASSET_E1D5E8_ARR_LZHAL);
        if (comp_data) {
            uint8_t *vram_dst = &ppu.vram[VRAM_GAME_OVER_L1_TILEMAP * 2];
            decomp(comp_data, comp_size, vram_dst, 2048);
        }
    }

    /* Decompress palette (BINARY E1D4F4.pal.lzhal).
     * Assembly decompresses to PALETTES ($7E0200 = wram offset $0200), then:
     *   1. MEMCPY16(src=PALETTES start, dst=$02E0, count=$0020)
     *      -> copy ert.palettes[0..15] (group 0) to ert.palettes[112..127] (group 7)
     *   2. MEMSET16(dst=$0220, byte=0, count=$00C0)
     *      -> zero ert.palettes[16..111] (groups 1-6, 192 bytes)
     *   3. MEMCPY16(src=$02E0, dst=$0240, count=$0020)
     *      -> copy ert.palettes[112..127] to ert.palettes[32..47] (group 2)
     *
     * Net result: group 0 = original, groups 1,3-6 = black, group 2 = copy of group 0,
     * group 7 = copy of group 0, groups 8-15 = from decompressed data. */
    {
        size_t comp_size;
        comp_size = ASSET_SIZE(ASSET_E1D4F4_PAL_LZHAL);
        const uint8_t *comp_data = ASSET_DATA(ASSET_E1D4F4_PAL_LZHAL);
        if (comp_data) {
            decomp(comp_data, comp_size, (uint8_t *)ert.palettes, 512);

            /* Step 1: Copy group 0 to group 7 */
            uint16_t save_buf[COLORS_PER_GROUP];
            memcpy(save_buf, &ert.palettes[PALETTE_GROUP(0)], BPP4PALETTE_SIZE);
            memcpy(&ert.palettes[PALETTE_GROUP(7)], save_buf, BPP4PALETTE_SIZE);

            /* Step 2: Zero groups 1-6 */
            memset(&ert.palettes[PALETTE_GROUP(1)], 0, BPP4PALETTE_SIZE * 6);

            /* Step 3: Copy group 7 (= original group 0) to group 2 */
            memcpy(&ert.palettes[PALETTE_GROUP(2)], &ert.palettes[PALETTE_GROUP(7)], BPP4PALETTE_SIZE);
        }
    }

    /* Initialize battle UI state and load window graphics */
    initialize_battle_ui_state();
    text_load_window_gfx();

    /* Upload text tiles to VRAM (mode 1).
     * Assembly: LDA #1; JSL UPLOAD_TEXT_TILES_TO_VRAM */
    upload_text_tiles_to_vram(1);

    /* Load character window palette */
    load_character_window_palette();

    /* Set palette upload mode = 24 (full) */
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;

    /* TM_MIRROR = $05 (BG1 + BG3 on main screen, BG2 off, OBJ off) */
    ppu.tm = 0x05;

    /* Clear state.
     * Assembly clears BG2 and BG1 scroll (lines 98-101, note: line 101 is a
     * duplicate STZ BG1_X_POS — likely intended as BG1_Y_POS).
     * Also clear BG3 scroll: BG3 is the text layer (TM=$05 = BG1+BG3), and
     * non-zero BG3 scroll from battle causes the dialogue window to wrap. */
    bt.party_members_alive_overworld = 0;
    ppu.bg_hofs[0] = 0;  /* BG1_X_POS */
    ppu.bg_vofs[0] = 0;  /* BG1_Y_POS */
    ppu.bg_hofs[1] = 0;  /* BG2_X_POS */
    ppu.bg_vofs[1] = 0;  /* BG2_Y_POS */
    ppu.bg_hofs[2] = 0;  /* BG3_X_POS — prevents text window horizontal wrapping */
    ppu.bg_vofs[2] = 0;  /* BG3_Y_POS */

    /* Fade in (step=1, delay=1) and wait for completion */
    fade_in(1, 1);
    wait_for_fade_complete();
}

/* ---- PLAY_COMEBACK_SEQUENCE (port of asm/misc/play_comeback_sequence.asm) ----
 * Displays the comeback dialogue (MSG_SYS_COMEBACK) and runs 4 palette
 * animation phases with skippable pauses between them.
 * Returns -1 if player chose "Continue", 0 if "No Continue". */
int16_t play_comeback_sequence(void) {
    /* Initial pause (60 frames) */
    skippable_pause(60);

    /* Display comeback message text.
     * Assembly: DISPLAY_TEXT_PTR MSG_SYS_COMEBACK */
    display_text_from_snes_addr(MSG_SYS_COMEBACK);

    /* Close all windows and hide HPPP.
     * Assembly: JSL CLOSE_ALL_WINDOWS_AND_HIDE_HPPP */
    close_all_windows();
    window_tick();
    hide_hppp_windows();
    window_tick();

    /* Assembly: GET_EVENT_FLAG(EVENT_FLAG_NOCONTINUE_SELECTED); CMP #0; BNE @NO_CONTINUE
     * If flag NOT set -> player chose "Continue" -> pause 60, return -1.
     * If flag IS set -> player chose "No Continue" -> run fade phases, return 0. */
    if (!event_flag_get(EVENT_FLAG_NOCONTINUE_SELECTED)) {
        /* Player chose "Continue" — just pause and return -1 */
        skippable_pause(60);
        return -1;
    }

    /* Player chose "No Continue" — run 4 palette fade phases */

    /* 60-frame pause, then palette animation phase 1 (90 frames) */
    if (skippable_pause(60) != 0) return 0;
    if (animate_map_palette_change(1, 90) != 0) return 0;
    if (skippable_pause(1) != 0) return 0;

    /* Phase 2: palette animation phase 2 (90 frames) */
    if (animate_map_palette_change(2, 90) != 0) return 0;
    if (skippable_pause(1) != 0) return 0;

    /* Phase 3: palette animation phase 3 (90 frames) */
    if (animate_map_palette_change(3, 90) != 0) return 0;
    if (skippable_pause(1) != 0) return 0;

    /* Phase 4: palette animation phase 4 (8 frames) */
    if (animate_map_palette_change(4, 8) != 0) return 0;

    return 0;
}

/* ---- SPAWN (port of asm/overworld/spawn.asm) ----
 * Game over / comeback sequence. Called when all party members are KO'd.
 * Saves respawn position, shows game over screen, runs comeback sequence,
 * then either returns to overworld (continue) or reinitializes the map (no continue). */
int16_t spawn(void) {
    uint16_t saved_x = ow.respawn_x;
    uint16_t saved_y = ow.respawn_y;

    disable_all_entities();
    initialize_game_over_screen();
    int16_t comeback_result = play_comeback_sequence();

    if (comeback_result != 0) {
        /* Player chose "Continue" (result == -1) — fade out and return.
         * Assembly: CMP #0; BEQ @COMEBACK_SEQUENCE_FAILED; FADE_OUT_WITH_MOSAIC */
        fade_out(2, 1);
        wait_for_fade_complete();
        enable_all_entities();
        return comeback_result;
    }

    /* Player chose "No Continue" — reinitialize the world */

    /* Fade all ert.palettes to white (32 frames) */
    fade_palette_to_white(32);

    /* Stop audio effects.
     * Assembly: LDA #2; JSL WRITE_APU_PORT1 */
    write_apu_port1(2);

    /* TM_MIRROR = $17 (all layers + OBJ on main screen) */
    ppu.tm = 0x17;

    /* Invalidate map/music caches so they reload.
     * Assembly: LDA #-1; STA LOADED_MAP_TILE_COMBO / CURRENT_MAP_MUSIC_TRACK / CURRENT_MUSIC_TRACK */
    ow.loaded_map_tile_combo = -1;
    ml.current_map_music_track = (uint16_t)-1;
    audio_invalidate_music_cache();

    /* Set flag to wipe ert.palettes on next map load */
    dr.wipe_palettes_on_map_load = 1;

    wait_for_vblank();

    /* Reinitialize the map at the respawn position.
     * Assembly: LDA ow.respawn_x; LDX ow.respawn_y; LDY #6; JSL INITIALIZE_MAP */
    initialize_map(saved_x, saved_y, 6 /* direction = LEFT */);

    /* Compute leader character struct pointer.
     * Assembly: party_members[0]-1 -> index into party_characters[] */
    int leader_char_id = game_state.party_members[0] & 0xFF;
    CharStruct *leader_cs = &party_characters[leader_char_id - 1];

    /* Clear afflictions for leader (6 groups, matching assembly loop count).
     * Assembly loops i=0..5 with SEP #$20; STZ char_struct::afflictions,X */
    for (int i = 0; i < 6; i++)
        leader_cs->afflictions[i] = 0;

    /* Restore leader HP to max, zero PP.
     * Assembly: max_hp -> current_hp_target -> current_hp; 0 -> current_pp_target -> current_pp */
    leader_cs->current_hp_target = leader_cs->max_hp;
    leader_cs->current_hp = leader_cs->max_hp;
    leader_cs->current_pp_target = 0;
    leader_cs->current_pp = 0;

    /* Halve money (round up).
     * Assembly: DIVISION32(money, 2) + (money & 1) = ceiling(money / 2) */
    game_state.money_carried = (game_state.money_carried + 1) / 2;

    refresh_party_entities();

    /* Clear event flags 1-10 (set each to 0).
     * Assembly: loop Y=1..10, SET_EVENT_FLAG(flag=Y, value=0) */
    for (uint16_t flag = 1; flag <= 10; flag++)
        event_flag_clear(flag);

    /* Clear all entity collision objects.
     * Assembly: loop 0..MAX_ENTITIES-1. */
    for (int slot = 0; slot < MAX_ENTITIES; slot++)
        entities.collided_objects[slot] = -1;

    /* Reset interaction state */
    reset_queued_interactions();
    ow.dad_phone_queued = 0;
    ow.player_intangibility_frames = 0;

    /* Spawn buzz buzz entities and any pending deliveries */
    spawn_buzz_buzz();

    oam_clear();
    enable_all_entities();

    /* Animate palette fade with rendering (32 frames) */
    animate_palette_fade_with_rendering(32);

    return comeback_result;
}
