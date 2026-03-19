/*
 * Callroutine palette fade and color math functions.
 * Extracted from callroutine.c.
 */
#include "entity/callroutine_internal.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "game/game_state.h"
#include "game/fade.h"
#include "game/audio.h"
#include "game/map_loader.h"
#include "core/decomp.h"
#include "data/assets.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/constants.h"
#include "core/memory.h"
#include "game_main.h"
#include <string.h>

/* ---- Palette fade engine state (see buffer_layout.h) ---- */
#include "entity/buffer_layout.h"

int16_t get_colour_fade_slope(int16_t current, int16_t target,
                                     int16_t frames) {
    if (frames <= 0) return 0;
    int32_t diff = (int32_t)((uint32_t)(int32_t)(target - current) << 8);
    return (int16_t)(diff / frames);
}

void copy_fade_buffer_to_palettes(void) {
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    memcpy(fade->target, ert.palettes, 512);
}

void prepare_palette_fade_slopes(int16_t frames, uint16_t mask) {
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    memset(fade->slope_r, 0, sizeof(fade->slope_r) + sizeof(fade->slope_g)
           + sizeof(fade->slope_b) + sizeof(fade->accum_r)
           + sizeof(fade->accum_g) + sizeof(fade->accum_b));

    uint16_t group_mask = mask;
    for (int group = 0; group < 16; group++) {
        int start = group * 16;
        int end = start + 16;

        if (group_mask & 1) {
            for (int i = start; i < end; i++) {
                uint16_t target = fade->target[i];
                uint16_t current = ert.palettes[i];

                int16_t t_r = target & 0x1F;
                int16_t t_g = (target >> 5) & 0x1F;
                int16_t t_b = (target >> 10) & 0x1F;
                int16_t c_r = current & 0x1F;
                int16_t c_g = (current >> 5) & 0x1F;
                int16_t c_b = (current >> 10) & 0x1F;

                fade->slope_r[i] = get_colour_fade_slope(c_r, t_r, frames);
                fade->slope_g[i] = get_colour_fade_slope(c_g, t_g, frames);
                fade->slope_b[i] = get_colour_fade_slope(c_b, t_b, frames);

                fade->accum_r[i] = (int16_t)(c_r << 8);
                fade->accum_g[i] = (int16_t)(c_g << 8);
                fade->accum_b[i] = (int16_t)(c_b << 8);
            }
        } else {
            for (int i = start; i < end; i++) {
                uint16_t current = ert.palettes[i];
                fade->accum_r[i] = (int16_t)((current & 0x1F) << 8);
                fade->accum_g[i] = (int16_t)(((current >> 5) & 0x1F) << 8);
                fade->accum_b[i] = (int16_t)(((current >> 10) & 0x1F) << 8);
            }
        }

        group_mask >>= 1;
    }
}

void update_map_palette_animation(void) {
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);

    for (int i = 0; i < 256; i++) {
        int16_t accum_r = fade->accum_r[i] + fade->slope_r[i];
        if (accum_r < 0) {
            fade->slope_r[i] = 0;
            accum_r = 0;
        } else if ((accum_r & 0x1F00) == 0x1F00) {
            fade->slope_r[i] = 0;
            accum_r = 0x1F00;
        }
        fade->accum_r[i] = accum_r;
        uint8_t r = (uint8_t)((accum_r >> 8) & 0x1F);

        int16_t accum_g = fade->accum_g[i] + fade->slope_g[i];
        if (accum_g < 0) {
            fade->slope_g[i] = 0;
            accum_g = 0;
        } else if ((accum_g & 0x1F00) == 0x1F00) {
            fade->slope_g[i] = 0;
            accum_g = 0x1F00;
        }
        fade->accum_g[i] = accum_g;
        uint8_t g = (uint8_t)((accum_g >> 8) & 0x1F);

        int16_t accum_b = fade->accum_b[i] + fade->slope_b[i];
        if (accum_b < 0) {
            /* ROM bug fix: original zeros slope_g (green) instead of
             * slope_b (blue), freezing the green channel mid-fade and
             * causing a green tint.  Fixed to zero the correct channel. */
            fade->slope_b[i] = 0;
            accum_b = 0;
        } else if ((accum_b & 0x1F00) == 0x1F00) {
            fade->slope_b[i] = 0;
            accum_b = 0x1F00;
        }
        fade->accum_b[i] = accum_b;
        uint8_t b = (uint8_t)((accum_b >> 8) & 0x1F);

        ert.palettes[i] = (uint16_t)r | ((uint16_t)g << 5) | ((uint16_t)b << 10);
    }

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/* ---- LOAD_PALETTE_TO_FADE_BUFFER (port of C4954C) ----
 * Applies a brightness fade to each of the 256 palette entries and
 * stores the faded colors into fade->target[].
 *
 * fade_style semantics (from FADE_PALETTE_COLOR, C49496):
 *   0      -> all black
 *   1-49   -> darken (component * fade_style * 5 / 256)
 *   50     -> identity (original color unchanged)
 *   >50    -> all white (0x7FFF) */
