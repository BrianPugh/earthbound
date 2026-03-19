#include "game/battle_bg.h"
#include "game/battle.h"
#include "entity/entity.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "data/assets.h"
#include "include/binary.h"
#include "include/constants.h"
#include <string.h>
#include <stdio.h>

/* DISTORT_30FPS — when set, layer 2 distortion runs at 30fps (skip odd frames).
 * Set by LOAD_BATTLE_BG when layer 2 has active distortion styles. */
uint16_t distort_30fps;

/* Per-scanline horizontal offset array for HDMA emulation */
int16_t bg2_scanline_hoffset[BATTLEBG_MAX_SCANLINES];
bool bg2_distortion_active = false;

/* Loaded BG data structs — palette, cycling, and animation config per layer.
 * Assembly: LOADED_BG_DATA_LAYER1/LAYER2 in BSS (ram.asm lines 1511-1514). */
LoadedBGData loaded_bg_data_layer1;
LoadedBGData loaded_bg_data_layer2;

/* Runtime state for the loaded battle background */
static BattleBGState bg_state;

/* Saved palette for cycling */
static uint16_t bg_palette[16];

/* Forward declarations for palette cycling helpers */
static void palette_cycle_rotate(LoadedBGData *data, uint8_t first,
                                 uint16_t count, uint8_t step);
static uint16_t bg_palette_count;

/* Sine table for distortion — matches ROM's SINE_LOOKUP_TABLE (256 signed
 * bytes, peak ±127).  Generated mathematically: round(127 * sin(i*2π/256)).
 * The ROM stores these as .BYTE in asm/data/sine_table.asm. */
