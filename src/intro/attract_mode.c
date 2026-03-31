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
    fprintf(stderr, "attract: failed to load data/attract_mode_txt.bin\n");
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

uint16_t run_attract_mode(uint16_t scene_index) {
  uint16_t button_pressed = 0;

  /* Clamp scene index to valid range */
  if (scene_index >= 10)
    scene_index = 9;
  current_scene_index = scene_index;

  /* Lazy-load EEVENT0 bytecode, pointer table, and map data tables.
   * These are loaded once and cached for subsequent scenes. */
  display_text_load_eevent0();
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

  /* Step 14: Run DISPLAY_TEXT with attract mode scene script.
   * This drives the entire scene: sets event flags, adds party members,
   * teleports to the scene location (loading map data), spawns entities
   * with movement scripts, and pauses for the scene duration.
   * DISPLAY_TEXT blocks until the script hits END_BLOCK. */
  {
    display_text_from_addr(attract_mode_text_addrs[scene_index]);
  }

  /* The assembly does NOT clear ow.camera_focus_entity here — the camera
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
  OBJECT_TICK_DISABLED
   * flag in the high byte.
   *
  /* Main loop — faithful port of @UNKNOWN2..@UNKNOWN7 in C4D989.asm.
   * After DISPLAY_TEXT returns, entity scripts continue running via
   * render_frame_tick → run_actionscript_frame. Wait until scripts
   * call SET_ACTIONSCRIPT_STATE_RUNNING or the user presses a button.
   *
   * Assembly loop order:
   *   @UNKNOWN7: check ACTIONSCRIPT_STATE → if 0, continue
   *   @UNKNOWN2: UPDATE_SWIRL_EFFECT, check PAD_PRESS for A/B/Start
   *   @UNKNOWN4: RENDER_FRAME_TICK
   *   @UNKNOWN5: if frame<=1, TM_MIRROR = $13
   *   @UNKNOWN6: increment frame counter → BRA @UNKNOWN7
   */
  {
    int loop_frame = 0;
    while (ert.actionscript_state == 0) {
      /* @UNKNOWN2: Update oval window animation */
      update_swirl_effect();

      /* Check for button press (A, B, or Start) */
      uint16_t pressed = platform_input_get_pad_new();
      if (pressed & PAD_ANY_BUTTON) {
        button_pressed = 1;
        break;
      }

      /* @UNKNOWN4: Render frame — entity scripts execute here via
       * run_actionscript_frame(). Scripts like EVENT_535 call
       * DECOMP_ITOI_PRODUCTION and WRITE_BYTE_WRAM to set up BG3. */
      render_frame_tick();

      /* Process brightness fades (ACTIONSCRIPT_FADE_OUT/IN).
       * Assembly: the NMI handler applies FADE_PARAMETERS per frame.
       * C port: fade_update() must be called explicitly each frame. */
      fade_update();

      /* @UNKNOWN5-6: Enable BG1+BG2+OBJ on the first two frames.
       * This comes AFTER render_frame_tick (matching assembly order).
       * Entity scripts may set TM to $17 (enabling BG3) during
       * render_frame_tick, but on frames 0-1 we override to $13.
       * After frame 1, TM stays at whatever the entity script set. */
      if (loop_frame <= 1) {
        ppu.tm = 0x13; /* BG1 | BG2 | OBJ */
      }

      loop_frame++;

      /* Safety timeout — assembly has no timeout; scenes end when the
       * entity script calls SET_ACTIONSCRIPT_STATE_RUNNING. */
      if (loop_frame >= 36000)
        break;

      if (platform_input_quit_requested())
        break;
    }
  }

  /* Close the oval window */
  close_oval_window();

  /* Wait for oval close animation to finish */
  while (is_psi_animation_active()) {
    render_frame_tick();
    update_swirl_effect();
    if (platform_input_quit_requested())
      break;
  }

  /* Fade out */
  fade_out(1, 1); /* Assembly: A=1 (step), X=1 (delay) */
  while (fade_active()) {
    fade_update();
    render_frame_tick();
    if (platform_input_quit_requested())
      break;
  }

  /* Stop oval window system */
  stop_oval_window();
  ert.actionscript_state = 0;
  clear_map_entities();

  return button_pressed;
}
