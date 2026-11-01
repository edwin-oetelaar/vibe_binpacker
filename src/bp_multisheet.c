#include "bp_multisheet.h"

#include "bp_contract.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* Validates storage and the orientation bounds before touching any buffers. */
static void
s_require_args(const bp_multisheet_args_t *args)
{
    uint32_t index;

    BP_REQUIRE(args);
    BP_REQUIRE(args->sheet);
    BP_REQUIRE(args->sheet->words);
    BP_REQUIRE(args->sheet->word_capacity >= bp_bitmap_required_words(
        args->sheet->width_cells, args->sheet->height_cells));
    BP_REQUIRE(args->parts);
    BP_REQUIRE(args->part_count > 0U);
    BP_REQUIRE(args->max_sheets > 0U);
    BP_REQUIRE(args->sort == BP_NEST_SORT_INPUT_ORDER
        || args->sort == BP_NEST_SORT_AREA_DESCENDING
        || args->sort == BP_NEST_SORT_LONGEST_SIDE_DESCENDING);
    BP_REQUIRE(args->place_mode == BP_PLACE_REFERENCE
        || args->place_mode == BP_PLACE_JUMP
        || args->place_mode == BP_PLACE_EROSION);
    BP_REQUIRE(isfinite(args->time_budget_seconds));
    BP_REQUIRE(args->time_budget_seconds >= 0.0);
    BP_REQUIRE(args->scratch_order);
    BP_REQUIRE(args->scratch_best_order);
    BP_REQUIRE(args->scratch_assigned);
    BP_REQUIRE(args->scratch_placements);
    BP_REQUIRE(args->scratch_sheets);
    BP_REQUIRE(args->placements);
    BP_REQUIRE(args->sheets);
    BP_REQUIRE(args->scratch_order != args->scratch_best_order);
    BP_REQUIRE(args->scratch_placements != args->placements);
    BP_REQUIRE(args->scratch_sheets != args->sheets);
    BP_REQUIRE(args->part_capacity >= args->part_count);
    BP_REQUIRE(args->sheet_capacity >= (args->max_sheets < args->part_count
        ? args->max_sheets : args->part_count));
    if (args->place_mode == BP_PLACE_EROSION) {
        BP_REQUIRE(args->scratch_rows);
        BP_REQUIRE(args->scratch_row_capacity
            >= bp_place_scratch_words(args->sheet));
    }

    for (index = 0U; index < args->part_count; ++index) {
        const bp_part_t *part = &args->parts[index];
        uint32_t orientation;
        uint32_t other;

        BP_REQUIRE(part->orientations);
        BP_REQUIRE(part->orientation_count > 0U);
        for (orientation = 0U; orientation < part->orientation_count;
            ++orientation) {
            const bp_orientation_t *option = &part->orientations[orientation];

            bp_mask_require_valid(option->solid);
            bp_mask_require_valid(option->collision);
            BP_REQUIRE(option->solid_offset_x_cells
                <= option->collision->width_cells);
            BP_REQUIRE(option->solid_offset_y_cells
                <= option->collision->height_cells);
            BP_REQUIRE(option->solid->width_cells
                <= option->collision->width_cells - option->solid_offset_x_cells);
            BP_REQUIRE(option->solid->height_cells
                <= option->collision->height_cells - option->solid_offset_y_cells);
        }
        for (other = index + 1U; other < args->part_count; ++other)
            BP_REQUIRE(part->id != args->parts[other].id);
    }
}

/* Returns the baseline sorting key; equal keys preserve input order. */
static uint64_t
s_key(const bp_part_t *part, bp_nest_sort_t sort)
{
    const bp_mask_t *solid;

    BP_REQUIRE(part);
    BP_REQUIRE(part->orientations);
    BP_REQUIRE(part->orientation_count > 0U);
    solid = part->orientations[0].solid;
    if (sort == BP_NEST_SORT_AREA_DESCENDING)
        return solid->area_cells;
    if (sort == BP_NEST_SORT_LONGEST_SIDE_DESCENDING)
        return solid->width_cells > solid->height_cells
            ? solid->width_cells : solid->height_cells;
    return 0U;
}

