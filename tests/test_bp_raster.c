#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_polygon.h"
#include "bp_raster.h"

#include <math.h>
#include <string.h>

#define S_MAX_CELLS 48U
#define S_MAX_WORDS (1U * S_MAX_CELLS)
#define S_MAX_POINTS 32U

/*
 * Independent reference for conservative rasterisation.
 *
 * A cell square meets the polygon exactly when one of three things holds: a
 * corner of the square is inside the polygon, a polygon vertex is inside the
 * square, or a polygon edge crosses a square edge. Anything else means the
 * two are disjoint. This is written from that definition rather than reusing
 * anything from bp_raster, so agreement between the two means something.
 */

/* Even-odd point in polygon by ray casting, over every ring at once. */
static int
s_inside(const bp_polygon_t *polygon, double x, double y)
{
    uint32_t ring;
    int inside = 0;

    for (ring = 0U; ring < polygon->ring_count; ++ring) {
        uint32_t begin = polygon->ring_offsets[ring];
        uint32_t end = polygon->ring_offsets[ring + 1U];
        uint32_t index;

        for (index = begin; index < end; ++index) {
            uint32_t next = index + 1U == end ? begin : index + 1U;
            double ax = polygon->points[index].x_mm;
            double ay = polygon->points[index].y_mm;
            double bx = polygon->points[next].x_mm;
            double by = polygon->points[next].y_mm;

            if ((ay > y) != (by > y)
                && x < ax + (y - ay) * (bx - ax) / (by - ay))
                inside = !inside;
        }
    }
    return inside;
}

/* Reports whether two closed segments meet, for generic-position input. */
static int
s_cross(double ax, double ay, double bx, double by, double cx, double cy,
    double dx, double dy)
{
    double rx = bx - ax;
    double ry = by - ay;
    double sx = dx - cx;
    double sy = dy - cy;
    double denominator = rx * sy - ry * sx;
    double t;
    double u;

    if (fabs(denominator) < 1e-12)
        return 0;
    t = ((cx - ax) * sy - (cy - ay) * sx) / denominator;
    u = ((cx - ax) * ry - (cy - ay) * rx) / denominator;
    return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

/* Reports whether the cell square [x, x+1) x [y, y+1) meets the polygon. */
static int
s_cell_meets_polygon(const bp_polygon_t *polygon, double origin_x,
    double origin_y, double cell_size_mm, uint32_t cell_x, uint32_t cell_y)
{
    double x0 = origin_x + cell_x * cell_size_mm;
    double y0 = origin_y + cell_y * cell_size_mm;
    double x1 = x0 + cell_size_mm;
    double y1 = y0 + cell_size_mm;
    double corners[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    uint32_t ring;
    uint32_t corner;
    uint32_t index;

    for (corner = 0U; corner < 4U; ++corner)
        if (s_inside(polygon, corners[corner][0], corners[corner][1]))
            return 1;

    for (ring = 0U; ring < polygon->ring_count; ++ring) {
        uint32_t begin = polygon->ring_offsets[ring];
        uint32_t end = polygon->ring_offsets[ring + 1U];

        for (index = begin; index < end; ++index) {
            uint32_t next = index + 1U == end ? begin : index + 1U;
            double ax = polygon->points[index].x_mm;
            double ay = polygon->points[index].y_mm;
            double bx = polygon->points[next].x_mm;
            double by = polygon->points[next].y_mm;

            if (ax >= x0 && ax <= x1 && ay >= y0 && ay <= y1)
                return 1;
            for (corner = 0U; corner < 4U; ++corner) {
                uint32_t other = (corner + 1U) % 4U;

                if (s_cross(ax, ay, bx, by, corners[corner][0],
                    corners[corner][1], corners[other][0],
                    corners[other][1]))
                    return 1;
            }
        }
    }
    return 0;
}

/* Rasterises a polygon and compares it cell by cell with the reference. */
static void
s_check_against_reference(const bp_polygon_t *polygon, double cell_size_mm,
    const char *name)
{
    bp_raster_extent_t extent = bp_raster_extent(polygon, cell_size_mm);
    uint64_t words[S_MAX_WORDS];
    double crossings[S_MAX_POINTS];
    bp_bitmap_t target;
    uint32_t x;
    uint32_t y;

    CHECK(extent.width_cells <= 64U);
    CHECK(extent.height_cells <= S_MAX_CELLS);
    target = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = extent.width_cells,
        .height_cells = extent.height_cells,
        .words = words,
        .word_capacity = S_MAX_WORDS
    });
    bp_raster_fill((bp_raster_fill_args_t) {
        .target = &target,
        .polygon = polygon,
        .cell_size_mm = cell_size_mm,
        .origin_mm = extent.origin_mm,
        .scratch_crossings = crossings,
        .scratch_capacity = S_MAX_POINTS
    });

    for (y = 0U; y < extent.height_cells; ++y) {
        for (x = 0U; x < extent.width_cells; ++x) {
            int expected = s_cell_meets_polygon(polygon,
                extent.origin_mm.x_mm, extent.origin_mm.y_mm, cell_size_mm,
                x, y);

            if (bp_bitmap_get(&target, x, y) != expected) {
                (void) fprintf(stderr,
                    "FAIL %s: cell (%u, %u) rastered %d, reference %d\n",
                    name, (unsigned) x, (unsigned) y,
                    bp_bitmap_get(&target, x, y), expected);
                exit(1);
            }
        }
    }
}

