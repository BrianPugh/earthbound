/*
 * Town map display system.
 *
 * Ports of:
 *   SHOW_TOWN_MAP          (asm/overworld/show_town_map.asm)
 *   DISPLAY_TOWN_MAP       (asm/overworld/display_town_map.asm)
 *   RUN_TOWN_MAP_MENU      (asm/text/menu/run_town_map_menu.asm)
 *   GET_TOWN_MAP_ID        (asm/overworld/get_town_map_id.asm)
 *   LOAD_TOWN_MAP_DATA     (asm/overworld/load_town_map_data.asm)
 *   RENDER_TOWN_MAP_ICONS  (asm/overworld/map/render_town_map_icons.asm)
 *   UPDATE_TOWN_MAP_PLAYER_ICON (asm/overworld/map/update_town_map_player_icon.asm)
 *   CYCLE_MAP_ICON_PALETTE (asm/system/palette/cycle_map_icon_palette.asm)
 */
#include "game/town_map.h"
#include "game/map_loader.h"
#include "game/overworld.h"
#include "game/game_state.h"
#include "game/fade.h"
#include "game/flyover.h"
#include "game/inventory.h"
#include "game/battle.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "data/assets.h"
#include "include/binary.h"
#include "include/pad.h"
#include "snes/ppu.h"
#include "platform/platform.h"
#include <string.h>

#include "game_main.h"

/* Town map item ID */
#define ITEM_TOWN_MAP 0xCA

/* TOWN_MAP_LABEL_GFX_SIZE for US retail (include/config.asm line 149) */
#define TOWN_MAP_LABEL_GFX_SIZE 0x2400

/* Number of town maps */
#define TOWN_MAP_COUNT 6

/* Within-bank base address of TOWN_MAP_ICON_SPRITEMAPS spritemap data.
 * Pointer table entries are within-bank addresses; subtract this to get ert.buffer offset. */
#define SPRITEMAP_ROM_BASE 0xF203

/* Forward declarations */
static uint16_t get_town_map_id(uint16_t leader_x, uint16_t leader_y);
static void load_town_map_data(uint16_t map_index);
static void render_town_map_icons(uint16_t map_index);
static void update_town_map_player_icon(void);
static void cycle_map_icon_palette(void);

/* Town map animation state (RAM variables from ram.asm) */
static uint16_t town_map_animation_frame;
static uint16_t town_map_player_icon_animation_frame;
static uint16_t frames_until_map_icon_palette_update;

/* Helper: look up spritemap ert.buffer offset from a TOWN_MAP_MAPPING entry.
 * The mapping entry is an index into TOWN_MAP_ICON_SPRITEMAP_PTR_TABLE. The pointer table
 * contains within-bank addresses; subtract SPRITEMAP_ROM_BASE for ert.buffer offset. */
static uint16_t smap_offset_from_mapping(const uint8_t *smap_ptrs, size_t ptrs_size,
                                          uint16_t map_entry) {
    uint16_t ptr_idx = map_entry * 2;  /* ASL in assembly */
    if (ptr_idx + 1 >= ptrs_size) return 0;
    uint16_t rom_addr = read_u16_le(&smap_ptrs[ptr_idx]);
    return rom_addr - SPRITEMAP_ROM_BASE;
}

/*
 * GET_TOWN_MAP_ID (asm/overworld/get_town_map_id.asm)
 *
 * Returns the raw sector byte for the given world coordinates.
 * The low nibble (AND #$000F) is the town map ID (1-6, 0=none).
 */
static uint16_t get_town_map_id(uint16_t leader_x, uint16_t leader_y) {
    uint16_t x_sector = (leader_x >> 8) & 0xFF;
    uint16_t x_offset = x_sector * 3;
    uint16_t y_sector = leader_y / 128;
    uint16_t y_offset = y_sector * 96;
    uint16_t index = y_offset + x_offset;

    size_t data_size = ASSET_SIZE(ASSET_DATA_PER_SECTOR_TOWN_MAP_BIN);
    const uint8_t *data = ASSET_DATA(ASSET_DATA_PER_SECTOR_TOWN_MAP_BIN);
    if (!data || index >= data_size) {
        return 0;
    }

    uint16_t result = data[index] & 0xFF;
    return result;
}

