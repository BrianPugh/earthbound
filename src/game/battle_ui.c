/*
 * Battle UI and sprite functions.
 *
 * Extracted from battle.c — sprite rendering, palette effects,
 * letterbox HDMA, scene loading, battle sprite management,
 * and screen effect updates.
 */
#include "game/battle.h"
#include "game/battle_internal.h"
#include "game/game_state.h"
#include "game/audio.h"
#include "game/display_text.h"
#include "game/inventory.h"
#include "game/text.h"
#include "game/window.h"

#include "data/assets.h"
#include "data/battle_text_data.h"
#include "game/fade.h"
#include "game/battle_bg.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "game/oval_window.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "snes/ppu.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "include/binary.h"
#include "include/pad.h"
#include "platform/platform.h"
#include "game_main.h"
#include <string.h>

/* Asset table references loaded from ROM (defined in battle.c) */
extern const uint8_t *btl_entry_ptr_table;
extern const uint8_t *btl_entry_bg_table;
#include "game/map_loader.h"
#include "game/oval_window.h"


/*
 * SETUP_BATTLE_SPRITE_PALETTE_EFFECT (port of C2FB35)
 *
 * Initializes a per-component palette fade for a single battle sprite palette
 * entry.  The entry is in palette group 12 (ert.palettes[192..255]).  For each
 * RGB channel, it computes the number of steps and the delta per step
 * (±1/±32/±0x400 for R/G/B bit positions in 15-bit BGR color).
 *
 * Parameters:
 *   palette_index — index within the battle sprite palette area (0–63)
 *   r, g, b       — target color components (0–31 each)
 */
void setup_battle_sprite_palette_effect(uint16_t palette_index,
                                         uint16_t r, uint16_t g, uint16_t b) {
    /* Set frames countdown for this palette group (4 groups of 16 entries) */
    uint16_t group_idx = palette_index >> 4;
    bt.battle_sprite_palette_effect_frames_left[group_idx] =
        bt.battle_sprite_palette_effect_speed;

    /* Read current color from palette group 12 (battle sprites) */
    uint16_t current_color = ert.palettes[192 + palette_index];
    uint16_t current_r = current_color & 0x1F;
    uint16_t current_g = (current_color >> 5) & 0x1F;
    uint16_t current_b = (current_color >> 10) & 0x1F;

    uint16_t idx = palette_index * 3;

    /* R component (bit position 0, delta = ±1) */
    if (r > current_r) {
        bt.battle_sprite_palette_effect_steps[idx + 0] = (int16_t)(r - current_r);
        bt.battle_sprite_palette_effect_deltas[idx + 0] = 1;
    } else if (r == current_r) {
        bt.battle_sprite_palette_effect_deltas[idx + 0] = 0;
    } else {
        bt.battle_sprite_palette_effect_steps[idx + 0] = (int16_t)(current_r - r);
        bt.battle_sprite_palette_effect_deltas[idx + 0] = -1;
    }

    /* G component (bit position 5, delta = ±32) */
    if (g > current_g) {
        bt.battle_sprite_palette_effect_steps[idx + 1] = (int16_t)(g - current_g);
        bt.battle_sprite_palette_effect_deltas[idx + 1] = 32;
    } else if (g == current_g) {
        bt.battle_sprite_palette_effect_deltas[idx + 1] = 0;
    } else {
        bt.battle_sprite_palette_effect_steps[idx + 1] = (int16_t)(current_g - g);
        bt.battle_sprite_palette_effect_deltas[idx + 1] = -32;
    }

    /* B component (bit position 10, delta = ±0x0400) */
    if (b > current_b) {
        bt.battle_sprite_palette_effect_steps[idx + 2] = (int16_t)(b - current_b);
        bt.battle_sprite_palette_effect_deltas[idx + 2] = 0x0400;
    } else if (b == current_b) {
        bt.battle_sprite_palette_effect_deltas[idx + 2] = 0;
    } else {
        bt.battle_sprite_palette_effect_steps[idx + 2] = (int16_t)(current_b - b);
        bt.battle_sprite_palette_effect_deltas[idx + 2] = (int16_t)0xFC00; /* -0x0400 */
    }

    /* Clear all counters for this entry */
    bt.battle_sprite_palette_effect_counters[idx + 0] = 0;
    bt.battle_sprite_palette_effect_counters[idx + 1] = 0;
    bt.battle_sprite_palette_effect_counters[idx + 2] = 0;
}


/*
 * SET_BATTLE_SPRITE_PALETTE_EFFECT_SPEED (port of C2FAD8)
 *
 * Sets the speed (frames between steps) for the battle sprite palette effect.
 */
void set_battle_sprite_palette_effect_speed(uint16_t speed) {
    bt.battle_sprite_palette_effect_speed = speed;
}


/*
 * REVERSE_BATTLE_SPRITE_PALETTE_EFFECT (asm/battle/effects/reverse_battle_sprite_palette_effect.asm)
 *
 * Reverses the palette fade direction for a given palette group.
 * Negates all deltas and clears all counters for the group.
 * frames: number of frames for the reversed effect.
 * palette_group: which group to reverse (0-3).
 */
void reverse_battle_sprite_palette_effect(uint16_t frames, uint16_t palette_group) {
    bt.battle_sprite_palette_effect_speed = frames;
    bt.battle_sprite_palette_effect_frames_left[palette_group] = frames;

    uint16_t base = palette_group * 48;
    for (uint16_t i = 0; i < 48; i++) {
        uint16_t idx = base + i;
        bt.battle_sprite_palette_effect_deltas[idx] = -bt.battle_sprite_palette_effect_deltas[idx];
        bt.battle_sprite_palette_effect_counters[idx] = 0;
    }
}


/*
 * UPDATE_BATTLE_SPRITE_PALETTE_ANIM (asm/battle/effects/update_battle_sprite_palette_anim.asm)
 *
 * Runtime tick for the battle sprite palette effect system.
 * For each of the 4 palette groups with frames remaining:
 *   Decrements frame counter, then for each of the 15 non-transparent colors,
 *   processes 3 components (R/G/B). Each component uses a Bresenham-style
 *   accumulator: counter += step each frame; whenever counter >= speed,
 *   the pre-shifted delta is applied to the palette word and speed is
 *   subtracted from the counter.
 * After processing, triggers OBJ palette upload.
 */
void update_battle_sprite_palette_anim(void) {
    for (int group = 0; group < 4; group++) {
        if (bt.battle_sprite_palette_effect_frames_left[group] == 0)
            continue;
        bt.battle_sprite_palette_effect_frames_left[group]--;

        uint16_t base = group * 48; /* 16 colors * 3 components */
        uint16_t pal_base = 192 + group * 16; /* palette 12 + group */

        for (int color = 1; color < 16; color++) {
            uint16_t idx = base + color * 3;
            uint16_t pal_idx = pal_base + color;

            for (int comp = 0; comp < 3; comp++) {
                int16_t delta = bt.battle_sprite_palette_effect_deltas[idx + comp];
                if (delta == 0)
                    continue;

                bt.battle_sprite_palette_effect_counters[idx + comp] +=
                    bt.battle_sprite_palette_effect_steps[idx + comp];

                while ((int16_t)bt.battle_sprite_palette_effect_speed <=
                       bt.battle_sprite_palette_effect_counters[idx + comp]) {
                    bt.battle_sprite_palette_effect_counters[idx + comp] -=
                        (int16_t)bt.battle_sprite_palette_effect_speed;
                    ert.palettes[pal_idx] += delta;
                }
            }
        }

        ert.palette_upload_mode = PALETTE_UPLOAD_OBJ_ONLY;
    }
}


/*
 * RENDER_BATTLE_SPRITE_ROW (asm/battle/render_battle_sprite_row.asm)
 *
 * Iterates through enemy battler slots (FIRST_ENEMY_INDEX..BATTLER_COUNT-1)
 * and renders sprites for enemies in the specified row. Handles:
 *   - blink_timer: damage blink (decrement, hide on odd /3 frames)
 *   - shake_timer: attack animation shake (decrement, use alt spritemap on some frames)
 *   - use_alt_spritemap: persistent alternate appearance
 *   - targeting flash: all non-targeted enemies use alt; targeted enemy blinks
 *
 * row: 0 = front row, 1 = back row.
 */
static void render_battle_sprite_row(uint16_t row) {
    for (int i = FIRST_ENEMY_INDEX; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];

        /* Skip non-renderable battlers */
        if (b->consciousness == 0) continue;
        if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == STATUS_0_UNCONSCIOUS) continue;
        if (b->ally_or_enemy != 1) continue;
        if (b->row != row) continue;
        if (b->sprite == 0) continue;

        /* Blink timer — damage flash effect.
         * Decrement each frame; divide by 3, skip rendering on odd result. */
        if (b->blink_timer != 0) {
            b->blink_timer--;
            if ((b->blink_timer / 3) & 1)
                continue;
        }

        /* Spritemap ert.buffer offset: vram_sprite_index × 80
         * (BATTLE_SPRITEMAP_ENTRIES × BATTLE_SPRITEMAP_ENTRY_BYTES = 16 × 5 = 80) */
        uint16_t smap_offset = b->vram_sprite_index *
            (BATTLE_SPRITEMAP_ENTRIES * BATTLE_SPRITEMAP_ENTRY_BYTES);

        /* Screen position adjusted for screen shake offsets.
         * VIEWPORT_PAD_LEFT centers sprites when BGs fill the wider viewport. */
        int16_t sx = (int16_t)b->sprite_x - bt.screen_effect_horizontal_offset + VIEWPORT_PAD_LEFT;
        int16_t sy = (int16_t)b->sprite_y - bt.screen_effect_vertical_offset;

        /* Shake timer — attack animation bob.
         * Decrement each frame; when (decremented & 4) == 0, use alt spritemap. */
        if (b->shake_timer != 0) {
            b->shake_timer--;
            if ((b->shake_timer & 4) == 0) {
                write_spritemap_to_oam(bt.alt_battle_spritemaps + smap_offset, sx, sy);
                continue;
            }
        }

        /* Persistent alt spritemap (e.g. different animation frame) */
        if (b->use_alt_spritemap) {
            write_spritemap_to_oam(bt.alt_battle_spritemaps + smap_offset, sx, sy);
            continue;
        }

        /* Targeting flash effect */
        if (bt.enemy_targetting_flashing) {
            if (b->is_flash_target == 0) {
                /* Non-targeted enemies: always draw with alt spritemap */
                write_spritemap_to_oam(bt.alt_battle_spritemaps + smap_offset, sx, sy);
                continue;
            }
            /* Targeted enemy: blink between normal and alt every 8 frames */
            if (core.frame_counter & 0x08) {
                write_spritemap_to_oam(bt.alt_battle_spritemaps + smap_offset, sx, sy);
                continue;
            }
        }

        /* Normal rendering — draw from BATTLE_SPRITEMAPS */
        write_spritemap_to_oam(bt.battle_spritemaps + smap_offset, sx, sy);
    }
}


/*
 * RENDER_ALL_BATTLE_SPRITES (asm/battle/render_all_battle_sprites.asm)
 *
 * Clears OAM, renders all enemy battle sprites for both rows, and syncs
 * ert.palettes. Called each frame during battle and at battle init.
 */
void render_all_battle_sprites(void) {
    /* SWAP_SPRITEMAP_BANK — assembly sets bank to $7E for BSS reads.
     * In C port, we pass pointers directly so no bank swap needed,
     * but update the variable for consistency. */
    ert.spritemap_bank = 0x7E;

    /* OAM_CLEAR — hide all sprites */
    oam_clear();
    ert.oam_write_index = 0;

    /* Render front row then back row */
    render_battle_sprite_row(0);
    render_battle_sprite_row(1);

    /* UPDATE_SCREEN — in battle context, sprites are written to OAM
     * directly (not via priority queues), so only palette sync is needed. */
    sync_palettes_to_cgram();
}


/*
 * FIND_BATTLE_SPRITE_FOR_ENEMY (asm/battle/enemy/find_battle_sprite_for_enemy.asm)
 *
 * Searches bt.current_battle_sprite_enemy_ids[0..3] for a matching enemy ID.
 * Returns the index (0-3) if found, or 0 if not found.
 */
