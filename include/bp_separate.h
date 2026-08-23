#pragma once

#include "bp_nester.h"
#include "bp_penetration.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    bp_penetration_t *field;
    const bp_part_t *parts;
    uint32_t part_count;
    /* Indexed by part, updated in place as parts are moved. */
    bp_placement_t *placements;
    const uint8_t *placed;

    /* Parts must stay inside this region, anchored at the sheet origin. */
    uint32_t region_width_cells;
    uint32_t region_height_cells;

    uint32_t max_iterations;
    /* Candidate positions tried per move before the descent refines them. */
    uint32_t samples;
    /* Moves without progress before the attempt is given up. */
    uint32_t patience;

    uint64_t *random_state;
    /* One penalty per part; reset by this call. */
    uint32_t *scratch_weights;
    /* One overlap measurement per part, refreshed each move. */
    uint64_t *scratch_overlap;
    size_t scratch_capacity;
} bp_separate_args_t;

typedef struct {
    int resolved;
    uint64_t overlap_cells;
    uint32_t iterations_run;
    uint64_t probes;
} bp_separate_result_t;

/*
 * Drives the clearance violations of a layout to zero by moving one part at a
 * time.
 *
 * The field must hold each placed part's **true** geometry, added at the solid
 * origin. Probing happens with the **grown** footprint. That asymmetry is not
 * an optimisation, it is the only formulation that can reach zero: two parts
 * exactly one clearance apart each have the gap between them inside their own
 * grown footprint, so a field built from grown masks reports thousands of
 * cells of overlap for a layout that is perfectly legal. Measured on a
 * 36-part sheet, shrinking the band by a single cell appeared to create 7706
 * cells of conflict, of which all but a few hundred were this artefact.
 *
 * A part's violation is therefore how much *other* material lies inside its
 * grown footprint, and zero for every part means every pair is at least the
 * clearance apart.
 *
 * This is the idea the nesting literature settled on and it is
 * counter-intuitive: instead of forbidding overlap, allow it and minimise it.
 * Feasibility stops being a wall the search bounces off and becomes a quantity
 * it can descend, which is what lets a layout pass through arrangements no
 * sequence of legal placements could reach.
 *
 * Each move lifts the worst offender out, looks for somewhere it overlaps
 * less - random samples over the region, more of them near where it already
 * is, then a coordinate descent on the best of those - and puts it back. A
 * part that keeps colliding accumulates a penalty that makes it more likely to
 * be picked next, which is the guided part of guided local search: it stops
 * the same two parts shoving each other back and forth forever.
 *
 * The penalty steers which part moves, not where it goes. Weighting positions
 * per colliding pair would need a second field of weighted counts, and is a
 * refinement rather than a prerequisite.
 *
 * Returns resolved when the overlap reached zero, in which case the
 * placements are a legal layout. Failing to resolve is an expected outcome,
 * not a contract violation: the caller squeezed too hard and will roll back.
 */
bp_separate_result_t
bp_separate_resolve(bp_separate_args_t args);
