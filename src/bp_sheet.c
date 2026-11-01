#include "bp_sheet.h"

#include "bp_bitmap.h"
#include "bp_contract.h"
#include "bp_penetration.h"
#include "bp_place.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S_MIB (1024.0 * 1024.0)

/* Returns the default guardrails documented in the header. */
bp_sheet_limits_t
bp_sheet_default_limits(void)
{
    return (bp_sheet_limits_t) {
        .max_cells_per_axis = 8192U,
        .max_total_cells = UINT64_C(16000000),
        .max_buffer_bytes = UINT64_C(256) * 1024U * 1024U
    };
}

/* Returns the bytes the loader allocates per sheet of this raster. */
uint64_t
bp_sheet_buffer_bytes(uint32_t width_cells, uint32_t height_cells)
{
    /*
     * bp_place_scratch_words reads only the width of the sheet it is given,
     * so a bitmap header without storage is enough to ask it.
     */
    bp_bitmap_t probe = { .width_cells = width_cells,
        .height_cells = height_cells };

    BP_REQUIRE(width_cells > 0U);
    BP_REQUIRE(height_cells > 0U);
    BP_REQUIRE((uint64_t) width_cells * height_cells <= UINT64_MAX / 32U);
    probe.stride_words = bp_bitmap_stride_words(width_cells);

    return (uint64_t) bp_bitmap_required_words(width_cells, height_cells)
            * sizeof (uint64_t)
        + (uint64_t) bp_penetration_count_capacity(width_cells, height_cells)
            * sizeof (uint32_t)
        + (uint64_t) bp_penetration_prefix_capacity(width_cells, height_cells)
            * sizeof (uint64_t)
        + (uint64_t) bp_place_scratch_words(&probe) * sizeof (uint64_t);
}

/* Converts a sheet size to cells and checks it against the limits. */
bp_sheet_plan_t
bp_sheet_plan(bp_sheet_plan_args_t args)
{
    bp_sheet_plan_t plan = { .status = BP_SHEET_OK, .width_mm = args.width_mm,
        .height_mm = args.height_mm, .cell_size_mm = args.cell_size_mm };
    double width;
    double height;

    BP_REQUIRE(args.limits.max_cells_per_axis > 0U);
    BP_REQUIRE(args.limits.max_total_cells > 0U);
    BP_REQUIRE(args.limits.max_buffer_bytes > 0U);
    /*
     * A row of running totals is one longer than the row, so the largest
     * element count any buffer needs is max_total_cells plus one axis. Both
     * that and the byte sum must be representable for the checks below to
     * mean anything.
     */
    BP_REQUIRE(args.limits.max_total_cells
        <= (uint64_t) SIZE_MAX - args.limits.max_cells_per_axis);
    BP_REQUIRE(args.limits.max_total_cells <= UINT64_MAX / 32U);

    if (!isfinite(args.width_mm) || !isfinite(args.height_mm)
        || !(args.width_mm > 0.0) || !(args.height_mm > 0.0)) {
        plan.status = BP_SHEET_BAD_SIZE;
        return plan;
    }
    if (!isfinite(args.cell_size_mm) || !(args.cell_size_mm > 0.0)) {
        plan.status = BP_SHEET_BAD_CELL_SIZE;
        return plan;
    }

    /* Same tolerance as the loader, so 1250 / 0.5 is 2500 and not 2499. */
    width = floor(args.width_mm / args.cell_size_mm + 1e-9);
    height = floor(args.height_mm / args.cell_size_mm + 1e-9);
    if (width < 1.0 || height < 1.0) {
        plan.status = BP_SHEET_TOO_SMALL;
        return plan;
    }
    /* Also catches a quotient that overflowed to infinity. */
    if (width > (double) args.limits.max_cells_per_axis
        || height > (double) args.limits.max_cells_per_axis) {
        plan.status = BP_SHEET_AXIS_OVER_LIMIT;
        return plan;
    }

    plan.width_cells = (uint32_t) width;
    plan.height_cells = (uint32_t) height;
    plan.total_cells = (uint64_t) plan.width_cells * plan.height_cells;
    if (plan.total_cells > args.limits.max_total_cells) {
        plan.status = BP_SHEET_CELLS_OVER_LIMIT;
        return plan;
    }

    plan.buffer_bytes = bp_sheet_buffer_bytes(plan.width_cells,
        plan.height_cells);
    if (plan.buffer_bytes > args.limits.max_buffer_bytes)
        plan.status = BP_SHEET_BYTES_OVER_LIMIT;
    return plan;
}

