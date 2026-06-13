/*
 * Attract mode — demo scenes shown when the player idles on the title screen.
 *
 * Port of RUN_ATTRACT_MODE (asm/misc/run_attract_mode.asm).
 *
 * Each scene shows Ness walking around an overworld area with a circular
 * spotlight (oval window). The scene sequence is driven by DISPLAY_TEXT
 * running the attract mode bytecode scripts from EEVENT0.
 */

#include "intro/attract_mode.h"
#include "data/assets.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "game/battle_bg.h"
#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "game/fade.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "game/oval_window.h"
#include "game/overworld.h"
#include "include/binary.h"
#include "include/constants.h"
#include "include/pad.h"
#include "platform/platform.h"
#include "snes/ppu.h"
#include "core/log.h"
#include "core/mode_stack.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations */
#include "game_main.h"

static uint16_t current_scene_index;

#define ATTRACT_MODE_SCENE_COUNT 10

/* Attract mode pointer table — loaded from ROM asset.
 * Assembly (C4D989.asm lines 64-73) uses ATTRACT_MODE_TXT, a table of
 * 10 × 4-byte far pointers to MSG_MD_* scripts within EEVENT0.
 * We load the table at runtime and derive offsets by subtracting the
 * base address (first entry, MSG_MD_TOTO at offset 0). */
static uint32_t attract_mode_text_addrs[ATTRACT_MODE_SCENE_COUNT];
static bool attract_mode_offsets_loaded = false;

static bool load_attract_mode_text_offsets(void) {
  if (attract_mode_offsets_loaded)
    return true;

  size_t size = ASSET_SIZE(ASSET_DATA_ATTRACT_MODE_TXT_BIN);
  const uint8_t *data = ASSET_DATA(ASSET_DATA_ATTRACT_MODE_TXT_BIN);
  if (!data || size < ATTRACT_MODE_SCENE_COUNT * 4) {
    LOG_WARN("attract: failed to load data/attract_mode_txt.bin\n");
    return false;
  }

  /* Each entry is a 32-bit little-endian SNES far address.
   * Store as-is — resolve_text_addr() handles SNES→blob remapping. */
  for (int i = 0; i < ATTRACT_MODE_SCENE_COUNT; i++) {
    attract_mode_text_addrs[i] = read_u32_le(&data[i * 4]);
  }
  attract_mode_offsets_loaded = true;
  return true;
}

/* ---- GAME_MODE_ATTRACT ----------------------------------------------------
 * Run-to-completion port of the three blocking loops at the tail of
 * run_attract_mode(). See the AttractState comment in mode_stack.h. The single
 * yield is owned by the pump; this body never calls wait_for_vblank() (per-frame
 * work uses render_frame_tick_work()). */
StepResult mode_step_attract_mode(ModeState *st) {
  AttractState *s = &st->attract;

  switch ((AttractPhase)s->phase) {
  case AT_SCRIPT: {
    /* Drive the scene by running its attract-mode bytecode script as a child
     * DISPLAY_TEXT (replaces the former blocking display_text_from_addr()). The
     * script sets event flags, adds party members, teleports to the scene
     * location, spawns entities with movement scripts, and pauses for the scene
     * duration. On its POP we resume at AT_MAIN to run the post-script frames. */
    s->phase = AT_MAIN;
    static ModeState dt_init; /* must outlive this dispatch (pump copies it) */
    if (dt_make_child_init(&dt_init, attract_mode_text_addrs[s->scene_index])) {
      return STEP_RESULT_PUSH_INIT(GAME_MODE_DISPLAY_TEXT, &dt_init);
    }
    /* Unresolvable address: warn-and-continue, matching the blocking path. */
    LOG_WARN("attract: resolve_text_addr(0x%06X) returned NULL\n",
             attract_mode_text_addrs[s->scene_index]);
    return STEP_RESULT_CONTINUE();
  }

  case AT_MAIN: {
    /* while(actionscript_state == 0): swirl, button check, render, fade, TM. */
    bool done = false;
    if (ert.actionscript_state != 0) {
      done = true;
    } else {
      update_swirl_effect();
      if (platform_input_get_pad_new() & PAD_ANY_BUTTON) {
        s->button_pressed = 1;
        done = true;
      } else {
        render_frame_tick_work();
        fade_update();
        if (s->loop_frame <= 1)
          ppu.tm = 0x13; /* BG1 | BG2 | OBJ on the first two frames */
        s->loop_frame++;
        if (s->loop_frame >= 36000) /* safety timeout (no timeout in the ROM) */
          done = true;
      }
    }
    if (done) {
      close_oval_window();
      s->phase = AT_OVAL_CLOSE;
    }
    return STEP_RESULT_CONTINUE();
  }

  case AT_OVAL_CLOSE:
    /* Wait for the oval-close animation to finish. */
    if (!is_psi_animation_active()) {
      fade_out(1, 1); /* A=1 (step), X=1 (delay) */
      s->phase = AT_FADEOUT;
      return STEP_RESULT_CONTINUE();
    }
    render_frame_tick_work();
    update_swirl_effect();
    return STEP_RESULT_CONTINUE();

  case AT_FADEOUT:
    if (!fade_active()) {
      stop_oval_window();
      ert.actionscript_state = 0;
      clear_map_entities();
      return STEP_RESULT_POP(s->button_pressed);
    }
    fade_update();
    render_frame_tick_work();
    return STEP_RESULT_CONTINUE();
  }
  return STEP_RESULT_POP(s->button_pressed);
}

