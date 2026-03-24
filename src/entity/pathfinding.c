/*
 * Pathfinding system — A*-style path computation for enemy entities.
 * See pathfinding.h for the list of ported assembly routines.
 */

#include "entity/pathfinding.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "entity/sprite.h"
#include "game/game_state.h"
#include "game/map_loader.h"
#include "include/binary.h"
#include <stdlib.h>
#include <string.h>

/* Read party entity slot from the packed byte array */
#define READ_PARTY_SLOT(i) \
    read_u16_le(&game_state.party_entity_slots[(i)*2])

/* ---- Pathfinding heap (bump allocator in PATHFINDING_BUFFER, 0xC00 bytes) ---- */

#define PF_HEAP_SIZE 0xC00

_Static_assert(BUF_PF_HEAP + PF_HEAP_SIZE <= BUFFER_SIZE,
               "pf_heap must fit within ert.buffer");

static uint16_t *pf_heap;  /* 16-bit word array, points into ert.buffer[BUF_PF_HEAP] */
static uint16_t pf_heap_start;   /* word index */
static uint16_t pf_heap_current; /* word index */
static uint16_t pf_heap_end;     /* word index */

/* Allocate size_words 16-bit words from the heap. Returns starting word index. */
static uint16_t pf_heap_alloc(uint16_t size_words) {
    uint16_t result = pf_heap_current;
    if (pf_heap_current + size_words > pf_heap_end)
        return 0xFFFF;
    pf_heap_current += size_words;
    return result;
}

/* ---- Path matrix (collision grid, stored in ert.buffer[0x3000]) ---- */

/* The collision matrix is a byte array: each cell is one byte.
 * Values: 0xFD(-3)=wall, 0xFE(-2)=unvisited, 0xFF(-1)=target,
 *         0xFC(-4)=visited/blocked, 0=reached, 1-250=BFS cost. */
#define PATH_CELL_WALL     0xFD  /* -3 as uint8 */
#define PATH_CELL_UNVISITED 0xFE /* -2 */
#define PATH_CELL_TARGET   0xFF  /* -1 */
#define PATH_CELL_VISITED  0xFC  /* -4 */
#define PATH_CELL_EMPTY    0x00

static uint8_t *path_matrix;        /* pointer into ert.buffer[] */
static int16_t path_matrix_rows;
static int16_t path_matrix_columns;
static int16_t path_matrix_border;
static int16_t path_matrix_size;     /* rows * columns */

/* Cardinal direction offsets (flat index deltas for the matrix) */
static int16_t path_cardinal_offset[4];

/* Cardinal and diagonal direction coordinate deltas */
static const PathfinderCoords path_cardinal_index[4] = {
    { -1,  0 },  /* up */
    {  0,  1 },  /* right */
    {  1,  0 },  /* down */
    {  0, -1 },  /* left */
};
static const PathfinderCoords path_diagonal_index[4] = {
    { -1,  1 },  /* up-right */
    {  1,  1 },  /* down-right */
    {  1, -1 },  /* down-left */
    { -1, -1 },  /* up-left */
};

/* BFS circular queue indices (word indices into pf_heap) */
static uint16_t search_queue_start;
static uint16_t search_queue_end;
static uint16_t search_queue_read;
static uint16_t search_queue_write;

/* ---- INIT_PATH_MATRIX_BORDERS (C4B7A5) ---- */

static void init_path_matrix_borders(void) {
    /* Mark first and last column of each row as walls */
    for (int16_t row = 0; row < path_matrix_rows; row++) {
        int16_t last_col = path_matrix_columns - 1;
        /* Last column */
        int idx = row * path_matrix_columns + last_col;
        if (path_matrix[idx] != PATH_CELL_WALL)
            ; /* assembly always writes 253 regardless */
        path_matrix[idx] = PATH_CELL_WALL;
        /* First column */
        idx = row * path_matrix_columns;
        path_matrix[idx] = PATH_CELL_WALL;
    }

    /* Mark first and last row of each column as walls */
    for (int16_t col = 0; col < path_matrix_columns; col++) {
        int16_t last_row = path_matrix_rows - 1;
        /* Last row */
        path_matrix[last_row * path_matrix_columns + col] = PATH_CELL_WALL;
        /* First row */
        path_matrix[col] = PATH_CELL_WALL;
    }
}

/* ---- SORT_PATH_NODES (C4B859) — selection sort by (hitbox.y, hitbox.x) ---- */

/* Sorts an array of pathfinder indices by their hitbox.y (primary, ascending)
 * then hitbox.x (secondary, ascending). Uses selection sort.
 * This groups entities with identical collision dimensions together so they
 * can share a single BFS run. */
