#ifndef GAME_OVERWORLD_H
#define GAME_OVERWORLD_H

#include "core/types.h"
#include "include/binary.h"

/* Predefined hotspot coordinates entry (8 bytes, packed to match ROM binary layout).
 * 56 entries in the table. Coordinates are in tile units (multiply by 8 for pixels). */
#define HOTSPOT_COORD_COUNT 56

PACKED_STRUCT
typedef struct {
    uint16_t x1;  /* 0: left boundary (tile coords, LE) */
    uint16_t y1;  /* 2: top boundary (LE) */
    uint16_t x2;  /* 4: right boundary (LE) */
    uint16_t y2;  /* 6: bottom boundary (LE) */
} HotspotCoords;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(HotspotCoords, 8);

/* Entity tick/move disable flags (from include/enums.asm) */
#define OBJECT_TICK_DISABLED  0x8000
#define OBJECT_MOVE_DISABLED  0x4000

/* Collision disabled sentinel.
 * Assembly uses 0x8000; narrowed to int8_t -128 after field was shrunk to 8-bit. */
#define ENTITY_COLLISION_DISABLED ((int8_t)-128)

/* Active hotspot — runtime state for map hotspot boundaries.
 * Port of active_hotspot struct (include/structs.asm).
 * 2 hotspot slots, 14 bytes each. */
typedef struct {
    uint16_t mode;    /* 0=inactive, 1=inside check, 2=outside check */
    uint16_t x1;      /* left boundary (world pixel coords) */
    uint16_t y1;      /* top boundary */
    uint16_t x2;      /* right boundary */
    uint16_t y2;      /* bottom boundary */
    uint32_t pointer; /* ROM pointer to hotspot text/event data */
} ActiveHotspot;

#define NUM_ACTIVE_HOTSPOTS 2

/* Interaction queue (from ram.asm, structs.asm).
 * Circular queue of 4 entries for pending NPC/door/text interactions. */
#define INTERACTION_QUEUE_SIZE 4

typedef struct {
    uint16_t type;       /* Interaction type (0=text, 2=door, 8=NPC, 9=text, 10=dad phone) */
    uint32_t data_ptr;   /* ROM pointer to text or door data */
} QueuedInteraction;

/* Entity creation queue (from ram.asm:975-977, structs.asm).
 * Queued sprite+script pairs are flushed by FLUSH_ENTITY_CREATION_QUEUE,
 * which calls CREATE_PREPARED_ENTITY_SPRITE for each entry. */
#define ENTITY_CREATION_QUEUE_CAPACITY 12

typedef struct {
    uint16_t sprite;  /* Sprite ID */
    uint16_t script;  /* Script ID */
} QueuedEntityCreation;

/* Delivery system constants */
#define DELIVERY_TABLE_COUNT      10

/* Delivery table entry (20 bytes, packed to match ROM binary layout).
 * 10 entries in the table. Text pointers are split into addr (16-bit) + bank (8-bit). */
PACKED_STRUCT
typedef struct {
    uint16_t sprite_id;         /*  0: overworld sprite (LE) */
    uint16_t event_flag;        /*  2: event flag to check (LE) */
    uint16_t attempt_limit;     /*  4: max delivery attempts (0x00FF = unlimited) (LE) */
    uint16_t delivery_time;     /*  6: time value (LE) */
    uint16_t timer_value;       /*  8: initial timer frames (LE) */
    uint16_t success_addr;      /* 10: success text ptr low word (LE) */
    uint8_t  success_bank;      /* 12: success text ptr bank */
    uint16_t failure_addr;      /* 13: failure text ptr low word (LE) */
    uint8_t  failure_bank;      /* 15: failure text ptr bank */
    uint16_t enter_speed;       /* 16: entity enter speed (LE) */
    uint16_t exit_speed;        /* 18: entity exit speed (LE) */
} DeliveryEntry;
END_PACKED_STRUCT
ASSERT_STRUCT_SIZE(DeliveryEntry, 20);

