/*
 * Event script data — runtime-loaded from ROM binary assets.
 *
 * The actual bytecode and spritemap data is extracted from the donor ROM
 * by ebtools and loaded at runtime from binary files.
 *
 * The general event script system uses:
 *   - EVENT_SCRIPT_POINTERS: table of 3-byte (PTR3) ROM addresses, one per script ID
 *   - Script bank buffers: contiguous ROM regions loaded into memory
 *   - Script IDs are looked up in the pointer table to find their ROM address,
 *     then the ROM address is resolved to a buffer offset within the correct bank
 *
 * Title screen scripts (EVENT_BATTLE_FX=787, TITLE_SCREEN_1-11=788-798) are in bank C4, starting at $C42172.
 * Naming screen scripts (EVENT_502-533) span banks C3 and C4.
 */
#ifndef DATA_EVENT_SCRIPT_DATA_H
#define DATA_EVENT_SCRIPT_DATA_H

#include "core/types.h"

/* Opcode constants (from eventmacros.asm) */
#define OP_END              0x00
#define OP_LOOP             0x01
#define OP_LOOP_END         0x02
#define OP_PAUSE            0x06
#define OP_START_TASK       0x07
#define OP_HALT             0x09
#define OP_SHORTCALL_COND   0x0A  /* jump if tempvar == 0 */
#define OP_SHORTCALL_COND_NOT 0x0B /* jump if tempvar != 0 */
#define OP_END_TASK         0x0C
#define OP_SET_VAR          0x0E
#define OP_WRITE_WORD_WRAM  0x15  /* write 16-bit value to WRAM address */
#define OP_SHORTJUMP        0x19
#define OP_SHORTCALL        0x1A
#define OP_SHORT_RETURN     0x1B
#define OP_SET_ANIM_PTR     0x1C
#define OP_WRITE_WORD_TEMPVAR 0x1D
#define OP_WRITE_WRAM_TEMPVAR 0x1E
#define OP_SET_DRAW_CALLBACK 0x22
#define OP_SET_POS_CALLBACK  0x23
#define OP_SET_MOVE_CALLBACK 0x25
#define OP_SET_X            0x28
#define OP_SET_Y            0x29
#define OP_SET_VEL_ZERO     0x39
#define OP_SET_ANIMATION    0x3B
#define OP_NEXT_ANIM_FRAME  0x3C
#define OP_PREV_ANIM_FRAME  0x3D
#define OP_SKIP_ANIM_FRAMES 0x3E
#define OP_SET_X_VELOCITY   0x3F
#define OP_SET_Y_VELOCITY   0x40
#define OP_SET_Z_VELOCITY   0x41
#define OP_CALLROUTINE      0x42
#define OP_SET_PRIORITY     0x43

/* Additional opcodes for naming/overworld scripts */
#define OP_SET_TICK_CALLBACK     0x08
#define OP_CLEAR_TICK_CALLBACK   0x0F
#define OP_SET_Z                 0x2A
#define OP_SLEEP_FROM_TEMPVAR    0x44

/* Callback IDs for entity callbacks */
#define CB_MOVE_APPLY_DELTA     0  /* APPLY_ENTITY_DELTA_POSITION_ENTRY2 */
#define CB_MOVE_FORCE_MOVE      1  /* ENTITY_PHYSICS_FORCE_MOVE (delta pos + surface flags) */
#define CB_MOVE_PARTY_SPRITE    2  /* UPDATE_PARTY_SPRITE_POSITION (screen coords from abs) */
#define CB_MOVE_WITH_COLLISION  3  /* ENTITY_PHYSICS_WITH_COLLISION (collision check → zero vel or force move) */
#define CB_MOVE_SIMPLE_COLLISION 4  /* ENTITY_PHYSICS_SIMPLE_COLLISION (like WITH but no surface flags) */
#define CB_MOVE_DELTA_3D        5  /* APPLY_ENTITY_DELTA_POSITION_3D (X/Y/Z delta + surface flags) */
#define CB_MOVE_NOP             6  /* APPLY_ENTITY_DELTA_POSITION_ENTRY4 (no-op, bare RTS) */
#define CB_POS_SCREEN_BG1       0  /* ENTITY_SCREEN_COORDS_BG1 (default) */
#define CB_POS_COPY_ABS         1  /* COPY_ENTITY_ABS_TO_SCREEN */
#define CB_POS_NOP              2  /* POSITION_CALLBACK_NOP (do nothing) */
#define CB_POS_SCREEN_BG3       3  /* ENTITY_WORLD_TO_SCREEN (BG3-relative, ending credits) */
#define CB_POS_SCREEN_BG1_Z     4  /* ENTITY_SCREEN_COORDS_BG1_WITH_Z (BG1 minus Z height) */
#define CB_POS_SCREEN_BG3_Z     5  /* ENTITY_SCREEN_COORDS_BG3_WITH_Z (BG3 minus Z height) */
#define CB_POS_FORCE_MOVE       6  /* ENTITY_PHYSICS_FORCE_MOVE (delta pos + surface flags, used as pos CB) */
#define CB_DRAW_ENTITY_SPRITE   0  /* DRAW_ENTITY_SPRITE (default) */
#define CB_DRAW_TITLE_LETTER    1  /* DRAW_TITLE_LETTER (title screen letter draw) */