uint16_t find_battle_sprite_for_enemy(uint16_t enemy_id) {
    for (uint16_t i = 0; i < 4; i++) {
        if (bt.current_battle_sprite_enemy_ids[i] == enemy_id)
            return i;
    }
    return 0;
}


/*
 * GET_BATTLE_SPRITE_WIDTH (asm/battle/get_battle_sprite_width.asm)
 *
 * Returns the width in 8px tile units for a battle sprite given its 1-based ID.
 * Reads the BATTLE_SPRITE_SIZE enum byte from BATTLE_SPRITES_POINTERS table
 * (byte 4 of each 5-byte entry) and maps to tile count:
 *   1(_32X32),3(_32X64) → 4; 2(_64X32),4(_64X64) → 8; 5(_128X64),6(_128X128) → 16
 */
uint16_t get_battle_sprite_width(uint16_t sprite_id) {
    if (battle_sprites_pointers_data == NULL || sprite_id == 0) return 0;
    uint8_t size = battle_sprites_pointers_data[(sprite_id - 1) * BATTLE_SPRITES_ENTRY_SIZE + 4];
    switch (size) {
        case 1: case 3: return 4;
        case 2: case 4: return 8;
        case 5: case 6: return 16;
        default: return 0;
    }
}


/*
 * GET_BATTLE_SPRITE_HEIGHT (asm/battle/get_battle_sprite_height.asm)
 *
 * Returns the height in 8px tile units for a battle sprite given its 1-based ID.
 *   1(_32X32),2(_64X32) → 4; 3(_32X64),4(_64X64),5(_128X64) → 8; 6(_128X128) → 16
 */
uint16_t get_battle_sprite_height(uint16_t sprite_id) {
    if (battle_sprites_pointers_data == NULL || sprite_id == 0) return 0;
    uint8_t size = battle_sprites_pointers_data[(sprite_id - 1) * BATTLE_SPRITES_ENTRY_SIZE + 4];
    switch (size) {
        case 1: case 2: return 4;
        case 3: case 4: case 5: return 8;
        case 6: return 16;
        default: return 0;
    }
}


/*
 * CALCULATE_BATTLER_ROW_WIDTH (asm/battle/calculate_battler_row_width.asm)
 *
 * Sums the widths (in tile units) of all conscious enemies in the back row
 * (battler slots 8 through BATTLER_COUNT-1). Used for layout calculations.
 */
uint16_t calculate_battler_row_width(void) {
    uint16_t total = 0;
    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 1) {
            total += get_battle_sprite_width(b->sprite);
        }
    }
    return total;
}


/*
 * SORT_BATTLERS_INTO_ROWS (asm/battle/sort_battlers_into_rows.asm)
 *
 * Counts conscious, non-unconscious enemies (slots 8-31) in front/back rows,
 * then selection-sorts them left-to-right by sprite_x into the row arrays.
 * Fills position arrays: x = sprite_x >> 3, y = baseline - sprite_height.
 * Front row baseline = 18, back row baseline = 16.
 */
void sort_battlers_into_rows(void) {
    /* Phase 1: Count battlers in each row */
    bt.num_battlers_in_front_row = 0;
    bt.num_battlers_in_back_row = 0;

    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 0) continue;
        if ((b->afflictions[0] & 0xFF) == 1) continue;  /* UNCONSCIOUS */
        if ((b->ally_or_enemy & 0xFF) != 1) continue;   /* not enemy */
        if ((b->row & 0xFF) == 0)
            bt.num_battlers_in_front_row++;
        else
            bt.num_battlers_in_back_row++;
    }

    /* Phase 2: Selection-sort front row by sprite_x (ascending) */
    uint16_t last_x = 0;
    for (uint16_t out = 0; out < bt.num_battlers_in_front_row; out++) {
        uint16_t min_x = 0xFFFF;
        uint16_t best = 0;
        for (uint16_t j = 8; j < BATTLER_COUNT; j++) {
            Battler *b = &bt.battlers_table[j];
            if ((b->consciousness & 0xFF) == 0) continue;
            if ((b->afflictions[0] & 0xFF) == 1) continue;
            if ((b->ally_or_enemy & 0xFF) != 1) continue;
            if ((b->row & 0xFF) != 0) continue;
            uint16_t sx = b->sprite_x & 0xFF;
            if (sx <= last_x) continue;  /* BLTEQ: skip if <= previous */
            if (sx > min_x) continue;    /* BGT: skip if > current min */
            best = j;
            min_x = sx;
        }
        bt.front_row_battlers[out] = (uint8_t)best;
        bt.battler_front_row_x_positions[out] = (uint8_t)(min_x >> 3);
        uint16_t height = get_battle_sprite_height(bt.battlers_table[best].sprite);
        bt.battler_front_row_y_positions[out] = (uint8_t)(18 - height);
        last_x = min_x;
    }

    /* Phase 3: Selection-sort back row by sprite_x (ascending) */
    last_x = 0;
    for (uint16_t out = 0; out < bt.num_battlers_in_back_row; out++) {
        uint16_t min_x = 0xFFFF;
        uint16_t best = 0;
        for (uint16_t j = 8; j < BATTLER_COUNT; j++) {
            Battler *b = &bt.battlers_table[j];
            if ((b->consciousness & 0xFF) == 0) continue;
            if ((b->afflictions[0] & 0xFF) == 1) continue;
            if ((b->ally_or_enemy & 0xFF) != 1) continue;
            if ((b->row & 0xFF) == 0) continue;  /* skip front row */
            uint16_t sx = b->sprite_x & 0xFF;
            if (sx <= last_x) continue;
            if (sx > min_x) continue;
            best = j;
            min_x = sx;
        }
        bt.back_row_battlers[out] = (uint8_t)best;
        bt.battler_back_row_x_positions[out] = (uint8_t)(min_x >> 3);
        uint16_t height = get_battle_sprite_height(bt.battlers_table[best].sprite);
        bt.battler_back_row_y_positions[out] = (uint8_t)(16 - height);
        last_x = min_x;
    }
}


/*
 * GET_BATTLER_ROW_X_POSITION (asm/battle/ui/get_battler_row_x_position.asm)
 *
 * Returns the x position for a battler at the given index in the specified row.
 * row: 0 = front row, nonzero = back row.
 * index: position within the row array.
 */
uint16_t get_battler_row_x_position(uint16_t row, uint16_t index) {
    if (row == 0)
        return bt.battler_front_row_x_positions[index] & 0xFF;
    return bt.battler_back_row_x_positions[index] & 0xFF;
}


/*
 * CLAMP_ENEMIES_TO_SCREEN_WIDTH (asm/battle/clamp_enemies_to_screen_width.asm)
 *
 * Trims bt.enemies_in_battle count so that the total sprite width
 * of all enemies doesn't exceed 32 tiles (256 pixels).
 * Iterates through enemy slots, accumulating widths; when total
 * exceeds 32, truncates the count.
 */
void clamp_enemies_to_screen_width(void) {
    uint16_t total_width = 0;
    uint16_t i = 0;
    while (i < bt.enemies_in_battle) {
        uint16_t eid = bt.enemies_in_battle_ids[i];
        /* Look up battle_sprite from enemy config table */
        uint16_t sprite_id = 0;
        if (enemy_config_table != NULL)
            sprite_id = enemy_config_table[eid].battle_sprite;
        uint16_t width = get_battle_sprite_width(sprite_id);
        total_width += width;
        if (total_width > 32) {
            bt.enemies_in_battle = i;
            return;
        }
        i++;
    }
}


/* ======================================================================
 * Enemy flashing
 * ====================================================================== */

/*
 * ENEMY_FLASHING_OFF (asm/battle/enemy_flashing_off.asm)
 *
 * Turn off the currently flashing enemy sprite (used during target selection).
 * Clears the flashing state and redraws windows.
 */
void enemy_flashing_off(void) {
    if (bt.current_flashing_enemy == -1)
        return;

    uint16_t battler_index;
    if (bt.current_flashing_enemy_row != 0) {
        /* Back row */
        battler_index = bt.back_row_battlers[bt.current_flashing_enemy];
    } else {
        /* Front row */
        battler_index = bt.front_row_battlers[bt.current_flashing_enemy];
    }

    Battler *b = &bt.battlers_table[battler_index];
    b->is_flash_target = 0;

    bt.enemy_targetting_flashing = 0;
    bt.current_flashing_enemy = -1;
    ow.redraw_all_windows = 1;
}


/*
 * ENEMY_FLASHING_ON (asm/battle/enemy_flashing_on.asm)
 *
 * Turn on enemy targeting flash for the specified enemy index and row.
 * First turns off any existing flash. Sets battler::is_flash_target = 1 for
 * the target, enables ENEMY_TARGETTING_FLASHING, and redraws windows.
 *
 * row: 0 = front, nonzero = back.
 * enemy: index into bt.front_row_battlers or bt.back_row_battlers.
 */
void enemy_flashing_on(uint16_t row, uint16_t enemy) {
    if (bt.current_flashing_enemy != -1)
        enemy_flashing_off();

    bt.current_flashing_enemy = (int16_t)enemy;
    bt.current_flashing_enemy_row = row;

    uint16_t battler_index;
    if (row != 0) {
        battler_index = bt.back_row_battlers[enemy] & 0xFF;
    } else {
        battler_index = bt.front_row_battlers[enemy] & 0xFF;
    }

    bt.battlers_table[battler_index].is_flash_target = 1;
    bt.enemy_targetting_flashing = 1;
    ow.redraw_all_windows = 1;
}


/*
 * FORCE_BLANK_AND_WAIT_VBLANK (asm/overworld/force_blank_and_wait_vblank.asm)
 *
 * Force-blanks the screen (INIDISP = 0x80), disables HDMA,
 * cancels any active fade, then waits for one VBlank.
 * After VBlank, writes 0 to HDMAEN hardware register.
 */
void force_blank_and_wait_vblank(void) {
    ppu.inidisp = 0x80;
    ppu.window_hdma_active = false;
    bg2_distortion_active = false;
    /* Cancel any active fade (assembly: STZ FADE_PARAMETERS::step) */
    fade_out(0, 0);
    render_frame_tick();
}


/* COLOR_MATH_REGISTER_TABLE (asm/system/palette/color_math_register_table.asm)
 *   [0..10]  = TM (main screen designation)
 *   [11..20] = TD (sub screen designation)
 *   [21..30] = CGWSEL (color math control)
 *   [31..40] = CGADSUB (color math designation)
 */
static const uint8_t color_math_register_table[] = {
    /* TM */
    0x17, 0x1F, 0x17, 0x17, 0x17, 0x17, 0x15, 0x15, 0x15, 0x15, 0x15,
    /* TD */
    0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x02, 0x02, 0x02, 0x02,
    /* CGWSEL */
    0x00, 0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    /* CGADSUB */
    0x00, 0x00, 0x24, 0x64, 0xA4, 0xE4, 0x21, 0x61, 0xA1, 0xE1,
};

/*
 * SET_COLOR_MATH_FROM_TABLE (asm/system/palette/set_color_math_from_table.asm)
 *
 * Configures PPU color math registers from a preset table.
 * index: layer configuration mode (0-10).
 */
void set_color_math_from_table(uint16_t index) {
    /* TM has 11 entries (0-10), TD/CGWSEL/CGADSUB have 10 entries (0-9).
     * Callers use index 0-7 in practice; clamp to prevent OOB. */
    if (index > 10) return;
    ppu.tm      = color_math_register_table[index];
    ppu.ts      = color_math_register_table[11 + (index <= 9 ? index : 9)];
    ppu.cgwsel  = color_math_register_table[21 + (index <= 9 ? index : 9)];
    ppu.cgadsub = color_math_register_table[31 + (index <= 9 ? index : 9)];
}


/*
 * CLEAR_BATTLE_VISUAL_EFFECTS (asm/battle/effects/clear_battle_visual_effects.asm)
 *
 * Resets all active battle visual effects:
 *  - Clears green/red flash durations
 *  - Resets swirl update timer
 *  - Redraws any blinking HP/PP window to solid state
 *  - Clears fixed color data (COLDATA) to black
 *  - Resets color math to mode 1 (normal rendering)
 */
