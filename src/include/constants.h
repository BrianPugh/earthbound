#ifndef CONSTANTS_H
#define CONSTANTS_H

/* From include/enums.asm */

#define EB_NULL    0
#define EB_FALSE   0
#define EB_TRUE    1

#define OUT_OF_BATTLE 0
#define IN_BATTLE     1

/* Characters */
#define CHARACTER_NESS   0
#define CHARACTER_PAULA  1
#define CHARACTER_JEFF   2
#define CHARACTER_POO    3

#define NUM_ENEMIES      231
#define EVENT_FLAG_COUNT  1024

/* Event flag IDs (1-based, from include/constants/event_flags.asm) */
#define EVENT_FLAG_NESS_PAJAMA    749  /* FLG_MYHOME_NES_CHANGE */
#define EVENT_FLAG_BUNBUN         18  /* FLG_BUNBUN — BuzzBuzz in party */
#define EVENT_FLAG_MONSTER_OFF    11  /* FLG_SYS_MONSTER_OFF */
#define EVENT_FLAG_DIS_2H_PAPA  775  /* FLG_SYS_DIS_2H_PAPA — disables dad phone */
#define EVENT_FLAG_WIN_GIEGU     73  /* FLG_WIN_GIEGU — defeated Giygas */
#define EVENT_FLAG_DISABLE_TELEPORT 754 /* FLG_SYS_DISTLPT — disable PSI Teleport */

/* Party member NPCs (from include/enums.asm PARTY_MEMBER enum) */
#define PARTY_NPC_DUNGEON_MAN  10

/* Map sector config flags (from include/enums.asm MAP_SECTOR_CONFIG) */
#define MAP_SECTOR_CANNOT_TELEPORT  0x0080  /* bit 7 */

/* Palette sizes */
#define COLOUR_SIZE       2
#define BPP4PALETTE_SIZE  (COLOUR_SIZE * 16)
#define BPP2PALETTE_SIZE  (COLOUR_SIZE * 4)
#define PALETTE_SIZE      BPP4PALETTE_SIZE

/* Save system */
#define SAVE_COUNT       3
#define SAVE_COPY_COUNT  2
#define SRAM_VERSION     0x493  /* US retail */

/* Entities */
#define MAX_ENTITIES  30
#define MAX_SCRIPTS   70
#define INIT_ENTITY_SLOT          23
#define PARTY_LEADER_ENTITY_INDEX 24
#define NPC_BASE_ENTITY_SLOT      28

/* Teleport styles (from include/enums.asm TELEPORT_STYLE enum) */
#define TELEPORT_STYLE_NONE       0
#define TELEPORT_STYLE_PSI_ALPHA  1
#define TELEPORT_STYLE_PSI_BETA   2
#define TELEPORT_STYLE_INSTANT    3
#define TELEPORT_STYLE_PSI_BETTER 4
#define TELEPORT_STYLE_STAR_MASTER 5

/* Direction values (from include/enums.asm DIRECTION enum) */
enum Direction {
    DIRECTION_UP         = 0,
    DIRECTION_UP_RIGHT   = 1,
    DIRECTION_RIGHT      = 2,
    DIRECTION_DOWN_RIGHT = 3,
    DIRECTION_DOWN       = 4,
    DIRECTION_DOWN_LEFT  = 5,
    DIRECTION_LEFT       = 6,
    DIRECTION_UP_LEFT    = 7,
    DIRECTION_NONE       = 8,
};

/* Text window flavour */
enum TextFlavour {
    TEXT_FLAVOUR_NONE       = 0,
    TEXT_FLAVOUR_PLAIN      = 1,
    TEXT_FLAVOUR_MINT       = 2,
    TEXT_FLAVOUR_STRAWBERRY = 3,
    TEXT_FLAVOUR_BANANA     = 4,
    TEXT_FLAVOUR_PEANUT     = 5,
};

