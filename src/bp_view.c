#include "bp_view.h"

#include "bp_contract.h"
#include "bp_svg.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Measures a sheet from true geometry, never from the reusable bitmap. */
static void
s_measure(bp_view_t *view, bp_sheet_layout_t *sheet)
{
    uint32_t index;
    uint32_t min_x = UINT32_MAX;
    uint32_t min_y = UINT32_MAX;
    uint32_t max_x = 0U;
    uint32_t max_y = 0U;

    BP_REQUIRE(view);
    BP_REQUIRE(sheet);
    BP_REQUIRE(sheet->placement_count > 0U);
    sheet->placed_area_cells = 0U;
    for (index = 0U; index < sheet->placement_count; ++index) {
        const bp_placement_t *spot = &view->placements[sheet->first_placement + index];
        const bp_part_t *part;
        const bp_orientation_t *option;
        uint32_t x;
        uint32_t y;

        BP_REQUIRE(spot->part_index < view->part_count);
        BP_REQUIRE(!view->placed[spot->part_index]);
        part = &view->parts[spot->part_index];
        BP_REQUIRE(spot->part_id == part->id);
        BP_REQUIRE(part->orientations);
        BP_REQUIRE(spot->orientation < part->orientation_count);
        option = &part->orientations[spot->orientation];
        bp_mask_require_valid(option->solid);
        BP_REQUIRE(option->solid_offset_x_cells <= view->sheet_width_cells);
        BP_REQUIRE(option->solid_offset_y_cells <= view->sheet_height_cells);
        BP_REQUIRE(spot->x_cells <= view->sheet_width_cells - option->solid_offset_x_cells);
        BP_REQUIRE(spot->y_cells <= view->sheet_height_cells - option->solid_offset_y_cells);
        x = spot->x_cells + option->solid_offset_x_cells;
        y = spot->y_cells + option->solid_offset_y_cells;
        BP_REQUIRE(option->solid->width_cells <= view->sheet_width_cells - x);
        BP_REQUIRE(option->solid->height_cells <= view->sheet_height_cells - y);
        if (x < min_x)
            min_x = x;
        if (y < min_y)
            min_y = y;
        if (x + option->solid->width_cells > max_x)
            max_x = x + option->solid->width_cells;
        if (y + option->solid->height_cells > max_y)
            max_y = y + option->solid->height_cells;
        sheet->placed_area_cells += option->solid->area_cells;
        view->placed[spot->part_index] = 1U;
    }
    sheet->min_x_cells = min_x;
    sheet->min_y_cells = min_y;
    sheet->width_cells = max_x - min_x;
    sheet->height_cells = max_y - min_y;
}

/* Normalises solver output into one presentation model and resets selection. */
bp_view_t
bp_view_publish(bp_view_publish_args_t args)
{
    bp_view_t view = { 0 };
    uint32_t offset = 0U;
    uint32_t index;

    BP_REQUIRE(args.parts);
    BP_REQUIRE(args.part_count > 0U);
    BP_REQUIRE(args.placement_count <= args.part_count);
    BP_REQUIRE(args.placements || args.placement_count == 0U);
    BP_REQUIRE(args.sheets);
    BP_REQUIRE(args.sheet_capacity >= args.sheet_count);
    BP_REQUIRE(args.sheet_count <= args.placement_count);
    BP_REQUIRE(args.sheet_count > 0U || args.placement_count == 0U);
    BP_REQUIRE(args.multiple_sheets || args.sheet_count <= 1U);
    BP_REQUIRE(args.placed);
    BP_REQUIRE(args.placed_capacity >= args.part_count);
    BP_REQUIRE(args.sheet_width_cells > 0U && args.sheet_height_cells > 0U);
    BP_REQUIRE(isfinite(args.cell_size_mm) && args.cell_size_mm > 0.0);
    view.parts = args.parts;
    view.labels = args.labels;
    view.part_count = args.part_count;
    view.placements = args.placements;
    view.placement_count = args.placement_count;
    view.sheets = args.sheets;
    view.sheet_count = args.sheet_count;
    view.sheet_width_cells = args.sheet_width_cells;
    view.sheet_height_cells = args.sheet_height_cells;
    view.cell_size_mm = args.cell_size_mm;
    view.multiple_sheets = args.multiple_sheets;
    view.placed = args.placed;
    memset(view.placed, 0, args.part_count);
    for (index = 0U; index < view.sheet_count; ++index) {
        bp_sheet_layout_t *sheet = &view.sheets[index];

        BP_REQUIRE(sheet->first_placement == offset);
        BP_REQUIRE(sheet->placement_count > 0U);
        BP_REQUIRE(sheet->placement_count <= view.placement_count - offset);
        s_measure(&view, sheet);
        offset += sheet->placement_count;
        view.placed_area_cells += sheet->placed_area_cells;
    }
    BP_REQUIRE(offset == view.placement_count);
    for (index = 0U; index < view.part_count; ++index) {
        const bp_part_t *part = &view.parts[index];
        uint32_t orientation;
        int fits = 0;

        BP_REQUIRE(part->orientations && part->orientation_count > 0U);
        for (orientation = 0U; orientation < part->orientation_count; ++orientation) {
            const bp_mask_t *mask = part->orientations[orientation].collision;

            BP_REQUIRE(mask);
            if (mask->width_cells <= view.sheet_width_cells
                && mask->height_cells <= view.sheet_height_cells)
                fits = 1;
        }
        if (!view.placed[index] && !fits)
            ++view.oversized_count;
    }
    if (view.sheet_count > 0U)
        bp_view_select(&view, 0U);
    return view;
}

