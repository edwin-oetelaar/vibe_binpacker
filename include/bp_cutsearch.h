#pragma once

#include "bp_guillotine.h"

#include <stdint.h>

/*
 * Search for guillotine plans: local search over the order the packer takes
 * items in and over its turn preferences, with bp_guillotine_pack as the
 * decoder. Every candidate the search looks at is therefore a complete,
 * sawable plan; the search never repairs anything.
 *
 * Plans are compared lexicographically, as docs/plan_guillotine.md sets out:
 * more parts placed, then fewer plates, then a larger strip left over on the
 * last plate, which a saw shop puts back on the rack, then fewer cuts, which
 * is saw time.
 *
 * Moves: swap two items in the order, move one earlier or later, move an item
 * from the last plate or an unplaced one earlier, so the last plate empties,
 * and flip one item's turn preference. A candidate that is not worse than the
 * current plan replaces it, so the search walks plateaus; the best plan seen
 * is kept.
 *
 * With a fixed number of rounds and a fixed seed the result is the same on
 * every run. A time budget makes it depend on the machine, as in the other
 * search modes.
 */

typedef struct {
    /*
     * The packing to improve, as for bp_guillotine_pack. order and turn_bias
     * must be NULL: the search owns them. On return the outputs hold the
     * best plan found.
     */
    bp_guillotine_args_t pack;
    /* Rounds after the first packing; with a time budget, whichever ends first. */
    uint32_t iterations;
    /* CPU seconds, checked between rounds; 0 for none. */
    double time_budget_seconds;
    uint64_t seed;
    /* bp_cutsearch_order_words(item_count) words. */
    uint32_t *scratch_orders;
    uint32_t scratch_order_capacity;
    /* 3 * item_count bytes. */
    uint8_t *scratch_bias;
    uint32_t scratch_bias_capacity;
} bp_cutsearch_args_t;

typedef struct {
    /* The best plan's packing result, as in the outputs. */
    bp_guillotine_result_t best;
    uint32_t cut_count;
    /* Area of the full strip left on the last plate, in 0.01 mm squared. */
    uint64_t remnant_area_10um2;
    /* The default order's plan, for comparison. */
    uint32_t start_plate_count;
    uint32_t start_placed_count;
    uint32_t iterations_run;
    uint32_t improvements;
    double seconds;
} bp_cutsearch_result_t;

/* Returns the order scratch words a search over item_count items needs. */
uint32_t
bp_cutsearch_order_words(uint32_t item_count);

/* Searches for a better plan and leaves the best one in the outputs. */
bp_cutsearch_result_t
bp_cutsearch_run(bp_cutsearch_args_t args);