/* Title screen event script IDs (global ROM IDs from EVENT_SCRIPT_POINTERS).
 * These are the same IDs used by the assembly — entity_init resolves them
 * through the global event script pointer table, not a local lookup. */
#define EVENT_SCRIPT_TITLE_SCREEN_BG  787  /* EVENT_BATTLE_FX (asm alias) */
#define EVENT_SCRIPT_TITLE_SCREEN_1   788  /* TITLE_SCREEN_1 */
#define EVENT_SCRIPT_TITLE_SCREEN_2   789  /* TITLE_SCREEN_2 */
#define EVENT_SCRIPT_TITLE_SCREEN_3   790  /* TITLE_SCREEN_3 */
#define EVENT_SCRIPT_TITLE_SCREEN_4   791  /* TITLE_SCREEN_4 */
#define EVENT_SCRIPT_TITLE_SCREEN_5   792  /* TITLE_SCREEN_5 */
#define EVENT_SCRIPT_TITLE_SCREEN_6   793  /* TITLE_SCREEN_6 */
#define EVENT_SCRIPT_TITLE_SCREEN_7   794  /* TITLE_SCREEN_7 */
#define EVENT_SCRIPT_TITLE_SCREEN_8   795  /* TITLE_SCREEN_8 */
#define EVENT_SCRIPT_TITLE_SCREEN_9   796  /* TITLE_SCREEN_9 */
#define EVENT_SCRIPT_TITLE_SCREEN_10  797  /* TITLE_SCREEN_10 */
#define EVENT_SCRIPT_TITLE_SCREEN_11  798  /* TITLE_SCREEN_11 */
#define TITLE_SCREEN_SCRIPT_COUNT      12

/* Total entries in EVENT_SCRIPT_POINTERS table */
#define EVENT_SCRIPT_POINTER_COUNT 895

/* WRAM sentinels for WRITE_WRAM_TEMPVAR — interpreter maps to C variables */
#define WRAM_FADE_STEP               0x0028  /* $7E0028 — FADE_PARAMETERS::step, non-zero while fading */
#define WRAM_TITLE_SCREEN_QUICK_MODE 0x9F75  /* $7E9F75 */
#define WRAM_WAIT_FOR_NAMING_SCREEN  0xB4B4  /* $7EB4B4 */

/* POST_TELEPORT_CALLBACK — 4-byte function pointer at $7E9D1B.
 * EVENT_WRITE_DWORD_WRAM emits two WRITE_WORD_WRAM opcodes (lo then hi). */
#define WRAM_POST_TELEPORT_CALLBACK_LO 0x9D1B  /* $7E9D1B */
#define WRAM_POST_TELEPORT_CALLBACK_HI 0x9D1D  /* $7E9D1D */
#define ROM_ADDR_UNDRAW_FLYOVER_TEXT    0xC4800B