void clear_battle_visual_effects(void) {
    bt.green_flash_duration = 0;
    bt.red_flash_duration = 0;
    reset_swirl_update_timer();

    if (bt.hp_pp_box_blink_duration != 0) {
        /* Redraw the blinking HP/PP window in solid state */
        draw_and_mark_hppp_window(bt.hp_pp_box_blink_target);
        bt.hp_pp_box_blink_duration = 0;
    }

    /* SET_COLDATA(0, 0, 0) — clear fixed color to black */
    ppu.coldata_r = 0;
    ppu.coldata_g = 0;
    ppu.coldata_b = 0;

    /* Reset color math to mode 1 (all layers on main screen, no color math) */
    set_color_math_from_table(1);
}


/*
 * LOAD_ENEMY_BATTLE_SPRITES (asm/battle/load_enemy_battle_sprites.asm)
 *
 * Sets up PPU registers for the battle screen:
 *   - Mode 1 + BG3 priority
 *   - BG1: tiles $0000, tilemap $5800, 32x32
 *   - BG2: tiles $1000, tilemap $5C00, 32x32
 *   - BG3: tiles $6000, tilemap $7C00, 32x32
 *   - OAM: 16x16/32x32 sprites, name base 1
 *   - Clears BG3 tilemap (0x800 bytes at VRAM word $7C00)
 */
void load_enemy_battle_sprites(void) {
    /* SET_BGMODE(9): mode 1 + BG3 priority bit */
    ppu.bgmode = 0x09;

    /* SET_BG1_VRAM_LOCATION(Y=$0000, X=$5800, A=0) */
    ppu.bg_sc[0] = 0x58;
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | 0x00;
    ppu.bg_hofs[0] = 0;
    ppu.bg_vofs[0] = 0;

    /* SET_BG2_VRAM_LOCATION(Y=$1000, X=$5C00, A=0) */
    ppu.bg_sc[1] = 0x5C;
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0x0F) | 0x10;
    ppu.bg_hofs[1] = 0;
    ppu.bg_vofs[1] = 0;

    /* SET_BG3_VRAM_LOCATION(Y=$6000, X=$7C00, A=0) */
    ppu.bg_sc[2] = 0x7C;
    ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | 0x06;
    ppu.bg_hofs[2] = 0;
    ppu.bg_vofs[2] = 0;

    /* SET_OAM_SIZE($61): size mode 3 (16x16/32x32), name base 1 */
    ppu.obsel = 0x61;

    /* Clear BG3 tilemap: fixed-source DMA of 0x800 bytes at VRAM word $7C00 */
    memset(&ppu.vram[0x7C00 * 2], 0, 0x800);
}


/*
 * UPLOAD_TEXT_TILES_TO_VRAM (asm/system/dma/upload_text_tiles_to_vram.asm)
 *
 * Copies text tile graphics from ert.buffer[] to VRAM at TEXT_LAYER_TILES.
 * The tiles are stored in a scattered layout in ert.buffer[] and need to be
 * copied to their correct VRAM positions.
 *
 * param 0: scattered blocks only (queued DMA via PREPARE_VRAM_COPY)
 * param 1: big block + scattered blocks (queued DMA)
 * param 2: scattered blocks + big block (immediate DMA via TRANSFER_TO_VRAM)
 */
void upload_text_tiles_to_vram(uint16_t param) {
    if (param > 2) return;

    /* param 1 or 2: Copy large block (BUFFER+$2000 → TEXT_LAYER_TILES+$1000) */
    if (param == 1 || param == 2) {
        memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x1000) * 2],
               &ert.buffer[BUF_TEXT_LAYER2_TILES], 0x1800);
    }

    /* All valid params: Copy 6 scattered blocks from ert.buffer to VRAM */
    memcpy(&ppu.vram[VRAM_TEXT_LAYER_TILES * 2],                   &ert.buffer[BUF_TEXT_TILES_BLOCK1], 0x0450);
    memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x0278) * 2],        &ert.buffer[BUF_TEXT_TILES_BLOCK2], 0x0060);
    memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x02F8) * 2],        &ert.buffer[BUF_TEXT_TILES_BLOCK3], 0x00B0);
    memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x0380) * 2],        &ert.buffer[BUF_TEXT_TILES_BLOCK4], 0x00A0);
    memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x0400) * 2],        &ert.buffer[BUF_TEXT_TILES_BLOCK5], 0x0010);
    memcpy(&ppu.vram[(VRAM_TEXT_LAYER_TILES + 0x0480) * 2],        &ert.buffer[BUF_TEXT_TILES_BLOCK6], 0x0010);
}


void build_letterbox_hdma_table(void);
void apply_letterbox_to_ppu(void);

/*
 * LOAD_BATTLE_BG — Port of asm/battle/load_battlebg.asm (725 lines).
 *
 * Loads battle background graphics, arrangement, and palette for up to 2 layers.
 * Sets up VRAM layout, palette data, letterbox parameters, and initial animation frame.
 *
 * Parameters (from assembly A, X, Y):
 *   layer1_id: BG_DATA_TABLE index for background layer 1
 *   layer2_id: BG_DATA_TABLE index for background layer 2 (0 = none)
 *   letterbox_style: 0=none, 1=large, 2=medium, 3=small; bit 2 = 4bpp layer 2
 */
