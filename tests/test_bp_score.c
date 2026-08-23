#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_place.h"
#include "bp_score.h"

#define S_MAX_ROWS 32U
#define S_MAX_SPANS 64U
#define S_MAX_SCRATCH 64U

/* Builds a bitmap from an ASCII picture whose first line is the top row. */
static void
s_picture(bp_bitmap_t *bitmap, uint64_t *words, size_t word_capacity,
    uint32_t width_cells, uint32_t height_cells, const char *rows[])
{
    uint32_t y;

    *bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width_cells,
        .height_cells = height_cells,
        .words = words,
        .word_capacity = word_capacity
    });
    for (y = 0U; y < height_cells; ++y) {
        const char *line = rows[height_cells - 1U - y];
        uint32_t x;

        for (x = 0U; x < width_cells; ++x)
            if (line[x] == '#')
                bp_bitmap_set_span(bitmap, y, x, 1U);
    }
}

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

/*
 * Contact counts the boundary cells that meet material or the sheet edge, so
 * the same part scores far higher in a corner than in open space.
 */
static void
s_test_contact(void)
{
    static const char *picture[] = {
        "........",
        "........",
        "........",
        "........"
    };
    uint64_t words[4];
    bp_bitmap_t sheet;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = s_rect(2U, 2U, row_offsets, spans);

    s_picture(&sheet, words, 4U, 8U, 4U, picture);

    /*
     * A 2x2 part has four runs of length two counting both directions, so its
     * capacity is 2 * (2 + 2 * 2) = 12 boundary cells.
     */
    CHECK(bp_score_contact_capacity(&mask) == 12U);

    /* In the bottom-left corner: left edge twice, bottom edge twice. */
    CHECK(bp_score_contact(&sheet, &mask, 0U, 0U) == 4U);
    /* Against the bottom edge only. */
    CHECK(bp_score_contact(&sheet, &mask, 3U, 0U) == 2U);
    /* Free of every edge, on an empty sheet. */
    CHECK(bp_score_contact(&sheet, &mask, 3U, 1U) == 0U);

    /* A neighbour on the left adds two more touching cells. */
    bp_bitmap_set_span(&sheet, 1U, 2U, 1U);
    bp_bitmap_set_span(&sheet, 2U, 2U, 1U);
    CHECK(bp_score_contact(&sheet, &mask, 3U, 1U) == 2U);
}

/*
 * With contact switched off and one height to look at, the ranking has to
 * agree with plain bottom-left. That is the anchor the whole scoring layer
 * hangs on: any difference is the score overruling a rule that measured
 * better than it.
 */
static void
s_test_matches_bottom_left(void)
{
    uint64_t sheet_words[2U * 24U];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 40U,
        .height_cells = 24U,
        .words = sheet_words,
        .word_capacity = sizeof sheet_words / sizeof sheet_words[0]
    });
    uint64_t scratch[S_MAX_SCRATCH];
    uint64_t state = UINT64_C(0xc0ffee123);
    uint32_t trial;

    for (trial = 0U; trial < 500U; ++trial) {
        uint32_t row_offsets[S_MAX_ROWS + 1U];
        bp_span_t spans[S_MAX_SPANS];
        bp_mask_t mask;
        bp_place_result_t expected;
        bp_score_best_result_t ranked;
        uint32_t blocker;
        uint32_t blockers = t_random_below(&state, 10U);

        bp_bitmap_clear(&sheet);
        for (blocker = 0U; blocker < blockers; ++blocker) {
            uint32_t y = t_random_below(&state, 24U);
            uint32_t x = t_random_below(&state, 40U);

            bp_bitmap_set_span(&sheet, y, x, 1U + t_random_below(&state,
                40U - x));
        }
        mask = s_rect(1U + t_random_below(&state, 6U),
            1U + t_random_below(&state, 5U), row_offsets, spans);

        expected = bp_place_bottom_left((bp_place_args_t) {
            .sheet = &sheet,
            .mask = &mask,
            .mode = BP_PLACE_JUMP,
            .scratch = scratch,
            .scratch_capacity = S_MAX_SCRATCH
        });
        ranked = bp_score_best_position((bp_score_best_args_t) {
            .sheet = &sheet,
            .mask = &mask,
            .weights = bp_score_default_weights(),
            .height_window = 1U,
            .max_positions = 64U,
            .blink_rate = 0.0,
            .random_state = NULL,
            .scratch = scratch,
            .scratch_capacity = S_MAX_SCRATCH
        });

        CHECK(ranked.found == expected.found);
        if (expected.found) {
            CHECK(ranked.x_cells == expected.x_cells);
            CHECK(ranked.y_cells == expected.y_cells);
        }
    }
}

