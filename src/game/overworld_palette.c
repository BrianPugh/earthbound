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
#include "game/display_text_internal.h"  /* dt_make_child_init */
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
#include "core/mode_stack.h"
#include "core/log.h"
#include "game_main.h"
#include <string.h>
#include "data/text_refs.h"

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
 * Checks whether a party member's HP has fallen below 20% of max. Returns true
 * if the "[name]'s HP is very low!" warning should be shown now (it had not been
 * shown yet) — the caller (the damage loop) shows GAME_MODE_HP_ALERT, either via
 * a pump (blocking wrapper) or a STEP_PUSH (the overworld root mode). The
 * already-shown latch (ow.hp_alert_shown) is set here so a resume does not
 * re-show, matching the assembly which set it after the message returned.
 *
 * party_index: 0-based index into party_order / player_controlled_party_members.
 */
static bool ow_check_low_hp(uint16_t party_index) {
    uint8_t char_id = game_state.player_controlled_party_members[party_index];
    CharStruct *cs = &party_characters[char_id];

    /* Threshold = 20% of max HP = max_hp * 20 / 100 */
    uint16_t threshold = (uint16_t)((uint32_t)cs->max_hp * 20 / 100);
    if (cs->current_hp < threshold) {
        /* HP is low — show alert if not already shown */
        if (!ow.hp_alert_shown[party_index]) {
            ow.hp_alert_shown[party_index] = 1;
            return true;
        }
    } else {
        /* HP is OK — clear the alert flag */
        ow.hp_alert_shown[party_index] = 0;
    }
    return false;
}

/* ---- GAME_MODE_HP_ALERT step (run-to-completion port of SHOW_HP_ALERT) ----
 * See GAME_MODE_HP_ALERT in core/mode_stack.h. Assembly: disable entities ->
 * open window -> set attacker name -> DISPLAY_TEXT_PTR
 * MSG_SYS_MAP_CRITICAL_SITUATION -> close window -> WINDOW_TICK -> enable. */
StepResult mode_step_hp_alert(ModeState *st) {
    HpAlertState *hs = &st->hp_alert;

    switch ((HpAlertPhase)hs->phase) {
    case HA_TEXT: {
        uint8_t char_id = game_state.player_controlled_party_members[hs->party_index];
        CharStruct *cs = &party_characters[char_id];
        disable_all_entities();
        create_window(0x01);  /* WINDOW::TEXT_STANDARD */
        set_battle_attacker_name((const char *)cs->name, sizeof(cs->name));
        hs->phase = HA_CLOSE;
        static ModeState dt_init;  /* outlives this dispatch (pump copies it) */
        if (dt_make_child_init(&dt_init, MSG_SYS_HP_CRITICAL_WARNING))
            return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &dt_init);
        LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n",
                 (uint32_t)MSG_SYS_HP_CRITICAL_WARNING);
        return STEP_RESULT_CONTINUE();
    }

    case HA_CLOSE:
        close_focus_window();
        window_tick_work();
        hs->phase = HA_DONE;
        return STEP_RESULT_CONTINUE();

    case HA_DONE:
    default:
        enable_all_entities();
        return STEP_RESULT_POP(0);
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
OwDamageStatus ow_damage_step(OwDamageState *s) {
    if (!s->started) {
        s->started = 1;
        s->i = 0;
        s->ko_count = 0;
        s->total_hp = 0;
        s->damage_events = 0;
        s->resume_alert = 0;

        /* Early exit: special camera mode or status suppressed (the assembly
         * returns 1 without running the loop or the post-loop flash/KO update). */
        if (game_state.camera_mode == 2) {
            s->total_hp = 1;
            return OW_DMG_DONE;
        }
        if (ow.overworld_status_suppression) {
            s->total_hp = 1;
            return OW_DMG_DONE;
        }
    }

    while (s->i < 6) {
        uint16_t i = s->i;

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

        if (s->resume_alert) {
            /* Re-entry after the member's HP_ALERT popped: skip damage
             * application and resume at the depletion check, exactly where the
             * assembly's `goto after_damage` would land after the message. */
            s->resume_alert = 0;
        } else {
            /* Skip unconscious (1) and affliction 2 (no after_damage check). */
            if (affliction == 1 || affliction == 2) {
                s->i++;
                continue;
            }

            bool need_alert = false;
            if (affliction == 5) {
                /* --- Poison damage: 10 HP every 120 frames --- */
                if (ow.overworld_damage_countdown_frames[i] > 0) {
                    ow.overworld_damage_countdown_frames[i]--;
                    if (ow.overworld_damage_countdown_frames[i] != 0)
                        goto after_damage;  /* no damage this frame */
                    /* Timer expired — apply damage */
                    s->damage_events++;
                    cs->current_hp -= 10;
                    cs->current_hp_target -= 10;
                    need_alert = ow_check_low_hp(i);
                } else {
                    /* Reset timer */
                    ow.overworld_damage_countdown_frames[i] = 120;
                }
            } else {
                /* --- Environmental/sunstroke damage --- */
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
                            goto after_damage;  /* no damage this frame */
                        /* Timer expired — apply damage */
                        s->damage_events++;
                        if (affliction == 4) {
                            /* Sunstroke: 10 HP damage */
                            cs->current_hp -= 10;
                            cs->current_hp_target -= 10;
                        } else {
                            /* Other environmental: 2 HP damage */
                            cs->current_hp -= 2;
                            cs->current_hp_target -= 2;
                        }
                        need_alert = ow_check_low_hp(i);
                    } else {
                        /* Reset timer: 120 frames for sunstroke, 240 for others */
                        ow.overworld_damage_countdown_frames[i] = (affliction == 4) ? 120 : 240;
                    }
                }
            }

            if (need_alert) {
                /* Yield to show the "[name]'s HP is very low!" warning. The
                 * caller pushes/pumps GAME_MODE_HP_ALERT for member s->i, then
                 * re-calls; resume_alert routes us to after_damage on re-entry. */
                s->resume_alert = 1;
                return OW_DMG_ALERT;
            }
        }