void load_battle_bg(uint16_t layer1_id, uint16_t layer2_id, uint16_t letterbox_style) {
    /* BG1/BG2 (battle backgrounds) should fill the viewport with wrapping.
     * Sprite Y offset centers SNES-coordinate sprites vertically. */
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_FILL;
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_FILL;
    ppu.sprite_y_offset = VIEWPORT_PAD_TOP;

    /* Clear screen effect globals (assembly lines 24-29) */
    bt.red_flash_duration = 0;
    bt.green_flash_duration = 0;
    bt.shake_duration = 0;
    bt.wobble_duration = 0;
    bt.screen_effect_minimum_wait_frames = 0;
    bt.vertical_shake_hold_duration = 0;
    bt.vertical_shake_duration = 0;

    /* Letterbox setup (assembly lines 31-63) */
    uint16_t lb = letterbox_style & 0x0003;
    if (lb == 0) {
        bt.letterbox_top_end = 0;
        bt.letterbox_bottom_start = SNES_HEIGHT;
    } else if (lb == 1) { /* LARGE */
        bt.letterbox_top_end = 48 - 1;
        bt.letterbox_bottom_start = SNES_HEIGHT - 48;
    } else if (lb == 2) { /* MEDIUM */
        bt.letterbox_top_end = 58 - 1;
        bt.letterbox_bottom_start = SNES_HEIGHT - 58;
    } else if (lb == 3) { /* SMALL */
        bt.letterbox_top_end = 68 - 1;
        bt.letterbox_bottom_start = SNES_HEIGHT - 68;
    }

    /* Initialize letterbox state (assembly lines 64-70) */
    bt.letterbox_effect_ending = 0;
    bt.letterbox_effect_ending_bottom = 0x7000;
    bt.letterbox_effect_ending_top = 0x7000;
    bt.enable_background_darkening = 0;
    bt.background_brightness = 0xFFFF;

    /* Load BG_DATA_TABLE */
    static const uint8_t *bg_data_table = NULL;
    if (!bg_data_table)
        bg_data_table = ASSET_DATA(ASSET_DATA_BG_DATA_TABLE_BIN);
    if (!bg_data_table) return;

    /* Get config entry for layer 1 (17 bytes per entry) */
    uint16_t config_offset = layer1_id * 17;
    uint8_t gfx_index = bg_data_table[config_offset + 0];
    uint8_t pal_index = bg_data_table[config_offset + 1];
    uint8_t bitdepth = bg_data_table[config_offset + 2];

    /* Load and decompress graphics directly to ppu.vram (assembly lines 71-99) */
    const uint8_t *gfx_comp = ASSET_DATA(ASSET_BATTLE_BGS_GRAPHICS(gfx_index));
    size_t gfx_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_GRAPHICS(gfx_index));
    if (gfx_comp) {
        /* Special case: Giygas prayer phase uses different VRAM locations */
        if (bt.current_battle_group == ENEMY_GROUP_BOSS_GIYGAS_DURING_PRAYER_1) {
            /* Assembly lines 90-97: VRAM $3000, tilemap $5C00 */
            ppu.bg_sc[1] = 0x5C;
            ppu.bg_nba[0] = (ppu.bg_nba[0] & 0x0F) | 0x30;
            decomp(gfx_comp, gfx_comp_size, &ppu.vram[0x3000 * 2], 0x5000);
        } else {
            /* Normal: VRAM $1000 (assembly line 99) */
            decomp(gfx_comp, gfx_comp_size, &ppu.vram[0x1000 * 2], 0x2000);
        }
    }

    /* Clear BG3/BG4 tilemaps (assembly lines 101-110):
     * Zero 0x800 bytes at VRAM $5800 and 0x800 bytes at VRAM $0000 */
    memset(&ppu.vram[0x5800 * 2], 0, 0x800);
    memset(&ppu.vram[0x0000 * 2], 0, 0x800);

    /* Load and decompress arrangement directly to VRAM $5C00 (assembly lines 112-133).
     * Both 4bpp and 2bpp paths write to the same VRAM address with in-place fixup. */
    const uint8_t *arr_comp = ASSET_DATA(ASSET_BATTLE_BGS_ARRANGEMENTS(gfx_index));
    size_t arr_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_ARRANGEMENTS(gfx_index));
    uint8_t *arr_buf = &ppu.vram[0x5C00 * 2];
    size_t arr_size = 0;
    if (arr_comp) {
        arr_size = decomp(arr_comp, arr_comp_size, arr_buf, 0x800);
    }

    /* Branch based on bitdepth: 4bpp → mode 9, 2bpp → mode 1 (assembly line 143) */
    if (bitdepth == 4) {
        /* Mode 9 (assembly lines 145-148): mode 1 + BG3 priority */
        ppu.bgmode = 0x09;

        /* Adjust tilemap entries (assembly lines 150-166):
         * For each tilemap word, high byte: AND $DF, ORA $08
         * (clear priority bit, set palette bit → palette 2) */
        for (size_t i = 1; i < arr_size && i < 0x800; i += 2) {
            arr_buf[i] = (arr_buf[i] & 0xDF) | 0x08;
        }

        /* Load BG config for layer 1 (assembly lines 171-248) */
        BGLayerConfigEntry config;
        memcpy(&config, &bg_data_table[config_offset], 17);
        load_bg_layer_config(&loaded_bg_data_layer1, &config);

        /* Set palette_index to PALETTES sub-palette 2 (assembly line 188):
         * PALETTES + BPP4PALETTE_SIZE * 2 = offset 32 in ert.palettes[] (color index 32) */
        loaded_bg_data_layer1.palette_index = 32;

        /* Load palette (assembly lines 193-230) */
        const uint8_t *pal_data = ASSET_DATA(ASSET_BATTLE_BGS_PALETTES(pal_index));
        size_t pal_size = ASSET_SIZE(ASSET_BATTLE_BGS_PALETTES(pal_index));
        if (pal_data) {
            uint16_t colors = (uint16_t)(pal_size / 2);
            if (colors > 16) colors = 16;
            memcpy(loaded_bg_data_layer1.palette, pal_data, colors * 2);
            memcpy(loaded_bg_data_layer1.palette2, pal_data, colors * 2);
            /* Copy to global ert.palettes[] at sub-palette 2 */
            memcpy(&ert.palettes[32], pal_data, colors * 2);
        }

        /* Set target layer to BG2 (assembly line 244) */
        loaded_bg_data_layer1.target_layer = 2;

        /* Generate initial frame (assembly line 248) */
        generate_battlebg_frame(&loaded_bg_data_layer1, 0);

        /* Zero layer 2 target (assembly lines 249-254) */
        loaded_bg_data_layer2.target_layer = 0;

        /* Set color math config 1 (assembly lines 256-257) */
        bt.current_layer_config = 1;
        set_color_math_from_table(1);

        /* Letterbox screen values (assembly lines 258-261) */
        bt.letterbox_visible_screen_value = 0x0017;
        bt.letterbox_nonvisible_screen_value = 0x0015;

    } else {
        /* Mode 1 / 2bpp path (assembly lines 439-480 for @SETUP_MODE1) */
        ppu.bgmode = 0x08; /* mode 0 + priority (assembly line 442) */

        /* SET_BG1_VRAM_LOCATION(Y=$6000, X=$7C00, A=normal) (line 443-446) */
        ppu.bg_sc[0] = 0x7C;
        ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | 0x06;
        ppu.bg_hofs[0] = 0;
        ppu.bg_vofs[0] = 0;

        /* SET_BG2_VRAM_LOCATION(Y=$0000, X=$5800, A=0) (lines 447-450) */
        ppu.bg_sc[1] = 0x58;
        ppu.bg_nba[0] = (ppu.bg_nba[0] & 0x0F) | 0x00;
        ppu.bg_hofs[1] = 0;
        ppu.bg_vofs[1] = 0;

        /* SET_BG3_VRAM_LOCATION(Y=$1000, X=$5C00, A=normal) (lines 451-454) */
        ppu.bg_sc[2] = 0x5C;
        ppu.bg_nba[1] = (ppu.bg_nba[1] & 0xF0) | 0x01;
        ppu.bg_hofs[2] = 0;
        ppu.bg_vofs[2] = 0;

        /* SET_BG4_VRAM_LOCATION(Y=$3000, X=$0C00, A=normal) (lines 455-458) */
        ppu.bg_sc[3] = 0x0C;
        ppu.bg_nba[1] = (ppu.bg_nba[1] & 0x0F) | 0x30;
        ppu.bg_hofs[3] = 0;
        ppu.bg_vofs[3] = 0;

        /* Adjust tilemap entries for mode 1 2bpp (lines 462-477):
         * AND $DF (clear priority), no palette ORA (palette 0) */
        for (size_t i = 1; i < arr_size && i < 0x800; i += 2) {
            arr_buf[i] = arr_buf[i] & 0xDF;
        }

        /* Load BG config for layer 1 (assembly lines 482-496) */
        BGLayerConfigEntry config;
        memcpy(&config, &bg_data_table[config_offset], 17);
        load_bg_layer_config(&loaded_bg_data_layer1, &config);

        /* Set palette_index to PALETTES sub-palette 4 (assembly line 499):
         * PALETTES + BPP4PALETTE_SIZE * 4 = ert.palettes[64] */
        loaded_bg_data_layer1.palette_index = 64;

        /* Load palette (assembly lines 504-551) */
        const uint8_t *pal_data = ASSET_DATA(ASSET_BATTLE_BGS_PALETTES(pal_index));
        size_t pal_size = ASSET_SIZE(ASSET_BATTLE_BGS_PALETTES(pal_index));
        if (pal_data) {
            uint16_t colors = (uint16_t)(pal_size / 2);
            if (colors > 16) colors = 16;
            memcpy(loaded_bg_data_layer1.palette, pal_data, colors * 2);
            memcpy(loaded_bg_data_layer1.palette2, pal_data, colors * 2);
            /* Copy to global ert.palettes[] at sub-palette 4 */
            memcpy(&ert.palettes[64], pal_data, colors * 2);
        }

        /* Set target layer to BG3 (assembly line 554) */
        loaded_bg_data_layer1.target_layer = 3;

        /* Letterbox screen values for mode 1 (lines 702-705) */
        bt.letterbox_visible_screen_value = 0x0817;
        bt.letterbox_nonvisible_screen_value = 0x0013;
    }

    /* === Layer 2 setup === */
    if (layer2_id != 0) {
        if (bitdepth == 4 && (letterbox_style & 0x0004)) {
            /* 4bpp layer 2 with separate graphics (assembly lines 267-418) */
            bt.current_layer_config = 7;
            set_color_math_from_table(7);

            uint16_t l2_config_offset = layer2_id * 17;
            uint8_t l2_gfx_index = bg_data_table[l2_config_offset + 0];
            uint8_t l2_pal_index = bg_data_table[l2_config_offset + 1];

            /* Load and decompress layer 2 graphics directly to VRAM $0000 (lines 280-294) */
            const uint8_t *l2_gfx_comp = ASSET_DATA(ASSET_BATTLE_BGS_GRAPHICS(l2_gfx_index));
            size_t l2_gfx_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_GRAPHICS(l2_gfx_index));
            if (l2_gfx_comp) {
                decomp(l2_gfx_comp, l2_gfx_comp_size, &ppu.vram[0x0000 * 2], 0x2000);
            }

            /* Load and decompress layer 2 arrangement directly to VRAM $5800 (lines 296-330) */
            const uint8_t *l2_arr_comp = ASSET_DATA(ASSET_BATTLE_BGS_ARRANGEMENTS(l2_gfx_index));
            size_t l2_arr_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_ARRANGEMENTS(l2_gfx_index));
            if (l2_arr_comp) {
                uint8_t *l2_arr_dst = &ppu.vram[0x5800 * 2];
                size_t l2_arr_size = decomp(l2_arr_comp, l2_arr_comp_size,
                                            l2_arr_dst, 0x800);
                /* Adjust tilemap: AND $DF, ORA $10 (palette 4) (lines 313-329) */
                for (size_t i = 1; i < l2_arr_size && i < 0x800; i += 2) {
                    l2_arr_dst[i] = (l2_arr_dst[i] & 0xDF) | 0x10;
                }
            }

            /* Load BG config for layer 2 (lines 332-349) */
            BGLayerConfigEntry l2_config;
            memcpy(&l2_config, &bg_data_table[l2_config_offset], 17);
            load_bg_layer_config(&loaded_bg_data_layer2, &l2_config);

            /* Set palette_index to PALETTES sub-palette 4 (line 352):
             * PALETTES + BPP4PALETTE_SIZE * 4 = ert.palettes[64] */
            loaded_bg_data_layer2.palette_index = 64;

            /* Set target_layer = 1 (BG1) (line 358) */
            loaded_bg_data_layer2.target_layer = 1;

            /* Load layer 2 palette (lines 362-410) */
            const uint8_t *l2_pal_data = ASSET_DATA(ASSET_BATTLE_BGS_PALETTES(l2_pal_index));
            size_t l2_pal_size = ASSET_SIZE(ASSET_BATTLE_BGS_PALETTES(l2_pal_index));
            if (l2_pal_data) {
                uint16_t l2_colors = (uint16_t)(l2_pal_size / 2);
                if (l2_colors > 16) l2_colors = 16;
                memcpy(loaded_bg_data_layer2.palette, l2_pal_data, l2_colors * 2);
                memcpy(loaded_bg_data_layer2.palette2, l2_pal_data, l2_colors * 2);
                /* Copy to global ert.palettes[] at sub-palette 4 */
                memcpy(&ert.palettes[64], l2_pal_data, l2_colors * 2);
            }

            /* Generate initial frame for layer 2 (line 413) */
            generate_battlebg_frame(&loaded_bg_data_layer2, 1);

            /* Letterbox screen values (lines 414-417) */
            bt.letterbox_visible_screen_value = 0x0215;
            bt.letterbox_nonvisible_screen_value = 0x0014;

        } else if (bitdepth == 4) {
            /* Simple layer 2 (mode 9): config only, no graphics load (lines 419-438) */
            uint16_t l2_config_offset = layer2_id * 17;
            BGLayerConfigEntry l2_config;
            memcpy(&l2_config, &bg_data_table[l2_config_offset], 17);
            load_bg_layer_config(&loaded_bg_data_layer2, &l2_config);
            loaded_bg_data_layer2.freeze_palette_scrolling = 1;
            loaded_bg_data_layer2.target_layer = 2;

        } else {
            /* Mode 1 layer 2 with separate graphics (assembly lines 559-696) */
            bt.current_layer_config = 3;
            set_color_math_from_table(3);

            uint16_t l2_config_offset = layer2_id * 17;
            uint8_t l2_gfx_index = bg_data_table[l2_config_offset + 0];
            uint8_t l2_pal_index = bg_data_table[l2_config_offset + 1];

            /* Load and decompress layer 2 graphics directly to VRAM $3000 (lines 565-580) */
            const uint8_t *l2_gfx_comp = ASSET_DATA(ASSET_BATTLE_BGS_GRAPHICS(l2_gfx_index));
            size_t l2_gfx_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_GRAPHICS(l2_gfx_index));
            if (l2_gfx_comp) {
                decomp(l2_gfx_comp, l2_gfx_comp_size, &ppu.vram[0x3000 * 2], 0x1800);
            }

            /* Load and decompress layer 2 arrangement directly to VRAM $0C00 (lines 582-616) */
            const uint8_t *l2_arr_comp = ASSET_DATA(ASSET_BATTLE_BGS_ARRANGEMENTS(l2_gfx_index));
            size_t l2_arr_comp_size = ASSET_SIZE(ASSET_BATTLE_BGS_ARRANGEMENTS(l2_gfx_index));
            if (l2_arr_comp) {
                uint8_t *l2_arr_dst = &ppu.vram[0x0C00 * 2];
                size_t l2_arr_size = decomp(l2_arr_comp, l2_arr_comp_size,
                                            l2_arr_dst, 0x800);
                /* Mode 1 layer 2 tilemap: AND $DF only (lines 600-615) */
                for (size_t i = 1; i < l2_arr_size && i < 0x800; i += 2) {
                    l2_arr_dst[i] = l2_arr_dst[i] & 0xDF;
                }
            }

            /* Load BG config for layer 2 (lines 618-635) */
            BGLayerConfigEntry l2_config;
            memcpy(&l2_config, &bg_data_table[l2_config_offset], 17);
            load_bg_layer_config(&loaded_bg_data_layer2, &l2_config);

            /* Set palette_index to PALETTES sub-palette 6 (line 638):
             * PALETTES + BPP4PALETTE_SIZE * 6 = ert.palettes[96] */
            loaded_bg_data_layer2.palette_index = 96;

            /* Load layer 2 palette (lines 643-691) */
            const uint8_t *l2_pal_data = ASSET_DATA(ASSET_BATTLE_BGS_PALETTES(l2_pal_index));
            size_t l2_pal_size = ASSET_SIZE(ASSET_BATTLE_BGS_PALETTES(l2_pal_index));
            if (l2_pal_data) {
                uint16_t l2_colors = (uint16_t)(l2_pal_size / 2);
                if (l2_colors > 16) l2_colors = 16;
                memcpy(loaded_bg_data_layer2.palette, l2_pal_data, l2_colors * 2);
                memcpy(loaded_bg_data_layer2.palette2, l2_pal_data, l2_colors * 2);
                /* Copy to global ert.palettes[] at sub-palette 6 */
                memcpy(&ert.palettes[96], l2_pal_data, l2_colors * 2);
            }

            /* Set target_layer = 4 (BG4) (line 694) */
            loaded_bg_data_layer2.target_layer = 4;
        }
    } else if (bitdepth != 4) {
        /* No layer 2 in mode 1 (assembly line 699) */
        loaded_bg_data_layer2.target_layer = 0;
    }

    /* Finalize (assembly lines 706-724) */
    distort_30fps = 0;
    if ((loaded_bg_data_layer2.target_layer & 0xFF) != 0) {
        if ((bg_data_table[layer2_id * 17 + 13] & 0xFF) != 0) {
            /* Layer 2 has distortion → enable 30fps distortion */
            distort_30fps = 1;
        }
    }

    /* Build letterbox HDMA table and apply to PPU */
    build_letterbox_hdma_table();
#if BATTLE_LETTERBOX_ENABLED
    apply_letterbox_to_ppu();
#endif

    /* Stop the battle swirl effect */
    stop_battle_swirl();
}


/*
 * LOAD_BATTLE_SPRITE — Port of asm/battle/load_battle_sprite.asm (447 lines).
 *
 * Initializes spritemap entries for a battle sprite, determines its size,
 * decompresses its graphics, and copies tile data to BUFFER.
 *
 * sprite_id: 1-based index into BATTLE_SPRITES_POINTERS table.
 */
