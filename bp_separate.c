#include "bp_separate.h"

#include "bp_contract.h"

/* Steps the coordinate descent tries, coarse to fine. */
static const uint32_t s_descent_steps[] = { 64U, 32U, 16U, 8U, 4U, 2U, 1U };

#define S_DESCENT_COUNT (sizeof s_descent_steps / sizeof s_descent_steps[0])

/* Window around a part's current position that half the samples fall in. */
#define S_LOCAL_REACH 48U

/* xorshift64star, so a separation replays exactly from its seed. */
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

/* Returns a value in [0, 1). */
static double
s_random_unit(uint64_t *state)
{
    return (double) (s_random_next(state) >> 11) / 9007199254740992.0;
}

/* Returns the orientation a placement currently uses. */
static const bp_orientation_t *
s_option(const bp_separate_args_t *args, uint32_t index)
{
    return &args->parts[index]
        .orientations[args->placements[index].orientation];
}

/* Returns where a placement's true geometry sits. */
static void
s_solid_origin(const bp_separate_args_t *args, uint32_t index, uint32_t x,
    uint32_t y, uint32_t *solid_x, uint32_t *solid_y)
{
    const bp_orientation_t *option = s_option(args, index);

    *solid_x = x + option->solid_offset_x_cells;
    *solid_y = y + option->solid_offset_y_cells;
}

/* Validates the configuration and that every part can fit the region. */
static void
s_require_args(const bp_separate_args_t *args)
{
    uint32_t index;

    BP_REQUIRE(args);
    BP_REQUIRE(args->field);
    BP_REQUIRE(args->parts);
    BP_REQUIRE(args->part_count > 0U);
    BP_REQUIRE(args->placements);
    BP_REQUIRE(args->placed);
    BP_REQUIRE(args->samples > 0U);
    BP_REQUIRE(args->patience > 0U);
    BP_REQUIRE(args->random_state);
    BP_REQUIRE(args->scratch_weights);
    BP_REQUIRE(args->scratch_overlap);
    BP_REQUIRE(args->scratch_capacity >= args->part_count);
    BP_REQUIRE(args->region_width_cells > 0U);
    BP_REQUIRE(args->region_height_cells > 0U);
    BP_REQUIRE(args->region_width_cells <= args->field->width_cells);
    BP_REQUIRE(args->region_height_cells <= args->field->height_cells);

    for (index = 0U; index < args->part_count; ++index) {
        const bp_mask_t *mask;

        if (!args->placed[index])
            continue;
        BP_REQUIRE(args->placements[index].orientation
            < args->parts[index].orientation_count);
        mask = s_option(args, index)->collision;
        /* Squeezing below what a part measures is the caller's mistake. */
        BP_REQUIRE(mask->width_cells <= args->region_width_cells);
        BP_REQUIRE(mask->height_cells <= args->region_height_cells);
        BP_REQUIRE(args->placements[index].x_cells
            <= args->region_width_cells - mask->width_cells);
        BP_REQUIRE(args->placements[index].y_cells
            <= args->region_height_cells - mask->height_cells);
    }
}

/*
 * Returns how much other material lies inside one part's grown footprint,
 * which is exactly how much it violates the clearance by.
 */
static uint64_t
s_overlap_of(const bp_separate_args_t *args, uint32_t index)
{
    const bp_orientation_t *option = s_option(args, index);
    uint64_t own = (uint64_t) args->scratch_weights[index]
        * option->solid->area_cells;
    uint64_t covered = bp_penetration_probe(args->field, option->collision,
        args->placements[index].x_cells, args->placements[index].y_cells);

    /* Its own geometry lies inside its grown footprint and does not count. */
    BP_REQUIRE(covered >= own);
    return covered - own;
}

