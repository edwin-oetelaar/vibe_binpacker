#include "test_support.h"

#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_penetration.h"
#include "bp_separate.h"

#define S_SIDE 100U
#define S_PARTS 6U
#define S_MAX_ROWS 64U
#define S_MAX_SPANS 64U

/* Backing store for one part, with the grown footprint kept separate. */
typedef struct {
    uint32_t solid_offsets[S_MAX_ROWS + 1U];
    bp_span_t solid_spans[S_MAX_SPANS];
    uint32_t grown_offsets[S_MAX_ROWS + 1U];
    bp_span_t grown_spans[S_MAX_SPANS];
    bp_mask_t solid;
    bp_mask_t grown;
    bp_orientation_t orientation;
} s_storage_t;

/* Everything one scenario needs, so the tests stay readable. */
typedef struct {
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    bp_placement_t placements[S_PARTS];
    uint8_t placed[S_PARTS];
    uint32_t weights[S_PARTS];
    uint64_t overlap[S_PARTS];
    uint32_t counts[S_SIDE * S_SIDE];
    uint64_t prefix[S_SIDE * (S_SIDE + 1U)];
    bp_penetration_t field;
} s_scene_t;

/* Builds a square part of the given side, grown by the clearance. */
static void
s_make_part(s_scene_t *scene, uint32_t index, uint32_t side,
    uint32_t clearance)
{
    s_storage_t *storage = &scene->storage[index];

    storage->solid = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = side,
        .height_cells = side,
        .row_offsets = storage->solid_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = storage->solid_spans,
        .span_capacity = S_MAX_SPANS
    });
    storage->grown = bp_mask_rectangle((bp_mask_rectangle_args_t) {
        .width_cells = side + 2U * clearance,
        .height_cells = side + 2U * clearance,
        .row_offsets = storage->grown_offsets,
        .row_offset_capacity = S_MAX_ROWS + 1U,
        .spans = storage->grown_spans,
        .span_capacity = S_MAX_SPANS
    });
    storage->orientation.solid = &storage->solid;
    storage->orientation.collision = &storage->grown;
    storage->orientation.solid_offset_x_cells = clearance;
    storage->orientation.solid_offset_y_cells = clearance;

    scene->parts[index].id = index + 1U;
    scene->parts[index].orientations = &storage->orientation;
    scene->parts[index].orientation_count = 1U;
}

/* Loads the field from the current placements, every part at weight one. */
static void
s_load_field(s_scene_t *scene, uint32_t count)
{
    uint32_t index;

    scene->field = bp_penetration_init((bp_penetration_init_args_t) {
        .width_cells = S_SIDE,
        .height_cells = S_SIDE,
        .counts = scene->counts,
        .count_capacity = sizeof scene->counts / sizeof scene->counts[0],
        .prefix = scene->prefix,
        .prefix_capacity = sizeof scene->prefix / sizeof scene->prefix[0]
    });
    for (index = 0U; index < count; ++index) {
        const bp_orientation_t *option = &scene->storage[index].orientation;

        if (!scene->placed[index])
            continue;
        bp_penetration_add(&scene->field, option->solid,
            scene->placements[index].x_cells + option->solid_offset_x_cells,
            scene->placements[index].y_cells + option->solid_offset_y_cells,
            1U);
    }
}

/*
 * Checks the clearance directly on the geometry, without asking the field.
 * Every pair of true footprints must stay the clearance apart, measured the
 * way bp_raster grows a mask: cell squares, not cell centres.
 */
static void
s_check_clearance(const s_scene_t *scene, uint32_t count, uint32_t clearance)
{
    uint32_t a;

    for (a = 0U; a < count; ++a) {
        uint32_t b;

        if (!scene->placed[a])
            continue;
        for (b = a + 1U; b < count; ++b) {
            const bp_mask_t *first;
            const bp_mask_t *second;
            int64_t ax;
            int64_t ay;
            int64_t bx;
            int64_t by;
            int64_t gap_x;
            int64_t gap_y;

            if (!scene->placed[b])
                continue;
            first = scene->storage[a].orientation.solid;
            second = scene->storage[b].orientation.solid;
            ax = scene->placements[a].x_cells
                + scene->storage[a].orientation.solid_offset_x_cells;
            ay = scene->placements[a].y_cells
                + scene->storage[a].orientation.solid_offset_y_cells;
            bx = scene->placements[b].x_cells
                + scene->storage[b].orientation.solid_offset_x_cells;
            by = scene->placements[b].y_cells
                + scene->storage[b].orientation.solid_offset_y_cells;

            /* Squares, so a shared edge or corner counts as touching. */
            gap_x = bx > ax
                ? bx - (ax + (int64_t) first->width_cells)
                : ax - (bx + (int64_t) second->width_cells);
            gap_y = by > ay
                ? by - (ay + (int64_t) first->height_cells)
                : ay - (by + (int64_t) second->height_cells);
            if (gap_x < 0)
                gap_x = 0;
            if (gap_y < 0)
                gap_y = 0;
            CHECK(gap_x * gap_x + gap_y * gap_y
                >= (int64_t) clearance * clearance);
        }
    }
}