static void sort_path_nodes(PathfindingState *state, uint16_t *sorted_indices,
                            int16_t count) {
    /* Initialize sorted_indices with pathfinder heap offsets */
    for (int16_t i = 0; i < count; i++) {
        sorted_indices[i] = (uint16_t)i;  /* index into state->pathfinders[] */
    }

    if (count <= 1)
        return;

    /* Selection sort: for each position, find the minimum remaining */
    for (int16_t i = 0; i < count - 1; i++) {
        int16_t best_primary = -1;
        int16_t best_secondary = -1;
        int16_t best_pos = i;

        for (int16_t j = i; j < count; j++) {
            Pathfinder *pf = &state->pathfinders[sorted_indices[j]];
            int16_t primary = pf->hitbox.y;
            int16_t secondary = pf->hitbox.x;

            int is_less = 0;
            if (primary != best_primary) {
                is_less = ((uint16_t)primary < (uint16_t)best_primary) ? 1 : 0;
            } else {
                is_less = ((uint16_t)secondary < (uint16_t)best_secondary) ? 1 : 0;
            }

            if (is_less) {
                best_primary = primary;
                best_secondary = secondary;
                best_pos = j;
            }
        }

        /* Swap */
        uint16_t tmp = sorted_indices[i];
        sorted_indices[i] = sorted_indices[best_pos];
        sorted_indices[best_pos] = tmp;
    }
}

/* ---- POPULATE_PATH_MATRIX (C4B923) ---- */

/* For each pathfinder entity, marks its area in the collision grid.
 * If from_offscreen == 0, marks just the origin cell.
 * If from_offscreen != 0, marks border regions and resets origin to (0,0). */
static void populate_path_matrix(PathfindingState *state, uint16_t *sorted_indices,
                                 int16_t count) {
    /* Phase 1: Clear all non-wall cells to UNVISITED */
    for (int16_t i = 0; i < path_matrix_size; i++) {
        if (path_matrix[i] != PATH_CELL_WALL)
            path_matrix[i] = PATH_CELL_UNVISITED;
    }

    /* Phase 2: Process each entity */
    for (int16_t idx = 0; idx < count; idx++) {
        Pathfinder *pf = &state->pathfinders[sorted_indices[idx]];

        if (pf->from_offscreen == 0) {
            /* On-screen entity: mark origin cell as TARGET (-1) if not a wall */
            int cell = pf->origin.y * path_matrix_columns + pf->origin.x;
            if (path_matrix[cell] != PATH_CELL_WALL)
                path_matrix[cell] = PATH_CELL_TARGET;
        } else {
            /* Off-screen entity: mark border regions as TARGET */
            /* Top border rows */
            for (int16_t r = 0; r < path_matrix_border; r++) {
                for (int16_t c = 0; c < path_matrix_columns; c++) {
                    int cell = r * path_matrix_columns + c;
                    if (path_matrix[cell] != PATH_CELL_WALL)
                        path_matrix[cell] = PATH_CELL_TARGET;
                }
            }
            /* Bottom border rows */
            for (int16_t r = path_matrix_rows - path_matrix_border; r < path_matrix_rows; r++) {
                for (int16_t c = 0; c < path_matrix_columns; c++) {
                    int cell = r * path_matrix_columns + c;
                    if (path_matrix[cell] != PATH_CELL_WALL)
                        path_matrix[cell] = PATH_CELL_TARGET;
                }
            }
            /* Left border columns */
            for (int16_t c = 0; c < path_matrix_border; c++) {
                for (int16_t r = 0; r < path_matrix_rows; r++) {
                    int cell = r * path_matrix_columns + c;
                    if (path_matrix[cell] != PATH_CELL_WALL)
                        path_matrix[cell] = PATH_CELL_TARGET;
                }
            }
            /* Right border columns */
            for (int16_t c = path_matrix_columns - path_matrix_border; c < path_matrix_columns; c++) {
                for (int16_t r = 0; r < path_matrix_rows; r++) {
                    int cell = r * path_matrix_columns + c;
                    if (path_matrix[cell] != PATH_CELL_WALL)
                        path_matrix[cell] = PATH_CELL_TARGET;
                }
            }
            /* Reset origin to (0,0) for off-screen entities */
            pf->origin.y = 0;
            pf->origin.x = 0;
        }
    }
}

/* ---- PATH_BFS_SEARCH (C4BAF6) ---- */

/* BFS flood-fill from target positions. Expands wavefront through the grid,
 * marking each cell with its distance from the nearest target.
 * Respects entity collision dimensions (hitbox_height × hitbox_width).
 * state: pathfinding state with pathfinder entries
 * pathfinder_idx: index of the current pathfinder being processed
 * target_count: number of initial target cells to enqueue
 * sorted_indices: sorted pathfinder index array (heap-allocated)
 * targets_ptr: pointer to target coordinates array
 * max_iterations: max BFS iterations before stopping
 * max_cost: max BFS cost level before blocking neighbors
 * max_steps: clamp for target count */