void load_battle_sprite(uint16_t sprite_id) {
    if (sprite_id == 0 || !battle_sprites_pointers_data) return;

    /* Record spritemap allocation start for this sprite (assembly lines 26-30) */
    bt.battle_spritemap_allocation_counts[bt.current_battle_sprites_allocated] =
        bt.current_battle_spritemaps_allocated;

    /* Determine sprite size from BATTLE_SPRITES_POINTERS table (lines 134-142) */
    uint8_t size_enum = battle_sprites_pointers_data[(sprite_id - 1) * BATTLE_SPRITES_ENTRY_SIZE + 4];

    /* Set width and height based on sprite size enum (lines 143-296) */
    uint16_t width = 1, height = 1;
    switch (size_enum) {
        case 1: width = 1; height = 1; break; /* 32x32 */
        case 2: width = 2; height = 1; break; /* 64x32 */
        case 3: width = 1; height = 2; break; /* 32x64 */
        case 4: width = 2; height = 2; break; /* 64x64 */
        case 5: width = 4; height = 2; break; /* 128x64 */
        case 6: width = 4; height = 4; break; /* 128x128 */
        default: break;
    }

    /* Initialize all 16 spritemap entries with defaults (lines 49-132) */
    uint8_t *smap = &bt.battle_spritemaps[bt.current_battle_sprites_allocated * 80];

    static const uint16_t tile_grid[32] = {
        0x0000, 0x0004, 0x0008, 0x000C,
        0x0040, 0x0044, 0x0048, 0x004C,
        0x0080, 0x0084, 0x0088, 0x008C,
        0x00C0, 0x00C4, 0x00C8, 0x00CC,
        0x0100, 0x0104, 0x0108, 0x010C,
        0x0140, 0x0144, 0x0148, 0x014C,
        0x0180, 0x0184, 0x0188, 0x018C,
        0x01C0, 0x01C4, 0x01C8, 0x01CC
    };

    for (uint16_t i = 0; i < 16; i++) {
        uint16_t off = i * 5;
        smap[off + 0] = 224;   /* y_offset: offscreen default */
        smap[off + 3] = 240;   /* x_offset: offscreen default */
        smap[off + 4] = 1;     /* special_flags */

        uint16_t grid_index = i + bt.current_battle_spritemaps_allocated;
        uint16_t tile_val = (grid_index < 32) ? tile_grid[grid_index] : 0;
        smap[off + 1] = (uint8_t)(tile_val & 0xFF);        /* tile */
        smap[off + 2] = (uint8_t)(bt.current_battle_sprites_allocated * 2
                                   + (tile_val >> 8) + 32); /* flags */
    }

    /* Size-specific position adjustments (lines 155-296) */
    switch (size_enum) {
        case 2: /* 2x1: two sprites side by side */
            smap[0 * 5 + 3] = 224;                /* entry 0: x = -32 */
            smap[1 * 5 + 3] = 0;                  /* entry 1: x = 0 */
            break;
        case 3: /* 1x2: two sprites stacked */
            smap[0 * 5 + 0] = 192;                /* entry 0: y = -64 */
            break;
        case 4: /* 2x2 */
            smap[0 * 5 + 0] = 192;                /* entries 0,1: y = -64 */
            smap[1 * 5 + 0] = 192;
            smap[0 * 5 + 3] = 224;                /* entries 0,2: x = -32 */
            smap[2 * 5 + 3] = 224;
            smap[1 * 5 + 3] = 0;                  /* entries 1,3: x = 0 */
            smap[3 * 5 + 3] = 0;
            break;
        case 5: /* 4x2 */
            for (int j = 0; j < 4; j++)
                smap[j * 5 + 0] = 192;            /* entries 0-3: y = -64 */
            smap[0 * 5 + 3] = 192;                /* col 0: x = -64 */
            smap[4 * 5 + 3] = 192;
            smap[1 * 5 + 3] = 224;                /* col 1: x = -32 */
            smap[5 * 5 + 3] = 224;
            smap[2 * 5 + 3] = 0;                  /* col 2: x = 0 */
            smap[6 * 5 + 3] = 0;
            smap[3 * 5 + 3] = 32;                 /* col 3: x = 32 */
            smap[7 * 5 + 3] = 32;
            break;
        case 6: /* 4x4 */
            for (int j = 0; j < 4; j++)
                smap[j * 5 + 0] = 160;            /* row 0: y = -96 */
            for (int j = 4; j < 8; j++)
                smap[j * 5 + 0] = 192;            /* row 1: y = -64 */
            /* row 2 (entries 8-11): y = 224 (default, = -32) */
            for (int j = 12; j < 16; j++)
                smap[j * 5 + 0] = 0;              /* row 3: y = 0 */
            for (int r = 0; r < 4; r++) {
                smap[(r * 4 + 0) * 5 + 3] = 192;  /* col 0: x = -64 */
                smap[(r * 4 + 1) * 5 + 3] = 224;  /* col 1: x = -32 */
                smap[(r * 4 + 2) * 5 + 3] = 0;    /* col 2: x = 0 */
                smap[(r * 4 + 3) * 5 + 3] = 32;   /* col 3: x = 32 */
            }
            break;
    }

    /* Set terminator at end of used entries (lines 297-307) */
    uint16_t term_off = (width * height) * 5 - 1;
    if (term_off < 80) smap[term_off] = 0x81;

    /* Copy to ALT_BATTLE_SPRITEMAPS and adjust palette flags +8 (lines 309-351) */
    uint8_t *alt_smap = &bt.alt_battle_spritemaps[bt.current_battle_sprites_allocated * 80];
    memcpy(alt_smap, smap, 80);
    for (uint16_t i = 0; i < 16; i++)
        alt_smap[i * 5 + 2] += 8;

    /* Store width/height for this sprite (lines 352-365) */
    bt.current_battle_sprite_widths[bt.current_battle_sprites_allocated] = width;
    bt.current_battle_sprite_heights[bt.current_battle_sprites_allocated] = height;
    bt.current_battle_sprites_allocated++;

    /* Decompress sprite graphics (lines 367-392) */
    static const uint16_t buffer_offsets[32] = {
        0x0000, 0x0080, 0x0100, 0x0180,
        0x0800, 0x0880, 0x0900, 0x0980,
        0x1000, 0x1080, 0x1100, 0x1180,
        0x1800, 0x1880, 0x1900, 0x1980,
        0x2000, 0x2080, 0x2100, 0x2180,
        0x2800, 0x2880, 0x2900, 0x2980,
        0x3000, 0x3080, 0x3100, 0x3180,
        0x3800, 0x3880, 0x3900, 0x3980
    };

    const uint8_t *compressed = ASSET_DATA(ASSET_BATTLE_SPRITES(sprite_id - 1));
    size_t compressed_size = ASSET_SIZE(ASSET_BATTLE_SPRITES(sprite_id - 1));
    if (!compressed) return;

    /* Decompress to decomp_staging (shared_scratch.decomp) to avoid aliasing
     * with the scatter-copy destinations in ert.buffer (buffer_offsets[], all < $4000).
     * Assembly uses BUFFER+$8000; we use decomp_staging (32 KB, free during
     * battle) to avoid the high ert.buffer offset. */
    size_t decompressed_size = decomp(compressed, compressed_size,
                                       decomp_staging, SHARED_SCRATCH_SIZE);
    if (decompressed_size == 0) return;
    const uint8_t *decompressed = decomp_staging;

    /* Copy decompressed tiles to ert.buffer at correct offsets (lines 393-446).
     * Each tile slot is 4 rows of 128 bytes, placed 0x200 apart in the ert.buffer
     * (matching the SNES VRAM sprite tile row stride of 16 tiles). */
    uint16_t total_tiles = width * height;
    const uint8_t *src = decompressed;
    for (uint16_t t = 0; t < total_tiles; t++) {
        if (bt.current_battle_spritemaps_allocated >= 32) break;
        uint16_t dest_base = buffer_offsets[bt.current_battle_spritemaps_allocated];
        bt.current_battle_spritemaps_allocated++;
        for (uint16_t row = 0; row < 4; row++) {
            memcpy(&ert.buffer[dest_base + row * 0x200], src, 128);
            src += 128;
        }
    }
}


/*
 * SETUP_BATTLE_ENEMY_SPRITES — Port of asm/battle/enemy/setup_battle_enemy_sprites.asm (116 lines).
 *
 * Iterates through the enemy group, loads each enemy's battle sprite palette
 * to CGRAM, records enemy IDs, and calls LOAD_BATTLE_SPRITE for each.
 * Finally copies sprite tile data from ert.buffer to VRAM.
 */
void setup_battle_enemy_sprites(void) {
    bt.current_battle_sprites_allocated = 0;
    bt.current_battle_spritemaps_allocated = 0;

    /* Load BATTLE_SPRITES_POINTERS table (110 entries × 5 bytes each) */
    if (!battle_sprites_pointers_data)
        battle_sprites_pointers_data = ASSET_DATA(ASSET_DATA_BATTLE_SPRITES_POINTERS_BIN);

    /* Load BTL_ENTRY_PTR_TABLE to iterate enemy group (assembly lines 13-21) */
    static const uint8_t *ptr_table = NULL;
    if (!ptr_table)
        ptr_table = ASSET_DATA(ASSET_DATA_BTL_ENTRY_PTR_TABLE_BIN);
    if (!ptr_table) return;

    /* Get pointer to enemy group data */
    uint32_t entry_off = (uint32_t)bt.current_battle_group * 8;
    uint32_t rom_ptr = (uint32_t)ptr_table[entry_off]
                     | ((uint32_t)ptr_table[entry_off + 1] << 8)
                     | ((uint32_t)ptr_table[entry_off + 2] << 16);

    /* Load ENEMY_BATTLE_GROUPS_TABLE */
    static const uint8_t *groups_table = NULL;
    if (!groups_table)
        groups_table = ASSET_DATA(ASSET_DATA_ENEMY_BATTLE_GROUPS_TABLE_BIN);
    if (!groups_table) return;

    uint32_t groups_offset = rom_ptr - 0xD0D52D;
    const uint8_t *grp_ptr = groups_table + groups_offset;

    /* Iterate through enemy entries (assembly lines 23-101).
     * Each entry is 3 bytes: {terminator_check, enemy_id_lo, enemy_id_hi}.
     * ONE sprite is loaded per entry (duplicates share the same sprite).
     * Terminated by first byte == 0xFF. */
    while (1) {
        uint8_t first_byte = *grp_ptr;
        if (first_byte == 0xFF) break;

        uint16_t enemy_id = read_u16_le(&grp_ptr[1]);

        /* Get battle sprite and palette info from enemy config */
        uint16_t battle_sprite = enemy_config_table[enemy_id].battle_sprite;
        uint8_t palette_id = enemy_config_table[enemy_id].battle_sprite_palette;

        /* Load palette (lines 48-79): 32 bytes to ert.palettes[] sub-palette 8+ */
        const uint8_t *pal_data = ASSET_DATA(ASSET_BATTLE_SPRITES_PALETTES(palette_id));
        size_t pal_size = ASSET_SIZE(ASSET_BATTLE_SPRITES_PALETTES(palette_id));
        if (pal_data) {
            uint16_t pal_offset = 128 + bt.current_battle_sprites_allocated * 16;
            uint16_t colors = (uint16_t)(pal_size / 2);
            if (colors > 16) colors = 16;
            memcpy(&ert.palettes[pal_offset], pal_data, colors * 2);
        }

        /* Record enemy ID (lines 83-84) */
        bt.current_battle_sprite_enemy_ids[bt.current_battle_sprites_allocated] = enemy_id;

        /* Load the battle sprite (lines 85-87) */
        load_battle_sprite(battle_sprite);

        grp_ptr += 3;
    }

    /* Copy sprite tile data from ert.buffer to VRAM (assembly lines 103-116):
     * If spritemaps > 16, copy $3000 bytes; else copy $2000 bytes.
     * Destination: VRAM word address $2000. */
    uint16_t vram_size = (bt.current_battle_spritemaps_allocated > 16) ? 0x3000 : 0x2000;
    ppu_vram_dma(ert.buffer, 0x2000, vram_size);
}


/*
 * LOAD_BATTLE_SCENE (asm/battle/load_battle_scene.asm)
 *
 * Loads (or reloads) the battle scene during boss transitions. Manages swirl
 * transitions, loads enemy sprites/BG/window GFX, fades in.
 *
 * Parameters:
 *   battle_group: enemy group index (into BTL_ENTRY_BG_TABLE / BTL_ENTRY_PTR_TABLE)
 *   music_id: music track to play (0 = don't change music)
 */
