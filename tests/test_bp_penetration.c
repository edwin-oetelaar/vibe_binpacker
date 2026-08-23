#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_penetration.h"

#define S_SIDE 40U
#define S_MAX_ROWS 32U
#define S_MAX_SPANS 64U

/* Builds a rectangle mask over caller storage. */
static bp_mask_t
s_rect(uint32_t width_cells, uint32_t height_cells, uint32_t *row_offsets,
    bp_span_t *spans)
{
    return bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = width_cells,
        .height_cells = height_cells,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
}

/* Sums the field cell by cell, which is what the running totals stand in for. */
static uint64_t
s_reference_probe(const bp_penetration_t *field, const bp_mask_t *mask,
    uint32_t x, uint32_t y)
{
    uint32_t row;
    uint64_t total = 0U;

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t cell;

            for (cell = 0U; cell < span->length_cells; ++cell)
                total += field->counts[(size_t) (y + row) * field->width_cells
                    + x + span->x_cells + cell];
        }
    }
    return total;
}

/* Returns the summed weight over the whole field. */
static uint64_t
s_total(const bp_penetration_t *field)
{
    size_t index;
    uint64_t total = 0U;

    for (index = 0U;
        index < (size_t) field->width_cells * field->height_cells; ++index)
        total += field->counts[index];
    return total;
}

/*
 * The running totals must agree with a cell-by-cell sum for every mask and
 * position, or every probe the separation makes is quietly wrong.
 */
static void
s_test_probe_matches_reference(void)
{
    static uint32_t counts[S_SIDE * S_SIDE];
    static uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    uint32_t offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_penetration_t field = bp_penetration_init(
        (bp_penetration_init_args_t) {
            .width_cells = S_SIDE,
            .height_cells = S_SIDE,
            .counts = counts,
            .count_capacity = sizeof counts / sizeof counts[0],
            .prefix = prefix,
            .prefix_capacity = sizeof prefix / sizeof prefix[0]
        });
    uint64_t state = UINT64_C(0xfeedface);
    uint32_t trial;

    for (trial = 0U; trial < 200U; ++trial) {
        uint32_t added_offsets[S_MAX_ROWS + 1U];
        bp_span_t added_spans[S_MAX_SPANS];
        uint32_t width = 1U + t_random_below(&state, 12U);
        uint32_t height = 1U + t_random_below(&state, 10U);
        bp_mask_t added = s_rect(width, height, added_offsets, added_spans);
        uint32_t x = t_random_below(&state, S_SIDE - width + 1U);
        uint32_t y = t_random_below(&state, S_SIDE - height + 1U);
        uint32_t weight = 1U + t_random_below(&state, 3U);
        uint32_t probe;

        bp_penetration_add(&field, &added, x, y, weight);

        for (probe = 0U; probe < 8U; ++probe) {
            uint32_t probe_width = 1U + t_random_below(&state, 12U);
            uint32_t probe_height = 1U + t_random_below(&state, 10U);
            bp_mask_t mask = s_rect(probe_width, probe_height, offsets, spans);
            uint32_t px = t_random_below(&state, S_SIDE - probe_width + 1U);
            uint32_t py = t_random_below(&state, S_SIDE - probe_height + 1U);

            CHECK(bp_penetration_probe(&field, &mask, px, py)
                == s_reference_probe(&field, &mask, px, py));
        }
    }
}

/*
 * Adding and removing the same part must leave nothing behind. The separation
 * lifts a part out and puts it back on every single move, so a leak here would
 * poison the whole search.
 */
static void
s_test_add_remove_round_trip(void)
{
    static uint32_t counts[S_SIDE * S_SIDE];
    static uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    uint32_t offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_penetration_t field = bp_penetration_init(
        (bp_penetration_init_args_t) {
            .width_cells = S_SIDE,
            .height_cells = S_SIDE,
            .counts = counts,
            .count_capacity = sizeof counts / sizeof counts[0],
            .prefix = prefix,
            .prefix_capacity = sizeof prefix / sizeof prefix[0]
        });
    bp_mask_t mask = s_rect(9U, 7U, offsets, spans);
    uint64_t state = UINT64_C(0x0badc0de);
    uint32_t trial;

    for (trial = 0U; trial < 300U; ++trial) {
        uint32_t x = t_random_below(&state, S_SIDE - 9U + 1U);
        uint32_t y = t_random_below(&state, S_SIDE - 7U + 1U);
        uint32_t weight = 1U + t_random_below(&state, 4U);

        bp_penetration_add(&field, &mask, x, y, weight);
        CHECK(s_total(&field) == (uint64_t) weight * mask.area_cells);
        CHECK(bp_penetration_probe(&field, &mask, x, y)
            == (uint64_t) weight * mask.area_cells);
        bp_penetration_remove(&field, &mask, x, y, weight);
        CHECK(s_total(&field) == 0U);
    }
}

/* Weight is additive, which is how a penalty is applied without moving. */
static void
s_test_weight_is_additive(void)
{
    static uint32_t counts[S_SIDE * S_SIDE];
    static uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    uint32_t offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_penetration_t field = bp_penetration_init(
        (bp_penetration_init_args_t) {
            .width_cells = S_SIDE,
            .height_cells = S_SIDE,
            .counts = counts,
            .count_capacity = sizeof counts / sizeof counts[0],
            .prefix = prefix,
            .prefix_capacity = sizeof prefix / sizeof prefix[0]
        });
    bp_mask_t mask = s_rect(6U, 5U, offsets, spans);

    bp_penetration_add(&field, &mask, 3U, 4U, 1U);
    bp_penetration_add(&field, &mask, 3U, 4U, 1U);
    CHECK(bp_penetration_probe(&field, &mask, 3U, 4U) == 2U * mask.area_cells);
    bp_penetration_remove(&field, &mask, 3U, 4U, 2U);
    CHECK(s_total(&field) == 0U);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_out_of_bounds(void)
{
    static uint32_t counts[S_SIDE * S_SIDE];
    static uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    uint32_t offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_penetration_t field = bp_penetration_init(
        (bp_penetration_init_args_t) {
            .width_cells = S_SIDE,
            .height_cells = S_SIDE,
            .counts = counts,
            .count_capacity = sizeof counts / sizeof counts[0],
            .prefix = prefix,
            .prefix_capacity = sizeof prefix / sizeof prefix[0]
        });
    bp_mask_t mask = s_rect(6U, 5U, offsets, spans);

    bp_penetration_add(&field, &mask, S_SIDE - 5U, 0U, 1U);
}

static void
s_body_remove_too_much(void)
{
    static uint32_t counts[S_SIDE * S_SIDE];
    static uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    uint32_t offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_penetration_t field = bp_penetration_init(
        (bp_penetration_init_args_t) {
            .width_cells = S_SIDE,
            .height_cells = S_SIDE,
            .counts = counts,
            .count_capacity = sizeof counts / sizeof counts[0],
            .prefix = prefix,
            .prefix_capacity = sizeof prefix / sizeof prefix[0]
        });
    bp_mask_t mask = s_rect(6U, 5U, offsets, spans);

    bp_penetration_add(&field, &mask, 0U, 0U, 1U);
    bp_penetration_remove(&field, &mask, 0U, 0U, 2U);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_probe_matches_reference();
    s_test_add_remove_round_trip();
    s_test_weight_is_additive();

    t_expect_abort(s_body_out_of_bounds, "out of bounds");
    t_expect_abort(s_body_remove_too_much, "remove too much");

    (void) printf("test_bp_penetration: all checks passed\n");
    return 0;
}
