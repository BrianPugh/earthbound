/*
 * Battle PSI functions.
 *
 * Extracted from battle.c — PSI availability checks, menu generation,
 * animation playback, and PSI battle effect application.
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
#include "game/map_loader.h"
#include "game/overworld.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "game/oval_window.h"
#include <string.h>

/* PSI ability IDs */
#define PSI_STARSTORM_ALPHA  21
#define PSI_STARSTORM_OMEGA  22

/* PARTY_PSI_FLAGS (from enums.asm) */
#define PARTY_PSI_TELEPORT_ALPHA   (1 << 0)
#define PARTY_PSI_STARSTORM_ALPHA  (1 << 1)
#define PARTY_PSI_STARSTORM_OMEGA  (1 << 2)
#define PARTY_PSI_TELEPORT_BETA    (1 << 3)

/* PSI category name size (from config.asm, US version) */
#define PSI_CATEGORY_NAME_SIZE  8

/* Number of PSI categories in the menu */
#define PSI_CATEGORY_COUNT  4

static size_t battle_psi_table_size = 0;

/* ROM address constants: within-bank LOWORDs of the 4 PSI GFX sets. */
#define PSI_ANIM_GFX_LOWORD_SET1  0xAC25
#define PSI_ANIM_GFX_LOWORD_SET2  0xB613
#define PSI_ANIM_GFX_LOWORD_SET3  0xDB27
#define PSI_ANIM_GFX_LOWORD_SET4  0xE31D

#define PSI_ANIM_COUNT           34
#define PSI_FRAME_SIZE           0x400  /* bytes per arrangement frame (32×32 tilemap) */
#define PSI_FRAMES_PER_BUNDLE    8     /* frames per compressed bundle in .arr.bundled */

PACKED_STRUCT
typedef struct {
    uint16_t gfx_loword;           /*  0: GFX set identifier (LE) */
    uint8_t  frame_hold;           /*  2: frames per animation frame */
    uint8_t  pal_anim_frames;     /*  3: palette cycle period */
    uint8_t  pal_anim_lower;      /*  4: lower palette index */
    uint8_t  pal_anim_upper;      /*  5: upper palette index */
    uint8_t  total_frames;        /*  6: total animation frames */
    uint8_t  target_type;         /*  7: targeting mode */
    uint8_t  color_change_start;  /*  8: delay before color change */
    uint8_t  color_change_duration; /* 9: color change length */
    uint16_t color_rgb;            /* 10: target color (LE, RGB555) */
} PsiAnimConfig;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(PsiAnimConfig, 12);

/* PSI target animation types */
#define PSI_TARGET_SINGLE       0
#define PSI_TARGET_ROW          1
#define PSI_TARGET_ALL_ENEMIES  2
#define PSI_TARGET_RANDOM       3

/* PSI category names — loaded from ROM via asset pipeline.
 * 4 entries × PSI_CATEGORY_NAME_SIZE bytes, EB-encoded and null-padded. */

/* PSI target text — loaded from ROM via asset pipeline.
 * 10 entries × PSI_TARGET_TEXT_LENGTH bytes, EB-encoded and null-padded.
 * Indexed as [direction * 5 + target]:
 *   direction 0 = PARTY (ally), direction 1 = ENEMY
 *   target 0=NONE, 1=ONE, 2=RANDOM, 3=ROW, 4=ALL */
#define PSI_TARGET_TEXT_LENGTH 20