/* ROM addresses for CALLROUTINE dispatch */
#define ROM_ADDR_UPDATE_MINI_GHOST_POSITION   0xC0778A
#define ROM_ADDR_UPDATE_BATTLE_SCREEN_EFFECTS 0xC2DB3F
#define ROM_ADDR_SPAWN_ENTITY                 0xC09E71
#define ROM_ADDR_DECOMPRESS_TITLE_DATA        0xC0EC77
#define ROM_ADDR_LOAD_TITLE_PALETTE           0xC0ECB7
#define ROM_ADDR_CYCLE_ENTITY_PALETTE         0xC0EDDA
#define ROM_ADDR_SET_STATE_PAUSED             0xC0EDD1
#define ROM_ADDR_SHOW_ENTITY_SPRITE           0xC0EE53
#define ROM_ADDR_UPDATE_PALETTE_ANIM          0xC426ED
#define ROM_ADDR_SET_STATE_RUNNING            0xC46E46
#define ROM_ADDR_FINALIZE_PALETTE_FADE        0xC49740
#define ROM_ADDR_LOAD_FILE_SELECT_PALETTES    0xC0ED5C
#define ROM_ADDR_FILL_PALETTES_WHITE          0xC0ED14
#define ROM_ADDR_FILL_PALETTES_BLACK          0xC0ED39
/* Gas station callroutines */
#define ROM_ADDR_LOAD_GAS_STATION_PALETTE       0xC0F3E8
#define ROM_ADDR_LOAD_GAS_STATION_FLASH_PALETTE 0xC0F3B2
/* Naming screen callroutines */
#define ROM_ADDR_SET_ENTITY_DIRECTION_AND_FRAME 0xC0AA6E
#define ROM_ADDR_DEALLOCATE_ENTITY_SPRITE       0xC020F1
#define ROM_ADDR_IS_X_LESS_THAN_ENTITY          0xC468B5
#define ROM_ADDR_IS_Y_LESS_THAN_ENTITY          0xC468DC
#define ROM_ADDR_MOVEMENT_CMD_CLEAR_LOOP_COUNTER 0xC0AAFD
#define ROM_ADDR_MOVEMENT_CMD_LOOP              0xC0AAD5
/* File select / overworld entity sprite callroutines */
#define ROM_ADDR_RENDER_ENTITY_SPRITE_ENTRY3    0xC0A4A8
#define ROM_ADDR_RENDER_ENTITY_SPRITE_ME1       0xC0A4B2
#define ROM_ADDR_RENDER_ENTITY_SPRITE_ME2       0xC0A4BF
#define ROM_ADDR_RESET_ENTITY_ANIMATION         0xC40015
/* Overworld movement callroutines — extra bytes consumed from script stream noted */
#define ROM_ADDR_SET_ENTITY_MOVEMENT_SPEED          0xC0A685  /* 2 extra bytes */
#define ROM_ADDR_SET_ENTITY_MOVEMENT_SPEED_ENTRY2   0xC0A68B  /* 0 extra bytes */
#define ROM_ADDR_GET_ENTITY_MOVEMENT_SPEED          0xC0A691  /* 0 extra bytes */
#define ROM_ADDR_SET_DIRECTION8                     0xC0A651  /* 1 extra byte  */
#define ROM_ADDR_SET_DIRECTION                      0xC0A65F  /* 0 extra bytes */
#define ROM_ADDR_SET_SURFACE_FLAGS_CR               0xC0A679  /* 1 extra byte  */
#define ROM_ADDR_SET_ENTITY_VELOCITY_FROM_DIR       0xC47044  /* 0 extra bytes */
#define ROM_ADDR_MOVE_ENTITY_DISTANCE               0xC0A6AD  /* 2 extra bytes */
#define ROM_ADDR_HALVE_ENTITY_DELTA_Y               0xC4730E  /* 0 extra bytes */
#define ROM_ADDR_LOAD_CURRENT_MAP_BLOCK_EVENTS      0xC4733C  /* 0 extra bytes */
#define ROM_ADDR_REVERSE_DIRECTION_8                0xC46B37  /* 0 extra bytes */
#define ROM_ADDR_QUANTIZE_ENTITY_DIRECTION          0xC46B0A  /* 0 extra bytes */
#define ROM_ADDR_CALCULATE_DIRECTION_TO_TARGET      0xC46ADB  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_ENTITY_COLLISION2          0xC0A82F  /* 0 extra bytes */
#define ROM_ADDR_MOVEMENT_DISPLAY_TEXT              0xC0A8A0  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_PLAY_SOUND            0xC0A841  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_COPY_SPRITE_POS       0xC0A86F  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_COPY_LEADER_POS       0xC0A864  /* 1 extra byte  */
#define ROM_ADDR_MOVEMENT_STORE_OFFSET_POSITION     0xC0A8B3  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_GET_EVENT_FLAG        0xC0A84C  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_SET_EVENT_FLAG        0xC0A857  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_QUEUE_INTERACTION         0xC0A88D  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_STORE_NPC_POS         0xC0A92D  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_STORE_SPRITE_POS      0xC0A938  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_FACE_TOWARD_NPC       0xC0A94E  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_FACE_TOWARD_SPRITE    0xC0A959  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_SET_BOUNDING_BOX          0xC0A964  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_SET_POS_FROM_SCREEN       0xC0A87A  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_SET_NPC_ID            0xC0A643  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_ANIMATE_PAL_FADE      0xC0AAB5  /* 4 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_SETUP_SPOTLIGHT       0xC0AA23  /* 6 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_APPLY_COLOR_MATH      0xC0AA3F  /* 3 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_NAME       0xC0A9B3  /* 6 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_VAR0       0xC0A9EB  /* 6 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_PRINT_CAST_PARTY      0xC0A9CF  /* 6 extra bytes */
#define ROM_ADDR_MOVE_TOWARD_NO_SPRITE_CB           0xC0A8DC  /* 0 extra bytes */
#define ROM_ADDR_MOVE_TOWARD_REVERSED_CB            0xC0A8D1  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_DIR_VELOCITY_CB             0xC0A8E7  /* 0 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_PREPARE_ENTITY        0xC0A912  /* 5 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_LEADER     0xC0A8FF  /* 0 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_SELF       0xC0A8F7  /* 0 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_FADE_OUT              0xC09FBB  /* 2 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_FADE_IN               0xC0886C  /* 2 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_GET_PARTY_MEMBER_POS  0xC0A943  /* 1 extra byte  */
#define ROM_ADDR_PREPARE_ENTITY_AT_TELEPORT_DEST    0xC46DE5  /* 1 extra byte  */
#define ROM_ADDR_DISABLE_OBJ_HDMA                   0xC4248A  /* 0 extra bytes */
#define ROM_ADDR_INIT_WINDOW_REGISTERS_CR           0xC423DC  /* 0 extra bytes */
#define ROM_ADDR_DECOMP_ITOI_PRODUCTION             0xC4DD28  /* 0 extra bytes */
#define ROM_ADDR_DECOMP_NINTENDO_PRESENTATION       0xC4DDD0  /* 0 extra bytes */
#define ROM_ADDR_PLAY_FLYOVER_SCRIPT                0xC49EC4  /* 0 extra bytes */
#define ROM_ADDR_CHOOSE_RANDOM                      0xC09F82  /* variable      */
#define ROM_ADDR_TEST_PLAYER_IN_AREA                0xC46E74  /* 0 extra bytes */
#define ROM_ADDR_MAKE_PARTY_LOOK_AT_ENTITY          0xC48B3B  /* 0 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_CALC_TRAVEL_FRAMES     0xC0A6A2  /* 2 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_RETURN_2               0xC0AACD  /* 0 extra bytes */
#define ROM_ADDR_IS_ENTITY_NEAR_LEADER                     0xC0C6B6  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENTITY_OBSTACLES             0xC05E76  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENEMY_MOVEMENT_OBSTACLES    0xC05E82  /* 0 extra bytes */
#define ROM_ADDR_CHECK_NPC_PLAYER_OBSTACLES        0xC05ECE  /* 0 extra bytes */
#define ROM_ADDR_CHECK_PROSPECTIVE_NPC_COLLISION   0xC06478  /* 0 extra bytes */
#define ROM_ADDR_FADE_OUT_WITH_MOSAIC               0xC08814  /* 6 extra bytes */
#define ROM_ADDR_SPAWN_ENTITY_RELATIVE              0xC0A98B  /* 4 extra bytes */
/* ROM_ADDR_CREATE_ENTITY_AT_BG3 merged with ROM_ADDR_CREATE_ENTITY_BG3 (both 0xC0A99F) */
/* Attract mode / overworld entity callroutines */
#define ROM_ADDR_SET_ENTITY_DIRECTION_VELOCITY      0xC0C83B  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_ENTITY_ANIMATION            0xC0A6E3  /* 0 extra bytes */
#define ROM_ADDR_CLEAR_CURRENT_ENTITY_COLLISION     0xC0A6DA  /* 0 extra bytes */
#define ROM_ADDR_INITIALIZE_PARTY_MEMBER_ENTITY     0xC03DAA  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_FOLLOWER_VISUALS            0xC04EF0  /* 0 extra bytes */
#define ROM_ADDR_SRAM_CHECK_ROUTINE_CHECKSUM        0xC1FFD3  /* 0 extra bytes */
#define ROM_ADDR_INFLICT_SUNSTROKE_CHECK            0xC20000  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_ENTITY_SURFACE_FLAGS       0xC0C7DB  /* 0 extra bytes */
#define ROM_ADDR_SET_NPC_DIR_FROM_FLAG             0xC0C353  /* 0 extra bytes */
#define ROM_ADDR_IS_ENTITY_COLLISION_ENABLED       0xC0A6B8  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENTITY_CAN_PURSUE           0xC0C48F  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENTITY_CAN_PURSUE_SHORT     0xC0C4AF  /* 0 extra bytes */
#define ROM_ADDR_CALCULATE_DIRECTION_TO_LEADER     0xC0C62B  /* 0 extra bytes */
#define ROM_ADDR_STORE_LEADER_POS_TO_ENTITY_VARS   0xC46B65  /* 0 extra bytes */
#define ROM_ADDR_CALCULATE_SLEEP_FRAMES            0xC0CA4E  /* 0 extra bytes */
#define ROM_ADDR_SET_ENTITY_SLEEP_FRAMES           0xC40023  /* 0 extra bytes */
#define ROM_ADDR_GET_NPC_DEFAULT_DIRECTION          0xC46914  /* 0 extra bytes */
#define ROM_ADDR_SET_ENTITY_DIRECTION_CR            0xC46957  /* 0 extra bytes (param in tempvar) */
#define ROM_ADDR_INIT_PARTY_POSITION_BUFFER         0xC03F1E  /* 0 extra bytes */
#define ROM_ADDR_CREATE_ENTITY_BG3                  0xC0A99F  /* 4 extra bytes (sprite_id + script_id) */
#define ROM_ADDR_CALCULATE_SLEEP_FROM_VARS          0xC0CC11  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENTITY_ENEMY_COLLISION       0xC0D15C  /* 0 extra bytes */
#define ROM_ADDR_GET_OVERWORLD_STATUS               0xC0C35D  /* 0 extra bytes */
#define ROM_ADDR_CHECK_PROSPECTIVE_ENTITY_COLLISION 0xC064A6  /* 0 extra bytes */
#define ROM_ADDR_CHECK_NPC_PATROL_BOUNDARY          0xC47269  /* 0 extra bytes */
#define ROM_ADDR_ENABLE_ALL_ENTITIES                0xC09451  /* 0 extra bytes */
#define ROM_ADDR_INSTANT_WIN_PP_RECOVERY           0xC2654C  /* 0 extra bytes */
#define ROM_ADDR_SETUP_ENTITY_COLOR_MATH           0xC474A8  /* 0 extra bytes */
#define ROM_ADDR_GET_ENTITY_DIRECTION               0xC0A673  /* 0 extra bytes */
#define ROM_ADDR_RAND_HIGH_BYTE                    0xC09FA8  /* 0 extra bytes */
#define ROM_ADDR_MOVEMENT_CMD_SET_DIR_VELOCITY     0xC0A697  /* 1 extra byte  */
#define ROM_ADDR_GET_ENTITY_OBSTACLE_FLAGS         0xC0A6C5  /* 0 extra bytes */
#define ROM_ADDR_GET_ENTITY_PATHFINDING_STATE      0xC0A6CB  /* 0 extra bytes */
#define ROM_ADDR_GET_DIRECTION_ROTATED_CLOCKWISE  0xC0C682  /* 0 extra bytes */
#define ROM_ADDR_GET_PAD_PRESS                    0xC468A9  /* 0 extra bytes */
#define ROM_ADDR_GET_PAD_STATE                    0xC468AF  /* 0 extra bytes */
#define ROM_ADDR_IS_BELOW_LEADER_Y                0xC46903  /* 0 extra bytes */
#define ROM_ADDR_GET_LEADER_DIRECTION_OFFSET      0xC46A6E  /* 0 extra bytes */
#define ROM_ADDR_GET_CARDINAL_DIRECTION           0xC46A9A  /* 0 extra bytes */
#define ROM_ADDR_GET_PARTY_COUNT                  0xC47333  /* 0 extra bytes */
#define ROM_ADDR_IS_PLAYER_BUSY                   0xEF0F60  /* 0 extra bytes */
#define ROM_ADDR_NULL_CALLROUTINE                 0xC4CC2C  /* 0 extra bytes */
#define ROM_ADDR_FORCE_RENDER_ENTITY_SPRITE      0xC0AAAC  /* 0 extra bytes */
#define ROM_ADDR_MOVE_TOWARD_TARGET_CB           0xC0A8C6  /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENTITY_COLLISION_AHEAD    0xC0D0D9  /* 0 extra bytes */
#define ROM_ADDR_SET_DEFAULT_TM                  0xC0EE47  /* 0 extra bytes */
#define ROM_ADDR_RESTORE_ENTITY_CALLBACK_FLAGS   0xC09F71  /* 0 extra bytes */
#define ROM_ADDR_SAVE_ENTITY_POSITION            0xC0D7B3  /* 0 extra bytes */
#define ROM_ADDR_RESTORE_ENTITY_POSITION         0xC0D7C7  /* 0 extra bytes */
#define ROM_ADDR_LOAD_INITIAL_MAP_DATA_FAR       0xC47369  /* 0 extra bytes */
#define ROM_ADDR_GET_RANDOM_NPC_DELAY            0xC467B4  /* 0 extra bytes */
#define ROM_ADDR_GET_RANDOM_SCREEN_DELAY         0xC467C2  /* 0 extra bytes */
#define ROM_ADDR_INIT_OVAL_WINDOW_FAR            0xC49841  /* 0 extra bytes */
#define ROM_ADDR_LOAD_TEXT_LAYER_TILEMAP         0xC4981F  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_OTHER_ENTITY_CALLBACKS  0xC09F43  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_OTHER_ENTITY_UPDATES    0xC0D77F  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_ALL_ENTITIES            0xC0943C  /* 0 extra bytes */
#define ROM_ADDR_IS_PSI_ANIMATION_ACTIVE         0xC2EACF  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_DIR_VELOCITY_REVERSED_CB    0xC0A8EF  /* 0 extra bytes */
#define ROM_ADDR_SCALE_DIRECTION_DISTANCE          0xC46B2D  /* 0 extra bytes */
#define ROM_ADDR_SETUP_PSI_TELEPORT_DEPARTURE      0xC48B2C  /* 0 extra bytes */
#define ROM_ADDR_STEER_ENTITY_TOWARD_DIRECTION     0xC0CEBE  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_CURRENT_ENTITY_COLLISION  0xC0A6D1  /* 0 extra bytes */
#define ROM_ADDR_STORE_ENTITY_POSITION_TO_VARS     0xC46C45  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_HDMA_CHANNEL4_ALT         0xC4257F  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_HDMA_CHANNEL4             0xC425F3  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_HDMA_CHANNEL5             0xC42624  /* 0 extra bytes */
#define ROM_ADDR_GET_DIRECTION_FROM_PLAYER_TO_ENTITY 0xC0C4F7 /* 0 extra bytes */
#define ROM_ADDR_CHECK_ENEMY_SHOULD_FLEE             0xC0C524 /* 0 extra bytes */
#define ROM_ADDR_CHOOSE_ENTITY_DIRECTION_TO_PLAYER   0xC0C615 /* 0 extra bytes */
#define ROM_ADDR_RESTORE_ENTITY_POSITION_FROM_VARS 0xC46C87  /* 0 extra bytes */
#define ROM_ADDR_SET_ENTITY_RANDOM_SCREEN_X        0xC46D23  /* 0 extra bytes */
#define ROM_ADDR_IS_BATTLE_PENDING                 0xC0D59B  /* 0 extra bytes */
#define ROM_ADDR_CLEAR_ENTITY_COLLISION2           0xC0A838  /* 0 extra bytes */
#define ROM_ADDR_RESTORE_MAP_PALETTE               0xC4978E  /* 0 extra bytes */
#define ROM_ADDR_SETUP_FULLSCREEN_WINDOW_CLIPPING  0xC4240A  /* 0 extra bytes */
#define ROM_ADDR_SETUP_DARK_WINDOW_EFFECT          0xC424D1  /* 0 extra bytes */
#define ROM_ADDR_SETUP_WHITE_WINDOW_EFFECT         0xC42509  /* 0 extra bytes */
#define ROM_ADDR_SETUP_DUAL_DARK_WINDOW_EFFECT     0xC4258C  /* 0 extra bytes */
#define ROM_ADDR_QUANTIZE_DIRECTION                0xC46B51  /* 0 extra bytes */
#define ROM_ADDR_ENTITY_COORDS_RELATIVE_TO_BG3     0xC0A06C  /* 0 extra bytes */
#define ROM_ADDR_REFLECT_ENTITY_Y_AT_TARGET        0xC47A6B  /* 0 extra bytes */
#define ROM_ADDR_IS_ENTITY_IN_RANGE_OF_LEADER      0xC46EF8  /* 0 extra bytes */
#define ROM_ADDR_IS_ENTITY_STILL_ON_CAST_SCREEN    0xC4ECE7  /* 0 extra bytes */
#define ROM_ADDR_RESTORE_OVERWORLD_STATE           0xEF0FF6  /* 0 extra bytes */
#define ROM_ADDR_ENABLE_TESSIE_LEAVES_ENTITIES     0xC467E6  /* 0 extra bytes */
#define ROM_ADDR_CHECK_SECTOR_USES_MINISPRITES     0xC2FF9A  /* 0 extra bytes */
#define ROM_ADDR_DISABLE_PARTY_MOVEMENT_AND_HIDE   0xC46712  /* 0 extra bytes */
#define ROM_ADDR_ENABLE_PARTY_MOVEMENT_AND_SHOW    0xC4675C  /* 0 extra bytes */
#define ROM_ADDR_RAND                              0xC08E9A  /* 0 extra bytes */
#define ROM_ADDR_QUEUE_NPC_TEXT_INTERACTION         0xC4681A  /* 0 extra bytes */
#define ROM_ADDR_INIT_OVAL_WINDOW                  0xC2EA15  /* 0 extra bytes (param in A/tempvar) */
#define ROM_ADDR_CLOSE_OVAL_WINDOW                 0xC2EA74  /* 0 extra bytes */
#define ROM_ADDR_CHECK_CAST_SCROLL_THRESHOLD       0xC4E4F9  /* 0 extra bytes */
#define ROM_ADDR_SET_CAST_SCROLL_THRESHOLD         0xC4E4DA  /* 0 extra bytes (param in A/tempvar) */
#define ROM_ADDR_HANDLE_CAST_SCROLLING             0xC4E51E  /* 0 extra bytes (tick callback) */
#define ROM_ADDR_UPLOAD_SPECIAL_CAST_PALETTE       0xC4EC6E  /* 0 extra bytes (param in A/tempvar) */
/* Delivery system callroutines (EF bank) */
#define ROM_ADDR_GET_DELIVERY_ATTEMPTS             0xEF0C87  /* 0 extra bytes */
#define ROM_ADDR_RESET_DELIVERY_ATTEMPTS           0xEF0C97  /* 0 extra bytes */
#define ROM_ADDR_CHECK_DELIVERY_ATTEMPT_LIMIT      0xEF0CA7  /* 0 extra bytes */
#define ROM_ADDR_GET_TIMED_DELIVERY_TIME           0xEF0D23  /* 0 extra bytes */
#define ROM_ADDR_SET_DELIVERY_TIMER                0xEF0D46  /* 0 extra bytes */
#define ROM_ADDR_DECREMENT_DELIVERY_TIMER          0xEF0D73  /* 0 extra bytes */
#define ROM_ADDR_QUEUE_DELIVERY_SUCCESS            0xEF0D8D  /* 0 extra bytes */
#define ROM_ADDR_QUEUE_DELIVERY_FAILURE            0xEF0DFA  /* 0 extra bytes */
#define ROM_ADDR_GET_TIMED_DELIVERY_ENTER_SPEED    0xEF0E67  /* 0 extra bytes */
#define ROM_ADDR_GET_TIMED_DELIVERY_EXIT_SPEED     0xEF0E8A  /* 0 extra bytes */
#define ROM_ADDR_INIT_DELIVERY_SEQUENCE            0xEF0FDB  /* 0 extra bytes */