after_damage:
        /* Check if HP has been depleted (negative or zero) */
        if (cs->current_hp == 0 || cs->current_hp > 0x8000) {
            /* Already unconscious? Skip (no HP accumulation). */
            if (affliction != 1) {
                /* Clear affliction bytes 0-4 only (assembly loop: counter < 5).
                 * afflictions[5] (SHIELDS/PSI status) and [6] are NOT cleared. */
                memset(cs->afflictions, 0, 5);
                cs->afflictions[0] = 1;  /* Unconscious */
                cs->current_hp_target = 0;
                cs->current_hp = 0;

                /* Set entity script var3 = 16 to trigger KO animation.
                 * char_struct.unknown59 = entity slot for this character. */
                uint16_t entity_slot = cs->unknown59;
                entities.var[3][entity_slot] = 16;
                s->ko_count++;
            }
        } else if (affliction != 2) {
            /* Accumulate HP for surviving, non-affliction-2 members */
            s->total_hp += cs->current_hp;
        }

        s->i++;
    }

    /* If any damage ticked, flash the screen red */
    if (s->damage_events > 0) {
        start_enemy_touch_flash();
    }

    /* If any party member was KO'd, update party state */
    if (s->ko_count > 0) {
        bt.party_members_alive_overworld = 0;
        update_party();
        refresh_party_entities();
        enable_all_entities();
    }

    return OW_DMG_DONE;
}

/* The blocking update_overworld_damage() pump bridge (drive ow_damage_step() to
 * completion, pumping the low-HP warning inline) was deleted in D4b: the
 * GAME_MODE_OVERWORLD root drives ow_damage_step() itself and STEP_PUSHes
 * GAME_MODE_HP_ALERT on OW_DMG_ALERT. */

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

/* ---- GAME_MODE_PALETTE_FADE step ----
 * Run-to-completion form of the fixed-length palette-fade loops below. The single
 * yield is owned by the pump; this body never calls wait_for_vblank(). See the
 * PaletteFadeState comment in mode_stack.h. */