static void path_bfs_search(PathfindingState *state, int16_t pathfinder_idx,
                            int16_t target_count, uint16_t *sorted_indices,
                            PathfinderCoords *targets_ptr,
                            int16_t max_iterations, int16_t max_cost,
                            int16_t max_steps) {
    Pathfinder *pf = &state->pathfinders[sorted_indices[pathfinder_idx]];
    int16_t hitbox_h = pf->hitbox.y;
    int16_t hitbox_w = pf->hitbox.x;

    int16_t found_targets = 0;
    int16_t total_iterations = 0;

    /* Reset queue pointers */
    search_queue_read = search_queue_start;
    search_queue_write = search_queue_start;

    /* Enqueue initial target positions */
    for (int16_t i = 0; i < target_count; i++) {
        int16_t flat_idx = targets_ptr[i].y * path_matrix_columns + targets_ptr[i].x;
        pf_heap[search_queue_write] = (uint16_t)flat_idx;
        if (search_queue_write == search_queue_end)
            search_queue_write = search_queue_start;
        else
            search_queue_write++;
    }

    /* BFS main loop */
    while (search_queue_read != search_queue_write) {
        /* Dequeue */
        uint16_t node_idx = pf_heap[search_queue_read];
        if (search_queue_read == search_queue_end)
            search_queue_read = search_queue_start;
        else
            search_queue_read++;

        uint8_t cell_val = path_matrix[node_idx];

        /* Skip cells below UNVISITED (already processed as VISITED or wall) */
        if (cell_val < PATH_CELL_UNVISITED)
            continue;

        /* Check entity collision box: verify no walls in the hitbox area */
        int passable = 1;
        int16_t base_row_idx = (int16_t)node_idx;
        for (int16_t r = 0; r < hitbox_h && passable; r++) {
            for (int16_t c = 0; c < hitbox_w; c++) {
                if (path_matrix[base_row_idx + c] == PATH_CELL_WALL) {
                    passable = 0;
                    break;
                }
            }
            base_row_idx += path_matrix_columns;
        }

        if (!passable) {
            /* Wall in hitbox — mark as VISITED and skip */
            path_matrix[node_idx] = PATH_CELL_VISITED;
            continue;
        }

        /* If this is a TARGET cell (-1), count it */
        if (cell_val == PATH_CELL_TARGET) {
            found_targets++;
            /* If this is the first found target and pathfinder expects exactly 1 target,
             * record its grid position */
            if (pf->from_offscreen == 1) {
                pf->origin.y = (int16_t)(node_idx / path_matrix_columns);
                pf->origin.x = (int16_t)(node_idx % path_matrix_columns);
            }
        }

        /* Mark as VISITED before expanding */
        uint8_t min_neighbor = PATH_CELL_VISITED;

        /* Expand cardinal neighbors */
        for (int dir = 0; dir < 4; dir++) {
            int16_t neighbor = (int16_t)node_idx + path_cardinal_offset[dir];
            if (neighbor < 0 || neighbor >= path_matrix_size)
                continue;
            uint8_t nval = path_matrix[neighbor];

            if (nval >= PATH_CELL_UNVISITED) {
                /* Unvisited or target — enqueue if queue not full */
                int queue_full;
                if (search_queue_read == search_queue_start) {
                    queue_full = (search_queue_write == search_queue_end) ? 1 : 0;
                } else {
                    queue_full = (search_queue_write + 1 == search_queue_read) ? 1 : 0;
                }

                if (!queue_full) {
                    pf_heap[search_queue_write] = (uint16_t)neighbor;
                    if (search_queue_write == search_queue_end)
                        search_queue_write = search_queue_start;
                    else
                        search_queue_write++;
                }
            } else {
                /* Already visited — track minimum cost neighbor.
                 * Assembly: BLTEQ @NEXT_NEIGHBOR — skips if min_neighbor <= nval */
                if (nval < min_neighbor)
                    min_neighbor = nval;
            }
        }

        /* Set this cell's cost */
        if (min_neighbor == PATH_CELL_VISITED) {
            /* No lower-cost neighbor found — this is a start cell (cost 0) */
            path_matrix[node_idx] = PATH_CELL_EMPTY;
        } else {
            uint8_t cost = (uint8_t)(min_neighbor + 1);
            path_matrix[node_idx] = cost;

            /* If cost == max_cost, block all unvisited neighbors */
            if ((cost & 0xFF) == (uint8_t)max_cost) {
                for (int dir = 0; dir < 4; dir++) {
                    int16_t n = (int16_t)node_idx + path_cardinal_offset[dir];
                    if (n < 0 || n >= path_matrix_size)
                        continue;
                    if (path_matrix[n] >= PATH_CELL_UNVISITED)
                        path_matrix[n] = PATH_CELL_VISITED;
                }
            }
        }

        /* Check termination conditions.
         * Assembly uses BLTEQ (unsigned <=): -1 = 0xFFFF = effectively unlimited. */
        total_iterations++;
        if ((uint16_t)max_iterations <= (uint16_t)total_iterations)
            break;
        if (found_targets == max_steps)
            break;
    }
}

/* ---- TRACE_PATH_ROUTE (C4BD9A) ---- */

/* Traces a path from a cell backward through the BFS cost gradient.
 * Returns the number of waypoints stored. */
