#pragma once

#include "bp_bitmap.h"
#include "bp_mask.h"

#include <stdint.h>

typedef enum {
    /* Cell-by-cell scan. Slow by design; it is the correctness oracle. */
    BP_PLACE_REFERENCE = 0,
    /* Same answer, skipping the stretch a blocker proves infeasible. */
    BP_PLACE_JUMP = 1
} bp_place_mode_t;

typedef struct {
    int found;
    uint32_t x_cells;
    uint32_t y_cells;
    uint64_t candidate_checks;
} bp_place_result_t;

/*
 * Finds the bottom-left-most free position for the mask: the smallest y, and
 * among those the smallest x. A mask that fits nowhere is an expected result,
 * reported as found == 0, not a contract violation.
 *
 * Both modes must return the same position for the same input; only the
 * amount of work differs. That equivalence is what the tests check, and it is
 * why the reference mode stays in the code rather than being deleted once the
 * fast path works.
 */
bp_place_result_t
bp_place_bottom_left(const bp_bitmap_t *sheet, const bp_mask_t *mask,
    bp_place_mode_t mode);