/* Builds the initial stable order without modifying caller-owned parts. */
static void
s_order(const bp_multisheet_args_t *args)
{
    uint32_t index;

    BP_REQUIRE(args);
    for (index = 0U; index < args->part_count; ++index)
        args->scratch_order[index] = index;
    for (index = 1U; index < args->part_count; ++index) {
        uint32_t value = args->scratch_order[index];
        uint64_t key = s_key(&args->parts[value], args->sort);
        uint32_t insert = index;

        while (insert > 0U && s_key(
            &args->parts[args->scratch_order[insert - 1U]], args->sort) < key) {
            args->scratch_order[insert] = args->scratch_order[insert - 1U];
            --insert;
        }
        args->scratch_order[insert] = value;
    }
}

/* Finds the lowest-leftmost legal orientation on the current sheet. */
static int
s_place(const bp_multisheet_args_t *args, bp_bitmap_t *sheet,
    uint32_t index, bp_placement_t *spot)
{
    const bp_part_t *part;
    uint32_t orientation;
    int found = 0;

    BP_REQUIRE(args);
    BP_REQUIRE(sheet);
    BP_REQUIRE(index < args->part_count);
    BP_REQUIRE(spot);
    part = &args->parts[index];
    for (orientation = 0U; orientation < part->orientation_count; ++orientation) {
        bp_place_result_t candidate = bp_place_bottom_left((bp_place_args_t) {
            .sheet = sheet,
            .mask = part->orientations[orientation].collision,
            .mode = args->place_mode,
            .scratch = args->scratch_rows,
            .scratch_capacity = args->scratch_row_capacity
        });

        if (!candidate.found)
            continue;
        if (!found || candidate.y_cells < spot->y_cells
            || (candidate.y_cells == spot->y_cells
                && candidate.x_cells < spot->x_cells)) {
            *spot = (bp_placement_t) {
                .part_index = index, .part_id = part->id,
                .orientation = orientation,
                .x_cells = candidate.x_cells, .y_cells = candidate.y_cells
            };
            found = 1;
        }
    }
    return found;
}

/* Adds true material and extends tight bounds, without counting the margins. */
static void
s_apply(const bp_multisheet_args_t *args, bp_bitmap_t *sheet,
    bp_sheet_layout_t *layout, const bp_placement_t *spot)
{
    const bp_orientation_t *option;
    uint32_t x;
    uint32_t y;
    uint32_t right;
    uint32_t top;

    BP_REQUIRE(args);
    BP_REQUIRE(sheet);
    BP_REQUIRE(layout);
    BP_REQUIRE(spot);
    BP_REQUIRE(spot->part_index < args->part_count);
    BP_REQUIRE(spot->orientation < args->parts[spot->part_index].orientation_count);
    option = &args->parts[spot->part_index].orientations[spot->orientation];
    x = spot->x_cells + option->solid_offset_x_cells;
    y = spot->y_cells + option->solid_offset_y_cells;
    right = x + option->solid->width_cells;
    top = y + option->solid->height_cells;
    bp_mask_blit(sheet, option->solid, x, y);
    if (layout->placement_count == 0U) {
        layout->min_x_cells = x;
        layout->min_y_cells = y;
    } else {
        uint32_t old_right = layout->min_x_cells + layout->width_cells;
        uint32_t old_top = layout->min_y_cells + layout->height_cells;

        if (old_right > right)
            right = old_right;
        if (old_top > top)
            top = old_top;
        if (x < layout->min_x_cells)
            layout->min_x_cells = x;
        if (y < layout->min_y_cells)
            layout->min_y_cells = y;
    }
    layout->width_cells = right - layout->min_x_cells;
    layout->height_cells = top - layout->min_y_cells;
    layout->placed_area_cells += option->solid->area_cells;
    ++layout->placement_count;
}