static int16_t trace_path_route(int16_t start_y, int16_t start_x,
                                int16_t max_waypoints,
                                PathfinderCoords *waypoints_out) {
    /* Read start cell cost */
    int cell = start_y * path_matrix_columns + start_x;
    uint8_t cost = path_matrix[cell];

    /* Valid BFS costs are 0x00-0xFB; 0xFC-0xFF are special markers */
    if (cost > 0xFB)
        return 0;

    if (max_waypoints == 0)
        return 0;

    /* Store first waypoint */
    waypoints_out[0].y = start_y;
    waypoints_out[0].x = start_x;
    int16_t waypoint_count = 1;

    int16_t cur_y = start_y;
    int16_t cur_x = start_x;
    int last_dir = 0;

    while ((cost & 0xFF) != 0) {
        int16_t best_cardinal_dir = 666;  /* sentinel */
        int16_t best_cardinal_y = 0, best_cardinal_x = 0;
        int16_t best_diagonal_dir = 666;
        int16_t best_diagonal_y = 0, best_diagonal_x = 0;

        cost--;

        int dir = last_dir;
        last_dir = 0;

        /* Check 4 cardinal directions */
        for (int attempts = 0; attempts < 4; attempts++) {
            int16_t ny = cur_y + path_cardinal_index[dir].y;
            int16_t nx = cur_x + path_cardinal_index[dir].x;
            int16_t next_dir = (dir + 1) & 3;

            int ncell = ny * path_matrix_columns + nx;
            uint8_t nval = path_matrix[ncell];

            if (nval != cost) {
                /* Not a matching cardinal — update dir and continue */
                dir = next_dir;
                last_dir++;
                continue;
            }

            /* Cardinal match found */
            if (best_cardinal_dir == 666) {
                best_cardinal_dir = dir;
                best_cardinal_y = ny;
                best_cardinal_x = nx;
            }

            /* Check if diagonal is also valid (shortcut) */
            int16_t dy = cur_y + path_diagonal_index[dir].y;
            int16_t dx = cur_x + path_diagonal_index[dir].x;
            int dcell = dy * path_matrix_columns + dx;
            uint8_t dval = path_matrix[dcell];

            if (dval != (uint8_t)(cost - 1)) {
                dir = next_dir;
                last_dir++;
                continue;
            }

            /* Check the other cardinal adjacent to the diagonal */
            int16_t adj_y = cur_y + path_cardinal_index[next_dir].y;
            int16_t adj_x = cur_x + path_cardinal_index[next_dir].x;
            int acell = adj_y * path_matrix_columns + adj_x;
            if (path_matrix[acell] != cost) {
                dir = next_dir;
                last_dir++;
                continue;
            }

            /* Valid diagonal shortcut */
            best_diagonal_dir = dir;
            best_diagonal_y = dy;
            best_diagonal_x = dx;
            break;
        }

        /* Apply best move */
        if (best_diagonal_dir != 666) {
            /* Diagonal move (costs 2 steps) */
            cur_y = best_diagonal_y;
            cur_x = best_diagonal_x;
            last_dir = best_diagonal_dir;
            cost--;  /* extra decrement for diagonal */
        } else if (best_cardinal_dir != 666) {
            /* Cardinal move */
            cur_y = best_cardinal_y;
            cur_x = best_cardinal_x;
            last_dir = best_cardinal_dir;
        } else {
            /* No valid move — path ends */
            break;
        }

        /* Check if ert.buffer full */
        if (max_waypoints == waypoint_count)
            return waypoint_count;

        /* Store waypoint */
        waypoints_out[waypoint_count].y = cur_y;
        waypoints_out[waypoint_count].x = cur_x;
        waypoint_count++;
    }

    return waypoint_count;
}

/* ---- COMPRESS_PATH_WAYPOINTS (C4BF7F) ---- */

/* Removes collinear intermediate waypoints from a path.
 * Returns the compressed waypoint count. */
static int16_t compress_path_waypoints(PathfinderCoords *waypoints, int16_t count) {
    if (count < 3)
        return count;

    /* Compute initial direction vector */
    int16_t prev_x = waypoints[1].x;
    int16_t prev_y = waypoints[1].y;
    int16_t dx = prev_x - waypoints[0].x;
    int16_t dy = prev_y - waypoints[0].y;

    int16_t out_count = 1;  /* waypoints[0] is always kept */
    /* waypoints[out_count] = last kept point */

    for (int16_t i = 2; i < count; i++) {
        int16_t cur_x = waypoints[i].x;
        int16_t cur_y = waypoints[i].y;

        /* Check if direction changed */
        int16_t new_dx = cur_x - prev_x;
        int16_t new_dy = cur_y - prev_y;

        if (new_dx == dx && new_dy == dy) {
            /* Same direction — update the last kept waypoint in-place */
            waypoints[out_count].x = cur_x;
            waypoints[out_count].y = cur_y;
        } else {
            /* Direction changed — emit a new waypoint */
            out_count++;
            waypoints[out_count].x = cur_x;
            waypoints[out_count].y = cur_y;
            dx = new_dx;
            dy = new_dy;
        }

        prev_x = cur_x;
        prev_y = cur_y;
    }

    return out_count + 1;
}

/* ---- INITIALIZE_PATHFINDER (C4B59F) ---- */

/* Main pathfinding orchestrator. Sets up the grid, runs BFS for each group
 * of pathfinders, traces routes, and stores compressed waypoints.
 * border_width: number of border rows/columns for off-screen entity targets (4).
 * max_steps: max waypoints per path AND BFS cost ceiling (64).
 * bfs_max_iterations: BFS iteration limit, -1 = unlimited (0xFFFF unsigned).
 * queue_alloc_size: BFS circular queue capacity - 1 (50 → 51 entries).
 * Returns nonzero on success, 0 on failure. */