StepResult mode_step_palette_fade(ModeState *st) {
    PaletteFadeState *s = &st->palette_fade;

    if (s->phase == 1) {
        /* PF_TO_WHITE: the frame after the white-fill+upload was yielded. */
        return STEP_RESULT_POP(0);
    }

    switch ((PaletteFadeKind)s->kind) {
    case PF_SKIPPABLE_PAUSE:
        if (s->remaining == 0)
            return STEP_RESULT_POP(0);
        if (core.pad1_pressed)
            return STEP_RESULT_POP(-1);
        s->remaining--;
        return STEP_RESULT_CONTINUE();

    case PF_MAP_CHANGE:
        if (s->remaining == 0) {
            /* Copy the staged palette (mf->target, 6 sub-palettes) back to live
             * ert.palettes groups 2-7. Only on normal completion (not on skip). */
            MapPaletteFadeBuffer *mf = buf_map_palette_fade(ert.buffer);
            memcpy(&ert.palettes[PALETTE_GROUP(2)], mf->target, BPP4PALETTE_SIZE * 6);
            return STEP_RESULT_POP(0);
        }
        if (core.pad1_pressed)
            return STEP_RESULT_POP(-1);
        update_map_palette_fade();
        s->remaining--;
        return STEP_RESULT_CONTINUE();

    case PF_TO_WHITE:
        if (s->remaining == 0) {
            memset(ert.palettes, 0xFF, 256 * sizeof(uint16_t));
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            s->phase = 1;
            return STEP_RESULT_CONTINUE();   /* the extra white-fill yield */
        }
        update_map_palette_animation();
        s->remaining--;
        return STEP_RESULT_CONTINUE();

    case PF_WITH_RENDERING:
        if (s->remaining == 0) {
            finalize_palette_fade();
            return STEP_RESULT_POP(0);
        }
        update_map_palette_animation();
        oam_clear();
        run_actionscript_frame();
        update_screen();
        s->remaining--;
        return STEP_RESULT_CONTINUE();
    }

    return STEP_RESULT_POP(0);   /* unreachable */
}

/* SKIPPABLE_PAUSE (asm/text/skippable_pause.asm), ANIMATE_MAP_PALETTE_CHANGE,
 * FADE_PALETTE_TO_WHITE and ANIMATE_PALETTE_FADE_WITH_RENDERING were blocking pump
 * bridges over GAME_MODE_PALETTE_FADE. Their sole callers (spawn/comeback) became
 * the GAME_OVER mode (D1b), which STEP_PUSHes GAME_MODE_PALETTE_FADE (kinds
 * PF_SKIPPABLE_PAUSE / PF_MAP_CHANGE / PF_TO_WHITE / PF_WITH_RENDERING) directly,
 * so the bridges were deleted (D4b). The non-bridge setup helpers below
 * (load_map_palette_animation_frame / initialize_map_palette_fade /
 * update_map_palette_fade) are still called by the GAME_OVER mode. */

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

/* ---- INITIALIZE_GAME_OVER_SCREEN (port of asm/misc/initialize_game_over_screen.asm) ----
 * Displays the "You Lose" screen. Plays game-over music if all party members
 * are dead, decompresses game-over graphics/tilemap/palette, sets up VRAM,
 * loads UI state, fades in. Split into a begin/setup pair so GAME_MODE_GAME_OVER
 * can yield on the two embedded wait_for_fade_complete()s.
 *
 * game_over_screen_begin(): the music + fade-out front half. Returns true if a
 * fade-out was started (the caller must wait for it before the setup half). */
static bool game_over_screen_begin(void) {
    if (!bt.party_members_alive_overworld) {
        /* Play "You Lose" music and fade out with mosaic */
        change_music(MUSIC_YOU_LOSE);

        /* FADE_OUT_WITH_MOSAIC(step=1, delay=1, mosaic_enable=0)
         * Assembly: LDY #0; LDX #1; TXA; JSL FADE_OUT_WITH_MOSAIC */
        fade_out(1, 1);
        return true;
    }
    return false;
}

/* game_over_screen_setup(): the decompress/VRAM/palette/UI setup + the fade-in
 * start (the caller waits for the fade-in to complete). */
static void game_over_screen_setup(void) {
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
             * Assembly: if party_members[0] == JEFF, use offset $8000. */
            if ((game_state.party_members[0] & 0xFF) == PARTY_MEMBER_JEFF)
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

    /* Fade in (step=1, delay=1); the caller waits for completion. */
    fade_in(1, 1);
}

/* ---- GAME_MODE_GAME_OVER step ----
 * Run-to-completion port of spawn() (asm/overworld/spawn.asm) with
 * initialize_game_over_screen() and play_comeback_sequence()
 * (asm/misc/play_comeback_sequence.asm) inlined as phases. See GameOverPhase in
 * core/mode_stack.h. Pops -1 ("Continue") or 0 ("No Continue"). */