/* Decodes an order; a narrowed final sheet may spill, which loses on count. */
static bp_multisheet_result_t
s_construct(const bp_multisheet_args_t *args, uint32_t narrow_sheet,
    uint32_t narrow_width)
{
    bp_multisheet_result_t result = { 0 };
    uint32_t index;

    BP_REQUIRE(args);
    BP_REQUIRE(narrow_width > 0U);
    BP_REQUIRE(narrow_width <= args->sheet->width_cells);
    result.unplaced_count = args->part_count;
    memset(args->scratch_assigned, 0, args->part_count);
    for (index = 0U; index < args->part_count; ++index) {
        const bp_part_t *part = &args->parts[index];
        uint32_t orientation;
        int fits = 0;

        for (orientation = 0U; orientation < part->orientation_count; ++orientation)
            if (bp_mask_in_bounds(args->sheet,
                part->orientations[orientation].collision, 0U, 0U))
                fits = 1;
        if (!fits) {
            args->scratch_assigned[index] = 1U;
            ++result.oversized_count;
        }
    }

    while (result.sheet_count < args->max_sheets
        && result.unplaced_count > result.oversized_count) {
        bp_sheet_layout_t layout = { .first_placement = result.placed_count };
        uint32_t width = result.sheet_count == narrow_sheet
            ? narrow_width : args->sheet->width_cells;
        bp_bitmap_t sheet;

        /* Retry at full width if a width trial would otherwise emit an empty sheet. */
        for (;;) {
            sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
                .width_cells = width,
                .height_cells = args->sheet->height_cells,
                .words = args->sheet->words,
                .word_capacity = args->sheet->word_capacity
            });
            for (index = 0U; index < args->part_count; ++index) {
                uint32_t part_index = args->scratch_order[index];
                bp_placement_t spot;

                if (args->scratch_assigned[part_index])
                    continue;
                if (!s_place(args, &sheet, part_index, &spot))
                    continue;
                s_apply(args, &sheet, &layout, &spot);
                args->scratch_placements[result.placed_count++] = spot;
                args->scratch_assigned[part_index] = 1U;
                --result.unplaced_count;
            }
            if (layout.placement_count > 0U || width == args->sheet->width_cells)
                break;
            width = args->sheet->width_cells;
        }
        BP_REQUIRE(layout.placement_count > 0U);
        BP_REQUIRE(bp_bitmap_occupied_count(&sheet) == layout.placed_area_cells);
        args->scratch_sheets[result.sheet_count++] = layout;
        result.last_box_area_cells = (uint64_t) layout.width_cells * layout.height_cells;
    }
    result.status = result.unplaced_count == 0U
        ? BP_MULTISHEET_COMPLETE : BP_MULTISHEET_PARTIAL;
    return result;
}

/* Compares complete candidate layouts before exposing any of their buffers. */
static int
s_better(const bp_multisheet_args_t *args, const bp_multisheet_result_t *candidate,
    const bp_multisheet_result_t *best)
{
    const bp_sheet_layout_t *a;
    const bp_sheet_layout_t *b;
    uint64_t a_sides;
    uint64_t b_sides;

    BP_REQUIRE(args);
    BP_REQUIRE(candidate);
    BP_REQUIRE(best);
    if (candidate->unplaced_count != best->unplaced_count)
        return candidate->unplaced_count < best->unplaced_count;
    if (candidate->sheet_count != best->sheet_count)
        return candidate->sheet_count < best->sheet_count;
    if (candidate->last_box_area_cells != best->last_box_area_cells)
        return candidate->last_box_area_cells < best->last_box_area_cells;
    if (best->sheet_count == 0U)
        return 0;
    a = &args->scratch_sheets[candidate->sheet_count - 1U];
    b = &args->sheets[best->sheet_count - 1U];
    a_sides = (uint64_t) a->width_cells + a->height_cells;
    b_sides = (uint64_t) b->width_cells + b->height_cells;
    if (a_sides != b_sides)
        return a_sides < b_sides;
    return a->height_cells < b->height_cells;
}

