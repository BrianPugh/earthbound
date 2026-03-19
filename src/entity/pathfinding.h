/*
 * Pathfinding system — A*-style path computation for enemy entities.
 *
 * Ports of:
 *   FIND_PATH_TO_PARTY               (asm/misc/find_path_to_party.asm)
 *   CALCULATE_PATHFINDING_TARGETS     (asm/overworld/pathfinding/calculate_pathfinding_targets.asm)
 *   INITIALIZE_PATHFINDING_FOR_ENTITIES (asm/overworld/pathfinding/initialize_pathfinding_for_entities.asm)
 *   INITIALIZE_PATHFINDER             (asm/overworld/pathfinding/initialize_pathfinder.asm)
 *   PATH_BFS_SEARCH                   (asm/overworld/pathfinding/path_bfs_search.asm)
 *   TRACE_PATH_ROUTE                  (asm/misc/trace_path_route.asm)
 *   COMPRESS_PATH_WAYPOINTS           (asm/misc/compress_path_waypoints.asm — misnamed file)
 *   POPULATE_PATH_MATRIX              (asm/misc/populate_path_matrix.asm)
 *   INIT_PATH_MATRIX_BORDERS          (asm/misc/init_path_matrix_borders.asm)
 *   SORT_PATH_NODES                   (asm/misc/sort_path_nodes.asm)
 *   PATH_HEAP_ALLOC                   (asm/misc/path_heap_alloc.asm)
 *   GET_PATH_HEAP_USED_SIZE           (asm/misc/get_path_heap_used_size.asm)
 */
#ifndef ENTITY_PATHFINDING_H
#define ENTITY_PATHFINDING_H

#include "core/types.h"

/* ---- Structs matching assembly (include/structs.asm) ---- */

/* pathfinder_coords: 4 bytes */
typedef struct {
    int16_t y;
    int16_t x;
} PathfinderCoords;

/* pathfinder: 18 bytes */
typedef struct {
    int16_t from_offscreen;     /* offset 0 */
    PathfinderCoords hitbox;    /* offset 2: collision dimensions */
    PathfinderCoords origin;    /* offset 6: grid position */
    int16_t path_point_count;   /* offset 10 (unknown10) */
    int16_t path_points_ptr;    /* offset 12 (unknown12): heap offset to path data */
    int16_t path_cost;          /* offset 14 (unknown14) */
    int16_t object_index;       /* offset 16: entity slot number */
} Pathfinder;

#define MAX_PATHFINDERS 8

/* pathfinding state: matches assembly's pathfinding struct (304 bytes used of 512) */
typedef struct {
    uint16_t radius_x;          /* offset 120 (radius low word) = grid columns */
    uint16_t radius_y;          /* offset 122 (radius high word) = grid rows */
    PathfinderCoords targets[8]; /* offset 124: party member grid positions */
    int16_t target_count;       /* offset 156 */
    int16_t pathfinder_count;   /* offset 158 */
    Pathfinder pathfinders[MAX_PATHFINDERS]; /* offset 160 */
} PathfindingState;

/* ---- Collision width/height tables (17 entries each) ---- */

/* ENTITY_COLLISION_WIDTH_TABLE (asm/data/unknown/C42AA7.asm) */
static const int16_t entity_collision_width_table[17] = {
    2, 0, 2, 4, 6, 2, 3, 2, 4, 6, 0, 2, 4, 6, 8, 0, 6
};

/* ENTITY_COLLISION_HEIGHT_TABLE (asm/data/unknown/C42AC9.asm) */
static const int16_t entity_collision_height_table[17] = {
    1, 0, 1, 1, 1, 1, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0, 2
};

/* ---- Public API ---- */

/* Compute paths from enemy entities to the party.
 * Called by initiate_enemy_encounter() after marking battle participants.
 * party_count: number of party members (targets).
 * radius_x, radius_y: grid dimensions in tiles (typically 64×64).
 * Returns pointer to internal PathfindingState (caller can read pathfinder data
 * for the trim loop). The pointer is valid until the next call.
 * Port of FIND_PATH_TO_PARTY (asm/misc/find_path_to_party.asm). */
PathfindingState *find_path_to_party(int16_t party_count, int16_t radius_x,
                                     int16_t radius_y);

/* Port of PATHFIND_TO_CURRENT_ENTITY (C0C1D2).
 * Runs pathfinding from offscreen to the current entity's position.
 * Used by SETUP_DELIVERY_PATH_FROM_ENTITY to find a reverse delivery path.
 * Returns 0 on success, nonzero on failure. */
int16_t pathfind_to_current_entity(void);

/* Port of PATHFIND_TO_PARTY_LEADER (C0C116).
 * Runs pathfinding from offscreen to the party leader's position.
 * On success, snaps the delivery entity to its path start and advances path by 1.
 * Used by SETUP_DELIVERY_PATH_REVERSE to find a forward delivery path.
 * Returns 0 on success, nonzero on failure. */
int16_t pathfind_to_party_leader(void);

#endif /* ENTITY_PATHFINDING_H */