uint16_t fade_palette_color(uint16_t color, uint8_t style) {
    if (style == 50) return color;
    if (style > 50) return 0x7FFF;  /* white */

    uint16_t mul = (uint16_t)style * 5;
    uint16_t r = color & 0x1F;
    uint16_t g = (color >> 5) & 0x1F;
    uint16_t b = (color >> 10) & 0x1F;

    /* Fixed-point multiply: component * mul, result in 8.8 format.
     * Clamp to 0x1F, then shift back. */
    uint16_t r_fp = r * mul;
    uint16_t g_fp = g * mul;
    uint16_t b_fp = b * mul;

    if (r_fp > 0x1E45) r_fp = 0x1F00;
    if (g_fp > 0x1E45) g_fp = 0x1F00;
    if (b_fp > 0x1E45) b_fp = 0x1F00;

    r = (r_fp >> 8) & 0x1F;
    g = (g_fp >> 8) & 0x1F;
    b = (b_fp >> 8) & 0x1F;

    return r | (g << 5) | (b << 10);
}

void load_palette_to_fade_buffer(uint8_t fade_style) {
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    for (int i = 0; i < 256; i++) {
        fade->target[i] = fade_palette_color(ert.palettes[i], fade_style);
    }
}

/* ---- FINALIZE_PALETTE_FADE (port of C49740) ----
 * Copies fade->target[] into ert.palettes[] and triggers full upload. */