static int16_t initialize_pathfinder(PathfindingState *state,
                                     int16_t pathfinder_count,
                                     int16_t target_count,
                                     int16_t border_width,
                                     int16_t max_steps,
                                     int16_t bfs_max_iterations,
                                     int16_t queue_alloc_size) {
    /* Initialize heap — overlay onto ert.buffer[BUF_PF_HEAP] to save static RAM */
    pf_heap = (uint16_t *)&ert.buffer[BUF_PF_HEAP];
    pf_heap_start = 0;
    pf_heap_current = 0;
    pf_heap_end = PF_HEAP_SIZE / 2;

    /* Set up matrix dimensions */
    path_matrix_rows = (int16_t)state->radius_y;
    path_matrix_columns = (int16_t)state->radius_x;
    path_matrix_border = border_width;

    int16_t matrix_size = path_matrix_rows * path_matrix_columns;
    path_matrix_size = matrix_size;
    path_matrix = &ert.buffer[BUF_PATHFINDING_MATRIX];

    /* Set up cardinal direction offsets (flat index deltas) */
    path_cardinal_offset[0] = -path_matrix_columns;  /* up */
    path_cardinal_offset[1] = 1;                      /* right */
    path_cardinal_offset[2] = path_matrix_columns;    /* down */
    path_cardinal_offset[3] = -1;                     /* left */

    /* Clamp max_steps to 251 */
    if (max_steps > 251)
        max_steps = 251;

    /* Allocate sorted index array */
    uint16_t sorted_indices_offset = pf_heap_alloc(pathfinder_count);
    uint16_t *sorted_indices = &pf_heap[sorted_indices_offset];

    /* Sort pathfinders */
    sort_path_nodes(state, sorted_indices, pathfinder_count);

    /* Allocate waypoint temp ert.buffer (max_steps × 4 bytes = max_steps × 2 words) */
    uint16_t waypoints_offset = pf_heap_alloc(max_steps * 2);
    PathfinderCoords *temp_waypoints = (PathfinderCoords *)&pf_heap[waypoints_offset];

    /* Initialize borders */
    init_path_matrix_borders();

    /* Allocate BFS queue (circular ert.buffer with queue_alloc_size + 1 entries).
     * Assembly: allocs (@LOCAL11 * 2 + 2) bytes = 51 entries for @LOCAL11=50. */
    uint16_t queue_offset = pf_heap_alloc(queue_alloc_size + 1);
    search_queue_start = queue_offset;
    search_queue_end = queue_offset + queue_alloc_size;
    search_queue_read = queue_offset;
    search_queue_write = queue_offset;

    int16_t success_count = 0;

    /* Process each pathfinder (or group of pathfinders at same position) */
    int16_t i = 0;
    while (i < pathfinder_count) {
        Pathfinder *cur = &state->pathfinders[sorted_indices[i]];

        /* Determine group: consecutive pathfinders with same (hitbox.y, hitbox.x) */
        int16_t group_count = 1;
        int16_t j = i + 1;
        while (j < pathfinder_count) {
            Pathfinder *next = &state->pathfinders[sorted_indices[j]];
            if (next->hitbox.y != cur->hitbox.y ||
                next->hitbox.x != cur->hitbox.x)
                break;
            group_count++;
            j++;
        }

        /* Populate the matrix and run BFS for this group.
         * BFS params: max_iterations=-1 (unlimited), max_cost=max_steps (64),
         * target-finding limit=group_count. */
        populate_path_matrix(state, sorted_indices + i, group_count);
        path_bfs_search(state, i, state->target_count, sorted_indices,
                        state->targets, bfs_max_iterations, max_steps,
                        group_count);

        /* Trace route for current pathfinder */
        Pathfinder *pf = &state->pathfinders[sorted_indices[i]];
        int16_t wp_count = trace_path_route(
            pf->origin.y, pf->origin.x,
            max_steps, temp_waypoints);
        pf->path_cost = wp_count;

        /* Compress waypoints */
        int16_t compressed_count = compress_path_waypoints(temp_waypoints, wp_count);

        /* Allocate and copy final waypoints */
        uint16_t final_wp_offset = pf_heap_alloc(compressed_count * 2);
        if (final_wp_offset == 0xFFFF) {
            pf->path_point_count = 0;
            pf->path_points_ptr = 0;
        } else {
            for (int16_t w = 0; w < compressed_count; w++) {
                pf_heap[final_wp_offset + w * 2] = (uint16_t)temp_waypoints[w].y;
                pf_heap[final_wp_offset + w * 2 + 1] = (uint16_t)temp_waypoints[w].x;
            }

            /* Store results in pathfinder */
            pf->path_point_count = compressed_count;
            pf->path_points_ptr = (int16_t)final_wp_offset;
        }

        if (compressed_count > 0)
            success_count++;

        /* Process remaining group members at same hitbox dimensions */
        for (int16_t k = i + 1; k < j; k++) {
            Pathfinder *gpf = &state->pathfinders[sorted_indices[k]];
            int16_t gwp = trace_path_route(
                gpf->origin.y, gpf->origin.x,
                max_steps, temp_waypoints);
            gpf->path_cost = gwp;
            int16_t gc = compress_path_waypoints(temp_waypoints, gwp);
            uint16_t goff = pf_heap_alloc(gc * 2);
            if (goff == 0xFFFF) {
                gpf->path_point_count = 0;
                gpf->path_points_ptr = 0;
            } else {
                for (int16_t w = 0; w < gc; w++) {
                    pf_heap[goff + w * 2] = (uint16_t)temp_waypoints[w].y;
                    pf_heap[goff + w * 2 + 1] = (uint16_t)temp_waypoints[w].x;
                }
                gpf->path_point_count = gc;
                gpf->path_points_ptr = (int16_t)goff;
            }
            if (gc > 0)
                success_count++;
        }

        i = j;
    }

    return success_count;
}