/*
 * Picks the part to move, at random among those that overlap, with a chance
 * proportional to how badly they do and how often they have failed.
 *
 * Always taking the worst offender looks obvious and deadlocks. A part whose
 * best available position is no better than the one it already has does not
 * move; its penalty then rises, which makes it the worst offender again, and
 * the search spends the rest of its budget picking the one part it cannot
 * help. Measured on four squares that fit a sheet easily, that left 23 cells
 * of overlap unresolved for two hundred straight iterations, while the part
 * that could have moved was never chosen.
 *
 * Measuring every part costs one probe each, so the overlaps are taken once
 * into scratch and the draw reads them back rather than probing twice.
 *
 * Returns 0 when nothing overlaps at all.
 */
static int
s_pick_part(const bp_separate_args_t *args, uint32_t *chosen,
    uint64_t *violation)
{
    uint32_t index;
    double total = 0.0;
    double target;
    double running = 0.0;
    int found = 0;

    *violation = 0U;
    for (index = 0U; index < args->part_count; ++index) {
        args->scratch_overlap[index] = 0U;
        if (!args->placed[index])
            continue;
        args->scratch_overlap[index] = s_overlap_of(args, index);
        *violation += args->scratch_overlap[index];
        if (args->scratch_overlap[index] == 0U)
            continue;
        found = 1;
        total += (double) args->scratch_overlap[index];
        *chosen = index;
    }
    if (!found || total <= 0.0)
        return found;

    target = s_random_unit(args->random_state) * total;
    for (index = 0U; index < args->part_count; ++index) {
        if (args->scratch_overlap[index] == 0U)
            continue;
        running += (double) args->scratch_overlap[index];
        if (running > target) {
            *chosen = index;
            return 1;
        }
    }
    return 1;
}

/* Walks downhill from a position in ever finer steps along both axes. */
static uint64_t
s_descend(const bp_separate_args_t *args, const bp_mask_t *mask, uint32_t *x,
    uint32_t *y, uint64_t cost, uint64_t *probes)
{
    uint32_t max_x = args->region_width_cells - mask->width_cells;
    uint32_t max_y = args->region_height_cells - mask->height_cells;
    size_t step_index;

    for (step_index = 0U; step_index < S_DESCENT_COUNT; ++step_index) {
        uint32_t step = s_descent_steps[step_index];
        int moved = 1;

        while (moved && cost > 0U) {
            int direction;

            moved = 0;
            for (direction = 0; direction < 4; ++direction) {
                int64_t try_x = (int64_t) *x
                    + (direction == 0 ? (int64_t) step
                        : direction == 1 ? -(int64_t) step : 0);
                int64_t try_y = (int64_t) *y
                    + (direction == 2 ? (int64_t) step
                        : direction == 3 ? -(int64_t) step : 0);
                uint64_t candidate;

                if (try_x < 0 || try_y < 0 || try_x > (int64_t) max_x
                    || try_y > (int64_t) max_y)
                    continue;
                candidate = bp_penetration_probe(args->field, mask,
                    (uint32_t) try_x, (uint32_t) try_y);
                ++*probes;
                if (candidate < cost) {
                    cost = candidate;
                    *x = (uint32_t) try_x;
                    *y = (uint32_t) try_y;
                    moved = 1;
                }
            }
        }
        if (cost == 0U)
            break;
    }
    return cost;
}

/* Returns a sample position, biased towards where the part already sits. */
static void
s_sample(const bp_separate_args_t *args, const bp_mask_t *mask, uint32_t from_x,
    uint32_t from_y, uint32_t which, uint32_t *x, uint32_t *y)
{
    uint32_t max_x = args->region_width_cells - mask->width_cells;
    uint32_t max_y = args->region_height_cells - mask->height_cells;

    /*
     * Half the samples land anywhere, which is how a part escapes the region
     * it is stuck in; the other half land nearby, which is how it settles.
     */
    if ((which & 1U) == 0U) {
        *x = max_x > 0U ? s_random_below(args->random_state, max_x + 1U) : 0U;
        *y = max_y > 0U ? s_random_below(args->random_state, max_y + 1U) : 0U;
        return;
    }
    {
        int64_t dx = (int64_t) s_random_below(args->random_state,
            2U * S_LOCAL_REACH + 1U) - (int64_t) S_LOCAL_REACH;
        int64_t dy = (int64_t) s_random_below(args->random_state,
            2U * S_LOCAL_REACH + 1U) - (int64_t) S_LOCAL_REACH;
        int64_t nx = (int64_t) from_x + dx;
        int64_t ny = (int64_t) from_y + dy;

        if (nx < 0)
            nx = 0;
        if (ny < 0)
            ny = 0;
        if (nx > (int64_t) max_x)
            nx = max_x;
        if (ny > (int64_t) max_y)
            ny = max_y;
        *x = (uint32_t) nx;
        *y = (uint32_t) ny;
    }
}

