#include "bp_raster.h"

#include "bp_contract.h"

#include <math.h>
#include <string.h>

/* Returns the smallest cell grid covering the polygon's bounding box. */
bp_raster_extent_t
bp_raster_extent(const bp_polygon_t *polygon, double cell_size_mm)
{
    bp_raster_extent_t extent;
    bp_box_t box;
    double width_cells;
    double height_cells;

    BP_REQUIRE(cell_size_mm > 0.0);
    box = bp_polygon_bounds(polygon);

    extent.origin_mm.x_mm = box.min_x_mm;
    extent.origin_mm.y_mm = box.min_y_mm;
    /* The tiny bias keeps a size that is an exact multiple off the next cell. */
    width_cells = ceil((box.max_x_mm - box.min_x_mm) / cell_size_mm - 1e-9);
    height_cells = ceil((box.max_y_mm - box.min_y_mm) / cell_size_mm - 1e-9);

    BP_REQUIRE(width_cells < 1e8);
    BP_REQUIRE(height_cells < 1e8);
    extent.width_cells = width_cells < 1.0 ? 1U : (uint32_t) width_cells;
    extent.height_cells = height_cells < 1.0 ? 1U : (uint32_t) height_cells;
    return extent;
}

/* Returns the crossing scratch a polygon needs: one entry per edge. */
size_t
bp_raster_scratch_crossings(const bp_polygon_t *polygon)
{
    return bp_polygon_point_count(polygon);
}

/* Sorts the crossing list in place; the lists are short, so insertion wins. */
static void
s_sort(double *values, size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        double value = values[index];
        size_t insert = index;

        while (insert > 0U && values[insert - 1U] > value) {
            values[insert] = values[insert - 1U];
            --insert;
        }
        values[insert] = value;
    }
}

/* Occupies one cell if it lies inside the target, ignoring it otherwise. */
static void
s_mark(bp_bitmap_t *target, int64_t x, int64_t y)
{
    if (x < 0 || y < 0)
        return;
    if (x >= (int64_t) target->width_cells || y >= (int64_t) target->height_cells)
        return;
    bp_bitmap_set_span(target, (uint32_t) y, (uint32_t) x, 1U);
}

/*
 * Marks every cell the segment passes through, by walking the grid boundary
 * by boundary in the order they are crossed. Endpoints outside the target are
 * fine: those steps are simply not marked.
 */
static void
s_trace_edge(bp_bitmap_t *target, bp_point_t from, bp_point_t to)
{
    int64_t x = (int64_t) floor(from.x_mm);
    int64_t y = (int64_t) floor(from.y_mm);
    int64_t end_x = (int64_t) floor(to.x_mm);
    int64_t end_y = (int64_t) floor(to.y_mm);
    double dx = to.x_mm - from.x_mm;
    double dy = to.y_mm - from.y_mm;
    int64_t step_x = dx > 0.0 ? 1 : -1;
    int64_t step_y = dy > 0.0 ? 1 : -1;
    double next_x = HUGE_VAL;
    double next_y = HUGE_VAL;
    double delta_x = HUGE_VAL;
    double delta_y = HUGE_VAL;
    int64_t guard;
    int64_t limit;

    if (dx != 0.0) {
        double boundary = dx > 0.0 ? (double) (x + 1) : (double) x;

        delta_x = fabs(1.0 / dx);
        next_x = (boundary - from.x_mm) / dx;
    }
    if (dy != 0.0) {
        double boundary = dy > 0.0 ? (double) (y + 1) : (double) y;

        delta_y = fabs(1.0 / dy);
        next_y = (boundary - from.y_mm) / dy;
    }

    /* One step per boundary crossed, plus slack for endpoints on a boundary. */
    limit = (end_x > x ? end_x - x : x - end_x)
        + (end_y > y ? end_y - y : y - end_y) + 2;

    for (guard = 0; guard <= limit; ++guard) {
        s_mark(target, x, y);
        if (x == end_x && y == end_y)
            return;
        if (next_x < next_y) {
            x += step_x;
            next_x += delta_x;
        } else {
            y += step_y;
            next_y += delta_y;
        }
    }
}

