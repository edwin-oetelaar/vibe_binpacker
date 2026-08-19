#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_place.h"

#define S_MAX_ROWS 64U
#define S_MAX_SPANS 512U

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

        for (x = 0U; x < width_cells; ++x) {
            CHECK(line[x] == '#' || line[x] == '.');
            if (line[x] == '#')
                bp_bitmap_set_span(bitmap, y, x, 1U);
        }
    }
}

/* An empty sheet places the first part in the bottom-left corner. */
static void
s_test_empty_sheet(void)
{
    uint64_t words[8];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 20U,
        .height_cells = 8U,
        .words = words,
        .word_capacity = 8U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 5U,
        .height_cells = 3U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
    bp_place_result_t reference =
        bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE);
    bp_place_result_t jump = bp_place_bottom_left(&sheet, &mask, BP_PLACE_JUMP);

    CHECK(reference.found == 1);
    CHECK(reference.x_cells == 0U);
    CHECK(reference.y_cells == 0U);
    CHECK(jump.found == 1);
    CHECK(jump.x_cells == 0U);
    CHECK(jump.y_cells == 0U);
}

/* A mask larger than the sheet is an expected miss, not a contract failure. */
static void
s_test_oversized_is_not_found(void)
{
    uint64_t words[8];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 20U,
        .height_cells = 8U,
        .words = words,
        .word_capacity = 8U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 21U,
        .height_cells = 3U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    CHECK(bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE).found == 0);
    CHECK(bp_place_bottom_left(&sheet, &mask, BP_PLACE_JUMP).found == 0);
}

/*
 * A concave part must slot into a matching notch rather than stack above it.
 * This is the behaviour that spans buy over bounding boxes.
 */
static void
s_test_concave_part_fills_notch(void)
{
    static const char *sheet_picture[] = {
        "..........",
        "..........",
        "###...####",
        "###...####"
    };
    static const char *part_picture[] = {
        "###",
        "###"
    };
    uint64_t sheet_words[4];
    uint64_t part_words[2];
    bp_bitmap_t sheet;
    bp_bitmap_t part_source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask;
    bp_place_result_t reference;
    bp_place_result_t jump;

    s_picture(&sheet, sheet_words, 4U, 10U, 4U, sheet_picture);
    s_picture(&part_source, part_words, 2U, 3U, 2U, part_picture);
    mask = bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &part_source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    reference = bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE);
    jump = bp_place_bottom_left(&sheet, &mask, BP_PLACE_JUMP);

    /* The 3-wide gap at x 3 is the only opening in the bottom two rows. */
    CHECK(reference.found == 1);
    CHECK(reference.x_cells == 3U);
    CHECK(reference.y_cells == 0U);
    CHECK(jump.x_cells == reference.x_cells);
    CHECK(jump.y_cells == reference.y_cells);
}

/*
 * A part placed inside another part's hole. Nothing in the search knows about
 * holes: the hole is simply free space, which is the whole argument for the
 * raster model.
 */
static void
s_test_part_nests_inside_hole(void)
{
    /* No free space outside the ring, so the hole is the only opening. */
    static const char *sheet_picture[] = {
        "#######",
        "#.....#",
        "#.....#",
        "#######"
    };
    static const char *part_picture[] = {
        "##",
        "##"
    };
    uint64_t sheet_words[4];
    uint64_t part_words[2];
    bp_bitmap_t sheet;
    bp_bitmap_t part_source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask;
    bp_place_result_t reference;
    bp_place_result_t jump;

    s_picture(&sheet, sheet_words, 4U, 7U, 4U, sheet_picture);
    s_picture(&part_source, part_words, 2U, 2U, 2U, part_picture);
    mask = bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &part_source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    reference = bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE);
    jump = bp_place_bottom_left(&sheet, &mask, BP_PLACE_JUMP);

    /* Rows 0 and 3 are blocked, so the lowest free 2x2 is the hole at y 1. */
    CHECK(reference.found == 1);
    CHECK(reference.y_cells == 1U);
    CHECK(reference.x_cells == 1U);
    CHECK(jump.x_cells == reference.x_cells);
    CHECK(jump.y_cells == reference.y_cells);
}

/* Fills a sheet with random blockers, reproducibly from the given seed. */
static void
s_random_sheet(bp_bitmap_t *sheet, uint64_t *state)
{
    uint32_t blocker;
    uint32_t count = t_random_below(state, 12U);

    bp_bitmap_clear(sheet);
    for (blocker = 0U; blocker < count; ++blocker) {
        uint32_t y = t_random_below(state, sheet->height_cells);
        uint32_t x = t_random_below(state, sheet->width_cells);
        uint32_t length = 1U + t_random_below(state, sheet->width_cells - x);

        bp_bitmap_set_span(sheet, y, x, length);
    }
}