/* Overworld sprites (from include/constants/overworldsprites.asm OVERWORLD_SPRITE enum) */
#define OVERWORLD_SPRITE_NONE           0
#define OVERWORLD_SPRITE_NESS           1
#define OVERWORLD_SPRITE_PAULA          2
#define OVERWORLD_SPRITE_JEFF           3
#define OVERWORLD_SPRITE_POO            4
#define OVERWORLD_SPRITE_NESS_ROBOT     5
#define OVERWORLD_SPRITE_NESS_PAJAMAS   6
#define OVERWORLD_SPRITE_NESS_BICYCLE   7
#define OVERWORLD_SPRITE_MINI_GHOST    264  /* OVERWORLD_SPRITE::MINI_GHOST */
#define OVERWORLD_SPRITE_LEAVES_FOR_TESSIE_SCENE 367  /* OVERWORLD_SPRITE::LEAVES_FOR_TESSIE_SCENE */
#define LOST_UNDERWORLD_SPRITE_OFFSET  26  /* LIL_NESS=27, LIL_PAULA=28, etc. */

/* Event scripts (from include/constants/event.asm EVENT_SCRIPT enum) */
#define EVENT_SCRIPT_001  1    /* EVENT_001: main overworld tick */
#define EVENT_SCRIPT_002  2    /* EVENT_002: party follower entity */
#define EVENT_SCRIPT_003  3    /* EVENT_003: Bubble Monkey follower */
#define EVENT_SCRIPT_DESPAWN        35   /* EVENT_DESPAWN: deallocate entity sprite */
#define EVENT_SCRIPT_MINI_GHOST    786   /* EVENT_MINI_GHOST: mini ghost entity */
#define EVENT_SCRIPT_ENTITY_WIPE   859   /* EVENT_ENTITY_WIPE: fade/wipe animation driver */
#define EVENT_SCRIPT_799  799  /* Photo object entity script */
#define EVENT_SCRIPT_800  800  /* Photo party entity script */
#define EVENT_SCRIPT_801  801  /* Cast scene entity wipe script */

/* Screen */
#define SCREEN_X_RESOLUTION VIEWPORT_WIDTH
#define SCREEN_Y_RESOLUTION VIEWPORT_HEIGHT

/* Timing (in frames at 60fps) */
#define FRAMES_PER_SECOND    60

/* VRAM addresses (word addresses) */
#define VRAM_LOGO_TILES       0x0000
#define VRAM_LOGO_TILEMAP     0x2000
#define VRAM_TITLE_TILEMAP_EB 0x5800
#define VRAM_TITLE_OBJ_EB     0x6000
#define VRAM_TEXT_LAYER_TILES  0x6000
#define VRAM_TEXT_LAYER_TILEMAP 0x7C00
#define VRAM_OBJ              0x4000
/* Cast scene VRAM (word addresses) */
#define VRAM_CAST_TILES         0x0000  /* = VRAM_TEXT_LAYER_TILES (USA) */
#define VRAM_CAST_TILEMAP       VRAM_TEXT_LAYER_TILEMAP  /* 0x7C00 */
/* Credits VRAM (word addresses) */
#define VRAM_CREDITS_LAYER_1_TILES    0x0000
#define VRAM_CREDITS_LAYER_1_TILEMAP  0x3800
#define VRAM_CREDITS_LAYER_2_TILES    0x2000
#define VRAM_CREDITS_LAYER_2_TILEMAP  0x7000
#define VRAM_CREDITS_LAYER_3_TILES    0x6000
#define VRAM_CREDITS_LAYER_3_TILEMAP  0x6C00
/* Gas station VRAM (word addresses, from include/enums.asm) */
#define VRAM_GAS_STATION_L1_TILES    0x0000
#define VRAM_GAS_STATION_L1_TILEMAP  0x7800
#define VRAM_GAS_STATION_L2_TILES    0x6000
#define VRAM_GAS_STATION_L2_TILEMAP  0x7C00

