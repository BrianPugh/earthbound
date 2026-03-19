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

/* DISPLAY_TOWN_MAP (C4D681) — full display with auto map selection from
 * leader position. Returns the map_id shown (0 if none). */
uint16_t display_town_map(void);

/* RUN_TOWN_MAP_MENU (C4D744) — display with up/down map selection. */
void run_town_map_menu(void);

#endif /* GAME_TOWN_MAP_H */