/* Drives the total overlap of a layout to zero by moving one part at a time. */
bp_separate_result_t
bp_separate_resolve(bp_separate_args_t args)
{
    bp_separate_result_t result = { 0, 0U, 0U, 0U };
    uint64_t best_overlap = UINT64_MAX;
    uint64_t violation = 0U;
    uint32_t stagnant = 0U;
    uint32_t index;
    uint32_t iteration;

    s_require_args(&args);
    for (index = 0U; index < args.part_count; ++index)
        args.scratch_weights[index] = 1U;

    for (iteration = 0U; iteration < args.max_iterations; ++iteration) {
        uint32_t worst = 0U;
        const bp_orientation_t *option;
        const bp_mask_t *mask;
        uint32_t x;
        uint32_t y;
        uint32_t solid_x;
        uint32_t solid_y;
        uint64_t cost;
        uint32_t sample;

        ++result.iterations_run;
        if (!s_pick_part(&args, &worst, &violation))
            break;

        option = s_option(&args, worst);
        mask = option->collision;
        x = args.placements[worst].x_cells;
        y = args.placements[worst].y_cells;
        s_solid_origin(&args, worst, x, y, &solid_x, &solid_y);
        bp_penetration_remove(args.field, option->solid, solid_x, solid_y,
            args.scratch_weights[worst]);

        cost = bp_penetration_probe(args.field, mask, x, y);
        ++result.probes;
        for (sample = 0U; sample < args.samples && cost > 0U; ++sample) {
            uint32_t try_x;
            uint32_t try_y;
            uint64_t candidate;

            s_sample(&args, mask, args.placements[worst].x_cells,
                args.placements[worst].y_cells, sample, &try_x, &try_y);
            candidate = bp_penetration_probe(args.field, mask, try_x, try_y);
            ++result.probes;
            if (candidate < cost) {
                cost = candidate;
                x = try_x;
                y = try_y;
            }
        }
        if (cost > 0U)
            cost = s_descend(&args, mask, &x, &y, cost, &result.probes);

        s_solid_origin(&args, worst, x, y, &solid_x, &solid_y);
        bp_penetration_add(args.field, option->solid, solid_x, solid_y,
            args.scratch_weights[worst]);
        args.placements[worst].x_cells = x;
        args.placements[worst].y_cells = y;

        /*
         * A part that is still stuck gets heavier, and that weight goes into
         * the field, so sitting on it costs more from now on. This is the
         * guided half of guided local search, and it is what the measurements
         * say the method lives or dies by: with penalties steering only which
         * part moves next, the search stalled after a handful of moves on a
         * tightly packed sheet, because no single part had anywhere better to
         * go and the landscape never changed. Raising the price of the
         * obstruction reshapes the landscape until something gives.
         */
        if (cost > 0U) {
            ++args.scratch_weights[worst];
            bp_penetration_add(args.field, option->solid, solid_x, solid_y, 1U);
        }

        if (violation < best_overlap) {
            best_overlap = violation;
            stagnant = 0U;
        } else if (++stagnant >= args.patience) {
            break;
        }
    }

    (void) s_pick_part(&args, &index, &violation);
    result.overlap_cells = violation;
    result.resolved = violation == 0U;
    return result;
}
