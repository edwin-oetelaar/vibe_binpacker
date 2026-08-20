#include "bp_search.h"

#include "bp_contract.h"
#include "bp_penetration.h"
#include "bp_place.h"
#include "bp_separate.h"

#include <string.h>
#include <time.h>

/* Everything the loop carries around, so the helpers stay short. */
typedef struct {
    const bp_search_args_t *args;
    uint64_t random_state;
    /* Cells actually written to the sheet, for the occupancy invariant. */
    uint64_t placed_area_cells;
    /*
     * What the layout is worth. Measured on each part's first orientation, so
     * turning a part cannot change its value: a polygon rasterised at an
     * angle covers more cells than the same polygon square-on, and scoring
     * the rastered area would reward turning parts for no gain in material.
     */
    uint64_t value_cells;
    uint32_t placed_count;
} s_state_t;

/* xorshift64star, so a run replays exactly from its seed. */
static uint64_t
s_random_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

/* Returns a value in [0, bound). */
static uint32_t
s_random_below(uint64_t *state, uint32_t bound)
{
    BP_REQUIRE(bound > 0U);
    return (uint32_t) (s_random_next(state) % bound);
}

/* Validates the configuration and every part it will be asked to place. */
static void
s_require_args(const bp_search_args_t *args)
{
    uint32_t index;

    BP_REQUIRE(args);
    BP_REQUIRE(args->sheet);
    BP_REQUIRE(args->parts);
    BP_REQUIRE(args->part_count > 0U);
    BP_REQUIRE(args->height_window > 0U);
    BP_REQUIRE(args->max_positions > 0U);
    BP_REQUIRE(args->max_ruin > 0U);
    BP_REQUIRE(args->blink_rate >= 0.0 && args->blink_rate < 1.0);
    BP_REQUIRE(args->compact_fraction >= 0.0 && args->compact_fraction <= 1.0);
    if (args->scratch_counts) {
        BP_REQUIRE(args->scratch_prefix);
        BP_REQUIRE(args->scratch_weights);
        BP_REQUIRE(args->scratch_overlap);
        BP_REQUIRE(args->scratch_count_capacity
            >= bp_penetration_count_capacity(args->sheet->width_cells,
                args->sheet->height_cells));
        BP_REQUIRE(args->scratch_prefix_capacity
            >= bp_penetration_prefix_capacity(args->sheet->width_cells,
                args->sheet->height_cells));
    }
    BP_REQUIRE(args->scratch_order);
    BP_REQUIRE(args->scratch_placed);
    BP_REQUIRE(args->scratch_best_placed);
    BP_REQUIRE(args->scratch_best);
    BP_REQUIRE(args->scratch_capacity >= args->part_count);
    BP_REQUIRE(args->scratch_rows);
    BP_REQUIRE(args->scratch_row_capacity
        >= bp_place_scratch_words(args->sheet));
    BP_REQUIRE(args->placements);
    BP_REQUIRE(args->placed);
    BP_REQUIRE(args->placement_capacity >= args->part_count);

    for (index = 0U; index < args->part_count; ++index) {
        uint32_t other;
        uint32_t orientation;

        BP_REQUIRE(args->parts[index].orientation_count > 0U);
        BP_REQUIRE(args->parts[index].orientations);
        for (orientation = 0U;
            orientation < args->parts[index].orientation_count; ++orientation) {
            const bp_orientation_t *option =
                &args->parts[index].orientations[orientation];

            bp_mask_require_valid(option->collision);
            bp_mask_require_valid(option->solid);
        }
        for (other = index + 1U; other < args->part_count; ++other)
            BP_REQUIRE(args->parts[index].id != args->parts[other].id);
    }
}

/* Returns where the solid geometry sits for a placement. */
static void
s_solid_origin(const bp_search_args_t *args, const bp_placement_t *placement,
    uint32_t *x, uint32_t *y)
{
    const bp_orientation_t *option =
        &args->parts[placement->part_index].orientations[placement->orientation];

    *x = placement->x_cells + option->solid_offset_x_cells;
    *y = placement->y_cells + option->solid_offset_y_cells;
}

