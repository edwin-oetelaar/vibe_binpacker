#pragma once

#include <stddef.h>
#include <stdint.h>

/* A vertex in millimetres. Geometry is metric until the rasteriser. */
typedef struct {
    double x_mm;
    double y_mm;
} bp_point_t;

/*
 * A polygon as one or more closed rings over a shared vertex array. Ring r
 * occupies points[ring_offsets[r] .. ring_offsets[r + 1]) and closes
 * implicitly from its last vertex back to its first.
 *
 * Filling uses the even-odd rule, so **ring orientation does not matter**: a
 * ring inside another ring is a hole whichever way round it is wound. That is
 * deliberate, because real CAD input arrives with arbitrary winding and
 * silently repairing it would be a guess. By convention ring 0 is the outer
 * boundary and the rest are holes, but nothing depends on it.
 *
 * Arrays are caller owned. The module allocates nothing.
 */
typedef struct {
    const bp_point_t *points;
    const uint32_t *ring_offsets;
    uint32_t ring_count;
} bp_polygon_t;

typedef struct {
    double min_x_mm;
    double min_y_mm;
    double max_x_mm;
    double max_y_mm;
} bp_box_t;

/* Returns the total number of vertices across every ring. */
uint32_t
bp_polygon_point_count(const bp_polygon_t *polygon);

/*
 * Checks the cheap structural invariants: at least one ring, at least three
 * vertices per ring, offsets that only grow, finite coordinates, no
 * zero-length edges and no ring of zero area. Self-intersection is not
 * covered here because it costs O(n^2); see bp_polygon_require_simple.
 */
void
bp_polygon_require_valid(const bp_polygon_t *polygon);

/*
 * Checks that no two edges cross, including edges of different rings, and
 * that no vertex doubles back on itself. Costs O(n^2) in the vertex count, so
 * it belongs at load time, once per part, and not in any search loop.
 */
void
bp_polygon_require_simple(const bp_polygon_t *polygon);

/* Returns the axis-aligned bounding box over every ring. */
bp_box_t
bp_polygon_bounds(const bp_polygon_t *polygon);

/*
 * Returns one ring's signed area: positive counter-clockwise, negative
 * clockwise. Used to reject degenerate rings, not to decide what is a hole.
 */
double
bp_polygon_ring_signed_area_mm2(const bp_polygon_t *polygon, uint32_t ring);

/*
 * Rotates every vertex about the origin by angle_degrees into dst, which must
 * hold at least bp_polygon_point_count entries and must not alias the source.
 * The rings are unchanged, so the caller reuses the same ring_offsets.
 */
void
bp_polygon_rotate(bp_point_t *dst, size_t dst_capacity,
    const bp_polygon_t *polygon, double angle_degrees);