const int8_t sine_table[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

void battle_bg_init(void) {
    memset(&bg_state, 0, sizeof(bg_state));
    memset(bg2_scanline_hoffset, 0, sizeof(bg2_scanline_hoffset));
    bg2_distortion_active = false;
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CENTER;
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_CENTER;
    ppu.sprite_y_offset = 0;
    bg_palette_count = 0;
    memset(&loaded_bg_data_layer1, 0, sizeof(loaded_bg_data_layer1));
    memset(&loaded_bg_data_layer2, 0, sizeof(loaded_bg_data_layer2));
}

void battle_bg_load_at(uint16_t bg_id, uint16_t gfx_vram_addr,
                       uint16_t arr_vram_addr, uint8_t cgram_start) {
    battle_bg_init();

    uint16_t gfx_index, pal_index, arr_index;

    if (bg_id == BATTLEBG_FILE_SELECT) {
        /*
         * BG_DATA_TABLE entry 230:
         *   Graphics: $43 (67), Palette: $56 (86), BPP: $04 (4bpp)
         *   No palette cycling, no scrolling, no distortion.
         */
        gfx_index = 67;
        pal_index = 86;
        arr_index = 67;
    } else if (bg_id == BATTLEBG_GAS_STATION) {
        /*
         * BG_DATA_TABLE entry 295:
         *   Graphics: $40 (64), Palette: $53 (83), BPP: $04 (4bpp)
         *   Palette cycling: style=1, first=1, last=15, speed=3
         *   Distortion style 1: $72
         *   No scrolling.
         *
         * Assembly (setup_gas_station_background.asm) calls
         * LOAD_BG_LAYER_CONFIG to fully initialize loaded_bg_data_layer1,
         * then uses GENERATE_BATTLEBG_FRAME for per-frame updates.
         * We replicate that here instead of using the simplified bg_state.
         */
        gfx_index = 64;
        pal_index = 83;
        arr_index = 64;
    } else {
        gfx_index = bg_id;
        pal_index = bg_id;
        arr_index = bg_id;
    }

    /* Load graphics — decompress directly to ppu.vram (no intermediate buffer) */
    size_t gfx_compressed_size = ASSET_SIZE(ASSET_BATTLE_BGS_GRAPHICS(gfx_index));
    const uint8_t *gfx_compressed = ASSET_DATA(ASSET_BATTLE_BGS_GRAPHICS(gfx_index));
    if (gfx_compressed) {
        uint8_t *vram_dst = &ppu.vram[(uint32_t)gfx_vram_addr * 2];
        decomp(gfx_compressed, gfx_compressed_size, vram_dst, 0x2000);
    }

    /* Load arrangement — decompress directly to ppu.vram, fixup in-place */
    size_t arr_compressed_size = ASSET_SIZE(ASSET_BATTLE_BGS_ARRANGEMENTS(arr_index));
    const uint8_t *arr_compressed = ASSET_DATA(ASSET_BATTLE_BGS_ARRANGEMENTS(arr_index));
    if (arr_compressed) {
        uint8_t *vram_dst = &ppu.vram[(uint32_t)arr_vram_addr * 2];
        size_t arr_size = decomp(arr_compressed, arr_compressed_size,
                                  vram_dst, 0x800);

        if (arr_size > 0) {
            /* 4bpp arrangement fixup: modify each tilemap entry's high byte:
               AND #$DF (clear priority bit)
               ORA #$08 (set palette bit 1 → palette 2 → CGRAM[32-47]) */
            for (size_t i = 1; i < arr_size; i += 2) {
                vram_dst[i] = (vram_dst[i] & 0xDF) | 0x08;
            }
        }
    }

    /* For gas station: call load_bg_layer_config() BEFORE palette loading,
     * matching assembly flow (setup_gas_station_background.asm lines 86-87).
     * This zeros loaded_bg_data_layer1 and copies config fields. */
    if (bg_id == BATTLEBG_GAS_STATION) {
        const uint8_t *bg_data_table = ASSET_DATA(ASSET_DATA_BG_DATA_TABLE_BIN);
        if (bg_data_table) {
            const BGLayerConfigEntry *config =
                (const BGLayerConfigEntry *)&bg_data_table[BATTLEBG_GAS_STATION * 17];
            load_bg_layer_config(&loaded_bg_data_layer1, config);
        }
    }

    /* Load palette to CGRAM and save for cycling */
    size_t pal_size = ASSET_SIZE(ASSET_BATTLE_BGS_PALETTES(pal_index));
    const uint8_t *pal_data = ASSET_DATA(ASSET_BATTLE_BGS_PALETTES(pal_index));
    if (pal_data) {
        uint16_t pal_colors = (uint16_t)(pal_size / 2);
        if (pal_colors > 16) pal_colors = 16;

        ppu_cgram_dma(pal_data, cgram_start, pal_colors * 2);
        memcpy(&ert.palettes[cgram_start], pal_data, pal_colors * 2);

        memcpy(bg_palette, pal_data, pal_colors * 2);
        bg_palette_count = pal_colors;

        /* Populate loaded_bg_data_layer1 palette fields.
         * Assembly (setup_gas_station_background.asm lines 93-138):
         *   palette_pointer = PALETTES + BPP4PALETTE_SIZE * 2
         *   memcpy palette from ROM → loaded_bg_data.palette[16]
         *   memcpy palette from ROM → loaded_bg_data.palette2[16] (backup) */
        memcpy(loaded_bg_data_layer1.palette, pal_data, pal_colors * 2);
        memcpy(loaded_bg_data_layer1.palette2, pal_data, pal_colors * 2);
        loaded_bg_data_layer1.palette_index = cgram_start;
        if (bg_id != BATTLEBG_GAS_STATION) {
            loaded_bg_data_layer1.bitdepth = 4;
        }
    }

    /* Set up BG2 VRAM registers from the provided word addresses */
    ppu.bg_sc[1] = (uint8_t)(arr_vram_addr >> 8);
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0x0F) | (uint8_t)((gfx_vram_addr >> 8) & 0xF0);

    /* For gas station: set target_layer, call generate_battlebg_frame for
     * initial frame, and disable layer 2 — matching assembly lines 140-148. */
    if (bg_id == BATTLEBG_GAS_STATION) {
        loaded_bg_data_layer1.target_layer = 2;  /* assembly line 140-141 */
        memcpy(&ert.palettes[cgram_start], loaded_bg_data_layer1.palette,
               bg_palette_count * 2);
        generate_battlebg_frame(&loaded_bg_data_layer1, 0);  /* assembly line 145 */
        loaded_bg_data_layer2.target_layer = 0;  /* assembly line 147 */
        bg2_distortion_active = true;
    }

    bg_state.active = true;
    if (bg_id != BATTLEBG_GAS_STATION) {
        bg2_distortion_active = (bg_state.dist_type != 0);
    }
}

void battle_bg_load(uint16_t bg_id) {
    /* Default VRAM locations for battle system */
    uint16_t gfx_vram = 0x1000;
    uint16_t arr_vram = 0x5C00;
    uint8_t  cgram = 32;

    if (bg_id == BATTLEBG_FILE_SELECT) {
        /* BPP = 4 → Mode 1 + BG3 priority */
        ppu.bgmode = 0x09;
    }

    battle_bg_load_at(bg_id, gfx_vram, arr_vram, cgram);
}