void finalize_palette_fade(void) {
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    for (int i = 0; i < 256; i++) {
        ert.palettes[i] = fade->target[i];
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/*
 * SETUP_COLOR_MATH_WINDOW — Port of asm/system/palette/setup_color_math_window.asm.
 * Sets color math PPU registers for screen darkening/brightening effects.
 *   cgadsub_val: CGADSUB value ($33 = add to BG1+BG2+OBJ+backdrop,
 *                                $B3 = subtract from same)
 *   intensity: 5-bit color intensity for fixed color (applied to R+G+B)
 */
void setup_color_math_window(uint8_t cgadsub_val, uint8_t intensity) {
    ppu.cgadsub = cgadsub_val;
    ppu.wobjsel = 0x20;
    ppu.wh0 = 0x00;
    ppu.wh1 = 0xFF;
    ppu.tmw = 0x13;
    ppu.wbglog = 0x00;
    ppu.wobjlog = 0x00;
    ppu.cgwsel = 0x10;
    /* FIXED_COLOR_DATA ($2132): bits 7-5 select R/G/B channels,
     * bits 4-0 = intensity. Value $E0 | intensity sets all three channels. */
    uint8_t color_val = intensity & 0x1F;
    ppu.coldata_r = color_val;
    ppu.coldata_g = color_val;
    ppu.coldata_b = color_val;
}

/*
 * CLAMP_COLOR_CHANNEL — Port of asm/system/palette/clamp_color_channel.asm.
 * Clamps a signed 16-bit value to the range [0, 31].
 */
int16_t clamp_color_channel(int16_t value) {
    if (value < 0) return 0;
    if (value > 31) return 31;
    return value;
}

/*
 * ADJUST_PALETTE_BRIGHTNESS — Port of asm/system/palette/adjust_palette_brightness.asm.
 * For a single 16-color sub-palette, reads from MAP_PALETTE_BACKUP,
 * adds brightness offset to each R/G/B channel (clamped to [0,31]),
 * and writes to PALETTES at sub-palette (palette_index + 2).
 */
void adjust_palette_brightness(int palette_index, int16_t brightness) {
    int src_base = palette_index * 16;
    int dst_base = (palette_index + 2) * 16;

    /* Assembly writes to PALETTES + BPP4PALETTE_SIZE*2 + palette_index*32.
     * For palette_index >= 14, destination exceeds the 256-entry palette array.
     * In assembly this harmlessly overwrites adjacent WRAM; in C we must guard. */
    if (dst_base + 16 > 256) return;

    for (int i = 0; i < 16; i++) {
        uint16_t color = ml.map_palette_backup[src_base + i];
        int16_t r = (int16_t)(color & 0x1F) + brightness;
        int16_t g = (int16_t)((color >> 5) & 0x1F) + brightness;
        int16_t b = (int16_t)((color >> 10) & 0x1F) + brightness;
        r = clamp_color_channel(r);
        g = clamp_color_channel(g);
        b = clamp_color_channel(b);
        ert.palettes[dst_base + i] = (uint16_t)(r | (g << 5) | (b << 10));
    }
}

/*
 * APPLY_PALETTE_BRIGHTNESS_ALL — Port of asm/system/palette/apply_palette_brightness_all.asm.
 * Applies brightness offset to all 16 sub-ert.palettes, then triggers full upload.
 */
void apply_palette_brightness_all(int16_t brightness) {
    for (int pal = 0; pal < 16; pal++) {
        adjust_palette_brightness(pal, brightness);
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
}

/*
 * SETUP_ENTITY_COLOR_MATH — Port of asm/overworld/entity/setup_entity_color_math.asm.
 * Reads the current entity's var0 to determine color math mode.
 * If var0 >= 0: add mode (cgadsub = $33), intensity = var0
 * If var0 < 0:  subtract mode (cgadsub = $B3), intensity = -var0
 */
void setup_entity_color_math(void) {
    int16_t entity_offset = ENT(ert.current_entity_slot);
    int16_t var0 = entities.var[0][entity_offset];

    uint8_t cgadsub_val;
    uint8_t intensity;

    if (var0 < 0) {
        cgadsub_val = 0xB3;  /* subtract from BG1+BG2+OBJ+backdrop */
        intensity = (uint8_t)(-var0);
    } else {
        cgadsub_val = 0x33;  /* add to BG1+BG2+OBJ+backdrop */
        intensity = (uint8_t)var0;
    }

    setup_color_math_window(cgadsub_val, intensity);
}

/* ---- Palette-related CALLROUTINE implementations ---- */

int16_t cr_cycle_entity_palette(int16_t ent, int16_t scr,
                                       uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    int16_t slot = ert.current_entity_slot;
    int16_t offset = ENT(slot);

    int16_t frame = entities.var[0][offset];
    int16_t target_pal = entities.var[1][offset];
    int16_t total_frames = entities.var[2][offset];

    uint16_t src_off = (uint16_t)(frame * BPP4PALETTE_SIZE);
    uint16_t dst_idx = (uint16_t)(target_pal * 16);

    if (src_off + BPP4PALETTE_SIZE <= BUFFER_SIZE && dst_idx + 16 <= 256) {
        memcpy(&ert.palettes[dst_idx], &ert.buffer[src_off], BPP4PALETTE_SIZE);
    }

    frame++;
    if (frame >= total_frames) {
        frame = 0;
    }
    entities.var[0][offset] = frame;

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

int16_t cr_set_state_paused(int16_t ent, int16_t scr,
                                   uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    ert.actionscript_state = 2;
    return 0;
}

int16_t cr_update_palette_anim(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    update_map_palette_animation();
    return 0;
}

int16_t cr_set_state_running(int16_t ent, int16_t scr,
                                    uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    ert.actionscript_state = 1;
    return 0;
}

int16_t cr_finalize_palette_fade(int16_t ent, int16_t scr,
                                        uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
    memcpy(ert.palettes, fade->target, sizeof(fade->target));
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

/*
 * LOAD_FILE_SELECT_PALETTES (C0ED5C)
 *
 * Loads the title screen palette instantly (no fade) for quick mode.
 *
 * ROM flow:
 *   1. Decompress TITLE_SCREEN_PALETTE -> PALETTES (full BG palette)
 *   2. DECOMPRESS_TITLE_DATA(0) -> BUFFER (gradient data E1AE83)
 *   3. Copy BUFFER+$1A0 (32 bytes) -> PALETTES + BPP4PALETTE_SIZE*8 (palette group 8)
 *   4. DECOMPRESS_TITLE_DATA(1) -> BUFFER (gradient data E1AEFD)
 *   5. Copy BUFFER+$260 (32 bytes) -> PALETTES + BPP4PALETTE_SIZE*7 (palette group 7)
 *   6. Set PALETTE_UPLOAD_MODE = FULL
 */
int16_t cr_load_file_select_palettes(int16_t ent, int16_t scr,
                                            uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    ert.palette_upload_mode = PALETTE_UPLOAD_NONE;

    /* Decompress title screen palette directly into ert.palettes */
    size_t comp_size;
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_PAL_LZHAL);
    const uint8_t *comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_PAL_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes, sizeof(ert.palettes));
    }

    /* Decompress gradient data 0 (E1AE83) into ert.buffer,
     * copy one palette frame to palette group 8 (colors 128-143) */
    comp_size = ASSET_SIZE(ASSET_E1AE83_BIN_LZHAL);
    comp_data = ASSET_DATA(ASSET_E1AE83_BIN_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, ert.buffer, BUFFER_SIZE);
    }
    memcpy((uint8_t *)ert.palettes + BPP4PALETTE_SIZE * 8,
           ert.buffer + 0x1A0, BPP4PALETTE_SIZE);

    /* Decompress gradient data 1 (E1AEFD) into ert.buffer,
     * copy one palette frame to palette group 7 (colors 112-127) */
    comp_size = ASSET_SIZE(ASSET_E1AEFD_BIN_LZHAL);
    comp_data = ASSET_DATA(ASSET_E1AEFD_BIN_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, ert.buffer, BUFFER_SIZE);
    }
    memcpy((uint8_t *)ert.palettes + BPP4PALETTE_SIZE * 7,
           ert.buffer + 0x260, BPP4PALETTE_SIZE);

    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

int16_t cr_fill_palettes_white(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    for (int i = 0; i < 256; i++) {
        ert.palettes[i] = 0x7FFF;
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

int16_t cr_fill_palettes_black(int16_t ent, int16_t scr,
                                      uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;
    for (int i = 0; i < 256; i++) {
        ert.palettes[i] = 0x0000;
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

/* ---- Gas station callroutines ---- */

/*
 * LOAD_GAS_STATION_PALETTE (C0F3E8)
 *
 * Decompresses the normal gas station palette into ert.palettes[],
 * sets palette upload mode to BG_ONLY ($18).
 */
int16_t cr_load_gas_station_palette(int16_t ent, int16_t scr,
                                            uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    size_t comp_size;
    comp_size = ASSET_SIZE(ASSET_INTRO_GAS_STATION_PAL_LZHAL);
    const uint8_t *comp_data = ASSET_DATA(ASSET_INTRO_GAS_STATION_PAL_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes, sizeof(ert.palettes));
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}

/*
 * LOAD_GAS_STATION_FLASH_PALETTE (C0F3B2)
 *
 * Decompresses the flash/strobe palette into ert.palettes[],
 * sets palette upload mode to BG_ONLY ($18).
 */
int16_t cr_load_gas_station_flash_palette(int16_t ent, int16_t scr,
                                                  uint16_t pc, uint16_t *out_pc) {
    *out_pc = pc;

    size_t comp_size;
    comp_size = ASSET_SIZE(ASSET_INTRO_GAS_STATION2_PAL_LZHAL);
    const uint8_t *comp_data = ASSET_DATA(ASSET_INTRO_GAS_STATION2_PAL_LZHAL);
    if (comp_data) {
        decomp(comp_data, comp_size, (uint8_t *)ert.palettes, sizeof(ert.palettes));
    }
    ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
    return 0;
}
