#include "bp_place.h"

#include "bp_contract.h"

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

/* Finds the bottom-left-most free position for the mask on the sheet. */
bp_place_result_t
bp_place_bottom_left(const bp_bitmap_t *sheet, const bp_mask_t *mask,
    bp_place_mode_t mode)
{
    bp_place_result_t result = { 0, 0U, 0U, 0U };
    uint32_t max_x;
    uint32_t max_y;
    uint32_t y;

    BP_REQUIRE(sheet);
    BP_REQUIRE(mode == BP_PLACE_REFERENCE || mode == BP_PLACE_JUMP);
    bp_mask_require_valid(mask);

    /* A mask larger than the sheet fits nowhere; that is a result, not a bug. */
    if (!bp_mask_in_bounds(sheet, mask, 0U, 0U))
        return result;

    max_x = sheet->width_cells - mask->width_cells;
    max_y = sheet->height_cells - mask->height_cells;

    for (y = 0U; y <= max_y; ++y) {
        uint32_t x = 0U;

        while (x <= max_x) {
            ++result.candidate_checks;
            if (mode == BP_PLACE_REFERENCE) {
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
