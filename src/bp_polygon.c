#include "bp_polygon.h"

#include "bp_contract.h"

#include <math.h>

/* Tolerance for treating a cross product as zero, in square millimetres. */
#define S_EPSILON 1e-9

/* Returns the total number of vertices across every ring. */
uint32_t
bp_polygon_point_count(const bp_polygon_t *polygon)
{
    BP_REQUIRE(polygon);
    BP_REQUIRE(polygon->ring_offsets);
    BP_REQUIRE(polygon->ring_count > 0U);
    return polygon->ring_offsets[polygon->ring_count];
}

/* Returns the vertex after index inside its ring, wrapping at the end. */
static const bp_point_t *
s_next_in_ring(const bp_polygon_t *polygon, uint32_t ring, uint32_t index)
{
    uint32_t begin = polygon->ring_offsets[ring];
    uint32_t end = polygon->ring_offsets[ring + 1U];

    BP_REQUIRE(index >= begin);
    BP_REQUIRE(index < end);
    return &polygon->points[index + 1U == end ? begin : index + 1U];
}

/* Returns one ring's signed area by the shoelace formula. */
double
bp_polygon_ring_signed_area_mm2(const bp_polygon_t *polygon, uint32_t ring)
{
    uint32_t index;
    double twice_area = 0.0;

    BP_REQUIRE(polygon);
    BP_REQUIRE(polygon->points);
    BP_REQUIRE(ring < polygon->ring_count);

    for (index = polygon->ring_offsets[ring];
        index < polygon->ring_offsets[ring + 1U]; ++index) {
        const bp_point_t *a = &polygon->points[index];
        const bp_point_t *b = s_next_in_ring(polygon, ring, index);

        twice_area += a->x_mm * b->y_mm - b->x_mm * a->y_mm;
    }
    return twice_area * 0.5;
}

/* Checks the cheap structural invariants of a polygon. */
void
bp_polygon_require_valid(const bp_polygon_t *polygon)
{
    uint32_t ring;

    BP_REQUIRE(polygon);
    BP_REQUIRE(polygon->points);
    BP_REQUIRE(polygon->ring_offsets);
    BP_REQUIRE(polygon->ring_count > 0U);
    BP_REQUIRE(polygon->ring_offsets[0] == 0U);

    for (ring = 0U; ring < polygon->ring_count; ++ring) {
        uint32_t begin = polygon->ring_offsets[ring];
        uint32_t end = polygon->ring_offsets[ring + 1U];
        uint32_t index;

        BP_REQUIRE(end >= begin + 3U);
        for (index = begin; index < end; ++index) {
            const bp_point_t *a = &polygon->points[index];
            const bp_point_t *b = s_next_in_ring(polygon, ring, index);

            BP_REQUIRE(isfinite(a->x_mm));
            BP_REQUIRE(isfinite(a->y_mm));
            /* A zero-length edge has no direction, so nothing downstream
               could rasterise or offset it sensibly. */
            BP_REQUIRE(a->x_mm != b->x_mm || a->y_mm != b->y_mm);
        }
        BP_REQUIRE(fabs(bp_polygon_ring_signed_area_mm2(polygon, ring))
            > S_EPSILON);
    }
}

/* Returns the sign of the turn from a->b to a->c; 0 when they are collinear. */
static int
s_orientation(const bp_point_t *a, const bp_point_t *b, const bp_point_t *c)
{
    double cross = (b->x_mm - a->x_mm) * (c->y_mm - a->y_mm)
        - (b->y_mm - a->y_mm) * (c->x_mm - a->x_mm);

    if (cross > S_EPSILON)
        return 1;
    if (cross < -S_EPSILON)
        return -1;
    return 0;
}

/* Reports whether collinear point c lies within the closed segment a-b. */
static int
s_on_segment(const bp_point_t *a, const bp_point_t *b, const bp_point_t *c)
{
    double low_x = a->x_mm < b->x_mm ? a->x_mm : b->x_mm;
    double high_x = a->x_mm < b->x_mm ? b->x_mm : a->x_mm;
    double low_y = a->y_mm < b->y_mm ? a->y_mm : b->y_mm;
    double high_y = a->y_mm < b->y_mm ? b->y_mm : a->y_mm;

    return c->x_mm >= low_x - S_EPSILON && c->x_mm <= high_x + S_EPSILON
        && c->y_mm >= low_y - S_EPSILON && c->y_mm <= high_y + S_EPSILON;
}

/* Reports whether two closed segments share any point. */
static int
s_segments_touch(const bp_point_t *a1, const bp_point_t *a2,
    const bp_point_t *b1, const bp_point_t *b2)
{
    int d1 = s_orientation(a1, a2, b1);
    int d2 = s_orientation(a1, a2, b2);
    int d3 = s_orientation(b1, b2, a1);
    int d4 = s_orientation(b1, b2, a2);

    if (d1 != d2 && d3 != d4)
        return 1;
    if (d1 == 0 && s_on_segment(a1, a2, b1))
        return 1;
    if (d2 == 0 && s_on_segment(a1, a2, b2))
        return 1;
    if (d3 == 0 && s_on_segment(b1, b2, a1))
        return 1;
    if (d4 == 0 && s_on_segment(b1, b2, a2))
        return 1;
    return 0;
}

