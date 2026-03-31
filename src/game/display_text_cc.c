/*
 * Display text control code handlers.
 *
 * Extracted from display_text.c — all cc_* functions that handle
 * text script control codes (0x03-0x1F dispatchers and sub-handlers).
 */
#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "game/game_state.h"
#include "game/battle.h"
#include "game/inventory.h"
#include "game/audio.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/text.h"
#include "game/fade.h"
#include "game/door.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "game/position_buffer.h"
#include "data/assets.h"
#include "core/math.h"
#include "core/memory.h"
#include "core/log.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/pad.h"
#include "platform/platform.h"
#include "game_main.h"
#include "data/battle_text_data.h"
#include <string.h>

/* --- CC handler: 0x04 SET_EVENT_FLAG ---
 * Bytecode: 04 <flag_lo> <flag_hi> (2 arg bytes) */
void cc_set_event_flag(ScriptReader *r) {
    uint16_t flag = script_read_word(r);
    event_flag_set(flag);
}


/* --- CC handler: 0x05 CLEAR_EVENT_FLAG ---
 * Bytecode: 05 <flag_lo> <flag_hi> (2 arg bytes) */
void cc_clear_event_flag(ScriptReader *r) {
    uint16_t flag = script_read_word(r);
    event_flag_clear(flag);
}


/* --- CC handler: 0x10 PAUSE ---
 * Bytecode: 10 <frames> (1 arg byte)
 * Waits for N frames, rendering each frame. */
/* Port of CC_13_14 (asm/text/ccs/halt.asm).
 * Waits for player button press with optional blinking triangle animation.
 *
 * show_triangle: 1 = draw blinking triangle at window bottom-right, 0 = no visual.
 * skip_text_speed: 1 = always wait for button, 0 = allow text speed shortcut.
 *
 * CC 0x03 (HALT_WITH_PROMPT):        show_triangle=1, skip_text_speed=0
 * CC 0x13 (HALT_WITHOUT_PROMPT):     show_triangle=0, skip_text_speed=0
 * CC 0x14 (HALT_WITH_PROMPT_ALWAYS): show_triangle=1, skip_text_speed=1
 */
void cc_halt(int show_triangle, int skip_text_speed) {
    /* Wait for dt.text_prompt_waiting_for_input to clear (assembly lines 26-27) */
    while (dt.text_prompt_waiting_for_input) {
        render_frame_tick();
        if (platform_input_quit_requested()) return;
    }

    /* Assembly (halt.asm:29-30): CLEAR_INSTANT_PRINTING then WINDOW_TICK.
     * Must call window_tick() (not render_frame_tick()) so that windows
     * are rendered to win.bg2_buffer before the frame is presented. */
    clear_instant_printing();
    window_tick();

    /* Text speed shortcut (assembly lines 31-39):
     * If not forced to wait, and triangle flag is set, and text speed is non-zero,
     * do the text speed delay and return without waiting for button press.
     * This is the "auto-advance" behavior for non-instant text speed. */
    if (!skip_text_speed && dt.blinking_triangle_flag && dt.text_speed_based_wait) {
        uint16_t frames = dt.text_speed_based_wait;
        for (uint16_t i = 0; i < frames; i++) {
            update_hppp_meter_and_render();
            if (platform_input_quit_requested()) return;
            if (core.pad1_pressed & PAD_TEXT_ADVANCE) break;
        }
        return;
    }

    /* PAUSE_MUSIC effect (assembly lines 41-43):
     * If triangle flag is set, disable HPPP rolling during the wait. */
    if (dt.blinking_triangle_flag) {
        bt.disable_hppp_rolling = 1;
    }

    WindowInfo *w = get_focus_window_info();

    if (!show_triangle || !w) {
        /* No triangle — wait for button press (assembly lines 57-63) */
        do {
            update_hppp_meter_and_render();
            if (platform_input_quit_requested()) break;
        } while (!(core.pad1_pressed & PAD_TEXT_ADVANCE));
    } else {
        /* Blinking triangle animation (assembly lines 64-162).
         * Triangle tilemap entries from asm/data/text/blinking_triangle_tiles.asm:
         *   Frame 0 (big):   0x3C14 — priority 1, palette 6, tile 0x14
         *   Frame 1 (small): 0x3C15 — priority 1, palette 6, tile 0x15
         *   Clear:           0xBC11 — priority 1, palette 6, vflip, tile 0x11 */
        #define TRIANGLE_TILE_BIG   0x3C14
        #define TRIANGLE_TILE_SMALL 0x3C15
        #define TRIANGLE_TILE_CLEAR 0xBC11

        /* Calculate VRAM position: bottom-right of window border.
         * Assembly: (y + height) * 32 + (x + width) + TEXT_LAYER_TILEMAP + 32
         * where asm height/width are content dims (C port's height/width minus 2). */
        uint16_t *tilemap = (uint16_t *)&ppu.vram[VRAM_TEXT_LAYER_TILEMAP * 2];
        uint16_t tri_pos = (uint16_t)((w->y + w->height - 1) * 32 + (w->x + w->width - 2));

        bool done = false;
        while (!done) {
            /* Frame 0: big triangle, 15 ticks (assembly lines 64-100) */
            tilemap[tri_pos] = TRIANGLE_TILE_BIG;
            for (int i = 15; i > 0; i--) {
                if (core.pad1_pressed & PAD_TEXT_ADVANCE) {
                    /* Button during frame 0: clear tile (assembly lines 138-162) */
                    tilemap[tri_pos] = TRIANGLE_TILE_CLEAR;
                    done = true;
                    break;
                }
                update_hppp_meter_and_render();
                if (platform_input_quit_requested()) { done = true; break; }
            }
            if (done) break;

            /* Frame 1: small triangle, 10 ticks (assembly lines 101-136) */
            tilemap[tri_pos] = TRIANGLE_TILE_SMALL;
            for (int i = 10; i > 0; i--) {
                if (core.pad1_pressed & PAD_TEXT_ADVANCE) {
                    done = true;
                    break;
                }
                update_hppp_meter_and_render();
                if (platform_input_quit_requested()) { done = true; break; }
            }
        }

        #undef TRIANGLE_TILE_BIG
        #undef TRIANGLE_TILE_SMALL
        #undef TRIANGLE_TILE_CLEAR
    }

    /* RESUME_MUSIC (assembly lines 163-164): clear meter speed flags */
    bt.half_hppp_meter_speed = 0;
    bt.disable_hppp_rolling = 0;
}


void cc_pause(ScriptReader *r) {
    uint8_t frames = script_read_byte(r);
    /* Port of CC_10 → TICK_HPPP_METER_N_FRAMES (C100D6).
     * Assembly renders (frames + 1) total frames:
     *   1st frame via WINDOW_TICK, remaining via UPDATE_HPPP_METER_AND_RENDER.
     * Both paths call RENDER_FRAME_TICK internally.
     * Using update_hppp_meter_and_render() for HP/PP roller animation. */
    /* Assembly: JSR CLEAR_INSTANT_PRINTING; JSL WINDOW_TICK.
     * WINDOW_TICK leaves dt.instant_printing=0 (no re-enable). */
    clear_instant_printing();
    window_tick();
    for (int i = 0; i < (int)frames; i++) {
        update_hppp_meter_and_render();
        if (platform_input_quit_requested()) break;
    }
}


/* --- CC 0x1F tree handlers (attract mode) --- */

/* CC_1F_11: ADD_PARTY_MEMBER — 1 arg byte
 *
 * Port of CC_1F_11 (asm/text/ccs/party_member_add.asm).
 * Reads char_id arg (0 → argument_memory).
 * Calls ADD_CHAR_TO_PARTY (asm/misc/party_add_char.asm) which does:
 *   - Sorted insertion into party_members[]
 *   - Entity tick/move disabled flags
 *   - UPDATE_TEDDY_BEAR_PARTY + INIT_ALL_ITEM_TRANSFORMATIONS for PCs */
static void cc_1f_add_party_member(ScriptReader *r) {
    uint8_t char_arg = script_read_byte(r);
    uint16_t char_id = char_arg ? (uint16_t)char_arg
                                : (uint16_t)(get_argument_memory() & 0xFFFF);
    if (char_id != 0) {
        add_char_to_party(char_id);
    }
}


/* CC_1F_15: GENERATE_ACTIVE_SPRITE — 5 arg bytes (word, word, byte)
 *
 * Port of CC_1F_15 (asm/text/ccs/create_entity_sprite.asm).
 * Gathers 5 argument bytes: sprite_id (word), script_id (word), param (byte).
 * Two paths based on param:
 *   param == 0xFF: QUEUE_ENTITY_CREATION (deferred entity creation)
 *   param != 0xFF: CREATE_PREPARED_ENTITY_SPRITE + INIT_ENTITY_FADE_STATE
 *
 * CREATE_PREPARED_ENTITY_SPRITE reads ow.entity_prepared_x/y for position
 * and ow.entity_prepared_direction to set the entity's initial direction.
 * INIT_ENTITY_FADE_STATE is a no-op for param 0, 1, or 6. */
static void cc_1f_generate_active_sprite(ScriptReader *r) {
    uint16_t sprite_id = script_read_word(r);
    uint16_t script_id = script_read_word(r);
    uint8_t param = script_read_byte(r);

    if (param == 0xFF) {
        /* QUEUE_ENTITY_CREATION path (assembly @UNKNOWN2 → QUEUE_ENTITY_CREATION).
         * Defers entity creation to FLUSH_ENTITY_CREATION_QUEUE. */
        queue_entity_creation(sprite_id, script_id);
    } else {
        /* CREATE_PREPARED_ENTITY_SPRITE path (assembly @UNKNOWN3).
         * Port of CREATE_PREPARED_ENTITY_SPRITE (asm/overworld/create_prepared_entity_sprite.asm):
         * Reads ow.entity_prepared_x/y for position, creates entity with slot=-1 (auto),
         * then sets entity direction from ow.entity_prepared_direction. */
        int16_t result = create_entity(sprite_id, script_id, -1,
                      ow.entity_prepared_x, ow.entity_prepared_y);
        if (result >= 0) {
            entities.directions[result] = ow.entity_prepared_direction;
        }
        /* INIT_ENTITY_FADE_STATE(A=entity_slot, X=param).
         * No-op for param 0, 1, or 6 (assembly lines 20-26 early-exit checks). */
        if (result >= 0) {
            init_entity_fade_state((uint16_t)result, param);
        }
    }
}


/* CC_1F_16: CHANGE_TPT_ENTRY_DIRECTION — 3 arg bytes (word, byte)
 *
 * Port of CC_1F_16 (asm/text/ccs/set_tpt_direction.asm) →
 * SET_NPC_DIRECTION (C462FF).
 * Text encoding uses 1-indexed directions; assembly does DEX to convert.
 * Assembly: after setting direction, calls RENDER_ENTITY_SPRITE_ENTRY2 (4-dir render). */
static void cc_1f_change_tpt_direction(ScriptReader *r) {
    uint16_t tpt_id = script_read_word(r);
    uint8_t direction = script_read_byte(r);
    int16_t found = find_entity_by_npc_id(tpt_id);
    if (found >= 0 && direction > 0) {
        int16_t new_dir = (int16_t)(direction - 1);
        if (entities.directions[found] != new_dir) {
            entities.directions[found] = new_dir;
            render_entity_sprite(found);  /* SET_NPC_DIRECTION calls RENDER_ENTITY_SPRITE_ENTRY2 */
        }
    }
}


/* CC_1F_21: TELEPORT_TO — 1 arg byte
 *
 * Faithful port of TELEPORT (asm/overworld/teleport.asm).
 * Handles event flag clearing, screen transitions (or simple fades when
 * ow.disabled_transitions is set), map loading, party repositioning, entity
 * queue flushing, and music resolution. */
static void cc_1f_teleport_to(ScriptReader *r) {
    uint8_t dest_id = script_read_byte(r);
    const TeleportDestination *dest = get_teleport_dest(dest_id);
    if (!dest) {
        fprintf(stderr, "display_text: bad teleport dest %d\n", dest_id);
        return;
    }
    /* Save and override ow.overworld_status_suppression (teleport.asm lines 12-15) */
    uint8_t saved_suppression = ow.overworld_status_suppression;
    ow.overworld_status_suppression = 1;

    /* Clear event flags 1-10 (FLG_TEMP_0 through FLG_TEMP_9).
     * Assembly (teleport.asm lines 25-37): SET_EVENT_FLAG(id, 0) for id 1..10.
     * These are temporary synchronization flags used by entity scripts
     * (e.g., attract mode scripts 115-119 use them for scene handoff). */
    for (int i = 1; i <= 10; i++) {
        event_flag_clear((uint16_t)i);
    }

    /* PROCESS_DOOR_INTERACTIONS (teleport.asm line 38) */
    process_door_interactions();

    /* Screen transition out (teleport.asm lines 39-60).
     * If ow.disabled_transitions: non-blocking FADE_OUT(step=1, delay=1).
     *   Assembly just sets fade parameters and continues — no wait.
     * Otherwise: play transition sound + full screen transition. */
    uint16_t sfx = get_screen_transition_sound_effect(
        dest->screen_transition, 1);
    play_sfx(sfx);
    if (ow.disabled_transitions) {
        fade_out(1, 1);
        /* Assembly: FADE_OUT is non-blocking (just sets params, RTL).
         * Do NOT call wait_for_fade_complete — assembly doesn't. */
    } else {
        screen_transition(dest->screen_transition, 1);
    }

    /* Coordinates are in 8-pixel units (ASL ASL ASL in assembly) */
    uint16_t x_pixels = dest->x_coord * 8;
    uint16_t y_pixels = dest->y_coord * 8;
    uint8_t raw_direction = dest->direction;

    /* Direction: assembly does AND #$007F, DEC (teleport.asm lines 80-82). */
    uint16_t direction_param = (raw_direction & 0x7F) - 1;

    /* LOAD_MAP_AT_POSITION (teleport.asm line 85) */
    load_map_at_position(x_pixels, y_pixels);
    ow.player_has_moved_since_map_load = 0;  /* teleport.asm line 86 */

    /* SET_LEADER_POSITION_AND_LOAD_PARTY (teleport.asm line 90).
     * Assembly passes: A=y_pixels, X=x_pixels, Y=direction_param.
     * This handles sector attrs, character_mode, entity creation via
     * LOAD_PARTY_AT_MAP_POSITION, surface flags, position buffer init,
     * animation fingerprints, pajama flag, and refresh_party_entities. */
    set_leader_position_and_load_party(x_pixels, y_pixels, direction_param);

    /* If bit 7 of the direction byte is set, fill position buffer with a
     * spread-out trail behind the leader (teleport.asm lines 91-96). */
    if (raw_direction & 0x80) {
        fill_party_position_buffer(direction_param);
    }

    /* Resolve music for new location (teleport.asm lines 98-101).
     * RESOLVE_MAP_SECTOR_MUSIC + APPLY_NEXT_MAP_MUSIC. */
    resolve_map_sector_music(x_pixels, y_pixels);
    apply_next_map_music();

    /* POST_TELEPORT_CALLBACK (teleport.asm lines 102-119).
     * If a deferred callback was set (e.g. UNDRAW_FLYOVER_TEXT by flyover/
     * sanctuary scripts), call it and clear it. */
    if (ow.post_teleport_callback) {
        ow.post_teleport_callback();
        ow.post_teleport_callback = NULL;
    }

    /* FLUSH_ENTITY_CREATION_QUEUE (teleport.asm line 120).
     * Creates any entities that were deferred during map loading
     * (e.g., via QUEUE_ENTITY_CREATION). */
    flush_entity_creation_queue();

    /* Screen transition in (teleport.asm lines 121-142).
     * If ow.disabled_transitions: non-blocking FADE_IN(step=1, delay=1).
     *   Assembly just sets fade parameters and continues — no wait.
     * Otherwise: play transition sound + full screen transition. */
    sfx = get_screen_transition_sound_effect(
        dest->screen_transition, 0);
    play_sfx(sfx);
    if (ow.disabled_transitions) {
        fade_in(1, 1);
        /* Assembly: FADE_IN is non-blocking (just sets params, RTL).
         * Do NOT call wait_for_fade_complete — assembly doesn't. */
    } else {
        screen_transition(dest->screen_transition, 0);
    }

    /* Reset stairs direction (teleport.asm line 145) */
    ow.stairs_direction = (uint16_t)-1;

    /* SPAWN_BUZZ_BUZZ (teleport.asm line 146) */
    spawn_buzz_buzz();

    /* Restore ow.overworld_status_suppression (teleport.asm lines 147-148) */
    ow.overworld_status_suppression = saved_suppression;

}


/* CC_1F_E5: SET_PLAYER_LOCK — 1 arg byte
 * Port of CC_1F_E5 (asm/text/ccs/set_player_movement_lock.asm).
 * Calls DISABLE_CHARACTER_MOVEMENT with the arg byte as char_id.
 * 0xFF = all party + init entity. */
static void cc_1f_set_player_lock(ScriptReader *r) {
    uint8_t char_id = script_read_byte(r);
    disable_character_movement((uint16_t)char_id);
}


/* CC_1F_E8: RESTRICT_PLAYER_MOVEMENT — 1 arg byte
 * Port of CC_1F_E8 (asm/text/ccs/set_player_movement_lock_if_camera_refocused.asm).
 * Calls ENABLE_CHARACTER_MOVEMENT with the arg byte as char_id.
 * 0xFF = all party + init entity. */
static void cc_1f_restrict_player_movement(ScriptReader *r) {
    uint8_t char_id = script_read_byte(r);
    enable_character_movement((uint16_t)char_id);
}


