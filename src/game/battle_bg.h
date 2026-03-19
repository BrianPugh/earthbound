#ifndef GAME_BATTLE_BG_H
#define GAME_BATTLE_BG_H

#include "core/types.h"

/* Battle background layer IDs */
#define BATTLEBG_FILE_SELECT   230
#define BATTLEBG_GAS_STATION   295

/* Maximum distortion table entries per scanline */
#define BATTLEBG_MAX_SCANLINES VIEWPORT_HEIGHT

/* Per-layer runtime animation state */
typedef struct {
    /* Scroll state (16.16 fixed point) */
    int32_t scroll_x;
    int32_t scroll_y;
    int32_t scroll_dx;     /* X velocity */
    int32_t scroll_dy;     /* Y velocity */
    int32_t scroll_ddx;    /* X acceleration */
    int32_t scroll_ddy;    /* Y acceleration */

    /* Distortion state (mirrors loaded_bg_data distortion fields)
     * See PREPARE_BG_OFFSET_TABLES in prepare_bg_offset_tables.asm
     * and GENERATE_BATTLEBG_FRAME in generate_frame.asm */
    uint16_t dist_type;        /* 0=none, 1=HORIZONTAL_SMOOTH, 2=HORIZONTAL_INTERLACED, 3=VERTICAL */
    int16_t  dist_amplitude;   /* ripple amplitude (high byte used as M7A multiplier) */
    uint8_t  dist_speed;       /* current phase index (8-bit, wrapping) */
    int16_t  dist_freq;        /* ripple frequency (phase advance per scanline) */
    int16_t  dist_compression; /* compression rate per scanline */
    /* Per-frame acceleration (added to parameters each frame) */
    int16_t  dist_freq_accel;
    int16_t  dist_amp_accel;
    int8_t   dist_speed_accel;
    int16_t  dist_comp_accel;

    /* Palette cycling */
    uint16_t pal_cycle_type;   /* 0=none, 1=step, 2=wrap, 3=bidirectional */
    uint16_t pal_cycle_start;  /* first palette entry */
    uint16_t pal_cycle_count;  /* number of entries in cycle */
    uint16_t pal_cycle_speed;  /* frames between cycles */
    uint16_t pal_cycle_counter; /* current frame counter */
    int16_t  pal_cycle_dir;    /* direction for bidirectional */

    /* Animation timing */
    uint16_t anim_duration;   /* frames per animation step */
    uint16_t anim_counter;    /* current frame count */

    bool active;
} BattleBGState;

/* LoadedBGData — mirrors the assembly loaded_bg_data struct (include/structs.asm).
 * One instance per background layer.  Holds palette data for fade/interpolation,
 * plus animation configuration loaded from BG_DATA_TABLE. */
typedef struct {
    uint8_t  target_layer;                  /* 0: which BG layer (1-4) */
    uint8_t  bitdepth;                      /* 1: 2 or 4 (bpp) */
    uint8_t  freeze_palette_scrolling;      /* 2: skip palette animation when set */
    uint8_t  palette_shifting_style;        /* 3: 0=none, 1=cycle1, 2=cycle2, 3=both */
    uint8_t  palette_cycle_1_first;         /* 4 */
    uint8_t  palette_cycle_1_last;          /* 5 */
    uint8_t  palette_cycle_2_first;         /* 6 */
    uint8_t  palette_cycle_2_last;          /* 7 */
    uint8_t  palette_cycle_1_step;          /* 8 */
    uint8_t  palette_cycle_2_step;          /* 9 */
    uint8_t  palette_change_speed;          /* 10: frames per palette step */
    uint8_t  palette_change_duration_left;  /* 11: frame counter */
    uint16_t palette[16];                   /* 12: current working palette (32 bytes) */
    uint16_t palette2[16];                  /* 44: backup/original palette (32 bytes) */
    uint16_t palette_index;                 /* 76: index into global palettes[] array
                                             * (assembly stores as WRAM pointer;
                                             *  C port uses palette array index) */

    /* Scrolling movement config (from assembly offsets 78-95) */
    uint8_t  scrolling_movements[4];        /* 78: 4 scrolling movement indices into BG_SCROLLING_TABLE */
    uint8_t  current_scrolling_movement;    /* 82: which movement is active (0-3, wraps) */
    uint16_t scrolling_duration_left;       /* 83: frames remaining for current movement */
    int16_t  horizontal_position;           /* 85: accumulated horizontal scroll position */
    int16_t  vertical_position;             /* 87: accumulated vertical scroll position */
    int16_t  horizontal_velocity;           /* 89: horizontal scroll speed per frame */
    int16_t  vertical_velocity;             /* 91: vertical scroll speed per frame */
    int16_t  horizontal_acceleration;       /* 93: horizontal velocity change per frame */
    int16_t  vertical_acceleration;         /* 95: vertical velocity change per frame */

    /* Distortion animation styles (from assembly offsets 97-103) */
    uint8_t  distortion_styles[4];          /* 97: 4 distortion style indices into BG_DISTORTION_TABLE */
    uint8_t  current_distortion_style_index;/* 101: which style is active (0-3, wraps) */
    uint16_t distortion_duration_left;      /* 102: frames remaining for current style */

    /* Distortion runtime parameters (from assembly offsets 104-118) */
    uint8_t  distortion_type;               /* 104: 0=none, 1=HORIZ_SMOOTH, 2=HORIZ_INTERLACED, 3=VERT */
    int16_t  distortion_ripple_frequency;   /* 105: phase advance per scanline */
    int16_t  distortion_ripple_amplitude;   /* 107: ripple amplitude (high byte used as M7A multiplier) */
    uint8_t  distortion_speed;              /* 109: current phase index (8-bit, wrapping) */
    int16_t  distortion_compression_rate;   /* 110: compression per scanline */
    int16_t  distortion_freq_accel;         /* 112: frequency acceleration per frame */
    int16_t  distortion_amp_accel;          /* 114: amplitude acceleration per frame */
    int8_t   distortion_speed_accel;        /* 116: speed acceleration per frame */
    int16_t  distortion_comp_accel;         /* 117: compression acceleration per frame */
} LoadedBGData;

