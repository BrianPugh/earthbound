/*
 * Display text menu functions.
 *
 * Extracted from display_text.c — store/shop menus, telephone,
 * escargo express, teleport destination selection, name entry,
 * sound stone, and special event dispatch.
 */
#include "game/display_text.h"
#include "game/display_text_internal.h"
#include "game/game_state.h"
#include "game/battle.h"
#include "game/inventory.h"
#include "game/audio.h"
#include "game/overworld.h"
#include "game/map_loader.h"
#include "game/window.h"
#include "game/text.h"
#include "game/fade.h"
#include "game/door.h"
#include "entity/entity.h"
#include "data/assets.h"
#include "core/math.h"
#include "core/memory.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "include/pad.h"
#include "platform/platform.h"
#include "game_main.h"
#include "data/battle_text_data.h"
#include "intro/file_select.h"
#include "intro/title_screen.h"
#include "game/battle_bg.h"
#include "game/town_map.h"
#include "game/ending.h"
#include "game/flyover.h"
#include "core/decomp.h"
#include <string.h>

/* --- Store table ---
 * Loaded from store_table.bin (extracted from ROM).
 * 66 shops × 7 item IDs each = 462 bytes.
 * Index: shop_id * 7 + item_slot (0-6). Value: item ID (0 = empty). */
#define STORE_ITEMS_PER_SHOP  7
#define STORE_TABLE_SHOPS     66
static const uint8_t *store_table_data;
static size_t store_table_size;

/* --- Telephone contacts table ---
 * Loaded from telephone_contacts_table.bin (extracted from ROM).
 * Each entry is 31 bytes: label[25] + event_flag[2] + text[4].
 * Matches struct telephone_contact from include/structs.asm. */
#define TELEPHONE_CONTACT_LABEL_LEN   25
#define TELEPHONE_CONTACT_ENTRY_SIZE  31
static const uint8_t *telephone_contacts_data;
static size_t telephone_contacts_size;

/* --- Status/equip window text assets ---
 * Small EB-encoded title strings used as window titles. */
static const uint8_t *status_equip_text_7;     /* "Stored Goods" (12 bytes) */
static size_t status_equip_text_7_size;
static const uint8_t *status_equip_text_14;    /* "To:" (3 bytes) */
static size_t status_equip_text_14_size;
static const uint8_t *phone_call_text_data;    /* "Call:" (5 bytes) */
static size_t phone_call_text_size;


/*
 * ENTER_YOUR_NAME_PLEASE — Port of asm/text/enter_your_name_please.asm (117 lines).
 *
 * In-game name registration dialog.  Called from event scripts when the player
 * is asked to register their real name.
 *
 * param=0: Register the EarthBound player name (earthbound_playername, 24 chars).
 *          Prompt: "Register your name, please".  Result also copied to
 *          mother2_playername (first 12 bytes).
 * param=1: Register the Mother 2 player name (mother2_playername, 12 chars).
 *          Shows the existing EB player name as context above the keyboard.
 *
 * Returns TEXT_INPUT_DIALOG result (0 = confirmed, always 0 for standalone).
 */
uint16_t enter_your_name_please(uint16_t param) {
    /* Assembly: STZ ENABLE_WORD_WRAP, ALLOW_TEXT_OVERFLOW=1, SET_INSTANT_PRINTING */
    dt.enable_word_wrap = 0;
    dt.allow_text_overflow = 1;
    set_instant_printing();

    /* Create the prompt window (WINDOW::NAMING_PROMPT = 0x27) */
    create_window(WINDOW_NAMING_PROMPT);

    uint16_t result;

    if (param != 0) {
        /* --- Mother 2 player name path (param=1) ---
         * Assembly lines 21-56: shows EB name at row 0, M2 name input at row 1. */

        /* Print existing EB player name at text position (0, 0) */
        set_focus_text_cursor(0, 0);
        /* Convert EB-encoded earthbound_playername to ASCII for print_string.
         * Assembly: PRINT_STRING with max_len=24 from game_state pointer. */
        int len = 0;
        while (len < 24 && game_state.earthbound_playername[len] != 0x00) len++;
        print_eb_string(game_state.earthbound_playername, len);

        /* Position cursor at row 1 for name input */
        set_focus_text_cursor(0, 1);

        /* Pre-fill existing M2 name if game_state byte 0 != 0
         * (assembly lines 36-43: check GAME_STATE AND #$00FF) */
        const uint8_t *existing_m2 = NULL;
        if (((uint8_t *)&game_state)[0] != 0) {
            existing_m2 = game_state.mother2_playername;
        }

        /* TEXT_INPUT_DIALOG: keyboard input for M2 name (12 chars, no Don't Care).
         * Name display goes in NAMING_PROMPT at text row 1 (below EB name). */
        result = (uint16_t)text_input_dialog(game_state.mother2_playername, 12, -1,
                                             WINDOW_NAMING_PROMPT, 1, existing_m2);
    } else {
        /* --- EarthBound player name path (param=0) ---
         * Assembly lines 58-104: shows prompt, EB name input at row 1,
         * then copies result to mother2_playername. */

        /* Print NAME_REGISTRY_REQUEST_STRING at text position (0, 0)
         * Assembly: LOADPTR NAME_REGISTRY_REQUEST_STRING, PRINT_STRING #26 */
        set_focus_text_cursor(0, 0);
        print_eb_string(ASSET_DATA(ASSET_US_DATA_NAME_REGISTRY_REQUEST_STRING_BIN), 26);

        /* Position cursor at row 1 for name input */
        set_focus_text_cursor(0, 1);

        /* Pre-fill existing EB name if non-empty (assembly lines 74-80) */
        const uint8_t *existing_eb = NULL;
        if (game_state.earthbound_playername[0] != 0) {
            existing_eb = game_state.earthbound_playername;
        }

        /* TEXT_INPUT_DIALOG: keyboard input for EB name (24 chars, no Don't Care).
         * Name display goes in NAMING_PROMPT at text row 1 (below prompt). */
        result = (uint16_t)text_input_dialog(game_state.earthbound_playername, 24, -1,
                                             WINDOW_NAMING_PROMPT, 1, existing_eb);

        /* Assembly lines 96-104: PROCESS_NAME_INPUT_STRING + MEMCPY16.
         * In US version, PROCESS_NAME_INPUT_STRING is effectively memcpy
         * (romaji-to-kana is JP only; EB codes 0x50+ never match 0x41-0x5A).
         * Copy first 12 bytes of EB name to M2 name. */
        memcpy(game_state.mother2_playername, game_state.earthbound_playername,
               sizeof(game_state.mother2_playername));
    }

    /* Cleanup (assembly @DONE, lines 105-117).
     * text_input_dialog already closed the keyboard window (0x1C). */
    close_window(WINDOW_NAMING_PROMPT);
    dt.enable_word_wrap = 0xFF;
    dt.allow_text_overflow = 0;

    return result;
}