/* CC_1F_EB: MAKE_INVISIBLE — 2 arg bytes (char_id, fade_param)
 *
 * Port of CC_1F_EB (asm/text/ccs/set_character_invisibility.asm) →
 * HIDE_ENTITY_SPRITES (asm/overworld/entity/hide_entity_sprites.asm).
 *
 * char_id: character ID to hide, or 0xFF to hide all party members.
 * fade_param: parameter for INIT_ENTITY_FADE_STATE (no-op for 0, 1, 6).
 *
 * Sets DRAW_DISABLED (bit 15) on spritemap_ptr_hi for the target entities,
 * which causes call_entity_draw() to skip drawing them. */
static void cc_1f_make_invisible(ScriptReader *r) {
    uint8_t char_id = script_read_byte(r);
    uint8_t fade_param = script_read_byte(r);

    /* Assembly: FIND_ENTITY_FOR_CHARACTER → INIT_ENTITY_FADE_STATE → HIDE_ENTITY_SPRITES */
    int16_t entity_slot = find_entity_for_character(char_id);
    if (entity_slot >= 0)
        init_entity_fade_state((uint16_t)entity_slot, (uint16_t)fade_param);

    /* HIDE_ENTITY_SPRITES (C463F4): clears intangibility, then sets
     * DRAW_DISABLED (bit 15) on the target entity/entities. */
    hide_entity_sprites(char_id == 0xFF ? 0xFF : (uint16_t)char_id);
}


/* ---- CC_1F_EC: MAKE_VISIBLE (port of asm/text/ccs/set_character_visibility.asm) ----
 *
 * char_id: character ID to show, or 0xFF to show all party members.
 * fade_param: parameter for INIT_ENTITY_FADE_STATE (no-op for 0, 1, 6).
 *
 * Clears DRAW_DISABLED (bit 15) on spritemap_ptr_hi for the target entities. */
static void cc_1f_make_visible(ScriptReader *r) {
    uint8_t char_id = script_read_byte(r);
    uint8_t fade_param = script_read_byte(r);

    /* Assembly: FIND_ENTITY_FOR_CHARACTER → INIT_ENTITY_FADE_STATE → SHOW_ENTITY_SPRITES */
    int16_t entity_slot = find_entity_for_character(char_id);
    if (entity_slot >= 0)
        init_entity_fade_state((uint16_t)entity_slot, (uint16_t)fade_param);

    /* SHOW_ENTITY_SPRITES (C4645A): clears DRAW_DISABLED. */
    show_entity_sprites((uint16_t)char_id);
}


/* CC_1F_EF: SET_CAMERA_FOCUS_BY_SPRITE_ID — 2 arg bytes (word)
 *
 * Port of CC_1F_EF (asm/text/ccs/unknown_1F_EF.asm) →
 * SET_CAMERA_FOCUS_BY_SPRITE_ID (C466A8).
 * Finds the entity with the given sprite_id and makes the camera follow it.
 * Also sets camera_mode = 2 to enable special camera mode (SYNC_CAMERA_TO_ENTITY)
 * in UPDATE_LEADER_MOVEMENT → UPDATE_SPECIAL_CAMERA_MODE. */
static void cc_1f_set_camera_focus(ScriptReader *r) {
    uint16_t sprite_id = script_read_word(r);
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    /* Assembly always writes to CAMERA_FOCUS_ENTITY, even -1 when not found */
    ow.camera_focus_entity = slot;
    /* Assembly (C466A8 line 8-9): LDA #2 / STA camera_mode.
     * This enables special camera mode 2 (SYNC_CAMERA_TO_ENTITY) in
     * UPDATE_SPECIAL_CAMERA_MODE, called from UPDATE_LEADER_MOVEMENT.
     * Mode 2 syncs game_state.leader_x/y_coord to the camera entity's
     * position each frame, which drives the position ert.buffer system and
     * makes party followers trail the camera entity. */
    game_state.camera_mode = 2;
}


/* CC_1F_F0: RIDE_BICYCLE — 0 arg bytes
 * Port of tree_1F.asm @UNKNOWN145 → GET_ON_BICYCLE (asm/overworld/get_on_bicycle.asm).
 * Mounts the bicycle for the party leader. */
static void cc_1f_ride_bicycle(ScriptReader *r) {
    (void)r;
    get_on_bicycle();
}


/* CC_1F_F1: SET_TPT_MOVEMENT_CODE — 4 arg bytes (word, word)
 *
 * Port of CC_1F_F1 (asm/text/ccs/set_tpt_entity_movement.asm) →
 * SET_TPT_ENTITY_SCRIPT.
 * Finds the entity by NPC ID and reassigns its movement script. */
static void cc_1f_set_tpt_movement(ScriptReader *r) {
    uint16_t tpt_id = script_read_word(r);
    uint16_t script_id = script_read_word(r);
    int16_t ent_offset = find_entity_by_npc_id(tpt_id);
    if (ent_offset >= 0) {
        reassign_entity_script(ent_offset, script_id);
    }
}


/* CC_1F_F2: SET_SPRITE_MOVEMENT_CODE — 4 arg bytes (word, word) */
static void cc_1f_set_sprite_movement(ScriptReader *r) {
    uint16_t sprite_id = script_read_word(r);
    uint16_t script_id = script_read_word(r);
    int16_t slot = find_entity_by_sprite_id(sprite_id);
    if (slot >= 0) {
        reassign_entity_script(slot, script_id);
    }
}


/* --- CC 0x1F tree dispatch ---
 * Argument byte counts from include/textmacros.asm EBTEXT_* macros.
 *
 * All CC 0x1F sub-handlers are implemented below. Categories:
 *   Audio (0x00-0x07), Party (0x11-0x14), Entity (0x15-0x1F),
 *   Teleport (0x20-0x21), Window/font (0x30-0x31), Event/input (0x41, 0x50-0x52),
 *   Map/movement (0x60-0x69), Combat (0x23), E-range (0xE1-0xF4). */