/* Writes one part's true geometry onto the sheet. */
static void
s_apply(s_state_t *state, const bp_placement_t *placement)
{
    const bp_search_args_t *args = state->args;
    const bp_orientation_t *option =
        &args->parts[placement->part_index].orientations[placement->orientation];
    uint32_t x;
    uint32_t y;

    s_solid_origin(args, placement, &x, &y);
    bp_mask_blit(args->sheet, option->solid, x, y);
    state->placed_area_cells += option->solid->area_cells;
    state->value_cells +=
        args->parts[placement->part_index].orientations[0].solid->area_cells;
    ++state->placed_count;
}

/* Takes one part back off the sheet. */
static void
s_remove(s_state_t *state, const bp_placement_t *placement)
{
    const bp_search_args_t *args = state->args;
    const bp_orientation_t *option =
        &args->parts[placement->part_index].orientations[placement->orientation];
    uint32_t x;
    uint32_t y;

    s_solid_origin(args, placement, &x, &y);
    bp_mask_erase(args->sheet, option->solid, x, y);
    BP_REQUIRE(state->placed_area_cells >= option->solid->area_cells);
    state->placed_area_cells -= option->solid->area_cells;
    state->value_cells -=
        args->parts[placement->part_index].orientations[0].solid->area_cells;
    BP_REQUIRE(state->placed_count > 0U);
    --state->placed_count;
}

/*
 * Puts one part on the sheet at the best-scoring position over all of its
 * orientations. Returns 1 when it went down, 0 when nothing fits, which is an
 * expected outcome on a full sheet.
 */
static int
s_insert(s_state_t *state, uint32_t part_index, uint64_t *positions_examined)
{
    const bp_search_args_t *args = state->args;
    const bp_part_t *part = &args->parts[part_index];
    bp_placement_t placement;
    double best_score = 0.0;
    int have_best = 0;
    uint32_t orientation;

    for (orientation = 0U; orientation < part->orientation_count;
        ++orientation) {
        bp_score_best_result_t candidate = bp_score_best_position(
            (bp_score_best_args_t) {
                .sheet = args->sheet,
                .mask = part->orientations[orientation].collision,
                .weights = args->weights,
                .height_window = args->height_window,
                .max_positions = args->max_positions,
                .blink_rate = args->blink_rate,
                .random_state = &state->random_state,
                .scratch = args->scratch_rows,
                .scratch_capacity = args->scratch_row_capacity
            });

        *positions_examined += candidate.positions_examined;
        if (!candidate.found)
            continue;
        if (!have_best || candidate.score > best_score) {
            have_best = 1;
            best_score = candidate.score;
            placement.part_index = part_index;
            placement.part_id = part->id;
            placement.orientation = orientation;
            placement.x_cells = candidate.x_cells;
            placement.y_cells = candidate.y_cells;
        }
    }

    if (!have_best)
        return 0;
    args->placements[part_index] = placement;
    args->placed[part_index] = 1U;
    s_apply(state, &placement);
    return 1;
}

/* Returns the centre of a placed part, in cells. */
static void
s_centre(const bp_search_args_t *args, uint32_t index, uint32_t *cx,
    uint32_t *cy)
{
    const bp_orientation_t *option = &args->parts[index]
        .orientations[args->placements[index].orientation];
    uint32_t x;
    uint32_t y;

    s_solid_origin(args, &args->placements[index], &x, &y);
    *cx = x + option->solid->width_cells / 2U;
    *cy = y + option->solid->height_cells / 2U;
}

/*
 * Lifts out a cluster of parts that sit near each other.
 *
 * Removing parts at random scatters small holes, and a greedy recreate simply
 * puts each one back where it came from, so the search never moves: measured
 * over a thousand rounds it found no improvement at all. Emptying one
 * contiguous region instead gives the recreate room to arrange that region
 * differently, which is the whole point of ruin and recreate.
 *
 * Returns how many parts came off the sheet.
 */
