#ifndef GAME_OVERWORLD_INTERNAL_H
#define GAME_OVERWORLD_INTERNAL_H

#include "game/overworld.h"

/* SPRITE_TABLE_10_FLAGS bit masks (from include/enums.asm) */
#define SPRITE_TABLE_10_SPACING_HIDE        (1 << 12)
#define SPRITE_TABLE_10_FORCE_STATIC_ANIM   (1 << 13)
#define SPRITE_TABLE_10_WALKING_STYLE_CHANGED (1 << 15)

/* PSI teleport destination table entry layout (from include/structs.asm) */
#define PSI_TELEPORT_DEST_NAME_LEN    25
#define PSI_TELEPORT_DEST_ENTRY_SIZE  31  /* name[25] + event_flag[2] + x[2] + y[2] */

/* Text address constants used by teleport */
#define MSG_EVT_MASTER_TLPT_SNES_ADDR 0xC62AE7u

/* ---- Teleport functions (overworld_teleport.c) ---- */
void psi_teleport_alpha_tick(int16_t entity_offset);
void psi_teleport_beta_tick(int16_t entity_offset);
void psi_teleport_decelerate_tick(int16_t entity_offset);
void psi_teleport_success_tick(int16_t entity_offset);
void teleport_mainloop(void);
void calculate_velocity_components(uint16_t angle, int16_t speed, int16_t *out_x, int16_t *out_y);
void update_party_entity_from_buffer(int16_t entity_offset);
int16_t get_direction_from_player_to_entity(void);
int16_t get_opposite_direction_from_player_to_entity(void);
int16_t choose_entity_direction_to_player(void);
void get_off_bicycle_with_message(void);

/* ---- Spawn functions (overworld_spawn.c) ---- */
bool ensure_delivery_tables(void);
void load_enemy_spawn_data(void);

/* ---- Palette/damage functions (overworld_palette.c) ---- */
void restore_bg_palette_callback(void);
void start_enemy_touch_flash(void);
void check_low_hp_alert(uint16_t party_index);
int16_t skippable_pause(uint16_t frames);
void load_map_palette_animation_frame(uint16_t frame_index);
void initialize_map_palette_fade(uint16_t frames);
void update_map_palette_fade(void);
int16_t animate_map_palette_change(uint16_t frame_index, uint16_t frames);
void fade_palette_to_white(uint16_t frames);
void animate_palette_fade_with_rendering(uint16_t frames);
void initialize_game_over_screen(void);
int16_t play_comeback_sequence(void);

/* ---- Collision functions (overworld_collision.c) ---- */
int16_t check_entity_collision_at_position(int16_t x, int16_t y, int16_t entity_slot);
int16_t check_entity_and_npc_collision(int16_t x, int16_t y, int16_t entity_slot);
void clear_entity_delta_motion(int16_t slot);

/* Direction offset tables (overworld_collision.c) */
extern const int16_t interaction_x_offsets[8];
extern const int16_t interaction_y_offsets[8];
extern const int16_t direction_facing_table[8];

/* ---- Interaction functions (overworld_interaction.c) ---- */
/* (public declarations are in overworld.h) */

/* ---- Shared helpers (overworld.c) ---- */
/* These may need to be added as we discover cross-file dependencies */

#endif /* GAME_OVERWORLD_INTERNAL_H */