/* ---- OverworldState: all overworld module globals ---- */
typedef struct {
    /* Entity spawn control */
    uint8_t npc_spawns_enabled;
    uint8_t enemy_spawns_enabled;

    /* Player movement state flags (from ram.asm:861).
     * Bit 1 (0x0002): player is in a special movement state (e.g., cutscene),
     * prevents enemy collision detection. */
    uint8_t player_movement_flags;
    uint16_t player_intangibility_frames;
    uint8_t player_has_moved_since_map_load;
    uint8_t player_has_done_something_this_frame;

    /* Camera focus entity offset (-1 = none).
     * Set by SET_CAMERA_FOCUS_BY_SPRITE_ID. */
    int16_t camera_focus_entity;

    /* When non-zero, GET_ON_BICYCLE skips playing bicycle music.
     * Set by INIT_INTRO for the entire intro/title/attract sequence. */
    uint8_t disable_music_changes;
    uint8_t enable_auto_sector_music_changes;

    /* Footstep sound globals (from bankconfig/common/ram.asm:540-542). */
    uint16_t footstep_sound_id;
    uint16_t footstep_sound_id_override;
    int16_t footstep_sound_ignore_entity;

    /* Entity spawn preparation globals (from bankconfig/common/ram.asm:1235-1239).
     * Set by PREPARE_NEW_ENTITY_AT_* functions, read by CREATE_PREPARED_ENTITY_SPRITE. */
    int16_t entity_prepared_x;
    int16_t entity_prepared_y;
    int16_t entity_prepared_direction;

    /* AUTO_MOVEMENT_DIRECTION — direction used during camera mode 1 auto-movement.
     * When walking_style == STAIRS, this overrides leader_direction. */
    uint16_t auto_movement_direction;

    /* Movement state (from ram.asm) */
    uint16_t stairs_direction;         /* 0x0100 = up/right, 0x0200 = down/left */
    uint16_t escalator_entrance_direction;
    int16_t final_movement_direction;
    uint8_t not_moving_in_same_direction_faced;

    /* Collision working variables (from ram.asm:907-927) */
    int16_t entity_movement_prospective_x;
    int16_t entity_movement_prospective_y;
    uint16_t checked_collision_left_x;
    uint16_t checked_collision_top_y;
    int16_t ladder_stairs_tile_x;
    int16_t ladder_stairs_tile_y;

    /* Overworld status suppression (from ram.asm:897) */
    uint8_t overworld_status_suppression;

    /* Demo/attract mode */
    uint16_t demo_frames_left;
    uint16_t demo_recording_flags;

    /* Mushroomization state (from ram.asm:402-404) */
    uint8_t mushroomized_walking_flag;
    uint16_t mushroomization_timer;
    uint16_t mushroomization_modifier;

    /* Debug */
    uint8_t debug_flag;
    uint8_t debug_mode_number;

    /* Respawn position (from ram.asm:1214-1217) */
    uint16_t respawn_x;
    uint16_t respawn_y;

    /* Teleport destination override (from ram.asm:733-735) */
    uint16_t current_teleport_destination_x;
    uint16_t current_teleport_destination_y;

    /* Map loading cache (set to -1 to invalidate) */
    int16_t loaded_map_tile_combo;
    int16_t loaded_map_palette;
    uint8_t show_npc_flag;

    /* Hotspots */
    ActiveHotspot active_hotspots[NUM_ACTIVE_HOTSPOTS];

    /* Per-frame state tracking (from ram.asm:263, 867-885, 1323-1327) */
    uint16_t last_sector_x;
    uint16_t last_sector_y;
    uint16_t current_leader_direction;
    uint16_t current_leading_party_member_entity;
    int16_t mini_ghost_entity_id;
    uint16_t mini_ghost_angle;
    uint16_t possessed_player_count;
    uint8_t hp_alert_shown[6];
    uint8_t overworld_damage_countdown_frames[6];
    int16_t moving_party_member_entity_id;

    /* Battle / encounter state (from ram.asm) */
    uint16_t battle_mode;
    uint16_t input_disable_frame_counter;
    uint16_t overworld_enemy_count;
    uint16_t overworld_enemy_maximum;
    uint8_t magic_butterfly_spawned;
    uint16_t enemy_spawn_range_width;
    uint16_t enemy_spawn_range_height;
    uint16_t enemy_spawn_counter;
    uint16_t enemy_spawn_too_many_failures;
    uint16_t enemy_spawn_encounter_id;
    uint16_t enemy_spawn_remaining_count;
    uint16_t enemy_spawn_chance;
    uint16_t spawning_enemy_group;
    uint16_t spawning_enemy_sprite;
    uint16_t spawning_enemy_name;
    uint16_t battle_swirl_countdown;
    uint8_t enemy_has_been_touched;

    /* Interaction queue */
    uint16_t pending_interactions;
    QueuedInteraction queued_interactions[INTERACTION_QUEUE_SIZE];
    uint16_t current_queued_interaction;      /* Read index (0-3) */
    uint16_t next_queued_interaction;          /* Write index (0-3) */
    uint16_t current_queued_interaction_type;  /* Type being processed, 0xFFFF=none */

    /* NPC interaction state (from ram.asm:875-878) */
    uint16_t interacting_npc_id;
    uint32_t map_object_text;
    uint16_t interacting_npc_entity;

    /* Dad phone */
    uint16_t dad_phone_timer;
    uint8_t dad_phone_queued;

    /* Misc flags */
    uint8_t pajama_flag;
    uint8_t disabled_transitions;
    uint8_t render_hppp_windows;
    uint8_t redraw_all_windows;
    uint8_t currently_drawn_hppp_windows;

    /* PSI Teleport state */
    uint16_t psi_teleport_speed_int;
    uint16_t psi_teleport_style;
    uint16_t psi_teleport_destination;
    uint16_t psi_teleport_state;
    uint16_t psi_teleport_beta_angle;
    uint16_t psi_teleport_beta_progress;
    uint16_t psi_teleport_better_progress;
    uint16_t psi_teleport_beta_x_adjustment;
    uint16_t psi_teleport_beta_y_adjustment;

    /* PSI Teleport 32-bit fixed-point speed variables (16.16 format) */
    int32_t psi_teleport_speed;
    int32_t psi_teleport_speed_x;
    int32_t psi_teleport_speed_y;
    int32_t psi_teleport_next_x;
    int32_t psi_teleport_next_y;

    /* PSI Teleport success screen animation */
    int16_t psi_teleport_success_screen_speed_x;
    int16_t psi_teleport_success_screen_x;
    int16_t psi_teleport_success_screen_speed_y;
    int16_t psi_teleport_success_screen_y;

    /* Entity fade */
    int16_t entity_fade_entity;

    /* Entity creation queue */
    QueuedEntityCreation entity_creation_queue[ENTITY_CREATION_QUEUE_CAPACITY];
    uint16_t entity_creation_queue_length;

    /* Delivery system state */
    int16_t delivery_timers[DELIVERY_TABLE_COUNT];
    int16_t delivery_attempts[DELIVERY_TABLE_COUNT];

    /* Photographer */
    uint16_t spawning_travelling_photographer_id;

    /* Bubble Monkey follower state (from ram.asm:1277-1288).
     * Used by UPDATE_PARTY_FOLLOWER_MOVEMENT tick callback. */
    uint16_t bubble_monkey_mode;
    uint16_t bubble_monkey_movement_change_timer;
    uint16_t bubble_monkey_distracted_next_direction;
    uint16_t bubble_monkey_distracted_next_direction_change_time;
    uint16_t bubble_monkey_distracted_direction_changes_left;

    /* POST_TELEPORT_CALLBACK (ram.asm $7E9D1B) — deferred callback called
     * after the next teleport completes.  Set by flyover/sanctuary scripts
     * to UNDRAW_FLYOVER_TEXT; TELEPORT calls it then clears it. */
    void (*post_teleport_callback)(void);
} OverworldState;