/* Actionscript callroutines (C0 bank) */
#define ROM_ADDR_ACTIONSCRIPT_FADE_OUT_WITH_MOSAIC  0xC0AA07  /* 6 extra bytes */
#define ROM_ADDR_ACTIONSCRIPT_PREPARE_AT_TELEPORT   0xC0A907  /* 1 extra byte  */

/* Entity sprite rendering callroutines (C0 bank) */
#define ROM_ADDR_RENDER_ENTITY_SPRITE_ME3           0xC0A480  /* 0 extra bytes */

/* Pathfinding / delivery callroutines (C0 bank) */
#define ROM_ADDR_ADVANCE_ENTITY_PATH_POINT         0xC0D98F  /* 0 extra bytes */
#define ROM_ADDR_ADVANCE_ENTITY_TOWARD_LEADER      0xC0D0E6  /* 0 extra bytes */
#define ROM_ADDR_HANDLE_ENEMY_CONTACT              0xC0D5B0  /* 0 extra bytes */
#define ROM_ADDR_SETUP_DELIVERY_PATH_FROM_ENTITY   0xC0C251  /* 0 extra bytes */
#define ROM_ADDR_SETUP_DELIVERY_PATH_REVERSE       0xC0C19B  /* 0 extra bytes */

/* Photographer callroutines (C4 bank) */
#define ROM_ADDR_SET_PHOTOGRAPHER_POSITION         0xC46D4B  /* 0 extra bytes */