void cc_1f_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    /* --- Audio commands --- */
    case 0x00: {
        /* PLAY_MUSIC: 2 args (slot, track_id).
         * Port of CC_1F_00 (asm/text/ccs/play_music.asm) → SET_MAP_MUSIC.
         * Slot byte is unused by SET_MAP_MUSIC (it only calls CHANGE_MUSIC
         * with the track_id and stores it in map music globals).
         * If track_id==0, uses argument_memory as track number. */
        script_read_byte(r);  /* slot (unused) */
        uint8_t track = script_read_byte(r);
        uint16_t track_id = track ? (uint16_t)track : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (track_id != 0) {
            change_music((uint8_t)track_id);
        }
        break;
    }
    case 0x01: {
        /* STOP_MUSIC: 1 arg.
         * Port of CC_1F_01 (asm/text/ccs/stop_music.asm) → REDIRECT_STOP_MUSIC.
         * The arg byte goes through TXA but REDIRECT_STOP_MUSIC ignores it. */
        script_read_byte(r);
        stop_music();
        break;
    }
    case 0x02: {
        /* PLAY_SOUND: 1 arg (sfx_id).
         * Port of CC_1F_02: plays a sound effect via the SFX queue. */
        uint8_t sfx_id = script_read_byte(r);
        play_sfx(sfx_id);
        break;
    }
    case 0x03: {
        /* RESTORE_DEFAULT_MUSIC: 0 args.
         * Port of tree_1F.asm @RESTORE_DEFAULT_MUSIC → GET_MAP_MUSIC_AT_LEADER + SET_MAP_MUSIC.
         * Resolves the music track for the current map sector and plays it. */
        uint16_t track = get_map_music_at_leader();
        change_music(track);
        ml.current_map_music_track = track;
        ml.next_map_music_track = track;
        break;
    }
    case 0x04: {
        /* SET_TEXT_PRINTING_SOUND: 1 arg.
         * Port of CC_1F_04 (asm/text/ccs/toggle_text_printing_sound.asm) →
         * SET_TEXT_SOUND_MODE (asm/text/set_text_sound_mode.asm).
         * Sets the sound effect mode for text printing.
         * arg==0 → use argument_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t mode = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        dt.text_sound_mode = mode;
        break;
    }
    case 0x05:
        /* DISABLE_SECTOR_MUSIC_CHANGE: 0 args.
         * Port of CC_1F_05 (asm/text/ccs/tree_1F.asm) → SET_AUTO_SECTOR_MUSIC_CHANGES(0). */
        ow.enable_auto_sector_music_changes = 0;
        break;
    case 0x06:
        /* ENABLE_SECTOR_MUSIC_CHANGE: 0 args.
         * Port of CC_1F_06 (asm/text/ccs/tree_1F.asm) → SET_AUTO_SECTOR_MUSIC_CHANGES(1). */
        ow.enable_auto_sector_music_changes = 1;
        break;
    case 0x07: {
        /* APPLY_MUSIC_EFFECT: 1 arg.
         * Port of CC_1F_07 (asm/text/ccs/set_music_effect.asm).
         * Writes the effect value to APU port 1 with bit-flip protocol.
         * If effect==0, uses argument_memory. */
        uint8_t effect_arg = script_read_byte(r);
        uint16_t effect = effect_arg ? (uint16_t)effect_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        write_apu_port1((uint8_t)effect);
        break;
    }

    /* --- Party/character commands --- */
    case 0x11: cc_1f_add_party_member(r); break;          /* ADD_PARTY_MEMBER: 1 arg */
    case 0x12: {
        /* REMOVE_PARTY_MEMBER: 1 arg byte (char_id).
         * Port of CC_1F_12 (asm/text/ccs/party_member_remove.asm).
         * If arg==0, uses argument_memory. Calls REMOVE_CHAR_FROM_PARTY
         * (asm/misc/party_remove_char.asm) which removes from party_members[],
         * compacts, and calls INIT_ALL_ITEM_TRANSFORMATIONS. */
        uint8_t char_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (char_id != 0) {
            remove_char_from_party(char_id);
        }
        break;
    }
    case 0x13: {
        /* CHANGE_CHARACTER_DIRECTION: 2 args (char_id, direction).
         * Port of CC_1F_13 (asm/text/ccs/set_character_direction.asm) →
         * SET_CHARACTER_ENTITY_DIRECTION (C46363) → UPDATE_ENTITY_SPRITE.
         * Direction is 1-indexed in script; assembly does DEX to convert.
         * char_id==0 → working_memory, direction==0 → argument_memory.
         * Assembly calls UPDATE_ENTITY_SPRITE (8-dir path via RENDER_ENTITY_SPRITE_8DIR),
         * not RENDER_ENTITY_SPRITE (4-dir). Use update_entity_sprite() to match. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t dir_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t direction = dir_arg ? (uint16_t)dir_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (char_id != 0 && direction != 0) {
            int16_t ent = find_entity_for_character(char_id);
            if (ent >= 0) {
                int16_t new_dir = (int16_t)(direction - 1);
                if (entities.directions[ent] != new_dir) {
                    entities.directions[ent] = new_dir;
                    update_entity_sprite(ent);
                }
            }
        }
        break;
    }
    case 0x14: {
        /* CHANGE_PARTY_DIRECTION: 1 arg (direction).
         * Port of CC_1F_14 (asm/text/ccs/set_party_direction.asm) →
         * SET_ALL_PARTY_DIRECTIONS (C46397) → UPDATE_ENTITY_SPRITE.
         * Direction is 1-indexed; assembly does DEC to convert.
         * direction==0 → argument_memory.
         * Assembly calls UPDATE_ENTITY_SPRITE (8-dir path), not 4-dir render.
         * Use update_entity_sprite() to match. */
        uint8_t dir_arg = script_read_byte(r);
        uint16_t direction = dir_arg ? (uint16_t)dir_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (direction != 0) {
            int16_t new_dir = (int16_t)(direction - 1);
            for (int i = 0; i < (game_state.party_count & 0xFF); i++) {
                uint8_t party_member = game_state.party_order[i];
                if (party_member == 0 || party_member >= 16)
                    continue;  /* Assembly: CLC+SBC #16, BRANCHLTEQS → skip if >= 16 */
                uint16_t ent_slot =
                    read_u16_le(&game_state.party_entity_slots[i * 2]);
                if (entities.directions[ent_slot] != new_dir) {
                    entities.directions[ent_slot] = new_dir;
                    update_entity_sprite(ent_slot);
                }
            }
        }
        break;
    }

    /* --- Entity/sprite commands --- */
    case 0x15: cc_1f_generate_active_sprite(r); break;    /* GENERATE_ACTIVE_SPRITE: 5 args */
    case 0x16: cc_1f_change_tpt_direction(r); break;      /* CHANGE_TPT_ENTRY_DIRECTION: 3 args */
    case 0x17: {
        /* CREATE_ENTITY (NPC): 5 args (npc_id word, script_id word, fade_param byte).
         * Port of CC_1F_17 (asm/text/ccs/create_entity_tpt.asm).
         * Calls CREATE_PREPARED_ENTITY_NPC(A=npc_id, X=script_id) to create
         * an NPC entity at ow.entity_prepared_x/y with the TPT's sprite.
         * Then INIT_ENTITY_FADE_STATE(X=fade_param). */
        uint16_t npc_id = script_read_word(r);
        uint16_t script_id = script_read_word(r);
        uint8_t fade_param = script_read_byte(r);
        int16_t entity_slot = create_prepared_entity_npc(npc_id, script_id);
        /* Assembly: A from CREATE_PREPARED_ENTITY_NPC → INIT_ENTITY_FADE_STATE */
        if (entity_slot >= 0)
            init_entity_fade_state((uint16_t)entity_slot, (uint16_t)fade_param);
        break;
    }
    case 0x18: /* DUMMY_1F_18: 7 args (dummy handler, no action).
                 * ASM: LDA #$0006 → gathers 7 bytes (counter 0..6). */
        for (int i = 0; i < 7; i++) script_read_byte(r);
        break;
    case 0x19: /* DUMMY_1F_19: 7 args (dummy handler, no action).
                 * ASM: LDA #$0006 → gathers 7 bytes (counter 0..6). */
        for (int i = 0; i < 7; i++) script_read_byte(r);
        break;
    case 0x1A: {
        /* CREATE_FLOATING_SPRITE_AT_TPT_ENTITY: 3 args (npc_id word, table_index byte).
         * Port of CC_1F_1A (asm/text/ccs/create_floating_sprite_at_tpt_entity.asm).
         * Reads NPC ID (2 bytes) and table index (1 byte), finds entity by NPC ID,
         * spawns a floating sprite from the table at that entity. */
        uint16_t npc_id = script_read_word(r);
        uint8_t table_index = script_read_byte(r);
        spawn_floating_sprite_for_npc(npc_id, table_index);
        break;
    }
    case 0x1B: {
        /* DELETE_FLOATING_SPRITE_AT_TPT_ENTITY: 2 args (npc_id as word).
         * Port of CC_1F_1B (asm/text/ccs/delete_floating_sprite_at_tpt_entity.asm).
         * Finds entity by NPC ID, removes all floating sprites associated with it. */
        uint16_t npc_id = script_read_word(r);
        int16_t ent_offset = find_entity_by_npc_id(npc_id);
        remove_associated_entities(ent_offset);
        break;
    }
    case 0x1C: {
        /* CREATE_FLOATING_SPRITE_AT_CHARACTER: 2 args (char_id, table_index).
         * Port of CC_1F_1C (asm/text/ccs/create_floating_sprite_at_character.asm).
         * arg1: character ID (0 → use working_memory).
         * arg2: table index (0 → use argument_memory).
         * Finds entity for character, spawns floating sprite from table. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t idx_arg  = script_read_byte(r);
        uint8_t char_id = char_arg ? char_arg : (uint8_t)(get_working_memory() & 0xFF);
        uint16_t table_index = idx_arg ? (uint16_t)idx_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        spawn_floating_sprite_for_character(char_id, table_index);
        break;
    }
    case 0x1D: {
        /* DELETE_FLOATING_SPRITE_AT_CHARACTER: 1 arg (char_id).
         * Port of CC_1F_1D (asm/text/ccs/delete_floating_sprite_at_character.asm).
         * If arg==0, uses working_memory. Finds entity for character,
         * removes all floating sprites associated with it. */
        uint8_t char_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        int16_t slot = find_entity_for_character((uint8_t)char_id);
        remove_associated_entities(slot);
        break;
    }
    case 0x1E: {
        /* DELETE_TPT_INSTANCE: 3 args (npc_id_word, fade_param_byte).
         * Port of CC_1F_1E (asm/text/ccs/delete_entity_tpt.asm).
         * Finds entity by NPC ID, saves position, reassigns to deactivation script. */
        uint16_t npc_id = script_read_word(r);
        uint8_t fade_param = script_read_byte(r);
        deactivate_npc_entity(npc_id, (uint16_t)fade_param);
        break;
    }
    case 0x1F: {
        /* DELETE_GENERATED_SPRITE: 3 args (sprite_id_word, fade_param_byte).
         * Port of CC_1F_1F (asm/text/ccs/delete_entity_sprite.asm).
         * Finds entity by sprite ID, saves position, reassigns to deactivation script. */
        uint16_t sprite_id = script_read_word(r);
        uint8_t fade_param = script_read_byte(r);
        deactivate_sprite_entity(sprite_id, (uint16_t)fade_param);
        break;
    }
    case 0x20: {
        /* TRIGGER_PSI_TELEPORT: 2 args (destination, style).
         * Port of CC_1F_20 (asm/text/ccs/trigger_psi_teleport.asm).
         * Arg 1 = destination (0 → working_memory).
         * Arg 2 = style (0 → argument_memory).
         * Calls SET_TELEPORT_STATE(A=destination, @PARAM01=style). */
        uint8_t dest_arg = script_read_byte(r);
        uint8_t style_arg = script_read_byte(r);
        uint16_t dest = dest_arg ? (uint16_t)dest_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t style = style_arg ? (uint16_t)style_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        set_teleport_state((uint8_t)dest, (uint8_t)style);
        break;
    }
    case 0x21: cc_1f_teleport_to(r); break;               /* TELEPORT_TO: 1 arg */
    case 0x23: {
        /* TRIGGER_BATTLE: 2 args forming a 16-bit battle_group_id.
         * Port of CC_1F_23 (asm/text/ccs/trigger_battle.asm).
         * If battle_group_id == 0, reads from argument_memory instead.
         * Calls init_battle_scripted and stores result in working_memory. */
        uint16_t battle_group = script_read_word(r);
        if (battle_group == 0)
            battle_group = (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = init_battle_scripted(battle_group);
        set_working_memory((uint32_t)(int32_t)(int16_t)result);
        break;
    }

    /* --- Misc commands (0x40, 0x81, 0x90) --- */
    case 0x40: script_read_byte(r); script_read_byte(r); break; /* NOP_1F_40: 2 args (intentional no-op, asm/text/ccs/nop_1f_40.asm) */
    case 0x81: {
        /* TEST_CHARACTER_CAN_EQUIP_ITEM: 2 args (char_id, item_id).
         * Port of CC_1F_81 (asm/text/ccs/test_character_can_equip_item.asm).
         * Arg 1 = char_id (0 → working_memory), Arg 2 = item_id (0 → argument_memory).
         * Calls CHECK_ITEM_USABLE_BY to test if character can use the item.
         * Stores result in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = check_item_usable_by(char_id, item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x90: {
        /* OPEN_TELEPHONE_MENU: 0 args.
         * Port of tree_1F.asm @OPEN_TELEPHONE_MENU → OPEN_TELEPHONE_MENU (C19441).
         * Opens the phone directory menu and stores result in working_memory.
         * Returns selected contact index (1-based), or 0 if cancelled/empty. */
        uint16_t tel_result = open_telephone_menu();
        set_working_memory((uint32_t)tel_result);
        break;
    }

    /* --- Font commands --- */
    case 0x30:
    case 0x31: {
        /* USE_NORMAL_FONT / USE_MR_SATURN_FONT: 0 args each.
         * Port of CHANGE_CURRENT_WINDOW_FONT (asm/text/change_current_window_font.asm).
         * Assembly: if A == WINDOW::BATTLE_MENU_FULL (0x30) → font 0 (NORMAL),
         *           else → font 1 (MR_SATURN).
         * Stores to window_stats.font for the currently focused window. */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            w->font = (sub == 0x30) ? 0 : 1;
        }
        break;
    }

    /* --- Event/input commands --- */
    case 0x41: {
        /* TRIGGER_SPECIAL_EVENT: 1 arg byte (event_id).
         * Port of CC_1F_41 (asm/text/ccs/trigger_special_event.asm).
         * Calls DISPATCH_SPECIAL_EVENT with arg, stores signed result
         * in working_memory. Port of asm/text/dispatch_special_event.asm. */
        uint8_t event_id = script_read_byte(r);
        uint16_t result = dispatch_special_event(event_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x50:
        /* LOCK_INPUT: 0 args.
         * Port of CC_1F_50 (asm/text/lock_input.asm).
         * Sets TEXT_PROMPT_WAITING_FOR_INPUT = 1. */
        dt.text_prompt_waiting_for_input = 1;
        break;
    case 0x51:
        /* UNLOCK_INPUT: 0 args.
         * Port of CC_1F_51 (asm/text/unlock_input.asm).
         * Clears TEXT_PROMPT_WAITING_FOR_INPUT = 0. */
        dt.text_prompt_waiting_for_input = 0;
        break;
    case 0x52: {
        /* CREATE_NUMBER_SELECTOR: 1 arg (max_digits).
         * Port of CC_1F_52 (asm/text/ccs/create_number_selector.asm)
         * → NUM_SELECT_PROMPT (asm/text/num_select_prompt.asm).
         *
         * Displays an interactive multi-digit number input.
         * LEFT/RIGHT moves cursor between digit positions.
         * UP/DOWN increments/decrements the selected digit (wraps 0-9).
         * A/L confirms, B/SELECT cancels (returns -1 → working_memory=0).
         *
         * Rendering uses PRINT_CHAR_WITH_SOUND for each digit:
         *   tile 16 = highlighted prefix (marks selected digit position)
         *   tile 48 = normal prefix (unselected digits)
         *   The digit value (0-9) is added to the prefix tile. */
        uint8_t max_digits_arg = script_read_byte(r);
        uint16_t max_digits = max_digits_arg;

        /* NUM_SELECT_PROMPT: save cursor position, run input loop */
        WindowInfo *w = get_focus_window_info();
        if (!w || win.current_focus_window == WINDOW_ID_NONE) {
            set_working_memory(0);
            break;
        }

        uint16_t start_x = w->text_x;
        uint16_t start_y = w->text_y;
        int32_t value = 0;       /* current number (@LOCAL05) */
        uint16_t cursor_pos = 1; /* digit position from right, 1-based (@LOCAL04) */
        int32_t place_value = 1; /* multiplier for current digit (@LOCAL03) */

        for (;;) {
            /* Render the number at the saved cursor position */
            set_instant_printing();
            set_focus_text_cursor(start_x, start_y);

            /* Convert value to digits.
             * NUMBER_TO_TEXT_BUFFER: assembly converts a 32-bit value to
             * a 7-digit decimal string in a ert.buffer. We do it inline. */
            char digits[8];
            {
                int32_t tmp = value;
                for (int d = 6; d >= 0; d--) {
                    digits[d] = (char)(tmp % 10);
                    tmp /= 10;
                }
            }
            digits[7] = '\0';

            /* Print digits right-to-left from max_digits position.
             * Leading digits: printed as prefix tile (16=selected, 48=normal).
             * All digits: prefix + digit value → PRINT_CHAR_WITH_SOUND. */
            int num_significant = 0;
            {
                int32_t tmp = value;
                while (tmp > 0) { num_significant++; tmp /= 10; }
                if (num_significant == 0) num_significant = 1;
            }
            int start_digit = 7 - max_digits;

            for (int i = start_digit; i < 7; i++) {
                uint16_t digit_pos = (uint16_t)(7 - i);  /* position from right, 1-based */
                uint16_t prefix = (digit_pos == cursor_pos) ? 16 : 48;
                uint16_t tile = prefix + (uint16_t)digits[i];
                print_char_with_sound(tile);
            }

            clear_instant_printing();
            window_tick();

            /* Input loop: wait for button press */
            for (;;) {
                update_hppp_meter_and_render();

                if (core.pad1_pressed & PAD_LEFT) {
                    /* Move cursor left (higher digit) */
                    if (cursor_pos < max_digits) {
                        play_sfx(2);  /* SFX::CURSOR2 */
                        cursor_pos++;
                        place_value *= 10;
                    }
                    break;
                }
                if (core.pad1_pressed & PAD_RIGHT) {
                    /* Move cursor right (lower digit) */
                    if (cursor_pos > 1) {
                        play_sfx(2);  /* SFX::CURSOR2 */
                        cursor_pos--;
                        place_value /= 10;
                    }
                    break;
                }
                if (core.pad1_autorepeat & PAD_UP) {
                    /* Increment current digit (wraps 9→0) */
                    play_sfx(3);  /* SFX::CURSOR3 */
                    int32_t current_digit = (value / place_value) % 10;
                    if (current_digit == 9) {
                        value -= place_value * 9;
                    } else {
                        value += place_value;
                    }
                    break;
                }
                if (core.pad1_autorepeat & PAD_DOWN) {
                    /* Decrement current digit (wraps 0→9) */
                    play_sfx(3);  /* SFX::CURSOR3 */
                    int32_t current_digit = (value / place_value) % 10;
                    if (current_digit == 0) {
                        value += place_value * 9;
                    } else {
                        value -= place_value;
                    }
                    break;
                }
                if (core.pad1_pressed & PAD_CONFIRM) {
                    /* Confirm selection */
                    play_sfx(1);  /* SFX::CURSOR1 */
                    goto num_select_done;
                }
                if (core.pad1_pressed & PAD_CANCEL) {
                    /* Cancel: return -1 → working_memory=0, argument_memory=0 */
                    play_sfx(2);  /* SFX::CURSOR2 */
                    value = -1;
                    goto num_select_done;
                }
            }
        }
    num_select_done:
        if (value == -1) {
            /* Cancelled: set both working_memory and argument_memory to 0 */
            set_working_memory(0);
            set_argument_memory(0);
        } else {
            set_working_memory((uint32_t)value);
        }
        break;
    }

    /* --- Map/movement commands --- */
    case 0x60: {
        /* TEXT_SPEED_DELAY: 0 args.
         * Port of CC_1F_60 (asm/text/ccs/unknown_1F_60.asm) → TEXT_SPEED_DELAY
         * (asm/text/text_speed_delay.asm).
         * Applies per-character typing delay based on dt.text_speed_based_wait.
         * Loops calling render_frame_tick() for the delay count frames.
         * Button press (B/SELECT/A/L) cancels the remaining delay.
         * dt.text_speed_based_wait is set during file select from game_state.text_speed. */
        if (!dt.text_prompt_waiting_for_input) {
            uint16_t frames = dt.text_speed_based_wait;
            for (uint16_t i = 0; i < frames; i++) {
                update_hppp_meter_and_render();
                if (platform_input_quit_requested()) break;
                if (core.pad1_pressed & PAD_TEXT_ADVANCE) break;
            }
        }
        break;
    }
    case 0x61:
        /* WAIT_FOR_ACTIONSCRIPT: 0 args.
         * Port of WAIT_FOR_ACTIONSCRIPT (asm/text/wait_for_actionscript.asm).
         * Assembly sequence:
         *   1. STZ ACTIONSCRIPT_STATE
         *   2. JSR CLEAR_INSTANT_PRINTING
         *   3. JSL WINDOW_TICK            (initial: renders windows + frame)
         *   4. loop: JSL RENDER_FRAME_TICK (subsequent: frame only)
         *      until ACTIONSCRIPT_STATE != 0
         *   5. STZ ACTIONSCRIPT_STATE
         * The initial WINDOW_TICK is critical: it renders any open windows
         * to win.bg2_buffer so text is visible while waiting for entity scripts. */
        ert.actionscript_state = 0;
        clear_instant_printing();
        window_tick();
        while (ert.actionscript_state == 0) {
            render_frame_tick();
            if (platform_input_quit_requested()) break;
        }
        ert.actionscript_state = 0;
        break;
    case 0x62: {
        /* ENABLE_BLINKING_TRIANGLE: 1 arg byte.
         * Port of CC_1F_62 (asm/text/ccs/enable_blinking_triangle.asm) →
         * ENABLE_BLINKING_TRIANGLE (asm/text/enable_blinking_triangle.asm).
         * Simply stores the arg to BLINKING_TRIANGLE_FLAG. */
        uint8_t flag = script_read_byte(r);
        dt.blinking_triangle_flag = flag;
        break;
    }
    case 0x63: {
        /* SCREEN_RELOAD_PTR: 4 args — pointer to screen reload data.
         * Port of CC_1F_63 (asm/text/ccs/screen_reload_pointer.asm).
         * Disables all party+init entity movement, then queues a type-10
         * interaction (screen reload) with the given data pointer. */
        uint32_t ptr = script_read_dword(r);
        disable_character_movement(0xFF);  /* assembly: LDA #$00FF */
        queue_interaction(10, ptr);
        break;
    }
    case 0x64: {
        /* SAVE_PARTY_NPC_STATE: 0 args.
         * Port of SAVE_PARTY_NPC_STATE (asm/battle/save_party_npc_state.asm).
         * Saves NPC party member IDs and HP to backup fields,
         * removes both NPCs from the party, and backs up wallet to wallet_backup.
         * Then clears money_carried to 0. */
        game_state.party_npc_1_id_copy = game_state.party_npc_1;
        game_state.party_npc_1_hp_copy = game_state.party_npc_1_hp;
        game_state.party_npc_2_id_copy = game_state.party_npc_2;
        game_state.party_npc_2_hp_copy = game_state.party_npc_2_hp;
        /* Remove NPC 2 first (assembly order), then NPC 1 */
        remove_char_from_party((uint16_t)game_state.party_npc_2);
        remove_char_from_party((uint16_t)game_state.party_npc_1);
        game_state.wallet_backup = game_state.money_carried;
        game_state.money_carried = 0;
        break;
    }
    case 0x65: {
        /* RESTORE_PARTY_NPC_STATE: 0 args.
         * Port of RESTORE_PARTY_NPC_STATE (asm/battle/restore_party_npc_state.asm).
         * Removes current NPCs, restores saved NPC IDs and HP,
         * adds them back to party, and restores wallet from backup. */
        /* Remove current NPCs first (assembly lines 10-17) */
        remove_char_from_party((uint16_t)game_state.party_npc_1);
        remove_char_from_party((uint16_t)game_state.party_npc_2);
        /* Restore NPC 1 if saved (assembly lines 18-28).
         * NPC2 is nested inside NPC1 check — if npc_1_copy == 0, both skip. */
        if (game_state.party_npc_1_id_copy != 0) {
            game_state.party_npc_1 = game_state.party_npc_1_id_copy;
            add_char_to_party((uint16_t)game_state.party_npc_1);
            game_state.party_npc_1_hp = game_state.party_npc_1_hp_copy;
            /* Restore NPC 2 if saved (assembly lines 29-39) */
            if (game_state.party_npc_2_id_copy != 0) {
                game_state.party_npc_2 = game_state.party_npc_2_id_copy;
                add_char_to_party((uint16_t)game_state.party_npc_2);
                game_state.party_npc_2_hp = game_state.party_npc_2_hp_copy;
            }
        }
        /* Restore wallet (assembly lines 41-42) */
        game_state.money_carried = game_state.wallet_backup;
        break;
    }
    case 0x66: {
        /* ACTIVATE_HOTSPOT: 6 args (slot, hotspot_id, pointer[4]).
         * Port of CC_1F_66 (asm/text/ccs/activate_hotspot.asm) →
         * ACTIVATE_HOTSPOT (asm/overworld/activate_hotspot.asm).
         * Sets up an active hotspot boundary for player position tracking. */
        uint8_t slot_arg = script_read_byte(r);
        uint8_t id_arg = script_read_byte(r);
        uint32_t pointer = script_read_dword(r);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg
                                 : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t hotspot_id = id_arg ? (uint16_t)id_arg
                                     : (uint16_t)(get_working_memory() & 0xFFFF);
        if (slot >= 1 && slot <= NUM_ACTIVE_HOTSPOTS) {
            activate_hotspot(slot, hotspot_id, pointer);
        }
        break;
    }
    case 0x67: {
        /* DEACTIVATE_HOTSPOT: 1 arg (hotspot_id, 1-indexed).
         * Port of CC_1F_67 (asm/text/ccs/deactivate_hotspot.asm) → DISABLE_HOTSPOT.
         * arg==0 → use argument_memory. Clears ow.active_hotspots[id-1].mode
         * and game_state.active_hotspot_modes[id-1]. */
        uint8_t arg = script_read_byte(r);
        uint16_t id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (id >= 1 && id <= NUM_ACTIVE_HOTSPOTS) {
            ow.active_hotspots[id - 1].mode = 0;
            game_state.active_hotspot_modes[id - 1] = 0;
        }
        break;
    }
    case 0x68:
        /* STORE_COORDINATES_TO_MEMORY: 0 args.
         * Port of CC_1F_68 (asm/text/ccs/tree_1F.asm line 285-288).
         * Saves leader position to exit_mouse coords for later teleport. */
        game_state.exit_mouse_x_coord = game_state.leader_x_coord;
        game_state.exit_mouse_y_coord = game_state.leader_y_coord;
        break;
    case 0x69: {
        /* TELEPORT_TO_STORED_COORDINATES: 0 args.
         * Port of tree_1F.asm @TELEPORT_TO_STORED_COORDS (lines 290-326).
         * Used by the Exit Mouse (Escape Rope) item:
         * 1. Clear event flags 1-9 (SET_EVENT_FLAG with X=0)
         * 2. Fade out (step=1, delay=1)
         * 3. Play SFX::EQUIPPED_ITEM (115) sound
         * 4. Load map at stored exit_mouse_x/y coordinates
         * 5. SET_LEADER_POSITION_AND_LOAD_PARTY(x, y, DOWN)
         * 6. STAIRS_DIRECTION = -1
         * 7. Fade in (step=1, delay=1) */
        /* Assembly (tree_1F.asm:290-303): BLTEQ clears flags 1-10 inclusive */
        for (int flag = 1; flag <= 10; flag++) {
            event_flag_clear(flag);
        }
        fade_out(1, 1);
        play_sfx(115);  /* SFX::EQUIPPED_ITEM = 115 */
        uint16_t x = game_state.exit_mouse_x_coord;
        uint16_t y = game_state.exit_mouse_y_coord;
        load_map_at_position(x, y);
        ow.player_has_moved_since_map_load = 0;  /* Assembly (tree_1F.asm:315-316) */
        set_leader_position_and_load_party(x, y, 4);  /* direction = DOWN */
        fade_in(1, 1);
        ow.stairs_direction = (uint16_t)-1;  /* Assembly: set AFTER fade_in (tree_1F.asm:324-325) */
        break;
    }

    /* --- Misc commands --- */
    case 0x71: {
        /* LEARN_SPECIAL_PSI: 2 args (unused, psi_type).
         * Port of CC_1F_71 (asm/text/ccs/learn_special_psi.asm).
         * Arg-gathering handler: first byte is unused, second byte is the PSI type.
         * Sets the corresponding bit in game_state.party_psi:
         *   1=TELEPORT_ALPHA(0x01), 2=STARSTORM_ALPHA(0x02),
         *   3=STARSTORM_OMEGA(0x04), 4=TELEPORT_BETA(0x08). */
        script_read_byte(r); /* first arg (unused by LEARN_SPECIAL_PSI) */
        uint8_t psi_type = script_read_byte(r);
        learn_special_psi(psi_type);
        break;
    }
    case 0x83: {
        /* EQUIP_ITEM_TO_CHARACTER: 2 data bytes.
         * Port of CC_1F_83 (asm/text/ccs/equip_character_from_inventory.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_slot (0 → argument_memory).
         * Reads item at slot, determines equipment type, equips it.
         * Stores old equipment slot in argument_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = equip_item(char_id, slot);
        set_argument_memory((uint32_t)result);
        break;
    }
    case 0xA0:
        /* SET_INTERACTING_EVENT_FLAG_ON: 0 args.
         * Port of tree_1F.asm @UNKNOWN122.
         * SET_CURRENT_INTERACTING_EVENT_FLAG(1) — sets the event flag
         * identified by dt.current_interacting_event_flag to 1.
         * Assembly also calls SET_NPC_DIRECTION_FROM_EVENT_FLAG(INTERACTING_NPC_ENTITY). */
        if (dt.current_interacting_event_flag != 0) {
            event_flag_set(dt.current_interacting_event_flag);
        }
        if (ow.interacting_npc_entity != 0xFFFF) {
            int16_t npc_off = (int16_t)ow.interacting_npc_entity;
            uint16_t npc_id = entities.npc_ids[npc_off];
            uint16_t ef_id = get_npc_config_event_flag(npc_id);
            entities.directions[npc_off] = event_flag_get(ef_id) ? 0 : 4;
            render_entity_sprite(npc_off);
        }
        break;
    case 0xA1:
        /* SET_INTERACTING_EVENT_FLAG_OFF: 0 args.
         * Port of tree_1F.asm @UNKNOWN123.
         * SET_CURRENT_INTERACTING_EVENT_FLAG(0) — clears the event flag
         * identified by dt.current_interacting_event_flag.
         * Assembly also calls SET_NPC_DIRECTION_FROM_EVENT_FLAG(INTERACTING_NPC_ENTITY). */
        if (dt.current_interacting_event_flag != 0) {
            event_flag_clear(dt.current_interacting_event_flag);
        }
        if (ow.interacting_npc_entity != 0xFFFF) {
            int16_t npc_off = (int16_t)ow.interacting_npc_entity;
            uint16_t npc_id = entities.npc_ids[npc_off];
            uint16_t ef_id = get_npc_config_event_flag(npc_id);
            entities.directions[npc_off] = event_flag_get(ef_id) ? 0 : 4;
            render_entity_sprite(npc_off);
        }
        break;
    case 0xA2:
        /* GET_INTERACTING_EVENT_FLAG: 0 args.
         * Port of tree_1F.asm @UNKNOWN124.
         * GET_CURRENT_INTERACTING_EVENT_FLAG → sign-extend → set_working_memory.
         * Returns the value of the event flag identified by
         * dt.current_interacting_event_flag. */
        {
            int16_t val = dt.current_interacting_event_flag ?
                (int16_t)event_flag_get(dt.current_interacting_event_flag) : 0;
            set_working_memory((uint32_t)(int32_t)val);
        }
        break;
    case 0xB0:
        /* SAVE_GAME: 0 args.
         * Port of tree_1F.asm @SAVE_GAME → SAVE_CURRENT_GAME
         * (asm/misc/save_game.asm). Saves to the currently active slot. */
        save_game(current_save_slot - 1);
        break;

    /* --- JUMP_MULTI2: variable length (1 byte count + N * 4 bytes) --- */
    case 0xC0: {
        /* Port of CC_1F_C0 (asm/text/ccs/jump_multi2.asm).
         * Like JUMP_MULTI (CC_09) but performs a gosub: calls DISPLAY_TEXT
         * recursively with the selected target, then returns to continue
         * execution past the jump table.
         * working_memory is a 1-based selector into the address table.
         * If 0 (NULL) or out of range, skips all entries. */
        uint8_t count = script_read_byte(r);
        uint32_t wm = get_working_memory();
        uint32_t target = 0;
        /* Read all entries, remembering the one we want */
        for (int i = 0; i < count; i++) {
            uint32_t addr = script_read_dword(r);
            if ((uint16_t)wm == (uint16_t)(i + 1)) {
                target = addr;
            }
        }
        /* If a valid selection was made, call display_text recursively */
        if (target != 0 && wm != 0 && (uint16_t)wm <= count) {
            display_text_from_addr(target);
        }
        break;
    }

    /* --- D-range commands --- */
    case 0xD0: {
        /* TRY_FIX_ITEM: 1 arg (fix_probability, 0 → argument_memory).
         * Port of CC_1F_D0 (asm/text/ccs/try_fixing_items.asm).
         * Calls TRY_FIX_BROKEN_ITEM to attempt fixing a broken item in Jeff's
         * inventory. On success:
         *   working_memory = fixed item ID (via GET_ITEM_EP)
         *   argument_memory = original broken item ID
         * On failure: both set to 0. */
        uint8_t arg = script_read_byte(r);
        uint16_t probability = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t broken_item = try_fix_broken_item(probability);
        if (broken_item != 0) {
            uint16_t fixed_item = get_item_ep(broken_item);
            set_working_memory((uint32_t)fixed_item);
        } else {
            set_working_memory(0);
        }
        set_argument_memory((uint32_t)broken_item);
        break;
    }
    case 0xD1:
        /* GET_DISTANCE_TO_MAGIC_TRUFFLE: 0 args.
         * Port of GET_DISTANCE_TO_MAGIC_TRUFFLE (asm/overworld/get_distance_to_magic_truffle.asm).
         * Finds the Magic Truffle entity, computes Manhattan distance to leader.
         * Returns: 0=not found, 1=out of range, 10=far (>=16px),
         * or direction+2 (2-9) if close.
         * Stores result in working_memory. */
        {
            #define MAGIC_TRUFFLE_SPRITE_ID 376
            int16_t slot = find_entity_by_sprite_id(MAGIC_TRUFFLE_SPRITE_ID);
            if (slot < 0) {
                set_working_memory(0);
                break;
            }
            int16_t truffle_x = entities.abs_x[slot];
            int16_t truffle_y = entities.abs_y[slot];
            int16_t leader_x = (int16_t)game_state.leader_x_coord;
            int16_t leader_y = (int16_t)game_state.leader_y_coord;
            /* Check if within ±64 pixels in both axes */
            if ((uint16_t)truffle_x < (uint16_t)(leader_x - 64) ||
                (uint16_t)truffle_x > (uint16_t)(leader_x + 64) ||
                (uint16_t)truffle_y < (uint16_t)(leader_y - 64) ||
                (uint16_t)truffle_y > (uint16_t)(leader_y + 64)) {
                set_working_memory(1);
                break;
            }
            /* Manhattan distance */
            int16_t dy = truffle_y - leader_y;
            int16_t dx = truffle_x - leader_x;
            int16_t abs_dy = (dy < 0) ? -dy : dy;
            int16_t abs_dx = (dx < 0) ? -dx : dx;
            if (abs_dy + abs_dx >= 16) {
                set_working_memory(10);
                break;
            }
            /* Close enough — compute direction from leader to truffle */
            int16_t dir = calculate_direction_8(leader_x, leader_y,
                                                 truffle_x, truffle_y);
            set_working_memory((uint32_t)(dir + 2));
        }
        break;
    case 0xD2: {
        /* TRIGGER_PHOTOGRAPHER_EVENT: 1 arg (photo_id).
         * Port of CC_1F_D2 (asm/text/ccs/trigger_photographer_event.asm).
         * If arg == 0, uses argument_memory. Calls ENCOUNTER_TRAVELLING_PHOTOGRAPHER. */
        uint8_t arg = script_read_byte(r);
        uint16_t photo_id = arg;
        if (photo_id == 0) photo_id = (uint16_t)get_argument_memory();
        encounter_travelling_photographer(photo_id);
        break;
    }
    case 0xD3: {
        /* TRIGGER_TIMED_EVENT: 1 arg (delivery_id).
         * Port of CC_1F_D3 (asm/text/ccs/trigger_timed_event.asm).
         * Passes the arg directly to GET_DELIVERY_SPRITE_AND_PLACEHOLDER
         * which creates a delivery entity with a placeholder sprite. */
        uint8_t delivery_id = script_read_byte(r);
        get_delivery_sprite_and_placeholder(delivery_id);
        break;
    }

    /* --- E-range commands --- */
    case 0xE1: {
        /* CHANGE_MAP_PALETTE: 3 args (tileset_combo, palette_index, fade_frames).
         * Port of CC_1F_E1 (asm/text/ccs/set_map_palette.asm) → LOAD_MAP_PALETTE.
         * Loads the map palette for a given tileset combo and palette index.
         * fade_frames == 0: immediate palette swap.
         * fade_frames > 0: animated fade over N frames. */
        uint8_t tileset_combo = script_read_byte(r);
        uint8_t palette_index = script_read_byte(r);
        uint8_t fade_frames = script_read_byte(r);
        load_map_palette(tileset_combo, palette_index, fade_frames);
        break;
    }
    case 0xE4: {
        /* CHANGE_GENERATED_SPRITE_DIRECTION: 3 args (sprite_id_word, direction_byte).
         * Port of CC_1F_E4 (asm/text/ccs/set_entity_direction_sprite.asm).
         * Finds entity by sprite ID and sets its direction.
         * Direction is 1-indexed in script; assembly does DEX to convert.
         * sprite_id==0 → working_memory, direction==0 → argument_memory. */
        uint16_t sprite_id_arg = script_read_word(r);
        uint8_t direction_arg = script_read_byte(r);
        uint16_t sprite_id = sprite_id_arg ? sprite_id_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t direction = direction_arg ? (uint16_t)direction_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (sprite_id != 0 && direction != 0) {
            set_entity_direction_by_sprite_id(sprite_id, (int16_t)(direction - 1));
        }
        break;
    }
    case 0xE5: cc_1f_set_player_lock(r); break;           /* SET_PLAYER_LOCK: 1 arg */
    case 0xE6: {
        /* DISABLE_NPC_MOVEMENT: 2 args (NPC ID word).
         * Port of CC_1F_E6 (asm/text/ccs/set_tpt_entity_delay.asm). */
        uint16_t npc_id = script_read_word(r);
        disable_npc_movement(npc_id);
        break;
    }
    case 0xE7: {
        /* DISABLE_SPRITE_MOVEMENT: 2 args (sprite ID word).
         * Port of CC_1F_E7 (asm/text/ccs/unknown_1F_E7.asm). */
        uint16_t sprite_id = script_read_word(r);
        disable_sprite_movement(sprite_id);
        break;
    }
    case 0xE8: cc_1f_restrict_player_movement(r); break;  /* RESTRICT_PLAYER_MOVEMENT: 1 arg */
    case 0xE9: {
        /* ENABLE_NPC_MOVEMENT: 2 args (NPC ID word).
         * Port of CC_1F_E9 (asm/text/ccs/unknown_1F_E9.asm). */
        uint16_t npc_id = script_read_word(r);
        enable_npc_movement(npc_id);
        break;
    }
    case 0xEA: {
        /* ENABLE_SPRITE_MOVEMENT: 2 args (sprite ID word).
         * Port of CC_1F_EA (asm/text/ccs/unknown_1F_EA.asm). */
        uint16_t sprite_id = script_read_word(r);
        enable_sprite_movement(sprite_id);
        break;
    }
    case 0xEB: cc_1f_make_invisible(r); break;            /* MAKE_INVISIBLE: 2 args */
    case 0xEC: cc_1f_make_visible(r); break;               /* MAKE_VISIBLE: 2 args */
    case 0xED:
        /* RESTORE_MOVEMENT: 0 args.
         * Port of CC_1F_ED (asm/text/ccs/tree_1F.asm) → RESET_CAMERA_MODE (C466B8).
         * Clears leader_moved and camera_mode to restore normal camera behavior. */
        game_state.leader_moved = 0;
        game_state.camera_mode = 0;
        break;
    case 0xEE: {
        /* SET_CAMERA_FOCUS_BY_NPC_ID: 2 args (NPC ID word).
         * Port of CC_1F_EE (asm/text/ccs/teleport_party_to_tpt_entity.asm) →
         * SET_CAMERA_FOCUS_BY_NPC_ID (C46698).
         * Finds entity by NPC ID, sets ow.camera_focus_entity and camera_mode=2. */
        uint16_t npc_id = script_read_word(r);
        int16_t slot = find_entity_by_npc_id(npc_id);
        ow.camera_focus_entity = slot;
        game_state.camera_mode = 2;
        break;
    }
    case 0xEF: cc_1f_set_camera_focus(r); break;           /* SET_CAMERA_FOCUS_BY_SPRITE_ID: 2 args */
    case 0xF0: cc_1f_ride_bicycle(r); break;              /* RIDE_BICYCLE: 0 args */
    case 0xF1: cc_1f_set_tpt_movement(r); break;          /* SET_TPT_MOVEMENT_CODE: 4 args */
    case 0xF2: cc_1f_set_sprite_movement(r); break;       /* SET_SPRITE_MOVEMENT_CODE: 4 args */
    case 0xF3: {
        /* CREATE_FLOATING_SPRITE_AT_SPRITE_ENTITY: 3 args (sprite_id word, table_index byte).
         * Port of CC_1F_F3 (asm/text/ccs/create_floating_sprite_at_sprite_entity.asm).
         * Reads sprite ID (2 bytes) and table index (1 byte), finds entity by sprite ID,
         * spawns a floating sprite from the table at that entity. */
        uint16_t f3_sprite_id = script_read_word(r);
        uint8_t f3_table_index = script_read_byte(r);
        spawn_floating_sprite_for_sprite(f3_sprite_id, f3_table_index);
        break;
    }
    case 0xF4: {
        /* DELETE_FLOATING_SPRITE_AT_SPRITE_ENTITY: 2 args (sprite_id as word).
         * Port of CC_1F_F4 (asm/text/ccs/delete_floating_sprite_at_sprite_entity.asm).
         * Finds entity by sprite ID, removes all floating sprites associated with it. */
        uint16_t sprite_id = script_read_word(r);
        int16_t slot = find_entity_by_sprite_id(sprite_id);
        remove_associated_entities(slot);
        break;
    }

    default:
        /* Sub-codes 0xE2, 0xE3, 0xED are unused gaps in the assembly dispatch
         * (tree_1F.asm).  They fall through to JMP @RETURN_NULL — no args consumed,
         * no side-effects.  Any other unhandled code is a real bug. */
        if (sub != 0xE2 && sub != 0xE3 && sub != 0xED) {
            FATAL("display_text: unknown CC 1F %02X\n", sub);
        }
        break;
    }
}


/* --- CC 0x18 tree: window management ---
 * Byte counts from include/textmacros.asm EBTEXT_* macros.
 *
 * CC 0x18 tree: window management commands.
 * Port from asm/text/ccs/cc_18_*.asm files. */
void cc_18_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x00:
        /* CLOSE_WINDOW: 0 args. Port of tree_18.asm @UNKNOWN0 → CLOSE_FOCUS_WINDOW. */
        close_focus_window();
        break;
    case 0x01: {
        /* OPEN_WINDOW: 1 arg (window ID).
         * Port of CC_18_01 (asm/text/ccs/open_window.asm) → CREATE_WINDOW. */
        uint8_t window_id = script_read_byte(r);
        create_window(window_id);
        break;
    }
    case 0x02:
        /* SAVE_WINDOW_TEXT_ATTRIBUTES: 0 args.
         * Port of tree_18.asm @SAVE_TEXT_ATTRIBUTES →
         * SAVE_WINDOW_TEXT_ATTRIBUTES (asm/battle/save_window_text_attributes.asm).
         * Assembly (tree_18.asm lines 43-51): saves attrs then sets
         * display_text_state::unknown4 = 1 so END_BLOCK restores them.
         * Saves focus window ID, text position, padding, attributes, font. */
        save_window_text_attributes();
        dt.g_cc18_attrs_saved = 1;  /* display_text_state::unknown4 = 1 */
        break;
    case 0x03: {
        /* SWITCH_TO_WINDOW: 1 arg (window ID).
         * Port of CC_18_03 (asm/text/ccs/switch_to_window.asm) → SET_WINDOW_FOCUS. */
        uint8_t window_id = script_read_byte(r);
        win.current_focus_window = window_id;
        break;
    }
    case 0x04:
        /* CLOSE_ALL_WINDOWS: 0 args.
         * Port of tree_18.asm @CLOSE_ALL.
         * Assembly: JSR CLOSE_ALL_WINDOWS → JSR HIDE_HPPP_WINDOWS → JSL WINDOW_TICK. */
        close_all_windows();
        hide_hppp_windows();
        window_tick();
        break;
    case 0x05: {
        /* FORCE_TEXT_ALIGNMENT: 2 args (x, y).
         * Port of CC_18_05 (asm/text/ccs/force_text_alignment.asm).
         * Sets text cursor position within the focus window.
         * If dt.force_left_text_alignment is set, args are pixel positions;
         * otherwise they are tile positions. */
        uint8_t x = script_read_byte(r);
        uint8_t y = script_read_byte(r);
        if (dt.force_left_text_alignment) {
            set_text_pixel_position(y, x);
        } else {
            set_focus_text_cursor(x, y);
        }
        break;
    }
    case 0x06: {
        /* CLEAR_WINDOW: 0 args.
         * Port of tree_18.asm @UNKNOWN6 → clears focus window text content.
         * Resets text cursor and clears content_tilemap. */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            w->text_x = 0;
            w->text_y = 0;
            w->cursor_pixel_x = 0;
            /* Free tiles and clear per-window content tilemap */
            uint16_t cw = w->width - 2;
            uint16_t itr = w->height - 2;
            uint16_t total = cw * itr;
            if (total > w->content_tilemap_size) total = w->content_tilemap_size;
            for (uint16_t i = 0; i < total; i++) {
                free_tile_safe(w->content_tilemap[i]);
                w->content_tilemap[i] = 0;
            }
            vwf_init();
        }
        break;
    }
    case 0x07: {
        /* CHECK_FOR_INEQUALITY: 5 args (4-byte value + 1-byte source selector).
         * Port of CC_18_07 (asm/text/ccs/test_equality.asm).
         * Assembles 4 argument bytes into a 32-bit comparison value.
         * The 5th byte selects the source register:
         *   0 = working_memory, 1 = argument_memory, 2+ = secondary_memory.
         * Compares source against the assembled value:
         *   0 = source < value, 1 = source == value, 2 = source > value.
         * Stores result in working_memory. */
        uint32_t cmp_value = script_read_dword(r);
        uint8_t selector = script_read_byte(r);
        uint32_t source;
        if (selector == 0) {
            source = get_working_memory();
        } else if (selector == 1) {
            source = get_argument_memory();
        } else {
            source = (uint32_t)get_secondary_memory();
        }
        uint32_t result;
        if (source < cmp_value) {
            result = 0;
        } else if (source == cmp_value) {
            result = 1;
        } else {
            result = 2;
        }
        set_working_memory(result);
        break;
    }
    case 0x08: {
        /* SELECTION_MENU_NO_CANCEL (despite confusing label in tree_18.asm): 4 args (dword).
         * Port of CC_18_08 (asm/text/ccs/selection_menu_allow_cancel.asm).
         * Assembly: TXA (window_id byte from stream); LDX #0 (allow_cancel=0);
         *   JSR SELECTION_MENU_WITH_FOCUS — so NO cancel allowed.
         * Note: tree_18.asm labels it @SELECTION_MENU_ALLOW_CANCEL but code uses LDX #0.
         * The dword arg per textmacro EBTEXT_SELECTION_MENU_NO_CANCEL; handler only uses 1 byte
         * via gather mechanism but textmacro declares DWORD (4 bytes in stream).
         * Stores result in working_memory. */
        uint32_t cancel_target = script_read_dword(r);
        uint16_t result = selection_menu(0);  /* allow_cancel=0: NO cancel */
        set_working_memory((uint32_t)result);
        if (result == 0 && cancel_target != 0) {
            resolve_text_jump(r, cancel_target);
        }
        break;
    }
    case 0x09: {
        /* SELECTION_MENU_ALLOW_CANCEL (despite confusing label in tree_18.asm): 1 arg byte.
         * Port of CC_18_09 (asm/text/ccs/selection_menu_no_cancel.asm).
         * Assembly: TXA (window_id byte from stream); LDX #1 (allow_cancel=1);
         *   JSR SELECTION_MENU_WITH_FOCUS — so ALLOW cancel (B button exits).
         * SELECTION_MENU_WITH_FOCUS saves/restores text attributes and
         * temporarily sets focus to the specified window. */
        uint8_t window_id = script_read_byte(r);
        save_window_text_attributes();
        set_window_focus((uint16_t)window_id);
        uint16_t result = selection_menu(1);  /* allow_cancel=1: ALLOW cancel */
        restore_window_text_attributes();
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x0A:
        /* DISPLAY_MONEY_WINDOW: 0 args.
         * Port of DISPLAY_MONEY_WINDOW (asm/text/window/display_money_window.asm).
         * Saves/restores text attributes, creates money window, prints "$N". */
        display_money_window();
        break;
    case 0x0D: {
        /* DISPLAY_STATUS: 2 args (char_id, mode).
         * Port of CC_18_0D (asm/text/ccs/display_status.asm).
         * Arg 1 = char_id (0 → working_memory).
         * Arg 2 = mode: 1 → DISPLAY_STATUS_WINDOW, 2 → null (no-op). */
        uint8_t char_arg = script_read_byte(r);
        uint8_t mode = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg
                                    : (uint16_t)(get_working_memory() & 0xFFFF);
        if (mode == 1)
            display_status_window(char_id);
        /* mode == 2 is NULL_C3EF23 (no-op) */
        break;
    }
    default:
        FATAL("display_text: unknown CC 18 %02X\n", sub);
        break;
    }
}