/* Tilemap size in words */
#define TILEMAP_SIZE  (32 * 32)

/* Number of menu options */
#define NUM_MENU_OPTIONS 70

/* Naming */
#define THINGS_NAMED_COUNT 7

/* Affliction count */
#define AFFLICTION_GROUP_COUNT 7

/* Number of photos */
#define NUM_PHOTOS 32

/* File menu naming enum */
enum {
    FILE_MENU_NEW_GAME_NAME_CHAR_1 = 0,
    FILE_MENU_NEW_GAME_NAME_CHAR_2,
    FILE_MENU_NEW_GAME_NAME_CHAR_3,
    FILE_MENU_NEW_GAME_NAME_CHAR_4,
    FILE_MENU_NEW_GAME_NAME_DOG,
    FILE_MENU_NEW_GAME_NAME_FAVORITE_FOOD,
    FILE_MENU_NEW_GAME_NAME_FAVORITE_THING,
};

/* Font enum */
enum {
    FONT_NORMAL = 0,
    FONT_MRSATURN,
    FONT_BATTLE,
    FONT_TINY,
    FONT_LARGE,
};

/* Item IDs — generated from items.json (full 254-entry enum) */
#include "items_generated.h"

/* Walking styles (from include/enums.asm WALKING_STYLE) */
#define WALKING_STYLE_NORMAL     0
#define WALKING_STYLE_BICYCLE    3
#define WALKING_STYLE_GHOST      4
#define WALKING_STYLE_SLOWER     6
#define WALKING_STYLE_LADDER     7
#define WALKING_STYLE_ROPE       8
#define WALKING_STYLE_SLOWEST   10
#define WALKING_STYLE_ESCALATOR 12
#define WALKING_STYLE_STAIRS    13

/* VRAM size */
#ifndef VRAM_SIZE
#define VRAM_SIZE  0x10000
#endif

/* Ending / credits */
#define CREDITS_LENGTH        0x11B0  /* USA: total scroll distance in pixels */
#define STAFF_CREDITS_FONT_GFX_SIZE  0xC00  /* USA decompressed font size */

/* BG tilemap size enum (from include/hardware.asm) */
#define BG_TILEMAP_SIZE_NORMAL      0
#define BG_TILEMAP_SIZE_HORIZONTAL  1
#define BG_TILEMAP_SIZE_VERTICAL    2
#define BG_TILEMAP_SIZE_BOTH        3

/* Battle background layers (from include/constants/battlebgs.asm) */
#define BATTLEBG_LAYER_NONE        0
#define BATTLEBG_LAYER_UNKNOWN279  279

/* Photographer config entry field offsets (from include/structs.asm) */
#define PHOTOGRAPHER_CFG_ENTRY_SIZE         62
#define PHOTOGRAPHER_CFG_EVENT_FLAG          0
#define PHOTOGRAPHER_CFG_MAP_X               2
#define PHOTOGRAPHER_CFG_MAP_Y               4
#define PHOTOGRAPHER_CFG_PALETTES_OFFSET     6
#define PHOTOGRAPHER_CFG_SLIDE_DIRECTION     8
#define PHOTOGRAPHER_CFG_SLIDE_DISTANCE      9
#define PHOTOGRAPHER_CFG_PHOTOGRAPHER_X     10
#define PHOTOGRAPHER_CFG_PHOTOGRAPHER_Y     12
#define PHOTOGRAPHER_CFG_PARTY_CONFIG       14
#define PHOTOGRAPHER_CFG_OBJECT_CONFIG      38
/* photographer_config_entry_object: 6 bytes (tile_x:2, tile_y:2, sprite:2) */
#define PHOTOGRAPHER_OBJ_TILE_X  0
#define PHOTOGRAPHER_OBJ_TILE_Y  2
#define PHOTOGRAPHER_OBJ_SPRITE  4
#define PHOTOGRAPHER_OBJ_SIZE    6

#endif /* CONSTANTS_H */