/* Rasterises the polygon into the target bitmap conservatively. */
void
bp_raster_fill(bp_raster_fill_args_t args)
{
    uint32_t total;
    uint32_t row;
    uint32_t ring;
    uint32_t index;
    double inverse_cell;

    BP_REQUIRE(args.target);
    BP_REQUIRE(args.cell_size_mm > 0.0);
    BP_REQUIRE(args.scratch_crossings);
    bp_polygon_require_valid(args.polygon);
    total = bp_polygon_point_count(args.polygon);
    BP_REQUIRE(args.scratch_capacity >= total);

    bp_bitmap_clear(args.target);
    inverse_cell = 1.0 / args.cell_size_mm;

    /* Pass one: every cell whose centre is inside, by even-odd scanline. */
    for (row = 0U; row < args.target->height_cells; ++row) {
        double centre_y = (double) row + 0.5;
        size_t count = 0U;
        size_t pair;

        for (ring = 0U; ring < args.polygon->ring_count; ++ring) {
            uint32_t begin = args.polygon->ring_offsets[ring];
            uint32_t end = args.polygon->ring_offsets[ring + 1U];

            for (index = begin; index < end; ++index) {
                uint32_t next = index + 1U == end ? begin : index + 1U;
                double ax = (args.polygon->points[index].x_mm
                    - args.origin_mm.x_mm) * inverse_cell;
                double ay = (args.polygon->points[index].y_mm
                    - args.origin_mm.y_mm) * inverse_cell;
                double bx = (args.polygon->points[next].x_mm
                    - args.origin_mm.x_mm) * inverse_cell;
                double by = (args.polygon->points[next].y_mm
                    - args.origin_mm.y_mm) * inverse_cell;

                /* Half-open in y, so a vertex on the line counts once. */
                if ((ay > centre_y) == (by > centre_y))
                    continue;
                BP_REQUIRE(count < args.scratch_capacity);
                args.scratch_crossings[count] = ax
                    + (centre_y - ay) * (bx - ax) / (by - ay);
                ++count;
            }
        }

        s_sort(args.scratch_crossings, count);
        for (pair = 0U; pair + 1U < count; pair += 2U) {
            double from = args.scratch_crossings[pair];
            double to = args.scratch_crossings[pair + 1U];
            double first = ceil(from - 0.5);
            double last = ceil(to - 0.5) - 1.0;
            int64_t cell;

            if (first < 0.0)
                first = 0.0;
            if (last > (double) args.target->width_cells - 1.0)
                last = (double) args.target->width_cells - 1.0;
            for (cell = (int64_t) first; cell <= (int64_t) last; ++cell)
                s_mark(args.target, cell, (int64_t) row);
        }
    }

    /* Pass two: every cell any edge passes through, holes included. */
    for (ring = 0U; ring < args.polygon->ring_count; ++ring) {
        uint32_t begin = args.polygon->ring_offsets[ring];
        uint32_t end = args.polygon->ring_offsets[ring + 1U];

        for (index = begin; index < end; ++index) {
            uint32_t next = index + 1U == end ? begin : index + 1U;
            bp_point_t from;
            bp_point_t to;

            from.x_mm = (args.polygon->points[index].x_mm
                - args.origin_mm.x_mm) * inverse_cell;
            from.y_mm = (args.polygon->points[index].y_mm
                - args.origin_mm.y_mm) * inverse_cell;
            to.x_mm = (args.polygon->points[next].x_mm
                - args.origin_mm.x_mm) * inverse_cell;
            to.y_mm = (args.polygon->points[next].y_mm
                - args.origin_mm.y_mm) * inverse_cell;
            s_trace_edge(args.target, from, to);
        }
    }
}