/* --- CC 0x19 tree: inventory/character queries ---
 * Byte counts from include/textmacros.asm.
 *
 * CC 0x19 tree: inventory/character state queries.
 * Port from asm/text/ccs/cc_19_*.asm files. */
void cc_19_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x02: {
        /* LOAD_STRING_TO_MEMORY: variable-length args.
         * Port of CC_19_02 (asm/text/ccs/load_string.asm) +
         * CC_GATHER_MENU_OPTION_TEXT (asm/text/menu/cc_gather_menu_option_text.asm) +
         * CC_ADD_MENU_OPTION_WITH_CALLBACK (asm/text/menu/cc_add_menu_option_with_callback.asm).
         * Reads EB-encoded text bytes into menu option ert.buffer until
         * 0x01 or 0x02 terminator.
         * 0x02 = add menu option (no callback), done.
         * 0x01 = add menu option with 4-byte callback address following.
         * Assembly ADD_MENU_OPTION sets type=1 (counted), page=1, sound_effect=1.
         * selection_menu returns 1-based index for type=1 items. */
        char label_buf[MENU_LABEL_SIZE];
        int pos = 0;
        uint8_t b;
        while (r->ptr < r->end && pos < MENU_LABEL_SIZE - 1) {
            b = script_read_byte(r);
            if (b == 0x01) {
                /* Terminator with 4-byte callback address.
                 * Assembly: assembles 4 bytes into a callback pointer and
                 * calls ADD_MENU_OPTION(text, callback). The callback is a
                 * SNES far text pointer invoked by selection_menu when the
                 * cursor lands on this option (hover preview text). */
                uint32_t callback = script_read_dword(r);
                label_buf[pos] = '\0';
                WindowInfo *w = get_focus_window_info();
                if (w) {
                    add_menu_item(label_buf, (uint16_t)(w->menu_count + 1),
                                  w->text_x, w->text_y);
                    /* Store callback on the just-added item (asm: menu_option::script) */
                    w->menu_items[w->menu_count - 1].script = callback;
                }
                break;
            } else if (b == 0x02) {
                /* Terminator without callback — ADD_MENU_OPTION(text, NULL) */
                label_buf[pos] = '\0';
                WindowInfo *w = get_focus_window_info();
                if (w) {
                    add_menu_item(label_buf, (uint16_t)(w->menu_count + 1),
                                  w->text_x, w->text_y);
                }
                break;
            } else {
                /* EB-encoded text byte — convert to ASCII for display */
                label_buf[pos++] = eb_char_to_ascii(b);
            }
        }
        break;
    }
    case 0x04: {
        /* CLEAR_LOADED_STRINGS: 0 args.
         * Port of tree_19.asm @UNKNOWN4 → clears focus window menu options.
         * Assembly: CURRENT_FOCUS_WINDOW → CLEAR_WINDOW_MENU_OPTIONS. */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            w->menu_count = 0;
        }
        break;
    }
    case 0x05: {
        /* INFLICT_STATUS: 3 args (char_id, status_group, value).
         * Port of CC_19_05 (asm/text/ccs/inflict_character_status.asm).
         * Arg 1 = char_id (0 → working_memory).
         * Arg 2 = status_group (0 → argument_memory).
         * Arg 3 = value to inflict (the "+1" encoded status value).
         * INFLICT_STATUS_NONBATTLE(A=char_id, X=status_group, Y=value):
         * if status_group==8, sets party_status=value-1.
         * Otherwise sets afflictions[status_group-1]=value-1 for char_id.
         * Stores char_id (or 0 if not in party) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t group_arg = script_read_byte(r);
        uint8_t value = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t status_group = group_arg ? (uint16_t)group_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = inflict_status_nonbattle(char_id, status_group, (uint16_t)value);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x10: {
        /* GET_CHARACTER_NUMBER: 1 arg (party position).
         * Port of CC_19_10 (asm/text/ccs/get_character_number.asm).
         * arg==0 → use argument_memory as party position.
         * GET_CHARACTER_BY_PARTY_POSITION (C190E6): reads party_order[pos-1]
         * to get character ID at that 1-indexed party position.
         * Stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t pos;
        if (arg == 0) {
            pos = (uint16_t)(get_argument_memory() & 0xFFFF);
        } else {
            pos = (uint16_t)arg;
        }
        uint8_t char_id = 0;
        if (pos >= 1 && pos <= TOTAL_PARTY_COUNT) {
            char_id = game_state.party_order[pos - 1];
        }
        set_working_memory((uint32_t)char_id);
        break;
    }
    case 0x11: {
        /* GET_CHARACTER_NAME_LETTER: 1 arg (character ID).
         * Port of CC_19_11 (asm/text/ccs/get_letter_from_character_name.asm).
         * arg==0 → use argument_memory as character ID.
         * Gets the character's name, then reads the letter at position
         * stored in secondary_memory (1-indexed).
         * Stores the EB-encoded character code in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t char_id;
        if (arg == 0) {
            char_id = (uint16_t)(get_argument_memory() & 0xFFFF);
        } else {
            char_id = (uint16_t)arg;
        }
        uint8_t letter = 0;
        if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT) {
            const uint8_t *name = party_characters[char_id - 1].name;
            uint16_t pos = get_secondary_memory();
            if (pos >= 1 && pos <= 5) {
                letter = name[pos - 1];
            }
        }
        set_working_memory((uint32_t)letter);
        break;
    }
    case 0x14: {
        /* GET_ESCARGO_EXPRESS_ITEM: 0 args.
         * Port of tree_19.asm @GET_ESCARGO_EXPRESS_ITEM.
         * Gets escargo express item at the position stored in secondary_memory,
         * stores result in working_memory, then increments secondary_memory.
         * Assembly: GET_SECONDARY_MEMORY → DEX → LDA escargo_express_items,X
         *           → SET_WORKING_MEMORY → INCREMENT_SECONDARY_MEMORY. */
        uint16_t idx = get_secondary_memory();
        uint8_t item = 0;
        if (idx > 0 && idx <= 36) {
            item = game_state.escargo_express_items[idx - 1];
        }
        set_working_memory((uint32_t)item);
        increment_secondary_memory();
        break;
    }
    case 0x16: {
        /* GET_CHARACTER_STATUS: 2 args (char_id, status_group).
         * Port of CC_19_16 (asm/text/ccs/get_character_status.asm).
         * Arg 1 = char_id (0 → working_memory), Arg 2 = status_group (0 → argument_memory).
         * CHECK_STATUS_GROUP(A=char_id, X=status_group): group 1-7=affliction slot,
         * group 8=party_status. Returns affliction_value+1, or 0 if not in party. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t group_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t status_group = group_arg ? (uint16_t)group_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = check_status_group(status_group, char_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x18: {
        /* GET_EXP_FOR_NEXT_LEVEL: 1 arg (char_id, 0 → argument_memory).
         * Port of CC_19_18 (asm/text/ccs/get_exp_for_next_level.asm).
         * Calls GET_REQUIRED_EXP and stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t char_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint32_t exp_needed = get_required_exp(char_id);
        set_working_memory(exp_needed);
        break;
    }
    case 0x19: {
        /* GET_CHARACTER_ITEM / ADD_ITEM_ID_TO_WORK_MEMORY: 2 args.
         * Port of CC_19_19 (asm/text/ccs/get_item_number.asm) →
         * GET_CHARACTER_ITEM (asm/misc/get_character_item.asm).
         * Byte 1 = char_id (0 → working_memory).
         * Byte 2 = item_slot (0 → argument_memory), 1-indexed.
         * Reads the item at party_characters[char_id-1].items[slot-1].
         * Stores item_id in argument_memory, char_id in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint8_t item_id = 0;
        if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && slot >= 1 && slot <= 14) {
            item_id = party_characters[char_id - 1].items[slot - 1];
        }
        set_argument_memory((uint32_t)item_id);
        set_working_memory((uint32_t)char_id);
        break;
    }
    case 0x1A: {
        /* GET_ESCARGO_EXPRESS_ITEM_BY_SLOT: 1 arg (slot number, 1-indexed).
         * Port of CC_19_1A (asm/text/ccs/unknown_19_1A.asm).
         * arg==0 → use argument_memory. Reads escargo_express_items[slot-1]
         * and stores in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t slot;
        if (arg == 0) {
            slot = (uint16_t)(get_argument_memory() & 0xFFFF);
        } else {
            slot = (uint16_t)arg;
        }
        uint8_t item = 0;
        if (slot >= 1 && slot <= 36) {
            item = game_state.escargo_express_items[slot - 1];
        }
        set_working_memory((uint32_t)item);
        break;
    }
    case 0x1B: {
        /* GET_WINDOW_MENU_OPTION_COUNT: 1 arg (window_id).
         * Port of CC_19_1B (asm/text/ccs/unknown_19_1B.asm) →
         * GET_WINDOW_MENU_OPTION_COUNT (asm/text/window/get_window_menu_option_count.asm).
         * If arg==0, uses current focus window. Returns menu option count
         * via COUNT_MENU_OPTION_CHAIN. Stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t win_id = arg ? (uint16_t)arg : win.current_focus_window;
        WindowInfo *w = (win_id != WINDOW_ID_NONE) ? get_window(win_id) : NULL;
        uint16_t count = w ? w->menu_count : 0;
        set_working_memory((uint32_t)count);
        break;
    }
    case 0x1C: {
        /* TRANSFER_ITEM_TO_QUEUE: 2 args (source, slot).
         * Port of CC_19_1C (asm/text/ccs/unknown_19_1C.asm).
         * arg1=source: char_id (1-indexed) or 0xFF for escargo express. 0 → working_memory.
         * arg2=slot: inventory or escargo slot (1-indexed). 0 → argument_memory.
         * Removes item from source, then queues it via QUEUE_ITEM_FOR_CHARACTER
         * for later retrieval by CC_19_1D (GET_CHARACTER_ENCOUNTER_DATA). */
        uint8_t arg1 = script_read_byte(r);
        uint8_t arg2 = script_read_byte(r);
        uint16_t source = arg1 ? (uint16_t)arg1 : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = arg2 ? (uint16_t)arg2 : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t item_id;
        if (source == 0xFF) {
            item_id = remove_escargo_express_item(slot);
        } else {
            item_id = get_character_item(source, slot);
            remove_item_from_inventory(source, slot);
        }
        queue_item_for_character(source, item_id);
        break;
    }
    case 0x1D: {
        /* GET_CHARACTER_ENCOUNTER_DATA: 2 args (char_index, clear_flag).
         * Port of CC_19_1D (asm/text/ccs/unknown_19_1D.asm).
         * arg1==0 → use working_memory for char_index (1-indexed).
         * Reads game_state.unknownB8[index] → working_memory.
         * Reads game_state.unknownB6[index] → argument_memory.
         * If arg2 != 0, clears both bytes. */
        uint8_t arg1 = script_read_byte(r);
        uint8_t arg2 = script_read_byte(r);
        uint16_t char_index;
        if (arg1 == 0) {
            char_index = (uint16_t)(get_working_memory() & 0xFFFF);
        } else {
            char_index = (uint16_t)arg1;
        }
        if (char_index >= 1 && char_index <= 3) {
            uint8_t idx = char_index - 1;
            set_working_memory((uint32_t)game_state.unknownB8[idx]);
            set_argument_memory((uint32_t)game_state.unknownB6[idx]);
            if (arg2 != 0) {
                game_state.unknownB8[idx] = 0;
                game_state.unknownB6[idx] = 0;
            }
        }
        break;
    }
    case 0x1E:
        /* GET_CURRENT_NUMBER: 0 args.
         * Port of tree_19.asm @GET_CNUM → GET_CNUM (C1AD26).
         * Reads the CNUM (current action number) global and stores it
         * in working_memory. Set by number selector or battle code. */
        set_working_memory(get_cnum());
        break;
    case 0x1F:
        /* GET_CURRENT_INVENTORY_ITEM: 0 args.
         * Port of tree_19.asm @GET_CURRENT_ITEM → GET_CURRENT_ITEM (C1AD02).
         * Reads the CITEM (current item) global and stores it in
         * working_memory. Set by inventory display or battle code. */
        set_working_memory((uint32_t)dt.citem);
        break;
    case 0x20:
        /* GET_PLAYER_CONTROLLED_PARTY_COUNT: 0 args.
         * Port of tree_19.asm @UNKNOWN20. Returns player_controlled_party_count
         * in working_memory. */
        set_working_memory((uint32_t)game_state.player_controlled_party_count);
        break;
    case 0x21: {
        /* IS_ITEM_DRINK: 1 arg (item_id, 0 → argument_memory).
         * Port of CC_19_21 (asm/text/ccs/test_item_is_drink.asm).
         * Calls GET_ITEM_SUBTYPE_2. Stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = get_item_subtype_2(item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x22: {
        /* GET_DIR_FROM_CHAR: 4 arg bytes (byte char_id, byte entity_type, word entity_id).
         * Port of CC_19_22 (asm/text/ccs/get_direction_from_character_to_entity.asm).
         * Computes direction from a party character to any entity.
         * Calls GET_DIRECTION_BETWEEN_CHARACTER_ENTITIES.
         * Result + 1 (1-8 direction) → argument_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t entity_type_arg = script_read_byte(r);
        uint16_t entity_id_raw = script_read_word(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t entity_id = entity_id_raw ? entity_id_raw : (uint16_t)(get_argument_memory() & 0xFFFF);
        int16_t target_type = (int16_t)(entity_type_arg - 1);
        int16_t dir = get_direction_between_entities(0, (int16_t)char_id, target_type, (int16_t)entity_id);
        uint32_t result = (uint32_t)(dir + 1);
        set_argument_memory(result);
        break;
    }
    case 0x23: {
        /* GET_DIR_FROM_NPC: 5 arg bytes (word npc_id, byte entity_type, word entity_id).
         * Port of CC_19_23 (asm/text/ccs/get_direction_from_tpt_entity_to_entity.asm).
         * Computes direction from an NPC (TPT) entity to any entity.
         * Calls GET_DIRECTION_BETWEEN_NPC_ENTITIES.
         * Result + 1 (1-8 direction) → argument_memory. */
        uint16_t npc_id_raw = script_read_word(r);
        uint8_t entity_type_arg = script_read_byte(r);
        uint16_t entity_id_raw = script_read_word(r);
        uint16_t npc_id = npc_id_raw ? npc_id_raw : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t entity_id = entity_id_raw ? entity_id_raw : (uint16_t)(get_argument_memory() & 0xFFFF);
        int16_t target_type = (int16_t)(entity_type_arg - 1);
        int16_t dir = get_direction_between_entities(1, (int16_t)npc_id, target_type, (int16_t)entity_id);
        uint32_t result = (uint32_t)(dir + 1);
        set_argument_memory(result);
        break;
    }
    case 0x24: {
        /* GET_DIR_FROM_SPRITE: 5 arg bytes (word sprite_id, byte entity_type, word entity_id).
         * Port of CC_19_24 (asm/text/ccs/get_direction_from_sprite_entity_to_entity.asm).
         * Computes direction from a sprite entity to any entity.
         * Calls GET_DIRECTION_BETWEEN_SPRITE_ENTITIES.
         * Result + 1 (1-8 direction) → argument_memory.
         * NOTE: Fixed byte count — was incorrectly 4 in the stub, actually 5. */
        uint16_t sprite_id_raw = script_read_word(r);
        uint8_t entity_type_arg = script_read_byte(r);
        uint16_t entity_id_raw = script_read_word(r);
        uint16_t sprite_id = sprite_id_raw ? sprite_id_raw : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t entity_id = entity_id_raw ? entity_id_raw : (uint16_t)(get_argument_memory() & 0xFFFF);
        int16_t target_type = (int16_t)(entity_type_arg - 1);
        int16_t dir = get_direction_between_entities(2, (int16_t)sprite_id, target_type, (int16_t)entity_id);
        uint32_t result = (uint32_t)(dir + 1);
        set_argument_memory(result);
        break;
    }
    case 0x25: {
        /* IS_ITEM_CONDIMENT: 1 arg (item_id, 0 → argument_memory).
         * Port of CC_19_25 (asm/text/ccs/test_item_is_condiment.asm).
         * Calls FIND_CONDIMENT(item_id) → checks if food-type, then searches
         * CURRENT_ATTACKER's inventory for a condiment. Returns condiment item_id
         * or 0 if none. Stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = find_condiment(item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x26: {
        /* SET_RESPAWN_POINT: 1 arg (value, 0 → argument_memory).
         * Port of CC_19_26 (asm/text/ccs/set_respawn_point.asm).
         * Calls SET_TELEPORT_BOX_DESTINATION. */
        uint8_t arg = script_read_byte(r);
        uint16_t value = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        set_teleport_box_destination((uint8_t)value);
        break;
    }
    case 0x27: {
        /* RESOLVE_CC_TABLE_DATA: 1 arg (table index, 0 → argument_memory).
         * Port of CC_19_27 (asm/text/ccs/unknown_19_27.asm) →
         * RESOLVE_CC_TABLE_DATA (asm/battle/resolve_cc_table_data.asm).
         * Resolves the CC_1C_01_TABLE entry value and stores in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t table_index = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        int type = CC_TABLE_TYPE_INT;
        int str_len = 0;
        uintptr_t value = resolve_cc_table_data(table_index, &type, &str_len);
        set_working_memory((uint32_t)value);
        break;
    }
    case 0x28: {
        /* GET_LETTER_FROM_STAT: 1 arg (table index, via X register).
         * Port of CC_19_28 (asm/text/ccs/get_letter_from_stat.asm).
         * Gets a specific character/byte from a string stat entry.
         * Uses secondary_memory for bounds check and character position.
         * Stores result byte in working_memory (0 if out of range). */
        uint8_t arg = script_read_byte(r);
        uint16_t table_index = (uint16_t)arg;

        /* First secondary_memory read: bounds check value */
        uint16_t bounds = get_secondary_memory();
        uint8_t entry_size = get_cc_table_entry_size(table_index);

        if (entry_size < bounds) {
            set_working_memory(0);
        } else {
            /* Second secondary_memory read: character position (1-indexed) */
            uint16_t char_pos = get_secondary_memory();

            /* Resolve the data to get pointer to the field */
            int type = CC_TABLE_TYPE_INT;
            int str_len = 0;
            uintptr_t value = resolve_cc_table_data(table_index, &type, &str_len);

            uint8_t result = 0;
            if (type == CC_TABLE_TYPE_STRING && char_pos >= 1) {
                const uint8_t *data = (const uint8_t *)value;
                if (data && (int)(char_pos - 1) < str_len) {
                    result = data[char_pos - 1];
                }
            }
            set_working_memory((uint32_t)result);
        }
        break;
    }
    default:
        FATAL("display_text: unknown CC 19 %02X\n", sub);
        break;
    }
}

