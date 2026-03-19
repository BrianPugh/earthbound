/*
 * Oval window / swirl effect system.
 *
 * Ported from assembly:
 *   INIT_OVAL_WINDOW        — asm/battle/init_oval_window.asm
 *   INIT_SWIRL_EFFECT       — asm/misc/init_swirl_effect.asm
 *   UPDATE_SWIRL_EFFECT     — asm/misc/update_swirl_effect.asm
 *   CLOSE_OVAL_WINDOW       — asm/battle/close_oval_window.asm
 *   STOP_OVAL_WINDOW        — asm/battle/stop_oval_window.asm
 *   GENERATE_OVAL_WINDOW_DATA — asm/text/generate_oval_window_data.asm
 *   SET_WINDOW_MASK          — asm/system/set_window_mask.asm
 *   DISABLE_WINDOWS          — asm/text/disable_windows.asm
 *   IS_PSI_ANIMATION_ACTIVE  — asm/battle/is_psi_animation_active.asm
 */

#include "game/oval_window.h"
#include "game/battle.h"
#include "game/battle_bg.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include <string.h>

/* ---- Oval window animation data ---- */
/* These are animation parameters (code constants), not ROM game data.
 * Ported from asm/data/unknown/C4A5CE.asm etc.
 * 0x8000 in initial_width/height means "keep current value". */

/* Standard open: centre=(VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y), start at 0 size, expand */
static const OvalWindowData oval_standard_open[] = {
    { 0x3D, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, 0x0000, 0x0000, 0, 0, 0x00E0, 0x00B7, 0x0004, 0x0003 },
    { 0 } /* terminator */
};

/* Variant open: same as standard but slower (duration=0x64) */
static const OvalWindowData oval_variant_open[] = {
    { 0x64, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, 0x0000, 0x0000, 0, 0, 0x00E0, 0x00B7, 0x0004, 0x0003 },
    { 0 } /* terminator */
};

/* Standard close: shrink from current position */
static const OvalWindowData oval_standard_close[] = {
    { 0x3D, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, (int16_t)0x8000, (int16_t)0x8000,
      0, 0, (int16_t)0xFF20, (int16_t)0xFF49, (int16_t)0xFFFC, (int16_t)0xFFFD },
    { 0 } /* terminator */
};

/* Variant close: slower */
static const OvalWindowData oval_variant_close[] = {
    { 0x64, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, (int16_t)0x8000, (int16_t)0x8000,
      0, 0, (int16_t)0xFF20, (int16_t)0xFF49, (int16_t)0xFFFC, (int16_t)0xFFFD },
    { 0 } /* terminator */
};

/* Battle variant: 3 frames (shrink, hold, shrink more) */
static const OvalWindowData oval_battle[] = {
    { 0x3C, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, (int16_t)0x9800, (int16_t)0x7F00,
      0, 0, (int16_t)0xFF20, (int16_t)0xFF49, (int16_t)0xFFFC, (int16_t)0xFFFD },
    { 0x3C, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, (int16_t)0x8000, (int16_t)0x8000,
      0, 0, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0x3C, 0, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, (int16_t)0x8000, (int16_t)0x8000,
      0, 0, (int16_t)0xFF38, (int16_t)0xFF50, (int16_t)0xFFFC, (int16_t)0xFFFD },
    { 0 } /* terminator */
};

/* Arc cosine lookup table (256 entries) — mathematical table for ellipse generation.
 * Ported from asm/data/unknown/C0B2FF.asm (OVAL_ARC_TABLE). */
