#include "bp_mask.h"

#include "bp_contract.h"

/* Returns the row_offsets capacity a mask of this height needs. */
size_t
bp_mask_row_offset_capacity(uint32_t height_cells)
{
    BP_REQUIRE(height_cells > 0U);
    return (size_t) height_cells + 1U;
}

/*
 * Checks every documented invariant: ordered disjoint spans inside the row,
 * offsets that only grow, a tight bounding box and a consistent area.
 */
void
bp_mask_require_valid(const bp_mask_t *mask)
{
    uint32_t row;
    uint32_t touches_left = 0U;
    uint32_t touches_right = 0U;
    uint64_t area = 0U;

    BP_REQUIRE(mask);
    BP_REQUIRE(mask->width_cells > 0U);
    BP_REQUIRE(mask->height_cells > 0U);
    BP_REQUIRE(mask->row_offsets);
    BP_REQUIRE(mask->spans);
    BP_REQUIRE(mask->span_count > 0U);
    BP_REQUIRE(mask->row_offsets[0] == 0U);
    BP_REQUIRE(mask->row_offsets[mask->height_cells] == mask->span_count);

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t begin = mask->row_offsets[row];
        uint32_t end = mask->row_offsets[row + 1U];
        uint32_t index;
        uint32_t previous_end = 0U;

        BP_REQUIRE(end >= begin);
        BP_REQUIRE(end <= mask->span_count);

        for (index = begin; index < end; ++index) {
            const bp_span_t *span = &mask->spans[index];

            BP_REQUIRE(span->length_cells > 0U);
            BP_REQUIRE(span->x_cells < mask->width_cells);
            BP_REQUIRE(span->length_cells <= mask->width_cells - span->x_cells);
            /* Strictly increasing, and a gap between runs so none touch. */
            if (index > begin)
                BP_REQUIRE(span->x_cells > previous_end);
            previous_end = span->x_cells + span->length_cells;

            if (span->x_cells == 0U)
                touches_left = 1U;
            if (previous_end == mask->width_cells)
                touches_right = 1U;
            area += span->length_cells;
        }
    }

    /* A tight bounding box makes the used-area report meaningful. */
    BP_REQUIRE(mask->row_offsets[1] > mask->row_offsets[0]);
    BP_REQUIRE(mask->row_offsets[mask->height_cells]
        > mask->row_offsets[mask->height_cells - 1U]);
    BP_REQUIRE(touches_left == 1U);
    BP_REQUIRE(touches_right == 1U);
    BP_REQUIRE(area == mask->area_cells);
}

/* Builds a solid rectangle mask: one full-width span per row. */
bp_mask_t
bp_mask_rectangle(bp_mask_rectangle_args_t args)
{
    bp_mask_t mask;
    uint32_t row;

    BP_REQUIRE(args.width_cells > 0U);
    BP_REQUIRE(args.height_cells > 0U);
    BP_REQUIRE(args.row_offsets);
    BP_REQUIRE(args.spans);
    BP_REQUIRE(args.row_offset_capacity
        >= bp_mask_row_offset_capacity(args.height_cells));
    BP_REQUIRE(args.span_capacity >= (size_t) args.height_cells);

    for (row = 0U; row < args.height_cells; ++row) {
        args.row_offsets[row] = row;
        args.spans[row].x_cells = 0U;
        args.spans[row].length_cells = args.width_cells;
    }
    args.row_offsets[args.height_cells] = args.height_cells;

    mask.width_cells = args.width_cells;
    mask.height_cells = args.height_cells;
    mask.row_offsets = args.row_offsets;
    mask.spans = args.spans;
    mask.span_count = args.height_cells;
    mask.area_cells = (uint64_t) args.width_cells * args.height_cells;
    bp_mask_require_valid(&mask);
    return mask;
}

/* Counts the spans an occupancy bitmap will produce. */
uint32_t
bp_mask_count_spans(const bp_bitmap_t *source)
{
    uint32_t row;
    uint32_t count = 0U;

    BP_REQUIRE(source);
    for (row = 0U; row < source->height_cells; ++row) {
        uint32_t x = 0U;

        while (x < source->width_cells) {
            if (!bp_bitmap_get(source, x, row)) {
                ++x;
                continue;
            }
            ++count;
            while (x < source->width_cells && bp_bitmap_get(source, x, row))
                ++x;
        }
    }
    return count;
}

/*
 * Converts an occupancy bitmap into row spans. This is the seam the polygon
 * rasteriser will plug into: it produces cells, this produces the spans every
 * search path consumes.
 */
bp_mask_t
bp_mask_from_bitmap(bp_mask_from_bitmap_args_t args)
{
    bp_mask_t mask;
    uint32_t row;
    uint32_t written = 0U;
    uint64_t area = 0U;

    BP_REQUIRE(args.source);
    BP_REQUIRE(args.row_offsets);
    BP_REQUIRE(args.spans);
    BP_REQUIRE(args.row_offset_capacity
        >= bp_mask_row_offset_capacity(args.source->height_cells));

    for (row = 0U; row < args.source->height_cells; ++row) {
        uint32_t x = 0U;

        args.row_offsets[row] = written;
        while (x < args.source->width_cells) {
            uint32_t length;

            if (!bp_bitmap_get(args.source, x, row)) {
                ++x;
                continue;
            }
            length = 1U;
            while (x + length < args.source->width_cells
                && bp_bitmap_get(args.source, x + length, row))
                ++length;

            BP_REQUIRE(written < args.span_capacity);
            args.spans[written].x_cells = x;
            args.spans[written].length_cells = length;
            ++written;
            area += length;
            x += length;
        }
    }
    args.row_offsets[args.source->height_cells] = written;

    mask.width_cells = args.source->width_cells;
    mask.height_cells = args.source->height_cells;
    mask.row_offsets = args.row_offsets;
    mask.spans = args.spans;
    mask.span_count = written;
    mask.area_cells = area;
    /* Rejects a source whose bounding box is not tight, which is a caller bug. */
    bp_mask_require_valid(&mask);
    return mask;
}

/* Reports whether the mask placed at (x, y) lies wholly inside the sheet. */
int
bp_mask_in_bounds(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y)
{
    BP_REQUIRE(sheet);
    BP_REQUIRE(mask);
    BP_REQUIRE(mask->width_cells > 0U);
    BP_REQUIRE(mask->height_cells > 0U);

    if (mask->width_cells > sheet->width_cells
        || mask->height_cells > sheet->height_cells)
        return 0;
    return x <= sheet->width_cells - mask->width_cells
        && y <= sheet->height_cells - mask->height_cells;
}

/* Reports whether the mask placed at (x, y) touches an occupied sheet cell. */
int
bp_mask_overlaps(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y)
{
    uint32_t row;

    BP_REQUIRE(bp_mask_in_bounds(sheet, mask, x, y));

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];

            if (bp_bitmap_span_occupied(sheet, y + row, x + span->x_cells,
                span->length_cells))
                return 1;
        }
    }
    return 0;
}

/* Occupies the mask's cells on the sheet after proving the placement is free. */
void
bp_mask_blit(bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x, uint32_t y)
{
    uint32_t row;

    BP_REQUIRE(bp_mask_in_bounds(sheet, mask, x, y));
    BP_REQUIRE(!bp_mask_overlaps(sheet, mask, x, y));

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];

            bp_bitmap_set_span(sheet, y + row, x + span->x_cells,
                span->length_cells);
        }
    }
}