/* Returns the selected range only when selection is meaningful. */
const bp_sheet_layout_t *
bp_view_active(const bp_view_t *view)
{
    BP_REQUIRE(view);
    BP_REQUIRE(view->parts);
    BP_REQUIRE(view->sheets);
    BP_REQUIRE(view->active_sheet < view->sheet_count);
    return &view->sheets[view->active_sheet];
}

/* Changing plates resets the local replay without marking the result dirty. */
void
bp_view_select(bp_view_t *view, uint32_t sheet_index)
{
    BP_REQUIRE(view);
    BP_REQUIRE(view->parts);
    BP_REQUIRE(sheet_index < view->sheet_count);
    view->active_sheet = sheet_index;
    view->visible_count = bp_view_active(view)->placement_count;
}

/* A local prefix affects rendering only, never exported geometry or scores. */
void
bp_view_set_visible(bp_view_t *view, uint32_t count)
{
    BP_REQUIRE(view);
    BP_REQUIRE(count <= bp_view_active(view)->placement_count);
    view->visible_count = count;
}

/* Translates a visible local index to the immutable global placement list. */
const bp_placement_t *
bp_view_placement(const bp_view_t *view, uint32_t local_index)
{
    const bp_sheet_layout_t *sheet;

    BP_REQUIRE(view);
    BP_REQUIRE(local_index < view->visible_count);
    sheet = bp_view_active(view);
    return &view->placements[sheet->first_placement + local_index];
}

/* Empty results have zero utilisation; otherwise all used plates count. */
double
bp_view_utilisation(const bp_view_t *view)
{
    BP_REQUIRE(view);
    BP_REQUIRE(view->parts);
    if (view->sheet_count == 0U)
        return 0.0;
    return (double) view->placed_area_cells / ((double) view->sheet_count
        * view->sheet_width_cells * view->sheet_height_cells);
}

/* Exports full plate ranges, committing saved state only after the final close. */
bp_view_export_result_t
bp_view_export(bp_view_export_args_t args)
{
    bp_view_export_result_t result = { .status = BP_VIEW_EXPORT_OK };
    size_t length;
    size_t stem;
    uint32_t index;

    BP_REQUIRE(args.view);
    BP_REQUIRE(args.view->parts);
    BP_REQUIRE(args.path);
    BP_REQUIRE(args.scratch_path);
    BP_REQUIRE(args.path_capacity > 0U);
    BP_REQUIRE(args.path != args.scratch_path);
    args.scratch_path[0] = '\0';
    if (args.view->sheet_count == 0U) {
        result.status = BP_VIEW_EXPORT_EMPTY;
        return result;
    }
    length = strlen(args.path);
    stem = args.view->multiple_sheets && length >= 4U
        && strcmp(args.path + length - 4U, ".svg") == 0 ? length - 4U : length;
    /* Reserve the largest uint32 suffix before writing any files. */
    if (stem >= args.path_capacity
        || (args.view->multiple_sheets && args.path_capacity - stem < 16U)) {
        result.status = BP_VIEW_EXPORT_PATH_TOO_LONG;
        return result;
    }
    memcpy(args.scratch_path, args.path, stem);
    args.scratch_path[stem] = '\0';
    for (index = 0U; index < args.view->sheet_count; ++index) {
        const bp_sheet_layout_t *sheet = &args.view->sheets[index];
        FILE *out;
        bp_svg_result_t written;
        int write_error;
        int closed;

        if (args.view->multiple_sheets)
            (void) snprintf(args.scratch_path + stem, args.path_capacity - stem,
                "-%03u.svg", (unsigned) index + 1U);
        out = fopen(args.scratch_path, "w");
        if (!out) {
            result.system_error = errno;
            result.status = BP_VIEW_EXPORT_WRITE_FAILED;
            return result;
        }
        errno = 0;
        written = bp_svg_write((bp_svg_write_args_t) {
            .out = out,
            .sheet_width_cells = args.view->sheet_width_cells,
            .sheet_height_cells = args.view->sheet_height_cells,
            .cell_size_mm = args.view->cell_size_mm,
            .parts = args.view->parts, .part_count = args.view->part_count,
            .placements = args.view->placements + sheet->first_placement,
            .placement_count = sheet->placement_count,
            .labels = args.view->labels, .draw_labels = 1
        });
        write_error = written.status == BP_SVG_OK ? 0 : errno;
        closed = fclose(out);
        if (closed != 0 || written.status != BP_SVG_OK) {
            result.system_error = write_error ? write_error : (closed != 0 ? errno : 0);
            result.status = BP_VIEW_EXPORT_WRITE_FAILED;
            return result;
        }
        ++result.written_count;
    }
    args.view->saved = 1;
    return result;
}
