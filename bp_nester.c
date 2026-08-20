#include "bp_nester.h"

#include "bp_contract.h"

/* Returns the sort key of one part; larger keys are placed first. */
static uint64_t
s_sort_key(const bp_part_t *part, bp_nest_sort_t sort)
{
    const bp_mask_t *mask;

    BP_REQUIRE(part);
    BP_REQUIRE(part->orientation_count > 0U);
    mask = part->orientations[0].solid;

    switch (sort) {
    case BP_NEST_SORT_AREA_DESCENDING:
        return mask->area_cells;
    case BP_NEST_SORT_LONGEST_SIDE_DESCENDING:
        return mask->width_cells > mask->height_cells
            ? mask->width_cells
            : mask->height_cells;
    case BP_NEST_SORT_INPUT_ORDER:
    default:
        return 0U;
    }
}

/* Validates the solver configuration, including unique part identifiers. */
static void
s_require_args(const bp_nester_solve_args_t *args)
{
    uint32_t index;

    BP_REQUIRE(args);
    BP_REQUIRE(args->sheet);
    BP_REQUIRE(args->parts);
    BP_REQUIRE(args->part_count > 0U);
    BP_REQUIRE(args->sort == BP_NEST_SORT_INPUT_ORDER
        || args->sort == BP_NEST_SORT_AREA_DESCENDING
        || args->sort == BP_NEST_SORT_LONGEST_SIDE_DESCENDING);
    BP_REQUIRE(args->place_mode == BP_PLACE_REFERENCE
        || args->place_mode == BP_PLACE_JUMP
        || args->place_mode == BP_PLACE_EROSION);
    if (args->place_mode == BP_PLACE_EROSION) {
        BP_REQUIRE(args->scratch_rows);
        BP_REQUIRE(args->scratch_row_capacity
            >= bp_place_scratch_words(args->sheet));
    }
    BP_REQUIRE(args->scratch_order);
    BP_REQUIRE(args->scratch_capacity >= args->part_count);
    BP_REQUIRE(args->placements);
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
            /* The true geometry must sit wholly inside the grown footprint. */
            BP_REQUIRE(option->solid->width_cells
                <= option->collision->width_cells
                    - option->solid_offset_x_cells);
            BP_REQUIRE(option->solid->height_cells
                <= option->collision->height_cells
                    - option->solid_offset_y_cells);
        }
        for (other = index + 1U; other < args->part_count; ++other)
            BP_REQUIRE(args->parts[index].id != args->parts[other].id);
    }
}

/*
 * Builds the placement order. Insertion sort keeps equal keys in input order,
 * so the same input always produces the same packing; reproducibility matters
 * more here than sorting speed, which is never the bottleneck.
 */
static void
s_build_order(const bp_nester_solve_args_t *args)
{
    uint32_t index;

    for (index = 0U; index < args->part_count; ++index)
        args->scratch_order[index] = index;

    if (args->sort == BP_NEST_SORT_INPUT_ORDER)
        return;

    for (index = 1U; index < args->part_count; ++index) {
        uint32_t value = args->scratch_order[index];
        uint64_t value_key = s_sort_key(&args->parts[value], args->sort);
        uint32_t insert = index;

        while (insert > 0U
            && s_sort_key(&args->parts[args->scratch_order[insert - 1U]],
                args->sort) < value_key) {
            args->scratch_order[insert] = args->scratch_order[insert - 1U];
            --insert;
        }
        args->scratch_order[insert] = value;
    }
}

/*
 * Places one part by trying every orientation and keeping the lowest result,
 * then the leftmost, then the lowest orientation index. Returns 1 when the
 * part was placed and fills the placement.
 */
static int
s_place_part(const bp_nester_solve_args_t *args, uint32_t part_index,
    bp_placement_t *placement, uint64_t *candidate_checks)
{
    const bp_part_t *part = &args->parts[part_index];
    uint32_t orientation;
    int have_best = 0;
    bp_place_result_t best = { 0, 0U, 0U, 0U };
    uint32_t best_orientation = 0U;

    for (orientation = 0U; orientation < part->orientation_count;
        ++orientation) {
        bp_place_result_t candidate = bp_place_bottom_left(
            (bp_place_args_t) {
                .sheet = args->sheet,
                .mask = part->orientations[orientation].collision,
                .mode = args->place_mode,
                .scratch = args->scratch_rows,
                .scratch_capacity = args->scratch_row_capacity
            });

        *candidate_checks += candidate.candidate_checks;
        if (!candidate.found)
            continue;
        if (!have_best
            || candidate.y_cells < best.y_cells
            || (candidate.y_cells == best.y_cells
                && candidate.x_cells < best.x_cells)) {
            have_best = 1;
            best = candidate;
            best_orientation = orientation;
        }
    }

    if (!have_best)
        return 0;

    placement->part_index = part_index;
    placement->part_id = part->id;
    placement->orientation = best_orientation;
    placement->x_cells = best.x_cells;
    placement->y_cells = best.y_cells;
    return 1;
}

/* Decodes a part order into a packing by bottom-left-fill. */
bp_nester_solve_result_t
bp_nester_solve(bp_nester_solve_args_t args)
{
    bp_nester_solve_result_t result = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint32_t step;

    s_require_args(&args);
    bp_bitmap_clear(args.sheet);
    s_build_order(&args);

    for (step = 0U; step < args.part_count; ++step) {
        uint32_t part_index = args.scratch_order[step];
        bp_placement_t placement;
        const bp_orientation_t *option;
        uint32_t solid_x;
        uint32_t solid_y;
        uint32_t right;
        uint32_t top;

        if (!s_place_part(&args, part_index, &placement,
            &result.candidate_checks)) {
            ++result.unplaced_count;
            continue;
        }

        option = &args.parts[part_index].orientations[placement.orientation];
        solid_x = placement.x_cells + option->solid_offset_x_cells;
        solid_y = placement.y_cells + option->solid_offset_y_cells;
        /* Only the true geometry is written; the clearance stays free. */
        bp_mask_blit(args.sheet, option->solid, solid_x, solid_y);
        args.placements[result.placed_count] = placement;
        ++result.placed_count;
        result.placed_area_cells += option->solid->area_cells;

        right = solid_x + option->solid->width_cells;
        top = solid_y + option->solid->height_cells;
        if (right > result.used_width_cells)
            result.used_width_cells = right;
        if (top > result.used_height_cells)
            result.used_height_cells = top;
    }

    /* The sheet must hold exactly the area of the parts written onto it. */
    BP_REQUIRE(bp_bitmap_occupied_count(args.sheet)
        == result.placed_area_cells);
    return result;
}