void load_battle_scene(uint16_t battle_group, uint16_t music_id) {
    uint16_t skip_swirl = 0;

    /* Determine if we should skip the swirl-in transition:
     * - If not yet in battle mode (first load), skip swirl
     * - If this is the final Giygas phase, skip swirl */
    if (!bt.battle_mode_flag || battle_group == ENEMY_GROUP_BOSS_GIYGAS_PHASE_FINAL) {
        skip_swirl = 1;
    }

    /* Swirl in (if applicable) */
    if (!skip_swirl) {
        start_battle_swirl(6, 1, 30);
        while (is_battle_swirl_active()) {
            window_tick();
        }
    }

    /* Set current battle group */
    bt.current_battle_group = battle_group;

    /* Read BG table entries for this battle group */
    uint16_t bg_id = 0;
    uint16_t palette_id = 0;
    uint16_t letterbox_style = 0;
    if (btl_entry_bg_table) {
        uint16_t offset = bt.current_battle_group * 4;
        bg_id = read_u16_le(&btl_entry_bg_table[offset]);
        palette_id = read_u16_le(&btl_entry_bg_table[offset + 2]);
    }

    /* Get letterbox style from btl_entry_ptr_table */
    if (btl_entry_ptr_table) {
        uint16_t entry_offset = bt.current_battle_group * 8 + 7; /* battle_entry_ptr_entry::letterbox_style */
        letterbox_style = btl_entry_ptr_table[entry_offset];
    }

    /* Load battle visuals */
    force_blank_and_wait_vblank();
    load_enemy_battle_sprites();
    text_load_window_gfx();
    upload_text_tiles_to_vram(1);
    load_battle_bg(bg_id, palette_id, letterbox_style);
    setup_battle_enemy_sprites();
    set_palette_upload_mode(24);
    render_all_battle_sprites();

    /* Mark battle mode active */
    bt.battle_mode_flag = 1;

    /* Play music if requested */
    if (music_id != 0) {
        change_music(music_id);
    }

    /* Fade in */
    blank_screen_and_wait_vblank();
    if (skip_swirl) {
        /* Quick fade in (step=1, delay=4 frames) */
        fade_in(1, 4);
        wait_for_fade_with_tick();
    } else {
        /* Full fade in (step=15, delay=1 frame) + swirl out */
        fade_in(15, 1);
        if (battle_group != ENEMY_GROUP_BOSS_GIYGAS_PHASE_FINAL) {
            start_battle_swirl(6, 0, 5);
            while (is_battle_swirl_active()) {
                window_tick();
            }
        }
    }
}


/*
 * LAYOUT_ENEMY_BATTLE_POSITIONS (asm/battle/layout_enemy_battle_positions.asm)
 *
 * Assigns enemy battlers to rows (front/back), calculates X positions for
 * each enemy, centers the group, converts to pixel coords, sets Y positions,
 * handles special group 475 overrides, and bubble-sorts battlers by
 * rendering priority (row, y, x).
 *
 * Returns 0 if layout fails (too many sprites in a row), nonzero on success.
 */
uint16_t layout_enemy_battle_positions(void) {
    uint16_t battle_sprite_row_width[2] = {0, 0};

    /* ================================================================
     * Phase 1: Row assignment (lines 40-142)
     * Assign each enemy battler to a row, tracking accumulated width.
     * If a row overflows (>30 units), try the other row.
     * ================================================================ */
    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 0)
            continue;
        if ((b->ally_or_enemy & 0xFF) != 1)
            continue;

        /* Assign VRAM sprite index */
        b->vram_sprite_index = (uint8_t)find_battle_sprite_for_enemy(b->id);

        uint16_t row = b->row & 0xFF;
        uint16_t width = get_battle_sprite_width(b->sprite);

        /* Add spacing if row already has sprites */
        if (battle_sprite_row_width[row] != 0)
            width++;

        if (battle_sprite_row_width[row] + width > 30) {
            /* Row overflow — try other row */
            uint16_t alt_row = 1 - row;
            uint16_t alt_width = get_battle_sprite_width(b->sprite);
            if (battle_sprite_row_width[alt_row] != 0)
                alt_width++;

            if (battle_sprite_row_width[alt_row] + alt_width > 30)
                return 0;  /* Layout failed — no room */

            b->row = (uint8_t)alt_row;
            battle_sprite_row_width[alt_row] += alt_width;
        } else {
            battle_sprite_row_width[row] += width;
        }
    }

    /* ================================================================
     * Phase 2: X-position assignment — first row group (lines 143-280)
     * Position battlers in the same row as battler[8]'s row.
     * Starts at center (32), expands left and right.
     * ================================================================ */
    uint16_t first_row = bt.battlers_table[8].row & 0xFF;
    uint16_t left_bound = 32;   /* left edge of placed sprites */
    uint16_t right_bound = 32;  /* right edge of placed sprites */
    uint16_t last_dir = 0;      /* last placement direction (for alternating) */
    uint16_t last_pos = 0;      /* last position used (for tie-breaking) */

    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 0)
            continue;
        if ((b->ally_or_enemy & 0xFF) != 1)
            continue;
        if ((b->row & 0xFF) != first_row)
            continue;

        uint16_t half_width = get_battle_sprite_width(b->sprite) / 2;

        if (left_bound == right_bound) {
            /* First sprite: place at center (32) */
            b->sprite_x = (uint8_t)left_bound;
            left_bound -= half_width;
            right_bound += half_width;
            /* Random initial direction */
            if (rand_byte() & 1) {
                last_dir = left_bound;
                last_pos = left_bound;
            } else {
                last_dir = right_bound;
                last_pos = right_bound;
            }
        } else {
            /* Compare space on left vs right */
            uint16_t left_space = 32 - left_bound;
            uint16_t right_space = right_bound - 32;

            if (left_space < right_space) {
                goto place_left;
            } else if (left_space > right_space) {
                goto place_right;
            } else {
                /* Equal space — random */
                if (rand_byte() & 1)
                    goto place_left;
                /* else fall through to place_right */
            }

            place_right:
            {
                uint8_t pos = (uint8_t)(right_bound + half_width + 1);
                b->sprite_x = pos;
                right_bound = (pos & 0xFF) + half_width;
                goto placed;
            }
            place_left:
            {
                uint8_t pos = (uint8_t)(left_bound - half_width - 1);
                b->sprite_x = pos;
                left_bound = (pos & 0xFF) - half_width;
                goto placed;
            }
            placed:;
        }
    }

    last_pos = last_dir;  /* assembly stores @LOCAL04 = @VIRTUAL04 after loop */
    uint16_t group1_left = left_bound;
    uint16_t group1_right = right_bound;

    /* ================================================================
     * Phase 3: X-position assignment — other row group (lines 281-426)
     * Position battlers NOT in the first row's group.
     * ================================================================ */
    uint16_t other_left = last_pos;
    uint16_t other_right = last_pos;

    for (uint16_t idx = 8; idx < BATTLER_COUNT; idx++) {
        Battler *b = &bt.battlers_table[idx];
        if ((b->consciousness & 0xFF) == 0)
            continue;
        if ((b->ally_or_enemy & 0xFF) != 1)
            continue;
        if ((b->row & 0xFF) == first_row)
            continue;

        uint16_t half_width = get_battle_sprite_width(b->sprite) / 2;

        if (other_left == other_right) {
            /* First sprite in other row: place at same X as group 1 center */
            b->sprite_x = (uint8_t)other_left;
            other_left -= half_width;
            other_right += half_width;
        } else {
            /* Compare space on left vs right (relative to 32) */
            if (other_right <= 32) {
                goto other_place_right;
            } else if (other_left > 32) {
                goto other_place_left;
            } else {
                uint16_t space_left = 32 - other_left;
                uint16_t space_right = other_right - 32;
                if (space_left < space_right) {
                    goto other_place_left;
                } else if (space_left > space_right) {
                    goto other_place_right;
                } else {
                    if (rand_byte() & 1)
                        goto other_place_left;
                }
            }

            other_place_right:
            {
                uint8_t pos = (uint8_t)(other_right + half_width + 1);
                b->sprite_x = pos;
                other_right = (pos & 0xFF) + half_width;
                goto other_placed;
            }
            other_place_left:
            {
                uint8_t pos = (uint8_t)(other_left - half_width - 1);
                b->sprite_x = pos;
                other_left = (pos & 0xFF) - half_width;
                goto other_placed;
            }
            other_placed:;
        }
    }

    /* ================================================================
     * Phase 4: Single-row fix (lines 427-464)
     * If both groups ended up with equal bounds (meaning only 1 row used),
     * clear all enemy battler rows to 0.
     * ================================================================ */
    if (first_row == 1 && other_left == other_right) {
        for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
            Battler *b = &bt.battlers_table[i];
            if ((b->consciousness & 0xFF) == 0)
                continue;
            if ((b->ally_or_enemy & 0xFF) != 1)
                continue;
            b->row = 0;
        }
    }

    /* Update global bounds: take minimum left, maximum right */
    if (other_left < group1_left)
        group1_left = other_left;
    if (other_right > group1_right)
        group1_right = other_right;

    /* ================================================================
     * Phase 5: Centering & pixel conversion (lines 475-553)
     * Calculate center offset, convert unit coords to pixel coords,
     * set Y positions by row.
     * ================================================================ */
    uint16_t center = (group1_left + group1_right) / 2;
    int16_t offset = (int16_t)(32 - center - 16);

    for (uint16_t i = 8; i < BATTLER_COUNT; i++) {
        Battler *b = &bt.battlers_table[i];
        if ((b->consciousness & 0xFF) == 0)
            continue;
        if ((b->ally_or_enemy & 0xFF) != 1)
            continue;

        /* Apply offset and convert to pixel coords (multiply by 8) */
        uint8_t unit_x = b->sprite_x;
        b->sprite_x = (uint8_t)((unit_x + (int8_t)offset) * 8);

        /* Set Y position by row: front=144, back=128 */
        if ((b->row & 0xFF) == 0) {
            b->sprite_y = 144;
        } else {
            b->sprite_y = 128;
        }
    }

    /* Special case: enemy group 475 has hardcoded positions */
    if (bt.current_battle_group == ENEMY_GROUP_BOSS_GIYGAS_PHASE_1_ENTRY) {
        bt.battlers_table[8].sprite_x = 128;
        bt.battlers_table[8].sprite_y = 128;
        bt.battlers_table[9].sprite_x = 200;
        bt.battlers_table[9].sprite_y = 144;
    }

    /* ================================================================
     * Phase 6: Bubble sort to assign letter suffixes by screen position
     * (lines 554-730)
     *
     * Among battlers with the same enemy ID, reassign the_flag (letter
     * suffix A, B, C...) so letters match visual position order.
     * Swaps the_flag first, then does a full battler swap only when
     * the original a.the_flag < b.the_flag (to keep battler data in
     * the correct slot).
     * Uses bt.battlers_table[BATTLER_COUNT-1] as temp swap space.
     * Repeats until no swaps occur.
     * ================================================================ */
    uint16_t sort_swapped;
sort_restart:
    sort_swapped = 0;
    for (uint16_t i = 0; (int16_t)i < (int16_t)(bt.enemies_in_battle - 1); i++) {
        Battler *ba = &bt.battlers_table[8 + i];
        for (uint16_t j = i + 1; j < bt.enemies_in_battle; j++) {
            Battler *bb = &bt.battlers_table[8 + j];

            /* Only sort battlers with the same enemy ID */
            if (ba->id != bb->id)
                continue;

            int need_swap = 0;

            if (ba->the_flag < bb->the_flag) {
                /* a has lower letter but should we swap?
                 * Swap if a is visually above b (lower sprite_y),
                 * or same y and a is further right (higher sprite_x) */
                if (ba->sprite_y < bb->sprite_y) {
                    need_swap = 1;
                } else if (ba->sprite_y == bb->sprite_y) {
                    if (ba->sprite_x > bb->sprite_x)
                        need_swap = 1;
                }
                /* If a.sprite_y > b.sprite_y, fall through to @COMPARE_ROW */
                if (!need_swap) goto compare_row;
            } else if (ba->the_flag > bb->the_flag) {
                /* a has higher letter — @COMPARE_ROW path */
compare_row:
                /* USA: BGT (strict greater) */
                if (ba->the_flag <= bb->the_flag)
                    continue;  /* equal flags — no swap */

                /* a.the_flag > b.the_flag: swap if a is visually below b
                 * (higher sprite_y), or same y and a is further left */
                if (ba->sprite_y > bb->sprite_y) {
                    need_swap = 1;
                } else if (ba->sprite_y == bb->sprite_y) {
                    if (ba->sprite_x < bb->sprite_x)
                        need_swap = 1;
                    else
                        continue;
                } else {
                    continue;
                }
            } else {
                continue;  /* same the_flag, no swap */
            }

            if (need_swap) {
                sort_swapped = 1;

                /* Swap the_flag values */
                uint8_t tmp_flag = ba->the_flag;
                ba->the_flag = bb->the_flag;
                bb->the_flag = tmp_flag;

                /* Full battler swap only if original a.the_flag < b.the_flag
                 * (after swap: new a.the_flag > new b.the_flag) */
                if (ba->the_flag > bb->the_flag) {
                    Battler *temp = &bt.battlers_table[BATTLER_COUNT - 1];
                    memcpy(temp, ba, sizeof(Battler));
                    memcpy(ba, bb, sizeof(Battler));
                    memcpy(bb, temp, sizeof(Battler));
                }
            }
        }
    }
    if (sort_swapped)
        goto sort_restart;

    /* Clear temp battler slot (last in array) */
    memset(&bt.battlers_table[BATTLER_COUNT - 1], 0, sizeof(Battler));

    return 1;  /* Success */
}


