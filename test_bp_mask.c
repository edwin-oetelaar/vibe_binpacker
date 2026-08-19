#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"

/* Scratch large enough for every mask this file builds. */
#define S_MAX_ROWS 32U
#define S_MAX_SPANS 128U

/* A rectangle is one full-width span per row, and its area is exact. */
static void
s_test_rectangle(void)
{
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 7U,
        .height_cells = 4U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
    uint32_t row;

    CHECK(mask.width_cells == 7U);
    CHECK(mask.height_cells == 4U);
    CHECK(mask.span_count == 4U);
    CHECK(mask.area_cells == 28U);
    for (row = 0U; row < 4U; ++row) {
        CHECK(mask.row_offsets[row + 1U] - mask.row_offsets[row] == 1U);
        CHECK(mask.spans[mask.row_offsets[row]].x_cells == 0U);
        CHECK(mask.spans[mask.row_offsets[row]].length_cells == 7U);
    }
}

/* Builds a bitmap from a row-major ASCII picture, bottom row written last. */
static bp_bitmap_t
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
        /* rows[0] is the top line of the picture, so flip it into cell space. */
        const char *line = rows[height_cells - 1U - y];
        uint32_t x;

        for (x = 0U; x < width_cells; ++x) {
            CHECK(line[x] == '#' || line[x] == '.');
            if (line[x] == '#')
                bp_bitmap_set_span(bitmap, y, x, 1U);
        }
    }
    return *bitmap;
}

/* An L shape has one span per row, but of two different widths. */
static void
s_test_l_shape_from_bitmap(void)
{
    static const char *picture[] = {
        "##..",
        "##..",
        "####",
        "####"
    };
    uint64_t words[4];
    bp_bitmap_t source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask;

    (void) s_picture(&source, words, 4U, 4U, 4U, picture);
    mask = bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    CHECK(mask.width_cells == 4U);
    CHECK(mask.height_cells == 4U);
    CHECK(mask.span_count == 4U);
    CHECK(mask.area_cells == 12U);
    /* Rows run bottom to top: the two wide rows come first. */
    CHECK(mask.spans[0].length_cells == 4U);
    CHECK(mask.spans[1].length_cells == 4U);
    CHECK(mask.spans[2].length_cells == 2U);
    CHECK(mask.spans[3].length_cells == 2U);
}

/*
 * A ring becomes two spans in the rows crossing its hole. Nothing else in the
 * codebase needs to know a hole exists, which is the point of this
 * representation.
 */
static void
s_test_ring_from_bitmap(void)
{
    static const char *picture[] = {
        "#####",
        "#...#",
        "#...#",
        "#####"
    };
    uint64_t words[4];
    bp_bitmap_t source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask;

    (void) s_picture(&source, words, 4U, 5U, 4U, picture);
    mask = bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    CHECK(mask.span_count == 6U);
    CHECK(mask.area_cells == 14U);
    /* Bottom and top rows are solid, the two middle rows are split. */
    CHECK(mask.row_offsets[1] - mask.row_offsets[0] == 1U);
    CHECK(mask.row_offsets[2] - mask.row_offsets[1] == 2U);
    CHECK(mask.row_offsets[3] - mask.row_offsets[2] == 2U);
    CHECK(mask.row_offsets[4] - mask.row_offsets[3] == 1U);
}

/* Bounds are half-open, so a mask may end exactly on the sheet edge. */
static void
s_test_in_bounds(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 4U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
    uint32_t oversize_offsets[S_MAX_ROWS + 1U];
    bp_span_t oversize_spans[S_MAX_SPANS];
    bp_mask_t oversize = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 11U,
        .height_cells = 1U,
        .row_offsets = oversize_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = oversize_spans,
        .span_capacity = S_MAX_SPANS
    });

    CHECK(bp_mask_in_bounds(&sheet, &mask, 0U, 0U) == 1);
    CHECK(bp_mask_in_bounds(&sheet, &mask, 6U, 2U) == 1);
    CHECK(bp_mask_in_bounds(&sheet, &mask, 7U, 2U) == 0);
    CHECK(bp_mask_in_bounds(&sheet, &mask, 6U, 3U) == 0);
    CHECK(bp_mask_in_bounds(&sheet, &oversize, 0U, 0U) == 0);
}

/* Parts whose edges touch do not overlap, because cell ranges are half-open. */
static void
s_test_touching_is_not_overlap(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 4U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    bp_mask_blit(&sheet, &mask, 0U, 0U);
    CHECK(bp_bitmap_occupied_count(&sheet) == mask.area_cells);
    CHECK(bp_mask_overlaps(&sheet, &mask, 3U, 0U) == 1);
    CHECK(bp_mask_overlaps(&sheet, &mask, 4U, 0U) == 0);
    CHECK(bp_mask_overlaps(&sheet, &mask, 0U, 1U) == 1);
    CHECK(bp_mask_overlaps(&sheet, &mask, 0U, 2U) == 0);

    bp_mask_blit(&sheet, &mask, 4U, 0U);
    CHECK(bp_bitmap_occupied_count(&sheet) == 2U * mask.area_cells);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_untight_source(void)
{
    /* Column 4 is empty, so the bounding box is not tight. */
    static const char *picture[] = {
        "####.",
        "####."
    };
    uint64_t words[2];
    bp_bitmap_t source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];

    (void) s_picture(&source, words, 2U, 5U, 2U, picture);
    (void) bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });
}

static void
s_body_span_capacity_too_small(void)
{
    static const char *picture[] = {
        "#####",
        "#...#",
        "#####"
    };
    uint64_t words[3];
    bp_bitmap_t source;
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[2];

    (void) s_picture(&source, words, 3U, 5U, 3U, picture);
    (void) bp_mask_from_bitmap((bp_mask_from_bitmap_args_t) {
        .source = &source,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = 2U
    });
}

static void
s_body_blit_over_occupied(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 4U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    bp_mask_blit(&sheet, &mask, 0U, 0U);
    bp_mask_blit(&sheet, &mask, 3U, 0U);
}

static void
s_body_blit_out_of_bounds(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    uint32_t row_offsets[S_MAX_ROWS + 1U];
    bp_span_t spans[S_MAX_SPANS];
    bp_mask_t mask = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = 4U,
        .height_cells = 2U,
        .row_offsets = row_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = spans,
        .span_capacity = S_MAX_SPANS
    });

    bp_mask_blit(&sheet, &mask, 7U, 0U);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_rectangle();
    s_test_l_shape_from_bitmap();
    s_test_ring_from_bitmap();
    s_test_in_bounds();
    s_test_touching_is_not_overlap();

    t_expect_abort(s_body_untight_source, "untight source");
    t_expect_abort(s_body_span_capacity_too_small, "span capacity");
    t_expect_abort(s_body_blit_over_occupied, "blit over occupied");
    t_expect_abort(s_body_blit_out_of_bounds, "blit out of bounds");

    (void) printf("test_bp_mask: all checks passed\n");
    return 0;
}