void battle_bg_update(void) {
    if (!bg_state.active) return;

    /* When loaded_bg_data_layer1 is fully initialized (target_layer != 0),
     * delegate to generate_battlebg_frame() which handles palette cycling,
     * scrolling, and distortion via the proper state machine.
     * This is the path used by the gas station and normal battles. */
    if (loaded_bg_data_layer1.target_layer != 0) {
        generate_battlebg_frame(&loaded_bg_data_layer1, 0);
        return;
    }

    /* --- Simplified path for backgrounds without full config --- */

    /* --- Scrolling ---
     * Assembly (generate_frame.asm 456-466): velocity updated FIRST,
     * then NEW velocity added to position. */
    bg_state.scroll_dx += bg_state.scroll_ddx;
    bg_state.scroll_dy += bg_state.scroll_ddy;
    bg_state.scroll_x += bg_state.scroll_dx;
    bg_state.scroll_y += bg_state.scroll_dy;

    /* Apply scroll to BG2 offset registers (high 16 bits of 16.16 fixed point) */
    ppu.bg_hofs[1] = (uint16_t)(bg_state.scroll_x >> 16);
    ppu.bg_vofs[1] = (uint16_t)(bg_state.scroll_y >> 16);

    /* --- Distortion (per-scanline horizontal offsets) ---
     *
     * Ported from PREPARE_BG_OFFSET_TABLES (prepare_bg_offset_tables.asm).
     *
     * ROM hardware multiply: M7A (16-bit signed) × M7B (8-bit signed) → 24-bit.
     * Reading MPYM:MPYH ($2135-$2136) gives the upper 16 bits = result >> 8.
     *
     * Amplitude setup (generate_frame.asm:812-816):
     *   Load 16-bit ripple_amplitude, XBA, AND #$00FF → extract high byte.
     *   Store to M7A as 16-bit ($00xx), so M7A = amplitude_high_byte.
     *
     * Phase setup: speed (8-bit) stored at accumulator high byte ($03),
     *   low byte ($02) = 0. Each scanline: accumulator += frequency.
     *   Phase index = high byte of accumulator = (speed + n * freq_hi).
     *
     * Sine table: ROM SINE_LOOKUP_TABLE = 256 signed bytes, peak ±127.
     * Multiply result: amp_hi * sine[idx], read as (result >> 8).
     * In C: (int32_t)amp_hi * (int32_t)sine[idx], then >> 8.
     */
    /* Update distortion parameters with per-frame acceleration FIRST.
     * Assembly (generate_frame.asm lines 746-784) applies acceleration BEFORE
     * computing the per-scanline offset table (line 820). */
    if (bg_state.dist_type != 0) {
        bg_state.dist_freq += bg_state.dist_freq_accel;
        bg_state.dist_amplitude += bg_state.dist_amp_accel;
        bg_state.dist_speed += bg_state.dist_speed_accel;  /* 8-bit wrapping */
        bg_state.dist_compression += bg_state.dist_comp_accel;
    }

    /* Assembly subtracts 1 from dist_type before dispatch (generate_frame.asm:793 DEC).
     * dist_type 1 → style 0 = plain horizontal, dist_type 2 → style 1 = paired. */
    int style = bg_state.dist_type - 1;

    if (style >= 0 && style <= 3) {
        uint16_t amp_hi = ((uint16_t)bg_state.dist_amplitude >> 8) & 0xFF;
        uint16_t phase = ((uint16_t)bg_state.dist_speed) << 8;

        if (style == 0) {
            /* Style 0 (dist_type 1 HORIZONTAL_SMOOTH): plain horizontal.
             * One ripple per scanline, no pairing or compression. */
            for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y++) {
                uint8_t idx = (uint8_t)(phase >> 8);
                int16_t ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                bg2_scanline_hoffset[y] = ripple;
                phase += (uint16_t)bg_state.dist_freq;
            }
        } else if (style == 1) {
            /* Style 1 (dist_type 2 HORIZONTAL_INTERLACED): paired even/odd.
             * Even scanline: +ripple, odd scanline: -ripple. */
            for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y += 2) {
                uint8_t idx = (uint8_t)(phase >> 8);
                int16_t ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                bg2_scanline_hoffset[y] = ripple;
                phase += (uint16_t)bg_state.dist_freq;
                if (y + 1 < BATTLEBG_MAX_SCANLINES) {
                    idx = (uint8_t)(phase >> 8);
                    ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                    bg2_scanline_hoffset[y + 1] = -ripple;
                    phase += (uint16_t)bg_state.dist_freq;
                }
            }
        } else if (style == 2) {
            /* Style 2 (dist_type 3 VERTICAL_SMOOTH): compression accumulator.
             * Assembly increments comp_accum BEFORE extracting high byte. */
            int16_t comp_accum = 0;
            for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y++) {
                comp_accum += bg_state.dist_compression;
                uint8_t idx = (uint8_t)(phase >> 8);
                int16_t ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                int16_t comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
                bg2_scanline_hoffset[y] = comp_part + ripple;
                phase += (uint16_t)bg_state.dist_freq;
            }
        }
        else if (style == 3) {
            /* Style 3 (dist_type 4 BIDIR_COMPRESS): compression + paired.
             * Combines style 2 (compression accumulator per scanline) with
             * style 1 (paired even/odd: even adds sine, odd subtracts sine).
             * Compression accumulator advances for BOTH even and odd scanlines.
             * Port of @BIDIR_COMPRESS_LOOP in prepare_bg_offset_tables.asm. */
            int16_t comp_accum = 0;
            for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y += 2) {
                /* Even scanline: compress + sine */
                uint8_t idx = (uint8_t)(phase >> 8);
                int16_t ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                comp_accum += bg_state.dist_compression;
                int16_t comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
                bg2_scanline_hoffset[y] = comp_part + ripple;
                phase += (uint16_t)bg_state.dist_freq;

                if (y + 1 < BATTLEBG_MAX_SCANLINES) {
                    /* Odd scanline: compress - sine */
                    idx = (uint8_t)(phase >> 8);
                    ripple = (int16_t)(((int32_t)amp_hi * (int32_t)sine_table[idx]) >> 8);
                    comp_accum += bg_state.dist_compression;
                    comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
                    bg2_scanline_hoffset[y + 1] = comp_part - ripple;
                    phase += (uint16_t)bg_state.dist_freq;
                }
            }
        }
    }
}