void cc_1a_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x00: /* fall through */
    case 0x01: {
        /* PARTY_MEMBER_SELECTION_MENU: 17 args (4 dword script_ptrs + 1 byte mode).
         * CC_1A_00 (uncancellable) and CC_1A_01 (cancellable).
         * Port of asm/text/ccs/party_selection_menu_uncancellable.asm and
         * asm/text/ccs/party_selection_menu.asm.
         *
         * Arguments: 4 x uint32_t text script pointers (one per party member,
         * used in battle mode for displaying character-specific text), followed
         * by 1 byte mode (0=overworld, 1=battle HPPP column selection).
         *
         * Calls PARTY_CHARACTER_SELECTOR (C1244C.asm) with:
         *   A = pointer to script_ptrs, X = mode, Y = allow_cancel (0 or 1). */
        uint32_t script_ptrs[4];
        script_ptrs[0] = script_read_dword(r);
        script_ptrs[1] = script_read_dword(r);
        script_ptrs[2] = script_read_dword(r);
        script_ptrs[3] = script_read_dword(r);
        uint8_t mode = script_read_byte(r);
        uint16_t allow_cancel = (sub == 0x01) ? 1 : 0;
        uint16_t result = party_character_selector(script_ptrs, mode, allow_cancel);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x04: {
        /* SELECTION_MENU_NO_CANCEL: 0 args.
         * Port of tree_1A.asm @SELECTION_MENU_NO_CANCEL.
         * Runs selection_menu(0), stores result, clears focus window menu options. */
        uint16_t result = selection_menu(0);
        set_working_memory((uint32_t)result);
        WindowInfo *w = get_focus_window_info();
        if (w) w->menu_count = 0;
        break;
    }
    case 0x05: {
        /* SHOW_CHARACTER_INVENTORY: 2 args (window_id, char_source).
         * Port of CC_1A_05 (asm/text/ccs/show_character_inventory.asm).
         * Opens a window showing the character's inventory items.
         * char_source: 0 = read char_id from argument_memory, else = char_id directly. */
        uint8_t window_id_arg = script_read_byte(r);
        uint8_t char_source = script_read_byte(r);
        show_character_inventory(window_id_arg, char_source);
        break;
    }
    case 0x06: {
        /* DISPLAY_SHOP_MENU: 1 arg (shop_id).
         * Port of CC_1A_06 (asm/text/ccs/display_shop_menu.asm).
         * Opens the shop item selection menu for the given shop_id.
         *
         * CC_1A_06 pre-processing (US path):
         *   CLEAR_INSTANT_PRINTING, CREATE_WINDOW(current), WINDOW_TICK
         *   If shop_id==0, reads shop from argument_memory.
         * Then calls OPEN_STORE_MENU which builds item list with prices. */
        uint8_t shop_arg = script_read_byte(r);
        uint16_t shop_id;
        if (shop_arg == 0) {
            shop_id = (uint16_t)get_argument_memory();
        } else {
            shop_id = shop_arg;
        }
        uint16_t result = open_store_menu(shop_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x07: {
        /* SELECT_ESCARGO_EXPRESS_ITEM: 0 args.
         * Port of tree_1A.asm @SELECT_ESCARGO_EXPRESS_ITEM.
         * Calls SELECT_ESCARGO_EXPRESS_ITEM (C19A43.asm). */
        uint16_t result = select_escargo_express_item();
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x08: {
        /* SELECTION_MENU_NO_CANCEL (variant): 0 args.
         * Port of tree_1A.asm @SELECTION_MENU_NO_CANCEL_2.
         * Same as 0x04 but does NOT clear menu options afterward. */
        uint16_t result = selection_menu(0);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x09: {
        /* SELECTION_MENU_ALLOW_CANCEL: 0 args.
         * Port of tree_1A.asm @SELECTION_MENU_ALLOW_CANCEL.
         * Runs selection_menu(1), stores result in working_memory. */
        uint16_t result = selection_menu(1);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x0A: {
        /* DISPLAY_TELEPHONE_CONTACT: 0 args.
         * Port of tree_1A.asm @DISPLAY_TELEPHONE_CONTACT.
         * Calls DISPLAY_TELEPHONE_CONTACT_TEXT (C1AC00.asm). */
        uint16_t result = display_telephone_contact_text();
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x0B: {
        /* OPEN_TELEPORT_MENU: 0 args.
         * Port of tree_1A.asm @OPEN_TELEPORT_MENU.
         * Calls OPEN_TELEPORT_DESTINATION_MENU (C1AAFA.asm). */
        uint16_t result = open_teleport_destination_menu();
        set_working_memory((uint32_t)result);
        break;
    }
    default:
        FATAL("display_text: unknown CC 1A %02X\n", sub);
        break;
    }
}


/* --- CC 0x1B tree: memory operations ---
 * Port of CC_1B_TREE (asm/text/ccs/tree_1B.asm).
 *
 * These manage the text script's per-window memory registers used for
 * conditional logic and branching.
 *   0x00: COPY_ACTIVE_MEMORY_TO_STORAGE — save working/arg/secondary to backups
 *   0x01: COPY_STORAGE_MEMORY_TO_ACTIVE — restore from backups
 *   0x02: JUMP_IF_FALSE(addr) — branch if working memory is 0
 *   0x03: JUMP_IF_TRUE(addr) — branch if working memory is non-zero
 *   0x04: SWAP_WORKING_AND_ARG_MEMORY — exchange working and argument registers
 *   0x05: COPY_ACTIVE_TO_WORKING — save all 3 registers to global backups
 *   0x06: COPY_WORKING_TO_ACTIVE — restore all 3 from global backups */
void cc_1b_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x00: {
        /* TRANSFER_ACTIVE_MEM_STORAGE: copy active → storage backup.
         * Port of TRANSFER_ACTIVE_MEM_STORAGE (asm/text/transfer_active_mem_storage.asm). */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            w->working_memory_storage = w->working_memory;
            w->argument_memory_storage = w->argument_memory;
            w->secondary_memory_storage = w->secondary_memory;
        }
        break;
    }
    case 0x01: {
        /* TRANSFER_STORAGE_MEM_ACTIVE: copy storage backup → active.
         * Port of TRANSFER_STORAGE_MEM_ACTIVE (asm/text/transfer_storage_mem_active.asm). */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            w->working_memory = w->working_memory_storage;
            w->argument_memory = w->argument_memory_storage;
            w->secondary_memory = w->secondary_memory_storage;
        }
        break;
    }
    case 0x02: {
        /* JUMP_IF_FALSE: 4 args (dword jump target).
         * If working_memory == 0 (NULL), jump to target; else skip.
         * Port of tree_1B.asm @UNKNOWN5 (CMP32 with NULL). */
        uint32_t target = script_read_dword(r);
        if (get_working_memory() == 0) {
            resolve_text_jump(r, target);
        }
        break;
    }
    case 0x03: {
        /* JUMP_IF_TRUE: 4 args (dword jump target).
         * If working_memory != 0 (non-NULL), jump to target; else skip.
         * Port of tree_1B.asm @UNKNOWN8 (opposite of case 02). */
        uint32_t target = script_read_dword(r);
        if (get_working_memory() != 0) {
            resolve_text_jump(r, target);
        }
        break;
    }
    case 0x04: {
        /* SWAP_WORKING_AND_ARG_MEMORY: 0 args.
         * Port of tree_1B.asm @UNKNOWN11. */
        WindowInfo *w = get_focus_window_info();
        if (w) {
            uint32_t tmp = w->working_memory;
            w->working_memory = w->argument_memory;
            w->argument_memory = tmp;
        }
        break;
    }
    case 0x05: {
        /* COPY_ACTIVE_TO_GLOBAL_BACKUP: 0 args.
         * Port of tree_1B.asm @UNKNOWN12. Saves all 3 registers to globals. */
        dt.text_main_register_backup = get_working_memory();
        dt.text_sub_register_backup = get_argument_memory();
        dt.text_loop_register_backup = (uint8_t)get_secondary_memory();
        break;
    }
    case 0x06: {
        /* COPY_GLOBAL_BACKUP_TO_ACTIVE: 0 args.
         * Port of tree_1B.asm @UNKNOWN13. Restores all 3 from globals. */
        set_working_memory(dt.text_main_register_backup);
        set_argument_memory(dt.text_sub_register_backup);
        set_secondary_memory((uint16_t)(dt.text_loop_register_backup & 0xFF));
        break;
    }
    default:
        FATAL("display_text: unknown CC 1B %02X\n", sub);
        break;
    }
}


void cc_1c_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x00: {
        /* TEXT_COLOUR_EFFECTS: 1 arg (palette index).
         * Port of CC_1C_00 (asm/text/ccs/text_effects.asm) → SET_WINDOW_PALETTE_INDEX.
         * Sets curr_tile_attributes = palette * 1024 (SNES tilemap palette bits). */
        uint8_t palette = script_read_byte(r);
        WindowInfo *w = get_focus_window_info();
        if (w) w->curr_tile_attributes = palette * 1024;
        break;
    }
    case 0x01: {
        /* PRINT_STAT: 1 arg (stat index).
         * Port of CC_1C_01 (asm/text/ccs/print_stat.asm) → PRINT_CC_TABLE_VALUE.
         * arg==0 → use argument_memory. Looks up and prints the stat. */
        uint8_t arg = script_read_byte(r);
        uint16_t stat_index = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        print_cc_table_value(stat_index);
        break;
    }
    case 0x02: {
        /* PRINT_CHAR_NAME: 1 arg.
         * Port of CC_1C_02 (asm/text/ccs/print_character_name.asm).
         * arg=0xFF → use working_memory, arg=0 → use argument_memory,
         * else arg = character ID (1-based).
         * Characters 1-4 = party members (name from char_struct).
         * Characters 5-6 = King/Buzz Buzz (name from game_state.pet_name). */
        uint8_t arg = script_read_byte(r);
        uint16_t char_id;
        if (arg == 0xFF) {
            char_id = (uint16_t)(get_working_memory() & 0xFFFF);
        } else if (arg == 0) {
            char_id = (uint16_t)(get_argument_memory() & 0xFFFF);
        } else {
            char_id = arg;
        }
        /* Print EB-encoded name directly (no lossy ASCII round-trip) */
        const uint8_t *name_data = NULL;
        int name_max = 0;
        if (char_id >= 1 && char_id <= 4) {
            name_data = party_characters[char_id - 1].name;
            name_max = 5;
        } else if (char_id == PARTY_MEMBER_POKEY) {
            name_data = game_state.pet_name;
            name_max = 6;
        }
        if (name_data) {
            if (dt.allow_text_overflow) {
                int len = 0;
                while (len < name_max && name_data[len] != 0x00) len++;
                print_eb_string(name_data, len);
            } else {
                print_string_with_wordwrap(name_data, (int16_t)name_max);
            }
        }
        break;
    }
    case 0x03: {
        /* PRINT_CHAR: 1 arg (EB character code).
         * Port of CC_1C_03 (asm/text/ccs/print_character.asm) → PRINT_LETTER.
         * arg==0 → use argument_memory. Prints a single EB-encoded character. */
        uint8_t arg = script_read_byte(r);
        uint16_t char_code;
        if (arg == 0) {
            char_code = (uint16_t)(get_argument_memory() & 0xFFFF);
        } else {
            char_code = (uint16_t)arg;
        }
        uint8_t eb = (uint8_t)char_code;
        print_eb_string(&eb, 1);
        break;
    }
    case 0x04:
        /* OPEN_HP_PP_WINDOWS: 0 args.
         * Port of tree_1C.asm @SHOW_HPPP_WINDOWS → SHOW_HPPP_WINDOWS.
         * Sets ow.render_hppp_windows=1, ow.redraw_all_windows=1,
         * ow.currently_drawn_hppp_windows=-1. */
        show_hppp_windows();
        break;
    case 0x05: {
        /* PRINT_ITEM_NAME: 1 arg (item_id).
         * Port of CC_1C_05 (asm/text/ccs/print_item_name.asm) →
         * PRINT_ITEM_TYPE (asm/text/print_item_type.asm).
         * arg==0 → use argument_memory. Looks up item name (25 bytes,
         * EB-encoded) from ITEM_CONFIGURATION_TABLE and prints it. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        const ItemConfig *entry = get_item_entry(item_id);
        if (entry) {
            print_text_with_word_splitting(entry->name, ITEM_NAME_LEN);
        }
        break;
    }
    case 0x06: {
        /* PRINT_TELEPORT_DEST_NAME: 1 arg (dest_id).
         * Port of CC_1C_06 (asm/text/ccs/print_teleport_destination_name.asm).
         * arg==0 → use argument_memory. Loads PSI teleport destination table,
         * indexes by dest_id, prints the 25-byte EB-encoded name. */
        uint8_t arg = script_read_byte(r);
        uint16_t dest_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (dest_id < PSI_TELEPORT_DEST_MAX_ENTRIES) {
            const uint8_t *entry = psi_teleport_dest_data +
                                   (uint32_t)dest_id * PSI_TELEPORT_DEST_ENTRY_SIZE;
            print_string_with_wordwrap(entry, PSI_TELEPORT_DEST_NAME_LEN);
        }
        break;
    }
    case 0x07: {
        /* PRINT_HORIZONTAL_TEXT_STRING: 1 arg (column count).
         * Port of CC_1C_07 (asm/text/ccs/print_horizontal_strings.asm).
         * If arg==0, uses argument_memory for column count.
         * Calls open_window_and_print_menu with start_index=1 (renumber items from 1). */
        uint8_t columns_arg = script_read_byte(r);
        uint16_t columns = columns_arg ? (uint16_t)columns_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        open_window_and_print_menu(columns, 1);
        break;
    }
    case 0x08: {
        /* PRINT_SPECIAL_GFX / DISPATCH_WINDOW_BORDER_ANIMATION: 1 arg (mode).
         * Port of CC_1C_08 (asm/text/ccs/print_special_graphics.asm).
         * mode=1: animate window border, mode=2: animate border + HPPP update. */
        uint8_t mode = script_read_byte(r);
        dispatch_window_border_animation((uint16_t)mode);
        break;
    }
    case 0x09: {
        /* SET_WINDOW_NUMBER_PADDING: 1 arg (padding value).
         * Port of CC_1C_09 (asm/text/ccs/unknown_1C_09.asm) → SET_WINDOW_NUMBER_PADDING.
         * Sets the number_padding field on the focus window, controlling
         * leading space padding when printing numbers with PRINT_NUMBER. */
        uint8_t padding = script_read_byte(r);
        WindowInfo *w = get_focus_window_info();
        if (w) w->number_padding = padding;
        break;
    }
    case 0x0A: {
        /* PRINT_NUMBER: 4 args (dword value).
         * Port of CC_1C_0A (asm/text/ccs/print_number.asm) → PRINT_NUMBER.
         * Assembles 4 bytes into a 32-bit value. If 0, uses argument_memory.
         * Prints the decimal number with leading spaces per number_padding. */
        uint32_t value = script_read_dword(r);
        if (value == 0) value = (uint32_t)get_argument_memory();
        /* Delegate to print_number() like the assembly does
         * (JSR PRINT_NUMBER in ccs/print_number.asm line 57). */
        print_number((int)value, 0);
        break;
    }
    case 0x0B: {
        /* PRINT_MONEY_AMOUNT: 4 args (dword value).
         * Port of CC_1C_0B (asm/text/ccs/print_money_amount.asm).
         * US: → PRINT_MONEY_IN_WINDOW (prints "$N").
         * JP: → PRINT_CURRENCY_VALUE (prints "Nドル").
         * Assembles 4 bytes into 32-bit value. If 0, uses argument_memory. */
        uint32_t value = script_read_dword(r);
        if (value == 0) value = (uint32_t)get_argument_memory();
        print_money_in_window(value);
        break;
    }
    case 0x0C: {
        /* PRINT_VERTICAL_TEXT_STRING: 1 arg (column count).
         * Port of CC_1C_0C (asm/text/ccs/print_vertical_strings.asm).
         * If arg==0, uses argument_memory for column count.
         * Calls open_window_and_print_menu with start_index=0 (keep existing userdata). */
        uint8_t columns_arg = script_read_byte(r);
        uint16_t columns = columns_arg ? (uint16_t)columns_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        open_window_and_print_menu(columns, 0);
        break;
    }
    case 0x0D: {
        /* PRINT_ACTION_USER_NAME: 0 args.
         * Port of CC_1C_0D / @PRINT_ENEMY_ATTACKER_NAME (asm/text/ccs/tree_1C.asm:110-125).
         * USA: prints article prefix ("The"/"the"), then attacker name.
         * Calls RETURN_BATTLE_ATTACKER_ADDRESS → PRINT_STRING_WITH_WORDWRAP. */
        print_enemy_article(0);  /* USA only: conditional article */
        char *attacker_name = return_battle_attacker_address();
        /* Assembly: PRINT_STRING_WITH_WORDWRAP with max_len=80 */
        if (attacker_name[0] != '\0')
            print_string_with_wordwrap((const uint8_t *)attacker_name, BATTLE_NAME_ATTACKER_SIZE);
        break;
    }
    case 0x0E: {
        /* PRINT_ACTION_TARGET_NAME: 0 args.
         * Port of CC_1C_0E / @PRINT_ENEMY_TARGET_NAME (asm/text/ccs/tree_1C.asm:126-141).
         * USA: prints article prefix, then target name. */
        print_enemy_article(1);  /* USA only: conditional article */
        char *target_name = return_battle_target_address();
        /* Assembly: PRINT_STRING_WITH_WORDWRAP with max_len=80 */
        if (target_name[0] != '\0')
            print_string_with_wordwrap((const uint8_t *)target_name, BATTLE_NAME_TARGET_SIZE);
        break;
    }
    case 0x0F: {
        /* PRINT_ACTION_AMOUNT: 0 args.
         * Port of CC_1C_0F / @PRINT_ATTACK_NUMBER (asm/text/ccs/tree_1C.asm:142-146).
         * Calls GET_CNUM, then PRINT_NUMBER to print the current action number. */
        uint32_t value = get_cnum();
        char buf[12];
        snprintf(buf, sizeof(buf), "%u", (unsigned)value);
        print_string(buf);
        break;
    }
    case 0x11: {
        /* HINT_NEW_LINE: 1 arg (eb_char code).
         * Port of CC_1C_11 (asm/text/ccs/print_party_or_hint_new_line.asm).
         * If arg == 0, uses argument_memory. Calls CHECK_TEXT_FITS_IN_WINDOW
         * to insert a newline if the given character wouldn't fit on the
         * current line. */
        uint8_t arg = script_read_byte(r);
        uint16_t eb_char = arg;
        if (eb_char == 0) eb_char = (uint16_t)get_argument_memory();
        check_text_fits_in_window(eb_char);
        break;
    }
    case 0x12: {
        /* PRINT_PSI_NAME: 1 arg (ability_id).
         * Port of CC_1C_12 (asm/text/ccs/print_psi_name.asm) →
         * PRINT_PSI_ABILITY_NAME (asm/text/print_psi_ability_name.asm).
         * arg==0 → use argument_memory. Looks up ability in PSI_ABILITY_TABLE,
         * prints name from PSI_NAME_TABLE and level suffix from PSI_SUFFIXES. */
        uint8_t arg = script_read_byte(r);
        uint16_t ability_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        print_psi_ability_name(ability_id);
        break;
    }
    case 0x13: {
        /* DISPLAY_PSI_ANIMATION: 2 args (ally_effect_id, enemy_effect_id).
         * Port of CC_1C_13 (asm/text/ccs/display_battle_animation.asm).
         * Both args are 1-indexed; assembly decrements before use.
         * Calls CHECK_BATTLE_TARGET_TYPE → APPLY_PSI_BATTLE_EFFECT.
         * Result (bool: true=enemy/ghost, false=ally) stored in working_memory. */
        uint8_t arg0 = script_read_byte(r);
        uint8_t arg1 = script_read_byte(r);
        if (dt.blinking_triangle_flag) {
            bool result = check_battle_target_type(
                (uint16_t)(arg0 - 1), (uint16_t)(arg1 - 1));
            set_working_memory((uint32_t)(result ? 1 : 0));
        }
        break;
    }
    case 0x14: {
        /* GET_ATTACKER_GENDER: 1 arg (query_type).
         * Port of CC_1C_14 (asm/text/ccs/get_gender_etc.asm).
         * arg==1 → return gender: enemy → enemy_config_table[id].gender,
         *          ally → 2 if Paula (id==2), else 1.
         * arg!=1 → return count: enemy → min(bt.enemies_in_battle, 3),
         *          ally → min(count_alive_party_members(), 3).
         * Result stored in working_memory. */
        uint8_t query_type = script_read_byte(r);
        Battler *atk = battler_from_offset(bt.current_attacker);
        uint16_t result;
        if (atk->ally_or_enemy == 1) {
            /* Enemy */
            if (query_type == 1) {
                /* Return enemy gender from config table */
                if (enemy_config_table) {
                    result = enemy_config_table[atk->id].gender;
                } else {
                    result = 1;
                }
            } else {
                /* Return enemy count, capped to 3 */
                result = (bt.enemies_in_battle > 3) ? 3 : bt.enemies_in_battle;
            }
        } else {
            /* Ally */
            if (query_type == 1) {
                /* Paula (id==2) is female (2), others male (1) */
                result = (atk->id == PARTY_MEMBER_PAULA) ? 2 : 1;
            } else {
                /* Return alive party member count, capped to 3 */
                uint16_t alive = count_alive_party_members();
                result = (alive > 3) ? 3 : alive;
            }
        }
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x15: {
        /* GET_TARGET_GENDER: 1 arg (query_type).
         * Port of CC_1C_15 (asm/text/ccs/switch_gender_etc.asm).
         * Same logic as CC_1C_14 but uses bt.current_target instead of
         * bt.current_attacker. */
        uint8_t query_type = script_read_byte(r);
        Battler *tgt = battler_from_offset(bt.current_target);
        uint16_t result;
        if (tgt->ally_or_enemy == 1) {
            /* Enemy */
            if (query_type == 1) {
                if (enemy_config_table) {
                    result = enemy_config_table[tgt->id].gender;
                } else {
                    result = 1;
                }
            } else {
                result = (bt.enemies_in_battle > 3) ? 3 : bt.enemies_in_battle;
            }
        } else {
            /* Ally */
            if (query_type == 1) {
                result = (tgt->id == PARTY_MEMBER_PAULA) ? 2 : 1;
            } else {
                uint16_t alive = count_alive_party_members();
                result = (alive > 3) ? 3 : alive;
            }
        }
        set_working_memory((uint32_t)result);
        break;
    }
    default:
        FATAL("display_text: unknown CC 1C %02X\n", sub);
        break;
    }
}


