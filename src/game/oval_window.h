#ifndef GAME_OVAL_WINDOW_H
#define GAME_OVAL_WINDOW_H

#include "core/types.h"

/* PSI animation state (matches psi_animation_state struct from structs.asm).
 * Used by the PSI battle animation system (C2E6B3 update, show_psi_animation init).
 *
 * Arrangement frames use a bundled format: each .arr.bundled asset contains
 * 8-frame bundles compressed independently. Only one bundle (8 KB decompressed)
 * is resident at a time, eliminating the 64 KB buffer requirement. */
typedef struct {
    uint8_t  time_until_next_frame;             /* 0 */
    uint8_t  frame_hold_frames;                 /* 1 */
    uint8_t  total_frames;                      /* 2 */
    uint32_t frame_data;                        /* 3: current frame index (0-based) */
    uint8_t  palette_animation_lower_index;     /* 7 */
    uint8_t  palette_animation_upper_index;     /* 8 */
    uint8_t  palette_animation_current_index;   /* 9 */
    uint8_t  palette_animation_frames;          /* 10 */
    uint8_t  palette_animation_time_until_next_frame; /* 11 */
    uint16_t palette[16];                       /* 12: working palette (32 bytes) */
    uint16_t displayed_palette;                 /* 44 */
    uint16_t enemy_colour_change_start_frames_left; /* 46 */
    uint16_t enemy_colour_change_frames_left;   /* 48 */
    uint16_t enemy_colour_change_red;           /* 50 */
    uint16_t enemy_colour_change_green;         /* 52 */
    uint16_t enemy_colour_change_blue;          /* 54 */
    /* C port addition: bundled arrangement streaming state */
    const uint8_t *arr_bundled_data;  /* pointer to .arr.bundled asset */
    uint32_t arr_bundled_size;        /* total asset size (fixed-width for savestate ABI) */
    int16_t arr_current_bundle;       /* currently decompressed bundle index (-1 = none) */
    uint16_t arr_current_anim_id;     /* anim id backing arr_bundled_data — serializable form
                                         of the asset ptr, used to rebind it after a state load
                                         (savestate pointer purge, build item #3) */
    uint8_t *arr_bundle_buf;          /* 8 KB staging buffer (points to ert.buffer during PSI) */
} PsiAnimationState;

extern PsiAnimationState psi_animation_state;

/* Rebuild psi_animation_state's runtime pointers after a savestate load (defined in
 * battle_psi.c). Declared here because the struct lives in this header. */
void psi_animation_savestate_rebind(void);

/* Oval window animation frame data (matches oval_window struct from structs.asm) */
typedef struct {
    uint8_t duration;
    uint8_t unused;
    int16_t centre_x, centre_y;
    int16_t initial_width, initial_height;
    int16_t centre_x_add, centre_y_add;
    int16_t width_velocity, height_velocity;
    int16_t width_acceleration, height_acceleration;
} OvalWindowData;

/* Initialize oval window effect (port of INIT_OVAL_WINDOW, C2EA15)
 * type: 0 = standard, 1 = variant (longer), 2 = battle variant */
void init_oval_window(uint16_t type);

/* Initialize swirl effect (port of INIT_SWIRL_EFFECT, C4A67E)
 * type: animation_id (index into swirl primary table).
 * options: flags (bit 0=reverse, bit 1=invert, bit 2=mask, bit 7=repeat). */
void init_swirl_effect(uint16_t type, uint16_t options);

/* Start battle swirl (port of START_BATTLE_SWIRL, C2E8C4)
 * Calls init_swirl_effect then sets swirl_length_padding. */
void start_battle_swirl(uint16_t type, uint16_t options, uint16_t padding);

/* Update oval window each frame (port of UPDATE_SWIRL_EFFECT, C4A7B0) */
void update_swirl_effect(void);

/* Begin closing the oval window (port of CLOSE_OVAL_WINDOW, C2EA74) */
void close_oval_window(void);

/* Stop oval window immediately (port of STOP_OVAL_WINDOW, C2EAAA) */
void stop_oval_window(void);

/* Stop battle swirl effect (port of STOP_BATTLE_SWIRL, C2E9ED) */
void stop_battle_swirl(void);

/* Check if oval/PSI animation is still active (port of IS_PSI_ANIMATION_ACTIVE, C2EACF) */
bool is_psi_animation_active(void);

/* Check if battle swirl is still playing (port of IS_BATTLE_SWIRL_ACTIVE, C2E9C8) */
bool is_battle_swirl_active(void);

/* Reset the swirl update countdown timer to 0 (used by clear_battle_visual_effects) */
void reset_swirl_update_timer(void);

/* Set PPU window mask registers (port of SET_WINDOW_MASK) */
void set_window_mask(uint16_t config, uint16_t invert);

/* Disable PPU windows (port of DISABLE_WINDOWS, C0B0AA) */
void disable_windows(void);

/* Set swirl mask settings (needed by BATTLE_SWIRL_SEQUENCE) */
void set_swirl_mask_settings(uint8_t value);

/* Set swirl auto-restore flag (needed by BATTLE_SWIRL_SEQUENCE) */
void set_swirl_auto_restore(uint8_t value);

/* ---- Savestate snapshot ----
 * The swirl / oval-window animation runs across many frames driven by file-static
 * state in oval_window.c; a savestate taken mid-animation must round-trip it. One
 * tagged section, gathered/scattered by the pack/unpack pair. (loaded_oval_window is
 * a raw cursor pointer — valid in-process; cross-platform purge is build item #3.) */
typedef struct {
    uint8_t  frames_until_next_swirl_update;
    uint8_t  frames_until_next_swirl_frame;
    uint8_t  swirl_frames_left;
    uint8_t  swirl_hdma_table_id;
    uint8_t  swirl_invert_enabled;
    uint8_t  swirl_reversed;
    uint8_t  swirl_mask_settings;
    uint8_t  swirl_hdma_channel_offset;
    uint8_t  swirl_length_padding;
    uint8_t  swirl_auto_restore;
    uint8_t  swirl_next_swirl;
    uint8_t  swirl_repeat_speed;
    uint8_t  swirl_repeats_until_speed_up;
    uint8_t  active_oval_window;
    int16_t  loaded_oval_window_centre_x;
    int16_t  loaded_oval_window_centre_y;
    uint16_t loaded_oval_window_width;
    uint16_t loaded_oval_window_height;
    int16_t  loaded_oval_window_centre_x_add;
    int16_t  loaded_oval_window_centre_y_add;
    int16_t  loaded_oval_window_width_velocity;
    int16_t  loaded_oval_window_height_velocity;
    int16_t  loaded_oval_window_width_acceleration;
    int16_t  loaded_oval_window_height_acceleration;
    /* loaded_oval_window is a raw cursor into one of the static animation-frame
     * tables; serialize it as (sequence id, frame index) so the snapshot is
     * cross-process/cross-platform safe (build item #3). seq 0 = NULL (inactive). */
    uint8_t  loaded_oval_window_seq;
    uint16_t loaded_oval_window_index;
} OvalWindowSaveState;
void oval_window_savestate_pack(void *out);   /* out: OvalWindowSaveState* */
void oval_window_savestate_unpack(const void *in);

#endif /* GAME_OVAL_WINDOW_H */