static const uint8_t arc_table[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xFE, 0xFE, 0xFE,
    0xFE, 0xFE, 0xFE, 0xFE, 0xFD, 0xFD, 0xFD, 0xFD,
    0xFD, 0xFD, 0xFD, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC,
    0xFB, 0xFB, 0xFB, 0xFB, 0xFB, 0xFA, 0xFA, 0xFA,
    0xFA, 0xFA, 0xF9, 0xF9, 0xF9, 0xF9, 0xF8, 0xF8,
    0xF8, 0xF8, 0xF7, 0xF7, 0xF7, 0xF7, 0xF6, 0xF6,
    0xF6, 0xF5, 0xF5, 0xF5, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF3, 0xF3, 0xF3, 0xF2, 0xF2, 0xF1, 0xF1, 0xF1,
    0xF0, 0xF0, 0xF0, 0xEF, 0xEF, 0xEF, 0xEE, 0xEE,
    0xED, 0xED, 0xEC, 0xEC, 0xEC, 0xEB, 0xEB, 0xEA,
    0xEA, 0xE9, 0xE9, 0xE9, 0xE8, 0xE8, 0xE7, 0xE7,
    0xE6, 0xE6, 0xE5, 0xE5, 0xE4, 0xE4, 0xE3, 0xE3,
    0xE2, 0xE2, 0xE1, 0xE1, 0xE0, 0xDF, 0xDF, 0xDE,
    0xDE, 0xDD, 0xDD, 0xDC, 0xDB, 0xDB, 0xDA, 0xDA,
    0xD9, 0xD8, 0xD8, 0xD7, 0xD6, 0xD6, 0xD5, 0xD4,
    0xD4, 0xD3, 0xD2, 0xD2, 0xD1, 0xD0, 0xCF, 0xCF,
    0xCE, 0xCD, 0xCC, 0xCC, 0xCB, 0xCA, 0xC9, 0xC9,
    0xC8, 0xC7, 0xC6, 0xC5, 0xC5, 0xC4, 0xC3, 0xC2,
    0xC1, 0xC0, 0xBF, 0xBF, 0xBE, 0xBD, 0xBC, 0xBB,
    0xBA, 0xB9, 0xB8, 0xB7, 0xB6, 0xB5, 0xB4, 0xB3,
    0xB2, 0xB1, 0xB0, 0xAF, 0xAE, 0xAD, 0xAC, 0xAA,
    0xA9, 0xA8, 0xA7, 0xA6, 0xA5, 0xA3, 0xA2, 0xA1,
    0xA0, 0x9F, 0x9D, 0x9C, 0x9B, 0x99, 0x98, 0x97,
    0x95, 0x94, 0x92, 0x91, 0x8F, 0x8E, 0x8C, 0x8B,
    0x89, 0x88, 0x86, 0x85, 0x83, 0x81, 0x7F, 0x7E,
    0x7C, 0x7A, 0x78, 0x76, 0x74, 0x72, 0x70, 0x6E,
    0x6C, 0x6A, 0x68, 0x66, 0x63, 0x61, 0x5E, 0x5C,
    0x59, 0x56, 0x53, 0x51, 0x4D, 0x4A, 0x47, 0x43,
    0x3F, 0x3B, 0x37, 0x32, 0x2D, 0x27, 0x20, 0x17,
};

/* Window mask lookup table (port of WINDOW_MASK_LOOKUP, asm/data/unknown/C0B0A6.asm) */
static const uint8_t window_mask_lookup[4] = { 0x00, 0x0F, 0xF0, 0xFF };

/* PSI animation state — zeroed at startup, populated by show_psi_animation (battle.c) */
PsiAnimationState psi_animation_state;

/* ---- State variables (BSS mirrors from globals.inc.asm) ---- */
static uint8_t frames_until_next_swirl_update;
static uint8_t frames_until_next_swirl_frame;
static uint8_t swirl_frames_left;
static uint8_t swirl_hdma_table_id;
static uint8_t swirl_invert_enabled;
static uint8_t swirl_reversed;
static uint8_t swirl_mask_settings;
static uint8_t swirl_hdma_channel_offset;
static uint8_t swirl_length_padding;
static uint8_t swirl_auto_restore;
static uint8_t swirl_next_swirl;
static uint8_t swirl_repeat_speed;
static uint8_t swirl_repeats_until_speed_up;
static uint8_t active_oval_window;

static const OvalWindowData *loaded_oval_window;
static int16_t loaded_oval_window_centre_x;
static int16_t loaded_oval_window_centre_y;
static uint16_t loaded_oval_window_width;
static uint16_t loaded_oval_window_height;
static int16_t loaded_oval_window_centre_x_add;
static int16_t loaded_oval_window_centre_y_add;
static int16_t loaded_oval_window_width_velocity;
static int16_t loaded_oval_window_height_velocity;
static int16_t loaded_oval_window_width_acceleration;
static int16_t loaded_oval_window_height_acceleration;

/* Per-scanline window boundary buffer (VIEWPORT_HEIGHT entries, 2 bytes each: left|right packed) */
static uint16_t swirl_window_hdma_buffer[VIEWPORT_HEIGHT];

#define SWIRL_DATA_COUNT 126

