#pragma once

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_sheet.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BP_JOB_OK = 0,
    BP_JOB_CANNOT_OPEN = 1,
    BP_JOB_BAD_LINE = 2,
    BP_JOB_NO_SHEET = 3,
    BP_JOB_NO_PARTS = 4,
    BP_JOB_SHEET_TOO_SMALL = 5,
    BP_JOB_TOO_LARGE = 6,
    BP_JOB_OUT_OF_MEMORY = 7,
    /* An explicit sheet or cell size override is not positive and finite. */
    BP_JOB_BAD_OVERRIDE = 8
} bp_job_status_t;

/*
 * A loaded job: the sheet, the parts expanded to one entry per piece to cut,
 * and the scratch the nester needs. Everything here is owned by the job and
 * released by bp_job_destroy; treat the pointers as read-only.
 *
 * Rounding follows the rule the nesting literature calls inflating the items
 * and deflating the container: part dimensions round up and the sheet rounds
 * down, so the discrete model is never more optimistic than the real plate.
 */
typedef struct {
    /* The sheet and cell size in force, after any override. */
    double sheet_width_mm;
    double sheet_height_mm;
    double cell_size_mm;
    double clearance_mm;
    /*
     * What the file itself says, so a caller can show an override as one and
     * offer to go back to the file. file_cell_size_mm is the default 1.0 when
     * the file has no cell line.
     */
    double file_sheet_width_mm;
    double file_sheet_height_mm;
    double file_cell_size_mm;

    uint32_t sheet_width_cells;
    uint32_t sheet_height_cells;
    uint32_t clearance_cells;

    bp_bitmap_t sheet;
    const bp_part_t *parts;
    uint32_t part_count;
    /* The identifier from the job file, per part index, for reporting. */
    const uint32_t *labels;

    uint32_t *order;
    /* Row scratch sized for BP_PLACE_EROSION on this job's sheet. */
    uint64_t *scratch_rows;
    size_t scratch_row_capacity;
    /* Bookkeeping bp_search needs; unused by the plain nester. */
    uint8_t *placed;
    /* Penetration field storage for the compaction phase of bp_search. */
    uint32_t *scratch_counts;
    size_t scratch_count_capacity;
    uint64_t *scratch_prefix;
    size_t scratch_prefix_capacity;
    uint32_t *scratch_weights;
    uint64_t *scratch_overlap;
    uint8_t *scratch_placed;
    uint8_t *scratch_best_placed;
    bp_placement_t *scratch_best;
    bp_placement_t *placements;

    /* Owned storage. Nothing outside bp_job.c should touch these. */
    uint64_t *owned_sheet_words;
    uint32_t *owned_row_offsets;
    bp_span_t *owned_spans;
    bp_mask_t *owned_masks;
    bp_orientation_t *owned_orientations;
    bp_part_t *owned_parts;
    uint32_t *owned_labels;
} bp_job_t;

typedef struct {
    bp_job_status_t status;
    /* Line the parser stopped on, 0 when the failure is not line specific. */
    unsigned line_number;
    /* The job, or NULL when status is not BP_JOB_OK. */
    bp_job_t *job;
    /*
     * The sheet raster the load planned, so a caller can explain a rejected
     * size. Status is BP_SHEET_OK unless the sheet itself was refused, in
     * which case the load status is BP_JOB_SHEET_TOO_SMALL or
     * BP_JOB_TOO_LARGE. All zero when loading failed before the sheet was
     * planned. BP_JOB_TOO_LARGE with an OK plan means a part raster, not the
     * sheet, exceeded the budget.
     */
    bp_sheet_plan_t sheet_plan;
} bp_job_load_result_t;

/*
 * Reads a job file. The format is deliberately line based so the solver needs
 * no parser dependency; DXF and SVG import belong in the Python tooling that
 * writes these files.
 *
 *   sheet <width_mm> <height_mm>
 *   cell <size_mm>                     default 1.0
 *   clearance <mm>                     default 0.0
 *   rect <id> <quantity> <w_mm> <h_mm> <allow_quarter_turn>
 *
 * Blank lines and everything after a '#' are ignored. A positive
 * cell_size_override_mm replaces the file's cell line, which is how the
 * benchmark sweeps resolutions over one fixed job; it must not be negative.
 *
 * A malformed or unreadable file is an expected runtime failure and is
 * reported, not aborted.
 */
bp_job_load_result_t
bp_job_load(const char *path, double cell_size_override_mm);

typedef struct {
    /* Job file to read; must not be NULL. */
    const char *path;
    /*
     * Replaces the file's sheet line when both are nonzero; both zero keeps
     * the file's size. The file must still have a valid sheet line: an
     * override replaces it, it does not stand in for it.
     */
    double sheet_width_mm;
    double sheet_height_mm;
    /* Replaces the file's cell line when nonzero. */
    double cell_size_mm;
    /*
     * Budget for the sheet buffers and for the part raster scratch. All zero
     * means bp_sheet_default_limits(); otherwise every field is positive.
     */
    bp_sheet_limits_t limits;
} bp_job_load_args_t;

/*
 * Reads a job file with optional sheet and cell size overrides, applied after
 * parsing and before anything sized by the sheet is allocated, so every sheet
 * buffer matches the size actually used. The job file is never modified.
 *
 * Overrides come from a user, so one that is not positive and finite is
 * reported as BP_JOB_BAD_OVERRIDE rather than aborted, and a size outside the
 * limits as BP_JOB_SHEET_TOO_SMALL or BP_JOB_TOO_LARGE with the detail in
 * sheet_plan. Nothing is clamped or rounded to make it fit.
 *
 * bp_job_load is this with no sheet override and limits that only guard
 * against arithmetic overflow, so existing callers keep their behaviour.
 */
bp_job_load_result_t
bp_job_load_with(bp_job_load_args_t args);

/* Releases the job and clears the caller's pointer. */
void
bp_job_destroy(bp_job_t **job);

/* Returns a short human-readable form of a status, for error messages. */
const char *
bp_job_status_text(bp_job_status_t status);