static uint32_t
s_ruin_cluster(s_state_t *state, uint32_t ruin_count)
{
    const bp_search_args_t *args = state->args;
    uint32_t seed_x;
    uint32_t seed_y;
    uint32_t removed = 0U;
    uint32_t index;
    uint32_t attempt;
    uint32_t seed = 0U;
    int have_seed = 0;

    /* Any placed part will do as the centre of the region to clear. */
    for (attempt = 0U; attempt < 4U * args->part_count && !have_seed;
        ++attempt) {
        uint32_t candidate = s_random_below(&state->random_state,
            args->part_count);

        if (args->placed[candidate]) {
            seed = candidate;
            have_seed = 1;
        }
    }
    if (!have_seed)
        return 0U;
    s_centre(args, seed, &seed_x, &seed_y);

    while (removed < ruin_count) {
        uint64_t best_distance = 0U;
        uint32_t best = 0U;
        int found = 0;

        for (index = 0U; index < args->part_count; ++index) {
            uint32_t cx;
            uint32_t cy;
            int64_t dx;
            int64_t dy;
            uint64_t distance;

            if (!args->placed[index])
                continue;
            s_centre(args, index, &cx, &cy);
            dx = (int64_t) cx - (int64_t) seed_x;
            dy = (int64_t) cy - (int64_t) seed_y;
            distance = (uint64_t) (dx * dx + dy * dy);
            if (!found || distance < best_distance) {
                found = 1;
                best_distance = distance;
                best = index;
            }
        }
        if (!found)
            break;
        s_remove(state, &args->placements[best]);
        args->placed[best] = 0U;
        ++removed;
    }
    return removed;
}

/* Returns the area of a part, used to place the awkward ones first. */
static uint64_t
s_part_area(const bp_part_t *part)
{
    return part->orientations[0].solid->area_cells;
}

/* Orders parts largest first: what fits around a big part rarely fits it. */
static void
s_sort_by_area(const bp_search_args_t *args)
{
    uint32_t index;

    for (index = 0U; index < args->part_count; ++index)
        args->scratch_order[index] = index;

    for (index = 1U; index < args->part_count; ++index) {
        uint32_t value = args->scratch_order[index];
        uint64_t area = s_part_area(&args->parts[value]);
        uint32_t insert = index;

        while (insert > 0U
            && s_part_area(&args->parts[args->scratch_order[insert - 1U]])
                < area) {
            args->scratch_order[insert] = args->scratch_order[insert - 1U];
            --insert;
        }
        args->scratch_order[insert] = value;
    }
}

/* Returns the height the placed geometry reaches. */
static uint32_t
s_used_height(const s_state_t *state)
{
    const bp_search_args_t *args = state->args;
    uint32_t index;
    uint32_t top = 0U;

    for (index = 0U; index < args->part_count; ++index) {
        const bp_orientation_t *option;
        uint32_t x;
        uint32_t y;

        if (!args->placed[index])
            continue;
        option = &args->parts[index]
            .orientations[args->placements[index].orientation];
        s_solid_origin(args, &args->placements[index], &x, &y);
        if (y + option->solid->height_cells > top)
            top = y + option->solid->height_cells;
    }
    return top;
}

/*
 * Returns the layout's centre of mass, as the sum over placed parts of area
 * times the height of the part's middle.
 *
 * This exists because the used height is a terrible thing to search on: it
 * only moves when the single topmost part moves, so almost every rearrangement
 * scores identically and the search has nothing to climb. Weighing every part
 * by how low it sits gives a gradient that responds to all of them, and
 * pulling the mass down is what eventually pulls the top down too.
 */
static uint64_t
s_lift(const s_state_t *state)
{
    const bp_search_args_t *args = state->args;
    uint32_t index;
    uint64_t lift = 0U;

    for (index = 0U; index < args->part_count; ++index) {
        const bp_orientation_t *option;
        uint32_t x;
        uint32_t y;

        if (!args->placed[index])
            continue;
        option = &args->parts[index]
            .orientations[args->placements[index].orientation];
        s_solid_origin(args, &args->placements[index], &x, &y);
        lift += option->solid->area_cells
            * (2U * (uint64_t) y + option->solid->height_cells) / 2U;
    }
    return lift;
}