/*
 * LOAD_BG_LAYER_CONFIG — Port of asm/battle/load_bg_layer_config.asm (101 lines).
 *
 * Zeroes the entire loaded_bg_data struct, then copies config fields from a
 * bg_layer_config_entry source (loaded from BG_DATA_TABLE).
 *
 * Fields copied: bitdepth, palette_shifting_style, palette_cycle_1_first/last,
 * palette_cycle_2_first/last, palette_change_speed, scrolling_movements[4],
 * distortion_styles[4].
 *
 * Initializes: scrolling_duration_left=1, distortion_duration_left=1,
 * palette_change_duration_left=1.
 */
void load_bg_layer_config(LoadedBGData *target, const BGLayerConfigEntry *config) {
    /* Assembly: MEMSET16 zeroes sizeof(loaded_bg_data) bytes */
    memset(target, 0, sizeof(LoadedBGData));

    /* Copy individual config fields (assembly lines 26-53, 8-bit accum) */
    target->bitdepth = config->bitdepth;
    target->palette_shifting_style = config->palette_shifting_style;
    target->palette_cycle_1_first = config->palette_cycle_1_first;
    target->palette_cycle_1_last = config->palette_cycle_1_last;
    target->palette_cycle_2_first = config->palette_cycle_2_first;
    target->palette_cycle_2_last = config->palette_cycle_2_last;
    target->palette_change_speed = config->palette_change_speed;

    /* Copy scrolling_movements[4] (assembly lines 55-76, MEMCPY16) */
    memcpy(target->scrolling_movements, config->scrolling_movements, 4);

    /* Copy distortion_styles[4] (assembly lines 77-92, MEMCPY16) */
    memcpy(target->distortion_styles, config->distortion_styles, 4);

    /* Initialize duration counters to 1 (assembly lines 93-98) */
    target->scrolling_duration_left = 1;
    target->distortion_duration_left = 1;
    target->palette_change_duration_left = 1;
}

/*
 * INTERPOLATE_BG_PALETTE_COLOR — Port of asm/battle/effects/interpolate_bg_palette_color.asm (191 lines).
 *
 * Adjusts a single palette entry in a loaded_bg_data struct.
 *
 * Special brightness values:
 *   0xFFFF — set to white (0xFFFF)
 *   0x0000 — set to black (0x0000)
 *   0x0100 — restore from backup (palette2 → palette)
 *   other  — interpolate: for each R/G/B channel of palette2[index],
 *            compute (channel * brightness) >> 8 and reassemble
 *
 * After computing the new color, stores it to both data->palette[index]
 * and ert.palettes[data->palette_index + index] (the global CGRAM mirror),
 * UNLESS the index falls within an active palette cycling range (to avoid
 * clobbering animated colors).
 */
void interpolate_bg_palette_color(LoadedBGData *data, uint16_t index,
                                  uint16_t brightness) {
    uint16_t new_color;

    if (brightness == 0xFFFF || brightness == 0) {
        /* Direct set: white or black */
        new_color = brightness;
        data->palette[index] = new_color;
        ert.palettes[data->palette_index + index] = new_color;
        return;
    }

    if (brightness == 0x0100) {
        /* Restore from backup */
        new_color = data->palette2[index];
        data->palette[index] = new_color;
        ert.palettes[data->palette_index + index] = new_color;
        return;
    }

    /* Interpolate: decompose palette2 color into R/G/B, multiply by brightness/256.
     * SNES 15-bit color: 0bBBBBBGGGGGRRRRR */
    uint16_t original = data->palette2[index];
    uint16_t r = original & 0x001F;
    uint16_t g = (original >> 5) & 0x001F;
    uint16_t b = (original >> 10) & 0x001F;

    uint16_t r_new = (uint16_t)((r * brightness) >> 8);
    uint16_t g_new = (uint16_t)((g * brightness) >> 8);
    uint16_t b_new = (uint16_t)((b * brightness) >> 8);

    new_color = r_new | (g_new << 5) | (b_new << 10);
    data->palette[index] = new_color;

    /* Conditionally copy to global PALETTES array.
     * Skip if index falls within an active palette cycling range
     * (assembly lines 147-173). */
    if (data->palette_shifting_style == 2) {
        if (index >= data->palette_cycle_2_first &&
            index <= data->palette_cycle_2_last)
            return;
    }
    if (data->palette_shifting_style != 0) {
        if (index >= data->palette_cycle_1_first &&
            index <= data->palette_cycle_1_last)
            return;
    }
    ert.palettes[data->palette_index + index] = new_color;
}