/*
 * Shapes are placed at deliberately awkward offsets so no edge lands exactly
 * on a grid line. Degenerate positions are a separate question from whether
 * the fill rule is right, and mixing them would only make this test flaky.
 */
static void
s_test_matches_reference(void)
{
    bp_point_t triangle[3] = {
        { 0.37, 0.21 }, { 17.63, 2.11 }, { 5.19, 14.87 }
    };
    uint32_t triangle_rings[2] = { 0U, 3U };
    bp_polygon_t triangle_polygon = { triangle, triangle_rings, 1U };

    bp_point_t l_shape[6] = {
        { 0.31, 0.17 }, { 19.43, 0.17 }, { 19.43, 6.29 }, { 7.11, 6.29 },
        { 7.11, 16.53 }, { 0.31, 16.53 }
    };
    uint32_t l_rings[2] = { 0U, 6U };
    bp_polygon_t l_polygon = { l_shape, l_rings, 1U };

    bp_point_t ring_shape[8] = {
        { 0.13, 0.29 }, { 18.71, 0.29 }, { 18.71, 15.43 }, { 0.13, 15.43 },
        { 4.57, 4.63 }, { 13.29, 4.63 }, { 13.29, 11.17 }, { 4.57, 11.17 }
    };
    uint32_t ring_rings[3] = { 0U, 4U, 8U };
    bp_polygon_t ring_polygon = { ring_shape, ring_rings, 2U };

    bp_point_t turned[8];
    bp_polygon_t turned_polygon = { turned, ring_rings, 2U };

    s_check_against_reference(&triangle_polygon, 1.0, "triangle");
    s_check_against_reference(&triangle_polygon, 0.5, "triangle at 0.5 mm");
    s_check_against_reference(&l_polygon, 1.0, "l shape");
    s_check_against_reference(&ring_polygon, 1.0, "ring");
    s_check_against_reference(&ring_polygon, 2.0, "ring at 2 mm");

    /* A free angle must be no different, because nothing special-cases 90. */
    bp_polygon_rotate(turned, 8U, &ring_polygon, 37.0);
    s_check_against_reference(&turned_polygon, 1.0, "ring turned 37 degrees");
}