extern OverworldState ow;

/* ---- Function declarations ---- */

/* Initialize overworld PPU settings and clear VRAM tile 0.
 * Port of OVERWORLD_INITIALIZE (asm/overworld/initialize.asm). */
void overworld_initialize(void);

/* Set BG mode, tilemap, and tile data addresses for overworld.
 * Port of OVERWORLD_SETUP_VRAM (asm/overworld/setup_vram.asm). */
void overworld_setup_vram(void);

/* Place the party leader entity at world coordinates.
 * Port of PLACE_LEADER_AT_POSITION (asm/overworld/place_leader_at_position.asm). */
void place_leader_at_position(uint16_t x, uint16_t y);

/* Clear movement speeds, collision objects, NPC IDs for all entities.
 * Port of INITIALIZE_MISC_OBJECT_DATA (asm/overworld/initialize_misc_object_data.asm). */
void initialize_misc_object_data(void);

/* Reset party state variables.
 * Simplified port of RESET_PARTY_STATE (asm/overworld/reset_party_state.asm). */
void reset_party_state(void);

/* Initialize party members from game_state.party_members[].
 * Simplified port of INITIALIZE_PARTY (asm/overworld/initialize_party.asm). */
void initialize_party(void);

/* Remove all entities except the leader.
 * Port of CLEAR_MAP_ENTITIES (asm/overworld/clear_map_entities.asm). */
void clear_map_entities(void);

/* Render one frame: OAM clear, run actionscript, sync palettes, present.
 * Port of RENDER_FRAME_TICK (asm/system/render_frame_tick.asm).
 *
 * NOTE: This does NOT update windows or HP/PP meters. Most battle loops
 * should call window_tick() instead, which re-renders windows into
 * bg2_buffer, updates HP/PP rolling, uploads to VRAM, then calls this.
 * Only use render_frame_tick() directly when the assembly also calls
 * RENDER_FRAME_TICK (or WAIT_UNTIL_NEXT_FRAME) rather than WINDOW_TICK. */
void render_frame_tick(void);

/* Allocate/clear sprite VRAM table.
 * Port of ALLOC_SPRITE_MEM (asm/system/alloc_sprite_mem.asm).
 * id=0x8000 clears the entire table. */
void alloc_sprite_mem(uint16_t id, uint16_t param);

/* Remove an entity: free spritemaps, free VRAM, clear IDs, deactivate.
 * Port of REMOVE_ENTITY (asm/overworld/remove_entity.asm).
 * slot: entity slot number (0-29), NOT offset. */
void remove_entity(int16_t slot);

/* CLEAR_ALL_ENEMIES: Port of asm/overworld/clear_all_enemies.asm.
 * Zeroes enemy spawn counters, removes all non-party entities, clears collision data. */
void clear_all_enemies(void);

/* ADD_PARTY_MEMBER: Port of asm/overworld/add_party_member.asm.
 * Adds a character to the party with insertion-sort ordering, creates entity,
 * sets up position from predecessor's position buffer.
 * char_id: 1-indexed character ID. Returns entity slot. */
uint16_t add_party_member(uint16_t char_id);

/* REMOVE_PARTY_MEMBER: Port of asm/overworld/party/remove_party_member.asm.
 * Removes a party member from party_order/party_entity_slots arrays,
 * saves the removed entity's position as entity_prepared, removes the entity,
 * and calls update_npc_party_lineup + update_party to re-sort.
 * char_id: 1-indexed character ID. */
void remove_party_member(uint16_t char_id);

/* UPDATE_NPC_PARTY_LINEUP: Port of asm/overworld/party/update_npc_party_lineup.asm.
 * Syncs game_state NPC tracking fields with the sorted party_members array. */
void update_npc_party_lineup(void);

/* UPDATE_PARTY: Port of asm/overworld/update_party.asm.
 * Sorts party ordering based on member status (alive first, incapacitated middle,
 * NPCs last). Updates party_order, party_entity_slots, player_controlled_party_members. */
void update_party(void);

/* Mount the bicycle: swap leader sprite to NESS_BICYCLE, set walking_style.
 * Port of GET_ON_BICYCLE (asm/overworld/get_on_bicycle.asm).
 * Only works when party_count == 1 and single-character mode. */