/*
 * Parse SNES HDMA table data and fill the per-scanline window boundary buffer.
 *
 * Swirl data format:
 *   Byte 0: DMAP value
 *     0x01 = 2-register mode (2 bytes/entry: WH0, WH1)
 *     0x04 = 4-register mode (4 bytes/entry: WH0, WH1, WH2, WH3)
 *   Remaining: standard SNES HDMA entries:
 *     - 0x00: end of table
 *     - 0x01-0x7F: repeat mode — next N bytes for count scanlines
 *     - 0x80-0xFF: individual mode — (N & 0x7F) scanlines, N bytes each
 *
 * Output: swirl_window_hdma_buffer[VIEWPORT_HEIGHT], packed as (WH0 << 8) | WH1.
 */
static void parse_hdma_to_window_buffer(const uint8_t *data, size_t data_size) {
    /* Clear buffer to disabled state (WH0=0xFF, WH1=0x00) */
    for (int i = 0; i < VIEWPORT_HEIGHT; i++)
        swirl_window_hdma_buffer[i] = 0xFF00;

    if (data_size < 2) return;

    /* Read DMAP byte to determine bytes per entry */
    uint8_t dmap = data[0];
    int bytes_per_entry = (dmap == 0x04) ? 4 : 2;

    const uint8_t *p = data + 1;
    const uint8_t *end = data + data_size;
    int scanline = VIEWPORT_PAD_TOP;
    int max_scanline = VIEWPORT_PAD_TOP + SNES_HEIGHT;

    while (p < end && scanline < max_scanline) {
        uint8_t count_byte = *p++;
        if (count_byte == 0) break; /* end of table */

        if (count_byte & 0x80) {
            /* Individual mode: (count & 0x7F) scanlines with per-line data */
            int count = count_byte & 0x7F;
            for (int i = 0; i < count && scanline < max_scanline && p + 1 < end; i++) {
                uint8_t wh0 = *p++;
                uint8_t wh1 = *p++;
                swirl_window_hdma_buffer[scanline++] = (uint16_t)((wh0 << 8) | wh1);
                /* Skip WH2/WH3 in 4-register mode */
                if (bytes_per_entry == 4 && p + 1 < end)
                    p += 2;
            }
        } else {
            /* Repeat mode: same bytes for `count` scanlines */
            int count = count_byte;
            if (p + 1 >= end) break;
            uint8_t wh0 = *p++;
            uint8_t wh1 = *p++;
            /* Skip WH2/WH3 in 4-register mode */
            if (bytes_per_entry == 4 && p + 1 < end)
                p += 2;
            for (int i = 0; i < count && scanline < max_scanline; i++) {
                swirl_window_hdma_buffer[scanline++] = (uint16_t)((wh0 << 8) | wh1);
            }
        }
    }

    /* Extend edge values into padding rows so the swirl covers the full viewport */
    if (VIEWPORT_PAD_TOP > 0) {
        uint16_t top_val = swirl_window_hdma_buffer[VIEWPORT_PAD_TOP];
        for (int i = 0; i < VIEWPORT_PAD_TOP; i++)
            swirl_window_hdma_buffer[i] = top_val;

        int last = VIEWPORT_PAD_TOP + SNES_HEIGHT - 1;
        uint16_t bot_val = swirl_window_hdma_buffer[last];
        for (int i = last + 1; i < VIEWPORT_HEIGHT; i++)
            swirl_window_hdma_buffer[i] = bot_val;
    }
}

/* ---- SWIRL_PRIMARY_TABLE (from asm/bankconfig/common/bank0e.asm) ---- */
/* 7 entries x 4 bytes: speed, hdma_table_id, frames_left, unused */
static const uint8_t swirl_primary_table[] = {
    0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x17, 0x00,
    0x04, 0x17, 0x0F, 0x00,
    0x03, 0x26, 0x16, 0x00,
    0x04, 0x3C, 0x15, 0x00,
    0x02, 0x51, 0x1C, 0x00,
    0x03, 0x6D, 0x11, 0x00,
};

/* ---- Generate oval window HDMA data ---- */
/* Port of GENERATE_OVAL_WINDOW_DATA (C0B149).
 * Computes per-scanline left/right window boundaries for an ellipse.
 * centre_x: screen X of ellipse centre
 * centre_y: screen Y of ellipse centre (unused in calculation but used for direction)
 * half_width: horizontal radius (high byte of width)
 * half_height: vertical radius (high byte of height)
 */