/* bg_layer_config_entry — matches the assembly struct (include/structs.asm).
 * 17-byte config loaded from BG_DATA_TABLE for each battle background layer. */
typedef struct {
    uint8_t graphics;               /* 0: graphics asset index */
    uint8_t palette;                /* 1: palette asset index */
    uint8_t bitdepth;               /* 2: 2 or 4 (bpp) */
    uint8_t palette_shifting_style; /* 3: 0=none, 1=cycle1, 2=cycle2, 3=both */
    uint8_t palette_cycle_1_first;  /* 4 */
    uint8_t palette_cycle_1_last;   /* 5 */
    uint8_t palette_cycle_2_first;  /* 6 */
    uint8_t palette_cycle_2_last;   /* 7 */
    uint8_t palette_change_speed;   /* 8 */
    uint8_t scrolling_movements[4]; /* 9: indices into BG_SCROLLING_TABLE */
    uint8_t distortion_styles[4];   /* 13: indices into BG_DISTORTION_TABLE */
} BGLayerConfigEntry;

/* Sine lookup table — 256 signed bytes, peak +/-127.
 * Precomputed to match ROM's SINE_LOOKUP_TABLE. */
extern const int8_t sine_table[256];

/* Two global instances matching LOADED_BG_DATA_LAYER1/LAYER2 in BSS */
extern LoadedBGData loaded_bg_data_layer1;
extern LoadedBGData loaded_bg_data_layer2;

/* Per-scanline horizontal offset array for HDMA emulation */
extern int16_t bg2_scanline_hoffset[BATTLEBG_MAX_SCANLINES];
extern bool bg2_distortion_active;

/* Initialize the battle background system */
void battle_bg_init(void);

/* Load a battle background by layer ID.
   Loads graphics, tilemap, and palette to appropriate VRAM/CGRAM locations.
   Uses default VRAM addresses for the battle system (tiles=$1000, tilemap=$5C00). */
void battle_bg_load(uint16_t bg_id);

/* Load a battle background with explicit VRAM word addresses.
   Used by gas station (BG2 tiles=$6000, tilemap=$7C00). */
void battle_bg_load_at(uint16_t bg_id, uint16_t gfx_vram, uint16_t arr_vram,
                       uint8_t cgram_color);

/* Update battle background animation (call once per frame).
   Updates scrolling, distortion offsets, and palette cycling. */
void battle_bg_update(void);

/* INTERPOLATE_BG_PALETTE_COLORS — Port of asm/battle/effects/interpolate_bg_palette_colors.asm.
 * Adjusts all palette entries for the active background layer(s).
 * Special values: 0xFFFF=white, 0=black, 0x0100=restore from backup.
 * Other values = brightness multiplier (result = channel * value / 256). */
void interpolate_bg_palette_colors(uint16_t brightness);

/* INTERPOLATE_BG_PALETTE_COLOR — Port of asm/battle/effects/interpolate_bg_palette_color.asm.
 * Adjusts a single palette entry in a loaded_bg_data struct.
 * index: which of 16 colors.  brightness: see above. */
void interpolate_bg_palette_color(LoadedBGData *data, uint16_t index, uint16_t brightness);

/* ROTATE_BG_DISTORTION — Port of asm/battle/rotate_bg_distortion.asm.
 * Cycles distortion style entries: swaps [0] and [3], zeros [1],
 * resets distortion_duration_left to 1. Used during Giygas death. */
void rotate_bg_distortion(void);

/* LOAD_BG_LAYER_CONFIG — Port of asm/battle/load_bg_layer_config.asm.
 * Zeroes the target loaded_bg_data struct, then copies config fields from a
 * bg_layer_config_entry: bitdepth, palette shifting params, scrolling_movements,
 * distortion_styles. Initializes all duration counters to 1. */
void load_bg_layer_config(LoadedBGData *target, const BGLayerConfigEntry *config);

/* GENERATE_BATTLEBG_FRAME — Port of asm/misc/battlebgs/generate_frame.asm.
 * Per-layer animation state machine. Cycles through scrolling movements
 * and distortion styles from BG_SCROLLING_TABLE / BG_DISTORTION_TABLE,
 * updates positions and distortion parameters, applies to target BG layer,
 * and computes per-scanline HDMA offsets.
 * layer_index: 0 = layer 1, 1 = layer 2. */
void generate_battlebg_frame(LoadedBGData *data, int layer_index);

/* DISTORT_30FPS — when set, layer 2 distortion runs at 30fps. */
extern uint16_t distort_30fps;

/* DARKEN_BG_PALETTES — Port of asm/battle/effects/darken_bg_palettes.asm.
 * Halves each RGB channel of all 16 palette entries in both layers
 * (LSR + AND $3DEF), then copies the result to global palettes[].
 * Skips layer 2 if its target_layer is 0. */
void darken_bg_palettes(void);

/* RESTORE_BG_PALETTE_BACKUPS — Port of asm/battle/effects/restore_bg_palette_backups.asm.
 * Copies palette2[] (backup) → palette[] for both layers, then copies
 * palette[] → global palettes[].  Skips layer 2's global copy if
 * target_layer is 0. */
void restore_bg_palette_backups(void);

#endif /* GAME_BATTLE_BG_H */