/*
 * USE_SOUND_STONE (asm/overworld/use_sound_stone.asm, 527 lines)
 *
 * Plays the collected Sound Stone melodies in sequence with animated sprites.
 * Each melody has an idle state (visible dot at fixed position) and a playing
 * state (orbiting sprite pair with animated tile swap).
 *
 * The screen shows the sound stone graphics with 8 melody indicator sprites
 * arranged in an oval, plus a central rotating sprite.  Battle BG layers
 * 228/229 provide the animated background.
 *
 * cancellable: 0 = plays to completion, 1 = A/B/X cancels early.
 *
 * Data loaded from ROM:
 *   - sound_stone_config.bin: 119 bytes (ptr table + 8 small lookup tables)
 *   - sound_stone_melodies.bin: 992 bytes (9 contiguous melody curves)
 *   - sound_stone.gfx.lzhal: compressed graphics (0x2C00 bytes decompressed)
 *   - sound_stone.pal: 192 bytes (6 ert.palettes for melody indicators)
 */
uint16_t use_sound_stone(uint16_t cancellable) {
    /* ---- Load assets ---- */
    size_t gfx_size = ASSET_SIZE(ASSET_GRAPHICS_SOUND_STONE_GFX_LZHAL);
    const uint8_t *config_data = ASSET_DATA(ASSET_DATA_SOUND_STONE_CONFIG_BIN);
    const uint8_t *melody_data = ASSET_DATA(ASSET_DATA_SOUND_STONE_MELODIES_BIN);
    const uint8_t *gfx_comp = ASSET_DATA(ASSET_GRAPHICS_SOUND_STONE_GFX_LZHAL);
    const uint8_t *pal_data = ASSET_DATA(ASSET_SOUND_STONE_PAL);

    if (!config_data || !melody_data || !gfx_comp || !pal_data) {
        return 0;
    }

    /* Parse config blob offsets (see asm/bankconfig/US/bank04.asm lines 888-908) */
    const uint8_t *idle_x       = config_data + 36;  /* SOUND_STONE_UNKNOWN  — 8 bytes */
    const uint8_t *idle_y       = config_data + 44;  /* SOUND_STONE_UNKNOWN2 — 8 bytes */
    const uint8_t *idle_tiles   = config_data + 52;  /* SOUND_STONE_UNKNOWN3 — 8 bytes */
    const uint8_t *idle_pal     = config_data + 60;  /* SOUND_STONE_UNKNOWN4 — 8 bytes */
    const uint8_t *orbit_tiles  = config_data + 68;  /* SOUND_STONE_UNKNOWN5 — 8 bytes */
    const uint8_t *orbit_pal    = config_data + 76;  /* SOUND_STONE_UNKNOWN6 — 8 bytes */
    const uint8_t *music_ids    = config_data + 84;  /* SOUND_STONE_MUSIC    — 9 bytes */
    const uint8_t *timing_raw   = config_data + 93;  /* SOUND_STONE_UNKNOWN7+8 — 18 bytes (9 words) */
    const uint8_t *melody_flags = config_data + 111; /* SOUND_STONE_MELODY_FLAGS — 8 bytes */

    /* Parse melody pointer table → compute byte offsets within melody_data.
     * The first 36 bytes of config are 9 DWORD ROM addresses. */
    uint32_t melody_base = read_u32_le(config_data);
    uint16_t melody_offsets[9];
    for (int i = 0; i < 9; i++) {
        uint32_t ptr = read_u32_le(&config_data[i * 4]);
        melody_offsets[i] = (uint16_t)(ptr - melody_base);
    }

    /* Read timing values as 16-bit little-endian words */
    uint16_t timing[9];
    for (int i = 0; i < 9; i++)
        timing[i] = read_u16_le(&timing_raw[i * 2]);

    /* ---- Initialize screen (assembly lines 26-66) ---- */
    force_blank_and_wait_vblank();
    stop_music();
    load_enemy_battle_sprites();

    /* Decompress sound stone graphics directly to VRAM at word address 0x2000 (byte 0x4000).
     * Assembly: DECOMP, then COPY_TO_VRAM1P @VIRTUAL06, VRAM::SOUND_STONE_GFX, $2C00, 0 */
    decomp(gfx_comp, gfx_size, &ppu.vram[0x2000 * 2], 0x2C00);

    /* Copy 6 ert.palettes (192 bytes) to palette RAM at sub-palette 8.
     * Assembly: MEMCPY16 src=SOUND_STONE_PALETTE, size=BPP4PALETTE_SIZE*6,
     * dest=PALETTES + BPP4PALETTE_SIZE * 8.  BPP4PALETTE_SIZE=32 bytes=16 colors. */
    memcpy(&ert.palettes[128], pal_data, 192);

    load_character_window_palette();

    /* Load battle BG layers 228/229 (SOUNDSTONE1/2) with no letterbox (style 4).
     * Assembly: LDY #4; LDX #SOUNDSTONE2; LDA #SOUNDSTONE1; JSL LOAD_BATTLE_BG */
    load_battle_bg(228, 229, 4);

    /* Init spritemaps (assembly lines 48-66).
     * spritemap struct: { y_offset, tile, flags, x_offset, special_flags } = 5 bytes */
    uint8_t sm1[5] = {0}; /* idle/main sprite */
    uint8_t sm2[5] = {0}; /* orbit/center sprite */
    sm1[0] = 240;  /* y_offset — centers 16x16 sprite at base pos */
    sm1[3] = 240;  /* x_offset */
    sm2[0] = 248;  /* y_offset */
    sm2[3] = 248;  /* x_offset */
    sm1[4] = 0x81; /* special_flags: bit 7 = 16x16, bit 0 = hi tile name */
    sm2[4] = 0x80; /* special_flags: bit 7 = 16x16 */

    /* ---- Init melody playback state (assembly lines 68-104) ----
     * sound_stone_playback_state: { state, counter, tile_toggle,
     *   orbit_frame, orbit_pos1, orbit_pos2, pad } × 8 melodies, 14 bytes each */
    typedef struct {
        int16_t state;       /* 0=inactive, 1=idle, 2=playing */
        int16_t counter;     /* animation frame counter */
        int16_t tile_toggle; /* orbit tile frame modifier (0 or 2) */
        int16_t orbit_frame; /* index into melody data */
        int16_t orbit_pos1;  /* orbit radius/position */
        int16_t orbit_pos2;  /* orbit angle accumulator (16-bit, hi byte = angle) */
        int16_t pad;         /* unused */
    } PlaybackState;

    PlaybackState ps[8] = {{0}};
    int16_t collected_count = 0;

    for (int i = 0; i < 8; i++) {
        if (event_flag_get(melody_flags[i])) {
            ps[i].state = 1; /* idle — melody collected */
            collected_count++;
        }
        ps[i].counter = 1;
        ps[i].orbit_frame = 0;
    }

    /* Fade in (assembly lines 105-108) */
    blank_screen_and_wait_vblank();
    fade_in(1, 0);

    /* ---- Initialize loop state (assembly lines 109-120) ---- */
    int16_t center_timer = 15;     /* @LOCAL0E: center sprite animation timer */
    int16_t center_frame = 0;      /* @LOCAL0F: center sprite frame (0-3) */
    int16_t initial_delay = 60;    /* @LOCAL0D: frames before sequence starts */
    int16_t exit_countdown = 0;    /* @LOCAL0C: frames until fade-out */
    int16_t seq_index = 0;         /* @LOCAL0B: melody_sequence_index */
    int16_t timing_counter = 0;    /* @VIRTUAL04 / @LOCAL0A */
    int16_t current_melody = 0;    /* @VIRTUAL02 / @LOCAL09 */

    /* ---- Main loop (assembly lines 121-507) ---- */
    for (;;) {
        if (platform_input_quit_requested()) break;
        wait_for_vblank();
        fade_update();

        uint16_t pressed = core.pad1_pressed;

        /* Reload timing_counter → local var (assembly line 125-126) */
        int16_t tc = timing_counter;

        /* Initial delay phase (assembly lines 127-138) */
        if (tc == 0) {
            initial_delay--;
            if (initial_delay == 0) {
                current_melody = -1;
                seq_index = -1;
                tc = 1;
                timing_counter = 1;
            }
        }

        /* Exit countdown (assembly lines 139-145) */
        if (exit_countdown > 0) {
            exit_countdown--;
            if (exit_countdown == 0) break; /* → EXIT_FADE_OUT */
            goto render_sprites;
        }

        /* Check sequence advancement (assembly lines 146-230) */
        if (tc == 0) goto render_sprites;

        tc--;
        timing_counter = tc;

        if (tc != 0) goto update_timing;

        /* Timing counter reached 0 — melody's timer expired */
        {
            int16_t cm = current_melody;

            /* Deactivate previously-playing melody (assembly lines 158-168) */
            if (cm < 8 && ps[cm].state == 2)
                ps[cm].state = 1;

            /* Check if we need to find next melody (assembly lines 169-193) */
            if (cm == 8) {
                /* Search from seq_index+1 for a melody with state > 0 */
                int16_t search = seq_index + 1;
                while (search < 8 && ps[search].state == 0)
                    search++;
                if (search >= 8) {
                    exit_countdown = 150;
                }
            }

            /* Advance to next melody (assembly lines 194-227) */
            seq_index++;
            if (seq_index >= 8) {
                /* All melodies done */
                exit_countdown = 150;
                goto update_timing;
            }

            current_melody = seq_index;

            if (ps[seq_index].state != 0) {
                ps[seq_index].state = 2; /* playing */
            } else {
                current_melody = 8; /* sentinel: no active melody */
            }

            /* Play music for this melody slot (assembly lines 216-226) */
            tc = timing[current_melody];
            timing_counter = tc;
            change_music((uint16_t)music_ids[current_melody]);
        }

update_timing:
        /* APU effect trigger (assembly lines 231-250):
         * When timing_counter equals (timing[melody] - 9), send APU command. */
        if (current_melody < 8) {
            int16_t threshold = (int16_t)(timing[current_melody] - 9);
            if (timing_counter == threshold)
                write_apu_port1((uint8_t)(collected_count + 8));
        }

render_sprites:
        /* ---- Render melody sprites (assembly lines 251-472) ---- */
        oam_clear();

        for (int i = 0; i < 8; i++) {
            if (ps[i].state == 1) {
                /* DRAW_IDLE_SPRITE (assembly lines 268-286):
                 * Draw at fixed position from lookup tables. */
                sm1[1] = idle_tiles[i];     /* tile */
                sm1[2] = 0x30;              /* flags: palette 1, priority 1 */
                write_spritemap_to_oam(sm1, (int16_t)(uint8_t)idle_x[i],
                                            (int16_t)(uint8_t)idle_y[i]);

            } else if (ps[i].state == 2) {
                /* DRAW_ORBIT_SPRITE (assembly lines 287-466):
                 * Animated orbiting sprite pair + main sprite at fixed pos. */

                /* Advance orbit angle (assembly lines 288-295) */
                ps[i].orbit_pos2 += (int16_t)(65540 / 20);

                /* Advance animation frame counter (assembly lines 297-344) */
                ps[i].counter--;
                if (ps[i].counter == 0) {
                    ps[i].counter = 2;

                    /* Read next position byte from melody data (assembly lines 315-330) */
                    uint16_t mel_offset = melody_offsets[i] + (uint16_t)ps[i].orbit_frame;
                    ps[i].orbit_pos1 = (int16_t)(melody_data[mel_offset] & 0xFF);
                    ps[i].orbit_frame++;

                    /* Toggle tile_toggle between 0 and 2 (assembly lines 336-344) */
                    ps[i].tile_toggle = 2 - ps[i].tile_toggle;
                }

                /* Draw orbit sprite pair (assembly lines 345-442) */
                sm2[1] = (uint8_t)(orbit_tiles[i] + ps[i].tile_toggle);
                sm2[2] = (uint8_t)(orbit_pal[i] * 2 + 0x31);

                int16_t pos1 = ps[i].orbit_pos1;
                if (pos1 != 0) {
                    uint8_t angle = (uint8_t)((ps[i].orbit_pos2 >> 8) & 0xFF);

                    /* First orbit sprite (assembly lines 388-410) */
                    int16_t sx1 = (int16_t)(uint8_t)idle_x[i] + cosine_func(pos1, angle);
                    int16_t sy1 = (int16_t)(uint8_t)idle_y[i] + cosine_sine(pos1, angle);
                    write_spritemap_to_oam(sm2, sx1, sy1);

                    /* Second orbit sprite at angle+128 (assembly lines 411-442) */
                    uint8_t angle2 = (angle + 128) & 0xFF;
                    int16_t sx2 = (int16_t)(uint8_t)idle_x[i] + cosine_func(pos1, angle2);
                    int16_t sy2 = (int16_t)(uint8_t)idle_y[i] + cosine_sine(pos1, angle2);
                    write_spritemap_to_oam(sm2, sx2, sy2);
                }

                /* Draw main sprite at fixed idle position (assembly lines 443-466) */
                sm1[1] = (uint8_t)(idle_tiles[i] + 128);
                sm1[2] = (uint8_t)(idle_pal[i] * 2 + 0x30);
                write_spritemap_to_oam(sm1, (int16_t)(uint8_t)idle_x[i],
                                            (int16_t)(uint8_t)idle_y[i]);
            }
        }

        /* Update center sprite animation (assembly lines 473-495) */
        center_timer--;
        if (center_timer == 0) {
            center_timer = 15;
            center_frame = (center_frame + 1) & 3;
        }
        sm2[1] = (uint8_t)(64 + center_frame * 2);
        sm2[2] = 0x3B; /* palette 5, priority 1 + hi priority */
        write_spritemap_to_oam(sm2, 128, 112);

        /* Update screen and battle BG (assembly lines 496-502) */
        update_screen();
        generate_battlebg_frame(&loaded_bg_data_layer1, 0);
        generate_battlebg_frame(&loaded_bg_data_layer2, 1);

        /* Button check for cancellable mode (assembly lines 503-507) */
        if (cancellable && (pressed & (PAD_B | PAD_A | PAD_X)))
            break;
    }

    /* ---- Exit: fade out and reload map (assembly lines 508-526) ---- */
    fade_out(1, 1);
    while (fade_active()) {
        if (platform_input_quit_requested()) break;
        wait_for_vblank();
        fade_update();
    }
    force_blank_and_wait_vblank();
    set_color_math_from_table(1);
    reload_map();
    fade_in(1, 1);

    return 0;
}