/*
 * More material placed wins outright; then the lower stack, because the
 * leftover is worth more as one clean strip than as scattered gaps; then the
 * lower centre of mass, which is what makes the difference measurable at all
 * once every part fits.
 */
static int
s_is_better(uint64_t value, uint32_t height, uint64_t lift,
    uint64_t best_value, uint32_t best_height, uint64_t best_lift)
{
    if (value != best_value)
        return value > best_value;
    if (height != best_height)
        return height < best_height;
    return lift < best_lift;
}

/* Rebuilds the sheet and the counters from a saved layout. */
static void
s_restore(s_state_t *state, const bp_placement_t *saved,
    const uint8_t *saved_placed)
{
    const bp_search_args_t *args = state->args;
    uint32_t index;

    bp_bitmap_clear(args->sheet);
    state->placed_area_cells = 0U;
    state->value_cells = 0U;
    state->placed_count = 0U;

    for (index = 0U; index < args->part_count; ++index) {
        args->placed[index] = saved_placed[index];
        if (!saved_placed[index])
            continue;
        args->placements[index] = saved[index];
        s_apply(state, &args->placements[index]);
    }
}

/* Returns the height the collision footprints reach, which is what squeezes. */
static uint32_t
s_region_height(const bp_search_args_t *args)
{
    uint32_t index;
    uint32_t top = 0U;

    for (index = 0U; index < args->part_count; ++index) {
        const bp_orientation_t *option;
        uint32_t reach;

        if (!args->placed[index])
            continue;
        option = &args->parts[index]
            .orientations[args->placements[index].orientation];
        reach = args->placements[index].y_cells
            + option->collision->height_cells;
        if (reach > top)
            top = reach;
    }
    return top;
}

/* Returns the tallest collision footprint, the floor any squeeze stops at. */
static uint32_t
s_tallest(const bp_search_args_t *args)
{
    uint32_t index;
    uint32_t tallest = 0U;

    for (index = 0U; index < args->part_count; ++index) {
        const bp_orientation_t *option;

        if (!args->placed[index])
            continue;
        option = &args->parts[index]
            .orientations[args->placements[index].orientation];
        if (option->collision->height_cells > tallest)
            tallest = option->collision->height_cells;
    }
    return tallest;
}

/* Rebuilds the sheet bitmap from the placements after a separation. */
static void
s_rebuild_sheet(s_state_t *state)
{
    const bp_search_args_t *args = state->args;
    uint32_t index;

    bp_bitmap_clear(args->sheet);
    state->placed_area_cells = 0U;
    state->value_cells = 0U;
    state->placed_count = 0U;
    for (index = 0U; index < args->part_count; ++index)
        if (args->placed[index])
            s_apply(state, &args->placements[index]);
}

/*
 * Squeezes the layout: shrink the band the parts have to fit in, push the ones
 * that now stick out back inside, and let bp_separate resolve the overlap that
 * creates. Success means a strictly shorter layout; failure means the squeeze
 * was too greedy and is rolled back.
 *
 * Each attempt asks less than the last one that failed, so the phase converges
 * on the tightest band that still separates rather than giving up at the first
 * refusal.
 */