static void generate_oval_window_data(int16_t centre_x, int16_t centre_y,
                                       uint16_t half_width, uint16_t half_height) {
    /* The assembly has two paths: one for centre_y in the upper half (drawing top-down),
     * and one for centre_y in the lower half (drawing bottom-up).
     * The top-down path starts from Y=0 in the buffer and works down.
     * The bottom-up path starts from Y=(VIEWPORT_HEIGHT-1) and works up.
     * In both cases, the algorithm is the same ellipse calculation. */

    /* Clear the buffer to 0xFF00 (left=0xFF, right=0x00 — window disabled).
     * Assembly uses LDA #$00FF which stores little-endian as [0xFF,0x00] → WH0=0xFF, WH1=0x00.
     * C packing is (left<<8)|right, so 0xFF00 → left=0xFF, right=0x00. */
    for (int i = 0; i < VIEWPORT_HEIGHT; i++)
        swirl_window_hdma_buffer[i] = 0xFF00;

    if (half_width == 0 || half_height == 0)
        return;

    /* Assembly: BMI→bottom-up (negative), BCS→top-down (>= centre), fall-through→bottom-up */
    bool top_down = (centre_y >= VIEWPORT_CENTER_Y);

    if (top_down) {
        /* Top-down path (assembly @UNKNOWN1 through @UNKNOWN15) */
        int y_idx = 0;

        /* Fill scanlines above the ellipse (window disabled) */
        int gap = centre_y - half_height;
        if (gap > 0) {
            for (int i = 0; i < gap && y_idx < VIEWPORT_HEIGHT; i++, y_idx++)
                swirl_window_hdma_buffer[y_idx] = 0xFF00;
        }

        /* Current row offset from edge of ellipse (counts down from half_height to 0) */
        int row_offset = (gap < 0) ? (half_height + gap) : half_height;
        if (row_offset < 0) row_offset = 0;

        /* Draw ellipse scanlines from top to bottom */
        while (row_offset >= 0 && y_idx < VIEWPORT_HEIGHT) {
            /* Compute horizontal half-width at this row */
            uint16_t x_extent;
            if (row_offset == 0) {
                x_extent = half_width;
            } else {
                /* Division: (row_offset * 256) / half_height */
                uint16_t div_result = ((uint16_t)row_offset << 8) / half_height;
                if (div_result < 256) {
                    /* Lookup arc table, multiply by half_width */
                    uint8_t arc_val = arc_table[div_result];
                    uint16_t product = (uint16_t)arc_val * half_width;
                    x_extent = (uint16_t)(((uint32_t)product + 0x80) >> 8);
                } else {
                    x_extent = 0;
                }
            }

            /* Compute left and right screen coordinates (viewport space) */
            int right = centre_x + x_extent;
            int left = centre_x - x_extent;

            /* Convert from viewport space to SNES coordinate space.
             * The rendering loop compares window boundaries against
             * SNES-space pixel coordinates (wx = x - VIEWPORT_PAD_LEFT),
             * so the HDMA table values must be in the same space. */
            left -= VIEWPORT_PAD_LEFT;
            right -= VIEWPORT_PAD_LEFT;

            uint16_t packed;
            if (right < 0 || left > 255) {
                packed = 0xFF00; /* fully off-screen: left=0xFF, right=0x00 → disabled */
            } else {
                if (right > 255) right = 255;
                if (left < 0) left = 0;
                /* Pack: high byte = left, low byte = right */
                packed = (uint16_t)((left << 8) | right);
            }

            swirl_window_hdma_buffer[y_idx] = packed;

            /* Mirror: also write the symmetric row below centre */
            int mirror_idx = y_idx + row_offset * 2;
            if (mirror_idx < VIEWPORT_HEIGHT) {
                swirl_window_hdma_buffer[mirror_idx] = packed;
            }

            y_idx++;
            row_offset--;
        }

        /* Assembly fills remaining scanlines below the ellipse with disabled values.
         * The C port pre-clears the entire buffer (line 164), so non-ellipse entries
         * are already disabled. No post-fill needed. */
    } else {
        /* Bottom-up path (assembly @UNKNOWN16 through @UNKNOWN30) */
        int y_idx = VIEWPORT_HEIGHT - 1;

        /* Fill scanlines below the ellipse (window disabled) */
        int gap = (VIEWPORT_HEIGHT - centre_y) - half_height;
        if (gap > 0) {
            for (int i = 0; i < gap && y_idx >= 0; i++, y_idx--)
                swirl_window_hdma_buffer[y_idx] = 0xFF00;
        }

        int row_offset = (gap < 0) ? (half_height + gap) : half_height;
        if (row_offset < 0) row_offset = 0;

        while (row_offset >= 0 && y_idx >= 0) {
            uint16_t x_extent;
            if (row_offset == 0) {
                x_extent = half_width;
            } else {
                uint16_t div_result = ((uint16_t)row_offset << 8) / half_height;
                if (div_result < 256) {
                    uint8_t arc_val = arc_table[div_result];
                    uint16_t product = (uint16_t)arc_val * half_width;
                    x_extent = (uint16_t)(((uint32_t)product + 0x80) >> 8);
                } else {
                    x_extent = 0;
                }
            }

            int right = centre_x + x_extent;
            int left = centre_x - x_extent;

            /* Convert from viewport space to SNES coordinate space */
            left -= VIEWPORT_PAD_LEFT;
            right -= VIEWPORT_PAD_LEFT;

            uint16_t packed;
            if (right < 0 || left > 255) {
                packed = 0xFF00; /* fully off-screen: left=0xFF, right=0x00 → disabled */
            } else {
                if (right > 255) right = 255;
                if (left < 0) left = 0;
                packed = (uint16_t)((left << 8) | right);
            }

            swirl_window_hdma_buffer[y_idx] = packed;

            int mirror_idx = y_idx - row_offset * 2;
            if (mirror_idx >= 0) {
                swirl_window_hdma_buffer[mirror_idx] = packed;
            }

            y_idx--;
            row_offset--;
        }

        /* Assembly fills remaining scanlines above the ellipse with disabled values.
         * The C port pre-clears the entire buffer (line 164), so non-ellipse entries
         * are already disabled. No post-fill needed.
         * NOTE: The previous code used `top_end = centre_y - half_height - 1` which
         * had an off-by-one that overwrote the extreme edge ellipse entry. */
    }
}