/* Runs the separation over the scene. */
static bp_separate_result_t
s_resolve(s_scene_t *scene, uint32_t count, uint32_t region, uint64_t *state)
{
    return bp_separate_resolve((bp_separate_args_t) {
        .field = &scene->field,
        .parts = scene->parts,
        .part_count = count,
        .placements = scene->placements,
        .placed = scene->placed,
        .region_width_cells = region,
        .region_height_cells = region,
        .max_iterations = 4000U,
        .samples = 32U,
        .patience = 800U,
        .random_state = state,
        .scratch_weights = scene->weights,
        .scratch_overlap = scene->overlap,
        .scratch_capacity = count
    });
}

/*
 * Four squares dropped nearly on top of each other must end up apart. This is
 * the property the whole method rests on: a layout that no sequence of legal
 * placements could reach, walked back to legality.
 */
static void
s_test_resolves_a_pile(void)
{
    static s_scene_t scene;
    uint64_t state = UINT64_C(0x2a2a2a);
    bp_separate_result_t result;
    uint32_t index;

    for (index = 0U; index < 4U; ++index) {
        s_make_part(&scene, index, 40U, 1U);
        scene.placements[index].part_index = index;
        scene.placements[index].part_id = index + 1U;
        scene.placements[index].orientation = 0U;
        scene.placements[index].x_cells = 5U * index;
        scene.placements[index].y_cells = 5U * index;
        scene.placed[index] = 1U;
    }
    s_load_field(&scene, 4U);

    result = s_resolve(&scene, 4U, S_SIDE, &state);
    CHECK(result.resolved);
    CHECK(result.overlap_cells == 0U);
    s_check_clearance(&scene, 4U, 1U);
}

/* The same seed must give the same layout, or nothing here is reproducible. */
static void
s_test_deterministic(void)
{
    static s_scene_t first;
    static s_scene_t second;
    uint64_t state_a = UINT64_C(99);
    uint64_t state_b = UINT64_C(99);
    uint32_t index;

    for (index = 0U; index < 4U; ++index) {
        uint32_t which;

        for (which = 0U; which < 2U; ++which) {
            s_scene_t *scene = which == 0U ? &first : &second;

            s_make_part(scene, index, 36U, 2U);
            scene->placements[index].part_index = index;
            scene->placements[index].part_id = index + 1U;
            scene->placements[index].orientation = 0U;
            scene->placements[index].x_cells = 8U * index;
            scene->placements[index].y_cells = 4U * index;
            scene->placed[index] = 1U;
        }
    }
    s_load_field(&first, 4U);
    s_load_field(&second, 4U);

    (void) s_resolve(&first, 4U, S_SIDE, &state_a);
    (void) s_resolve(&second, 4U, S_SIDE, &state_b);

    for (index = 0U; index < 4U; ++index) {
        CHECK(first.placements[index].x_cells
            == second.placements[index].x_cells);
        CHECK(first.placements[index].y_cells
            == second.placements[index].y_cells);
    }
}

/*
 * Six squares that cannot possibly fit must be reported as unresolved rather
 * than aborting. The caller squeezed too hard; that is an outcome, not a bug.
 */
static void
s_test_impossible_is_reported(void)
{
    static s_scene_t scene;
    uint64_t state = UINT64_C(4242);
    bp_separate_result_t result;
    uint32_t index;

    for (index = 0U; index < S_PARTS; ++index) {
        s_make_part(&scene, index, 44U, 1U);
        scene.placements[index].part_index = index;
        scene.placements[index].part_id = index + 1U;
        scene.placements[index].orientation = 0U;
        scene.placements[index].x_cells = 0U;
        scene.placements[index].y_cells = 0U;
        scene.placed[index] = 1U;
    }
    s_load_field(&scene, S_PARTS);

    /* Six 44-cell squares need far more than a 100 by 100 region. */
    result = s_resolve(&scene, S_PARTS, S_SIDE, &state);
    CHECK(!result.resolved);
    CHECK(result.overlap_cells > 0U);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_part_outside_region(void)
{
    static s_scene_t scene;
    uint64_t state = 1U;

    s_make_part(&scene, 0U, 40U, 1U);
    scene.placements[0].part_index = 0U;
    scene.placements[0].part_id = 1U;
    scene.placements[0].orientation = 0U;
    scene.placements[0].x_cells = 50U;
    scene.placements[0].y_cells = 0U;
    scene.placed[0] = 1U;
    s_load_field(&scene, 1U);

    /* The part starts beyond the region it is told to stay inside. */
    (void) s_resolve(&scene, 1U, 60U, &state);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_resolves_a_pile();
    s_test_deterministic();
    s_test_impossible_is_reported();

    t_expect_abort(s_body_part_outside_region, "part outside region");

    (void) printf("test_bp_separate: all checks passed\n");
    return 0;
}
