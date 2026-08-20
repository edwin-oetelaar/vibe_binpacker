#pragma once

#include "bp_bitmap.h"
#include "bp_polygon.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    /* The millimetre point that maps to the lower-left corner of cell (0,0). */
    bp_point_t origin_mm;
} bp_raster_extent_t;

/*
 * Returns the smallest cell grid that covers the polygon's bounding box, with
 * the origin at the box's lower-left corner. The grid is tight by
 * construction: conservative filling guarantees the first and last row and
 * column each contain at least one solid cell, which is what bp_mask expects.
 */
bp_raster_extent_t
bp_raster_extent(const bp_polygon_t *polygon, double cell_size_mm);

typedef struct {
    bp_bitmap_t *target;
    const bp_polygon_t *polygon;
    double cell_size_mm;
    bp_point_t origin_mm;
    /* Room for one crossing per edge; see bp_raster_scratch_crossings. */
    double *scratch_crossings;
    size_t scratch_capacity;
} bp_raster_fill_args_t;

/* Returns the crossing scratch a polygon needs: one entry per edge. */
size_t
bp_raster_scratch_crossings(const bp_polygon_t *polygon);

/*
 * Rasterises the polygon into the target bitmap, which is cleared first.
 *
 * The fill is **conservative**: a cell is solid when the polygon covers any
 * part of it, not merely when its centre is inside. That is what makes the
 * discrete solution valid in continuous space too, since rounding can only
 * push parts apart, never merge them.
 *
 * It works by taking the union of two passes. A scanline pass at cell centres
 * marks every cell that lies wholly inside, and a grid traversal of each edge
 * marks every cell any edge passes through. A cell that is only partly covered
 * must have the boundary running through it, so the second pass catches it;
 * a cell wholly inside has its centre inside, so the first pass catches it.
 *
 * The scanline uses the even-odd rule over all rings at once, so holes need no
 * special handling and ring winding is irrelevant.
 */
void
bp_raster_fill(bp_raster_fill_args_t args);

/*
 * Returns the scratch words bp_raster_dilate needs for a given clearance and
 * destination width.
 */
size_t
bp_raster_dilate_scratch_words(uint32_t clearance_cells,
    uint32_t dst_width_cells);

/*
 * Grows src into dst so that testing the grown footprint against undilated
 * occupancy guarantees at least clearance_cells of material between parts.
 *
 * The measure is the distance between **cell squares**, not between cell
 * centres, and that distinction is the whole point. Two cells offset by one
 * step diagonally have centres sqrt(2) apart, but their squares meet at a
 * corner, so the material distance is zero. Sizing the structuring element by
 * centre distance therefore lets parts touch at their corners, which an
 * end-to-end check of a real layout catches immediately.
 *
 * A cell at offset (dx, dy) is included when
 *
 *     max(0, |dx| - 1)^2 + max(0, |dy| - 1)^2 < clearance_cells^2
 *
 * which is the set of offsets whose squares come closer than the clearance.
 * For a clearance of one or two cells that works out as a full square, since
 * at that size a diagonal step buys no real distance; from three cells up the
 * corners start to round off and the shape stops over-reserving diagonals.
 *
 * dst must measure exactly (src width + 2 * clearance) by
 * (src height + 2 * clearance); the result sits at offset
 * (clearance, clearance), so the true geometry keeps a known position inside
 * the grown footprint. A clearance of zero copies src, which keeps the caller
 * free of a special case.
 */
void
bp_raster_dilate(bp_bitmap_t *dst, const bp_bitmap_t *src,
    uint32_t clearance_cells, uint64_t *scratch, size_t scratch_capacity);
