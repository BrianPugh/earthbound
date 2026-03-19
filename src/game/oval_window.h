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
    size_t arr_bundled_size;          /* total asset size */
    int16_t arr_current_bundle;       /* currently decompressed bundle index (-1 = none) */
    uint8_t *arr_bundle_buf;          /* 8 KB staging buffer (points to ert.buffer during PSI) */
} PsiAnimationState;

extern PsiAnimationState psi_animation_state;

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

#endif /* GAME_OVAL_WINDOW_H */