StepResult mode_step_game_over(ModeState *mst) {
    GameOverState *st = &mst->game_over;
    static ModeState child;  /* outlives this dispatch (pump/root copies it) */

    /* play_comeback_sequence's no-continue branch: an 8-step fade chain
     * (pause0, anim1, pause, anim2, pause, anim3, pause, anim4). Each entry is a
     * PALETTE_FADE child; a -1 pop (button skip) short-circuits to the tail
     * (the assembly's `if (... != 0) return 0;`). */
    static const struct { uint8_t pf_kind; uint8_t frame_index; uint16_t frames; } nc_seq[8] = {
        { PF_SKIPPABLE_PAUSE, 0, 60 },
        { PF_MAP_CHANGE,      1, 90 },
        { PF_SKIPPABLE_PAUSE, 0, 1  },
        { PF_MAP_CHANGE,      2, 90 },
        { PF_SKIPPABLE_PAUSE, 0, 1  },
        { PF_MAP_CHANGE,      3, 90 },
        { PF_SKIPPABLE_PAUSE, 0, 1  },
        { PF_MAP_CHANGE,      4, 8  },
    };

    for (;;) {
        switch ((GameOverPhase)st->phase) {
        case GO_ENTER:
            st->saved_x = ow.respawn_x;
            st->saved_y = ow.respawn_y;
            disable_all_entities();
            st->phase = GO_SETUP;
            if (game_over_screen_begin()) {
                /* fade-out started: wait for it before the setup half */
                child = (ModeState){0};
                child.fade_wait.tick_kind = FADE_TICK_SCREEN_ONLY;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_FADE_WAIT, &child);
            }
            continue;  /* party not all-dead: no fade, straight to setup */

        case GO_SETUP:
            game_over_screen_setup();
            /* fade-in started: wait for it */
            st->phase = GO_CB_PAUSE;
            child = (ModeState){0};
            child.fade_wait.tick_kind = FADE_TICK_SCREEN_ONLY;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_FADE_WAIT, &child);

        case GO_CB_PAUSE:
            /* play_comeback_sequence: initial skippable_pause(60), result ignored */
            st->phase = GO_CB_TEXT;
            child = (ModeState){0};
            child.palette_fade.kind      = PF_SKIPPABLE_PAUSE;
            child.palette_fade.remaining = 60;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_PALETTE_FADE, &child);

        case GO_CB_TEXT:
            /* DISPLAY_TEXT_PTR MSG_SYS_REVIVE_AFTER_KO (the comeback dialogue,
             * which sets EVENT_FLAG_NOCONTINUE_SELECTED on the player's choice) */
            st->phase = GO_CB_CLOSE1;
            if (dt_make_child_init(&child, MSG_SYS_REVIVE_AFTER_KO))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n",
                     (uint32_t)MSG_SYS_REVIVE_AFTER_KO);
            continue;

        case GO_CB_CLOSE1:
            /* CLOSE_ALL_WINDOWS_AND_HIDE_HPPP: close_all_windows + window_tick */
            close_all_windows();
            window_tick_work();
            st->phase = GO_CB_CLOSE2;
            return STEP_RESULT_CONTINUE();

        case GO_CB_CLOSE2:
            hide_hppp_windows();
            window_tick_work();
            st->phase = GO_CB_DECIDE;
            return STEP_RESULT_CONTINUE();

        case GO_CB_DECIDE:
            if (!event_flag_get(EVENT_FLAG_NOCONTINUE_SELECTED)) {
                /* "Continue": skippable_pause(60) (result ignored) then -1 */
                st->phase = GO_CONT_FADE;
                child = (ModeState){0};
                child.palette_fade.kind      = PF_SKIPPABLE_PAUSE;
                child.palette_fade.remaining = 60;
                return STEP_RESULT_PUSH_INIT(GAME_MODE_PALETTE_FADE, &child);
            }
            /* "No Continue": run the fade chain */
            st->nc_step = 0;
            st->phase = GO_NC_SEQ;
            continue;

        case GO_CONT_FADE:
            /* spawn() Continue path: FADE_OUT_WITH_MOSAIC(2,1) + wait */
            fade_out(2, 1);
            st->phase = GO_CONT_DONE;
            child = (ModeState){0};
            child.fade_wait.tick_kind = FADE_TICK_SCREEN_ONLY;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_FADE_WAIT, &child);

        case GO_CONT_DONE:
            enable_all_entities();
            return STEP_RESULT_POP(-1);

        case GO_NC_SEQ:
            if (st->nc_step >= 8) { st->phase = GO_NC_WHITE; continue; }
            child = (ModeState){0};
            child.palette_fade.kind      = nc_seq[st->nc_step].pf_kind;
            child.palette_fade.remaining = nc_seq[st->nc_step].frames;
            if (nc_seq[st->nc_step].pf_kind == PF_MAP_CHANGE) {
                /* animate_map_palette_change setup runs inline before the push */
                load_map_palette_animation_frame(nc_seq[st->nc_step].frame_index);
                initialize_map_palette_fade(nc_seq[st->nc_step].frames);
            }
            st->phase = GO_NC_SEQ_CHECK;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_PALETTE_FADE, &child);

        case GO_NC_SEQ_CHECK:
            if (mode_child_result() != 0) {
                /* button skip: play_comeback returns 0 -> jump to the tail */
                st->phase = GO_NC_WHITE;
            } else {
                st->nc_step++;
                st->phase = GO_NC_SEQ;
            }
            continue;

        case GO_NC_WHITE:
            /* spawn() No-Continue: fade_palette_to_white(32) */
            load_palette_to_fade_buffer(100);
            prepare_palette_fade_slopes(32, 0xFFFF);
            st->phase = GO_NC_REINIT;
            child = (ModeState){0};
            child.palette_fade.kind      = PF_TO_WHITE;
            child.palette_fade.remaining = 32;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_PALETTE_FADE, &child);

        case GO_NC_REINIT:
            /* Stop audio effects (WRITE_APU_PORT1 2); TM_MIRROR = $17; invalidate
             * map/music caches; wipe palettes on next map load. */
            write_apu_port1(2);
            ppu.tm = 0x17;
            ow.loaded_map_tile_combo    = -1;
            ml.current_map_music_track  = (uint16_t)-1;
            audio_invalidate_music_cache();
            dr.wipe_palettes_on_map_load = 1;
            /* wait_for_vblank() -> one CONTINUE before initialize_map */
            st->phase = GO_NC_MAP;
            return STEP_RESULT_CONTINUE();

        case GO_NC_MAP: {
            /* Reinitialize the map at the respawn position (direction LEFT). */
            initialize_map(st->saved_x, st->saved_y, 6);

            int leader_char_id = game_state.party_members[0] & 0xFF;
            CharStruct *leader_cs = &party_characters[leader_char_id - 1];
            for (int i = 0; i < 6; i++)
                leader_cs->afflictions[i] = 0;
            leader_cs->current_hp_target = leader_cs->max_hp;
            leader_cs->current_hp        = leader_cs->max_hp;
            leader_cs->current_pp_target = 0;
            leader_cs->current_pp        = 0;

            /* Halve money (round up). */
            game_state.money_carried = (game_state.money_carried + 1) / 2;
            refresh_party_entities();

            for (uint16_t flag = 1; flag <= 10; flag++)
                event_flag_clear(flag);
            for (int slot = 0; slot < MAX_ENTITIES; slot++)
                entities.collided_objects[slot] = -1;

            reset_queued_interactions();
            ow.dad_phone_queued = 0;
            ow.player_intangibility_frames = 0;

            /* spawn_buzz_buzz(): push the buzz-buzz check text (entity-spawn CC
             * codes gated by flags); spawn_delivery_entities() runs on resume,
             * exactly the door DTR_FINALIZE/DTR_BUZZ_DONE split. */
            st->phase = GO_NC_BUZZ_DONE;
            if (dt_make_child_init(&child, MSG_EVT0_BUZZBUZZ_CHECK))
                return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &child);
            LOG_WARN("WARNING: resolve_text_addr(0x%06X) returned NULL\n",
                     (uint32_t)MSG_EVT0_BUZZBUZZ_CHECK);
            continue;
        }

        case GO_NC_BUZZ_DONE:
            spawn_delivery_entities();
            oam_clear();
            enable_all_entities();
            /* animate_palette_fade_with_rendering(32) */
            prepare_palette_fade_slopes(32, 0xFFFF);
            st->phase = GO_NC_FINISH;
            child = (ModeState){0};
            child.palette_fade.kind      = PF_WITH_RENDERING;
            child.palette_fade.remaining = 32;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_PALETTE_FADE, &child);

        case GO_NC_FINISH:
        default:
            return STEP_RESULT_POP(0);
        }
    }
}

/* ---- SPAWN (port of asm/overworld/spawn.asm) ----
 * Game over / comeback sequence, run when the whole party is KO'd. The blocking
 * pump bridge is gone — the overworld root (OWP_LOOP_END, game_main.c) STEP_PUSHes
 * GAME_MODE_GAME_OVER directly (mode_step_game_over, above) and reads the pop
 * result (-1 "Continue" -> reboot / 0 "No Continue" -> world reinitialised). */
