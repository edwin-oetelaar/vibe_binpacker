#include "test_support.h"

#include "bp_bitmap.h"

#include <string.h>

/* Stride must round up to whole words, exactly at the 64-cell boundaries. */
static void
s_test_stride_and_capacity(void)
{
    CHECK(bp_bitmap_stride_words(1U) == 1U);
    CHECK(bp_bitmap_stride_words(63U) == 1U);
    CHECK(bp_bitmap_stride_words(64U) == 1U);
    CHECK(bp_bitmap_stride_words(65U) == 2U);
    CHECK(bp_bitmap_stride_words(128U) == 2U);
    CHECK(bp_bitmap_stride_words(1200U) == 19U);
    CHECK(bp_bitmap_required_words(1200U, 1200U) == 19U * 1200U);
    CHECK(bp_bitmap_required_words(100U, 3U) == 2U * 3U);
}

/* A freshly initialised bitmap owns cleared storage, whatever was there. */
static void
s_test_init_clears_storage(void)
{
    uint64_t words[2U * 4U];
    bp_bitmap_t bitmap;

    memset(words, 0xff, sizeof words);
    bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 100U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });

    CHECK(bitmap.width_cells == 100U);
    CHECK(bitmap.height_cells == 4U);
    CHECK(bitmap.stride_words == 2U);
    CHECK(bp_bitmap_occupied_count(&bitmap) == 0U);
}