static void
s_compact(s_state_t *state, clock_t started, double deadline,
    bp_search_result_t *result)
{
    const bp_search_args_t *args = state->args;
    bp_penetration_t field;
    uint32_t region_height = s_region_height(args);
    uint32_t floor_height = s_tallest(args);
    uint32_t shrink;
    uint32_t failures = 0U;

    if (region_height <= floor_height)
        return;
    shrink = region_height / 200U;
    if (shrink == 0U)
        shrink = 1U;

    field = bp_penetration_init((bp_penetration_init_args_t) {
        .width_cells = args->sheet->width_cells,
        .height_cells = args->sheet->height_cells,
        .counts = args->scratch_counts,
        .count_capacity = args->scratch_count_capacity,
        .prefix = args->scratch_prefix,
        .prefix_capacity = args->scratch_prefix_capacity
    });

    while (failures < 4U) {
        uint32_t target;
        uint32_t index;
        bp_separate_result_t separated;

        if (deadline > 0.0
            && (double) (clock() - started) / CLOCKS_PER_SEC >= deadline)
            break;

        target = region_height > shrink ? region_height - shrink : 0U;
        if (target < floor_height)
            target = floor_height;
        if (target >= region_height)
            break;

        /* Keep the layout to fall back to before anything is disturbed. */
        memcpy(args->scratch_best, args->placements,
            args->part_count * sizeof args->scratch_best[0]);
        memcpy(args->scratch_best_placed, args->placed,
            args->part_count * sizeof args->scratch_best_placed[0]);

        bp_penetration_clear(&field);
        for (index = 0U; index < args->part_count; ++index) {
            const bp_orientation_t *option;
            uint32_t limit;

            if (!args->placed[index])
                continue;
            option = &args->parts[index]
                .orientations[args->placements[index].orientation];
            limit = target - option->collision->height_cells;
            if (args->placements[index].y_cells > limit)
                args->placements[index].y_cells = limit;
            /* The field holds true geometry; see bp_separate for why. */
            /* Every part starts at weight one; bp_separate raises them. */
            bp_penetration_add(&field, option->solid,
                args->placements[index].x_cells
                    + option->solid_offset_x_cells,
                args->placements[index].y_cells
                    + option->solid_offset_y_cells, 1U);
        }

        ++result->squeezes;
        separated = bp_separate_resolve((bp_separate_args_t) {
            .field = &field,
            .parts = args->parts,
            .part_count = args->part_count,
            .placements = args->placements,
            .placed = args->placed,
            .region_width_cells = args->sheet->width_cells,
            .region_height_cells = target,
            .max_iterations = 300U * args->part_count,
            .samples = 48U,
            .patience = 30U * args->part_count,
            .random_state = &state->random_state,
            .scratch_weights = args->scratch_weights,
            .scratch_overlap = args->scratch_overlap,
            .scratch_capacity = args->part_count
        });

        if (separated.resolved) {
            ++result->squeezes_resolved;
            s_rebuild_sheet(state);
            region_height = s_region_height(args);
            failures = 0U;
        } else {
            memcpy(args->placements, args->scratch_best,
                args->part_count * sizeof args->placements[0]);
            memcpy(args->placed, args->scratch_best_placed,
                args->part_count * sizeof args->placed[0]);
            s_rebuild_sheet(state);
            ++failures;
            /* Ask for less next time rather than stopping outright. */
            shrink = shrink > 1U ? shrink / 2U : 1U;
            if (shrink == 1U && failures >= 4U)
                break;
        }
    }

    /* The compaction result is the layout to keep. */
    memcpy(args->scratch_best, args->placements,
        args->part_count * sizeof args->scratch_best[0]);
    memcpy(args->scratch_best_placed, args->placed,
        args->part_count * sizeof args->scratch_best_placed[0]);
}

