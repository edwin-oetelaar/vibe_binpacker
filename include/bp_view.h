#pragma once

#include "bp_multisheet.h"

/*
 * A published packing, independent of ImGui and of the solver's bitmap.
 * All pointers are borrowed and must outlive the view. Repacking invalidates
 * the old view: publish again after the solver finishes. A zeroed view means
 * no result yet; a published view may legitimately have zero used sheets.
 */
typedef struct {
    const bp_part_t *parts;
    const uint32_t *labels;
    uint32_t part_count;
    const bp_placement_t *placements;
    uint32_t placement_count;
    bp_sheet_layout_t *sheets;
    uint32_t sheet_count;
    uint32_t sheet_width_cells;
    uint32_t sheet_height_cells;
    double cell_size_mm;
    /* One flag per original part, rebuilt from placements rather than solver scratch. */
    uint8_t *placed;
    uint64_t placed_area_cells;
    uint32_t oversized_count;
    uint32_t active_sheet;
    uint32_t visible_count;
    int multiple_sheets;
    int saved;
} bp_view_t;

typedef struct {
    const bp_part_t *parts;
    const uint32_t *labels;
    uint32_t part_count;
    const bp_placement_t *placements;
    uint32_t placement_count;
    /* Input ranges must be contiguous, nonempty and cover every placement. */
    bp_sheet_layout_t *sheets;
    uint32_t sheet_count;
    size_t sheet_capacity;
    uint8_t *placed;
    size_t placed_capacity;
    uint32_t sheet_width_cells;
    uint32_t sheet_height_cells;
    double cell_size_mm;
    int multiple_sheets;
} bp_view_publish_args_t;

/*
 * Publishes a complete or partial result and recomputes tight solid bounds,
 * area and identity flags. Ranges, part indices and orientations must be
 * valid; no part may occur twice. Input placements must already be legal.
 * Sheet and flag arrays are writable, caller owned and mutually disjoint.
 * Single-sheet mode accepts zero or one sheet. Selection starts at plate 1,
 * fully visible, and a new result is unsaved. Allocates nothing.
 */
bp_view_t
bp_view_publish(bp_view_publish_args_t args);

/* Selects an existing zero-based sheet and resets its local step to full. */
void
bp_view_select(bp_view_t *view, uint32_t sheet_index);

/* Returns the selected sheet; the view must contain at least one sheet. */
const bp_sheet_layout_t *
bp_view_active(const bp_view_t *view);

/* Sets a plate-local prefix length, in [0, active sheet's placement_count]. */
void
bp_view_set_visible(bp_view_t *view, uint32_t count);

/* Returns one visible plate-local placement, preserving its original identity. */
const bp_placement_t *
bp_view_placement(const bp_view_t *view, uint32_t local_index);

/* Total material / all used plate area; an empty published result returns 0. */
double
bp_view_utilisation(const bp_view_t *view);

typedef enum {
    BP_VIEW_EXPORT_OK = 0,
    BP_VIEW_EXPORT_EMPTY = 1,
    BP_VIEW_EXPORT_PATH_TOO_LONG = 2,
    BP_VIEW_EXPORT_WRITE_FAILED = 3
} bp_view_export_status_t;

typedef struct {
    bp_view_export_status_t status;
    uint32_t written_count;
    /* Captured errno for WRITE_FAILED, or 0 when the C library supplied none. */
    int system_error;
} bp_view_export_result_t;

typedef struct {
    bp_view_t *view;
    const char *path;
    /* Writable, nonaliasing path scratch. Last attempted filename on return. */
    char *scratch_path;
    size_t path_capacity;
} bp_view_export_args_t;

/*
 * Exports the full result, regardless of selection and local step. Single
 * mode writes path verbatim; multi mode writes <stem>-001.svg, etc., even
 * when only one plate is used. A trailing .svg is removed from the stem.
 * Marks saved only after every file was written AND closed successfully.
 * On failure, already completed files remain and written_count identifies
 * that prefix. Retrying overwrites the same names; no batch atomicity is
 * promised. The published result and its previous saved flag remain valid.
 * system_error captures the open/write/close failure so callers need not read
 * ambient errno after reporting or cleanup has overwritten it.
 */
bp_view_export_result_t
bp_view_export(bp_view_export_args_t args);