/* Single cells must land on the documented bit, including at word edges. */
static void
s_test_single_cells(void)
{
    uint64_t words[2U * 3U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 100U,
        .height_cells = 3U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    const uint32_t probes[] = { 0U, 1U, 63U, 64U, 65U, 99U };
    size_t index;

    for (index = 0U; index < sizeof probes / sizeof probes[0]; ++index) {
        bp_bitmap_set_span(&bitmap, 1U, probes[index], 1U);
        CHECK(bp_bitmap_get(&bitmap, probes[index], 1U) == 1);
        CHECK(bp_bitmap_get(&bitmap, probes[index], 0U) == 0);
        bp_bitmap_clear_span(&bitmap, 1U, probes[index], 1U);
        CHECK(bp_bitmap_get(&bitmap, probes[index], 1U) == 0);
    }
    CHECK(bp_bitmap_occupied_count(&bitmap) == 0U);

    /* Bit 0 of a word is the smallest x, so cell 64 is bit 0 of word 1. */
    bp_bitmap_set_span(&bitmap, 2U, 64U, 1U);
    CHECK(bp_bitmap_row_const(&bitmap, 2U)[1] == UINT64_C(1));
}

/* Spans crossing word boundaries must set exactly the requested cells. */
static void
s_test_span_across_words(void)
{
    uint64_t words[3U * 2U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 130U,
        .height_cells = 2U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    uint32_t x;

    bp_bitmap_set_span(&bitmap, 0U, 60U, 10U);
    CHECK(bp_bitmap_row_count(&bitmap, 0U) == 10U);
    for (x = 0U; x < 130U; ++x)
        CHECK(bp_bitmap_get(&bitmap, x, 0U) == (x >= 60U && x < 70U));

    /* A span covering three words, ending on the last cell of the row. */
    bp_bitmap_set_span(&bitmap, 1U, 3U, 127U);
    CHECK(bp_bitmap_row_count(&bitmap, 1U) == 127U);
    CHECK(bp_bitmap_get(&bitmap, 2U, 1U) == 0);
    CHECK(bp_bitmap_get(&bitmap, 3U, 1U) == 1);
    CHECK(bp_bitmap_get(&bitmap, 129U, 1U) == 1);
}

/*
 * The padding invariant: cells past width_cells never become occupied, so a
 * full row counts exactly width_cells and the tail word stays masked.
 */
static void
s_test_padding_stays_zero(void)
{
    uint64_t words[2U * 2U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 100U,
        .height_cells = 2U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    uint64_t scratch[2];

    bp_bitmap_set_span(&bitmap, 0U, 0U, 100U);
    CHECK(bp_bitmap_row_count(&bitmap, 0U) == 100U);
    CHECK(bp_bitmap_occupied_count(&bitmap) == 100U);
    /* 100 cells is word 0 full plus 36 bits of word 1. */
    CHECK(bp_bitmap_row_const(&bitmap, 0U)[1]
        == (UINT64_C(1) << 36U) - UINT64_C(1));

    /* Translating right must drop cells off the end, not into padding. */
    bp_row_translate(scratch, bp_bitmap_row_const(&bitmap, 0U), 2U, 100U, 10);
    CHECK(scratch[1] == ((UINT64_C(1) << 36U) - UINT64_C(1)));
    CHECK(scratch[0] == ~((UINT64_C(1) << 10U) - UINT64_C(1)));
}

/* Occupancy queries must agree with a cell-by-cell reference count. */
static void
s_test_span_queries(void)
{
    uint64_t words[3U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 140U,
        .height_cells = 1U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    uint32_t x;
    uint32_t length;

    bp_bitmap_set_span(&bitmap, 0U, 10U, 5U);
    bp_bitmap_set_span(&bitmap, 0U, 62U, 4U);
    bp_bitmap_set_span(&bitmap, 0U, 130U, 3U);

    for (x = 0U; x < 140U; ++x) {
        for (length = 1U; length <= 140U - x; ++length) {
            uint32_t expected = 0U;
            uint32_t probe;

            for (probe = x; probe < x + length; ++probe)
                expected += (uint32_t) bp_bitmap_get(&bitmap, probe, 0U);

            CHECK(bp_bitmap_span_count(&bitmap, 0U, x, length) == expected);
            CHECK(bp_bitmap_span_occupied(&bitmap, 0U, x, length)
                == (expected > 0U));
        }
    }
}

/*
 * The jump scan depends on finding the rightmost blocker, so this is checked
 * against a backwards cell-by-cell reference over every span of a row.
 */
static void
s_test_span_last_occupied(void)
{
    uint64_t words[3U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 140U,
        .height_cells = 1U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    uint32_t x;
    uint32_t length;

    bp_bitmap_set_span(&bitmap, 0U, 7U, 2U);
    bp_bitmap_set_span(&bitmap, 0U, 63U, 3U);
    bp_bitmap_set_span(&bitmap, 0U, 128U, 1U);

    for (x = 0U; x < 140U; ++x) {
        for (length = 1U; length <= 140U - x; ++length) {
            uint32_t expected = 0U;
            int expected_found = 0;
            uint32_t probe = x + length;
            uint32_t found_at = UINT32_MAX;
            int found;

            while (probe-- > x) {
                if (bp_bitmap_get(&bitmap, probe, 0U)) {
                    expected = probe;
                    expected_found = 1;
                    break;
                }
            }

            found = bp_bitmap_span_last_occupied(&bitmap, 0U, x, length,
                &found_at);
            CHECK(found == expected_found);
            if (expected_found)
                CHECK(found_at == expected);
            else
                CHECK(found_at == UINT32_MAX);
        }
    }
}

/* Clearing a span must free exactly that span and nothing beside it. */
static void
s_test_clear_span(void)
{
    uint64_t words[2U];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 128U,
        .height_cells = 1U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });

    bp_bitmap_set_span(&bitmap, 0U, 0U, 128U);
    bp_bitmap_clear_span(&bitmap, 0U, 60U, 8U);
    CHECK(bp_bitmap_row_count(&bitmap, 0U) == 120U);
    CHECK(bp_bitmap_get(&bitmap, 59U, 0U) == 1);
    CHECK(bp_bitmap_get(&bitmap, 60U, 0U) == 0);
    CHECK(bp_bitmap_get(&bitmap, 67U, 0U) == 0);
    CHECK(bp_bitmap_get(&bitmap, 68U, 0U) == 1);
}

/* Row algebra is plain word-wise logic and may write into one of its inputs. */
static void
s_test_row_logic(void)
{
    uint64_t a[2] = { UINT64_C(0xf0f0), UINT64_C(0x00ff) };
    uint64_t b[2] = { UINT64_C(0xff00), UINT64_C(0x0f0f) };
    uint64_t dst[2];

    bp_row_and(dst, a, b, 2U);
    CHECK(dst[0] == UINT64_C(0xf000));
    CHECK(dst[1] == UINT64_C(0x000f));

    bp_row_or(dst, a, b, 2U);
    CHECK(dst[0] == UINT64_C(0xfff0));
    CHECK(dst[1] == UINT64_C(0x0fff));

    bp_row_and(a, a, b, 2U);
    CHECK(a[0] == UINT64_C(0xf000));
    CHECK(a[1] == UINT64_C(0x000f));
}

/*
 * Translation is compared against a reference that moves cells one at a time,
 * over every shift a row can take. This is the primitive the erosion scan
 * depends on, so it gets exhaustive rather than sampled coverage.
 */
static void
s_test_translate_matches_reference(void)
{
    const uint32_t width_cells = 140U;
    const size_t stride = 3U;
    uint64_t source_words[3U];
    uint64_t moved[3U];
    bp_bitmap_t source = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width_cells,
        .height_cells = 1U,
        .words = source_words,
        .word_capacity = stride
    });
    int32_t delta;

    bp_bitmap_set_span(&source, 0U, 0U, 3U);
    bp_bitmap_set_span(&source, 0U, 61U, 7U);
    bp_bitmap_set_span(&source, 0U, 127U, 2U);
    bp_bitmap_set_span(&source, 0U, 139U, 1U);

    for (delta = -160; delta <= 160; ++delta) {
        uint64_t expected_words[3U];
        bp_bitmap_t expected = bp_bitmap_init((bp_bitmap_init_args_t) {
            .width_cells = width_cells,
            .height_cells = 1U,
            .words = expected_words,
            .word_capacity = stride
        });
        uint32_t x;

        for (x = 0U; x < width_cells; ++x) {
            int64_t target = (int64_t) x + delta;

            if (bp_bitmap_get(&source, x, 0U) && target >= 0
                && target < (int64_t) width_cells)
                bp_bitmap_set_span(&expected, 0U, (uint32_t) target, 1U);
        }

        bp_row_translate(moved, bp_bitmap_row_const(&source, 0U), stride,
            width_cells, delta);
        CHECK(memcmp(moved, expected_words, sizeof moved) == 0);
    }
}

/* The 1200 x 1200 mm sheet at 1 mm is the size the solver actually runs on. */
static void
s_test_sheet_scale(void)
{
    const uint32_t side_cells = 1200U;
    uint64_t words[19U * 1200U];
    bp_bitmap_t sheet;
    uint32_t y;

    CHECK(sizeof words / sizeof words[0]
        == bp_bitmap_required_words(side_cells, side_cells));

    sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = side_cells,
        .height_cells = side_cells,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });

    /* A 300 x 200 part in the bottom-left corner. */
    for (y = 0U; y < 200U; ++y)
        bp_bitmap_set_span(&sheet, y, 0U, 300U);

    CHECK(bp_bitmap_occupied_count(&sheet) == 300U * 200U);
    CHECK(bp_bitmap_span_occupied(&sheet, 199U, 299U, 1U) == 1);
    CHECK(bp_bitmap_span_occupied(&sheet, 199U, 300U, 1U) == 0);
    CHECK(bp_bitmap_span_occupied(&sheet, 200U, 0U, 300U) == 0);
    CHECK(bp_bitmap_span_count(&sheet, 100U, 250U, 100U) == 50U);
}