/*
 * INTERPOLATE_BG_PALETTE_COLORS — Port of asm/battle/effects/interpolate_bg_palette_colors.asm (46 lines).
 *
 * Adjusts all palette entries for the loaded background layer(s).
 *
 * For 4bpp mode (bitdepth == 4): interpolates colors 1-15 on layer 1 only.
 * For 2bpp mode: interpolates colors 1-3 on BOTH layer 1 and layer 2.
 *
 * Color 0 (transparent) is always skipped (loop starts at index 1).
 */
void interpolate_bg_palette_colors(uint16_t brightness) {
    if ((loaded_bg_data_layer1.bitdepth & 0xFF) == 4) {
        /* 4bpp: 16-color palette on layer 1 only */
        for (int i = 1; i < 16; i++)
            interpolate_bg_palette_color(&loaded_bg_data_layer1, i, brightness);
    } else {
        /* 2bpp: 4-color ert.palettes on both layers */
        for (int i = 1; i < 4; i++) {
            interpolate_bg_palette_color(&loaded_bg_data_layer1, i, brightness);
            interpolate_bg_palette_color(&loaded_bg_data_layer2, i, brightness);
        }
    }
}

/*
 * DARKEN_BG_PALETTES — Port of asm/battle/effects/darken_bg_palettes.asm (52 lines).
 *
 * For each of the 16 palette entries in BOTH layers:
 *   LSR (shift right 1) + AND $3DEF  →  halves each 5-bit RGB channel.
 *
 * Then copies palette[] → global ert.palettes[] via palette_index.
 * Skips layer 2's global copy if target_layer == 0.
 */
void darken_bg_palettes(void) {
    /* Darken all 16 entries in both layers */
    for (int i = 0; i < 16; i++) {
        loaded_bg_data_layer1.palette[i] =
            (loaded_bg_data_layer1.palette[i] >> 1) & 0x3DEF;
        loaded_bg_data_layer2.palette[i] =
            (loaded_bg_data_layer2.palette[i] >> 1) & 0x3DEF;
    }

    /* Copy layer 1 palette to global PALETTES array */
    memcpy(&ert.palettes[loaded_bg_data_layer1.palette_index],
           loaded_bg_data_layer1.palette,
           sizeof(loaded_bg_data_layer1.palette));

    /* Copy layer 2 palette only if layer 2 is active */
    if (loaded_bg_data_layer2.target_layer != 0) {
        memcpy(&ert.palettes[loaded_bg_data_layer2.palette_index],
               loaded_bg_data_layer2.palette,
               sizeof(loaded_bg_data_layer2.palette));
    }
}

/*
 * ROTATE_BG_DISTORTION — Port of asm/battle/rotate_bg_distortion.asm (22 lines).
 *
 * Cycles the distortion style entries in loaded_bg_data_layer1:
 *   - Swaps distortion_styles[0] and distortion_styles[3]
 *   - Zeros distortion_styles[1]
 *   - Resets distortion_duration_left to 1
 *
 * Used during Giygas death sequence to cycle through distortion effects.
 */
void rotate_bg_distortion(void) {
    uint8_t temp = loaded_bg_data_layer1.distortion_styles[0];
    loaded_bg_data_layer1.distortion_styles[0] = loaded_bg_data_layer1.distortion_styles[3];
    loaded_bg_data_layer1.distortion_styles[1] = 0;
    loaded_bg_data_layer1.distortion_duration_left = 1;
    loaded_bg_data_layer1.distortion_styles[3] = temp;
}

/*
 * RESTORE_BG_PALETTE_BACKUPS — Port of asm/battle/effects/restore_bg_palette_backups.asm (56 lines).
 *
 * Copies palette2[] (backup) → palette[] for both layers,
 * then copies palette[] → global ert.palettes[] via palette_index.
 * Skips layer 2's global copy if target_layer == 0.
 *
 * Note: the assembly has different register allocation between USA and JPN
 * builds, but the logical operation is identical.
 */