void get_on_bicycle(void);

/* Dismount the bicycle: swap back to normal NESS sprite, clear walking_style.
 * Port of DISMOUNT_BICYCLE (asm/overworld/dismount_bicycle.asm).
 * No-op if not currently on bicycle. */
void dismount_bicycle(void);

/* SUM_ALIVE_PARTY_LEVELS: Port of asm/overworld/party/sum_alive_party_levels.asm.
 * Sums levels of all PC party members (char_id 1-4), excluding NPCs. */
uint16_t sum_alive_party_levels(void);

/* CHECK_ENEMY_SHOULD_FLEE: Port of asm/overworld/check_enemy_should_flee.asm.
 * Checks run-away flags and party level vs enemy level thresholds.
 * Returns 1 if enemy should flee, 0 otherwise. */
uint16_t check_enemy_should_flee(void);

/* PREPARE_NEW_ENTITY: Port of asm/overworld/prepare_new_entity.asm.
 * Sets entity_prepared_x/y/direction staging globals.
 * Assembly: A=direction, X=x, Y=y. */
void prepare_new_entity(uint16_t x, uint16_t y, uint8_t direction);

/* PREPARE_NEW_ENTITY_AT_EXISTING_ENTITY_LOCATION:
 * Port of asm/overworld/prepare_new_entity_at_existing_entity_location.asm.
 * source_selector: 0 = current_entity_slot, 1 = party leader, else = slot. */
void prepare_new_entity_at_existing_entity_location(int16_t source_selector);

/* PREPARE_NEW_ENTITY_AT_TELEPORT_DESTINATION:
 * Port of asm/overworld/prepare_new_entity_at_teleport_destination.asm.
 * dest_id: teleport destination index (8-bit). */
void prepare_new_entity_at_teleport_destination(uint16_t dest_id);

/* Calculate where an entity will be next frame by adding delta to abs.
 * Port of CALCULATE_PROSPECTIVE_POSITION (asm/overworld/calculate_prospective_position.asm).
 * entity_slot: entity slot number (NOT offset).
 * Returns count of changed axes (0=none, 1=one, 2=both). Assembly uses INY. */
int calculate_prospective_position(int16_t entity_slot);

/* Check if the current entity has collided with an enemy.
 * Port of CHECK_ENTITY_ENEMY_COLLISION (asm/overworld/collision/check_entity_enemy_collision.asm).
 * Returns -1 if collision detected, 0 otherwise. */
int16_t check_entity_enemy_collision(void);

/* Check obstacle flags for an entity at its prospective position.
 * Port of CHECK_CURRENT_ENTITY_OBSTACLES (asm/overworld/collision/check_current_entity_obstacles.asm).
 * Returns obstacle flags (8-bit), 0 = clear. */
int16_t check_current_entity_obstacles(void);

/* Check collision at the entity's prospective position with other entities.
 * Port of CHECK_PROSPECTIVE_ENTITY_COLLISION (asm/overworld/collision/check_prospective_entity_collision.asm).
 * Uses CURRENT_ENTITY_SLOT. */
void check_prospective_entity_collision(void);

/* Port of CHECK_PROSPECTIVE_NPC_COLLISION.
 * Checks entity's next-frame position against party and NPC entities. */
void check_prospective_npc_collision(void);

/* Port of CHECK_ENEMY_MOVEMENT_OBSTACLES.
 * Tile collision + enemy run restrictions for enemy collision loop. */
int16_t check_enemy_movement_obstacles(void);

/* Port of CHECK_NPC_PLAYER_OBSTACLES.
 * Player-style tile collision + enemy run restrictions for NPC collision loop. */
int16_t check_npc_player_obstacles(void);

/* Check NPC/entity collision with the leader at a prospective position.
 * Port of NPC_COLLISION_CHECK (asm/overworld/npc_collision_check.asm).
 * x, y: prospective leader position (world coords).
 * leader_slot: leader entity slot number.
 * Stores collided entity slot in entities.collided_objects[23] (init entity).
 * Returns the collided slot (0-22) or 0xFFFF (no collision). */
int16_t npc_collision_check(int16_t x, int16_t y, int16_t leader_slot);

/* Clear sprite hide flags (bit 15 of spritemap_ptr_hi) for party entity
 * slots 24-29. Called each frame while PLAYER_INTANGIBILITY_FRAMES > 0.
 * Port of CLEAR_PARTY_SPRITE_HIDE_FLAGS (asm/overworld/party/clear_party_sprite_hide_flags.asm). */
void clear_party_sprite_hide_flags(void);

/* Show/hide entity sprites by clearing/setting DRAW_DISABLED (bit 15).
 * char_id: specific character ID, or 0xFF/-1 for all party members.
 * Port of SHOW_ENTITY_SPRITES (C4645A) and HIDE_ENTITY_SPRITES (C463F4). */
void show_entity_sprites(uint16_t char_id);
void hide_entity_sprites(uint16_t char_id);

/* Disable tick and movement for a character entity (or all party + init).
 * char_id: character ID, or 0xFFFF for all party members + init entity.
 * Port of DISABLE_CHARACTER_MOVEMENT (C46594). */
void disable_character_movement(uint16_t char_id);

/* Enable tick and movement for a character entity (or all party + init).
 * char_id: character ID, or 0xFFFF for all party members + init entity.
 * Port of ENABLE_CHARACTER_MOVEMENT (C46631). */