/*
 * LOAD_TOWN_MAP_DATA (asm/overworld/load_town_map_data.asm)
 *
 * Loads graphics, ert.palettes, and tilemaps for the specified town map.
 * map_index: 0-5 (Onett, Twoson, Threed, Fourside, Scaraba, Summers)
 */
static void load_town_map_data(uint16_t map_index) {
    fade_out(2, 1);

    /* Decompress town map GFX into ert.buffer.
     * Maps 2 and 4 are locale-specific (US/town_maps/ in ebtools).
     * The ASSET_TOWN_MAPS family macro handles locale aliases. */
    size_t gfx_size = ASSET_SIZE(ASSET_TOWN_MAPS(map_index));
    const uint8_t *gfx_data = ASSET_DATA(ASSET_TOWN_MAPS(map_index));

    if (gfx_data) {
        decomp(gfx_data, gfx_size, ert.buffer, BUFFER_SIZE);
    }

    /* Wait for fade to complete */
    while (fade_active()) {
        wait_for_vblank();
    }

    /* Copy first 2 ert.palettes (64 bytes) from BUFFER to PALETTES */
    memcpy(ert.palettes, ert.buffer, BPP4PALETTE_SIZE * 2);

    /* Load icon palette to palette groups 8+.
     * Assembly copies BPP4PALETTE_SIZE*8=256 bytes, but the actual palette file
     * is only 64 bytes (2 sub-ert.palettes). The remaining bytes would read into
     * adjacent ROM data — harmless since palette groups 10-15 are unused. */
    size_t pal_size = ASSET_SIZE(ASSET_TOWN_MAPS_ICON_PAL);
    const uint8_t *icon_pal = ASSET_DATA(ASSET_TOWN_MAPS_ICON_PAL);
    if (icon_pal) {
        memcpy((uint8_t *)ert.palettes + BPP4PALETTE_SIZE * 8, icon_pal, pal_size);
    }

    /* SET_BG1_VRAM_LOCATION(Y=$0000, X=$3000, A=NORMAL) */
    ppu.bg_sc[0] = (0x3000 >> 8) | 0x00;
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | ((0x0000 >> 12) & 0x0F);

    /* SET_OAM_SIZE(3) */
    ppu.obsel = 0x03;

    /* Clear color math, set display layers */
    ppu.cgadsub = 0x00;
    ppu.cgwsel = 0x00;
    ppu.tm = 0x01;   /* BG1 only */
    ppu.ts = 0x00;

    /* Upload tilemap: BUFFER+$40 → VRAM $3000, $800 bytes */
    memcpy(&ppu.vram[0x3000 * 2], ert.buffer + 0x40, 0x800);

    /* Upload tile data: BUFFER+$840 → VRAM $0000, $4000 bytes */
    memcpy(&ppu.vram[0x0000], ert.buffer + 0x840, 0x4000);

    /* Decompress label GFX directly to VRAM $6000 */
    size_t label_size = ASSET_SIZE(ASSET_TOWN_MAPS_LABEL_GFX_LZHAL);
    const uint8_t *label_data = ASSET_DATA(ASSET_TOWN_MAPS_LABEL_GFX_LZHAL);
    if (label_data) {
        decomp(label_data, label_size, &ppu.vram[0x6000 * 2], TOWN_MAP_LABEL_GFX_SIZE);
    }

    ert.palette_upload_mode = 24;
    ppu.tm = 0x11;  /* BG1 + OBJ */
    ppu.bg_hofs[0] = 0;  /* BG1_X_POS */
    ppu.bg_vofs[0] = 0;  /* BG1_Y_POS */
    update_screen();
    fade_in(2, 1);
}

/*
 * CYCLE_MAP_ICON_PALETTE (asm/system/palette/cycle_map_icon_palette.asm)
 *
 * Every 12 frames, rotates colors 1-7 of palette group 8 (indices 129-135).
 * The first color is saved, all shift left by one, saved goes to the end.
 */