/* Contract bodies: each builds its own bitmap so no shared state is needed. */

static void
s_body_zero_width(void)
{
    uint64_t words[1];

    (void) bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 0U,
        .height_cells = 1U,
        .words = words,
        .word_capacity = 1U
    });
}

static void
s_body_short_capacity(void)
{
    uint64_t words[1];

    (void) bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 128U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 1U
    });
}

static void
s_body_row_out_of_range(void)
{
    uint64_t words[4];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 64U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });

    bp_bitmap_set_span(&bitmap, 4U, 0U, 1U);
}

static void
s_body_zero_length_span(void)
{
    uint64_t words[4];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 64U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });

    bp_bitmap_set_span(&bitmap, 0U, 0U, 0U);
}

static void
s_body_span_past_width(void)
{
    uint64_t words[4];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 60U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });

    bp_bitmap_set_span(&bitmap, 0U, 55U, 6U);
}

static void
s_body_translate_aliases(void)
{
    uint64_t words[2] = { 0U, 0U };

    bp_row_translate(words, words, 2U, 128U, 1);
}

static void
s_body_translate_stride_mismatch(void)
{
    uint64_t source[2] = { 0U, 0U };
    uint64_t destination[2] = { 0U, 0U };

    bp_row_translate(destination, source, 2U, 64U, 1);
}