/* Reports whether two edge indices are neighbours in the same ring. */
static int
s_are_neighbours(const bp_polygon_t *polygon, uint32_t ring, uint32_t first,
    uint32_t second)
{
    uint32_t begin = polygon->ring_offsets[ring];
    uint32_t end = polygon->ring_offsets[ring + 1U];
    uint32_t count = end - begin;
    uint32_t a = first - begin;
    uint32_t b = second - begin;
    uint32_t forward = (b + count - a) % count;

    return forward == 1U || forward == count - 1U;
}

/*
 * Rejects self-intersection. Edges that meet at a shared vertex are allowed,
 * but only if they actually turn: a pair that doubles straight back is a
 * zero-width spike, which has no interior and would rasterise into a stray
 * line of cells.
 */
void
bp_polygon_require_simple(const bp_polygon_t *polygon)
{
    uint32_t total;
    uint32_t ring;
    uint32_t other_ring;

    bp_polygon_require_valid(polygon);
    total = bp_polygon_point_count(polygon);
    BP_REQUIRE(total >= 3U);

    for (ring = 0U; ring < polygon->ring_count; ++ring) {
        uint32_t begin = polygon->ring_offsets[ring];
        uint32_t end = polygon->ring_offsets[ring + 1U];
        uint32_t first;

        for (first = begin; first < end; ++first) {
            uint32_t second_index = first + 1U == end ? begin : first + 1U;
            uint32_t third_index = second_index + 1U == end
                ? begin
                : second_index + 1U;
            const bp_point_t *a1 = &polygon->points[first];
            const bp_point_t *a2 = &polygon->points[second_index];
            const bp_point_t *a3 = &polygon->points[third_index];
            uint32_t second;

            /* A reversal at the shared vertex means a degenerate spike. */
            if (s_orientation(a1, a2, a3) == 0) {
                double back = (a1->x_mm - a2->x_mm) * (a3->x_mm - a2->x_mm)
                    + (a1->y_mm - a2->y_mm) * (a3->y_mm - a2->y_mm);

                BP_REQUIRE(back <= S_EPSILON);
            }

            for (second = first + 1U; second < end; ++second) {
                if (s_are_neighbours(polygon, ring, first, second))
                    continue;
                BP_REQUIRE(!s_segments_touch(a1, a2,
                    &polygon->points[second],
                    s_next_in_ring(polygon, ring, second)));
            }

            for (other_ring = ring + 1U; other_ring < polygon->ring_count;
                ++other_ring) {
                uint32_t index;

                for (index = polygon->ring_offsets[other_ring];
                    index < polygon->ring_offsets[other_ring + 1U]; ++index)
                    BP_REQUIRE(!s_segments_touch(a1, a2,
                        &polygon->points[index],
                        s_next_in_ring(polygon, other_ring, index)));
            }
        }
    }
}

/* Returns the axis-aligned bounding box over every ring. */
bp_box_t
bp_polygon_bounds(const bp_polygon_t *polygon)
{
    bp_box_t box;
    uint32_t total;
    uint32_t index;

    bp_polygon_require_valid(polygon);
    total = bp_polygon_point_count(polygon);

    box.min_x_mm = polygon->points[0].x_mm;
    box.max_x_mm = polygon->points[0].x_mm;
    box.min_y_mm = polygon->points[0].y_mm;
    box.max_y_mm = polygon->points[0].y_mm;

    for (index = 1U; index < total; ++index) {
        const bp_point_t *point = &polygon->points[index];

        if (point->x_mm < box.min_x_mm)
            box.min_x_mm = point->x_mm;
        if (point->x_mm > box.max_x_mm)
            box.max_x_mm = point->x_mm;
        if (point->y_mm < box.min_y_mm)
            box.min_y_mm = point->y_mm;
        if (point->y_mm > box.max_y_mm)
            box.max_y_mm = point->y_mm;
    }
    return box;
}

/* Rotates every vertex about the origin into dst. */
void
bp_polygon_rotate(bp_point_t *dst, size_t dst_capacity,
    const bp_polygon_t *polygon, double angle_degrees)
{
    double radians;
    double cosine;
    double sine;
    uint32_t total;
    uint32_t index;

    BP_REQUIRE(dst);
    BP_REQUIRE(isfinite(angle_degrees));
    bp_polygon_require_valid(polygon);
    total = bp_polygon_point_count(polygon);
    BP_REQUIRE(dst_capacity >= total);
    BP_REQUIRE(dst != polygon->points);

    radians = angle_degrees * (3.14159265358979323846 / 180.0);
    cosine = cos(radians);
    sine = sin(radians);

    for (index = 0U; index < total; ++index) {
        double x = polygon->points[index].x_mm;
        double y = polygon->points[index].y_mm;

        dst[index].x_mm = x * cosine - y * sine;
        dst[index].y_mm = x * sine + y * cosine;
    }
}
