#pragma once

#include "bp_nester.h"

/* Placements for one sheet occupy a contiguous range in the output array. */
typedef struct {
    uint32_t first_placement;
    uint32_t placement_count;
    /* Tight bounds of the solid geometry, excluding clearance and margins. */
    uint32_t min_x_cells;
    uint32_t min_y_cells;
    uint32_t width_cells;
    uint32_t height_cells;
    uint64_t placed_area_cells;
} bp_sheet_layout_t;

typedef enum {
    BP_MULTISHEET_COMPLETE = 0,
    BP_MULTISHEET_PARTIAL = 1
} bp_multisheet_status_t;

typedef struct {
    /* Reusable empty-sheet workspace; cleared on return, not a result bitmap. */
    bp_bitmap_t *sheet;
    const bp_part_t *parts;
    uint32_t part_count;
    uint32_t max_sheets;
    bp_nest_sort_t sort;
    bp_place_mode_t place_mode;
    uint32_t iterations;
    /* Soft CPU-time limit, checked between complete candidates; 0 = unlimited. */
    double time_budget_seconds;
    uint64_t seed;

    uint32_t *scratch_order;
    uint32_t *scratch_best_order;
    uint8_t *scratch_assigned;
    bp_placement_t *scratch_placements;
    bp_sheet_layout_t *scratch_sheets;
    uint64_t *scratch_rows;
    size_t scratch_row_capacity;

    bp_placement_t *placements;
    bp_sheet_layout_t *sheets;
    /* Every per-part buffer above must hold part_capacity entries. */
    size_t part_capacity;
    /* Both sheet arrays need min(max_sheets, part_count) entries. */
    size_t sheet_capacity;
} bp_multisheet_args_t;

typedef struct {
    bp_multisheet_status_t status;
    uint32_t sheet_count;
    uint32_t placed_count;
    uint32_t unplaced_count;
    /* Parts with no orientation fitting even a completely empty full sheet. */
    uint32_t oversized_count;
    uint64_t last_box_area_cells;
    uint32_t iterations_run;
    uint32_t improvements;
    double seconds;
} bp_multisheet_result_t;

/*
 * Packs identical sheets, carrying anything not placed onto the next sheet.
 * A partial result is expected when the sheet limit is reached or a part is
 * oversized. No empty sheets are returned, even when nothing fits.
 *
 * Objective, lexicographic: fewer unplaced parts, fewer sheets, then smaller
 * width * height of the last sheet's tight solid bounding box. Equal areas
 * prefer a smaller perimeter, then height. There is no optimality guarantee.
 *
 * The baseline uses the requested stable sort and bottom-left construction.
 * Search perturbs the order (which changes assignments between sheets) and
 * repacks the final sheet at trial container widths. This is a constructive
 * multi-sheet heuristic, not the single-sheet overlap-minimisation search.
 * Zero iterations returns the baseline; fixed seed and iterations reproduce
 * the result when the time limit is zero. More iterations never worsen it.
 *
 * IDs must be unique; orientations obey bp_orientation_t's containment and
 * clearance contract. Output part_index values always refer to the original
 * input array. Only placed_count placements and sheet_count sheets are valid.
 * All buffers are caller owned, mutually disjoint, and must outlive the call;
 * row scratch is required only in erosion mode. No allocation is performed.
 */
bp_multisheet_result_t
bp_multisheet_solve(bp_multisheet_args_t args);
