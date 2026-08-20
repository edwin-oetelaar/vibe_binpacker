#pragma once

#include "bp_bitmap.h"

#include <stddef.h>
#include <stdint.h>

/* One horizontal run of solid cells, offset from the mask's left edge. */
typedef struct {
    uint32_t x_cells;
    uint32_t length_cells;
} bp_span_t;

/*
 * A part footprint stored as row spans, which is the representation every
 * later phase needs: the erosion scan shifts one span at a time, and the
 * penetration measure costs one subtraction per span regardless of area.
 *
 * Row r occupies spans[row_offsets[r] .. row_offsets[r + 1]). Rows run bottom
 * to top, spans within a row run left to right and never touch or overlap,
 * because touching runs would be counted twice.
 *
 * The bounding box is tight: the bottom and top rows are non-empty, some span
 * starts at x 0, and some span ends at width_cells. A mask with holes simply
 * has more than one span in the affected rows, which is why parts with holes
 * need no special handling anywhere else.
 *
 * All arrays are caller owned and must outlive the mask.
 */
typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    const uint32_t *row_offsets;
    const bp_span_t *spans;
    uint32_t span_count;
    uint64_t area_cells;
} bp_mask_t;

typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    uint32_t *row_offsets;
    size_t row_offset_capacity;
    bp_span_t *spans;
    size_t span_capacity;
} bp_mask_rectangle_args_t;

typedef struct {
    const bp_bitmap_t *source;
    uint32_t *row_offsets;
    size_t row_offset_capacity;
    bp_span_t *spans;
    size_t span_capacity;
} bp_mask_from_bitmap_args_t;

/*
 * Returns the row_offsets capacity a mask of this height needs. Spans need
 * their own capacity, which depends on the shape and is at least the height.
 */
size_t
bp_mask_row_offset_capacity(uint32_t height_cells);

/* Builds a solid rectangle mask: one full-width span per row. */
bp_mask_t
bp_mask_rectangle(bp_mask_rectangle_args_t args);

/*
 * Counts the spans an occupancy bitmap will produce, so a caller can size its
 * span buffer before converting. Cheap enough to run before every conversion.
 */
uint32_t
bp_mask_count_spans(const bp_bitmap_t *source);

/*
 * Builds a mask from an occupancy bitmap, which is how the polygon rasteriser
 * will feed this module. The source bounding box must already be tight and
 * the source must have at least one occupied cell.
 */
bp_mask_t
bp_mask_from_bitmap(bp_mask_from_bitmap_args_t args);

/* Checks every documented invariant of a mask; used by tests and builders. */
void
bp_mask_require_valid(const bp_mask_t *mask);

/* Reports whether the mask placed at (x, y) lies wholly inside the sheet. */
int
bp_mask_in_bounds(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y);

/*
 * Reports whether the mask placed at (x, y) touches an occupied sheet cell.
 * The placement must be in bounds; use bp_mask_in_bounds first.
 */
int
bp_mask_overlaps(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y);

/*
 * Frees the mask's cells on the sheet after proving every one of them is
 * occupied. Lifting a part off the sheet is only safe because placements
 * never overlap: the cells can belong to nothing else.
 */
void
bp_mask_erase(bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y);

/*
 * Occupies the mask's cells on the sheet at (x, y) after proving the
 * placement is in bounds and free. Overlap here is a caller defect, because
 * the search is what decides where a part may go.
 */
void
bp_mask_blit(bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y);
