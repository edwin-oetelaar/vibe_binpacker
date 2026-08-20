#pragma once

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_place.h"

#include <stddef.h>
#include <stdint.h>

/*
 * How much each term counts when ranking a candidate position.
 *
 * Contact enters as a *fraction* of the part's own boundary, so it is
 * comparable between a small part and a large one. Weighing the raw count
 * instead lets a big part justify sitting hundreds of cells higher, which is
 * how a scoring rule quietly turns into a worse bottom-left.
 *
 * The height and left penalties are per cell, and contact_weight is therefore
 * best read as "how many cells of height a perfect fit is worth". Bottom-left
 * remains the backbone; contact only decides between positions that are
 * within a few cells of each other.
 *
 * A contact_weight of zero reduces the ranking to plain bottom-left, which is
 * what the tests use to show the two agree.
 */
typedef struct {
    double contact_weight;
    double height_penalty;
    double left_penalty;
} bp_score_weights_t;

/* The weights the search uses unless a caller overrides them. */
bp_score_weights_t
bp_score_default_weights(void);

/*
 * Counts the part's boundary cells that would touch occupied material or the
 * sheet edge if it were placed at (x, y). The position must be free.
 *
 * Only the cells directly beside, above and below each span are looked at, so
 * this costs a few word operations per span rather than anything proportional
 * to the part's area.
 */
uint32_t
bp_score_contact(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y);

/*
 * Returns the contact a part would have if it were surrounded on every side,
 * which is what bp_score_contact is measured against to get a fraction.
 */
uint32_t
bp_score_contact_capacity(const bp_mask_t *mask);

typedef struct {
    const bp_bitmap_t *sheet;
    const bp_mask_t *mask;
    bp_score_weights_t weights;
    /*
     * How many heights above the lowest feasible one to consider, and how
     * many positions to score within each. Both bound the work; a wider
     * search costs proportionally more and is what a time budget buys.
     */
    uint32_t height_window;
    uint32_t max_positions;
    /*
     * Chance in [0, 1) that a candidate position is passed over without being
     * ranked at all.
     *
     * A greedy recreate is a fixed point: lift parts out, put them back at
     * their best positions, and you get the layout you started from, so the
     * search never moves. Blinking a small fraction of candidates is the
     * cheapest known way out, and unlike a random insertion order it keeps the
     * big parts going down first, which is what makes greedy good.
     *
     * random_state is required when this is above zero, and is advanced.
     */
    double blink_rate;
    uint64_t *random_state;
    uint64_t *scratch;
    size_t scratch_capacity;
} bp_score_best_args_t;

typedef struct {
    int found;
    uint32_t x_cells;
    uint32_t y_cells;
    double score;
    uint64_t positions_examined;
} bp_score_best_result_t;

/*
 * Finds the best-scoring position for the mask.
 *
 * Heights are searched upwards until one has room, then that height and the
 * next few are ranked. Getting every position at a height is what makes this
 * affordable: bp_place_feasible_row produces them for a few row operations
 * per span, where scanning for the same set costs ten to thirty times more.
 *
 * A mask that fits nowhere is an expected result, reported as found == 0.
 */
bp_score_best_result_t
bp_score_best_position(bp_score_best_args_t args);