/* ---- CALCULATE_PATHFINDING_TARGETS (C0B9BC) ---- */

/* Compute grid-relative coordinates for each party member.
 * origin_x: grid origin X in tile coords (subtracted from absolute tile X).
 * origin_y: grid origin Y in tile coords (subtracted from absolute tile Y). */
static void calculate_pathfinding_targets(PathfindingState *state,
                                          int16_t party_count,
                                          int16_t origin_x,
                                          int16_t origin_y) {
    for (int16_t i = 0; i < party_count; i++) {
        /* Get entity slot for this party member */
        uint16_t slot = READ_PARTY_SLOT(i);
        int16_t ent = ENT(slot);
        int16_t size = entities.sizes[ent];

        /* target_x = (abs_x - collision_x_offset[size]) / 8 - origin_x, wrapped to 0-63 */
        int16_t tx = ((entities.abs_x[ent] - entity_collision_x_offset[size]) >> 3)
                     - origin_x;
        tx &= 0x3F;
        state->targets[i].x = tx;

        /* target_y = (abs_y - collision_y_offset[size] + hitbox_enable[size]) / 8 - origin_y */
        int16_t ty = ((entities.abs_y[ent] - entity_collision_y_offset[size]
                       + sprite_hitbox_enable[size]) >> 3)
                     - origin_y;
        ty &= 0x3F;
        state->targets[i].y = ty;
    }
}

/* ---- INITIALIZE_PATHFINDING_FOR_ENTITIES (C0BA35) ---- */

/* Build collision grid, set up pathfinder entries for each entity with
 * pathfinding_state == -1, then run the pathfinder.
 * max_steps: max waypoints AND BFS cost ceiling (64 from find_path_to_party).
 * queue_alloc_size: BFS queue capacity - 1 (50 from find_path_to_party).
 * Returns -1 on failure, 0 on success. */
static int16_t initialize_pathfinding_for_entities(
    PathfindingState *state,
    int16_t party_count,
    int16_t origin_x,
    int16_t origin_y,
    int16_t from_offscreen_val,
    int16_t max_steps,
    int16_t queue_alloc_size) {

    /* Phase 1: Build collision grid from LOADED_COLLISION_TILES */
    uint8_t *grid = &ert.buffer[BUF_PATHFINDING_MATRIX];
    for (int16_t row = 0; row < (int16_t)state->radius_y; row++) {
        for (int16_t col = 0; col < (int16_t)state->radius_x; col++) {
            /* Wrap coordinates to 64×64 tile grid */
            int16_t tile_x = (col + origin_x) & 0x3F;
            int16_t tile_y = (row + origin_y) & 0x3F;
            int grid_idx = tile_y * 64 + tile_x;
            uint8_t flags = ml.loaded_collision_tiles[grid_idx] & 0xC0;

            if (flags != 0)
                *grid = PATH_CELL_WALL;   /* -3: impassable */
            else
                *grid = PATH_CELL_EMPTY;  /* 0: passable */
            grid++;
        }
    }

    /* Phase 2: Set up pathfinder entries for entities with pathfinding_state == -1 */
    int16_t pathfinder_count = 0;

    for (int16_t slot = 0; slot < MAX_ENTITIES; slot++) {
        int16_t ent = ENT(slot);

        /* Skip inactive entities */
        if (entities.script_table[ent] == -1)
            continue;

        /* Only process entities participating in battle (pathfinding_state == -1) */
        if (entities.pathfinding_states[ent] != -1)
            continue;

        if (pathfinder_count >= MAX_PATHFINDERS)
            break;

        int16_t size = entities.sizes[ent];
        Pathfinder *pf = &state->pathfinders[pathfinder_count];

        pf->object_index = slot;
        pf->from_offscreen = from_offscreen_val;
        pf->hitbox.x = entity_collision_width_table[size];
        pf->hitbox.y = entity_collision_height_table[size];

        /* Compute grid-relative position */
        int16_t gx = ((entities.abs_x[ent] - entity_collision_x_offset[size]) >> 3)
                      - origin_x;
        gx &= 0x3F;
        pf->origin.x = gx;

        int16_t gy = ((entities.abs_y[ent] - entity_collision_y_offset[size]
                        + sprite_hitbox_enable[size]) >> 3)
                      - origin_y;
        gy &= 0x3F;
        pf->origin.y = gy;

        pathfinder_count++;
    }

    state->pathfinder_count = pathfinder_count;

    /* Set up targets pointer and pathfinder struct offset for INITIALIZE_PATHFINDER */
    state->target_count = party_count;

    /* Phase 3: Run pathfinder.
     * Assembly hardcodes border_width=4 and BFS max_iterations=-1.
     * max_steps (64) is used for waypoint ert.buffer size AND BFS cost ceiling.
     * queue_alloc_size (50) determines BFS queue capacity (51 entries). */
    int16_t result = initialize_pathfinder(
        state, pathfinder_count, party_count,
        4,                /* border_width (hardcoded in assembly) */
        max_steps,        /* max_steps = 64 */
        -1,               /* BFS max_iterations (unlimited, hardcoded in assembly) */
        queue_alloc_size  /* queue_alloc_size = 50 */
    );

    /* Phase 4: Wait for heap to settle (assembly loops on GET_PATH_HEAP_USED_SIZE) */
    /* In the C port this is synchronous, so no waiting needed */

    if (result == 0) {
        /* Pathfinding failed — set all entities to state 1 (request reset) */
        for (int16_t slot = 0; slot < MAX_ENTITIES; slot++) {
            int16_t ent = ENT(slot);
            if (entities.script_table[ent] != -1)
                entities.pathfinding_states[ent] = 1;
        }
        return -1;
    }

    /* Phase 5: Store path results for each pathfinder.
     * Assembly stores heap byte offsets directly into ENTITY_PATH_POINTS
     * (the consumer reads from PATHFINDING_BUFFER at that offset).
     * C port copies waypoint data from pf_heap to ert.delivery_paths and stores
     * the ert.delivery_paths byte offset in ert.entity_path_points. */
    uint16_t dp_offset = 0;
    for (int16_t pi = 0; pi < pathfinder_count; pi++) {
        Pathfinder *pf = &state->pathfinders[pi];
        int16_t slot = pf->object_index;
        int16_t ent = ENT(slot);

        if (pf->path_point_count == 0) {
            /* No path found — set pathfinding state to 1 */
            entities.pathfinding_states[ent] = 1;
        } else {
            /* Copy waypoints from pf_heap to ert.delivery_paths */
            uint16_t wp_heap_off = (uint16_t)pf->path_points_ptr;

            /* Write ert.delivery_paths offset to ert.entity_path_points (assembly writes
             * PATHFINDING_BUFFER byte address here) */
            ert.entity_path_points[ent] = dp_offset;

            for (int16_t w = 0; w < pf->path_point_count; w++) {
                uint16_t wy = pf_heap[wp_heap_off + w * 2];
                uint16_t wx = pf_heap[wp_heap_off + w * 2 + 1];
                write_u16_le(&ert.delivery_paths[dp_offset + w * 4], wy);
                write_u16_le(&ert.delivery_paths[dp_offset + w * 4 + 2], wx);
            }
            dp_offset += pf->path_point_count * 4;

            ert.entity_path_point_counts[ent] = pf->path_point_count;
        }
    }

    return 0;
}