/* --- CC 0x1D tree: inventory/money ---
 * Byte counts verified against include/textmacros.asm.
 * Port of asm/text/ccs/give_item_to_character.asm, wallet_increase.asm, etc.
 *
 * Common argument patterns:
 *   Two-arg gather: byte1 = char_id (0 → working_memory),
 *                   byte2 = item/value (0 → argument_memory).
 *   16-bit value: .WORD assembled from 2 bytes (0 → argument_memory).
 *   32-bit value: .DWORD assembled from 4 bytes (0 → argument_memory). */
void cc_1d_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    case 0x00: {
        /* GIVE_ITEM_TO_CHARACTER: 2 data bytes.
         * Port of CC_1D_00 (asm/text/ccs/give_item_to_character.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_id (0 → argument_memory).
         * Assembly: byte1→CC_ARGUMENT_STORAGE→A, byte2→X.
         * Calls GIVE_ITEM_TO_CHARACTER(A=char_id, X=item_id): supports 0xFF
         * for all-party, handles teddy bear and transform side effects.
         * Stores result (receiving char_id or 0) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = give_item_to_character(char_id, item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x01: {
        /* TAKE_ITEM_FROM_CHARACTER: 2 data bytes.
         * Port of CC_1D_01 (asm/text/ccs/take_item_from_character.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_id (0 → argument_memory).
         * Assembly: byte1→CC_ARGUMENT_STORAGE→A, byte2→X.
         * Calls TAKE_ITEM_FROM_CHARACTER(A=char_id, X=item_id): searches
         * inventory for matching item, removes via REMOVE_ITEM_FROM_INVENTORY.
         * Supports 0xFF for all-party search. Handles unequip/shift/side effects.
         * Stores result (char_id or 0) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = take_item_from_character(char_id, item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x02: {
        /* TEST_ITEM_TYPE_MATCH: 1 arg (type_to_compare).
         * Port of CC_1D_02 (asm/text/ccs/test_inventory_full.asm).
         * Reads item_id from argument_memory, gets its type category via
         * GET_ITEM_TYPE, compares with the arg. Result: 1=match, 0=no match.
         * Stores sign-extended result in working_memory. */
        uint8_t type_arg = script_read_byte(r);
        uint16_t item_id = (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t item_type = get_item_type(item_id);
        int16_t result = (item_type == (uint16_t)type_arg) ? 1 : 0;
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x03: {
        /* GET_PLAYER_HAS_INVENTORY_ROOM: 1 data byte.
         * Port of CC_1D_03 (asm/text/ccs/test_inventory_not_full.asm)
         * → FIND_INVENTORY_SPACE2 (asm/misc/find_inventory_space2.asm)
         * → FIND_INVENTORY_SPACE (asm/misc/find_inventory_space.asm).
         * Byte 1 = char_id (0 → argument_memory).
         * Returns char_id (1-based) if room found, 0 if full.
         * char_id == 0xFF → all-party search. */
        uint8_t char_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        int32_t result = 0;  /* 0 = no room (matches assembly FALSE) */
        if (char_id == 0xFF) {
            /* All-party search: find first party member with inventory space */
            for (int p = 0; p < (game_state.player_controlled_party_count & 0xFF); p++) {
                uint8_t member = game_state.party_members[p];
                if (member >= 1 && member <= TOTAL_PARTY_COUNT) {
                    CharStruct *c = &party_characters[member - 1];
                    bool has_space = false;
                    for (int i = 0; i < 14; i++) {
                        if (c->items[i] == 0) { has_space = true; break; }
                    }
                    if (has_space) { result = (int32_t)member; break; }
                }
            }
        } else if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT) {
            CharStruct *c = &party_characters[char_id - 1];
            for (int i = 0; i < 14; i++) {
                if (c->items[i] == 0) {
                    result = (int32_t)char_id;  /* return char_id, not slot index */
                    break;
                }
            }
        }
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x04: {
        /* CHECK_IF_ITEM_IS_EQUIPPED: 2 data bytes.
         * Port of CC_1D_04 (asm/text/ccs/test_character_doesnt_have_item.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_id (0 → argument_memory).
         * Assembly: byte1→CC_ARGUMENT_STORAGE→A, byte2→X.
         * Calls IS_ITEM_EQUIPPED_BY_ID(A=char_id, X=item_id): dereferences
         * equipment slot values to get actual item IDs and compares.
         * Result (sign-extended) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        int16_t result = (int16_t)is_item_equipped_by_id(char_id, item_id);
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x05: {
        /* CHECK_IF_CHARACTER_HAS_ITEM: 2 data bytes.
         * Port of CC_1D_05 (asm/text/ccs/test_character_has_item.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_id (0 → argument_memory).
         * Assembly: byte1→CC_ARGUMENT_STORAGE→A, byte2→X.
         * Calls FIND_ITEM_IN_INVENTORY2(A=char_id, X=item_id): supports
         * 0xFF for all-party search. Returns char_id if found, 0 if not.
         * Result (sign-extended) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = find_item_in_inventory2(char_id, item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x06: {
        /* ADD_TO_ATM: 4 data bytes (.DWORD).
         * Port of CC_1D_06 (asm/text/ccs/atm_increase.asm).
         * Builds 32-bit deposit amount. If zero, use argument_memory.
         * Calls DEPOSIT_INTO_ATM: bank_balance = min(balance + amount, 9999999). */
        uint32_t amount = script_read_dword(r);
        if (amount == 0) amount = (uint32_t)get_argument_memory();
        uint32_t new_balance = game_state.bank_balance + amount;
        if (new_balance > ATM_LIMIT) new_balance = ATM_LIMIT;
        game_state.bank_balance = new_balance;
        break;
    }
    case 0x07: {
        /* TAKE_FROM_ATM: 4 data bytes (.DWORD).
         * Port of CC_1D_07 (asm/text/ccs/atm_decrease.asm).
         * Builds 32-bit withdrawal amount. If zero, use argument_memory.
         * Calls WITHDRAW_FROM_ATM: subtracts from bank_balance if sufficient.
         * Stores withdrawal amount in working_memory. */
        uint32_t amount = script_read_dword(r);
        if (amount == 0) amount = (uint32_t)get_argument_memory();
        withdraw_from_atm(amount);
        set_working_memory(amount);
        break;
    }
    case 0x08: {
        /* ADD_TO_WALLET: 2 data bytes (.WORD).
         * Port of CC_1D_08 (asm/text/ccs/wallet_increase.asm).
         * Builds 16-bit amount. If zero, use argument_memory.
         * Calls INCREASE_WALLET_BALANCE: money_carried = min(balance + amount, 99999).
         * Stores new balance in working_memory. */
        uint16_t amount = script_read_word(r);
        uint32_t amt32 = amount ? (uint32_t)amount : (uint32_t)get_argument_memory();
        uint32_t new_balance = increase_wallet_balance(amt32);
        set_working_memory(new_balance);
        break;
    }
    case 0x09: {
        /* TAKE_FROM_WALLET: 2 data bytes (.WORD).
         * Port of CC_1D_09 (asm/text/ccs/wallet_decrease.asm).
         * Builds 16-bit amount. If zero, use argument_memory.
         * Calls DECREASE_WALLET_BALANCE: returns 0 (success) or 1 (insufficient).
         * Stores result in working_memory (sign-extended). */
        uint16_t amount = script_read_word(r);
        uint32_t amt32 = amount ? (uint32_t)amount : (uint32_t)get_argument_memory();
        uint16_t result = decrease_wallet_balance(amt32);
        set_working_memory((uint32_t)(int32_t)(int16_t)result);
        break;
    }
    case 0x0A: {
        /* GET_BUY_PRICE_OF_ITEM: 1 arg (item_id, 0 → argument_memory).
         * Port of CC_1D_0A (asm/text/ccs/get_item_price.asm).
         * Reads item.cost from ITEM_CONFIGURATION_TABLE, stores in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        const ItemConfig *entry = get_item_entry(item_id);
        uint16_t cost = entry ? entry->cost : 0;
        set_working_memory((uint32_t)cost);
        break;
    }
    case 0x0B: {
        /* GET_SELL_PRICE_OF_ITEM: 1 arg (item_id, 0 → argument_memory).
         * Port of CC_1D_0B (asm/text/ccs/get_item_sell_price.asm).
         * Reads item.cost / 2 from ITEM_CONFIGURATION_TABLE, stores in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        const ItemConfig *entry = get_item_entry(item_id);
        uint16_t cost = entry ? entry->cost : 0;
        set_working_memory((uint32_t)(cost >> 1));
        break;
    }
    case 0x0C: {
        /* ESCARGO_EXPRESS_ITEM_STATUS: 2 args (char_id, item_slot).
         * Port of CC_1D_0C (asm/text/ccs/unknown_1D_0C.asm).
         * Arg 1 = char_id (0 → working_memory), Arg 2 = item_slot (0 → argument_memory).
         * Checks IS_ESCARGO_EXPRESS_FULL: if full, base=2, else base=0.
         * Then reads item at char's inventory slot, checks ITEM_FLAGS::CANNOT_STORE (0x40).
         * If flag set, flag_bit=1, else 0. Result = base | flag_bit.
         * Stores sign-extended result in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t base = is_escargo_express_full() ? 2 : 0;
        uint16_t flag_bit = 0;
        if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && slot >= 1 && slot <= 14) {
            uint8_t item_id = party_characters[char_id - 1].items[slot - 1];
            const ItemConfig *entry = get_item_entry(item_id);
            if (entry && (entry->flags & 0x40))
                flag_bit = 1;
        }
        int16_t result = (int16_t)(base | flag_bit);
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x0D: {
        /* CHARACTER_HAS_AILMENT: 3 args (char_id, status_group, expected_value).
         * Port of CC_1D_0D (asm/text/ccs/test_character_status.asm).
         * Arg 1 = char_id (0 → working_memory).
         * Arg 2 = status_group (0 → argument_memory).
         * Arg 3 = expected status value (from @VIRTUAL02, third gathered arg).
         * Calls CHECK_STATUS_GROUP(A=char_id, X=status_group), compares result
         * with expected. Result: 1 if match, 0 if not. Sign-extended. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t group_arg = script_read_byte(r);
        uint8_t expected = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t status_group = group_arg ? (uint16_t)group_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t actual = check_status_group(status_group, char_id);
        int16_t result = (actual == (uint16_t)expected) ? 1 : 0;
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x0E: {
        /* GIVE_ITEM_TO_CHARACTER_B: 2 data bytes.
         * Port of CC_1D_0E (asm/text/ccs/give_item_to_character_2.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = item_id (0 → argument_memory).
         * Gives item to character (or first available if char_id=0xFF).
         * Stores next empty slot index in argument_memory.
         * Stores result (receiving char_id or 0) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t item_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_id = item_arg ? (uint16_t)item_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = give_item_to_character(char_id, item_id);
        uint16_t empty = 0;
        if (result) {
            empty = find_empty_inventory_slot(result);
        }
        set_argument_memory((uint32_t)empty);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x0F: {
        /* TAKE_ITEM_FROM_CHARACTER_2: 2 data bytes.
         * Port of CC_1D_0F (asm/text/ccs/take_item_from_character_2.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = slot (0 → argument_memory).
         * Gets item at slot, stores item_id in argument_memory.
         * Removes item from inventory, stores char_id in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t item_id = get_character_item(char_id, slot);
        set_argument_memory((uint32_t)item_id);
        uint16_t result = remove_item_from_inventory(char_id, slot);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x10: {
        /* CHECK_ITEM_EQUIPPED: 2 data bytes (char_id, item_slot).
         * Port of CC_1D_10 (asm/text/ccs/unknown_1D_10.asm) →
         * CHECK_ITEM_EQUIPPED (asm/misc/check_item_equipped.asm).
         * Byte 1 = char_id (0 → working_memory).
         * Byte 2 = item_slot (0 → argument_memory). 1-based inventory position.
         * Checks if the given inventory slot is currently equipped in any
         * equipment slot (weapon, body, arms, other).
         * Returns 1 if slot is equipped, 0 if not.
         * Stores result (sign-extended) in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        int16_t result = (int16_t)check_item_equipped(char_id, item_slot);
        set_working_memory((uint32_t)(int32_t)result);
        break;
    }
    case 0x11: {
        /* CHECK_ITEM_USABLE_BY_SLOT: 2 args (char_id, item_slot).
         * Port of CC_1D_11 (asm/text/ccs/unknown_1D_11.asm).
         * Arg 1 = char_id (0 → working_memory), Arg 2 = item_slot (0 → argument_memory).
         * Gets item at slot via GET_CHARACTER_ITEM, then checks CHECK_ITEM_USABLE_BY.
         * Stores result in working_memory. */
        uint8_t char_arg = script_read_byte(r);
        uint8_t slot_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t slot = slot_arg ? (uint16_t)slot_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint8_t item_id = 0;
        if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && slot >= 1 && slot <= 14) {
            item_id = party_characters[char_id - 1].items[slot - 1];
        }
        uint16_t result = check_item_usable_by(char_id, item_id);
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x12: {
        /* ESCARGO_EXPRESS_MOVE: 2 args (char_id, item_slot).
         * Port of CC_1D_12 (asm/text/ccs/unknown_1D_12.asm).
         * arg1==0 → working_memory, arg2==0 → argument_memory.
         * Moves item from character's inventory to escargo express. */
        uint8_t arg1 = script_read_byte(r);
        uint8_t arg2 = script_read_byte(r);
        uint16_t char_id = arg1 ? (uint16_t)arg1 : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t item_slot = arg2 ? (uint16_t)arg2 : (uint16_t)(get_argument_memory() & 0xFFFF);
        escargo_express_move(char_id, item_slot);
        break;
    }
    case 0x13: {
        /* DELIVER_ESCARGO_EXPRESS_ITEM: 2 args (char_id, escargo_slot).
         * Port of CC_1D_13 (asm/text/ccs/unknown_1D_13.asm).
         * arg1==0 → working_memory, arg2==0 → argument_memory.
         * Removes item from escargo and gives to character.
         * Sets argument_memory = find_empty_inventory_slot(char_id),
         * working_memory = char_id. */
        uint8_t arg1 = script_read_byte(r);
        uint8_t arg2 = script_read_byte(r);
        uint16_t char_id = arg1 ? (uint16_t)arg1 : (uint16_t)(get_working_memory() & 0xFFFF);
        uint16_t escargo_slot = arg2 ? (uint16_t)arg2 : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result_char = deliver_escargo_express_item(char_id, escargo_slot);
        uint16_t empty_slot = find_empty_inventory_slot(result_char);
        set_argument_memory((uint32_t)empty_slot);
        set_working_memory((uint32_t)result_char);
        break;
    }
    case 0x14: {
        /* HAVE_ENOUGH_MONEY: 4 data bytes (.DWORD).
         * Port of CC_1D_14 (asm/text/ccs/test_has_enough_money.asm).
         * Builds 32-bit required amount. If zero, use argument_memory.
         * Compares against game_state.money_carried.
         * Sets working_memory: 1 = insufficient, 0 = sufficient. */
        uint32_t amount = script_read_dword(r);
        if (amount == 0) amount = (uint32_t)get_argument_memory();
        uint32_t result = (game_state.money_carried >= amount) ? 0 : 1;
        set_working_memory(result);
        break;
    }
    case 0x15: {
        /* PUT_VAL_IN_ARGMEM: 2 data bytes (.WORD).
         * Port of CC_1D_15 (asm/text/ccs/set_argmem.asm).
         * Builds 16-bit value. If zero, use argument_memory.
         * Calls FIND_FIRST_UNCONSCIOUS_PARTY_SLOT (C226F0):
         *   Iterates party_order[0..party_count-1], finds first member
         *   whose afflictions == 1 (unconscious). Returns the index (0-based).
         *   If no unconscious member found, returns party_count.
         * Multiplies value * unconscious_slot, stores in working_memory. */
        uint16_t amount = script_read_word(r);
        uint32_t val = amount ? (uint32_t)amount : (uint32_t)get_argument_memory();
        /* FIND_FIRST_UNCONSCIOUS_PARTY_SLOT */
        uint16_t slot = find_first_unconscious_party_slot();
        set_working_memory(val * (uint32_t)slot);
        break;
    }
    case 0x17: {
        /* HAVE_ENOUGH_MONEY_IN_ATM: 4 data bytes (.DWORD).
         * Port of CC_1D_17 (asm/text/ccs/test_atm_has_enough_money.asm).
         * Same pattern as 0x14 but checks game_state.bank_balance. */
        uint32_t amount = script_read_dword(r);
        if (amount == 0) amount = (uint32_t)get_argument_memory();
        uint32_t result = (game_state.bank_balance >= amount) ? 0 : 1;
        set_working_memory(result);
        break;
    }
    case 0x18: {
        /* ESCARGO_EXPRESS_STORE: 1 arg (item_id).
         * Port of CC_1D_18 (asm/text/ccs/escargo_express_store.asm) →
         * ESCARGO_EXPRESS_STORE (asm/misc/escargo_express_store.asm).
         * arg==0 → use argument_memory.
         * Finds first empty slot in escargo_express_items[] and stores item. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        if (item_id != 0) {
            escargo_express_store(item_id);
        }
        break;
    }
    case 0x19: {
        /* HAVE_X_PARTY_MEMBERS: 1 arg.
         * Port of CC_1D_19 (asm/text/ccs/test_party_enough_characters.asm).
         * If arg != 0, use arg; else use argument_memory.
         * Sets working_memory to 1 if party_count < N, 0 otherwise. */
        uint8_t arg = script_read_byte(r);
        uint32_t n = arg ? (uint32_t)arg : get_argument_memory();
        uint32_t result = (game_state.player_controlled_party_count < n) ? 1 : 0;
        set_working_memory(result);
        break;
    }
    case 0x20: {
        /* TEST_IS_USER_TARGETTING_SELF: 0 args.
         * Port of tree_1D.asm @TEST_SELF_TARGET.
         * Compares dt.battle_attacker_name and dt.battle_target_name.
         * If equal (same entity targeting itself), stores 1 in working_memory.
         * Otherwise stores 0. */
        char *target = return_battle_target_address();
        char *attacker = return_battle_attacker_address();
        uint32_t result = (strcmp(attacker, target) == 0) ? 1 : 0;
        set_working_memory(result);
        break;
    }
    case 0x21: {
        /* GENERATE_RANDOM_NUMBER: 1 arg.
         * Port of CC_1D_21 (asm/text/ccs/get_random_number.asm).
         * If arg != 0, use arg; else use argument_memory.
         * Calls RAND_MOD(N) → rand() % (N + 1), stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t n = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t result = (uint16_t)((rand() % 256) % (n + 1));
        set_working_memory((uint32_t)result);
        break;
    }
    case 0x22: {
        /* TEST_IF_EXIT_MOUSE_USABLE: 0 args.
         * Inline handler from tree_1D.asm @TEST_EXIT_MOUSE_USABLE.
         * Checks if the leader is standing on a sector with attribute type 2
         * (exit mouse area). Stores 1 in working_memory if yes, 0 if no. */
        uint16_t x = game_state.leader_x_coord;
        uint16_t y = game_state.leader_y_coord;
        uint16_t sector_attrs = load_sector_attrs(x, y);
        uint32_t result = ((sector_attrs & 0x0007) == 2) ? 1 : 0;
        set_working_memory(result);
        break;
    }
    case 0x23: {
        /* GET_ITEM_CATEGORY: 1 arg (item_id, 0 → argument_memory).
         * Port of CC_1D_23 (asm/text/ccs/unknown_1D_23.asm).
         * Reads item.type, masks with 0x0C, categorizes:
         *   0x00 (none) → 1, 0x04/0x08/0x0C (has bits) → 2, other → 0.
         * Stores result in working_memory. */
        uint8_t arg = script_read_byte(r);
        uint16_t item_id = arg ? (uint16_t)arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        const ItemConfig *entry = get_item_entry(item_id);
        uint32_t result = 0;
        if (entry) {
            uint8_t type_masked = entry->type & 0x0C;
            if (type_masked == 0) {
                result = 1;
            } else if (type_masked == 0x04 || type_masked == 0x08 || type_masked == 0x0C) {
                result = 2;
            }
        }
        set_working_memory(result);
        break;
    }
    case 0x24: {
        /* GET_GAME_STATE_C4: 1 arg.
         * Port of CC_1D_24 (asm/text/ccs/unknown_1D_24.asm).
         * Reads game_state.unknownC4 as 32-bit into working_memory.
         * If arg == 2, clears unknownC4 to zero. */
        uint8_t arg = script_read_byte(r);
        uint32_t val;
        memcpy(&val, game_state.unknownC4, 4);
        set_working_memory(val);
        if (arg == 2) {
            memset(game_state.unknownC4, 0, 4);
        }
        break;
    }
    default:
        FATAL("display_text: unknown CC 1D %02X\n", sub);
        break;
    }
}