/* Misc callroutines (C4 bank) */
#define ROM_ADDR_CLEAR_SPRITEMAP_BUFFER            0xC4CEB0  /* 0 extra bytes */
#define ROM_ADDR_APPLY_ENTITY_PALETTE_BRIGHTNESS   0xC47499  /* 0 extra bytes */

/* Your Sanctuary display callroutines (C4 bank) */
#define ROM_ADDR_INITIALIZE_YOUR_SANCTUARY_DISPLAY  0xC4DE98  /* 0 extra bytes */
#define ROM_ADDR_ENABLE_YOUR_SANCTUARY_DISPLAY      0xC4DED0  /* 0 extra bytes */
#define ROM_ADDR_DISPLAY_YOUR_SANCTUARY_LOCATION    0xC4E2D7  /* 0 extra bytes (param in A/tempvar) */

/* Bubble Monkey callroutine (EF bank) */
#define ROM_ADDR_INIT_BUBBLE_MONKEY                 0xEF027D  /* 0 extra bytes */

/* Butterfly movement callroutines (C0 bank) */
#define ROM_ADDR_INIT_BUTTERFLY_MOVEMENT            0xC0CCCC  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_BUTTERFLY_MOVEMENT          0xC0CD50  /* 0 extra bytes */

/* Entity fade animation callroutines (C4 bank) */
#define ROM_ADDR_CLEAR_FADE_ENTITY_FLAGS           0xC4CB4F  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_FADE_ENTITY_SPRITES        0xC4CB8F  /* 0 extra bytes */
#define ROM_ADDR_HIDE_FADE_ENTITY_FRAMES           0xC4CBE3  /* 0 extra bytes */
#define ROM_ADDR_ANIMATE_ENTITY_TILE_COPY          0xC4CC2F  /* 0 extra bytes */
#define ROM_ADDR_ANIMATE_ENTITY_TILE_BLEND         0xC4CD44  /* 0 extra bytes */
#define ROM_ADDR_ANIMATE_ENTITY_TILE_MERGE         0xC4CED8  /* 0 extra bytes */