/* ---- FIND_PATH_TO_PARTY (asm/misc/find_path_to_party.asm) ---- */

PathfindingState *find_path_to_party(int16_t party_count, int16_t radius_x,
                                     int16_t radius_y) {
    static PathfindingState state;
    memset(&state, 0, sizeof(state));

    state.radius_x = (uint16_t)radius_x;
    state.radius_y = (uint16_t)radius_y;

    int16_t half_w = radius_x / 2;
    int16_t half_h = radius_y / 2;
    ert.pathfinding_target_width = half_w;
    ert.pathfinding_target_height = half_h;

    /* Get leader entity */
    int16_t leader_slot = (int16_t)game_state.current_party_members;
    int16_t leader_ent = ENT(leader_slot);
    int16_t leader_size = entities.sizes[leader_ent];

    /* Compute pathfinding centre (tile coordinates of leader) */
    ert.pathfinding_target_centre_x =
        (entities.abs_x[leader_ent] - entity_collision_x_offset[leader_size]) >> 3;
    ert.pathfinding_target_centre_y =
        (entities.abs_y[leader_ent] - entity_collision_y_offset[leader_size]
         + sprite_hitbox_enable[leader_size]) >> 3;

    /* Compute grid origin (top-left corner in tile coords) */
    int16_t origin_x = ert.pathfinding_target_centre_x - half_w;
    int16_t origin_y = ert.pathfinding_target_centre_y - half_h;

    /* Calculate target positions for party members */
    calculate_pathfinding_targets(&state, party_count, origin_x, origin_y);

    /* Run pathfinding for all entities */
    initialize_pathfinding_for_entities(
        &state, party_count, origin_x, origin_y,
        0,   /* from_offscreen = 0 */
        64,  /* max_cost = 64 */
        50   /* max_iterations = 50 */
    );

    return &state;
}

/* ---- PATHFIND_TO_CURRENT_ENTITY (asm/overworld/pathfinding/pathfind_to_current_entity.asm) ---- */

/* Runs pathfinding from offscreen TO the current entity's position.
 * Sets up a 56×56 tile grid centred on the current entity.
 * The single target is (half_w, half_h) = (28, 28), the grid centre.
 * from_offscreen=1: the delivery entity starts from the grid border.
 * Returns 0 on success, -1 on failure. */