void restore_bg_palette_backups(void) {
    /* Restore palette from backup for both layers */
    memcpy(loaded_bg_data_layer1.palette, loaded_bg_data_layer1.palette2,
           sizeof(loaded_bg_data_layer1.palette));
    memcpy(loaded_bg_data_layer2.palette, loaded_bg_data_layer2.palette2,
           sizeof(loaded_bg_data_layer2.palette));

    /* Copy layer 1 palette to global PALETTES array */
    memcpy(&ert.palettes[loaded_bg_data_layer1.palette_index],
           loaded_bg_data_layer1.palette,
           sizeof(loaded_bg_data_layer1.palette));

    /* Copy layer 2 palette only if layer 2 is active */
    if (loaded_bg_data_layer2.target_layer != 0) {
        memcpy(&ert.palettes[loaded_bg_data_layer2.palette_index],
               loaded_bg_data_layer2.palette,
               sizeof(loaded_bg_data_layer2.palette));
    }
}

/*
 * Palette cycling helper for styles 1 & 2 (circular rotation).
 * Rotates colors in palette[first..first+count-1] by `step` positions,
 * writing the remapped colors to ert.palettes[palette_index + first..].
 */
static void palette_cycle_rotate(LoadedBGData *data, uint8_t first,
                                 uint16_t count, uint8_t step) {
    for (uint16_t i = 0; i < count; i++) {
        uint16_t source;
        if (i < step) {
            source = i + count - step;
        } else {
            source = i - step;
        }
        ert.palettes[data->palette_index + first + i] =
            data->palette[first + source];
    }
}

/*
 * Palette cycling helper for style 3 (bidirectional / ping-pong).
 * Colors ping-pong through the range using `step` as the offset.
 */
static void palette_cycle_pingpong(LoadedBGData *data, uint8_t first,
                                   uint16_t count, uint8_t step) {
    uint16_t double_count = count * 2;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t target = i + step;
        if (target >= double_count) {
            target -= double_count;
        }
        if (target >= count) {
            target = double_count - 1 - target;
        }
        ert.palettes[data->palette_index + first + i] =
            data->palette[first + target];
    }
}

/*
 * GENERATE_BATTLEBG_FRAME — Port of asm/misc/battlebgs/generate_frame.asm.
 *
 * Per-layer animation state machine. Handles:
 *   1. Palette shifting (3 styles of color cycling)
 *   2. Scrolling state machine (cycles through 4 movement slots)
 *   3. Position update (velocity += accel, position += velocity)
 *   4. Apply scroll to target BG layer registers
 *   5. Distortion state machine (cycles through 4 distortion style slots)
 *   6. Distortion parameter acceleration
 *   7. Per-scanline HDMA offset computation
 *
 * layer_index: 0 = layer 1, 1 = layer 2.
 */