void enable_character_movement(uint16_t char_id);

/* Deactivate an NPC entity: save position, reassign to deallocation script.
 * Port of DEACTIVATE_NPC_ENTITY (C460CE). */
void deactivate_npc_entity(uint16_t npc_id, uint16_t fade_param);

/* Deactivate a sprite entity: save position, reassign to deallocation script.
 * Port of DEACTIVATE_SPRITE_ENTITY (C46125). */
void deactivate_sprite_entity(uint16_t sprite_id, uint16_t fade_param);

/* Disable tick and movement for all active entities (walk linked list).
 * Port of DISABLE_ALL_ENTITIES (C0943C). */
void disable_all_entities(void);

/* Enable tick and movement for all active entities (walk linked list).
 * Port of ENABLE_ALL_ENTITIES (C09451). */
void enable_all_entities(void);

/* Set an entity's direction by sprite ID and re-render if changed.
 * Port of SET_ENTITY_DIRECTION_BY_SPRITE_ID (C46331). */
void set_entity_direction_by_sprite_id(uint16_t sprite_id, int16_t direction);

/* Disable tick and movement for an NPC entity by NPC ID.
 * Port of DISABLE_NPC_MOVEMENT (C4655E). */
void disable_npc_movement(uint16_t npc_id);

/* Enable tick and movement for an NPC entity by NPC ID.
 * Port of ENABLE_NPC_MOVEMENT (C465FB). */
void enable_npc_movement(uint16_t npc_id);

/* Disable tick and movement for a sprite entity by sprite ID.
 * Port of DISABLE_ENTITY_BY_SPRITE_ID (C46579). */
void disable_sprite_movement(uint16_t sprite_id);

/* Enable tick and movement for a sprite entity by sprite ID.
 * Port of ENABLE_SPRITE_MOVEMENT (C46616). */
void enable_sprite_movement(uint16_t sprite_id);

/* Create an NPC entity at entity_prepared_x/y/direction.
 * Port of CREATE_PREPARED_ENTITY_NPC (asm/overworld/create_prepared_entity_npc.asm).
 * Looks up sprite from NPC_CONFIG_TABLE, creates entity, sets direction and NPC ID.
 * Returns entity slot number, or negative on failure. */
int16_t create_prepared_entity_npc(uint16_t npc_id, uint16_t action_script);

/* Reload active hotspot boundaries from saved game_state data.
 * Port of RELOAD_HOTSPOTS (asm/overworld/reload_hotspots.asm).
 * Called when continuing a saved game from file select. */
void reload_hotspots(void);

/* Activate a hotspot boundary for player position tracking.
 * Port of ACTIVATE_HOTSPOT (asm/overworld/activate_hotspot.asm).
 * slot: 1 or 2 (which hotspot slot to use).
 * hotspot_id: index into MAP_HOTSPOTS predefined coordinates table.
 * pointer: 32-bit ROM pointer to hotspot event/text data. */
void activate_hotspot(uint16_t slot, uint16_t hotspot_id, uint32_t pointer);

/* Check if player has exited a hotspot boundary.
 * Port of CHECK_HOTSPOT_EXIT (asm/overworld/check_hotspot_exit.asm).
 * hotspot_idx: 0 or 1 (alternates each frame in caller). */
void check_hotspot_exit(uint16_t hotspot_idx);

/* Per-frame overworld update tick callback for init entity (slot 23).
 * Port of UPDATE_OVERWORLD_FRAME (asm/overworld/update_overworld_frame.asm).
 * Called as a tick callback — handles animated tiles, palette animation,
 * then calls update_leader_movement(). */
void update_overworld_frame(int16_t entity_offset);

/* Reset the interaction queue to empty.
 * Port of RESET_QUEUED_INTERACTIONS (asm/overworld/reset_queued_interactions.asm). */
void reset_queued_interactions(void);

/* Enqueue an interaction. Skips if type matches current_queued_interaction_type.
 * Port of QUEUE_INTERACTION (asm/overworld/queue_interaction.asm). */
void queue_interaction(uint16_t type, uint32_t data_ptr);

/* Disable all character movement and queue a type-8 text interaction.
 * Port of FREEZE_AND_QUEUE_TEXT_INTERACTION (asm/text/freeze_and_queue_text_interaction.asm). */
void freeze_and_queue_text_interaction(uint32_t text_ptr);

/* Queue a sprite+script pair for deferred entity creation.
 * Port of QUEUE_ENTITY_CREATION (asm/overworld/entity/queue_entity_creation.asm). */
void queue_entity_creation(uint16_t sprite_id, uint16_t script_id);

/* Flush the entity creation queue, creating all queued entities.
 * Port of FLUSH_ENTITY_CREATION_QUEUE (asm/overworld/entity/flush_entity_creation_queue.asm). */
void flush_entity_creation_queue(void);

/* Port of SPAWN_BUZZ_BUZZ (asm/overworld/spawn_buzz_buzz.asm).
 * Displays MSG_EVT_BUNBUNBUN text (which may spawn entities via CC codes
 * if the appropriate event flags are set), then spawns delivery entities. */
void spawn_buzz_buzz(void);

/* Spawn delivery entities for pending item deliveries.
 * Port of SPAWN_DELIVERY_ENTITIES (asm/overworld/spawn_delivery_entities.asm). */