/* --- CC 0x1E tree: HP/PP/stats modification ---
 * Byte counts verified against include/textmacros.asm.
 * Port of asm/text/ccs/recover_hp_by_percent.asm, deplete_hp_by_percent.asm, etc.
 *
 * HP/PP handlers (0x00-0x07): 2 data bytes.
 *   Byte 1 = char_id (0 → argument_memory; 0xFF = all party members).
 *   Byte 2 = value (percent or fixed amount).
 *   Calls RECOVER/REDUCE_HP/PP_AMTPERCENT(A=char_id, X=value, Y=mode).
 *
 * Stat boost handlers (0x0A-0x0E): 1 byte + 1 word.
 *   Byte 1 = char_id (1-indexed).
 *   Word = boost amount (only low byte used, 8-bit addition).
 *   Adds to party_characters[char_id-1].boosted_*, then recalculates stat. */
void cc_1e_dispatch(ScriptReader *r) {
    uint8_t sub = script_read_byte(r);

    switch (sub) {
    /* --- HP recovery/depletion --- */
    case 0x00:   /* RECOVER_HP_PERCENT */
    case 0x01:   /* DEPLETE_HP_PERCENT */
    case 0x02:   /* RECOVER_HP_AMOUNT */
    case 0x03: { /* DEPLETE_HP_AMOUNT */
        uint8_t char_arg = script_read_byte(r);
        uint8_t value = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t mode = (sub >= 0x02) ? 1 : 0; /* 0=percent, 1=absolute */
        if (sub == 0x00 || sub == 0x02)
            recover_hp_amtpercent(char_id, (uint16_t)value, mode);
        else
            reduce_hp_amtpercent(char_id, (uint16_t)value, mode);
        break;
    }
    /* --- PP recovery/depletion --- */
    case 0x04:   /* RECOVER_PP_PERCENT */
    case 0x05:   /* DEPLETE_PP_PERCENT */
    case 0x06:   /* RECOVER_PP_AMOUNT */
    case 0x07: { /* DEPLETE_PP_AMOUNT */
        uint8_t char_arg = script_read_byte(r);
        uint8_t value = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        uint16_t mode = (sub >= 0x06) ? 1 : 0;
        if (sub == 0x04 || sub == 0x06)
            recover_pp_amtpercent(char_id, (uint16_t)value, mode);
        else
            reduce_pp_amtpercent(char_id, (uint16_t)value, mode);
        break;
    }
    case 0x08: {
        /* SET_CHARACTER_LEVEL: 2 data bytes.
         * Port of CC_1E_08 (asm/text/ccs/set_character_level.asm).
         * Byte 1 = char_id (0 → working_memory). Byte 2 = level (0 → argument_memory).
         * Calls RESET_CHAR_LEVEL_ONE(A=char_id, X=level, Y=1). */
        uint8_t char_arg = script_read_byte(r);
        uint8_t level_arg = script_read_byte(r);
        uint16_t char_id = char_arg ? (uint16_t)char_arg : (uint16_t)(get_working_memory() & 0xFF);
        uint16_t level = level_arg ? (uint16_t)level_arg : (uint16_t)(get_argument_memory() & 0xFFFF);
        reset_char_level_one(char_id, level, 1);
        break;
    }
    case 0x09: {
        /* GIVE_EXPERIENCE: 5 data bytes (LDA #4 counter check).
         * Port of CC_1E_09 (asm/text/ccs/increase_character_experience.asm).
         * Byte 1 = char_id. Bytes 2-5 = 32-bit experience amount
         * (only low 24 bits matter for exp storage).
         * Calls GAIN_EXP(A=char_id, X=1). play_sound=1 → plays level-up music. */
        uint8_t char_id = script_read_byte(r);
        uint8_t exp_b0 = script_read_byte(r);
        uint8_t exp_b1 = script_read_byte(r);
        uint8_t exp_b2 = script_read_byte(r);
        uint8_t exp_b3 = script_read_byte(r);
        uint32_t exp = (uint32_t)exp_b0 | ((uint32_t)exp_b1 << 8) |
                       ((uint32_t)exp_b2 << 16) | ((uint32_t)exp_b3 << 24);
        gain_exp(1, (uint16_t)char_id, exp);
        break;
    }
    /* --- Stat boosts: 1 byte char_id + 1 byte amount --- */
    case 0x0A:   /* BOOST_IQ */
    case 0x0B:   /* BOOST_GUTS */
    case 0x0C:   /* BOOST_SPEED */
    case 0x0D:   /* BOOST_VITALITY */
    case 0x0E: { /* BOOST_LUCK */
        /* Port of CC_1E_0A..0E (asm/text/ccs/increase_character_*.asm).
         * Assembly: gathers 2 data bytes (LDA #1 counter check).
         * Byte 1 = char_id. Byte 2 = boost amount.
         * Adds boost to the boosted_* field (8-bit add),
         * then calls RECALC_CHARACTER_POSTMATH_* to recalculate effective stat. */
        uint8_t char_id = script_read_byte(r);
        uint8_t boost = script_read_byte(r);
        if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT) {
            CharStruct *c = &party_characters[char_id - 1];
            switch (sub) {
            case 0x0A: c->boosted_iq += boost; recalc_character_postmath_iq(char_id); break;
            case 0x0B: c->boosted_guts += boost; recalc_character_postmath_guts(char_id); break;
            case 0x0C: c->boosted_speed += boost; recalc_character_postmath_speed(char_id); break;
            case 0x0D: c->boosted_vitality += boost; recalc_character_postmath_vitality(char_id); break;
            case 0x0E: c->boosted_luck += boost; recalc_character_postmath_luck(char_id); break;
            }
        }
        break;
    }
    default:
        FATAL("display_text: unknown CC 1E %02X\n", sub);
        break;
    }
}

