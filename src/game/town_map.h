#ifndef GAME_TOWN_MAP_H
#define GAME_TOWN_MAP_H

#include "core/types.h"

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

/* SHOW_TOWN_MAP (C13CE5) — checks for Town Map item, shows map if found. */
void show_town_map(void);

/* Overworld X-button town-map entry, split for GAME_MODE_OVERWORLD. Runs
 * show_town_map()/display_town_map()'s synchronous front half: if the party has
 * the Town Map item AND the leader's location has a map, disables entities, fills
 * *init with the GAME_MODE_TOWN_MAP (display variant) push, and returns true — the
 * OW mode pushes it then re-enables entities on resume. Returns false (no push, no
 * entity bracketing left dangling) if there is no map to show. */
typedef union ModeState ModeState;
bool show_town_map_prepare(ModeState *init);

/* DISPLAY_TOWN_MAP (C4D681) — full display with auto map selection from
 * leader position. Returns the map_id shown (0 if none). */
uint16_t display_town_map(void);

/* RUN_TOWN_MAP_MENU (C4D744) — display with up/down map selection. */
void run_town_map_menu(void);

/* run_town_map_menu()'s front half: set the icon-animation globals and fill *init
 * with the menu-mode TOWN_MAP display push, for mode callers that STEP_PUSH it. */
void run_town_map_menu_prepare(ModeState *init);

#endif /* GAME_TOWN_MAP_H */