void spawn_delivery_entities(void);

/* Returns pointer to the loaded timed_delivery_table binary data.
 * Ensures the table is loaded first. Returns NULL on failure. */
const DeliveryEntry *get_delivery_table(void);

/* Create a delivery placeholder entity for a timed delivery event.
 * Port of GET_DELIVERY_SPRITE_AND_PLACEHOLDER (asm/overworld/get_delivery_sprite_and_placeholder.asm).
 * delivery_id is 1-based. */
void get_delivery_sprite_and_placeholder(uint16_t delivery_id);

/* SAVE_PHOTO_STATE: Port of asm/misc/save_photo_state.asm.
 * Saves play time (frames/3600, capped at 59999) and party affliction status
 * (unconscious=bit5, diamondized=bit6, mushroomized=bit7) into
 * game_state.saved_photo_states[photo_id-1]. */
void save_photo_state(uint16_t photo_id);

/* ENCOUNTER_TRAVELLING_PHOTOGRAPHER: Port of asm/misc/encounter_travelling_photographer.asm.
 * Clears party hide flags, clears intangibility, sets photographer ID,
 * displays the photographer text, and saves photo state. */
void encounter_travelling_photographer(uint16_t photo_id);

/* Initialize the overworld after file select.
 * Port of INITIALIZE_OVERWORLD_STATE (asm/overworld/initialize_overworld_state.asm).
 * Creates the init entity (slot 23) with EVENT_001 (main overworld tick), sets up party,
 * loads the map, and prepares for gameplay. */
void initialize_overworld_state(void);

/* Snap all party member entities to the leader's position.
 * Port of SNAP_PARTY_TO_LEADER (asm/overworld/party/snap_party_to_leader.asm).
 * Called after map load to prevent followers from being off-screen. */
void snap_party_to_leader(void);

/* Refresh party entity positions, graphics, and screen coords.
 * Port of REFRESH_PARTY_ENTITIES (asm/overworld/party/refresh_party_entities.asm).
 * Called after init_party_position_buffer, after battle, and from events. */
void refresh_party_entities(void);

/* Set leader position, reload party entities, init position buffer.
 * Port of SET_LEADER_POSITION_AND_LOAD_PARTY (asm/overworld/party/set_leader_position_and_load_party.asm).
 * Called during teleport (CC_1F_69), map transitions, etc. */
void set_leader_position_and_load_party(uint16_t x, uint16_t y, uint16_t direction);

/* Rebuild party entities for the current map sector.
 * Port of LOAD_PARTY_AT_MAP_POSITION (asm/overworld/party/load_party_at_map_position.asm).
 * Removes old party entities, determines character sprites for the current
 * sector (normal/small/robot mode), creates new entities at leader coords,
 * snaps to leader, and checks collision for doors. */
void load_party_at_map_position(uint16_t direction);

/* Determine which overworld sprite to show for a party member.
 * Port of GET_PARTY_MEMBER_SPRITE_ID (asm/overworld/party/get_party_member_sprite_id.asm).
 * Returns OVERWORLD_SPRITE ID, or -1 (0xFFFF) to hide the entity.
 * Side effect: sets entity var3 (animation speed) based on terrain/status. */
int16_t get_party_member_sprite_id(int16_t char_id, uint16_t walking_style,
                                    int16_t entity_offset, int16_t party_idx);

/* CALCULATE_VELOCITY_COMPONENTS — Port of asm/misc/calculate_velocity_components.asm.
 * Decomposes a speed/angle pair into X/Y displacement components.
 * angle: 16-bit angle (high byte used, 0=north), speed: magnitude.
 * Returns dx and dy in screen coordinates (+Y = down). */
void calculate_velocity_components(uint16_t angle, int16_t speed,
                                   int16_t *out_x, int16_t *out_y);

/* Clear all 128 OAM entries off-screen.
 * Port of OAM_CLEAR (asm/system/oam.asm). */
void oam_clear(void);

/* Convert all active entity absolute positions to screen coordinates.
 * Port of UPDATE_ENTITY_SCREEN_POSITIONS (asm/overworld/entity/update_entity_screen_positions.asm).
 * screen_x = abs_x - BG1_HOFS, screen_y = abs_y - BG1_VOFS. */
void update_entity_screen_positions(void);

/* Render entities and sync palettes.
 * Port of UPDATE_SCREEN (asm/overworld/clear_oam_and_update_screen.asm).
 * In assembly this also handles OAM high table and scroll triple-buffering,
 * which are not needed in the C port. */
void update_screen(void);

/* Run the full render loop for N frames.
 * Port of WAIT_FRAMES_WITH_UPDATES (asm/overworld/wait_frames_with_updates.asm). */
void wait_frames_with_updates(uint16_t count);

/* Run the render loop until the current fade completes.
 * Port of RUN_FRAMES_UNTIL_FADE_DONE (asm/system/palette/run_frames_until_fade_done.asm). */
void run_frames_until_fade_done(void);

/* Display text from a SNES ROM address and wait for entity fade to complete.
 * Port of DISPLAY_TEXT_AND_WAIT_FOR_FADE (asm/text/display_text_and_wait_for_fade.asm).
 * Disables all entities, runs text script, waits for any entity fade,
 * then re-enables all entities. */
void display_text_and_wait_for_fade(uint32_t text_addr);