int16_t pathfind_to_current_entity(void) {
    static PathfindingState state;
    memset(&state, 0, sizeof(state));

    const int16_t radius = 56;
    const int16_t half_w = radius / 2;  /* 28 */
    const int16_t half_h = radius / 2;  /* 28 */

    state.radius_x = (uint16_t)radius;
    state.radius_y = (uint16_t)radius;
    ert.pathfinding_target_width  = half_w;
    ert.pathfinding_target_height = half_h;

    /* Get current entity (the delivery man — the pathfinding destination) */
    int16_t cur_slot = ert.current_entity_slot;
    int16_t cur_ent  = ENT(cur_slot);
    int16_t cur_size = entities.sizes[cur_ent];

    /* Compute tile coordinates of the current entity */
    int16_t tile_x = (entities.abs_x[cur_ent] - entity_collision_x_offset[cur_size]) >> 3;
    int16_t tile_y = (entities.abs_y[cur_ent] - entity_collision_y_offset[cur_size]
                      + sprite_hitbox_enable[cur_size]) >> 3;
    ert.pathfinding_target_centre_x = tile_x;
    ert.pathfinding_target_centre_y = tile_y;

    /* Grid origin: top-left corner in tile coords */
    int16_t origin_x = tile_x - half_w;
    int16_t origin_y = tile_y - half_h;

    /* The target is the current entity — grid centre (half_w, half_h).
     * Assembly stores: targets[0].x = half_w & 0x3F, targets[0].y = half_h & 0x3F */
    state.targets[0].x = half_w & 0x3F;
    state.targets[0].y = half_h & 0x3F;

    /* Run pathfinding: 1 target, from_offscreen=1, max_steps=252, queue_alloc=50 */
    return initialize_pathfinding_for_entities(
        &state, 1, origin_x, origin_y,
        1,    /* from_offscreen = 1 */
        252,  /* max_steps */
        50    /* queue_alloc_size */
    );
}

/* ---- PATHFIND_TO_PARTY_LEADER (asm/overworld/pathfinding/pathfind_to_party_leader.asm) ---- */

/* Runs pathfinding from offscreen TO the party leader (and all party members).
 * Sets up a 56×56 tile grid centred on the leader.
 * from_offscreen=1: delivery entity starts from the grid border.
 * On success, snaps the delivery entity's position to the first path waypoint
 * and advances path_points/path_point_count past that waypoint.
 * Returns 0 on success, -1 on failure. */
int16_t pathfind_to_party_leader(void) {
    static PathfindingState state;
    memset(&state, 0, sizeof(state));

    const int16_t radius = 56;
    const int16_t half_w = radius / 2;  /* 28 */
    const int16_t half_h = radius / 2;  /* 28 */

    state.radius_x = (uint16_t)radius;
    state.radius_y = (uint16_t)radius;
    ert.pathfinding_target_width  = half_w;
    ert.pathfinding_target_height = half_h;

    /* Get the leader entity slot from game_state.current_party_members */
    int16_t leader_slot = (int16_t)game_state.current_party_members;
    int16_t leader_ent  = ENT(leader_slot);
    int16_t leader_size = entities.sizes[leader_ent];

    /* Compute tile coordinates of leader */
    int16_t tile_x = (entities.abs_x[leader_ent] - entity_collision_x_offset[leader_size]) >> 3;
    int16_t tile_y = (entities.abs_y[leader_ent] - entity_collision_y_offset[leader_size]
                      + sprite_hitbox_enable[leader_size]) >> 3;
    ert.pathfinding_target_centre_x = tile_x;
    ert.pathfinding_target_centre_y = tile_y;

    /* Grid origin: top-left corner in tile coords */
    int16_t origin_x = tile_x - half_w;
    int16_t origin_y = tile_y - half_h;

    /* Calculate grid-relative positions for all party members as targets.
     * party_count = 1 (only the leader — index 0 in party_entity_slots). */
    calculate_pathfinding_targets(&state, 1, origin_x, origin_y);

    /* Run pathfinding: 1 target, from_offscreen=1, max_steps=252, queue_alloc=50 */
    int16_t result = initialize_pathfinding_for_entities(
        &state, 1, origin_x, origin_y,
        1,    /* from_offscreen = 1 */
        252,  /* max_steps */
        50    /* queue_alloc_size */
    );

    if (result != 0)
        return result;  /* pathfinding failed */

    /* Post-success: snap delivery entity position to the first path waypoint
     * and advance the path pointer past it.
     * Assembly: reads pathfinder[0].object_index, uses pathfinder[0].origin
     * (grid coords of the delivery entity's path start position),
     * converts back to pixel coords and stores into entity abs_x/abs_y.
     * Then increments entity_path_points[ent] by 4 and decrements count by 1. */
    Pathfinder *pf = &state.pathfinders[0];
    int16_t obj_slot = pf->object_index;
    int16_t obj_ent  = ENT(obj_slot);
    int16_t obj_size = entities.sizes[obj_ent];

    /* Convert grid origin back to pixel X:
     *   abs_x = (tile_x - half_w) * 8 + pf->origin.x * 8 + collision_x_offset[size]
     *         = origin_x * 8 + pf->origin.x * 8 + collision_x_offset[size] */
    entities.abs_x[obj_ent] = (int16_t)((origin_x + pf->origin.x) * 8
                                         + entity_collision_x_offset[obj_size]);

    /* Convert grid origin back to pixel Y:
     *   abs_y = origin_y * 8 + pf->origin.y * 8
     *           - sprite_hitbox_enable[size] + collision_y_offset[size] */
    entities.abs_y[obj_ent] = (int16_t)((origin_y + pf->origin.y) * 8
                                         - sprite_hitbox_enable[obj_size]
                                         + entity_collision_y_offset[obj_size]);

    /* Advance path: skip first waypoint (the entity's starting position) */
    ert.entity_path_points[obj_ent]      += 4;
    ert.entity_path_point_counts[obj_ent] -= 1;

    return 0;
}