/*
 * DESATURATE_PALETTES (asm/system/palette/desaturate_palettes.asm)
 *
 * Converts the first 128 palette entries to greyscale.
 * Backs up ert.palettes to ert.buffer+0x2000 first, then replaces each color
 * with its average (R+G+B)/3 applied to all three channels.
 * Processes BPP4PALETTE_SIZE * 4 = 128 colors (sub-ert.palettes 0-7).
 */
void desaturate_palettes(void) {
    /* Back up ert.palettes to BUFFER+$2000 */
    memcpy(&ert.buffer[BUF_BATTLE_PALETTE_SAVE], ert.palettes, 256 * 2);

    for (int i = 0; i < 128; i++) {
        uint16_t color = ert.palettes[i];
        uint16_t r = color & 0x1F;
        uint16_t g = (color >> 5) & 0x1F;
        uint16_t b = (color >> 10) & 0x1F;
        uint16_t grey = (r + g + b) / 3;
        ert.palettes[i] = grey | (grey << 5) | (grey << 10);
    }

    ert.palette_upload_mode = 24;  /* PALETTE_UPLOAD_FULL */
}


/*
 * SET_PALETTE_UPLOAD_MODE (asm/system/palette/set_palette_upload_mode.asm)
 *
 * Sets the palette upload mode flag (8-bit store).
 * NMI handler reads this to decide which ert.palettes to upload to CGRAM.
 */
void set_palette_upload_mode(uint16_t mode) {
    ert.palette_upload_mode = (uint8_t)mode;
}


/*
 * SET_COLDATA (asm/system/set_coldata.asm)
 *
 * Sets the fixed color registers (used for color math / screen tinting).
 * A=red, X=green, Y=blue (each 0-31).
 */
void set_coldata(uint8_t red, uint8_t green, uint8_t blue) {
    ppu.coldata_r = red & 0x1F;
    ppu.coldata_g = green & 0x1F;
    ppu.coldata_b = blue & 0x1F;
}


/*
 * SET_COLOUR_ADDSUB_MODE (asm/system/set_colour_addsub_mode.asm)
 *
 * Sets the CGWSEL and CGADSUB registers for color math configuration.
 */
void set_colour_addsub_mode(uint8_t cgwsel_val, uint8_t cgadsub_val) {
    ppu.cgwsel = cgwsel_val;
    ppu.cgadsub = cgadsub_val;
}


/*
 * RESTORE_BG_PALETTE_AND_ENABLE_DISPLAY (asm/system/palette/restore_bg_palette_and_enable_display.asm)
 *
 * Restores ert.palettes[0] from backup, enables all layers on main screen,
 * and triggers a palette upload.
 */
void restore_bg_palette_and_enable_display(void) {
    ert.palettes[0] = bt.background_colour_backup;
    ppu.tm = 0x17;  /* BG1 + BG2 + BG3 + OBJ on main screen */
    set_palette_upload_mode(8);
}


/*
 * BLANK_SCREEN_AND_WAIT_VBLANK (asm/overworld/blank_screen_and_wait_vblank.asm)
 *
 * Force-blanks the screen (INIDISP = 0x80) and waits for one VBlank.
 * Unlike FORCE_BLANK_AND_WAIT_VBLANK, does not disable HDMA or cancel fade.
 */
void blank_screen_and_wait_vblank(void) {
    ppu.inidisp = 0x80;
    render_frame_tick();
}


/*
 * INITIALIZE_BATTLE_UI_STATE (asm/battle/initialize_battle_ui_state.asm)
 * Resets all window, text, VWF, and battle UI state to initial values.
 * Called at the start of battle initialization and from file select/intro.
 */
void initialize_battle_ui_state(void) {
    ow.render_hppp_windows = 0;
    bt.current_flashing_enemy_row = 0xFFFF;
    bt.current_flashing_row = -1;
    bt.current_flashing_enemy = -1;
    win.battle_menu_current_character_id = -1;
    ow.redraw_all_windows = 0;
    ert.actionscript_state = 0;

    /* Reset window system (covers WINDOW_HEAD/TAIL, window_stats, open_window_table) */
    window_system_init();

    /* Clear BG2_BUFFER: assembly clears 0x380 words = 0x700 bytes (28 rows of tilemap) */
    memset(win.bg2_buffer, 0, 0x700);

    /* Reset VWF state (covers VWF_TILE_BUFFER, VWF_ACTIVE, VWF_X, VWF_TILE) */
    vwf_init();
    vwf_frame_reset();

    /* Reset text system state */
    dt.blinking_triangle_flag = 0;
    dt.text_sound_mode = 1;
    bt.battle_mode_flag = 0;
    dt.text_prompt_waiting_for_input = 0;
    win.current_focus_window = WINDOW_ID_NONE;

    /* USA-only: reset BG2 tile usage map and VWF text alignment state */
    init_used_bg2_tile_map();

    /* Load all battle text data blocks (EBATTLE0-9) */
    display_text_load_battle_text();
}


/*
 * INITIALIZE_BATTLE_PARTY (asm/battle/ui/initialize_battle_party.asm)
 * Debug battle initialization: resets all 4 characters to level 1,
 * levels them up to param, fully heals HP/PP, and clears afflictions.
 * param = target level for all characters.
 */
void initialize_battle_party(uint16_t param) {
    initialize_battle_ui_state();
    bt.battle_mode_flag = 1;

    for (uint16_t char_id = 1; char_id <= 4; char_id++) {
        reset_char_level_one(char_id, param, 1);
        recover_hp_amtpercent(char_id, 100, 0);
        recover_pp_amtpercent(char_id, 100, 0);

        uint16_t char_index = char_id - 1;
        CharStruct *ch = &party_characters[char_index];
        ch->current_hp = ch->current_hp_target;
        ch->current_pp = ch->current_pp_target;
        memset(ch->afflictions, 0, AFFLICTION_GROUP_COUNT);
    }
}


/*
 * REDIRECT_SHOW_HPPP_WINDOWS (asm/text/show_hppp_windows_redirect.asm)
 * Far wrapper for SHOW_HPPP_WINDOWS.
 */
void redirect_show_hppp_windows(void) {
    show_hppp_windows();
}


/*
 * REDIRECT_CLOSE_FOCUS_WINDOW (asm/text/close_focus_window_redirect.asm)
 * Far wrapper for CLOSE_FOCUS_WINDOW.
 */
void redirect_close_focus_window(void) {
    close_focus_window();
}


/*
 * CLEAR_FOCUS_WINDOW_CONTENT_FAR (asm/text/window/clear_focus_window_content_redirect.asm)
 *
 * Far wrapper for CLEAR_FOCUS_WINDOW_CONTENT, which clears
 * CLEAR_WINDOW_CONTENT(CURRENT_FOCUS_WINDOW).
 * Clears the window's text content and resets cursor position.
 */
void clear_focus_window_content_far(void) {
    if (win.current_focus_window == WINDOW_ID_NONE)
        return;
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w)
        return;

    /* Clear per-window content tilemap (assembly: fill with tile 64) */
    {
        uint16_t cw = w->width - 2;
        uint16_t itr = w->height - 2;
        uint16_t total = cw * itr;
        if (total > w->content_tilemap_size) total = w->content_tilemap_size;
        for (uint16_t i = 0; i < total; i++) {
            free_tile_safe(w->content_tilemap[i]);
            w->content_tilemap[i] = 0;
        }
    }

    w->text_x = 0;
    w->text_y = 0;
    w->cursor_pixel_x = 0;
    vwf_init();
}


/*
 * SELECT_BATTLE_MENU_CHARACTER_FAR (asm/battle/select_battle_menu_character.asm)
 * Far wrapper — delegates to select_battle_menu_character() in window.c.
 */
void select_battle_menu_character_far(uint16_t party_slot) {
    select_battle_menu_character(party_slot);
}


/*
 * CLEAR_BATTLE_MENU_CHARACTER_INDICATOR_FAR (asm/text/clear_battle_menu_character_indicator_redirect.asm)
 * Far wrapper — delegates to clear_battle_menu_character_indicator() in window.c.
 */
void clear_battle_menu_character_indicator_far(void) {
    clear_battle_menu_character_indicator();
}


/*
 * SET_CURRENT_ITEM_FAR (src/inventory/set_current_item_redirect.asm)
 * Far wrapper for SET_CURRENT_ITEM — sets CITEM for battle text display.
 */
void set_current_item_far(uint8_t item) {
    set_current_item(item);
}


/*
 * LOAD_ATTACK_PALETTE (asm/battle/effects/load_attack_palette.asm)
 *
 * Loads or generates attack ert.palettes into sub-ert.palettes 12-15.
 * type == 0: darkens sub-ert.palettes 8-11 into 12-15 (divides each color by 4).
 * type != 0: copies attack palette data from ATTACK_TYPE_PALETTES into 12-15.
 * BPP4PALETTE_SIZE = 32 bytes = 16 colors per sub-palette.
 */
void load_attack_palette(uint16_t type) {
    if (type == 0) {
        /* Darken sub-ert.palettes 8-11 → 12-15.
         * Each SNES color is 15-bit (0bbbbbgggggrrrrr).
         * LSR twice = divide entire word by 4, then AND to isolate
         * the top 3 bits of each 5-bit channel. */
        for (int i = 128; i < 192; i++) {
            ert.palettes[i + 64] = (ert.palettes[i] >> 2) & 0x1CE7;
        }
        set_palette_upload_mode(16);
    } else {
        /* Load attack palette from ATTACK_TYPE_PALETTES asset.
         * Each attack type is 32 bytes (16 colors). Copies the same
         * palette to sub-ert.palettes 12, 13, 14, and 15. */
        static const uint8_t *attack_palettes = NULL;
        if (!attack_palettes) {
            attack_palettes = ASSET_DATA(ASSET_DATA_ATTACK_TYPE_PALETTES_BIN);
        }
        uint16_t *src = (uint16_t *)(attack_palettes + (type - 1) * 32);
        for (int sp = 12; sp <= 15; sp++) {
            memcpy(&ert.palettes[sp * 16], src, 32);
        }
        set_palette_upload_mode(16);
    }
}


/*
 * UPDATE_BATTLE_SCREEN_EFFECTS — Port of asm/battle/effects/update_battle_screen_effects.asm (296 lines).
 *
 * BUILD_LETTERBOX_HDMA_TABLE — Port of asm/battle/build_letterbox_hdma_table.asm (60 lines).
 *
 * Builds the HDMA scanline table used for the letterbox (top/bottom black
 * bars) effect in battle. The table consists of 3-byte entries:
 *   byte 0: scanline count (max 0x7F per entry)
 *   bytes 1-2: 16-bit value (visible or nonvisible screen mode)
 *
 * Layout:
 *   1) top_end scanlines → nonvisible value
 *   2) (bottom_start - top_end) scanlines → visible value (chunked at 0x7F)
 *   3) 1 scanline → nonvisible value
 *   4) terminator (count = 0)
 */