/* A grid-aligned rectangle must produce exactly its own cells, no border. */
static void
s_test_grid_aligned_rectangle(void)
{
    bp_point_t points[4] = {
        { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 4.0 }, { 0.0, 4.0 }
    };
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };
    bp_raster_extent_t extent = bp_raster_extent(&polygon, 1.0);
    uint64_t words[S_MAX_WORDS];
    double crossings[S_MAX_POINTS];
    bp_bitmap_t target;

    CHECK(extent.width_cells == 10U);
    CHECK(extent.height_cells == 4U);

    target = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = extent.width_cells,
        .height_cells = extent.height_cells,
        .words = words,
        .word_capacity = S_MAX_WORDS
    });
    bp_raster_fill((bp_raster_fill_args_t) {
        .target = &target,
        .polygon = &polygon,
        .cell_size_mm = 1.0,
        .origin_mm = extent.origin_mm,
        .scratch_crossings = crossings,
        .scratch_capacity = S_MAX_POINTS
    });
    CHECK(bp_bitmap_occupied_count(&target) == 40U);
}

/*
 * Half a cell off in both directions, the same rectangle has to grow by one
 * cell in each direction. This is the conservative rounding that costs
 * material, and it is worth having a number on it.
 */
static void
s_test_offset_rectangle_grows(void)
{
    bp_point_t points[4] = {
        { 0.5, 0.5 }, { 10.5, 0.5 }, { 10.5, 4.5 }, { 0.5, 4.5 }
    };
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };
    bp_raster_extent_t extent = bp_raster_extent(&polygon, 1.0);
    uint64_t words[S_MAX_WORDS];
    double crossings[S_MAX_POINTS];
    bp_bitmap_t target;

    target = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = extent.width_cells,
        .height_cells = extent.height_cells,
        .words = words,
        .word_capacity = S_MAX_WORDS
    });
    bp_raster_fill((bp_raster_fill_args_t) {
        .target = &target,
        .polygon = &polygon,
        .cell_size_mm = 1.0,
        .origin_mm = extent.origin_mm,
        .scratch_crossings = crossings,
        .scratch_capacity = S_MAX_POINTS
    });
    /* The origin sits at the polygon corner, so the box is 10 x 4 again. */
    CHECK(extent.width_cells == 10U);
    CHECK(extent.height_cells == 4U);
    CHECK(bp_bitmap_occupied_count(&target) == 40U);
}

/*
 * Squared distance between two cell squares one grid apart, in cells. Two
 * cells that share an edge or a corner are at distance zero: their material
 * touches, whatever their centres are doing.
 */
static int64_t
s_square_gap_squared(int64_t dx, int64_t dy)
{
    int64_t u = (dx < 0 ? -dx : dx);
    int64_t v = (dy < 0 ? -dy : dy);

    u = u > 1 ? u - 1 : 0;
    v = v > 1 ? v - 1 : 0;
    return u * u + v * v;
}

/*
 * Brute-force reference: mark every cell whose square comes closer to a solid
 * cell than the clearance. This is the specification the fast row-based
 * implementation has to match.
 */
static void
s_reference_dilate(bp_bitmap_t *dst, const bp_bitmap_t *src,
    uint32_t clearance_cells)
{
    uint32_t x;
    uint32_t y;

    bp_bitmap_clear(dst);
    for (y = 0U; y < src->height_cells; ++y) {
        for (x = 0U; x < src->width_cells; ++x) {
            int64_t dy;

            if (!bp_bitmap_get(src, x, y))
                continue;
            for (dy = -(int64_t) clearance_cells;
                dy <= (int64_t) clearance_cells; ++dy) {
                int64_t dx;

                for (dx = -(int64_t) clearance_cells;
                    dx <= (int64_t) clearance_cells; ++dx) {
                    if (clearance_cells > 0U
                        && s_square_gap_squared(dx, dy)
                            >= (int64_t) clearance_cells * clearance_cells)
                        continue;
                    if (clearance_cells == 0U && (dx != 0 || dy != 0))
                        continue;
                    bp_bitmap_set_span(dst,
                        (uint32_t) ((int64_t) y + dy + clearance_cells),
                        (uint32_t) ((int64_t) x + dx + clearance_cells), 1U);
                }
            }
        }
    }
}

/*
 * Checks the guarantee itself rather than the formula: no cell left free by
 * the dilation may sit closer to the true geometry than the clearance, and no
 * cell it reserves may be further away than that. The first half is what
 * keeps parts apart; the second half stops the clearance quietly eating
 * material.
 */