/*
 * Blinking may pass over every candidate it looks at. It must still report a
 * position whenever one exists, and that position must really be free.
 */
static void
s_test_blinking_still_places(void)
{
    uint64_t sheet_words[2U * 24U];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 40U,
        .height_cells = 24U,
        .words = sheet_words,
        .word_capacity = sizeof sheet_words / sizeof sheet_words[0]
    });
    uint64_t scratch[S_MAX_SCRATCH];
    uint64_t state = UINT64_C(0x5eed5eed);
    uint64_t blink_state = UINT64_C(0x1234abcd);
    uint32_t moved = 0U;
    uint32_t trial;

    for (trial = 0U; trial < 500U; ++trial) {
        uint32_t row_offsets[S_MAX_ROWS + 1U];
        bp_span_t spans[S_MAX_SPANS];
        bp_mask_t mask;
        bp_place_result_t expected;
        bp_score_best_result_t ranked;
        uint32_t blocker;
        uint32_t blockers = t_random_below(&state, 8U);

        bp_bitmap_clear(&sheet);
        for (blocker = 0U; blocker < blockers; ++blocker) {
            uint32_t y = t_random_below(&state, 24U);
            uint32_t x = t_random_below(&state, 40U);

            bp_bitmap_set_span(&sheet, y, x, 1U + t_random_below(&state,
                40U - x));
        }
        mask = s_rect(1U + t_random_below(&state, 6U),
            1U + t_random_below(&state, 5U), row_offsets, spans);

        expected = bp_place_bottom_left((bp_place_args_t) {
            .sheet = &sheet,
            .mask = &mask,
            .mode = BP_PLACE_JUMP,
            .scratch = scratch,
            .scratch_capacity = S_MAX_SCRATCH
        });
        ranked = bp_score_best_position((bp_score_best_args_t) {
            .sheet = &sheet,
            .mask = &mask,
            .weights = bp_score_default_weights(),
            .height_window = 1U,
            .max_positions = 64U,
            .blink_rate = 0.5,
            .random_state = &blink_state,
            .scratch = scratch,
            .scratch_capacity = S_MAX_SCRATCH
        });

        CHECK(ranked.found == expected.found);
        if (!expected.found)
            continue;
        CHECK(bp_mask_in_bounds(&sheet, &mask, ranked.x_cells,
            ranked.y_cells));
        CHECK(!bp_mask_overlaps(&sheet, &mask, ranked.x_cells,
            ranked.y_cells));
        if (ranked.x_cells != expected.x_cells)
            ++moved;
    }
    /* Blinking has to actually blink, or this proves nothing. */
    CHECK(moved > 50U);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_zero_window(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 8U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = s_rect(2U, 2U, row_offsets, spans);
    uint64_t scratch[S_MAX_SCRATCH];

    (void) bp_score_best_position((bp_score_best_args_t) {
        .sheet = &sheet,
        .mask = &mask,
        .weights = bp_score_default_weights(),
        .height_window = 0U,
        .max_positions = 8U,
        .scratch = scratch,
        .scratch_capacity = S_MAX_SCRATCH
    });
}

static void
s_body_blink_without_state(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 8U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = s_rect(2U, 2U, row_offsets, spans);
    uint64_t scratch[S_MAX_SCRATCH];

    (void) bp_score_best_position((bp_score_best_args_t) {
        .sheet = &sheet,
        .mask = &mask,
        .weights = bp_score_default_weights(),
        .height_window = 2U,
        .max_positions = 8U,
        .blink_rate = 0.5,
        .random_state = NULL,
        .scratch = scratch,
        .scratch_capacity = S_MAX_SCRATCH
    });
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_contact();
    s_test_matches_bottom_left();
    s_test_blinking_still_places();

    t_expect_abort(s_body_zero_window, "zero window");
    t_expect_abort(s_body_blink_without_state, "blink without state");

    (void) printf("test_bp_score: all checks passed\n");
    return 0;
}