/* Copy HDMA buffer to PPU per-scanline window tables */
static void apply_hdma_to_ppu(void) {
    for (int i = 0; i < VIEWPORT_HEIGHT; i++) {
        uint16_t packed = swirl_window_hdma_buffer[i];
        ppu.wh0_table[i] = (packed >> 8) & 0xFF; /* left */
        ppu.wh1_table[i] = packed & 0xFF;         /* right */
    }
    ppu.window_hdma_active = true;
}

/* ---- SET_WINDOW_MASK (port of asm/system/set_window_mask.asm) ---- */
void set_window_mask(uint16_t config, uint16_t invert) {
    uint8_t val;

    /* W12SEL: config bits 0-1 */
    val = window_mask_lookup[config & 0x03];
    if (invert) val &= 0xAA;
    ppu.w12sel = val;

    /* W34SEL: config bits 2-3 */
    val = window_mask_lookup[(config >> 2) & 0x03];
    if (invert) val &= 0xAA;
    ppu.w34sel = val;

    /* WOBJSEL: config bits 4-5 */
    val = window_mask_lookup[(config >> 4) & 0x03];
    if (invert) val &= 0xAA;
    ppu.wobjsel = val;

    /* TMW and TSW: config bits 0-4 (low 5 bits) */
    ppu.tmw = config & 0x1F;
    ppu.tsw = config & 0x1F;

    /* WBGLOG and WOBJLOG */
    if (invert) {
        ppu.wbglog = 0x00;
        ppu.wobjlog = 0x00;
    } else {
        ppu.wbglog = 0x55;
        ppu.wobjlog = 0x55;
    }
}

/* ---- DISABLE_WINDOWS (port of C0B0AA) ---- */
void disable_windows(void) {
    ppu.wh0 = 0xFF;
    ppu.wh1 = 0x00;
    ppu.wh2 = 0xFF;
    ppu.wh3 = 0x00;
}

/* ---- INIT_SWIRL_EFFECT (port of C4A67E) ---- */
void init_swirl_effect(uint16_t type, uint16_t options) {
    /* Decode options */
    swirl_invert_enabled = (options & 0x02) ? 1 : 0;
    swirl_reversed = (options & 0x01) ? 1 : 0;
    swirl_mask_settings = (options & 0x04) ? 32 : 31;

    frames_until_next_swirl_update = 1;

    /* Read from swirl primary table: 4 bytes per entry */
    uint16_t offset = type * 4;
    frames_until_next_swirl_frame = swirl_primary_table[offset + 0];
    swirl_frames_left = swirl_primary_table[offset + 2];
    swirl_hdma_table_id = swirl_primary_table[offset + 1];

    if (swirl_reversed) {
        swirl_hdma_table_id += swirl_frames_left;
    }

    /* Set LOADED_OVAL_WINDOW for type 0 to standard open sequence */
    loaded_oval_window = NULL;
    if (type == 0) {
        loaded_oval_window = oval_standard_open;
    }

    swirl_hdma_channel_offset = 0;
    swirl_length_padding = 0;
    swirl_auto_restore = 1;

    if (options & 0x80) {
        swirl_next_swirl = (uint8_t)type;
        frames_until_next_swirl_frame = 4;
        swirl_repeat_speed = 0;
        swirl_repeats_until_speed_up = 8;
    } else {
        swirl_next_swirl = 0;
    }

    disable_windows();
}