/* Process the next queued interaction (NPC text, door, screen reload, etc.).
 * Port of PROCESS_QUEUED_INTERACTIONS (asm/overworld/process_queued_interactions.asm).
 * Dequeues from the circular interaction queue and dispatches by type. */
void process_queued_interactions(void);

/* Make an NPC entity face the leader (opposite direction).
 * Port of SET_ENTITY_DIRECTION_FROM_LEADER (asm/overworld/entity/set_entity_direction_from_leader.asm). */
void set_entity_direction_from_leader(int16_t entity_slot);

/* Check for NPC/door in a specific direction from the leader.
 * Port of CHECK_COLLISION_IN_DIRECTION (asm/overworld/collision/check_collision_in_direction.asm).
 * Sets interacting_npc_id/entity. Returns interacting_npc_id. */
int16_t check_collision_in_direction(int16_t direction);

/* Check for adjacent NPC/door in a direction (extended door search).
 * Port of CHECK_DIRECTIONAL_NPC_COLLISION (asm/overworld/collision/check_directional_npc_collision.asm). */
int16_t check_directional_npc_collision(int16_t direction);

/* Try 4 directions to find a talkable NPC near the leader.
 * Port of FIND_CLEAR_DIRECTION_FOR_LEADER (asm/overworld/find_clear_direction_for_leader.asm).
 * Updates leader_direction to face the found NPC. Returns direction or -1. */
int16_t find_clear_direction_for_leader(void);

/* Try 4 directions to find an adjacent NPC for checking.
 * Port of FIND_ADJACENT_NPC_INTERACTION (asm/overworld/npc/find_adjacent_npc_interaction.asm). */
int16_t find_adjacent_npc_interaction(void);

/* Initialize interaction state and find a nearby talkable NPC.
 * Port of FIND_NEARBY_TALKABLE_TPT_ENTRY (asm/overworld/find_nearby_talkable_tpt_entry.asm).
 * Updates leader facing direction. Returns interacting_npc_id. */
int16_t find_nearby_talkable_tpt_entry(void);

/* Initialize interaction state and find a nearby checkable NPC/object.
 * Port of FIND_NEARBY_CHECKABLE_TPT_ENTRY (asm/overworld/find_nearby_checkable_tpt_entry.asm). */
int16_t find_nearby_checkable_tpt_entry(void);

/* Find a talkable NPC, look up its dialogue text pointer.
 * Port of TALK_TO (asm/overworld/talk_to.asm).
 * Returns SNES ROM address of text, or 0 if nothing to talk to. */
uint32_t talk_to(void);

/* Find a checkable NPC/object, look up its text/item data.
 * Port of CHECK (asm/overworld/check.asm).
 * Returns SNES ROM address of text, or 0 if nothing to check. */
uint32_t check_action(void);

/* SET_TELEPORT_BOX_DESTINATION: Port of asm/misc/set_teleport_box_destination.asm.
 * Saves destination_id to game_state.unknownC3 and copies leader coords to respawn. */
void set_teleport_box_destination(uint8_t destination_id);

/* TELEPORT_FREEZEOBJECTS: Port of asm/misc/teleport_freezeobjects.asm.
 * Sets OBJECT_TICK_DISABLED | OBJECT_MOVE_DISABLED on entities 0-22 (raw slot loop). */
void teleport_freeze_entities(void);

/* TELEPORT_FREEZEOBJECTS2: Port of asm/misc/teleport_freezeobjects2.asm.
 * Same as teleport_freeze_entities but only freezes entities not already frozen. */
void teleport_freeze_entities_conditional(void);

/* ---- Overworld Task Scheduler ----
 * Port of SCHEDULE_OVERWORLD_TASK / PROCESS_OVERWORLD_TASKS.
 * 4-slot delayed callback system for escalators, stairs, etc. */
int schedule_overworld_task(void (*callback)(void), uint16_t frames);
void process_overworld_tasks(void);

/* ---- Auto-Movement Buffer ----
 * Port of the demo/auto-movement recording/playback system.
 * Used by escalator and stairs movement handlers. */
void clear_auto_movement_buffer(void);
void record_repeated_auto_movement(uint16_t direction_index, uint16_t count);
void queue_auto_movement_step(void);
uint16_t calculate_movement_path_steps(int16_t src_x, int16_t src_y,
                                        int16_t dest_x, int16_t dest_y);

/* Per-frame demo buffer tick — call from joypad update path.
 * Port of READ_JOYPAD demo section. */
void demo_playback_tick(void);

/* ATTEMPT_HOMESICKNESS: Port of asm/overworld/attempt_homesickness.asm.
 * Checks if Ness should become homesick based on level and probability table.
 * Returns 0 always. */
uint16_t attempt_homesickness(void);

/* ADJUST_SINGLE_COLOUR: Port of asm/overworld/adjust_single_colour.asm.
 * Adjusts a single colour channel toward a target, clamped to +/-6 step.
 * colour1 = current, colour2 = target. Returns adjusted value. */
uint16_t adjust_single_colour(uint16_t colour1, uint16_t colour2);

/* INITIALIZE_MAP: Port of asm/overworld/initialize_map.asm.
 * Calls resolve_map_sector_music, load_map_at_position,
 * set_leader_position_and_load_party, apply_next_map_music in sequence. */
void initialize_map(uint16_t x, uint16_t y, uint16_t direction);

