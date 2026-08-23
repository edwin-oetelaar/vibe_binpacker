#include "bp_place.h"

#include "bp_contract.h"

#include <string.h>

/* Rows of sheet-width scratch the erosion needs: free, run, temp, result. */
#define S_SCRATCH_ROWS 4U

/* Returns the scratch words BP_PLACE_EROSION needs on this sheet. */
size_t
bp_place_scratch_words(const bp_bitmap_t *sheet)
{
    BP_REQUIRE(sheet);
    return (size_t) S_SCRATCH_ROWS
        * bp_bitmap_stride_words(sheet->width_cells);
}

/*
 * Returns the smallest x that the first collision does not already rule out,
 * and reports through blocked whether the current x collides at all.
 *
 * If a span of the mask covers an occupied cell c, then for every x' up to
 * c - span->x_cells that same span still covers c, so no such x' can be free.
 * Jumping to c - span->x_cells + 1 therefore skips a provably blocked stretch
 * in one step.
 *
 * The scan stops at the first blocking span rather than taking the largest
 * threshold over all of them. Examining every span buys a longer jump in
 * principle, but bp_bench measures the difference at 0.4 % fewer candidate
 * positions for roughly seventy times the work per position, because most
 * candidates are rejected by their very first span. Early exit wins by two
 * orders of magnitude on wall time.
 */
static uint32_t
s_next_x(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y, int *blocked)
{
    uint32_t row;

    BP_REQUIRE(blocked);
    *blocked = 0;

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row];
            index < mask->row_offsets[row + 1U]; ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t blocker = 0U;

            if (bp_bitmap_span_last_occupied(sheet, y + row,
                x + span->x_cells, span->length_cells, &blocker)) {
                *blocked = 1;
                return blocker - span->x_cells + 1U;
            }
        }
    }
    return x + 1U;
}

/* Reports whether a row of words holds no set bits at all. */
static int
s_row_is_empty(const uint64_t *row, size_t stride_words)
{
    size_t word;

    for (word = 0U; word < stride_words; ++word)
        if (row[word] != UINT64_C(0))
            return 0;
    return 1;
}

/*
 * Returns the mask row with the most solid cells.
 *
 * The erosion intersects one run mask per span and can stop the moment the
 * intersection empties, so the order the rows are visited in decides how much
 * work a hopeless height costs. Starting with the widest row prunes fastest:
 * for a triangle it is the base that rules positions out, while the one-cell
 * tip rules out almost nothing.
 */
static uint32_t
s_most_constraining_row(const bp_mask_t *mask)
{
    uint32_t row;
    uint32_t best = 0U;
    uint32_t best_length = 0U;

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;
        uint32_t length = 0U;

        for (index = mask->row_offsets[row];
            index < mask->row_offsets[row + 1U]; ++index)
            length += mask->spans[index].length_cells;
        if (length > best_length) {
            best_length = length;
            best = row;
        }
    }
    return best;
}

/*
 * Derives every feasible x for one height at once.
 *
 * A span of length L fits at x wherever the sheet row has L free cells from
 * x onwards, which is exactly the run mask of that row. Shifting that mask
 * back by the span's own offset expresses it in terms of the part's origin,
 * and intersecting one such mask per span of every row leaves the positions
 * where the whole part fits. Positions that would run off the right edge fall
 * away by themselves, because cells past the sheet are not free.
 *
 * Returns 1 when at least one position survives, with the row of feasible
 * positions left in result.
 */
static int
s_feasible_row(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t y,
    uint32_t first_row, uint64_t *scratch, uint64_t *result,
    uint64_t *candidate_checks)
{
    size_t stride = sheet->stride_words;
    uint64_t *free_row = scratch;
    uint64_t *run = scratch + stride;
    uint64_t *temp = scratch + 2U * stride;
    uint32_t step;
    int started = 0;

    for (step = 0U; step < mask->height_cells; ++step) {
        /* Visit the most constraining row first, then the rest in order. */
        uint32_t row = step == 0U
            ? first_row
            : (step - 1U < first_row ? step - 1U : step);
        uint32_t index;

        bp_row_not(free_row, bp_bitmap_row_const(sheet, y + row), stride,
            sheet->width_cells);

        for (index = mask->row_offsets[row];
            index < mask->row_offsets[row + 1U]; ++index) {
            const bp_span_t *span = &mask->spans[index];
            const uint64_t *contribution;

            ++*candidate_checks;
            bp_row_run_mask(run, free_row, stride, sheet->width_cells,
                span->length_cells, temp);
            if (span->x_cells == 0U) {
                contribution = run;
            } else {
                /* Restate the mask in terms of the part's own origin. */
                bp_row_translate(temp, run, stride, sheet->width_cells,
                    -(int32_t) span->x_cells);
                contribution = temp;
            }

            if (started)
                bp_row_and(result, result, contribution, stride);
            else
                memcpy(result, contribution, stride * sizeof result[0]);
            started = 1;
            if (s_row_is_empty(result, stride))
                return 0;
        }
    }
    /* A valid mask has at least one span, so something was intersected. */
    BP_REQUIRE(started);
    return 1;
}