/* Animation sequence / visual callroutines (C4 bank) */
#define ROM_ADDR_ADVANCE_TILEMAP_ANIMATION_FRAME  0xC48A6D  /* 0 extra bytes */
#define ROM_ADDR_DISPLAY_ANIMATION_SEQUENCE_FRAME 0xC47B77  /* 0 extra bytes */
#define ROM_ADDR_LOAD_ANIMATION_SEQUENCE_FRAME    0xC47A9E  /* 0 extra bytes */
#define ROM_ADDR_LOAD_CHARACTER_PORTRAIT_SCREEN   0xC4880C  /* 0 extra bytes */
#define ROM_ADDR_UPDATE_SWIRL_EFFECT              0xC4A7B0  /* 0 extra bytes */

/* Movement callroutines (C0 bank) */
#define ROM_ADDR_MOVEMENT_LOAD_BATTLEBG           0xC0A977  /* 4 extra bytes */

/* Map / misc callroutines */
#define ROM_ADDR_RELOAD_MAP                       0xC018F3  /* 0 extra bytes */
#define ROM_ADDR_LOAD_MAP_ROW_AT_SCROLL_POS       0xC4734C  /* 0 extra bytes */
#define ROM_ADDR_DISPLAY_ANTI_PIRACY_SCREEN       0xC30100  /* 0 extra bytes */
#define ROM_ADDR_LOAD_DEBUG_CURSOR_GRAPHICS       0xEFE556  /* 0 extra bytes */