void generate_battlebg_frame(LoadedBGData *data, int layer_index) {
    uint16_t target_layer = data->target_layer & 0xFF;

    /* ===== Palette shifting (assembly lines 23-341) ===== */
    if (!data->freeze_palette_scrolling) {
        if (data->palette_change_duration_left != 0) {
            data->palette_change_duration_left--;
            if (data->palette_change_duration_left == 0) {
                /* Reload timer */
                data->palette_change_duration_left = data->palette_change_speed;

                uint8_t style = data->palette_shifting_style;
                if (style == 2) {
                    /* Style 2: cycle 2 first, then fall through to cycle 1 */
                    uint16_t count2 = (data->palette_cycle_2_last -
                                       data->palette_cycle_2_first) + 1;
                    palette_cycle_rotate(data, data->palette_cycle_2_first,
                                         count2, data->palette_cycle_2_step);
                    data->palette_cycle_2_step++;
                    if (data->palette_cycle_2_step >= count2)
                        data->palette_cycle_2_step = 0;
                    /* Fall through to cycle 1 */
                }
                if (style == 1 || style == 2) {
                    /* Style 1 (or style 2 fall-through): circular rotation */
                    uint16_t count1 = (data->palette_cycle_1_last -
                                       data->palette_cycle_1_first) + 1;
                    palette_cycle_rotate(data, data->palette_cycle_1_first,
                                         count1, data->palette_cycle_1_step);
                    data->palette_cycle_1_step++;
                    if (data->palette_cycle_1_step >= count1)
                        data->palette_cycle_1_step = 0;
                } else if (style == 3) {
                    /* Style 3: bidirectional ping-pong */
                    uint16_t count1 = (data->palette_cycle_1_last -
                                       data->palette_cycle_1_first) + 1;
                    palette_cycle_pingpong(data, data->palette_cycle_1_first,
                                           count1, data->palette_cycle_1_step);
                    data->palette_cycle_1_step++;
                    if (data->palette_cycle_1_step >= count1 * 2)
                        data->palette_cycle_1_step = 0;
                }

                ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            }
        }
    }

    /* ===== Giygas defeated check (assembly line 343) ===== */
    if (bt.giygas_phase == GIYGAS_DEFEATED)
        return;

    /* ===== Scrolling state machine (assembly lines 346-445) ===== */
    if (data->scrolling_duration_left != 0) {
        data->scrolling_duration_left--;
        if (data->scrolling_duration_left == 0) {
            /* Advance to next scrolling movement slot (AND 3 = wrap) */
            data->current_scrolling_movement =
                (data->current_scrolling_movement + 1) & 3;

            uint8_t scroll_idx =
                data->scrolling_movements[data->current_scrolling_movement];

            /* If the new slot is 0, wrap back to slot 0 */
            if (scroll_idx == 0) {
                data->current_scrolling_movement = 0;
                scroll_idx = data->scrolling_movements[0];
            }

            if (scroll_idx != 0) {
                /* Load scrolling entry from BG_SCROLLING_TABLE */
                static const uint8_t *scrolling_table = NULL;
                if (!scrolling_table) {
                    scrolling_table = ASSET_DATA(
                        ASSET_DATA_BG_SCROLLING_TABLE_BIN);
                }
                if (scrolling_table) {
                    const uint8_t *entry =
                        scrolling_table + (uint16_t)scroll_idx * 10;
                    data->scrolling_duration_left =
                        read_u16_le(&entry[0]);
                    data->horizontal_velocity =
                        (int16_t)read_u16_le(&entry[2]);
                    data->vertical_velocity =
                        (int16_t)read_u16_le(&entry[4]);
                    data->horizontal_acceleration =
                        (int16_t)read_u16_le(&entry[6]);
                    data->vertical_acceleration =
                        (int16_t)read_u16_le(&entry[8]);
                }
            }
        }
    }

    /* ===== Position update (assembly lines 446-486) ===== */
    data->horizontal_velocity += data->horizontal_acceleration;
    data->horizontal_position += data->horizontal_velocity;
    data->vertical_velocity += data->vertical_acceleration;
    data->vertical_position += data->vertical_velocity;

    /* ===== Apply to target BG layer (assembly lines 487-556) =====
     * XBA + AND #$00FF extracts the high byte (position >> 8).
     * Then add screen_effect offset. */
    if (target_layer >= 1 && target_layer <= 4) {
        int bg_idx = target_layer - 1;
        ppu.bg_hofs[bg_idx] = (uint16_t)(
            ((data->horizontal_position >> 8) & 0xFF) +
            bt.screen_effect_horizontal_offset);
        ppu.bg_vofs[bg_idx] = (uint16_t)(
            ((data->vertical_position >> 8) & 0xFF) +
            bt.screen_effect_vertical_offset);
    }

    /* ===== Distortion state machine (assembly lines 557-714) ===== */
    if (data->distortion_duration_left != 0) {
        data->distortion_duration_left--;
        if (data->distortion_duration_left == 0) {
            /* Advance to next distortion style slot */
            data->current_distortion_style_index =
                (data->current_distortion_style_index + 1) & 3;

            uint8_t dist_idx =
                data->distortion_styles[data->current_distortion_style_index];

            if (dist_idx == 0) {
                data->current_distortion_style_index = 0;
                dist_idx = data->distortion_styles[0];
            }

            if (dist_idx != 0) {
                static const uint8_t *distortion_table = NULL;
                if (!distortion_table) {
                    distortion_table = ASSET_DATA(
                        ASSET_DATA_BG_DISTORTION_TABLE_BIN);
                }
                if (distortion_table) {
                    const uint8_t *entry =
                        distortion_table + (uint16_t)dist_idx * 17;
                    data->distortion_duration_left =
                        read_u16_le(&entry[0]);
                    data->distortion_type = entry[2];
                    data->distortion_ripple_frequency =
                        (int16_t)read_u16_le(&entry[3]);
                    data->distortion_ripple_amplitude =
                        (int16_t)read_u16_le(&entry[5]);
                    data->distortion_speed = entry[7];
                    data->distortion_compression_rate =
                        (int16_t)read_u16_le(&entry[8]);
                    data->distortion_freq_accel =
                        (int16_t)read_u16_le(&entry[10]);
                    data->distortion_amp_accel =
                        (int16_t)read_u16_le(&entry[12]);
                    data->distortion_speed_accel = (int8_t)entry[14];
                    data->distortion_comp_accel =
                        (int16_t)read_u16_le(&entry[15]);
                }
            }
        }
    }

    /* ===== Distortion acceleration (assembly lines 737-785) ===== */
    if ((data->distortion_type & 0xFF) == 0)
        return;

    data->distortion_ripple_frequency += data->distortion_freq_accel;
    data->distortion_ripple_amplitude += data->distortion_amp_accel;
    data->distortion_speed += data->distortion_speed_accel;
    data->distortion_compression_rate += data->distortion_comp_accel;

    /* ===== 30fps gating (assembly lines 799-805) =====
     * If DISTORT_30FPS is set, skip distortion computation on
     * alternating frames (core.frame_counter & 1 != layer_index). */
    if ((core.frame_counter & 1) != (uint16_t)layer_index) {
        if (distort_30fps)
            return;
    }

    /* ===== Per-scanline offset computation (assembly lines 806-820) =====
     * Mirrors PREPARE_BG_OFFSET_TABLES logic.
     *
     * Phase accumulator: high byte = speed, low byte = 0.
     * Each scanline: phase += frequency. Index = phase >> 8.
     * Offset = (amp_hi * sine_table[index]) >> 8.
     *
     * For styles with target_layer > 0, the BG scroll position is
     * folded into the base offset and phase. */
    uint8_t dist_style = (data->distortion_type & 0xFF) - 1;
    uint16_t amp_hi = ((uint16_t)data->distortion_ripple_amplitude >> 8) & 0xFF;
    uint16_t phase = ((uint16_t)data->distortion_speed) << 8;
    int16_t freq = data->distortion_ripple_frequency;

    /* Adjust phase and base offset from target BG layer scroll position */
    int16_t base_hofs = 0;
    if (target_layer >= 1 && target_layer <= 4) {
        int bg_idx = target_layer - 1;
        if (dist_style < 2) {
            /* Horizontal styles: add LOW byte of BG vofs to phase, use BG hofs as base.
             * Assembly: LDA bg_vofs; XBA; AND #$FF00 → low byte shifted to high byte. */
            phase = (uint16_t)((((phase >> 8) + (ppu.bg_vofs[bg_idx] & 0xFF)) & 0xFF) << 8);
            base_hofs = (int16_t)ppu.bg_hofs[bg_idx];
        }
    }

    int16_t comp_accum = 0;
    if (dist_style >= 2 && target_layer >= 1 && target_layer <= 4) {
        /* Assembly: LDA bg_vofs; XBA; AND #$FF00 — low byte shifted to high byte */
        comp_accum = (int16_t)((ppu.bg_vofs[target_layer - 1] & 0xFF) << 8);
    }
    int16_t comp_rate = data->distortion_compression_rate;

    bg2_distortion_active = true;

    if (dist_style == 0) {
        /* Style 0 (HORIZONTAL_SMOOTH): simple sine ripple */
        for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y++) {
            uint8_t idx = (uint8_t)(phase >> 8);
            int16_t ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                         (int32_t)sine_table[idx]) >> 8);
            bg2_scanline_hoffset[y] = ripple + base_hofs;
            phase += (uint16_t)freq;
        }
    } else if (dist_style == 1) {
        /* Style 1 (HORIZONTAL_INTERLACED): alternating sign */
        for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y += 2) {
            uint8_t idx = (uint8_t)(phase >> 8);
            int16_t ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                         (int32_t)sine_table[idx]) >> 8);
            bg2_scanline_hoffset[y] = ripple + base_hofs;
            phase += (uint16_t)freq;
            if (y + 1 < BATTLEBG_MAX_SCANLINES) {
                idx = (uint8_t)(phase >> 8);
                ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                     (int32_t)sine_table[idx]) >> 8);
                bg2_scanline_hoffset[y + 1] = base_hofs - ripple;
                phase += (uint16_t)freq;
            }
        }
    } else if (dist_style == 2) {
        /* Style 2 (VERTICAL_SMOOTH): compression + sine */
        for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y++) {
            uint8_t idx = (uint8_t)(phase >> 8);
            int16_t ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                         (int32_t)sine_table[idx]) >> 8);
            comp_accum += comp_rate;
            int16_t comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
            bg2_scanline_hoffset[y] = comp_part + ripple;
            phase += (uint16_t)freq;
        }
    } else if (dist_style == 3) {
        /* Style 3: compression + alternating sign */
        for (int y = 0; y < BATTLEBG_MAX_SCANLINES; y += 2) {
            uint8_t idx = (uint8_t)(phase >> 8);
            int16_t ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                         (int32_t)sine_table[idx]) >> 8);
            comp_accum += comp_rate;
            int16_t comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
            bg2_scanline_hoffset[y] = comp_part + ripple;
            phase += (uint16_t)freq;
            if (y + 1 < BATTLEBG_MAX_SCANLINES) {
                idx = (uint8_t)(phase >> 8);
                ripple = (int16_t)(((int32_t)(int16_t)amp_hi *
                                     (int32_t)sine_table[idx]) >> 8);
                comp_accum += comp_rate;
                comp_part = (int16_t)((comp_accum >> 8) & 0xFF);
                bg2_scanline_hoffset[y + 1] = comp_part - ripple;
                phase += (uint16_t)freq;
            }
        }
    }
}