void build_letterbox_hdma_table(void) {
    uint16_t idx = 0;

    /* Top black bar */
    bt.letterbox_hdma_table[idx++] = (uint8_t)bt.letterbox_top_end;
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_nonvisible_screen_value & 0xFF);
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_nonvisible_screen_value >> 8);

    /* Visible section (split into 0x7F-scanline chunks) */
    uint16_t remaining = bt.letterbox_bottom_start - bt.letterbox_top_end;
    while (remaining >= 0x80) {
        bt.letterbox_hdma_table[idx++] = 0x7F;
        bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_visible_screen_value & 0xFF);
        bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_visible_screen_value >> 8);
        remaining -= 0x7F;
    }
    /* Final visible chunk */
    bt.letterbox_hdma_table[idx++] = (uint8_t)remaining;
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_visible_screen_value & 0xFF);
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_visible_screen_value >> 8);

    /* Bottom black bar (1 scanline) */
    bt.letterbox_hdma_table[idx++] = 1;
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_nonvisible_screen_value & 0xFF);
    bt.letterbox_hdma_table[idx++] = (uint8_t)(bt.letterbox_nonvisible_screen_value >> 8);

    /* Terminator */
    bt.letterbox_hdma_table[idx++] = 0;
}

/*
 * Apply the letterbox HDMA table to the PPU per-scanline TM/TS arrays.
 * Must be called after build_letterbox_hdma_table().
 */
void apply_letterbox_to_ppu(void) {
    /* No letterbox when top_end == 0 (style 0 or fully opened) */
    if (bt.letterbox_top_end == 0) {
        ppu.tm_hdma_active = false;
        return;
    }
    ppu.tm_hdma_active = true;
    int scanline = 0;
    int idx = 0;
    while (bt.letterbox_hdma_table[idx] != 0 && scanline < SNES_HEIGHT) {
        int count = bt.letterbox_hdma_table[idx++];
        uint8_t tm_val = bt.letterbox_hdma_table[idx++];
        uint8_t ts_val = bt.letterbox_hdma_table[idx++];
        for (int i = 0; i < count && scanline < SNES_HEIGHT; i++, scanline++) {
            ppu.tm_per_scanline[scanline] = tm_val;
            ppu.ts_per_scanline[scanline] = ts_val;
        }
    }
    /* Fill remaining scanlines with nonvisible value */
    uint8_t nv_tm = (uint8_t)(bt.letterbox_nonvisible_screen_value & 0xFF);
    uint8_t nv_ts = (uint8_t)(bt.letterbox_nonvisible_screen_value >> 8);
    for (; scanline < SNES_HEIGHT; scanline++) {
        ppu.tm_per_scanline[scanline] = nv_tm;
        ppu.ts_per_scanline[scanline] = nv_ts;
    }
}


/*
 * UPDATE_BATTLE_SCREEN_EFFECTS — Port of asm/battle/effects/update_battle_screen_effects.asm.
 *
 * Called every frame during battle (from RENDER_FRAME_TICK when bt.battle_mode_flag set).
 * Handles: background darkening, reflect/green-bg flashes, vertical shake,
 * horizontal wobble, horizontal shake, BG scroll offset application,
 * minimum wait frame countdown, battle sprite rendering, BG frame generation,
 * PSI animation update, red/green fixed-color flashes, HPPP box blink,
 * swirl effect, battle sprite palette animation, and letterbox ending.
 */
void update_battle_screen_effects(void) {
    /* -- Background darkening (battle intro fade) -- */
    if (bt.enable_background_darkening) {
        bt.background_brightness -= 0x0555;
        /* 16-bit value sign-extended to 32-bit for comparison */
        if ((int32_t)(int16_t)bt.background_brightness < (int32_t)0x6000) {
            bt.background_brightness = 0x6000;
            bt.enable_background_darkening = 0;
        }
        /* ASR8_POSITIVE with Y=8: logical shift right by 8 */
        uint16_t darken_level = bt.background_brightness >> 8;
        interpolate_bg_palette_colors(darken_level);
    }

    /* -- Reflect flash (shield reflect visual) -- */
    if (bt.reflect_flash_duration) {
        bt.reflect_flash_duration--;
        if (bt.reflect_flash_duration & 0x0002) {
            /* Bright frame: full white */
            interpolate_bg_palette_colors(0xFFFF);
        } else {
            /* Dim frame: restore from backup */
            interpolate_bg_palette_colors(0x0100);
        }
    }

    /* -- Green background flash (PSI shield) -- */
    if (bt.green_background_flash_duration) {
        ert.palettes[0] = 0;  /* clear BG color to black */
        if (bt.green_background_flash_duration == 3) {
            ert.palettes[0] = 0x03E0;  /* pure green in 15-bit BGR */
            set_palette_upload_mode(0x18);
        } else if (bt.green_background_flash_duration == 2) {
            set_palette_upload_mode(0x18);
        }
        bt.green_background_flash_duration--;
        if (bt.green_background_flash_duration & 0x0002) {
            interpolate_bg_palette_colors(0);
        } else {
            interpolate_bg_palette_colors(0x0100);
        }
    }

    /* -- Vertical shake (damage taken) -- */
    if (bt.vertical_shake_duration) {
        /* Index into shake amplitude table: index = 60 - duration */
        uint16_t index = 60 - bt.vertical_shake_duration;
        /* Assembly: sign-extends table byte to int16_t
         * via (val & 0xFF) - 0x80) ^ 0xFF80 */
        static const uint8_t *shake_amplitude_table = NULL;
        if (!shake_amplitude_table) {
            shake_amplitude_table = ASSET_DATA(ASSET_DATA_VERTICAL_SHAKE_AMPLITUDE_TABLE_BIN);
        }
        bt.screen_effect_vertical_offset = (int16_t)(int8_t)shake_amplitude_table[index];

        bt.vertical_shake_duration--;
        if (bt.vertical_shake_duration == 0 && bt.vertical_shake_hold_duration) {
            bt.vertical_shake_hold_duration--;
            bt.vertical_shake_duration = 10;  /* SIXTH_OF_A_SECOND */
        }
    } else {
        bt.screen_effect_vertical_offset = 0;
    }

    /* -- Horizontal wobble (wavy screen effect) -- */
    bt.screen_effect_horizontal_offset = 0;
    if (bt.wobble_duration) {
        /* Assembly: phase = duration % 72; index = (phase * 256) / 72;
         * sine value sign-extended, then * 64 / 256 (signed DIVISION16) */
        uint16_t phase = bt.wobble_duration % 72;
        bt.wobble_duration--;
        int16_t sine_index = (phase * 256) / 72;
        int16_t sine_val = (int16_t)sine_table[sine_index];
        bt.screen_effect_horizontal_offset = (sine_val * 64) / 256;
    }

    /* -- Horizontal shake (short left-right shake) -- */
    if (bt.shake_duration) {
        uint16_t phase = bt.shake_duration & 0x0003;
        bt.shake_duration--;
        switch (phase) {
        case 0: case 2:
            bt.screen_effect_horizontal_offset = 0;
            break;
        case 1:
            bt.screen_effect_horizontal_offset = 2;
            break;
        case 3:
            bt.screen_effect_horizontal_offset = -2;
            break;
        }
    }

    /* -- Apply screen offsets to BG layers -- */
    if ((loaded_bg_data_layer1.bitdepth & 0xFF) == 2) {
        /* 2bpp mode: offsets apply to BG1 */
        ppu.bg_hofs[0] = bt.screen_effect_horizontal_offset;
        ppu.bg_vofs[0] = bt.screen_effect_vertical_offset;
    } else if (bt.battle_mode_flag) {
        /* 4bpp mode: offsets apply to BG3 (text layer) */
        ppu.bg_hofs[2] = bt.screen_effect_horizontal_offset;
        ppu.bg_vofs[2] = bt.screen_effect_vertical_offset;
    }

    /* -- Minimum wait frame countdown -- */
    if (bt.screen_effect_minimum_wait_frames) {
        bt.screen_effect_minimum_wait_frames--;
    }

    /* -- Render battle sprites (enemies on screen) -- */
    if (bt.battle_mode_flag) {
        render_all_battle_sprites();
    }

    /* -- Generate battle background animation frames --
     * Assembly (C2DB3F.asm lines 176-187): always calls for layer 1,
     * conditionally calls for layer 2 if target_layer != 0. */
    generate_battlebg_frame(&loaded_bg_data_layer1, 0);
    if ((loaded_bg_data_layer2.target_layer & 0xFF) != 0) {
        generate_battlebg_frame(&loaded_bg_data_layer2, 1);
    }

    /* -- PSI animation update -- */
    update_psi_animation();

    /* -- Red flash (damage dealt) -- */
    if (bt.red_flash_duration) {
        bt.red_flash_duration--;
        if ((bt.red_flash_duration / 12) & 1) {
            /* Flash on: red tint via fixed color */
            set_coldata(31, 0, 4);
            set_colour_addsub_mode(0, 0x3F);
        } else {
            /* Flash off: clear fixed color, restore normal color math */
            set_coldata(0, 0, 0);
            set_color_math_from_table(bt.current_layer_config);
        }
    }

    /* -- Green flash (healing) -- */
    if (bt.green_flash_duration) {
        bt.green_flash_duration--;
        if ((bt.green_flash_duration / 12) & 1) {
            /* Flash on: green tint via fixed color */
            set_coldata(0, 31, 4);
            set_colour_addsub_mode(0, 0x3F);
        } else {
            /* Flash off: clear fixed color, restore normal color math */
            set_coldata(0, 0, 0);
            set_color_math_from_table(bt.current_layer_config);
        }
    }

    /* -- HPPP box blink (target indicator) -- */
    if (bt.hp_pp_box_blink_duration) {
        bt.hp_pp_box_blink_duration--;
        if ((bt.hp_pp_box_blink_duration / 3) & 1) {
            /* Blink off: undraw the HP/PP window */
            undraw_hp_pp_window(bt.hp_pp_box_blink_target);
        } else {
            /* Blink on: redraw the HP/PP window */
            draw_and_mark_hppp_window(bt.hp_pp_box_blink_target);
        }
    }

    /* -- Swirl effect (battle entry/exit) -- */
    update_swirl_effect();

    /* -- Battle sprite palette animation -- */
    update_battle_sprite_palette_anim();

    /* -- Letterbox ending (post-battle letterbox shrink) -- */
    if (bt.letterbox_effect_ending && bt.letterbox_top_end) {
        if (bt.letterbox_effect_ending_top < 0x03BB) {
            /* Letterbox fully open */
            bt.letterbox_effect_ending_top = 0;
            bt.letterbox_effect_ending_bottom = 224;
            bt.letterbox_effect_ending = 0;
        } else {
            /* Shrink letterbox */
            bt.letterbox_effect_ending_top -= 0x03BB;
            bt.letterbox_effect_ending_bottom += 0x03BB;
        }
        /* ASR8: top_pixels = bt.letterbox_effect_ending_top >> 8 */
        uint16_t top_pixels = bt.letterbox_effect_ending_top >> 8;
        if (top_pixels < bt.letterbox_top_end) {
            bt.letterbox_top_end = top_pixels;
        }
        /* ASR8: bottom_pixels = bt.letterbox_effect_ending_bottom >> 8 */
        uint16_t bottom_pixels = bt.letterbox_effect_ending_bottom >> 8;
        if (bottom_pixels > bt.letterbox_bottom_start) {
            bt.letterbox_bottom_start = bottom_pixels;
        }
        build_letterbox_hdma_table();
#if BATTLE_LETTERBOX_ENABLED
        apply_letterbox_to_ppu();
#endif
    }

#if BATTLE_LETTERBOX_ENABLED
    /* When letterbox fully opens, disable per-scanline override */
    if (!bt.letterbox_effect_ending && !bt.letterbox_top_end) {
        ppu.tm_hdma_active = false;
    }
#endif
}


/*
 * WAIT_AND_UPDATE_BATTLE_EFFECTS — Port of asm/battle/wait_and_update_battle_effects.asm
 * and its far wrapper C43568.asm.
 *
 * Waits one frame then processes all battle screen effects.
 * Called during battle loops that need to advance animation while waiting.
 */
void wait_and_update_battle_effects(void) {
    wait_for_vblank();
    update_battle_screen_effects();
}