/* ---- START_BATTLE_SWIRL (port of C2E8C4) ----
 * Wrapper around init_swirl_effect that also sets the padding length.
 * Assembly params: A=type, X=options, Y=padding. */
void start_battle_swirl(uint16_t type, uint16_t options, uint16_t padding) {
    init_swirl_effect(type, options);
    swirl_length_padding = (uint8_t)padding;
}

/* ---- INIT_OVAL_WINDOW (port of C2EA15) ---- */
void init_oval_window(uint16_t type) {
    active_oval_window = (uint8_t)type;
    init_swirl_effect(0, 0);
    swirl_mask_settings = 19;

    switch (type) {
    case 2:
        loaded_oval_window = oval_battle;
        break;
    case 1:
        loaded_oval_window = oval_variant_open;
        break;
    default:
        loaded_oval_window = oval_standard_open;
        break;
    }
}

/* ---- CLOSE_OVAL_WINDOW (port of C2EA74) ---- */
void close_oval_window(void) {
    init_swirl_effect(0, 0);
    swirl_mask_settings = 19;

    if (active_oval_window != 0) {
        loaded_oval_window = oval_variant_close;
    } else {
        loaded_oval_window = oval_standard_close;
    }
}

/* ---- STOP_OVAL_WINDOW (port of C2EAAA) ---- */
void stop_oval_window(void) {
    frames_until_next_swirl_update = 0;
    loaded_oval_window = NULL;
    /* mask_hdma_channel(3) — in the port we just disable the HDMA */
    ppu.window_hdma_active = false;
    set_window_mask(0, 0);
}

/* ---- RESET_SWIRL_UPDATE_TIMER ----
 * Resets FRAMES_UNTIL_NEXT_SWIRL_UPDATE to 0. Used by
 * CLEAR_BATTLE_VISUAL_EFFECTS to cancel pending swirl animation. */
void reset_swirl_update_timer(void) {
    frames_until_next_swirl_update = 0;
}

/* ---- STOP_BATTLE_SWIRL (port of C2E9ED) ----
 * Stops the battle swirl effect: clears swirl timer, masks HDMA channel,
 * clears the fixed color (COLDATA), and disables window masking. */
void stop_battle_swirl(void) {
    frames_until_next_swirl_update = 0;
    /* mask_hdma_channel(swirl_hdma_channel_offset+3) — disable the HDMA */
    ppu.window_hdma_active = false;
    /* SET_COLDATA(0,0,0) — clear fixed color to black */
    ppu.coldata_r = 0;
    ppu.coldata_g = 0;
    ppu.coldata_b = 0;
    /* SET_WINDOW_MASK(0,0) — disable window masking */
    set_window_mask(0, 0);
}

/* ---- IS_PSI_ANIMATION_ACTIVE (port of C2EACF) ---- */
bool is_psi_animation_active(void) {
    /* Port of C2EACF: active if either PSI animation or swirl update is running */
    if (psi_animation_state.time_until_next_frame != 0)
        return true;
    return frames_until_next_swirl_update != 0;
}

/*
 * IS_BATTLE_SWIRL_ACTIVE (port of C2E9C8)
 *
 * Returns true if the battle swirl animation is still playing.
 * Checks both the swirl update timer and the length padding counter.
 * CLC+SBC #4 computes A - 5. BRANCHLTEQS branches when result <= 0,
 * i.e. swirl_length_padding <= 5 → returns false (not active).
 * Active when frames_until_next_swirl_update != 0 AND swirl_length_padding > 5.
 */
bool is_battle_swirl_active(void) {
    if (frames_until_next_swirl_update == 0)
        return false;
    if (swirl_length_padding <= 5)
        return false;
    return true;
}