/* SET_TELEPORT_STATE: Port of asm/overworld/set_teleport_state.asm.
 * Stores teleport destination and style into global variables. */
void set_teleport_state(uint8_t destination, uint8_t style);

/* GET_DIRECTION_FROM_PLAYER_TO_ENTITY:
 * Port of asm/overworld/get_direction_from_player_to_entity.asm.
 * Computes 8-way direction from leader to current entity. */
int16_t get_direction_from_player_to_entity(void);

/* GET_OPPOSITE_DIRECTION_FROM_PLAYER_TO_ENTITY:
 * Port of asm/overworld/get_opposite_direction_from_player_to_entity.asm.
 * Returns the opposite of get_direction_from_player_to_entity(). */
int16_t get_opposite_direction_from_player_to_entity(void);

/* CHOOSE_ENTITY_DIRECTION_TO_PLAYER:
 * Port of asm/overworld/entity/choose_entity_direction_to_player.asm.
 * If enemy should flee, returns direction away from player.
 * Otherwise returns direction toward player. */
int16_t choose_entity_direction_to_player(void);

/* GET_OFF_BICYCLE_WITH_MESSAGE:
 * Port of GET_OFF_BICYCLE (asm/overworld/get_off_bicycle.asm).
 * Displays "got off the bicycle" text, then calls dismount_bicycle(). */
void get_off_bicycle_with_message(void);

/* UPDATE_OVERWORLD_DAMAGE: Port of asm/overworld/update_overworld_damage.asm.
 * Applies per-frame poison/environmental damage to party members.
 * Returns total remaining HP (0 = all party KO'd -> triggers SPAWN). */
uint16_t update_overworld_damage(void);

/* INITIATE_ENEMY_ENCOUNTER: Port of asm/overworld/initiate_enemy_encounter.asm.
 * Called after camera shake when an enemy touches the party.
 * Determines battle initiative from approach directions, sets up battle group,
 * marks participating enemy entities, and hides non-participants.
 * Clears enemy_has_been_touched and sets battle_swirl_countdown. */
void initiate_enemy_encounter(void);

/* GET_MAP_ENEMY_PLACEMENT: Port of asm/overworld/get_map_enemy_placement.asm.
 * Returns encounter_id for tile at (x, y) in the 128x160 enemy placement grid.
 * x: 0-127 (64px tile columns), y: 0-159 (64px tile rows). */
uint16_t get_map_enemy_placement(uint16_t x, uint16_t y);

/* CAN_ENEMY_RUN_IN_DIRECTION: Port of asm/overworld/can_enemy_run_in_direction.asm.
 * Checks if an enemy can move in a given direction based on surface flags.
 * Returns 0x80 if it can run, 0 if blocked. */
uint16_t can_enemy_run_in_direction(uint16_t surface_flags, uint16_t enemy_id);

/* ATTEMPT_ENEMY_SPAWN: Port of asm/overworld/attempt_enemy_spawn.asm.
 * Core random encounter spawn logic. Creates enemy entity at random position.
 * x/y: 64px grid coordinates, encounter_id: from MAP_ENEMY_PLACEMENT. */
void attempt_enemy_spawn(uint16_t x, uint16_t y, uint16_t encounter_id);

/* SPAWN_HORIZONTAL: Port of asm/overworld/spawn_horizontal.asm.
 * Spawns enemies along new map columns when scrolling.
 * new_x: new x position, y: y coordinate (in 8px tile coords). */
void spawn_horizontal(uint16_t new_x, uint16_t y);

/* SPAWN_VERTICAL: Port of asm/overworld/spawn_vertical.asm.
 * Spawns enemies along new map rows when scrolling.
 * x: x coordinate, new_y: new y position (in 8px tile coords). */
void spawn_vertical(uint16_t x, uint16_t new_y);

/* PSI teleport tick callbacks — called from dispatch_tick_callback in callroutine.c */
void psi_teleport_alpha_tick(int16_t entity_offset);
void psi_teleport_beta_tick(int16_t entity_offset);
void psi_teleport_decelerate_tick(int16_t entity_offset);
void psi_teleport_success_tick(int16_t entity_offset);
void update_party_entity_from_buffer(int16_t entity_offset);

/* CENTER_SCREEN: Port of asm/system/center_screen.asm.
 * Subtracts 128 from x and 112 from y, then refreshes map tilemaps. */
void center_screen(uint16_t x, uint16_t y);

/* SET_PARTY_TICK_CALLBACKS: Port of asm/overworld/set_party_tick_callbacks.asm.
 * Sets tick callback for leader entity (slot 23) and 6 follower entities.
 * leader_cb/follower_cb are 24-bit ROM addresses stored as uint32_t. */
void set_party_tick_callbacks(uint16_t leader_slot,
                              uint32_t leader_cb, uint32_t follower_cb);

/* TELEPORT_MAINLOOP: Port of asm/misc/teleport_mainloop.asm.
 * Full teleport event loop. Handles all teleport styles. */
void teleport_mainloop(void);

/* SPAWN: Port of asm/overworld/spawn.asm.
 * Game over / comeback sequence. Displays game over screen, runs
 * comeback dialogue with palette animations, revives leader, halves money,
 * clears event flags 1-10, and reinitializes the map.
 * Returns result of comeback sequence (-1 = continue, 0 = no continue). */
int16_t spawn(void);

#endif /* GAME_OVERWORLD_H */