static void cycle_map_icon_palette(void) {
    if (frames_until_map_icon_palette_update == 0) {
        frames_until_map_icon_palette_update = 12;

        /* Save color 1 of palette 8 (index 128+1 = 129) */
        uint16_t saved = ert.palettes[129];

        /* Shift colors left: [129] ← [130], [130] ← [131], ..., [134] ← [135] */
        for (int i = 129; i < 135; i++) {
            ert.palettes[i] = ert.palettes[i + 1];
        }

        /* Wrap: color 7 (index 135) ← saved */
        ert.palettes[135] = saved;

        ert.palette_upload_mode = 16;
    }

    frames_until_map_icon_palette_update--;
}

/*
 * UPDATE_TOWN_MAP_PLAYER_ICON (asm/overworld/map/update_town_map_player_icon.asm)
 *
 * Draws the player's position icon and directional arrows on the town map.
 * Uses MAP_DATA_PER_SECTOR_TOWN_MAP_DATA to look up the player's pixel
 * position and direction on the current map.
 */
static void update_town_map_player_icon(void) {
    uint16_t leader_x = game_state.leader_x_coord;
    uint16_t x_sector = (leader_x >> 8) & 0xFF;
    uint16_t leader_y = game_state.leader_y_coord;
    uint16_t y_div = leader_y / 128;

    size_t sector_size = ASSET_SIZE(ASSET_DATA_PER_SECTOR_TOWN_MAP_BIN);
    const uint8_t *sector_data = ASSET_DATA(ASSET_DATA_PER_SECTOR_TOWN_MAP_BIN);
    if (!sector_data) return;

    size_t mapping_size = ASSET_SIZE(ASSET_TOWN_MAPS_MAPPING_BIN);
    const uint8_t *mapping_raw = ASSET_DATA(ASSET_TOWN_MAPS_MAPPING_BIN);
    if (!mapping_raw) { return; }

    size_t ptrs_size = ASSET_SIZE(ASSET_TOWN_MAPS_ICON_SPRITEMAP_PTRS_BIN);
    const uint8_t *smap_ptrs = ASSET_DATA(ASSET_TOWN_MAPS_ICON_SPRITEMAP_PTRS_BIN);
    if (!smap_ptrs) { return; }

    /* Compute sector index: x_sector * 3 + y_div * 96 */
    uint16_t sector_idx = x_sector * 3 + y_div * 96;

    /* Read pixel position and direction from sector data.
     * Each sector entry is 3 bytes: [flags_and_dir, pixel_x, pixel_y] */
    uint16_t pixel_x = 0, pixel_y = 0, dir_flags = 0;
    if (sector_idx + 2 < sector_size) {
        dir_flags = sector_data[sector_idx] & 0x70;
        pixel_x = sector_data[sector_idx + 1] & 0xFF;
        pixel_y = sector_data[sector_idx + 2] & 0xFF;
    }

    /* TOWN_MAP_MAPPING: 6 words [small_icon, big_icon, up_arrow, down_arrow, left_arrow, right_arrow] */
    uint16_t mapping[6] = {0};
    for (int i = 0; i < 6 && (size_t)(i * 2 + 1) < mapping_size; i++) {
        mapping[i] = read_u16_le(&mapping_raw[i * 2]);
    }

    /* Draw direction arrow if applicable */
    if (dir_flags != 0) {
        uint16_t smap_off;
        if (dir_flags == (1 << 4)) {
            /* UP arrow: draw above player (Y - 8) */
            smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[2]);
            queue_sprite_draw(smap_off, (int16_t)pixel_x, (int16_t)(pixel_y - 8),
                              ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
        } else if (dir_flags == (2 << 4)) {
            /* DOWN arrow: draw below player (Y + 8) */
            smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[3]);
            queue_sprite_draw(smap_off, (int16_t)pixel_x, (int16_t)(pixel_y + 8),
                              ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
        } else if (dir_flags == (4 << 4)) {
            /* LEFT arrow: draw left of player (X - 8) */
            smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[4]);
            queue_sprite_draw(smap_off, (int16_t)(pixel_x - 8), (int16_t)pixel_y,
                              ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
        } else if (dir_flags == (3 << 4)) {
            /* RIGHT arrow: draw right of player (X + 16) */
            smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[5]);
            queue_sprite_draw(smap_off, (int16_t)(pixel_x + 16), (int16_t)pixel_y,
                              ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
        }
    }

    /* Draw player icon: big (frames 0-9) or small (frames 10-19) */
    if (town_map_player_icon_animation_frame < 10) {
        uint16_t smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[1]);
        queue_sprite_draw(smap_off, (int16_t)pixel_x, (int16_t)pixel_y,
                          ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
    } else {
        uint16_t smap_off = smap_offset_from_mapping(smap_ptrs, ptrs_size, mapping[0]);
        queue_sprite_draw(smap_off, (int16_t)pixel_x, (int16_t)pixel_y,
                          ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
    }

    /* Decrement animation frame, wrap 0 → 20 */
    town_map_player_icon_animation_frame--;
    if (town_map_player_icon_animation_frame == 0) {
        town_map_player_icon_animation_frame = 20;
    }

}

/*
 * RENDER_TOWN_MAP_ICONS (asm/overworld/map/render_town_map_icons.asm)
 *
 * Renders all town map icons for the specified map, plus the player icon.
 * map_index: 0-5
 */
static void render_town_map_icons(uint16_t map_index) {
    ert.current_sprite_drawing_priority = 0;

    size_t placement_size = ASSET_SIZE(ASSET_TOWN_MAPS_ICON_PLACEMENT_BIN);
    const uint8_t *placement_data = ASSET_DATA(ASSET_TOWN_MAPS_ICON_PLACEMENT_BIN);
    if (!placement_data) return;

    size_t flags_size = ASSET_SIZE(ASSET_TOWN_MAPS_ICON_ANIMATION_FLAGS_BIN);
    const uint8_t *anim_flags = ASSET_DATA(ASSET_TOWN_MAPS_ICON_ANIMATION_FLAGS_BIN);
    if (!anim_flags) { return; }

    size_t ptrs_size = ASSET_SIZE(ASSET_TOWN_MAPS_ICON_SPRITEMAP_PTRS_BIN);
    const uint8_t *smap_ptrs = ASSET_DATA(ASSET_TOWN_MAPS_ICON_SPRITEMAP_PTRS_BIN);
    if (!smap_ptrs) { return; }

    /* Find the start of entry map_index by scanning $FF terminators */
    const uint8_t *entry = placement_data;
    const uint8_t *end = placement_data + placement_size;
    for (uint16_t i = 0; i < map_index && entry < end; ) {
        if (*entry == 0xFF) { i++; entry++; }
        else { entry += 5; }
    }

    /* Iterate through icon entries (5 bytes each, $FF terminated) */
    while (entry < end && *entry != 0xFF) {
        uint8_t icon_x = entry[0];      /* screen X position */
        uint8_t icon_y = entry[1];      /* screen Y position */
        uint8_t icon_type = entry[2];   /* icon type (indexes into spritemap/anim tables) */
        uint16_t event_flag_raw = read_u16_le(&entry[3]);

        /* Check animation: if this icon type has anim flag set,
         * hide it during the blink-off phase (frames 0-9 of 60-frame cycle) */
        bool should_draw = true;
        if (icon_type < flags_size && anim_flags[icon_type] != 0) {
            if (town_map_animation_frame < 10) {
                should_draw = false;
            }
        }

        /* Check event flag visibility.
         * Bit 15 = inversion: 0 = show when flag is CLEAR, 1 = show when flag is SET */
        uint16_t invert = (event_flag_raw & 0x8000) ? 1 : 0;
        uint16_t flag_id = event_flag_raw & 0x7FFF;
        uint16_t flag_value = event_flag_get(flag_id) ? 1 : 0;
        if (flag_value != invert) {
            should_draw = false;
        }

        if (should_draw) {
            uint16_t ptr_idx = icon_type * 2;
            if (ptr_idx + 1 < ptrs_size) {
                uint16_t rom_addr = read_u16_le(&smap_ptrs[ptr_idx]);
                uint16_t smap_off = rom_addr - SPRITEMAP_ROM_BASE;
                queue_sprite_draw(smap_off, (int16_t)icon_x, (int16_t)icon_y,
                                  ert.current_sprite_drawing_priority, SMAP_BANK_TOWNMAP_ID);
            }
        }

        entry += 5;
    }

    update_town_map_player_icon();

    /* Decrement animation frame, wrap 0 → 60 */
    town_map_animation_frame--;
    if (town_map_animation_frame == 0) {
        town_map_animation_frame = 60;
    }

    cycle_map_icon_palette();

}

/*
 * DISPLAY_TOWN_MAP (asm/overworld/display_town_map.asm)
 *
 * Displays the town map based on the leader's current position.
 * Returns the map_id that was displayed (0 if none).
 */
uint16_t display_town_map(void) {
    town_map_animation_frame = 60;
    town_map_player_icon_animation_frame = 20;
    frames_until_map_icon_palette_update = 12;

    uint16_t raw_id = get_town_map_id(game_state.leader_x_coord,
                                       game_state.leader_y_coord);
    uint16_t map_id = raw_id & 0x000F;

    if (map_id == 0)
        return map_id;


    load_town_map_data(map_id - 1);

    /* Main render loop — exits on any button press */
    for (;;) {
        if (platform_input_quit_requested()) break;
        wait_for_vblank();
        oam_clear();
        render_town_map_icons(map_id - 1);
        update_screen();

        if (core.pad1_pressed & PAD_CONFIRM)
            break;
        if (core.pad1_pressed & PAD_CANCEL)
            break;
        if (core.pad1_pressed & PAD_X)
            break;
    }

    /* Fade out while continuing to render for 16 frames */
    fade_out(2, 1);
    for (uint16_t i = 0; i < 16; i++) {
        if (platform_input_quit_requested()) break;
        wait_for_vblank();
        oam_clear();
        render_town_map_icons(map_id - 1);
        update_screen();
    }

    /* Reload the normal map display */
    ow.disable_music_changes = 1;
    reload_map();
    ml.current_map_music_track = ml.next_map_music_track;
    undraw_flyover_text();
    ow.disable_music_changes = 0;

    ppu.tm = 0x17;
    fade_in(2, 1);


    return map_id;
}

/*
 * SHOW_TOWN_MAP (asm/overworld/show_town_map.asm)
 *
 * Entry point from overworld X button: checks for Town Map item, shows map.
 */
void show_town_map(void) {
    if (find_item_in_inventory2(CHAR_ID_ANY, ITEM_TOWN_MAP) == 0)
        return;

    disable_all_entities();
    display_town_map();
    enable_all_entities();
}

/*
 * RUN_TOWN_MAP_MENU (asm/text/menu/run_town_map_menu.asm)
 *
 * Town map display with up/down navigation between maps.
 * Used when selecting Town Map from the items menu.
 */
void run_town_map_menu(void) {
    uint16_t current_map = 0;
    uint16_t prev_map = 0;
    town_map_animation_frame = 60;
    town_map_player_icon_animation_frame = 20;
    frames_until_map_icon_palette_update = 12;


    load_town_map_data(current_map);

    for (;;) {
        if (platform_input_quit_requested()) break;
        wait_for_vblank();
        oam_clear();

        /* Handle UP/DOWN navigation */
        if (core.pad1_pressed & PAD_UP) {
            if (current_map == 0)
                current_map = TOWN_MAP_COUNT;
            current_map--;
        }
        if (core.pad1_pressed & PAD_DOWN) {
            current_map++;
        }

        /* Wrap around: assembly checks for -1 (0xFFFF) and >= 6 */
        if (current_map >= TOWN_MAP_COUNT) {
            current_map = 0;
        }

        /* Reload if map changed */
        if (current_map != prev_map) {
            load_town_map_data(current_map);
            prev_map = current_map;
        }

        render_town_map_icons(current_map);

        if (core.pad1_pressed & PAD_A)
            break;

        update_screen();
    }

    undraw_flyover_text();
    reload_map();
    ppu.tm = 0x17;


}