/* ---- UPDATE_SWIRL_EFFECT (port of C4A7B0) ----
 * This is the main per-frame update. It handles two paths:
 * 1. Oval window animation (when loaded_oval_window is not NULL)
 * 2. Battle swirl (SWIRL_DATA playback)
 */
void update_swirl_effect(void) {
    if (frames_until_next_swirl_update == 0)
        return;

    /* ---- Path 1: Oval window animation ---- */
    if (loaded_oval_window != NULL) {
        frames_until_next_swirl_update--;
        if (frames_until_next_swirl_update != 0)
            goto update_position;

        /* Read next animation frame */
        frames_until_next_swirl_update = loaded_oval_window->duration;
        if (frames_until_next_swirl_update == 0) {
            /* End of animation sequence */
            loaded_oval_window = NULL;
            return;
        }

        /* Load frame parameters (0x8000 = keep current value) */
        if (loaded_oval_window->centre_x != (int16_t)0x8000)
            loaded_oval_window_centre_x = loaded_oval_window->centre_x;
        if (loaded_oval_window->centre_y != (int16_t)0x8000)
            loaded_oval_window_centre_y = loaded_oval_window->centre_y;
        if (loaded_oval_window->initial_width != (int16_t)0x8000)
            loaded_oval_window_width = (uint16_t)loaded_oval_window->initial_width;
        if (loaded_oval_window->initial_height != (int16_t)0x8000)
            loaded_oval_window_height = (uint16_t)loaded_oval_window->initial_height;

        loaded_oval_window_centre_x_add = loaded_oval_window->centre_x_add;
        loaded_oval_window_centre_y_add = loaded_oval_window->centre_y_add;
        loaded_oval_window_width_velocity = loaded_oval_window->width_velocity;
        loaded_oval_window_height_velocity = loaded_oval_window->height_velocity;
        loaded_oval_window_width_acceleration = loaded_oval_window->width_acceleration;
        loaded_oval_window_height_acceleration = loaded_oval_window->height_acceleration;

        /* Advance to next frame in sequence */
        loaded_oval_window++;

update_position:
        /* Update position */
        loaded_oval_window_centre_x += loaded_oval_window_centre_x_add;
        loaded_oval_window_centre_y += loaded_oval_window_centre_y_add;

        /* Update velocities with acceleration */
        loaded_oval_window_width_velocity += loaded_oval_window_width_acceleration;
        loaded_oval_window_height_velocity += loaded_oval_window_height_acceleration;

        /* Apply velocity to width (clamp to 0 if shrinking past zero) */
        if (loaded_oval_window_width_velocity < 0) {
            uint16_t abs_vel = (uint16_t)(-(int16_t)loaded_oval_window_width_velocity);
            if (loaded_oval_window_width < abs_vel)
                loaded_oval_window_width = 0;
            else
                loaded_oval_window_width += (uint16_t)loaded_oval_window_width_velocity;
        } else {
            loaded_oval_window_width += (uint16_t)loaded_oval_window_width_velocity;
        }

        /* Apply velocity to height (same clamping) */
        if (loaded_oval_window_height_velocity < 0) {
            uint16_t abs_vel = (uint16_t)(-(int16_t)loaded_oval_window_height_velocity);
            if (loaded_oval_window_height < abs_vel)
                loaded_oval_window_height = 0;
            else
                loaded_oval_window_height += (uint16_t)loaded_oval_window_height_velocity;
        } else {
            loaded_oval_window_height += (uint16_t)loaded_oval_window_height_velocity;
        }

        /* If both width and height are zero, animation is done */
        if (loaded_oval_window_width == 0 && loaded_oval_window_height == 0) {
            frames_until_next_swirl_update = 0;
            loaded_oval_window = NULL;
            return;
        }

        /* Generate ellipse data and apply to PPU */
        uint16_t half_height = (loaded_oval_window_height >> 8) & 0xFF;
        uint16_t half_width = (loaded_oval_window_width >> 8) & 0xFF;

        generate_oval_window_data(loaded_oval_window_centre_x,
                                  loaded_oval_window_centre_y,
                                  half_width, half_height);
        apply_hdma_to_ppu();

        /* Set window mask registers */
        set_window_mask(swirl_mask_settings, swirl_invert_enabled);
        return;
    }

    /* ---- Path 2: Battle swirl (SWIRL_DATA playback) ---- */
    /* Port of @SWIRL_FRAME_UPDATE (C4A7B0 lines 210-376).
     * Decrement counter when no oval window is loaded. */
    frames_until_next_swirl_update--;
    if (frames_until_next_swirl_update != 0)
        return;

process_swirl_frame:
    if (swirl_frames_left > 0) {
        /* Load and apply the current swirl frame HDMA data.
         * Assembly: reads SWIRL_POINTER_TABLE[swirl_hdma_table_id] to get
         * an offset into SWIRL_DATA, then calls SETUP_HDMA_CHANNEL.
         * C port: load the swirl file by index and parse HDMA table. */
        frames_until_next_swirl_update = frames_until_next_swirl_frame;

        /* Toggle HDMA channel double-buffer (assembly lines 229-244) */
        swirl_hdma_channel_offset = (swirl_hdma_channel_offset + 1) & 1;

        /* Load swirl frame data */
        uint8_t table_id;
        if (!swirl_reversed) {
            table_id = swirl_hdma_table_id;
            swirl_hdma_table_id++;
        } else {
            swirl_hdma_table_id--;
            table_id = swirl_hdma_table_id;
        }

        if (table_id < SWIRL_DATA_COUNT) {
            size_t swirl_size;
            swirl_size = ASSET_SIZE(ASSET_SWIRLS(table_id));
            const uint8_t *swirl_data = ASSET_DATA(ASSET_SWIRLS(table_id));
            if (swirl_data) {
                parse_hdma_to_window_buffer(swirl_data, swirl_size);
                apply_hdma_to_ppu();
                ppu.window_hdma_active = true;
            }
        }

        set_window_mask(swirl_mask_settings, swirl_invert_enabled);
        swirl_frames_left--;
        return;
    }

    /* Swirl finished — check for repeat (assembly @NO_FRAMES_LEFT, lines 317-416).
     * If swirl_next_swirl != 0, restart the animation from SWIRL_PRIMARY_TABLE. */
    if (swirl_next_swirl != 0) {
        /* Decrement repeats counter; speed up at 0 */
        swirl_repeats_until_speed_up--;
        if (swirl_repeats_until_speed_up == 0) {
            /* Speed up: advance through 3 speed levels */
            swirl_repeat_speed++;
            if (swirl_repeat_speed == 1) {
                swirl_repeats_until_speed_up = 4;
                frames_until_next_swirl_frame = 3;
            } else if (swirl_repeat_speed == 2) {
                swirl_repeats_until_speed_up = 6;
                frames_until_next_swirl_frame = 2;
            } else if (swirl_repeat_speed == 3) {
                swirl_repeats_until_speed_up = 12;
                frames_until_next_swirl_frame = 1;
            }
            /* After speed level 3, repeats_until_speed_up stays at 0 → no further changes */
            if (swirl_repeats_until_speed_up != 0)
                goto process_swirl_frame;
            /* Speed levels exhausted (speed > 3): fall through to padding check,
             * matching assembly fallthrough from @AFTER_SPEED_SET to @NO_NEXT_SWIRL */
        } else {
            /* Reload frame count and starting HDMA table ID from primary table */
            uint16_t offset = swirl_next_swirl * 4;
            swirl_frames_left = swirl_primary_table[offset + 2];
            swirl_hdma_table_id = swirl_primary_table[offset + 1];
            if (swirl_reversed) {
                swirl_hdma_table_id += swirl_frames_left;
            }
            goto process_swirl_frame;
        }
    }

    /* Handle padding frames */
    if (swirl_length_padding > 0) {
        frames_until_next_swirl_update = 1;
        swirl_length_padding--;
        return;
    }

    /* Auto-restore: disable windows, restore palette, reset color math.
     * Assembly @CHECK_AUTO_RESTORE (lines 429-449):
     *   MASK_HDMA_CHANNEL, SET_WINDOW_MASK(0,0), RESTORE_BG_PALETTE_BACKUPS,
     *   SET_COLDATA(0,0,0), SET_COLOR_MATH_FROM_TABLE(CURRENT_LAYER_CONFIG) */
    if (swirl_auto_restore) {
        ppu.window_hdma_active = false;
        set_window_mask(0, 0);
        restore_bg_palette_backups();
        ppu.coldata_r = 0;
        ppu.coldata_g = 0;
        ppu.coldata_b = 0;
        set_color_math_from_table(bt.current_layer_config);
    }
}

void set_swirl_mask_settings(uint8_t value) {
    swirl_mask_settings = value;
}

void set_swirl_auto_restore(uint8_t value) {
    swirl_auto_restore = value;
}