/* Returns the finest ladder cell size at which the sheet fits the limits. */
double
bp_sheet_suggest_cell_size_mm(double width_mm, double height_mm,
    bp_sheet_limits_t limits)
{
    static const double ladder_mm[] = {
        0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0
    };
    size_t index;

    BP_REQUIRE(isfinite(width_mm) && width_mm > 0.0);
    BP_REQUIRE(isfinite(height_mm) && height_mm > 0.0);

    for (index = 0U; index < sizeof ladder_mm / sizeof ladder_mm[0]; ++index) {
        bp_sheet_plan_t plan = bp_sheet_plan((bp_sheet_plan_args_t) {
            .width_mm = width_mm,
            .height_mm = height_mm,
            .cell_size_mm = ladder_mm[index],
            .limits = limits
        });

        if (plan.status == BP_SHEET_OK)
            return ladder_mm[index];
    }
    return 0.0;
}

/*
 * Appends the cell size suggestion to a rejection, or says that none of the
 * ladder fits. Only called for a valid sheet size.
 */
static void
s_append_suggestion(const bp_sheet_plan_t *plan, bp_sheet_limits_t limits,
    char *text, size_t text_capacity)
{
    size_t used = strlen(text);
    double suggestion;

    BP_REQUIRE(used < text_capacity);
    suggestion = bp_sheet_suggest_cell_size_mm(plan->width_mm,
        plan->height_mm, limits);
    if (suggestion > 0.0)
        (void) snprintf(text + used, text_capacity - used,
            "; try %g mm cells", suggestion);
    else
        (void) snprintf(text + used, text_capacity - used,
            "; no cell size up to 10 mm fits");
}

/* Writes one line explaining a plan. */
void
bp_sheet_explain(const bp_sheet_plan_t *plan, bp_sheet_limits_t limits,
    char *text, size_t text_capacity)
{
    BP_REQUIRE(plan);
    BP_REQUIRE(text);
    BP_REQUIRE(text_capacity >= BP_SHEET_EXPLAIN_CAPACITY);

    /* %.6g keeps every figure short enough that the line always fits. */
    switch (plan->status) {
    case BP_SHEET_OK:
        BP_REQUIRE(plan->width_cells > 0U && plan->height_cells > 0U);
        (void) snprintf(text, text_capacity,
            "raster %u x %u cells, sheet buffers about %.2f MiB",
            (unsigned) plan->width_cells, (unsigned) plan->height_cells,
            (double) plan->buffer_bytes / S_MIB);
        return;
    case BP_SHEET_BAD_SIZE:
    case BP_SHEET_BAD_CELL_SIZE:
        (void) snprintf(text, text_capacity, "%s",
            bp_sheet_status_text(plan->status));
        return;
    case BP_SHEET_TOO_SMALL:
        (void) snprintf(text, text_capacity,
            "%.6g x %.6g mm holds less than one %.6g mm cell along an axis",
            plan->width_mm, plan->height_mm, plan->cell_size_mm);
        return;
    case BP_SHEET_AXIS_OVER_LIMIT:
        (void) snprintf(text, text_capacity,
            "%.6g x %.6g mm at %.6g mm is %.6g x %.6g cells, more than %u "
            "along an axis",
            plan->width_mm, plan->height_mm, plan->cell_size_mm,
            floor(plan->width_mm / plan->cell_size_mm + 1e-9),
            floor(plan->height_mm / plan->cell_size_mm + 1e-9),
            (unsigned) limits.max_cells_per_axis);
        break;
    case BP_SHEET_CELLS_OVER_LIMIT:
        (void) snprintf(text, text_capacity,
            "%.6g x %.6g mm at %.6g mm is %llu cells, more than %llu",
            plan->width_mm, plan->height_mm, plan->cell_size_mm,
            (unsigned long long) plan->total_cells,
            (unsigned long long) limits.max_total_cells);
        break;
    case BP_SHEET_BYTES_OVER_LIMIT:
        (void) snprintf(text, text_capacity,
            "%.6g x %.6g mm at %.6g mm needs %.2f MiB of sheet buffers, "
            "more than %.2f MiB",
            plan->width_mm, plan->height_mm, plan->cell_size_mm,
            (double) plan->buffer_bytes / S_MIB,
            (double) limits.max_buffer_bytes / S_MIB);
        break;
    default:
        BP_REQUIRE(0);
    }
    s_append_suggestion(plan, limits, text, text_capacity);
}

/* Returns a short human-readable form of a status. */
const char *
bp_sheet_status_text(bp_sheet_status_t status)
{
    switch (status) {
    case BP_SHEET_OK:
        return "ok";
    case BP_SHEET_BAD_SIZE:
        return "sheet size must be positive and finite";
    case BP_SHEET_BAD_CELL_SIZE:
        return "cell size must be positive and finite";
    case BP_SHEET_TOO_SMALL:
        return "sheet is smaller than one cell";
    case BP_SHEET_AXIS_OVER_LIMIT:
        return "sheet has too many cells along one axis";
    case BP_SHEET_CELLS_OVER_LIMIT:
        return "sheet has too many cells";
    case BP_SHEET_BYTES_OVER_LIMIT:
        return "sheet buffers exceed the memory budget";
    default:
        return "unknown status";
    }
}
