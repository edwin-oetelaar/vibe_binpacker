#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Sheet size planning: millimetres to cells, and whether the sheet buffers of
 * that raster fit the application budget. The loader and the GUI preview both
 * go through bp_sheet_plan, so what the GUI offers is exactly what the loader
 * accepts.
 *
 * A physical size alone says little about cost: halving the cell size
 * quadruples the cell count, and the loader keeps a uint32_t count and a
 * uint64_t running total per cell next to the bitboard. The limits below are
 * product guardrails on that cost, not measured hardware maxima, and they do
 * not bound total process memory: part masks, rotations and rasteriser scratch
 * come on top.
 *
 * Sizes typed by a user are external input, so a bad or oversized sheet is an
 * expected result here and is reported, never aborted, clamped or rounded to
 * something that would fit.
 */

typedef struct {
    /* Most cells allowed along either axis. */
    uint32_t max_cells_per_axis;
    /* Most cells allowed in the whole sheet. */
    uint64_t max_total_cells;
    /* Most bytes allowed for the sheet buffers, see bp_sheet_buffer_bytes. */
    uint64_t max_buffer_bytes;
} bp_sheet_limits_t;

typedef enum {
    BP_SHEET_OK = 0,
    /* A sheet dimension is zero, negative, NaN or infinite. */
    BP_SHEET_BAD_SIZE = 1,
    /* The cell size is zero, negative, NaN or infinite. */
    BP_SHEET_BAD_CELL_SIZE = 2,
    /* An axis holds less than one whole cell. */
    BP_SHEET_TOO_SMALL = 3,
    BP_SHEET_AXIS_OVER_LIMIT = 4,
    BP_SHEET_CELLS_OVER_LIMIT = 5,
    BP_SHEET_BYTES_OVER_LIMIT = 6
} bp_sheet_status_t;

typedef struct {
    double width_mm;
    double height_mm;
    double cell_size_mm;
    /* Every field positive; see bp_sheet_default_limits. */
    bp_sheet_limits_t limits;
} bp_sheet_plan_args_t;

/*
 * The raster a sheet size maps to. Each figure is filled as far as it could
 * be computed and zero beyond that: a rejected axis leaves the cell counts
 * zero, a rejected cell total still reports both axes and the total, and a
 * rejected byte budget reports everything. That is what a caller needs to
 * explain the rejection.
 */
typedef struct {
    bp_sheet_status_t status;
    /* The request as given, so a plan can be explained on its own. */
    double width_mm;
    double height_mm;
    double cell_size_mm;
    uint32_t width_cells;
    uint32_t height_cells;
    uint64_t total_cells;
    uint64_t buffer_bytes;
} bp_sheet_plan_t;

/*
 * Returns the default guardrails: 8192 cells per axis, 16 000 000 cells and
 * 256 MiB of sheet buffers. They admit 800 x 600 mm at 0.25 mm and
 * 1250 x 2500 mm at 0.5 mm, and reject 1250 x 2500 mm at 0.25 mm.
 */
bp_sheet_limits_t
bp_sheet_default_limits(void);

/*
 * Returns the bytes the loader allocates per sheet: the bitboard, the
 * penetration counts and running totals, and the erosion row scratch. Taken
 * from the real capacity functions, so word padding and the extra running
 * total per row are included; counting width times height is not enough.
 * Both sizes must be positive and the result must fit in size_t and in 64
 * bits; bp_sheet_plan only calls this once its cell limits guarantee that.
 */
uint64_t
bp_sheet_buffer_bytes(uint32_t width_cells, uint32_t height_cells);

/*
 * Converts a sheet size to cells and checks it against the limits. The sheet
 * rounds down, so the model is never larger than the real plate; parts keep
 * rounding up in the loader.
 *
 * The limits are configuration, not user input: each must be positive, and
 * max_total_cells must be small enough that the byte arithmetic cannot
 * overflow on this platform. Violating that aborts.
 */
bp_sheet_plan_t
bp_sheet_plan(bp_sheet_plan_args_t args);

/*
 * Returns the finest cell size from the fixed ladder 0.1, 0.2, 0.25, 0.5, 1,
 * 2, 5 and 10 mm at which the sheet fits the limits, or 0.0 when none does.
 * This is the suggestion shown with a rejection; nothing applies it
 * automatically. The sheet dimensions must be positive and finite.
 */
double
bp_sheet_suggest_cell_size_mm(double width_mm, double height_mm,
    bp_sheet_limits_t limits);

/* Smallest buffer bp_sheet_explain accepts; every message fits in it. */
#define BP_SHEET_EXPLAIN_CAPACITY 256U

/*
 * Writes one line explaining a plan, so the CLI and the GUI say the same
 * thing: for an accepted sheet the raster and the buffer size, for a
 * rejected one the reason, the figures behind it and the suggestion from
 * bp_sheet_suggest_cell_size_mm when there is one. limits must be the ones
 * the plan was made with. text_capacity must be at least
 * BP_SHEET_EXPLAIN_CAPACITY, and the text is always terminated.
 */
void
bp_sheet_explain(const bp_sheet_plan_t *plan, bp_sheet_limits_t limits,
    char *text, size_t text_capacity);

/* Returns a short human-readable form of a status, for error messages. */
const char *
bp_sheet_status_text(bp_sheet_status_t status);