/*
 * Builds a random tight mask. Cells (0, 0) and (width - 1, height - 1) are
 * always solid, which is the cheapest way to guarantee tightness.
 */
static bp_mask_t
s_random_mask(bp_bitmap_t *source, uint64_t *words, uint32_t *row_offsets,
    bp_span_t *spans, uint64_t *state)
{
    uint32_t width_cells = 1U + t_random_below(state, 7U);
    uint32_t height_cells = 1U + t_random_below(state, 5U);
    uint32_t y;

    *source = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width_cells,
        .height_cells = height_cells,
        .words = words,
        .word_capacity = height_cells
    });
    for (y = 0U; y < height_cells; ++y) {
        uint32_t x;

        for (x = 0U; x < width_cells; ++x)
            if (t_random_below(state, 3U) != 0U)
                bp_bitmap_set_span(source, y, x, 1U);
    }
    bp_bitmap_set_span(source, 0U, 0U, 1U);
    bp_bitmap_set_span(source, height_cells - 1U, width_cells - 1U, 1U);

    return bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = source,
        .row_offsets = row_offsets,
        .row_offset_capacity = (size_t) height_cells + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
}

/*
 * The property that matters: the jump scan must return exactly what the
 * cell-by-cell reference returns, on every shape and every obstacle pattern.
 * A fast path that is merely usually right would be worse than no fast path.
 */
static void
s_test_jump_matches_reference(void)
{
    uint64_t sheet_words[8U * 24U];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 40U,
        .height_cells = 24U,
        .words = sheet_words,
        .word_capacity = sizeof sheet_words / sizeof sheet_words[0]
    });
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t reference_checks = 0U;
    uint64_t jump_checks = 0U;
    uint32_t found_count = 0U;
    uint32_t trial;

    for (trial = 0U; trial < 4000U; ++trial) {
        uint64_t part_words[8];
        uint32_t row_offsets[8];
        bp_span_t spans[S_MAX_SPANS];
        bp_bitmap_t part_source;
        bp_mask_t mask;
        bp_place_result_t reference;
        bp_place_result_t jump;

        s_random_sheet(&sheet, &state);
        mask = s_random_mask(&part_source, part_words, row_offsets, spans,
            &state);

        reference = bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE);
        jump = bp_place_bottom_left(&sheet, &mask, BP_PLACE_JUMP);

        CHECK(jump.found == reference.found);
        if (reference.found) {
            CHECK(jump.x_cells == reference.x_cells);
            CHECK(jump.y_cells == reference.y_cells);
            /* The reported position must really be free and in bounds. */
            CHECK(bp_mask_in_bounds(&sheet, &mask, jump.x_cells, jump.y_cells));
            CHECK(!bp_mask_overlaps(&sheet, &mask, jump.x_cells,
                jump.y_cells));
            ++found_count;
        }
        reference_checks += reference.candidate_checks;
        jump_checks += jump.candidate_checks;
    }

    /* The random instances must not all be trivial misses. */
    CHECK(found_count > 3000U);
    /* Skipping positions can never require more candidates than testing all. */
    CHECK(jump_checks <= reference_checks);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_invalid_mode(void)
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
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 2U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    (void) bp_place_bottom_left(&sheet, &mask, (bp_place_mode_t) 7);
}

static void
s_body_corrupt_mask(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 8U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[3] = { 0U, 1U, 2U };
    bp_span_t spans[2] = { { 0U, 2U }, { 0U, 2U } };
    /* Claims a width of 4 while no span reaches it: not a tight box. */
    bp_mask_t mask = {
        .width_cells = 4U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .spans = spans,
        .span_count = 2U,
        .area_cells = 4U
    };

    (void) bp_place_bottom_left(&sheet, &mask, BP_PLACE_REFERENCE);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_empty_sheet();
    s_test_oversized_is_not_found();
    s_test_concave_part_fills_notch();
    s_test_part_nests_inside_hole();
    s_test_jump_matches_reference();

    t_expect_abort(s_body_invalid_mode, "invalid mode");
    t_expect_abort(s_body_corrupt_mask, "corrupt mask");

    (void) printf("test_bp_place: all checks passed\n");
    return 0;
}