/* Commits a completed candidate and the order needed to reproduce it. */
static void
s_save(const bp_multisheet_args_t *args, const bp_multisheet_result_t *result)
{
    BP_REQUIRE(args);
    BP_REQUIRE(result);
    memcpy(args->placements, args->scratch_placements,
        result->placed_count * sizeof args->placements[0]);
    memcpy(args->sheets, args->scratch_sheets,
        result->sheet_count * sizeof args->sheets[0]);
    memcpy(args->scratch_best_order, args->scratch_order,
        args->part_count * sizeof args->scratch_order[0]);
}

/* Advances an explicitly owned, nonzero xorshift64star state. */
static uint32_t
s_random_below(uint64_t *state, uint32_t bound)
{
    uint64_t value;

    BP_REQUIRE(state);
    BP_REQUIRE(*state != 0U);
    BP_REQUIRE(bound > 0U);
    value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return (uint32_t) ((value * UINT64_C(2685821657736338717)) % bound);
}

/* Searches assignments and final-sheet aspect ratios, retaining the best. */
bp_multisheet_result_t
bp_multisheet_solve(bp_multisheet_args_t args)
{
    bp_multisheet_result_t best;
    uint64_t random_state = args.seed ? args.seed : UINT64_C(0x9e3779b97f4a7c15);
    uint32_t best_width;
    uint32_t iteration;
    uint32_t improvements = 0U;
    clock_t started = clock();

    s_require_args(&args);
    best_width = args.sheet->width_cells;
    s_order(&args);
    best = s_construct(&args, UINT32_MAX, best_width);
    s_save(&args, &best);
    for (iteration = 0U; iteration < args.iterations && best.sheet_count > 0U;
        ++iteration) {
        bp_multisheet_result_t candidate;
        uint32_t width = best_width;
        uint32_t narrow_sheet = best.sheet_count - 1U;

        if (args.time_budget_seconds > 0.0
            && (double) (clock() - started) / CLOCKS_PER_SEC >= args.time_budget_seconds)
            break;
        memcpy(args.scratch_order, args.scratch_best_order,
            args.part_count * sizeof args.scratch_order[0]);
        if ((iteration % 2U) != 0U) {
            uint32_t trial = iteration / 2U;

            /* Cover aspect ratios first, then refine with seeded width samples. */
            width = trial < 16U
                ? 1U + (uint32_t) ((uint64_t) (args.sheet->width_cells - 1U)
                    * ((5U * trial + 7U) % 16U) / 15U)
                : 1U + s_random_below(&random_state, args.sheet->width_cells);
        } else {
            uint32_t count = (iteration % 4U) == 0U ? args.part_count : 2U;
            uint32_t index;

            /* Full restarts escape local order traps; swaps refine the incumbent. */
            for (index = count; index > 1U; --index) {
                uint32_t a = count == args.part_count ? index - 1U
                    : s_random_below(&random_state, args.part_count);
                uint32_t b = s_random_below(&random_state,
                    count == args.part_count ? index : args.part_count);
                uint32_t value = args.scratch_order[a];

                args.scratch_order[a] = args.scratch_order[b];
                args.scratch_order[b] = value;
            }
        }
        candidate = s_construct(&args, narrow_sheet, width);
        if (s_better(&args, &candidate, &best)) {
            best_width = candidate.sheet_count < best.sheet_count
                ? args.sheet->width_cells : width;
            best = candidate;
            s_save(&args, &best);
            ++improvements;
        }
    }
    best.iterations_run = iteration;
    best.improvements = improvements;
    best.seconds = (double) (clock() - started) / CLOCKS_PER_SEC;
    bp_bitmap_clear(args.sheet);
    return best;
}