/* Complementing a row must leave the padding past the width at zero. */
static void
s_test_row_not(void)
{
    uint64_t words[2];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 100U,
        .height_cells = 1U,
        .words = words,
        .word_capacity = 2U
    });
    uint64_t complement[2];

    bp_bitmap_set_span(&bitmap, 0U, 10U, 5U);
    bp_row_not(complement, bp_bitmap_row_const(&bitmap, 0U), 2U, 100U);
    CHECK(complement[0] == ~(((UINT64_C(1) << 5U) - 1U) << 10U));
    /* 100 cells is word 0 full plus 36 bits of word 1; the rest stays clear. */
    CHECK(complement[1] == (UINT64_C(1) << 36U) - UINT64_C(1));
}

/*
 * The run mask is the primitive the erosion scan rests on, so it is compared
 * against a cell-by-cell reference for every length a row can hold, on a row
 * with runs that start and end at word boundaries.
 */
static void
s_test_run_mask(void)
{
    const uint32_t width_cells = 140U;
    const size_t stride = 3U;
    uint64_t words[3];
    bp_bitmap_t bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width_cells,
        .height_cells = 1U,
        .words = words,
        .word_capacity = stride
    });
    uint64_t free_row[3];
    uint64_t mask[3];
    uint64_t scratch[3];
    bp_bitmap_t view;
    uint32_t length;

    bp_bitmap_set_span(&bitmap, 0U, 0U, 3U);
    bp_bitmap_set_span(&bitmap, 0U, 61U, 2U);
    bp_bitmap_set_span(&bitmap, 0U, 64U, 1U);
    bp_bitmap_set_span(&bitmap, 0U, 130U, 1U);
    bp_row_not(free_row, bp_bitmap_row_const(&bitmap, 0U), stride,
        width_cells);

    view = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width_cells,
        .height_cells = 1U,
        .words = mask,
        .word_capacity = stride
    });

    for (length = 1U; length <= width_cells; ++length) {
        uint32_t x;

        bp_row_run_mask(mask, free_row, stride, width_cells, length, scratch);
        for (x = 0U; x < width_cells; ++x) {
            int expected = 1;
            uint32_t probe;

            for (probe = x; probe < x + length; ++probe)
                if (probe >= width_cells || bp_bitmap_get(&bitmap, probe, 0U)) {
                    expected = 0;
                    break;
                }
            CHECK(bp_bitmap_get(&view, x, 0U) == expected);
        }
    }
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_stride_and_capacity();
    s_test_init_clears_storage();
    s_test_single_cells();
    s_test_span_across_words();
    s_test_padding_stays_zero();
    s_test_span_queries();
    s_test_span_last_occupied();
    s_test_clear_span();
    s_test_row_logic();
    s_test_row_not();
    s_test_run_mask();
    s_test_translate_matches_reference();
    s_test_sheet_scale();

    t_expect_abort(s_body_zero_width, "zero width");
    t_expect_abort(s_body_short_capacity, "short capacity");
    t_expect_abort(s_body_row_out_of_range, "row out of range");
    t_expect_abort(s_body_zero_length_span, "zero length span");
    t_expect_abort(s_body_span_past_width, "span past width");
    t_expect_abort(s_body_translate_aliases, "translate aliases");
    t_expect_abort(s_body_translate_stride_mismatch, "translate stride");

    (void) printf("test_bp_bitmap: all checks passed\n");
    return 0;
}
