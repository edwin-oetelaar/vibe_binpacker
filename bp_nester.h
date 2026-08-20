#pragma once

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_place.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BP_NEST_SORT_INPUT_ORDER = 0,
    BP_NEST_SORT_AREA_DESCENDING = 1,
    BP_NEST_SORT_LONGEST_SIDE_DESCENDING = 2
} bp_nest_sort_t;

/*
 * One way a part may be placed, as a pair of masks.
 *
 * `collision` is the footprint grown by the requested clearance, and it is
 * what the search tests against the sheet. `solid` is the true geometry, and
 * it is what gets written onto the sheet and cut. Testing a grown candidate
 * against ungrown occupancy leaves exactly the requested clearance between
 * neighbours, where growing both would leave twice that.
 *
 * The placement origin is the collision mask's corner; the solid sits at
 * (solid_offset_x_cells, solid_offset_y_cells) from it, and must lie wholly
 * inside the collision mask. With zero clearance the two masks are the same
 * and both offsets are zero.
 *
 * A consequence worth knowing: parts also end up one clearance away from the
 * sheet edge, since the grown mask is what has to fit inside the sheet.
 */
typedef struct {
    const bp_mask_t *collision;
    const bp_mask_t *solid;
    uint32_t solid_offset_x_cells;
    uint32_t solid_offset_y_cells;
} bp_orientation_t;

/*
 * A part and the orientations it may be placed in. A quarter turn, a free
 * angle and a mirrored copy are all the same thing here: another orientation
 * to try. Arrays are caller owned.
 */
typedef struct {
    uint32_t id;
    const bp_orientation_t *orientations;
    uint32_t orientation_count;
} bp_part_t;

/*
 * Where one part ended up. x_cells and y_cells are the collision mask's
 * origin; the cut geometry sits at that point plus the orientation's solid
 * offset.
 */
typedef struct {
    uint32_t part_index;
    uint32_t part_id;
    uint32_t orientation;
    uint32_t x_cells;
    uint32_t y_cells;
} bp_placement_t;

typedef struct {
    bp_bitmap_t *sheet;
    const bp_part_t *parts;
    uint32_t part_count;
    bp_nest_sort_t sort;
    bp_place_mode_t place_mode;
    uint32_t *scratch_order;
    size_t scratch_capacity;
    /* Row scratch for BP_PLACE_EROSION; see bp_place_scratch_words. */
    uint64_t *scratch_rows;
    size_t scratch_row_capacity;
    bp_placement_t *placements;
    size_t placement_capacity;
} bp_nester_solve_args_t;

/*
 * used_width_cells and used_height_cells bound the solid geometry, not the
 * grown collision masks, because what is left over is what can be re-used.
 */
typedef struct {
    uint32_t placed_count;
    uint32_t unplaced_count;
    uint32_t used_width_cells;
    uint32_t used_height_cells;
    uint64_t placed_area_cells;
    uint64_t candidate_checks;
} bp_nester_solve_result_t;

/*
 * Decodes a part order into a packing by bottom-left-fill: each part in turn
 * goes to the lowest, then leftmost free position, trying every orientation
 * and keeping the lowest result. This is the published BLF baseline and the
 * initialiser the later overlap-minimisation search will start from, not the
 * final algorithm.
 *
 * The sheet is cleared first. Part IDs must be unique and every part needs at
 * least one orientation; those are contract conditions. A part that fits
 * nowhere is an expected outcome and is counted as unplaced.
 *
 * Scratch and placement capacities must both cover every part. Placements are
 * written in the order the parts were placed, so replaying them reproduces the
 * layout exactly.
 */
bp_nester_solve_result_t
bp_nester_solve(bp_nester_solve_args_t args);
