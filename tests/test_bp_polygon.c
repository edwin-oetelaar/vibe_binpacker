#include "test_support.h"

#include "bp_polygon.h"

#include <math.h>

/* A unit square wound counter-clockwise, scaled by size. */
static void
s_square(bp_point_t *points, double x, double y, double size)
{
    points[0].x_mm = x;
    points[0].y_mm = y;
    points[1].x_mm = x + size;
    points[1].y_mm = y;
    points[2].x_mm = x + size;
    points[2].y_mm = y + size;
    points[3].x_mm = x;
    points[3].y_mm = y + size;
}

/* Counts and bounds must cover every ring, not only the first. */
static void
s_test_counts_and_bounds(void)
{
    bp_point_t points[8];
    uint32_t ring_offsets[3] = { 0U, 4U, 8U };
    bp_polygon_t polygon = { points, ring_offsets, 2U };
    bp_box_t box;

    s_square(points, 10.0, 20.0, 100.0);
    s_square(points + 4, 40.0, 50.0, 20.0);

    CHECK(bp_polygon_point_count(&polygon) == 8U);
    box = bp_polygon_bounds(&polygon);
    CHECK(box.min_x_mm == 10.0);
    CHECK(box.min_y_mm == 20.0);
    CHECK(box.max_x_mm == 110.0);
    CHECK(box.max_y_mm == 120.0);
}

/* Signed area carries the winding, which is used only to reject degeneracy. */
static void
s_test_signed_area(void)
{
    bp_point_t counter[4];
    bp_point_t clockwise[4];
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t ccw = { counter, ring_offsets, 1U };
    bp_polygon_t cw = { clockwise, ring_offsets, 1U };

    s_square(counter, 0.0, 0.0, 10.0);
    clockwise[0] = counter[0];
    clockwise[1] = counter[3];
    clockwise[2] = counter[2];
    clockwise[3] = counter[1];

    CHECK(fabs(bp_polygon_ring_signed_area_mm2(&ccw, 0U) - 100.0) < 1e-9);
    CHECK(fabs(bp_polygon_ring_signed_area_mm2(&cw, 0U) + 100.0) < 1e-9);
}

/* Rotation is about the origin, and four quarter turns return the original. */
static void
s_test_rotate(void)
{
    bp_point_t points[4];
    bp_point_t turned[4];
    bp_point_t back[4];
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };
    bp_polygon_t rotated = { turned, ring_offsets, 1U };
    uint32_t index;

    s_square(points, 1.0, 2.0, 3.0);
    bp_polygon_rotate(turned, 4U, &polygon, 90.0);

    /* (1, 2) turned a quarter turn about the origin becomes (-2, 1). */
    CHECK(fabs(turned[0].x_mm + 2.0) < 1e-9);
    CHECK(fabs(turned[0].y_mm - 1.0) < 1e-9);

    bp_polygon_rotate(back, 4U, &rotated, -90.0);
    for (index = 0U; index < 4U; ++index) {
        CHECK(fabs(back[index].x_mm - points[index].x_mm) < 1e-9);
        CHECK(fabs(back[index].y_mm - points[index].y_mm) < 1e-9);
    }
}

/* A concave outline with a hole is valid and simple, both rings included. */
static void
s_test_accepts_l_shape_with_hole(void)
{
    bp_point_t points[10] = {
        { 0.0, 0.0 }, { 60.0, 0.0 }, { 60.0, 20.0 }, { 20.0, 20.0 },
        { 20.0, 50.0 }, { 0.0, 50.0 },
        { 5.0, 5.0 }, { 15.0, 5.0 }, { 15.0, 15.0 }, { 5.0, 15.0 }
    };
    uint32_t ring_offsets[3] = { 0U, 6U, 10U };
    bp_polygon_t polygon = { points, ring_offsets, 2U };

    bp_polygon_require_valid(&polygon);
    bp_polygon_require_simple(&polygon);
}

/* Contract bodies each build their own geometry so nothing is shared. */

static void
s_body_too_few_points(void)
{
    bp_point_t points[2] = { { 0.0, 0.0 }, { 1.0, 1.0 } };
    uint32_t ring_offsets[2] = { 0U, 2U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };

    bp_polygon_require_valid(&polygon);
}

static void
s_body_zero_length_edge(void)
{
    bp_point_t points[4] = {
        { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 0.0 }, { 0.0, 10.0 }
    };
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };

    bp_polygon_require_valid(&polygon);
}

static void
s_body_zero_area_ring(void)
{
    bp_point_t points[4] = {
        { 0.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 }, { 5.0, 0.0 }
    };
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };

    bp_polygon_require_valid(&polygon);
}

static void
s_body_bowtie(void)
{
    /* The classic self-crossing quadrilateral. */
    bp_point_t points[4] = {
        { 0.0, 0.0 }, { 10.0, 10.0 }, { 10.0, 0.0 }, { 0.0, 10.0 }
    };
    uint32_t ring_offsets[2] = { 0U, 4U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };

    bp_polygon_require_simple(&polygon);
}

static void
s_body_rings_cross(void)
{
    bp_point_t points[8];
    uint32_t ring_offsets[3] = { 0U, 4U, 8U };
    bp_polygon_t polygon = { points, ring_offsets, 2U };

    s_square(points, 0.0, 0.0, 20.0);
    /* Straddles the outer boundary instead of sitting inside it. */
    s_square(points + 4, 15.0, 5.0, 10.0);
    bp_polygon_require_simple(&polygon);
}

static void
s_body_spike(void)
{
    /* Vertex 2 doubles straight back along the previous edge. */
    bp_point_t points[5] = {
        { 0.0, 0.0 }, { 20.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 10.0 },
        { 0.0, 10.0 }
    };
    uint32_t ring_offsets[2] = { 0U, 5U };
    bp_polygon_t polygon = { points, ring_offsets, 1U };

    bp_polygon_require_simple(&polygon);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_counts_and_bounds();
    s_test_signed_area();
    s_test_rotate();
    s_test_accepts_l_shape_with_hole();

    t_expect_abort(s_body_too_few_points, "too few points");
    t_expect_abort(s_body_zero_length_edge, "zero length edge");
    t_expect_abort(s_body_zero_area_ring, "zero area ring");
    t_expect_abort(s_body_bowtie, "bowtie");
    t_expect_abort(s_body_rings_cross, "rings cross");
    t_expect_abort(s_body_spike, "spike");

    (void) printf("test_bp_polygon: all checks passed\n");
    return 0;
}