/* ROM addresses for callback mapping (within-bank addresses) */
#define ROM_ADDR_DRAW_TITLE_LETTER  0xA0FA  /* DRAW_TITLE_LETTER */
#define ROM_ADDR_POS_COPY_ABS       0xA0BB  /* COPY_ENTITY_ABS_TO_SCREEN */
#define ROM_ADDR_POS_NOP            0xA039  /* POSITION_CALLBACK_NOP */
#define ROM_ADDR_POS_SCREEN_BG3     0xA055  /* ENTITY_WORLD_TO_SCREEN (BG3) */
#define ROM_ADDR_POS_BG1_WITH_Z     0xA03A  /* ENTITY_SCREEN_COORDS_BG1_WITH_Z */
#define ROM_ADDR_POS_BG3_WITH_Z     0xA0A0  /* ENTITY_SCREEN_COORDS_BG3_WITH_Z */
#define ROM_ADDR_MOVE_APPLY_DELTA   0x9FC8  /* APPLY_ENTITY_DELTA_POSITION_ENTRY2 */
#define ROM_ADDR_MOVE_NOP           0x9FF0  /* APPLY_ENTITY_DELTA_POSITION_ENTRY4 (bare RTS, no-op) */
#define ROM_ADDR_MOVE_FORCE_MOVE    0xA37A  /* ENTITY_PHYSICS_FORCE_MOVE */
#define ROM_ADDR_MOVE_WITH_COLLISION 0xA360 /* ENTITY_PHYSICS_WITH_COLLISION */
#define ROM_ADDR_MOVE_SIMPLE_COLLISION 0xA384 /* ENTITY_PHYSICS_SIMPLE_COLLISION */
#define ROM_ADDR_MOVE_PARTY_SPRITE  0xA26B  /* UPDATE_PARTY_SPRITE_POSITION */
#define ROM_ADDR_MOVE_DELTA_3D      0x9FF1  /* APPLY_ENTITY_DELTA_POSITION_3D */
#define ROM_ADDR_MOVE_Z_UPDATE      0xA00C  /* UPDATE_ENTITY_Z_POSITION */

