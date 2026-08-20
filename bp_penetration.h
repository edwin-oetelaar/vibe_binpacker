#pragma once

#include "bp_mask.h"

#include <stddef.h>
#include <stdint.h>

/*
 * How much material sits on each cell, so parts may be allowed to overlap and
 * the overlap measured. A bitmap cannot do this: two parts on one cell set the
 * same bit, and lifting either one would erase the other.
 *
 * Each row also carries a running total, so the material under any run of
 * cells is one subtraction rather than a walk. That is what makes probing a
 * candidate position cost one step per span of the part instead of one per
 * cell of its area, which is the difference between a search that can sample
 * a hundred positions per move and one that cannot.
 *
 * The price is that occupying a run costs a byte per cell where the bitboard
 * costs a word per sixty-four. Maintenance therefore dominates: adding and
 * removing a part is the expensive half of a move, probing is the cheap half.
 *
 * Material is added with a *weight* rather than simply counted. That is what
 * makes guided local search possible: a part that keeps getting in the way can
 * be made to weigh more, so the cost of sitting on it rises and the search
 * stops treating every obstruction as equally bad. With every weight at one
 * the field is a plain overlap count.
 *
 * All storage is caller owned.
 */
typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    /* Summed weight of everything covering each cell. */
    uint32_t *counts;
    /* height_cells rows of width_cells + 1 running totals. */
    uint64_t *prefix;
} bp_penetration_t;

typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    uint32_t *counts;
    size_t count_capacity;
    uint64_t *prefix;
    size_t prefix_capacity;
} bp_penetration_init_args_t;

/* Returns the caller-owned capacities a field of this size needs. */
size_t
bp_penetration_count_capacity(uint32_t width_cells, uint32_t height_cells);

size_t
bp_penetration_prefix_capacity(uint32_t width_cells, uint32_t height_cells);

/* Initialises an empty field over caller-owned storage. */
bp_penetration_t
bp_penetration_init(bp_penetration_init_args_t args);

/* Empties the field so it can be reused without reallocating. */
void
bp_penetration_clear(bp_penetration_t *field);

/*
 * Adds one part's material at (x, y) with the given weight, which must be
 * positive. Adding the same part again at the same spot raises its weight by
 * that much, which is how a penalty is applied without moving anything.
 */
void
bp_penetration_add(bp_penetration_t *field, const bp_mask_t *mask, uint32_t x,
    uint32_t y, uint32_t weight);

/*
 * Takes one part's material away again. Every cell it covers must currently
 * be covered at least once, which is a contract: removing a part that is not
 * there is a bookkeeping bug, not a runtime condition.
 */
void
bp_penetration_remove(bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y, uint32_t weight);

/*
 * Returns the summed weight of everything sitting under the mask at (x, y).
 * With the part itself removed first, that is what it would run into if
 * placed there.
 */
uint64_t
bp_penetration_probe(const bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y);