/* Returns the scratch words bp_raster_dilate needs. */
size_t
bp_raster_dilate_scratch_words(uint32_t clearance_cells,
    uint32_t dst_width_cells)
{
    BP_REQUIRE(dst_width_cells > 0U);
    /* One working row per growth step, plus one to translate the source into. */
    return ((size_t) clearance_cells + 2U)
        * bp_bitmap_stride_words(dst_width_cells);
}

/*
 * Returns how far the structuring element reaches sideways on a row that sits
 * `delta` rows away, measured so that cell squares stay at least the clearance
 * apart. See the header for why the measure is square to square.
 */
static uint32_t
s_reach_cells(uint32_t clearance_cells, int64_t delta)
{
    int64_t gap_rows;
    int64_t remaining;
    int64_t reach = 0;

    if (clearance_cells == 0U)
        return 0U;
    gap_rows = delta < 0 ? -delta : delta;
    gap_rows = gap_rows > 1 ? gap_rows - 1 : 0;
    remaining = (int64_t) clearance_cells * clearance_cells
        - gap_rows * gap_rows;
    BP_REQUIRE(remaining > 0);
    while ((reach + 1) * (reach + 1) < remaining)
        ++reach;
    /* One more, because two squares one step apart still touch. */
    return (uint32_t) reach + 1U;
}

/* Grows src by the clearance into dst, offset by (clearance, clearance). */
void
bp_raster_dilate(bp_bitmap_t *dst, const bp_bitmap_t *src,
    uint32_t clearance_cells, uint64_t *scratch, size_t scratch_capacity)
{
    size_t stride;
    uint64_t *staged;
    uint64_t *profile;
    uint32_t source_row;
    uint32_t step;

    BP_REQUIRE(dst);
    BP_REQUIRE(src);
    BP_REQUIRE(scratch);
    BP_REQUIRE(dst->width_cells == src->width_cells + 2U * clearance_cells);
    BP_REQUIRE(dst->height_cells == src->height_cells + 2U * clearance_cells);
    BP_REQUIRE(scratch_capacity
        >= bp_raster_dilate_scratch_words(clearance_cells, dst->width_cells));

    stride = dst->stride_words;
    staged = scratch;
    profile = scratch + stride;
    bp_bitmap_clear(dst);

    for (source_row = 0U; source_row < src->height_cells; ++source_row) {
        uint32_t offset;

        /*
         * Move the source row into destination coordinates once. Everything
         * below is then a shift within a row of the destination's width.
         */
        memset(staged, 0, stride * sizeof staged[0]);
        memcpy(staged, bp_bitmap_row_const(src, source_row),
            src->stride_words * sizeof staged[0]);
        bp_row_translate(profile, staged, stride, dst->width_cells,
            (int32_t) clearance_cells);

        /*
         * profile[k] is the row grown horizontally by k. Each step grows the
         * previous one by a single cell, so building the whole ladder costs
         * O(radius) row operations rather than O(radius^2).
         */
        for (step = 1U; step <= clearance_cells; ++step) {
            uint64_t *previous = profile + (size_t) (step - 1U) * stride;
            uint64_t *current = profile + (size_t) step * stride;

            memcpy(current, previous, stride * sizeof current[0]);
            bp_row_translate(staged, previous, stride, dst->width_cells, 1);
            bp_row_or(current, current, staged, stride);
            bp_row_translate(staged, previous, stride, dst->width_cells, -1);
            bp_row_or(current, current, staged, stride);
        }

        for (offset = 0U; offset <= 2U * clearance_cells; ++offset) {
            int64_t delta = (int64_t) offset - (int64_t) clearance_cells;
            uint32_t reach = s_reach_cells(clearance_cells, delta);
            uint64_t *dst_row = bp_bitmap_row(dst, source_row + offset);

            BP_REQUIRE(reach <= clearance_cells);
            bp_row_or(dst_row, dst_row,
                profile + (size_t) reach * stride, stride);
        }
    }
}