/* Packs by ruin and recreate. */
bp_search_result_t
bp_search_solve(bp_search_args_t args)
{
    bp_search_result_t result;
    s_state_t state;
    clock_t started = clock();
    uint64_t positions_examined = 0U;
    uint64_t best_value;
    uint32_t best_height;
    uint64_t best_lift;
    double reshuffle_deadline;
    uint32_t index;
    uint32_t iteration;

    s_require_args(&args);
    memset(&result, 0, sizeof result);
    state.args = &args;
    state.random_state = args.seed ? args.seed : UINT64_C(0x9e3779b97f4a7c15);
    state.placed_area_cells = 0U;
    state.value_cells = 0U;
    state.placed_count = 0U;

    bp_bitmap_clear(args.sheet);
    memset(args.placed, 0, args.part_count * sizeof args.placed[0]);

    /* Construct: largest first, each at its best-scoring position. */
    s_sort_by_area(&args);
    for (index = 0U; index < args.part_count; ++index)
        (void) s_insert(&state, args.scratch_order[index],
            &positions_examined);

    best_value = state.value_cells;
    best_height = s_used_height(&state);
    best_lift = s_lift(&state);
    memcpy(args.scratch_best, args.placements,
        args.part_count * sizeof args.scratch_best[0]);
    memcpy(args.scratch_best_placed, args.placed,
        args.part_count * sizeof args.scratch_best_placed[0]);

    reshuffle_deadline = args.time_budget_seconds > 0.0
        ? args.time_budget_seconds * (1.0 - args.compact_fraction)
        : 0.0;

    for (iteration = 0U; iteration < args.iterations; ++iteration) {
        uint32_t ruin_count;
        uint32_t removed;
        uint64_t value;
        uint32_t height;
        uint64_t lift;

        if (reshuffle_deadline > 0.0
            && (double) (clock() - started) / CLOCKS_PER_SEC
                >= reshuffle_deadline)
            break;
        ++result.iterations_run;

        /* Ruin: empty one region of the sheet. */
        ruin_count = 1U + s_random_below(&state.random_state, args.max_ruin);
        removed = s_ruin_cluster(&state, ruin_count);
        if (removed == 0U)
            break;

        /*
         * Recreate: everything now off the sheet goes back, largest first.
         *
         * The randomness belongs in *which* parts come out, not in the order
         * they go back. Putting a big part back after the small ones have
         * taken the good corners is simply worse, and measurably so: a random
         * insertion order lost to plain bottom-left on an over-subscribed
         * sheet, while a greedy one beats it.
         *
         * Parts that were never placed get another chance every round, which
         * is how the search recovers from a bad opening.
         */
        s_sort_by_area(&args);
        for (index = 0U; index < args.part_count; ++index) {
            uint32_t candidate = args.scratch_order[index];

            if (args.placed[candidate])
                continue;
            (void) s_insert(&state, candidate, &positions_examined);
        }

        value = state.value_cells;
        height = s_used_height(&state);
        lift = s_lift(&state);
        if (s_is_better(value, height, lift, best_value, best_height,
            best_lift)) {
            best_value = value;
            best_height = height;
            best_lift = lift;
            memcpy(args.scratch_best, args.placements,
                args.part_count * sizeof args.scratch_best[0]);
            memcpy(args.scratch_best_placed, args.placed,
                args.part_count * sizeof args.scratch_best_placed[0]);
            ++result.improvements;
        } else {
            s_restore(&state, args.scratch_best, args.scratch_best_placed);
        }
    }

    s_restore(&state, args.scratch_best, args.scratch_best_placed);
    if (args.scratch_counts)
        s_compact(&state, started, args.time_budget_seconds, &result);
    s_restore(&state, args.scratch_best, args.scratch_best_placed);

    result.placed_count = state.placed_count;
    result.unplaced_count = args.part_count - state.placed_count;
    result.placed_area_cells = state.placed_area_cells;
    result.used_height_cells = s_used_height(&state);
    result.lift_cells = s_lift(&state);
    for (index = 0U; index < args.part_count; ++index) {
        const bp_orientation_t *option;
        uint32_t x;
        uint32_t y;

        if (!args.placed[index])
            continue;
        option = &args.parts[index]
            .orientations[args.placements[index].orientation];
        s_solid_origin(&args, &args.placements[index], &x, &y);
        if (x + option->solid->width_cells > result.used_width_cells)
            result.used_width_cells = x + option->solid->width_cells;
    }
    result.seconds = (double) (clock() - started) / CLOCKS_PER_SEC;

    /* The sheet must hold exactly the area of the parts written onto it. */
    BP_REQUIRE(bp_bitmap_occupied_count(args.sheet)
        == result.placed_area_cells);
    return result;
}