/* Spritemap animation frame count (title screen) */
#define ANIMATION_FRAME_COUNT 9

/* ---- Script Bank System ---- */

/* Maximum number of loaded script bank regions */
#define MAX_SCRIPT_BANKS 5

/*
 * A loaded script bank region — a contiguous chunk of bytecode from ROM.
 * Within-bank addresses in the bytecode are translated to buffer offsets
 * by subtracting rom_base_addr.
 */
typedef struct {
    const uint8_t *data;       /* loaded bytecode buffer */
    uint32_t size;             /* buffer size in bytes */
    uint16_t rom_base_addr;    /* within-bank base address (e.g. 0x2172) */
    uint8_t rom_bank;          /* SNES bank number (e.g. 0xC4) */
} ScriptBankInfo;

extern ScriptBankInfo script_banks[MAX_SCRIPT_BANKS];
extern int script_bank_count;

/* ---- Runtime-loaded data (set by load_title_screen_script_data) ---- */

/* Script bytecode bank — loaded from title_screen_scripts.bin */
extern const uint8_t *title_script_bank;
extern uint16_t title_script_bank_size;

/* Base address of the script bank in ROM (e.g. 0x2172 within bank $C4) */
extern uint16_t title_script_bank_base;

/* Script pointer table — local offsets into title_script_bank */
extern uint16_t title_script_pointers[TITLE_SCREEN_SCRIPT_COUNT];

/* Spritemap data — loaded from title_screen_spritemaps.bin */
extern const uint8_t *title_spritemap_data;
extern uint16_t title_spritemap_offsets[ANIMATION_FRAME_COUNT];

/* General event script pointer table (895 × 3 bytes, loaded from ROM) */
extern const uint8_t *event_script_pointer_table;
extern uint16_t event_script_pointer_count;

/* ---- Naming Screen Entity Data ---- */

/* Number of entries in NAMING_SCREEN_ENTITIES pointer table.
 * 7 characters × 2 variants (initial animation + return animation) = 14 */
#define NAMING_SCREEN_ENTITY_COUNT 14

/* ROM within-bank base address of the naming screen entity data block.
 * The entity lists start at $C3FC49, followed by the pointer table at $C3FD2D.
 * The binary extraction begins at the entity lists. */
#define NAMING_ENTITIES_ROM_BASE       0xFC49
#define NAMING_ENTITIES_PTR_TABLE_OFF  0x00E4  /* 0xFD2D - 0xFC49 = 228 */

/* Naming screen entity data (loaded from naming_screen_entities.bin) */
extern const uint8_t *naming_entities_data;
extern uint16_t naming_entities_data_size;

/* Load all title screen script data from binary assets. */
void load_title_screen_script_data(void);

/* Free title screen script data */
void free_title_screen_script_data(void);

/* Load general event script data (pointer table + naming screen banks).
 * Call after load_title_screen_script_data for title, or independently. */
void load_event_script_data(void);

/* Free general event script data */
void free_event_script_data(void);

/* Register a script bank region. Returns bank index, or -1 on failure. */
int register_script_bank(const uint8_t *data, uint32_t size,
                         uint16_t rom_base_addr, uint8_t rom_bank);

/* Look up a script ID in EVENT_SCRIPT_POINTERS and return:
 *   - *out_bank_idx: index into script_banks[]
 *   - *out_offset: byte offset within that bank's data buffer
 * Returns true on success, false if script ID is out of range or bank not loaded. */
bool resolve_script_id(uint16_t script_id, int *out_bank_idx, uint16_t *out_offset);

/* Read a byte from a loaded script bank at the given offset */
uint8_t script_bank_read_byte(int bank_idx, uint16_t offset);

/* Read a little-endian 16-bit word from a loaded script bank */
uint16_t script_bank_read_word(int bank_idx, uint16_t offset);

/* Read a little-endian 24-bit address from a loaded script bank */
uint32_t script_bank_read_addr(int bank_idx, uint16_t offset);

/* Translate a within-bank ROM address to a buffer offset for the given bank */
uint16_t script_bank_rom_to_offset(int bank_idx, uint16_t rom_addr);

#endif /* DATA_EVENT_SCRIPT_DATA_H */