void run_attract_mode_prepare(uint16_t scene_index) {
  /* Clamp scene index to valid range */
  if (scene_index >= 10)
    scene_index = 9;
  current_scene_index = scene_index;

  /* Lazy-load EEVENT0 bytecode, pointer table, and map data tables.
   * These are loaded once and cached for subsequent scenes. */
  display_text_init();
  load_attract_mode_text_offsets();
  map_loader_init();
  if (!sprite_grouping_ptr_table)
    load_sprite_data();

  /* Step 1: Initialize entity system and clear sprites */
  entity_system_init();
  clear_overworld_spritemaps();
  ow.camera_focus_entity = -1;

  /* Step 2: ALLOC_SPRITE_MEM(X=0, A=$8000) — clear sprite VRAM table */
  alloc_sprite_mem(0x8000, 0);

  /* Step 3: Initialize entity data */
  initialize_misc_object_data();
  ow.npc_spawns_enabled = 1;
  ow.enemy_spawns_enabled = 0;

  ow.enable_auto_sector_music_changes = 0;

  /* Step 4: Restrict entity allocation to slots 23-24 */
  entities.alloc_min_slot = INIT_ENTITY_SLOT;
  entities.alloc_max_slot = PARTY_LEADER_ENTITY_INDEX;

  /* Step 5: Init entity with EVENT_001 (main overworld tick), X=0, Y=0 */
  entity_init(EVENT_SCRIPT_001, 0, 0);

  /* Step 6: Reset party state and clear party_members array */
  reset_party_state();
  for (int i = 0; i < 6; i++) {
    game_state.party_members[i] = 0;
  }

  /* Step 7: Place leader at default position and initialize party.
   * Assembly: LDX #2824 / LDA #7520 / JSL PLACE_LEADER_AT_POSITION
   * This sets an initial position; DISPLAY_TEXT's TELEPORT_TO CC
   * will reposition to the scene-specific location. */
  place_leader_at_position(7520, 2824);
  initialize_party();

  /* Step 8: Clear all ert.palettes (both mirror and CGRAM) */
  memset(ert.palettes, 0, sizeof(ert.palettes));
  memset(ppu.cgram, 0, sizeof(ppu.cgram));

  /* Step 9: Initialize overworld VRAM settings */
  overworld_initialize();

  /* Clear battle BG distortion left over from gas station intro.
   * Without this, the PPU renderer applies per-scanline horizontal offsets
   * to BG2, causing horizontal stripe artifacts in the overworld. */
  bg2_distortion_active = false;

  /* Clear color math registers — may have residual values from title screen
   * or gas station intro that would tint the backdrop (e.g. purple). */
  ppu.cgwsel = 0;
  ppu.cgadsub = 0;
  ppu.coldata_r = 0;
  ppu.coldata_g = 0;
  ppu.coldata_b = 0;

  /* Step 10: Clear TM (no layers visible initially) and enable screen.
   * The ROM's NMI handler writes INIDISP from a mirror; in the C port
   * we set it directly. After the title screen fadeout (INIDISP=$80),
   * we need to turn the screen back on at full brightness. */
  ppu.tm = 0;
  ppu.inidisp = 0x0F;

  /* Step 11: Initialize oval window (type 0 = standard open) */
  init_oval_window(0);

  /* Step 12: Run one update to start the animation */
  update_swirl_effect();

  /* Step 13: Clear actionscript state */
  ert.actionscript_state = 0;

  /* Step 14 (the scene-driving DISPLAY_TEXT) now runs as GAME_MODE_ATTRACT's
   * AT_SCRIPT phase (see mode_step_attract_mode), pushed by the init_intro parent
   * after this one-shot setup returns. The script drives the entire scene: sets
   * event flags, adds party members, teleports to the scene location (loading map
   * data), spawns entities with movement scripts, and pauses for the duration.
   *
   * The assembly does NOT clear ow.camera_focus_entity here — the camera
   * naturally follows the focus entity (sprite 106, invisible pathfinder)
   * during the entire scene via render_frame_tick()'s scroll update.
   * This is what makes the view pan across the map through the oval window.
   *
   * Entity position tracking:
   * In non-bicycle scenes, entity 24 (leader) has UPDATE_FOLLOWER_STATE as
   * its tick callback. Each frame, UPDATE_FOLLOWER_STATE reads from the
   * position ert.buffer (written by update_leader_movement →
  sync_camera_to_entity)
   * and updates entity 24's abs_x/abs_y. This keeps entity 24 centered in
   * the oval window as the camera pans.
   *
   * In bicycle scene (scene 5), GET_ON_BICYCLE sets OBJECT_TICK_DISABLED
   * (bit 15 of tick_callback_hi) on entity 24. This prevents
   * UPDATE_FOLLOWER_STATE from running, so entity 24's abs position stays
   * fixed at the teleport destination while the camera follows the pathfinder.
   * The bicycle sprite starts centered but drifts off-screen as the
   * pathfinder moves (~2px/frame). No per-frame position sync mechanism
   * was found in the assembly for the bicycle case — the drift appears to
   * be the original ROM behavior. Opcode 0x08 (SET_TICK_CALLBACK in
   * EVENT_002 / party follower) uses an 8-bit STA that preserves the
   * OBJECT_TICK_DISABLED flag in the high byte. */

  /* The scene-driving DISPLAY_TEXT (AT_SCRIPT), the main scene loop, the
   * oval-close wait, and the fade-out + cleanup all run as GAME_MODE_ATTRACT
   * (see mode_step_attract_mode above), pushed by the init_intro parent after
   * this setup returns. */
}