/*
 * DISPATCH_SPECIAL_EVENT (asm/text/dispatch_special_event.asm)
 *
 * Dispatch table for special in-game events triggered by CC 1F 41.
 * Each event_id maps to a specific game sequence or state change.
 * Returns 0 for most events; some return meaningful values.
 */
uint16_t dispatch_special_event(uint16_t event_id) {
    switch (event_id) {
    case 1:
        /* COFFEE_SCENE: calls COFFEETEA_SCENE(0) */
        coffeetea_scene(0);
        return 0;
    case 2:
        /* TEA_SCENE: calls COFFEETEA_SCENE(1) */
        coffeetea_scene(1);
        return 0;
    case 3:
        /* REGISTER_REAL_NAME: calls ENTER_YOUR_NAME_PLEASE(0) */
        return enter_your_name_please(0);
    case 4:
        /* REGISTER_REAL_NAME_2: calls ENTER_YOUR_NAME_PLEASE(1) */
        return enter_your_name_please(1);
    case 5:
        /* SET_OVERWORLD_STATUS_SUPPRESSION(1) */
        ow.overworld_status_suppression = 1;
        return 0;
    case 6:
        /* SET_OVERWORLD_STATUS_SUPPRESSION(get_event_flag(WIN_GIEGU)) */
        ow.overworld_status_suppression = event_flag_get(EVENT_FLAG_WIN_GIEGU) ? 1 : 0;
        return 0;
    case 7:
        /* DISPLAY_TOWN_MAP */
        return display_town_map();
    case 8: {
        /* IS_ATTACKER_ENEMY: Port of asm/battle/is_attacker_enemy.asm.
         * Checks battler at bt.current_attacker offset; returns 1 if ally_or_enemy != 0. */
        Battler *atk = battler_from_offset(bt.current_attacker);
        return (atk->ally_or_enemy != 0) ? 1 : 0;
    }
    case 9:
        /* USE_SOUND_STONE(cancellable=1) */
        return use_sound_stone(1);
    case 10:
        /* SHOW_TITLE_SCREEN(1) */
        show_title_screen(1);
        return 0;
    case 11:
        /* PLAY_CAST_SCENE */
        play_cast_scene();
        return 0;
    case 12:
        /* PLAY_CREDITS */
        play_credits();
        return 0;
    case 13:
        /* TOGGLE_HPPP_FLIPOUT_MODE(1) — enable */
        toggle_hppp_flipout_mode(1);
        return 0;
    case 14:
        /* TOGGLE_HPPP_FLIPOUT_MODE(0) — disable */
        toggle_hppp_flipout_mode(0);
        return 0;
    case 15:
        /* CLEAR_ALL_EVENT_FLAGS: memset event_flags to 0 */
        memset(event_flags, 0, EVENT_FLAG_COUNT / 8);
        return 0;
    case 16:
        /* USE_SOUND_STONE(cancellable=0) */
        return use_sound_stone(0);
    case 17:
        /* ATTEMPT_HOMESICKNESS */
        return attempt_homesickness();
    case 18: {
        /* CHECK_BICYCLE_AND_DISMOUNT: if walking_style==BICYCLE, dismount and return 1 */
        if (game_state.walking_style == WALKING_STYLE_BICYCLE) {
            dismount_bicycle();
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}


/*
 * SHOW_CHARACTER_INVENTORY — Port of CC_1A_05 (asm/text/ccs/show_character_inventory.asm)
 * + INVENTORY_GET_ITEM_NAME (asm/misc/inventory_get_item_name.asm).
 *
 * Displays a character's inventory as a 2-column menu in the given window.
 * Called from text scripts to show a character's items (e.g., during
 * Escargo Express delivery, give/take item events).
 *
 * CC_1A_05 arguments (2 bytes from text stream):
 *   window_id:   window to create for the inventory display
 *   char_source: 0 = read character ID from argument_memory (previous selection),
 *                nonzero = use directly as 1-based character ID (PARTY_MEMBER enum)
 *
 * Pre-processing (assembly lines 46-71, US path):
 *   If win.current_focus_window == 1 (main text window), clears the window tilemap,
 *   resets text cursor, saves text attributes, and clears FORCE_LEFT_TEXT_ALIGNMENT.
 *   This prepares the main window for inventory overlay.
 *
 * Then calls INVENTORY_GET_ITEM_NAME which:
 *   1. Creates the specified window
 *   2. Sets window title to character name
 *   3. Loops over 14 inventory slots, prefixing equipped items with '*'
 *   4. Calls OPEN_WINDOW_AND_PRINT_MENU(2, 0) for 2-column layout
 */
void show_character_inventory(uint16_t window_id, uint16_t char_source) {
    /* Determine character ID.
     * Assembly lines 73-84: if char_source == 0, get from argument_memory;
     * else use char_source directly. */
    uint16_t char_id;
    if (char_source == 0) {
        char_id = (uint16_t)get_argument_memory();
    } else {
        char_id = char_source;
    }

    if (char_id == 0 || char_id > 6) return;
    uint16_t char_idx = char_id - 1;

    /* Pre-processing: if main text window is focused, prepare for overlay.
     * Assembly lines 46-71: clears tilemap, resets cursor, saves attributes. */
    if (win.current_focus_window == 1) {
        /* Assembly: CLEAR_WINDOW_TILEMAP(1), reset text_x/text_y to 0 */
        clear_window_tilemap(1);
        /* Assembly line 68: SAVE_WINDOW_TEXT_ATTRIBUTES with window-specific param.
         * The param is char_id + 6, used as a WINDOW_TEXT_ATTRIBUTES_BACKUP index.
         * In C port, save_window_text_attributes uses a single backup. */
        save_window_text_attributes();
        dt.force_left_text_alignment = 0;
    }

    /* --- INVENTORY_GET_ITEM_NAME (asm/misc/inventory_get_item_name.asm) ---
     * Creates the window, sets title to character name, populates items. */
    /* Assembly (inventory_get_item_name.asm:25-31): set pagination for multi-party */
    if (game_state.player_controlled_party_count > 1) {
        dt.pagination_window = window_id;
    }
    create_window(window_id);

    /* Assembly lines 32-43: set window title to character name (max 5 chars).
     * SET_WINDOW_TITLE(window_id, char_name_ptr, sizeof(char_struct::name)=5). */
    {
        char name_buf[6];
        eb_to_ascii_buf(party_characters[char_idx].name, 5, name_buf);
        set_window_title(window_id, name_buf, 5);
    }

    /* Assembly lines 47-148: loop over ITEM_INVENTORY_SIZE (14) slots.
     * For each non-empty slot, check if equipped (US), copy item name from
     * ITEM_CONFIGURATION_TABLE, add as menu option. */
    for (int i = 0; i < ITEM_INVENTORY_SIZE; i++) {
        uint8_t item_id = party_characters[char_idx].items[i];
        if (item_id == 0) continue;

        const ItemConfig *item_entry = get_item_entry(item_id);
        if (!item_entry) continue;

        char label[MENU_LABEL_SIZE];
        int offset = 0;

        /* Check if equipped (US only, assembly lines 60-71).
         * Assembly uses CHAR::EQUIPPED ($22) as prefix character. */
        if (check_item_equipped(char_id, (uint16_t)(i + 1))) {
            label[0] = EB_CHAR_EQUIPPED;
            offset = 1;
        }

        /* Copy item name: EB encoding → ASCII */
        int j;
        for (j = 0; j < ITEM_NAME_LEN && (offset + j) < MENU_LABEL_SIZE - 1; j++) {
            if (item_entry->name[j] == 0x00) break;
            label[offset + j] = eb_char_to_ascii(item_entry->name[j]);
        }
        label[offset + j] = '\0';

        /* Assembly uses ADD_MENU_OPTION (type=1, returns 1-based position) */
        add_menu_item_no_position(label, (uint16_t)(i + 1));
    }

    /* Assembly lines 149-155: WINDOW_TICK_WITHOUT_INSTANT_PRINTING,
     * then OPEN_WINDOW_AND_PRINT_MENU(columns=2, start_index=0). */
    window_tick_without_instant_printing();
    open_window_and_print_menu(2, 0);
}


static void load_store_table(void) {
    if (store_table_data) return;
    store_table_size = ASSET_SIZE(ASSET_DATA_STORE_TABLE_BIN);
    store_table_data = ASSET_DATA(ASSET_DATA_STORE_TABLE_BIN);
}


/*
 * OPEN_STORE_MENU — Port of src/inventory/store/open_store_menu.asm.
 *
 * Displays a shop's item list with prices and runs selection_menu.
 * Returns the selected item ID, or 0 if cancelled.
 *
 * Flow:
 *   1. DISPLAY_MONEY_WINDOW — shows current wallet amount
 *   2. SET_INSTANT_PRINTING — force instant text rendering
 *   3. SAVE_WINDOW_TEXT_ATTRIBUTES — save text state
 *   4. CREATE_WINDOW(WINDOW::STORE_ITEM_LIST = 0x0C)
 *   5. SET_WINDOW_NUMBER_PADDING(5) — for price formatting
 *   6. Loop 7 items: read from STORE_TABLE[shop_id*7+i], get name from
 *      ITEM_CONFIGURATION_TABLE, add as menu option, print price via
 *      PRINT_MONEY_IN_WINDOW
 *   7. OPEN_WINDOW_AND_PRINT_MENU(1, 0) — single-column layout
 *   8. SET_CURSOR_MOVE_CALLBACK(SET_HPPP_WINDOW_MODE_ITEM) — shows equip info
 *   9. INITIALIZE_WINDOW_FLAVOUR_PALETTE
 *  10. SELECTION_MENU(1) — allow cancel
 *  11. RESET_HPPP_OPTIONS_AND_LOAD_PALETTE — cleanup
 *  12. CLOSE_FOCUS_WINDOW, RESTORE_WINDOW_TEXT_ATTRIBUTES, CLEAR_INSTANT_PRINTING
 */
uint16_t open_store_menu(uint16_t shop_id) {
    /* Assembly lines 14-20: display money window, enable instant printing,
     * save text attributes, create shop window, set padding to 5 digits. */
    display_money_window();
    set_instant_printing();
    save_window_text_attributes();
    create_window(WINDOW_STORE_ITEM_LIST);
    set_window_number_padding(5);

    /* Assembly lines 21-83: loop 7 items for this shop.
     * For each item: ADD_MENU_ITEM_NO_POSITION(name, item_id),
     * SET_FOCUS_TEXT_CURSOR(0, counter), PRINT_MONEY_IN_WINDOW(price).
     * Prices are right-aligned via PRINT_MONEY_IN_WINDOW. */
    load_store_table();
    if (store_table_data) {
        for (int i = 0; i < STORE_ITEMS_PER_SHOP; i++) {
            size_t table_offset = (size_t)shop_id * STORE_ITEMS_PER_SHOP + i;
            if (table_offset >= store_table_size) break;

            uint8_t item_id = store_table_data[table_offset];
            if (item_id == 0) continue;

            const ItemConfig *item_entry = get_item_entry(item_id);
            if (!item_entry) continue;

            /* Copy item name from config (EB encoding → ASCII) */
            char name_buf[ITEM_NAME_LEN + 1];
            eb_to_ascii_buf(item_entry->name, ITEM_NAME_LEN, name_buf);

            add_menu_item_no_position(name_buf, item_id);

            /* Print right-aligned price on the same line.
             * Assembly uses raw slot index (always increments) for Y,
             * not item_counter which only increments for non-empty items. */
            set_focus_text_cursor(0, (uint16_t)i);
            uint16_t price = item_entry->cost;
            print_money_in_window((uint32_t)price);


        }
    }

    /* Assembly lines 84-90: reset cursor, layout menu in 1 column */
    set_focus_text_cursor(0, 0);
    open_window_and_print_menu(1, 0);

    /* Assembly lines 91-93: SET_CURSOR_MOVE_CALLBACK(SET_HPPP_WINDOW_MODE_ITEM),
     * INITIALIZE_WINDOW_FLAVOUR_PALETTE.
     * Cursor callback updates HPPP window display to show stat comparison
     * when hovering over equippable items. */
    set_cursor_move_callback(set_hppp_window_mode_item);
    initialize_window_flavour_palette();

    /* Assembly line 94-95: selection_menu(1) — allow cancel */
    uint16_t result = selection_menu(1);

    /* Assembly lines 96-104: cleanup.
     * Order matches assembly: RESET → CLOSE → RESTORE → CLEAR. */
    reset_hppp_options_and_load_palette();
    close_focus_window();
    restore_window_text_attributes();
    clear_instant_printing();

    return result;
}


static void load_telephone_contacts_table(void) {
    if (telephone_contacts_data) return;
    telephone_contacts_size = ASSET_SIZE(ASSET_DATA_TELEPHONE_CONTACTS_TABLE_BIN);
    telephone_contacts_data = ASSET_DATA(ASSET_DATA_TELEPHONE_CONTACTS_TABLE_BIN);
}


static void load_menu_title_assets(void) {
    if (!status_equip_text_7) {
        status_equip_text_7_size = ASSET_SIZE(ASSET_DATA_STATUS_EQUIP_WINDOW_TEXT_7_BIN);
        status_equip_text_7 = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_WINDOW_TEXT_7_BIN);
    }
    if (!status_equip_text_14) {
        status_equip_text_14_size = ASSET_SIZE(ASSET_DATA_STATUS_EQUIP_WINDOW_TEXT_14_BIN);
        status_equip_text_14 = ASSET_DATA(ASSET_DATA_STATUS_EQUIP_WINDOW_TEXT_14_BIN);
    }
    if (!phone_call_text_data) {
        phone_call_text_size = ASSET_SIZE(ASSET_DATA_PHONE_CALL_TEXT_BIN);
        phone_call_text_data = ASSET_DATA(ASSET_DATA_PHONE_CALL_TEXT_BIN);
    }
}


/*
 * OPEN_TELEPORT_DESTINATION_MENU — Port of asm/text/menu/open_teleport_destination_menu.asm (95 lines).
 *
 * Displays a menu of unlocked PSI Teleport destinations.
 * Each destination has an event_flag; only destinations with their flag set appear.
 * Returns the 1-based selection index, or 0 if cancelled/empty.
 *
 * Assembly flow:
 *   1. DISPLAY_MENU_HEADER_TEXT(2) → "Where?"
 *   2. SAVE_WINDOW_TEXT_ATTRIBUTES
 *   3. CREATE_WINDOW(PHONE_MENU)
 *   4. SET_WINDOW_TITLE from STATUS_EQUIP_WINDOW_TEXT_14 ("To:"), max_len=3
 *   5. Loop over PSI_TELEPORT_DEST_TABLE entries:
 *      - If first byte != 0 and event_flag is set, add to menu
 *   6. If any items: OPEN_WINDOW_AND_PRINT_MENU(1, 0), SELECTION_MENU(1)
 *   7. Close windows and restore text attributes
 */
uint16_t open_teleport_destination_menu(void) {
    uint16_t result = 0;

    display_menu_header_text(2);  /* "Where?" */
    save_window_text_attributes();
    create_window(WINDOW_PHONE_MENU);

    /* Set window title: "To:" from STATUS_EQUIP_WINDOW_TEXT_14 */
    load_menu_title_assets();
    if (status_equip_text_14) {
        char title_buf[WINDOW_TITLE_SIZE];
        eb_to_ascii_buf(status_equip_text_14, (int)status_equip_text_14_size, title_buf);
        set_window_title(WINDOW_PHONE_MENU, title_buf, 3);
    }

    /* Load and iterate PSI teleport destination table */
    {
        int menu_item_count = 0;
        /* Assembly starts index at 1 (skips entry 0 sentinel), loops while first byte != 0 */
        for (int i = 1; ; i++) {
            size_t offset = (size_t)i * PSI_TELEPORT_DEST_ENTRY_SIZE;
            if (offset + PSI_TELEPORT_DEST_ENTRY_SIZE > psi_teleport_dest_size)
                break;
            const uint8_t *entry = psi_teleport_dest_data + offset;

            /* First byte == 0 marks end of table */
            if (entry[0] == 0x00)
                break;

            /* Read event_flag at offset 25 (after name[25]) */
            uint16_t flag = read_u16_le(&entry[PSI_TELEPORT_DEST_NAME_LEN]);
            if (!event_flag_get(flag))
                continue;

            /* Convert EB-encoded name to ASCII */
            char name_buf[PSI_TELEPORT_DEST_NAME_LEN + 1];
            eb_to_ascii_buf(entry, PSI_TELEPORT_DEST_NAME_LEN, name_buf);

            add_menu_item_no_position(name_buf, (uint16_t)i);
            menu_item_count++;
        }

        if (menu_item_count > 0) {
            open_window_and_print_menu(1, 0);
            result = selection_menu(1);
        }
    }

    close_focus_window();
    close_menu_header_window();
    restore_window_text_attributes();

    return result;
}


/*
 * SELECT_ESCARGO_EXPRESS_ITEM — Port of src/inventory/select_escargo_express_item.asm (94 lines).
 *
 * Displays a menu of items stored in Escargo Express.
 * Returns the 1-based selection index, or 0 if cancelled/empty.
 *
 * Assembly flow:
 *   1. SAVE_WINDOW_TEXT_ATTRIBUTES
 *   2. CREATE_WINDOW(ESCARGO_EXPRESS_ITEM)
 *   3. Build title from STATUS_EQUIP_WINDOW_TEXT_7 ("Stored Goods").
 *      Assembly also appends 3 EB chars (88,97,89) but these are title
 *      decoration; the C port uses just the base text.
 *   4. Loop i=0..35 (36 escargo slots):
 *      - If escargo_express_items[i] != 0, look up item name, add to menu
 *   5. OPEN_WINDOW_AND_PRINT_MENU(2, 0) — 2 columns
 *   6. SELECTION_MENU(1) — allow cancel
 *   7. CLEAR_WINDOW_TILEMAP(ESCARGO_EXPRESS_ITEM)
 *   8. Clear FORCE_LEFT_TEXT_ALIGNMENT, restore text attributes
 */
uint16_t select_escargo_express_item(void) {
    save_window_text_attributes();
    create_window(WINDOW_ESCARGO_EXPRESS_ITEM);

    /* Build window title: STATUS_EQUIP_WINDOW_TEXT_7 ("Stored Goods") */
    load_menu_title_assets();
    if (status_equip_text_7) {
        char title_buf[WINDOW_TITLE_SIZE];
        eb_to_ascii_buf(status_equip_text_7, (int)status_equip_text_7_size, title_buf);
        set_window_title(WINDOW_ESCARGO_EXPRESS_ITEM, title_buf, -1);
    }

    /* Loop over 36 Escargo Express storage slots.
     * Assembly uses ADD_MENU_OPTION (type=1, counted mode) — SELECTION_MENU
     * returns the 1-based option index. C port's selection_menu returns userdata,
     * so we assign sequential 1-based indices as userdata. */
    int seq = 1;
    for (int i = 0; i < 36; i++) {
        uint8_t item_id = game_state.escargo_express_items[i];
        if (item_id == 0) continue;

        /* Look up item name from item configuration table */
        const ItemConfig *item_entry = get_item_entry(item_id);
        if (!item_entry) continue;

        char name_buf[ITEM_NAME_LEN + 1];
        eb_to_ascii_buf(item_entry->name, ITEM_NAME_LEN, name_buf);

        add_menu_item_no_position(name_buf, (uint16_t)seq++);
    }

    open_window_and_print_menu(2, 0);
    uint16_t result = selection_menu(1);

    /* Assembly: CLEAR_WINDOW_TILEMAP(ESCARGO_EXPRESS_ITEM) */
    close_window(WINDOW_ESCARGO_EXPRESS_ITEM);
    dt.force_left_text_alignment = 0;
    restore_window_text_attributes();

    return result;
}


/*
 * OPEN_TELEPHONE_MENU — Port of asm/text/menu/open_telephone_menu.asm (85 lines).
 *
 * Displays a menu of telephone contacts whose event flags are set.
 * Returns the 1-based selection index, or 0 if cancelled/empty.
 *
 * Assembly flow:
 *   1. SAVE_WINDOW_TEXT_ATTRIBUTES
 *   2. CREATE_WINDOW(EQUIP_MENU_ITEMLIST)
 *   3. SET_WINDOW_TITLE from PHONE_CALL_TEXT ("Call:"), max_len = sizeof(char_struct::name) = 5
 *   4. Loop over TELEPHONE_CONTACTS_TABLE entries:
 *      - If first byte != 0 and event_flag is set, add to menu
 *   5. If any items: OPEN_WINDOW_AND_PRINT_MENU(1, 0), SELECTION_MENU(1)
 *   6. Close windows and restore text attributes
 */
uint16_t open_telephone_menu(void) {
    uint16_t result = 0;

    save_window_text_attributes();
    create_window(WINDOW_EQUIP_MENU_ITEMLIST);

    /* Set window title: "Call:" from PHONE_CALL_TEXT */
    load_menu_title_assets();
    if (phone_call_text_data) {
        char title_buf[WINDOW_TITLE_SIZE];
        eb_to_ascii_buf(phone_call_text_data, (int)phone_call_text_size, title_buf);
        /* Assembly: LDX #sizeof(char_struct::name) = 5 for max_len */
        set_window_title(WINDOW_EQUIP_MENU_ITEMLIST, title_buf, 5);
    }

    /* Load and iterate telephone contacts table */
    load_telephone_contacts_table();
    if (telephone_contacts_data) {
        int menu_item_count = 0;
        /* Assembly starts index at 1 (skips entry 0 sentinel), loops while first byte != 0 */
        for (int i = 1; ; i++) {
            size_t offset = (size_t)i * TELEPHONE_CONTACT_ENTRY_SIZE;
            if (offset + TELEPHONE_CONTACT_ENTRY_SIZE > telephone_contacts_size)
                break;
            const uint8_t *entry = telephone_contacts_data + offset;

            /* First byte == 0 marks end of table */
            if (entry[0] == 0x00)
                break;

            /* Read event_flag at offset 25 (after label[25]) */
            uint16_t flag = read_u16_le(&entry[TELEPHONE_CONTACT_LABEL_LEN]);
            if (!event_flag_get(flag))
                continue;

            /* Convert EB-encoded label to ASCII */
            char label_buf[TELEPHONE_CONTACT_LABEL_LEN + 1];
            eb_to_ascii_buf(entry, TELEPHONE_CONTACT_LABEL_LEN, label_buf);

            add_menu_item_no_position(label_buf, (uint16_t)i);
            menu_item_count++;
        }

        if (menu_item_count > 0) {
            open_window_and_print_menu(1, 0);
            result = selection_menu(1);
        }
    }

    close_focus_window();
    restore_window_text_attributes();

    return result;
}


/*
 * DISPLAY_TELEPHONE_CONTACT_TEXT — Port of asm/text/display_telephone_contact_text.asm (26 lines).
 *
 * Opens the telephone menu. If a contact is selected, looks up their
 * text script pointer from the contacts table and runs display_text.
 * Returns the selection index (0 if cancelled).
 *
 * Assembly flow:
 *   1. JSR OPEN_TELEPHONE_MENU → selection index in A
 *   2. If selection != 0:
 *      - Index into TELEPHONE_CONTACTS_TABLE[selection]
 *      - Read 4-byte text pointer at offset telephone_contact::text
 *      - JSL DISPLAY_TEXT with that pointer
 *   3. Return selection index
 */
uint16_t display_telephone_contact_text(void) {
    uint16_t selection = open_telephone_menu();

    if (selection != 0) {
        load_telephone_contacts_table();
        if (telephone_contacts_data) {
            size_t offset = (size_t)selection * TELEPHONE_CONTACT_ENTRY_SIZE;
            if (offset + TELEPHONE_CONTACT_ENTRY_SIZE > telephone_contacts_size)
                return selection;
            /* text pointer is at offset 27 (label[25] + event_flag[2]) */
            const uint8_t *entry = telephone_contacts_data + offset;
            uint32_t text_addr = read_u32_le(&entry[27]);
            if (text_addr != 0) {
                display_text_from_snes_addr(text_addr);
            }
        }
    }

    return selection;
}

