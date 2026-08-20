#pragma once

#include "bp_bitmap.h"
#include "bp_mask.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* Cell-by-cell scan. Slow by design; it is the correctness oracle. */
    BP_PLACE_REFERENCE = 0,
    /* Same answer, skipping the stretch a blocker proves infeasible. */
    BP_PLACE_JUMP = 1,
    /*
     * Same answer again, computed rather than searched: for each height the
     * whole row of feasible positions is derived at once by intersecting run
     * masks, one per span of the part. Needs scratch.
     */
    BP_PLACE_EROSION = 2
} bp_place_mode_t;

typedef struct {
    int found;
    uint32_t x_cells;
    uint32_t y_cells;
    /*
     * How much work the mode did: positions examined for the two scans, span
     * evaluations for the erosion. The unit differs by mode on purpose,
     * because the modes do not do comparable units of work; compare wall
     * time across modes and this figure only within one.
     */
    uint64_t candidate_checks;
} bp_place_result_t;

typedef struct {
    const bp_bitmap_t *sheet;
    const bp_mask_t *mask;
    bp_place_mode_t mode;
    /* Required by BP_PLACE_EROSION only; may be NULL for the scan modes. */
    uint64_t *scratch;
    size_t scratch_capacity;
} bp_place_args_t;

/* Returns the scratch words BP_PLACE_EROSION needs on this sheet. */
size_t
bp_place_scratch_words(const bp_bitmap_t *sheet);

typedef struct {
    const bp_bitmap_t *sheet;
    const bp_mask_t *mask;
    uint32_t y_cells;
    uint64_t *scratch;
    size_t scratch_capacity;
} bp_place_feasible_args_t;

typedef struct {
    /* Number of positions; zero means the part does not fit at this height. */
    uint32_t count;
    /*
     * One bit per x: set where the mask's origin may sit. Borrowed from the
     * caller's scratch and valid only until the next call that uses it.
     * NULL when count is zero.
     */
    const uint64_t *positions;
    size_t stride_words;
    uint64_t candidate_checks;
} bp_place_feasible_result_t;

/*
 * Returns every position the mask fits at one height, as a bitmap over x.
 *
 * This is what the raster model buys that a continuous method cannot: the
 * complete placement space for a part, rather than the first spot a search
 * happens to reach. A layer that scores placements - by contact, by the shape
 * of the waste it leaves, by how much of the remnant stays rectangular - needs
 * all of them, and this produces them for the price of a few row operations
 * per span.
 *
 * For merely finding the bottom-left position it is the wrong tool, and
 * bp_bench says so plainly: BP_PLACE_JUMP is several times faster there,
 * because it usually rejects a candidate on its first word while this
 * computes whole rows.
 */
bp_place_feasible_result_t
bp_place_feasible_row(bp_place_feasible_args_t args);

/*
 * Finds the bottom-left-most free position for the mask: the smallest y, and
 * among those the smallest x. A mask that fits nowhere is an expected result,
 * reported as found == 0, not a contract violation.
 *
 * Every mode must return the same position for the same input; only the work
 * differs. That equivalence is what the tests check, and it is why the
 * reference mode stays in the code rather than being deleted once the fast
 * paths work.
 */
bp_place_result_t
bp_place_bottom_left(bp_place_args_t args);