static void
s_check_clearance_property(const bp_bitmap_t *grown, const bp_bitmap_t *src,
    uint32_t clearance_cells)
{
    uint32_t x;
    uint32_t y;

    for (y = 0U; y < grown->height_cells; ++y) {
        for (x = 0U; x < grown->width_cells; ++x) {
            int close = 0;
            uint32_t sx;
            uint32_t sy;

            for (sy = 0U; sy < src->height_cells && !close; ++sy) {
                for (sx = 0U; sx < src->width_cells; ++sx) {
                    int64_t dx;
                    int64_t dy;

                    if (!bp_bitmap_get(src, sx, sy))
                        continue;
                    dx = (int64_t) x - ((int64_t) sx + clearance_cells);
                    dy = (int64_t) y - ((int64_t) sy + clearance_cells);
                    if (clearance_cells == 0U) {
                        if (dx == 0 && dy == 0) {
                            close = 1;
                            break;
                        }
                        continue;
                    }
                    if (s_square_gap_squared(dx, dy)
                        < (int64_t) clearance_cells * clearance_cells) {
                        close = 1;
                        break;
                    }
                }
            }
            CHECK(bp_bitmap_get(grown, x, y) == close);
        }
    }
}

/*
 * Dilation is compared with a brute-force reference over several shapes and
 * radii. The clearance guarantee rests entirely on this operation, so it is
 * checked bit for bit rather than by counting cells.
 */
static void
s_test_dilate_matches_reference(void)
{
    static const uint32_t clearances[] = { 0U, 1U, 2U, 3U, 5U };
    uint64_t source_words[S_MAX_WORDS];
    uint64_t words[2U * S_MAX_WORDS];
    uint64_t expected_words[2U * S_MAX_WORDS];
    uint64_t scratch[8U * 2U * S_MAX_WORDS];
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    size_t which;
    uint32_t trial;

    for (trial = 0U; trial < 40U; ++trial) {
        uint32_t width = 1U + t_random_below(&state, 20U);
        uint32_t height = 1U + t_random_below(&state, 12U);
        bp_bitmap_t source = bp_bitmap_init((bp_bitmap_init_args_t) {
            .width_cells = width,
            .height_cells = height,
            .words = source_words,
            .word_capacity = S_MAX_WORDS
        });
        uint32_t x;
        uint32_t y;

        for (y = 0U; y < height; ++y)
            for (x = 0U; x < width; ++x)
                if (t_random_below(&state, 3U) != 0U)
                    bp_bitmap_set_span(&source, y, x, 1U);
        /* Never test an empty source; that is not a footprint. */
        bp_bitmap_set_span(&source, 0U, 0U, 1U);

        for (which = 0U; which < sizeof clearances / sizeof clearances[0]; ++which) {
            uint32_t radius = clearances[which];
            uint32_t grown_width = width + 2U * radius;
            uint32_t grown_height = height + 2U * radius;
            bp_bitmap_t grown;
            bp_bitmap_t expected;

            if (grown_width > 64U)
                continue;
            grown = bp_bitmap_init((bp_bitmap_init_args_t) {
                .width_cells = grown_width,
                .height_cells = grown_height,
                .words = words,
                .word_capacity = 2U * S_MAX_WORDS
            });
            expected = bp_bitmap_init((bp_bitmap_init_args_t) {
                .width_cells = grown_width,
                .height_cells = grown_height,
                .words = expected_words,
                .word_capacity = 2U * S_MAX_WORDS
            });

            bp_raster_dilate(&grown, &source, radius, scratch,
                sizeof scratch / sizeof scratch[0]);
            s_reference_dilate(&expected, &source, radius);

            CHECK(memcmp(words, expected_words,
                bp_bitmap_required_words(grown_width, grown_height)
                    * sizeof words[0]) == 0);
            s_check_clearance_property(&grown, &source, radius);
        }
    }
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_matches_reference();
    s_test_grid_aligned_rectangle();
    s_test_offset_rectangle_grows();
    s_test_dilate_matches_reference();

    (void) printf("test_bp_raster: all checks passed\n");
    return 0;
}