/* Returns every position the mask fits at one height. */
bp_place_feasible_result_t
bp_place_feasible_row(bp_place_feasible_args_t args)
{
    bp_place_feasible_result_t result = { 0U, NULL, 0U, 0U };
    uint64_t *feasible;

    BP_REQUIRE(args.sheet);
    BP_REQUIRE(args.scratch);
    BP_REQUIRE(args.scratch_capacity >= bp_place_scratch_words(args.sheet));
    bp_mask_require_valid(args.mask);

    result.stride_words = args.sheet->stride_words;
    if (!bp_mask_in_bounds(args.sheet, args.mask, 0U, args.y_cells))
        return result;

    feasible = args.scratch + 3U * args.sheet->stride_words;
    if (!s_feasible_row(args.sheet, args.mask, args.y_cells,
        s_most_constraining_row(args.mask), args.scratch, feasible,
        &result.candidate_checks))
        return result;

    result.count = bp_row_popcount(feasible, args.sheet->stride_words);
    BP_REQUIRE(result.count > 0U);
    result.positions = feasible;
    return result;
}

/* Finds the bottom-left-most free position for the mask on the sheet. */
bp_place_result_t
bp_place_bottom_left(bp_place_args_t args)
{
    bp_place_result_t result = { 0, 0U, 0U, 0U };
    const bp_bitmap_t *sheet = args.sheet;
    const bp_mask_t *mask = args.mask;
    uint32_t max_x;
    uint32_t max_y;
    uint32_t first_row = 0U;
    uint32_t y;

    BP_REQUIRE(sheet);
    BP_REQUIRE(args.mode == BP_PLACE_REFERENCE || args.mode == BP_PLACE_JUMP
        || args.mode == BP_PLACE_EROSION);
    bp_mask_require_valid(mask);
    if (args.mode == BP_PLACE_EROSION) {
        BP_REQUIRE(args.scratch);
        BP_REQUIRE(args.scratch_capacity >= bp_place_scratch_words(sheet));
    }

    /* A mask larger than the sheet fits nowhere; that is a result, not a bug. */
    if (!bp_mask_in_bounds(sheet, mask, 0U, 0U))
        return result;

    max_x = sheet->width_cells - mask->width_cells;
    max_y = sheet->height_cells - mask->height_cells;
    if (args.mode == BP_PLACE_EROSION)
        first_row = s_most_constraining_row(mask);

    for (y = 0U; y <= max_y; ++y) {
        uint32_t x = 0U;

        if (args.mode == BP_PLACE_EROSION) {
            uint64_t *feasible = args.scratch + 3U * sheet->stride_words;

            if (!s_feasible_row(sheet, mask, y, first_row, args.scratch,
                feasible, &result.candidate_checks))
                continue;
            if (!bp_row_first_set(feasible, sheet->stride_words, &x))
                continue;
            /* Positions past the right edge cannot survive the run masks. */
            BP_REQUIRE(x <= max_x);
            result.found = 1;
            result.x_cells = x;
            result.y_cells = y;
            return result;
        }

        while (x <= max_x) {
            ++result.candidate_checks;
            if (args.mode == BP_PLACE_REFERENCE) {
                if (!bp_mask_overlaps(sheet, mask, x, y)) {
                    result.found = 1;
                    result.x_cells = x;
                    result.y_cells = y;
                    return result;
                }
                ++x;
            } else {
                int blocked = 0;
                uint32_t next = s_next_x(sheet, mask, x, y, &blocked);

                if (!blocked) {
                    result.found = 1;
                    result.x_cells = x;
                    result.y_cells = y;
                    return result;
                }
                x = next;
            }
        }
    }
    return result;
}
