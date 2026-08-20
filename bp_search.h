#pragma once

#include "bp_bitmap.h"
#include "bp_nester.h"
#include "bp_score.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    bp_bitmap_t *sheet;
    const bp_part_t *parts;
    uint32_t part_count;

    bp_score_weights_t weights;
    uint32_t height_window;
    uint32_t max_positions;
    /* Most parts lifted out in one round; the actual count is random up to it. */
    uint32_t max_ruin;
    /* Chance a candidate position is passed over; see bp_score_best_args_t. */
    double blink_rate;

    uint32_t iterations;
    /* Wall clock ceiling in CPU seconds; zero or less means no limit. */
    double time_budget_seconds;
    /* Every random choice comes from this, so a run replays exactly. */
    uint64_t seed;

    uint32_t *scratch_order;
    uint8_t *scratch_placed;
    uint8_t *scratch_best_placed;
    bp_placement_t *scratch_best;
    size_t scratch_capacity;

    uint64_t *scratch_rows;
    size_t scratch_row_capacity;

    /*
     * One entry per part, indexed by part index rather than packed, so the
     * placed flags line up with it. Only entries whose flag is set are valid.
     */
    bp_placement_t *placements;
    uint8_t *placed;
    size_t placement_capacity;
} bp_search_args_t;

typedef struct {
    uint32_t placed_count;
    uint32_t unplaced_count;
    uint32_t used_width_cells;
    uint32_t used_height_cells;
    uint64_t placed_area_cells;
    /*
     * Sum over placed parts of area times the height of the part's middle.
     * Reported because it is what the search actually descends: the used
     * height only changes when the single topmost part moves, which leaves
     * nothing to climb, while this responds to every part that settles lower.
     */
    uint64_t lift_cells;
    uint32_t iterations_run;
    uint32_t improvements;
    double seconds;
} bp_search_result_t;

/*
 * Packs by ruin and recreate: build a layout, then repeatedly lift a few parts
 * out and put them back, together with anything still unplaced, keeping the
 * result whenever it is an improvement.
 *
 * The recreate step is where the raster model pays. Every position a part can
 * take at a height comes out of one erosion pass, so putting a part back can
 * rank real alternatives instead of taking the first corner that fits; the
 * same set gathered by scanning costs ten to thirty times more.
 *
 * Better means more material placed; at equal material a lower used height,
 * because what is left over is worth more as one clean strip than as
 * scattered gaps; and at equal height a lower centre of mass, which is the
 * term that actually gives the search a gradient to follow.
 *
 * The sheet is cleared first. Every scratch array must hold part_count
 * entries, and scratch_rows must satisfy bp_place_scratch_words.
 */
bp_search_result_t
bp_search_solve(bp_search_args_t args);