bool ensure_battle_psi_table(void) {
    if (battle_psi_table) return true;
    battle_psi_table_size = ASSET_SIZE(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    battle_psi_table = (const PsiAbility *)ASSET_DATA(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    return battle_psi_table != NULL;
}


/*
 * SET_WINDOW_PALETTE_INDEX — now shared via window.h / window.c.
 * (Removed static version; callers use the shared set_window_palette_index.)
 */

/* set_cursor_move_callback / clear_cursor_move_callback:
 * Now shared via window.h — see window.c for implementation. */

/*
 * CHECK_CHARACTER_HAS_PSI_ABILITY (asm/text/check_character_has_psi_ability.asm)
 *
 * Checks if a character has any PSI ability matching usability and category masks.
 * char_id: PARTY_MEMBER enum (1=Ness, 2=Paula, 3=Jeff, 4=Poo)
 * usability: PSI_USABILITY bitmask (1=overworld, 2=battle)
 * category: PSI_CATEGORY bitmask (1=offense, 2=recover, 4=assist, 8=other)
 *
 * Returns 1 if the character has at least one matching PSI ability, 0 otherwise.
 * Jeff (char_id==3) always returns 0.
 * Also handles Starstorm (Ness, party-wide PSI) and Teleport (Ness, overworld).
 */
uint16_t check_character_has_psi_ability(uint16_t char_id,
                                        uint16_t usability,
                                        uint16_t category) {
    if (!ensure_battle_psi_table()) return 0;
    if (char_id == PARTY_MEMBER_JEFF) return 0;

    uint16_t char_idx = char_id - 1;  /* 0-based */

    /* Main loop: scan PSI ability table entries 1..N */
    for (uint16_t i = 1; i < PSI_MAX_ENTRIES; i++) {
        /* Check if this entry exists (name != 0) */
        const PsiAbility *psi = &battle_psi_table[i];
        if (psi->name == 0) break;

        /* Get character's required level for this ability */
        uint8_t learn_level = 0;
        switch (char_idx) {
        case 0: learn_level = psi->ness_level; break;
        case 1: learn_level = psi->paula_level; break;
        case 3: learn_level = psi->poo_level; break;
        }
        if (learn_level == 0) continue;

        /* Check usability mask */
        if ((psi->usability & usability) == 0) continue;

        /* Check character level */
        uint8_t current_level = party_characters[char_idx].level;
        if (learn_level > current_level) continue;

        /* Check category mask */
        if ((psi->category & category) == 0) continue;

        return 1;  /* found a matching ability */
    }

    /* Special cases after loop: Ness Teleport and Poo Starstorm (party-wide PSI) */
    /* Ness (char_idx==0): check overworld usability + other category for teleport */
    if (char_idx == 0 && (usability & PSI_USE_OVERWORLD) && (category & PSI_CAT_OTHER)) {
        if (game_state.party_psi & PARTY_PSI_TELEPORT_ALPHA)
            return 1;
    }

    /* Poo (char_idx==3): check battle usability + offense category for starstorm */
    if (char_idx == 3 && (usability & PSI_USE_BATTLE) && (category & PSI_CAT_OFFENSE)) {
        if (game_state.party_psi & (PARTY_PSI_STARSTORM_ALPHA | PARTY_PSI_STARSTORM_OMEGA))
            return 1;
    }

    return 0;
}


/*
 * CHECK_PSI_CATEGORY_AVAILABLE (asm/text/check_psi_category_available.asm)
 *
 * Checks if a PSI category (1=Offensive, 2=Recovery, 3=Assist) has any
 * available abilities for the given character. Category 4 (Other) is
 * always considered available if categories 1-3 work.
 *
 * char_id: PARTY_MEMBER enum value (passed in X register in assembly)
 * category: 1-based category number (1=Offensive, 2=Recovery, 3=Assist)
 *
 * Returns nonzero if the character has abilities in that category.
 */
uint16_t check_psi_category_available(uint16_t category, uint16_t char_id) {
    switch (category) {
    case 1:  /* Offensive */
        return check_character_has_psi_ability(char_id, PSI_USE_BATTLE, PSI_CAT_OFFENSE);
    case 2:  /* Recovery */
        return check_character_has_psi_ability(char_id, PSI_USE_BATTLE, PSI_CAT_RECOVER);
    case 3:  /* Assist */
        return check_character_has_psi_ability(char_id, PSI_USE_BATTLE, PSI_CAT_ASSIST);
    default:
        return 0;
    }
}


/*
 * GENERATE_PSI_LIST callback for cursor_move_callback.
 *
 * GENERATE_BATTLE_PSI_LIST (asm/battle/ui/generate_battle_psi_list.asm) is called as
 * a cursor_move_callback when the user navigates the PSI category menu.
 * It generates the PSI ability list for the selected category.
 *
 * This is a complex function that iterates PSI_ABILITY_TABLE, checks
 * character level/usability/category, and adds matching abilities as
 * menu items with their names printed using VWF.
 *
 * For now, this generates the menu items in the TEXT_STANDARD window.
 */
/*
 * generate_psi_list_items — core PSI ability list builder.
 *
 * Populates the current focus window with PSI abilities matching the given
 * character, usability, and category filters. Adds menu items with ability
 * IDs as userdata and prints PSI names at their prescribed positions.
 *
 * Handles special cases: Poo Starstorm (battle + offense), Ness Teleport
 * (overworld + other).
 *
 * Shared by GENERATE_BATTLE_PSI_LIST (cursor callback) and
 * GENERATE_PSI_LIST (direct call from DISPLAY_CHARACTER_PSI_LIST).
 */
static void generate_psi_list_items(uint16_t char_id,
                                    uint8_t usability_mask,
                                    uint8_t category_mask) {
    uint16_t char_idx = char_id - 1;  /* 0-based */
    uint8_t last_psi_name = 0;

    char suffix_label[4];

    /* Poo special: add Starstorm entries if available (offense category, battle usability) */
    if (char_idx == (PARTY_MEMBER_POO - 1) && (usability_mask & PSI_USE_BATTLE) &&
        (category_mask & PSI_CAT_OFFENSE)) {
        if (game_state.party_psi & PARTY_PSI_STARSTORM_ALPHA) {
            const PsiAbility *ss = &battle_psi_table[PSI_STARSTORM_ALPHA];
            set_focus_text_cursor(0, ss->menu_y);
            print_psi_name(ss->name);
            get_psi_suffix_label(PSI_STARSTORM_ALPHA, suffix_label, sizeof(suffix_label));
            add_menu_item(suffix_label, PSI_STARSTORM_ALPHA, ss->menu_x, ss->menu_y);
        }
        if (game_state.party_psi & PARTY_PSI_STARSTORM_OMEGA) {
            const PsiAbility *ss = &battle_psi_table[PSI_STARSTORM_OMEGA];
            set_focus_text_cursor(0, ss->menu_y);
            print_psi_name(ss->name);
            get_psi_suffix_label(PSI_STARSTORM_OMEGA, suffix_label, sizeof(suffix_label));
            add_menu_item(suffix_label, PSI_STARSTORM_OMEGA, ss->menu_x, ss->menu_y);
        }
    }

    /* Main PSI ability loop */
    for (uint16_t i = 1; i < PSI_MAX_ENTRIES; i++) {
        const PsiAbility *psi = &battle_psi_table[i];
        if (psi->name == 0) break;

        /* Get character's learn level */
        uint8_t learn_level = 0;
        switch (char_idx) {
        case 0: learn_level = psi->ness_level; break;
        case 1: learn_level = psi->paula_level; break;
        case 3: learn_level = psi->poo_level; break;
        }
        if (learn_level == 0) continue;

        /* Check usability */
        if ((psi->usability & usability_mask) == 0) continue;

        /* Check character level */
        if (learn_level > party_characters[char_idx].level) continue;

        /* Check category */
        if ((psi->category & category_mask) == 0) continue;

        /* Print PSI base name if different from last (assembly: GET_PSI_NAME) */
        if (psi->name != last_psi_name) {
            set_focus_text_cursor(0, psi->menu_y);
            print_psi_name(psi->name);
            last_psi_name = psi->name;
        }

        /* Add menu item — suffix label printed at menu_x by print_menu_items */
        get_psi_suffix_label(i, suffix_label, sizeof(suffix_label));
        add_menu_item(suffix_label, i, psi->menu_x, psi->menu_y);
    }

    /* Ness special: add Teleport entries if available (overworld, other category) */
    if (char_idx == 0 && (usability_mask & PSI_USE_OVERWORLD) &&
        (category_mask & PSI_CAT_OTHER)) {
        if (game_state.party_psi & PARTY_PSI_TELEPORT_ALPHA) {
            const PsiAbility *tp = &battle_psi_table[PSI_TELEPORT_ALPHA];
            set_focus_text_cursor(0, tp->menu_y);
            print_psi_name(tp->name);
            get_psi_suffix_label(PSI_TELEPORT_ALPHA, suffix_label, sizeof(suffix_label));
            add_menu_item(suffix_label, PSI_TELEPORT_ALPHA, tp->menu_x, tp->menu_y);
        }
        if (game_state.party_psi & PARTY_PSI_TELEPORT_BETA) {
            const PsiAbility *tp = &battle_psi_table[PSI_TELEPORT_BETA];
            set_focus_text_cursor(0, tp->menu_y);
            print_psi_name(tp->name);
            get_psi_suffix_label(PSI_TELEPORT_BETA, suffix_label, sizeof(suffix_label));
            add_menu_item(suffix_label, PSI_TELEPORT_BETA, tp->menu_x, tp->menu_y);
        }
    }
}


/*
 * GENERATE_BATTLE_PSI_LIST callback — Port of asm/battle/ui/generate_battle_psi_list.asm.
 *
 * Cursor move callback invoked when navigating the PSI category menu.
 * Creates/refreshes TEXT_STANDARD window and fills it with PSI abilities
 * for the selected category. Reads win.battle_menu_current_character_id.
 */
void generate_battle_psi_list_callback(uint16_t category) {
    if (!ensure_battle_psi_table()) return;

    /* Get the current character from win.battle_menu_current_character_id */
    uint16_t char_slot = (win.battle_menu_current_character_id == (int16_t)-1)
                         ? 0 : (uint16_t)win.battle_menu_current_character_id;
    uint8_t char_id = game_state.party_members[char_slot];

    /* Create/reuse text standard window for PSI list */
    WindowInfo *w = create_window(WINDOW_TEXT_STANDARD);
    if (!w) return;
    window_tick_without_instant_printing();  /* asm line 29 */

    /* Map category number to usability and category masks.
     * Assembly: category 1→(usability=2, cat=1), 2→(2,2), 3→(2,4), 4→(3,8) */
    uint8_t usability_mask, category_mask;
    switch (category) {
    case 1: usability_mask = PSI_USE_BATTLE; category_mask = PSI_CAT_OFFENSE; break;
    case 2: usability_mask = PSI_USE_BATTLE; category_mask = PSI_CAT_RECOVER; break;
    case 3: usability_mask = PSI_USE_BATTLE; category_mask = PSI_CAT_ASSIST;  break;
    case 4: usability_mask = PSI_USE_BATTLE | PSI_USE_OVERWORLD;
            category_mask = PSI_CAT_OTHER;  break;
    default: return;
    }

    /* Assembly GENERATE_PSI_LIST: set_instant, clear menu, populate, print, clear_instant */
    set_instant_printing();
    clear_focus_window_menu_options();
    generate_psi_list_items(char_id, usability_mask, category_mask);
    print_menu_items();
    clear_instant_printing();
}


/*
 * DISPLAY_CHARACTER_PSI_LIST — Port of asm/text/menu/display_character_psi_list.asm (45 lines).
 *
 * Creates TEXT_STANDARD window, sets the character's name as title,
 * enables pagination (if multi-party), then generates the full PSI
 * ability list for overworld use (all categories, overworld usability).
 *
 * Called by OVERWORLD_PSI_MENU and as a cursor callback during
 * character selection for the PSI menu.
 *
 * char_id: 1-based party member ID.
 */
void display_character_psi_list(uint16_t char_id) {
    if (!ensure_battle_psi_table()) return;

    /* Assembly line 11: create TEXT_STANDARD window */
    create_window(WINDOW_TEXT_STANDARD);

    /* Assembly line 13: US only — tick one frame to show window */
    window_tick_without_instant_printing();

    /* Assembly lines 15-20: if multi-party, enable pagination */
    if ((game_state.player_controlled_party_count & 0xFF) > 1) {
        dt.pagination_window = 1;
    }

    /* Assembly lines 22-34: set window title to character's name.
     * Name is 5 bytes at char_struct::name (EB-encoded). */
    uint16_t char_idx = char_id - 1;
    char name_buf[8];
    for (int j = 0; j < 5; j++)
        name_buf[j] = eb_char_to_ascii(party_characters[char_idx].name[j]);
    name_buf[5] = '\0';
    set_window_title(WINDOW_TEXT_STANDARD, name_buf, 5);

    /* Assembly lines 36-43: generate PSI list.
     * @LOCAL00 = [usability=1 (overworld), category=15 (all)].
     * GENERATE_PSI_LIST clears menu, adds items, prints, clears instant printing. */
    set_instant_printing();
    clear_focus_window_menu_options();
    generate_psi_list_items(char_id, PSI_USE_OVERWORLD, 0x0F);
    print_menu_items();
    clear_instant_printing();
}


/*
 * DISPLAY_PSI_TARGET_AND_COST (asm/text/menu/display_psi_target_and_cost.asm)
 *
 * Creates a window showing the PSI ability's target type and PP cost.
 * Called as cursor_move_callback during PSI ability selection.
 * ability_id: PSI ability index into PSI_ABILITY_TABLE.
 */
void display_psi_target_and_cost(uint16_t ability_id) {
    if (!ensure_battle_psi_table()) return;
    if (!battle_action_table) return;

    /* Create window for target/cost display (WINDOW::PSI_TARGET_COST) */
    create_window(WINDOW_PSI_TARGET_COST);
    window_tick_without_instant_printing();  /* asm line 17 */
    dt.enable_word_wrap = 0;                 /* asm line 18 */

    /* Assembly: reads psi_ability.name (byte 0). If name == 4 (Thunder),
     * shows generic "To enemy" text. Otherwise computes from battle_action. */
    const PsiAbility *psi = &battle_psi_table[ability_id];

    uint16_t text_idx;
    if (psi->name == 4) {
        /* PSI_ID::THUNDER = 4 — special case: just "To enemy" */
        text_idx = 0;
    } else {
        /* Look up direction and target from battle_action_table */
        uint16_t battle_action_id = psi->battle_action;
        uint8_t direction = battle_action_table[battle_action_id].direction;
        uint8_t target = battle_action_table[battle_action_id].target;
        text_idx = (uint16_t)direction * 5 + target;
        if (text_idx >= 10) text_idx = 0;  /* safety clamp */
    }

    const uint8_t *psi_target_data = ASSET_DATA(ASSET_US_DATA_PSI_TARGET_TEXT_BIN);
    print_eb_string(psi_target_data + text_idx * PSI_TARGET_TEXT_LENGTH, PSI_TARGET_TEXT_LENGTH);
    dt.enable_word_wrap = 0xFF;  /* asm lines 78-79 */

    /* Print PP cost on second line.
     * Assembly (US): PRINT_STRING "PP Cost:" (8 chars), PRINT_LETTER SPACE,
     * SET_WINDOW_NUMBER_PADDING(129), SET_TEXT_PIXEL_POSITION(1, 40),
     * then PRINT_NUMBER. */
    set_focus_text_cursor(0, 1);
    const uint8_t *pp_cost_data = ASSET_DATA(ASSET_US_DATA_PP_COST_TEXT_BIN);
    print_eb_string(pp_cost_data, 8);
    { static const uint8_t space = 0x50; print_eb_string(&space, 1); }
    set_window_number_padding(129);
    set_text_pixel_position(1, 40);

    uint16_t battle_action_id = psi->battle_action;
    uint8_t pp_cost = battle_action_table[battle_action_id].pp_cost;
    print_number(pp_cost, 1);
}


/*
 * DISPLAY_PSI_DESCRIPTION — Port of asm/text/menu/display_psi_description.asm (46 lines).
 *
 * Cursor move callback for PSI ability selection in the status menu.
 * Shows the target type / PP cost window and the PSI description text.
 * US-only: caches LAST_SELECTED_PSI_DESCRIPTION to avoid redundant redraws.
 */
void display_psi_description(uint16_t ability_id) {
    /* US-only optimization: if same ability already shown, skip */
    if (bt.last_selected_psi_description != 0x00FF &&
        ability_id == bt.last_selected_psi_description) {
        return;
    }

    /* Show target type and PP cost (creates PSI_TARGET_COST window) */
    display_psi_target_and_cost(ability_id);

    /* Create PSI description window */
    create_window(WINDOW_PSI_DESCRIPTION);

    /* Tick one frame to show the window before printing text */
    window_tick_without_instant_printing();

    /* Update cache */
    bt.last_selected_psi_description = ability_id;

    /* Read the 4-byte description text pointer from PSI_ABILITY_TABLE.
     * Assembly: LOADPTR PSI_ABILITY_TABLE; offset = ability_id * 15 + 11;
     * DEREFERENCE_PTR_TO reads the SNES address stored at that offset. */
    if (!ensure_battle_psi_table()) return;
    uint32_t text_addr = battle_psi_table[ability_id].text;

    /* Display the PSI description text script */
    display_text_from_snes_addr(text_addr);

    clear_instant_printing();
}


/*
 * BATTLE_PSI_MENU (asm/battle/battle_psi_menu.asm)
 *
 * PSI selection menu for battle. Shows PSI categories (Offense/Recover/
 * Assist/Other), then individual PSI abilities, handles PP cost checking,
 * targeting, and returns the selected action.
 *
 * Reads bt.battle_menu_user for the character ID.
 * On success, populates battle_menu_selection fields (param1, selected_action,
 * targetting, selected_target).
 *
 * Returns 0 if cancelled, nonzero on success.
 */
uint16_t battle_psi_menu(void) {
    if (!ensure_battle_psi_table()) return 0;

    uint16_t result = 0;
    uint8_t char_id = bt.battle_menu_user;
    uint16_t psi_selection = 0;
    uint16_t category = 0;
    uint16_t check_result = 0;
    uint16_t battle_action_id = 0;
    uint8_t action_direction = 0;
    uint8_t action_target = 0;

open_category_window:
    /* Create PSI category window (WINDOW::BATTLE_PSI_CATEGORY = 0x10) */
    create_window(WINDOW_PSI_CATEGORY);

    /* Add category menu items (Offense, Recover, Assist, Other)
     * Assembly: loops 0..2 adding categories 1-3, then adds 4th.
     * Uses ADD_MENU_ITEM_NO_POSITION with userdata = category (1-based). */
    for (uint16_t i = 0; i < PSI_CATEGORY_COUNT; i++) {
        /* Decode EB-encoded category name from ROM data to ASCII for menu */
        const uint8_t *eb = ASSET_DATA(ASSET_US_DATA_PSI_CATEGORIES_BIN) + i * PSI_CATEGORY_NAME_SIZE;
        char ascii_buf[PSI_CATEGORY_NAME_SIZE + 1];
        int len = 0;
        for (int j = 0; j < PSI_CATEGORY_NAME_SIZE && eb[j] != 0; j++)
            ascii_buf[len++] = eb_char_to_ascii(eb[j]);
        ascii_buf[len] = '\0';
        add_menu_item(ascii_buf, i + 1, 0, i * 2);
    }

    win.current_focus_window = WINDOW_PSI_CATEGORY;

    /* Layout and print category items (assembly: OPEN_WINDOW_AND_PRINT_MENU, battle_psi_menu.asm:57) */
    open_window_and_print_menu(1, 0);

    bool psi_menu_printed = false;  /* @LOCALEB — US-only first-entry guard */

category_selection:
    /* Assembly (battle_psi_menu.asm:58-60): SET_WINDOW_FOCUS every time */
    set_window_focus(WINDOW_PSI_CATEGORY);
    /* Set cursor move callback to generate PSI list when moving between categories */
    set_cursor_move_callback(generate_battle_psi_list_callback);

    /* Print menu items, but only on first entry (US: battle_psi_menu.asm:61-68) */
    if (!psi_menu_printed) {
        print_menu_items();
        psi_menu_printed = true;
    }

    /* Run category selection menu (allow cancel) */
    category = selection_menu(1);

    clear_cursor_move_callback();

    if (category == 0) {
        /* Cancelled - close and return */
        result = 0;
        goto close_and_return;
    }

    /* Check if the selected category has any PSI abilities for this character */
    if (check_psi_category_available(category, char_id) == 0) {
        goto category_selection;
    }

open_psi_list:
    /* Create text standard window for PSI list */
    create_window(WINDOW_TEXT_STANDARD);

    /* Generate the PSI list for the selected category */
    generate_battle_psi_list_callback(category);

    /* Set cursor move callback to display target/cost info */
    set_cursor_move_callback(display_psi_target_and_cost);

    /* Run PSI selection menu (allow cancel) */
    psi_selection = selection_menu(1);

    clear_cursor_move_callback();

    if (psi_selection == 0) {
        /* PSI cancelled: @PSI_CANCELLED sets check_result=1 */
        check_result = 1;
        goto check_result_label;
    }

    /* PSI ability selected. Look up the battle_action to check PP cost. */
    battle_action_id = battle_psi_table[psi_selection].battle_action;

    /* Check PP cost against character's current PP */
    {
        uint8_t pp_cost = battle_action_table ? battle_action_table[battle_action_id].pp_cost : 0;
        CharStruct *ch = &party_characters[char_id - 1];

        if (pp_cost > ch->current_pp_target) {
            /* Not enough PP — show message (assembly: battle_psi_menu.asm lines 141-146) */
            create_window(WINDOW_TEXT_BATTLE);
            dt.blinking_triangle_flag = 2;
            display_text_from_snes_addr(MSG_BTL_PSI_CANNOT_MENU);
            dt.blinking_triangle_flag = 0;  /* CLEAR_BLINKING_PROMPT */
            close_focus_window();
            check_result = 0;
            goto check_result_label;
        }
    }

    /* Get action direction and target type for window management */
    action_direction = battle_action_table ? battle_action_table[battle_action_id].direction : 0;
    action_target = battle_action_table ? battle_action_table[battle_action_id].target : 0;

    /* Assembly: for single-target PSI (direction==0 with target==1 or 3),
     * close the PSI windows and show the PSI name in BATTLE_ACTION_NAME */
    if (action_direction == 0 && (action_target == 1 || action_target == 3)) {
        close_window(WINDOW_PSI_CATEGORY);
        close_window(0x04);
        close_window(WINDOW_TEXT_STANDARD);
        create_window(WINDOW_BATTLE_ACTION_NAME);
        set_window_palette_index(6);
        print_psi_ability_name(psi_selection);
        set_window_palette_index(0);
    }

    /* Determine targeting for the selected PSI */
    check_result = determine_targetting(battle_action_id, char_id);

    /* Close windows based on whether we opened BATTLE_ACTION_NAME */
    if (action_direction == 0 && (action_target == 1 || action_target == 3)) {
        close_window(WINDOW_BATTLE_ACTION_NAME);
    } else {
        close_window(WINDOW_PSI_CATEGORY);
        close_window(0x04);
        close_window(WINDOW_TEXT_STANDARD);
    }

    if ((check_result & 0xFF) == 0) {
        /* Targeting cancelled - go back to category window */
        goto open_category_window;
    }

    /* Fall through to check_result_label with non-zero check_result */

check_result_label:
    /* Assembly @CHECK_RESULT: if check_result == 0 (PP check failed), reopen PSI list */
    if (check_result == 0) {
        goto open_psi_list;
    }

    /* Close the target/cost info window */
    close_window(0x04);

    /* If PSI was cancelled (psi_selection == 0), go back to category */
    if (psi_selection == 0) {
        goto category_selection;
    }

    /* Store results in battle_menu_selection */
    bt.battle_menu_param1 = (uint8_t)psi_selection;
    bt.battle_menu_selected_action = battle_psi_table[psi_selection].battle_action;
    bt.battle_menu_targetting = (uint8_t)(check_result >> 8);
    bt.battle_menu_selected_target = (uint8_t)(check_result & 0xFF);
    result = 1;

close_and_return:
    close_window(WINDOW_TEXT_STANDARD);
    close_window(WINDOW_PSI_CATEGORY);
    return result;
}


void show_psi_animation(uint16_t anim_id) {
    if (anim_id >= PSI_ANIM_COUNT) return;

    /* Load PSI_ANIM_CFG from ROM asset */
    size_t cfg_size = ASSET_SIZE(ASSET_DATA_PSI_ANIM_CFG_BIN);
    const uint8_t *cfg_data = ASSET_DATA(ASSET_DATA_PSI_ANIM_CFG_BIN);
    if (!cfg_data) return;
    if (cfg_size < (size_t)((anim_id + 1) * sizeof(PsiAnimConfig))) {
        return;
    }

    const PsiAnimConfig *cfg_table = (const PsiAnimConfig *)cfg_data;
    const PsiAnimConfig *entry = &cfg_table[anim_id];

    uint16_t gfx_loword = entry->gfx_loword;
    uint8_t  frame_hold = entry->frame_hold;
    uint8_t  pal_anim_frames = entry->pal_anim_frames;
    uint8_t  pal_anim_lower = entry->pal_anim_lower;
    uint8_t  pal_anim_upper = entry->pal_anim_upper;
    uint8_t  total_frames = entry->total_frames;
    uint8_t  target_type = entry->target_type;
    uint8_t  color_change_start = entry->color_change_start;
    uint8_t  color_change_duration = entry->color_change_duration;
    uint16_t color_rgb = entry->color_rgb;

    /* Determine which GFX set to load */
    int gfx_set;
    switch (gfx_loword) {
    case PSI_ANIM_GFX_LOWORD_SET1: gfx_set = 0; break;
    case PSI_ANIM_GFX_LOWORD_SET2: gfx_set = 1; break;
    case PSI_ANIM_GFX_LOWORD_SET3: gfx_set = 2; break;
    case PSI_ANIM_GFX_LOWORD_SET4: gfx_set = 3; break;
    default: return;
    }

    /* Load and decompress GFX */
    size_t gfx_compressed_size = ASSET_SIZE(ASSET_PSIANIMS_GFX(gfx_set));
    const uint8_t *gfx_compressed = ASSET_DATA(ASSET_PSIANIMS_GFX(gfx_set));
    if (!gfx_compressed) return;

    bool is_2bpp = (loaded_bg_data_layer1.bitdepth == 2);
    uint16_t displayed_pal_base;

    if (is_2bpp) {
        /* 2bpp mode: decompress directly to VRAM $0000, $1000 bytes.
         * Assembly: COPY_TO_VRAM3P @VIRTUAL06, $0000, $1000, 0 (line 36).
         * Intentional divergence: assembly stages through BUFFER+$8000. */
        decomp(gfx_compressed, gfx_compressed_size, &ppu.vram[0x0000], 0x1000);
        displayed_pal_base = 3 * 16; /* BPP4PALETTE_SIZE * 3 = palette group 3 */
    } else {
        /* 4bpp mode: decompress 2bpp to VRAM, then expand in-place to 4bpp.
         * Assembly: decompress to BUFFER, convert 2bpp→4bpp at BUFFER+$8000,
         * then DMA to VRAM. We expand directly in ppu.vram (iterate in reverse
         * so source bytes aren't overwritten before being read).
         * Assembly: 256-iteration loop at lines 61-147 of show_psi_animation.asm. */
        decomp(gfx_compressed, gfx_compressed_size, &ppu.vram[0x0000], 0x1000);

        /* Expand 2bpp→4bpp in-place (reverse order to avoid clobber):
         * 256 tiles × 16 bytes 2bpp → 256 tiles × 32 bytes 4bpp.
         * planes 0-1 copied, planes 2-3 zeroed. */
        for (int t = 255; t >= 0; t--) {
            memset(&ppu.vram[t * 32 + 16], 0, 16);     /* zero planes 2-3 */
            memmove(&ppu.vram[t * 32], &ppu.vram[t * 16], 16); /* move planes 0-1 */
        }
        displayed_pal_base = 4 * 16; /* BPP4PALETTE_SIZE * 4 = palette group 4 */
    }

    psi_animation_state.displayed_palette = displayed_pal_base;

    /* Wait one frame for VRAM upload to complete */
    render_frame_tick();

    /* Load PSI palette for this animation (4 colors = 8 bytes) */
    size_t pal_size = ASSET_SIZE(ASSET_PSIANIMS_PALETTES(anim_id));
    const uint8_t *pal_data = ASSET_DATA(ASSET_PSIANIMS_PALETTES(anim_id));
    if (pal_data) {
        uint16_t copy_bytes = (uint16_t)(pal_size < 8 ? pal_size : 8);
        /* Copy to psi_animation_state.palette */
        memcpy(psi_animation_state.palette, pal_data, copy_bytes);
        /* Copy to displayed palette location in global ert.palettes[] */
        memcpy(&ert.palettes[displayed_pal_base], pal_data, copy_bytes);
    }

    /* Load bundled arrangement asset.
     * Each .arr.bundled contains 8-frame bundles compressed independently.
     * Only one bundle (8 KB) is decompressed at a time into ert.buffer,
     * which is free during battle (verified by concurrency analysis). */
    {
        psi_animation_state.arr_bundled_data = ASSET_DATA(ASSET_PSIANIMS_ARRANGEMENTS(anim_id));
        psi_animation_state.arr_bundled_size = ASSET_SIZE(ASSET_PSIANIMS_ARRANGEMENTS(anim_id));
        psi_animation_state.arr_bundle_buf = ert.buffer;
        psi_animation_state.arr_current_bundle = -1; /* none decompressed yet */
    }

    /* Initialize animation state from CFG entry */
    psi_animation_state.frame_data = 0; /* current frame index (0-based) */
    psi_animation_state.time_until_next_frame = 1;
    psi_animation_state.frame_hold_frames = frame_hold;
    psi_animation_state.total_frames = total_frames;
    psi_animation_state.palette_animation_frames = pal_anim_frames;
    psi_animation_state.palette_animation_lower_index = pal_anim_lower;
    psi_animation_state.palette_animation_upper_index = pal_anim_upper;
    psi_animation_state.palette_animation_current_index = 0;
    psi_animation_state.palette_animation_time_until_next_frame = 1;
    psi_animation_state.enemy_colour_change_start_frames_left = color_change_start;
    psi_animation_state.enemy_colour_change_frames_left = color_change_duration;
    psi_animation_state.enemy_colour_change_red = color_rgb & 0x1F;
    psi_animation_state.enemy_colour_change_green = (color_rgb >> 5) & 0x1F;
    psi_animation_state.enemy_colour_change_blue = (color_rgb >> 10) & 0x1F;

    /* Darken battle background ert.palettes */
    darken_bg_palettes();

    /* Backup sprite ert.palettes: copy palette groups 8-11 → groups 12-15.
     * Assembly: MEMCPY16(PALETTES+BPP4PALETTE_SIZE*8, PALETTES+BPP4PALETTE_SIZE*12, 128) */
    memcpy(&ert.palettes[12 * 16], &ert.palettes[8 * 16], 128);

    /* Clear PSI_ANIMATION_ENEMY_TARGETS */
    for (int i = 0; i < 4; i++)
        bt.psi_animation_enemy_targets[i] = 0;

    /* Check if current target is conscious and an enemy */
    Battler *tgt = battler_from_offset(bt.current_target);
    if (tgt->consciousness == 0) return;
    if (tgt->ally_or_enemy != 1) return;

    /* Set up target animation offsets */
    bt.psi_animation_x_offset = 0;
    bt.psi_animation_y_offset = 0;

    if (target_type == PSI_TARGET_SINGLE || target_type == PSI_TARGET_RANDOM) {
        /* Single target: center animation on the target enemy */
        bt.psi_animation_x_offset = (int16_t)(128 - tgt->sprite_x);
        bt.psi_animation_y_offset = (int16_t)(144 - tgt->sprite_y);
        uint16_t height = get_battle_sprite_height(tgt->sprite);
        if (height == 8) {
            bt.psi_animation_y_offset += 16;
        }
        tgt->use_alt_spritemap = 1;
        bt.psi_animation_enemy_targets[tgt->vram_sprite_index] = 1;
    } else if (target_type == PSI_TARGET_ROW) {
        /* Row target: hit all enemies in the same y row */
        bt.psi_animation_y_offset = (int16_t)(144 - tgt->sprite_y);
        bool has_small_sprite = false;

        for (int i = 8; i < BATTLER_COUNT; i++) {
            Battler *b = &bt.battlers_table[i];
            if (b->consciousness == 0) continue;
            if (b->ally_or_enemy != 1) continue;
            if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == 1) continue;
            if (b->sprite_y != tgt->sprite_y) continue;

            uint16_t height = get_battle_sprite_height(b->sprite);
            if (height == 8) has_small_sprite = true;
            b->use_alt_spritemap = 1;
            bt.psi_animation_enemy_targets[b->vram_sprite_index] = 1;
        }
        if (has_small_sprite) {
            bt.psi_animation_y_offset += 16;
        }
    } else if (target_type == PSI_TARGET_ALL_ENEMIES) {
        /* All enemies */
        bt.psi_animation_y_offset = 16;
        for (int i = 8; i < BATTLER_COUNT; i++) {
            Battler *b = &bt.battlers_table[i];
            if (b->consciousness == 0) continue;
            if (b->ally_or_enemy != 1) continue;
            if (b->afflictions[STATUS_GROUP_PERSISTENT_EASYHEAL] == 1) continue;
            b->use_alt_spritemap = 1;
            bt.psi_animation_enemy_targets[b->vram_sprite_index] = 1;
        }
    }

    /* Set BG scroll offset based on bitdepth */
    if (is_2bpp) {
        ppu.bg_hofs[1] = (uint16_t)bt.psi_animation_x_offset;
        ppu.bg_vofs[1] = (uint16_t)bt.psi_animation_y_offset;
    } else {
        ppu.bg_hofs[0] = (uint16_t)bt.psi_animation_x_offset;
        ppu.bg_vofs[0] = (uint16_t)bt.psi_animation_y_offset;
    }
}


/*
 * UPDATE_PSI_ANIMATION — Port of asm/battle/psi_animation_fill_data.asm (242 lines).
 *
 * Called every frame during battle screen effects. Handles:
 *   1) Frame timer countdown → upload frame tilemap to VRAM $5800
 *   2) When all frames done → clear tilemap VRAM, restore BG ert.palettes
 *   3) Palette rotation animation (cyclic rotation of palette entries)
 *   4) Enemy color change: when start timer expires, apply palette effects
 *   5) Enemy color restore: when duration timer expires, reverse effects
 *
 * DMA mode details (from DMA_TABLE):
 *   Mode $06: write LOW bytes only (tile numbers) via register $2118, VMAIN=$00
 *   Mode $0F: write HIGH bytes with fixed source ($30) via register $2119, VMAIN=$80
 *   Mode $03: fixed source word-pair clear (zeros) via registers $2118/$2119
 * All three cover 0x400 VRAM words ($5800-$5BFF = 32×32 tilemap).
 */
void update_psi_animation(void) {
    /* --- Frame advancement (C2E6B3 lines 22-69) ---
     * When time_until_next_frame is already 0, the animation is inactive:
     * skip frame AND palette processing, go straight to enemy color change. */
    if (psi_animation_state.time_until_next_frame != 0) {
        psi_animation_state.time_until_next_frame--;

        if (psi_animation_state.time_until_next_frame == 0) {
            /* Timer just expired */
            if (psi_animation_state.total_frames == 0) {
                /* All frames done: clear entire tilemap region ($800 bytes at VRAM $5800).
                 * Assembly: COPY_TO_VRAM1 PSI_ANIMATION_FILL_DATA+1, $5800, $800, $03
                 * Mode $03 = fixed source {$00,$00}, word-pair writes. */
                memset(&ppu.vram[0x5800 * 2], 0, 0x800);
                restore_bg_palette_backups();
            } else {
                /* Reset frame hold timer */
                psi_animation_state.time_until_next_frame =
                    psi_animation_state.frame_hold_frames;

                /* Decompress the bundle containing this frame if not already resident.
                 * Each bundle = 8 frames × 0x400 bytes = 8 KB decompressed. */
                uint32_t frame_idx = psi_animation_state.frame_data;
                int16_t bundle_idx = (int16_t)(frame_idx / PSI_FRAMES_PER_BUNDLE);

                if (bundle_idx != psi_animation_state.arr_current_bundle) {
                    /* Parse bundled header to find this bundle's compressed data */
                    const uint8_t *asset = psi_animation_state.arr_bundled_data;
                    if (asset && psi_animation_state.arr_bundled_size >= 4) {
                        /* Header: u8 frames_per_bundle, u8 total_frames, u16 bundle_count */
                        uint16_t bundle_count = read_u16_le(&asset[2]);
                        if (bundle_idx < (int16_t)bundle_count) {
                            /* Offset table starts at byte 4, bundle_count+1 entries × 2 bytes */
                            size_t offset_table = 4;
                            size_t data_start = offset_table + ((size_t)bundle_count + 1) * 2;
                            uint16_t comp_off = read_u16_le(&asset[offset_table + bundle_idx * 2]);
                            uint16_t comp_end = read_u16_le(&asset[offset_table + (bundle_idx + 1) * 2]);
                            size_t comp_size = comp_end - comp_off;
                            decomp(&asset[data_start + comp_off], comp_size,
                                   psi_animation_state.arr_bundle_buf,
                                   PSI_FRAMES_PER_BUNDLE * PSI_FRAME_SIZE);
                        }
                    }
                    psi_animation_state.arr_current_bundle = bundle_idx;
                }

                /* Read frame from the decompressed bundle staging buffer */
                uint32_t frame_in_bundle = frame_idx % PSI_FRAMES_PER_BUNDLE;
                const uint8_t *frame_src = &psi_animation_state.arr_bundle_buf[frame_in_bundle * PSI_FRAME_SIZE];

                /* Upload frame tilemap to VRAM $5800.
                 * DMA mode $06: writes LOW bytes only (tile numbers from frame data).
                 * DMA mode $0F: writes HIGH bytes only (fixed $30 = palette 1, priority 1).
                 * Each frame = 0x400 tile entries covering the 32×32 BG tilemap. */
                for (int i = 0; i < PSI_FRAME_SIZE; i++) {
                    ppu.vram[0x5800 * 2 + i * 2]     = frame_src[i];
                    ppu.vram[0x5800 * 2 + i * 2 + 1] = 0x30;
                }

                /* Advance frame index and decrement remaining frames */
                psi_animation_state.frame_data++;
                psi_animation_state.total_frames--;
            }
        }

        /* --- Palette rotation (C2E6B3 lines 70-169) ---
         * Only runs when the frame timer was non-zero on entry (animation active). */
        if (psi_animation_state.palette_animation_time_until_next_frame != 0) {
            psi_animation_state.palette_animation_time_until_next_frame--;

            if (psi_animation_state.palette_animation_time_until_next_frame == 0) {
                /* Reset palette animation timer */
                psi_animation_state.palette_animation_time_until_next_frame =
                    psi_animation_state.palette_animation_frames;

                /* Rotate palette entries between lower and upper indices.
                 * Each frame, the "window" shifts by 1 position, creating a
                 * cycling color effect on the PSI animation. */
                uint16_t lower = psi_animation_state.palette_animation_lower_index;
                uint16_t count = (uint16_t)(
                    psi_animation_state.palette_animation_upper_index - lower) + 1;
                uint16_t current = psi_animation_state.palette_animation_current_index;

                for (uint16_t x = 0; x < count; x++) {
                    uint16_t mapped;
                    if (x < current) {
                        /* Wrap around: indices before current_index map to end */
                        mapped = x + count - current;
                    } else {
                        mapped = x - current;
                    }
                    ert.palettes[psi_animation_state.displayed_palette + lower + x] =
                        psi_animation_state.palette[lower + mapped];
                }

                /* Advance current index with wrap */
                psi_animation_state.palette_animation_current_index++;
                if (psi_animation_state.palette_animation_current_index >= count) {
                    psi_animation_state.palette_animation_current_index = 0;
                }

                set_palette_upload_mode(24);
            }
        }
    }

    /* --- Enemy color change start (C2E6B3 lines 170-214) ---
     * When enemy_colour_change_start_frames_left counts down to 0,
     * apply color shift to all targeted enemy sprite ert.palettes. */
    if (psi_animation_state.enemy_colour_change_start_frames_left != 0) {
        psi_animation_state.enemy_colour_change_start_frames_left--;
        if (psi_animation_state.enemy_colour_change_start_frames_left == 0) {
            set_battle_sprite_palette_effect_speed(20);
            for (uint16_t enemy = 0; enemy < 4; enemy++) {
                if (bt.psi_animation_enemy_targets[enemy] == 0) continue;
                /* Apply color effect to palette entries 1-15 of this enemy's
                 * sprite palette (skip entry 0 = transparent). */
                for (uint16_t color = 1; color < 16; color++) {
                    setup_battle_sprite_palette_effect(
                        enemy * 16 + color,
                        psi_animation_state.enemy_colour_change_red,
                        psi_animation_state.enemy_colour_change_green,
                        psi_animation_state.enemy_colour_change_blue);
                }
            }
        }
    }

    /* --- Enemy color restore (C2E6B3 lines 215-242) ---
     * When enemy_colour_change_frames_left counts down to 0,
     * reverse the palette effects to restore original colors. */
    if (psi_animation_state.enemy_colour_change_frames_left != 0) {
        psi_animation_state.enemy_colour_change_frames_left--;
        if (psi_animation_state.enemy_colour_change_frames_left == 0) {
            for (uint16_t enemy = 0; enemy < 4; enemy++) {
                if (bt.psi_animation_enemy_targets[enemy] == 0) continue;
                reverse_battle_sprite_palette_effect(20, enemy);
            }
        }
    }
}


/*
 * APPLY_PSI_BATTLE_EFFECT — Port of asm/battle/apply_psi_battle_effect.asm (135 lines).
 *
 * Dispatches a battle visual effect based on effect_id:
 *   <35  → PSI animation (SHOW_PSI_ANIMATION)
 *   35-45 → darken BG + enemy PSI swirl with color from ENEMY_PSI_COLOURS table
 *   46   → screen wobble (144 frames)
 *   47   → screen shake (300 frames)
 *   48   → nothing
 *   49-53 → darken BG + misc swirl with color from MISC_SWIRL_COLOURS table
 *           (49-52 → swirl type 4/options 5, 53 → swirl type 2/options 4)
 *   >=54 → nothing
 *
 * Color tables are 3-byte entries {R, G, B} with 5-bit components (0-31)
 * fed to set_coldata(). Swirl uses set_colour_addsub_mode(0x10, 0x3F)
 * = CGWSEL=$10 (fixed color sub-screen), CGADSUB=$3F (subtract all).
 */
void apply_psi_battle_effect(uint16_t effect_id) {
    if (effect_id < 35) {
        /* PSI animation */
        show_psi_animation(effect_id);
        return;
    }
    if (effect_id < 46) {
        /* Enemy PSI: darken + swirl with colors from table */
        darken_bg_palettes();
        const uint8_t *tbl = ASSET_DATA(ASSET_DATA_ENEMY_PSI_COLOURS_BIN);
        if (tbl) {
            uint16_t idx = (effect_id - 35) * 3;
            uint8_t red   = tbl[idx];
            uint8_t green = tbl[idx + 1];
            uint8_t blue  = tbl[idx + 2];
            set_coldata(red, green, blue);
        }
        set_colour_addsub_mode(0x10, 0x3F);
        init_swirl_effect(5, 7);
        return;
    }
    if (effect_id < 49) {
        /* Assembly: INC then compare. effect_id 46 → 47 → wobble,
         * 47 → 48 → shake, 48 → 49 → nothing (falls through). */
        uint16_t check = effect_id + 1;
        if (check == 47) {
            bt.wobble_duration = 144;
        } else if (check == 48) {
            bt.shake_duration = 300;
        }
        /* 48+1=49 → fall through to done (not matched by misc swirl either
         * since the assembly checks effect_id >= 49 separately). */
        return;
    }
    if (effect_id < 54) {
        /* Misc swirl: darken + swirl with colors from table */
        darken_bg_palettes();
        const uint8_t *tbl = ASSET_DATA(ASSET_DATA_MISC_SWIRL_COLOURS_BIN);
        if (tbl) {
            uint16_t idx = (effect_id - 49) * 3;
            uint8_t red   = tbl[idx];
            uint8_t green = tbl[idx + 1];
            uint8_t blue  = tbl[idx + 2];
            set_coldata(red, green, blue);
        }
        set_colour_addsub_mode(0x10, 0x3F);
        if (effect_id >= 53) {
            /* Slow swirl for IDs 53+ */
            init_swirl_effect(2, 4);
        } else {
            init_swirl_effect(4, 5);
        }
        return;
    }
    /* effect_id >= 54: nothing */
}

