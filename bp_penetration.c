#include "bp_penetration.h"

#include "bp_contract.h"

#include <string.h>

/* Returns the caller-owned cell capacity a field of this size needs. */
size_t
bp_penetration_count_capacity(uint32_t width_cells, uint32_t height_cells)
{
    BP_REQUIRE(width_cells > 0U);
    BP_REQUIRE(height_cells > 0U);
    BP_REQUIRE((size_t) width_cells <= SIZE_MAX / height_cells);
    return (size_t) width_cells * height_cells;
}

/* Returns the caller-owned running-total capacity a field needs. */
size_t
bp_penetration_prefix_capacity(uint32_t width_cells, uint32_t height_cells)
{
    BP_REQUIRE(width_cells > 0U);
    BP_REQUIRE(height_cells > 0U);
    BP_REQUIRE((size_t) width_cells + 1U <= SIZE_MAX / height_cells);
    return ((size_t) width_cells + 1U) * height_cells;
}

/* Initialises an empty field over caller-owned storage. */
bp_penetration_t
bp_penetration_init(bp_penetration_init_args_t args)
{
    bp_penetration_t field;

    BP_REQUIRE(args.width_cells > 0U);
    BP_REQUIRE(args.height_cells > 0U);
    BP_REQUIRE(args.counts);
    BP_REQUIRE(args.prefix);
    BP_REQUIRE(args.count_capacity
        >= bp_penetration_count_capacity(args.width_cells, args.height_cells));
    BP_REQUIRE(args.prefix_capacity
        >= bp_penetration_prefix_capacity(args.width_cells,
            args.height_cells));

    field.width_cells = args.width_cells;
    field.height_cells = args.height_cells;
    field.counts = args.counts;
    field.prefix = args.prefix;
    bp_penetration_clear(&field);
    return field;
}

/* Empties the field so it can be reused without reallocating. */
void
bp_penetration_clear(bp_penetration_t *field)
{
    BP_REQUIRE(field);
    BP_REQUIRE(field->counts);
    BP_REQUIRE(field->prefix);

    memset(field->counts, 0,
        bp_penetration_count_capacity(field->width_cells, field->height_cells)
            * sizeof field->counts[0]);
    memset(field->prefix, 0,
        bp_penetration_prefix_capacity(field->width_cells, field->height_cells)
            * sizeof field->prefix[0]);
}

/* Validates that a placement lies wholly inside the field. */
static void
s_require_in_bounds(const bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y)
{
    BP_REQUIRE(field);
    BP_REQUIRE(field->counts);
    bp_mask_require_valid(mask);
    BP_REQUIRE(mask->width_cells <= field->width_cells);
    BP_REQUIRE(mask->height_cells <= field->height_cells);
    BP_REQUIRE(x <= field->width_cells - mask->width_cells);
    BP_REQUIRE(y <= field->height_cells - mask->height_cells);
}

/*
 * Recomputes one row's running totals from the first cell that changed. Only
 * the tail can be affected, so a part sitting on the right of the sheet costs
 * proportionally less to move than one on the left.
 */
static void
s_refresh_row(bp_penetration_t *field, uint32_t row, uint32_t from_x)
{
    size_t stride = (size_t) field->width_cells + 1U;
    uint64_t *totals = field->prefix + (size_t) row * stride;
    const uint32_t *counts = field->counts + (size_t) row * field->width_cells;
    uint32_t x;

    for (x = from_x; x < field->width_cells; ++x)
        totals[x + 1U] = totals[x] + counts[x];
}

/* Returns the smallest x any span of a mask row covers. */
static uint32_t
s_row_first_x(const bp_mask_t *mask, uint32_t row)
{
    BP_REQUIRE(mask->row_offsets[row + 1U] > mask->row_offsets[row]);
    return mask->spans[mask->row_offsets[row]].x_cells;
}

/* Adds one part's material at (x, y). */
void
bp_penetration_add(bp_penetration_t *field, const bp_mask_t *mask, uint32_t x,
    uint32_t y, uint32_t weight)
{
    uint32_t row;

    s_require_in_bounds(field, mask, x, y);
    BP_REQUIRE(weight > 0U);

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t *counts = field->counts
            + (size_t) (y + row) * field->width_cells;
        uint32_t index;

        if (mask->row_offsets[row + 1U] == mask->row_offsets[row])
            continue;
        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t cell;

            for (cell = 0U; cell < span->length_cells; ++cell) {
                uint32_t *slot = &counts[x + span->x_cells + cell];

                BP_REQUIRE(*slot <= UINT32_MAX - weight);
                *slot += weight;
            }
        }
        s_refresh_row(field, y + row, x + s_row_first_x(mask, row));
    }
}

/* Takes one part's material away again. */
void
bp_penetration_remove(bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y, uint32_t weight)
{
    uint32_t row;

    s_require_in_bounds(field, mask, x, y);
    BP_REQUIRE(weight > 0U);

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t *counts = field->counts
            + (size_t) (y + row) * field->width_cells;
        uint32_t index;

        if (mask->row_offsets[row + 1U] == mask->row_offsets[row])
            continue;
        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t cell;

            for (cell = 0U; cell < span->length_cells; ++cell) {
                uint32_t *slot = &counts[x + span->x_cells + cell];

                BP_REQUIRE(*slot >= weight);
                *slot -= weight;
            }
        }
        s_refresh_row(field, y + row, x + s_row_first_x(mask, row));
    }
}

/* Returns how much material already sits under the mask at (x, y). */
uint64_t
bp_penetration_probe(const bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y)
{
    size_t stride = (size_t) field->width_cells + 1U;
    uint32_t row;
    uint64_t total = 0U;

    s_require_in_bounds(field, mask, x, y);

    for (row = 0U; row < mask->height_cells; ++row) {
        const uint64_t *totals = field->prefix + (size_t) (y + row) * stride;
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t left = x + span->x_cells;

            total += totals[left + span->length_cells] - totals[left];
        }
    }
    return total;
}
