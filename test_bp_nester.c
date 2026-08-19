#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_place.h"

#define S_MAX_ROWS 160U
#define S_MAX_SPANS 160U
#define S_MAX_PARTS 24U

/* Caller-owned backing store for one rectangular part and its orientations. */
typedef struct {
    uint32_t collision_offsets[2][S_MAX_ROWS + 1U];
    bp_span_t collision_spans[2][S_MAX_SPANS];
    uint32_t solid_offsets[2][S_MAX_ROWS + 1U];
    bp_span_t solid_spans[2][S_MAX_SPANS];
    bp_mask_t collision_masks[2];
    bp_mask_t solid_masks[2];
    bp_orientation_t orientations[2];
} s_rect_storage_t;

/* Fills one orientation slot with a rectangle grown by the clearance. */
static void
s_fill_orientation(s_rect_storage_t *storage, uint32_t slot,
    uint32_t width_cells, uint32_t height_cells, uint32_t clearance_cells)
{
    CHECK(width_cells + 2U * clearance_cells <= S_MAX_ROWS);
    CHECK(height_cells + 2U * clearance_cells <= S_MAX_ROWS);

    storage->solid_masks[slot] = bp_mask_rectangle(
        (bp_mask_rectangle_args_t) {
            .width_cells = width_cells,
            .height_cells = height_cells,
            .row_offsets = storage->solid_offsets[slot],
            .row_offset_capacity = S_MAX_ROWS + 1U,
            .spans = storage->solid_spans[slot],
            .span_capacity = S_MAX_SPANS
        });
    storage->collision_masks[slot] = bp_mask_rectangle(
        (bp_mask_rectangle_args_t) {
            .width_cells = width_cells + 2U * clearance_cells,
            .height_cells = height_cells + 2U * clearance_cells,
            .row_offsets = storage->collision_offsets[slot],
            .row_offset_capacity = S_MAX_ROWS + 1U,
            .spans = storage->collision_spans[slot],
            .span_capacity = S_MAX_SPANS
        });
    storage->orientations[slot].collision = &storage->collision_masks[slot];
    storage->orientations[slot].solid = &storage->solid_masks[slot];
    storage->orientations[slot].solid_offset_x_cells = clearance_cells;
    storage->orientations[slot].solid_offset_y_cells = clearance_cells;
}

/*
 * Builds a rectangular part. A quarter turn is just a second orientation,
 * which is why the nester needs no rotation logic of its own.
 */
static bp_part_t
s_rect_part_clearance(uint32_t id, uint32_t width_cells, uint32_t height_cells,
    int allow_quarter_turn, uint32_t clearance_cells,
    s_rect_storage_t *storage)
{
    bp_part_t part;

    s_fill_orientation(storage, 0U, width_cells, height_cells,
        clearance_cells);
    part.id = id;
    part.orientations = storage->orientations;
    part.orientation_count = 1U;

    if (allow_quarter_turn && width_cells != height_cells) {
        s_fill_orientation(storage, 1U, height_cells, width_cells,
            clearance_cells);
        part.orientation_count = 2U;
    }
    return part;
}

/* The common case: no clearance, so collision and solid masks coincide. */
static bp_part_t
s_rect_part(uint32_t id, uint32_t width_cells, uint32_t height_cells,
    int allow_quarter_turn, s_rect_storage_t *storage)
{
    return s_rect_part_clearance(id, width_cells, height_cells,
        allow_quarter_turn, 0U, storage);
}

/* Two halves that exactly tile the sheet must leave no waste at all. */
static void
s_test_exact_fit(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part(1U, 5U, 4U, 0, &storage[0]),
        s_rect_part(2U, 5U, 4U, 0, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[2];
    bp_nester_solve_result_t result = bp_nester_solve(
        (bp_nester_solve_args_t) {
            .sheet = &sheet,
            .parts = parts,
            .part_count = 2U,
            .sort = BP_NEST_SORT_INPUT_ORDER,
            .place_mode = BP_PLACE_JUMP,
            .scratch_order = order,
            .scratch_capacity = 2U,
            .placements = placements,
            .placement_capacity = 2U
        });

    CHECK(result.placed_count == 2U);
    CHECK(result.unplaced_count == 0U);
    CHECK(result.placed_area_cells == 40U);
    CHECK(result.used_width_cells == 10U);
    CHECK(result.used_height_cells == 4U);
    CHECK(placements[0].x_cells == 0U && placements[0].y_cells == 0U);
    CHECK(placements[1].x_cells == 5U && placements[1].y_cells == 0U);
}

/* A part that only fits turned must be turned rather than reported unplaced. */
static void
s_test_rotation_is_required(void)
{
    uint64_t words[10];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 4U,
        .height_cells = 10U,
        .words = words,
        .word_capacity = 10U
    });
    s_rect_storage_t storage;
    bp_part_t part = s_rect_part(1U, 10U, 4U, 1, &storage);
    uint32_t order[1];
    bp_placement_t placements[1];
    bp_nester_solve_result_t result = bp_nester_solve(
        (bp_nester_solve_args_t) {
            .sheet = &sheet,
            .parts = &part,
            .part_count = 1U,
            .sort = BP_NEST_SORT_INPUT_ORDER,
            .place_mode = BP_PLACE_JUMP,
            .scratch_order = order,
            .scratch_capacity = 1U,
            .placements = placements,
            .placement_capacity = 1U
        });

    CHECK(result.placed_count == 1U);
    CHECK(placements[0].orientation == 1U);
    CHECK(result.used_width_cells == 4U);
    CHECK(result.used_height_cells == 10U);
}

/* A part larger than the sheet is reported unplaced, not aborted. */
static void
s_test_oversized_part(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part(1U, 11U, 5U, 1, &storage[0]),
        s_rect_part(2U, 4U, 4U, 0, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[2];
    bp_nester_solve_result_t result = bp_nester_solve(
        (bp_nester_solve_args_t) {
            .sheet = &sheet,
            .parts = parts,
            .part_count = 2U,
            .sort = BP_NEST_SORT_INPUT_ORDER,
            .place_mode = BP_PLACE_JUMP,
            .scratch_order = order,
            .scratch_capacity = 2U,
            .placements = placements,
            .placement_capacity = 2U
        });

    CHECK(result.placed_count == 1U);
    CHECK(result.unplaced_count == 1U);
    CHECK(placements[0].part_id == 2U);
}

/*
 * The sorting trap: placing a tiny part first ruins the sheet for the big one.
 * This is exactly the weakness that makes BLF an initialiser rather than an
 * answer, and the reason the order is a decision variable at all.
 */
static void
s_test_sorting_trap(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet;
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part(1U, 1U, 1U, 0, &storage[0]),
        s_rect_part(2U, 4U, 4U, 0, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[2];
    bp_nester_solve_result_t input_order;
    bp_nester_solve_result_t area_order;

    sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 4U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    input_order = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = 2U,
        .sort = BP_NEST_SORT_INPUT_ORDER,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = order,
        .scratch_capacity = 2U,
        .placements = placements,
        .placement_capacity = 2U
    });
    area_order = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = 2U,
        .sort = BP_NEST_SORT_AREA_DESCENDING,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = order,
        .scratch_capacity = 2U,
        .placements = placements,
        .placement_capacity = 2U
    });

    CHECK(input_order.placed_area_cells == 1U);
    CHECK(area_order.placed_area_cells == 16U);
}

/* Sorting must be stable, so equal keys keep their input order. */
static void
s_test_sort_is_stable(void)
{
    uint64_t words[8];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 12U,
        .height_cells = 8U,
        .words = words,
        .word_capacity = 8U
    });
    s_rect_storage_t storage[3];
    bp_part_t parts[3] = {
        s_rect_part(7U, 4U, 4U, 0, &storage[0]),
        s_rect_part(8U, 4U, 4U, 0, &storage[1]),
        s_rect_part(9U, 4U, 4U, 0, &storage[2])
    };
    uint32_t order[3];
    bp_placement_t placements[3];
    bp_nester_solve_result_t result = bp_nester_solve(
        (bp_nester_solve_args_t) {
            .sheet = &sheet,
            .parts = parts,
            .part_count = 3U,
            .sort = BP_NEST_SORT_AREA_DESCENDING,
            .place_mode = BP_PLACE_JUMP,
            .scratch_order = order,
            .scratch_capacity = 3U,
            .placements = placements,
            .placement_capacity = 3U
        });

    CHECK(result.placed_count == 3U);
    CHECK(placements[0].part_id == 7U);
    CHECK(placements[1].part_id == 8U);
    CHECK(placements[2].part_id == 9U);
}

/*
 * With a clearance the search fits the grown mask but writes the true one, so
 * neighbouring parts end up exactly the requested distance apart rather than
 * twice it.
 */
static void
s_test_clearance_separates_parts(void)
{
    uint64_t words[6];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 20U,
        .height_cells = 6U,
        .words = words,
        .word_capacity = 6U
    });
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part_clearance(1U, 5U, 3U, 0, 1U, &storage[0]),
        s_rect_part_clearance(2U, 5U, 3U, 0, 1U, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[2];
    bp_nester_solve_result_t result = bp_nester_solve(
        (bp_nester_solve_args_t) {
            .sheet = &sheet,
            .parts = parts,
            .part_count = 2U,
            .sort = BP_NEST_SORT_INPUT_ORDER,
            .place_mode = BP_PLACE_JUMP,
            .scratch_order = order,
            .scratch_capacity = 2U,
            .placements = placements,
            .placement_capacity = 2U
        });
    uint32_t y;

    CHECK(result.placed_count == 2U);
    /* Only the true geometry is on the sheet, so no clearance is consumed. */
    CHECK(result.placed_area_cells == 2U * 5U * 3U);
    CHECK(bp_bitmap_occupied_count(&sheet) == 2U * 5U * 3U);

    /* First solid at x 1..5, second at x 7..11, leaving column 6 empty. */
    CHECK(placements[0].x_cells == 0U && placements[0].y_cells == 0U);
    CHECK(placements[1].x_cells == 6U && placements[1].y_cells == 0U);
    for (y = 0U; y < 6U; ++y)
        CHECK(bp_bitmap_get(&sheet, 6U, y) == 0);
    CHECK(bp_bitmap_get(&sheet, 5U, 1U) == 1);
    CHECK(bp_bitmap_get(&sheet, 7U, 1U) == 1);

    /* The used box bounds the cut geometry, not the grown footprints. */
    CHECK(result.used_width_cells == 12U);
    CHECK(result.used_height_cells == 4U);
}

/* Dimensions of the fixed workload, chosen to leave awkward gaps. */
static const uint32_t s_workload[S_MAX_PARTS][2] = {
    { 120U, 60U }, { 90U, 90U }, { 45U, 110U }, { 30U, 30U },
    { 75U, 25U }, { 100U, 40U }, { 20U, 95U }, { 60U, 60U },
    { 55U, 35U }, { 15U, 15U }, { 80U, 20U }, { 25U, 70U },
    { 110U, 15U }, { 40U, 40U }, { 35U, 85U }, { 65U, 30U },
    { 50U, 50U }, { 10U, 120U }, { 95U, 45U }, { 70U, 10U },
    { 5U, 5U }, { 128U, 12U }, { 12U, 128U }, { 33U, 33U }
};

/*
 * Runs the fixed workload in both modes. The two must agree placement for
 * placement: the jump scan exists to be faster, not to be different.
 */
static void
s_test_workload_modes_agree(void)
{
    uint64_t words[5U * 300U];
    bp_bitmap_t sheet;
    s_rect_storage_t storage[S_MAX_PARTS];
    bp_part_t parts[S_MAX_PARTS];
    uint32_t order[S_MAX_PARTS];
    bp_placement_t reference_placements[S_MAX_PARTS];
    bp_placement_t jump_placements[S_MAX_PARTS];
    bp_nester_solve_result_t reference;
    bp_nester_solve_result_t jump;
    uint32_t index;

    sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 300U,
        .height_cells = 300U,
        .words = words,
        .word_capacity = sizeof words / sizeof words[0]
    });
    for (index = 0U; index < S_MAX_PARTS; ++index)
        parts[index] = s_rect_part(index + 1U, s_workload[index][0],
            s_workload[index][1], 1, &storage[index]);

    reference = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = S_MAX_PARTS,
        .sort = BP_NEST_SORT_AREA_DESCENDING,
        .place_mode = BP_PLACE_REFERENCE,
        .scratch_order = order,
        .scratch_capacity = S_MAX_PARTS,
        .placements = reference_placements,
        .placement_capacity = S_MAX_PARTS
    });
    jump = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = S_MAX_PARTS,
        .sort = BP_NEST_SORT_AREA_DESCENDING,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = order,
        .scratch_capacity = S_MAX_PARTS,
        .placements = jump_placements,
        .placement_capacity = S_MAX_PARTS
    });

    CHECK(jump.placed_count == reference.placed_count);
    CHECK(jump.unplaced_count == reference.unplaced_count);
    CHECK(jump.placed_area_cells == reference.placed_area_cells);
    CHECK(jump.used_width_cells == reference.used_width_cells);
    CHECK(jump.used_height_cells == reference.used_height_cells);
    for (index = 0U; index < reference.placed_count; ++index) {
        CHECK(jump_placements[index].part_id
            == reference_placements[index].part_id);
        CHECK(jump_placements[index].orientation
            == reference_placements[index].orientation);
        CHECK(jump_placements[index].x_cells
            == reference_placements[index].x_cells);
        CHECK(jump_placements[index].y_cells
            == reference_placements[index].y_cells);
    }
    CHECK(jump.candidate_checks < reference.candidate_checks);

    /* Every part of this workload fits on a 300 x 300 sheet. */
    CHECK(reference.placed_count == S_MAX_PARTS);
    CHECK(bp_bitmap_occupied_count(&sheet) == reference.placed_area_cells);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_duplicate_ids(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part(3U, 2U, 2U, 0, &storage[0]),
        s_rect_part(3U, 2U, 2U, 0, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[2];

    (void) bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = 2U,
        .sort = BP_NEST_SORT_INPUT_ORDER,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = order,
        .scratch_capacity = 2U,
        .placements = placements,
        .placement_capacity = 2U
    });
}

static void
s_body_short_placement_capacity(void)
{
    uint64_t words[4];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = 10U,
        .height_cells = 4U,
        .words = words,
        .word_capacity = 4U
    });
    s_rect_storage_t storage[2];
    bp_part_t parts[2] = {
        s_rect_part(1U, 2U, 2U, 0, &storage[0]),
        s_rect_part(2U, 2U, 2U, 0, &storage[1])
    };
    uint32_t order[2];
    bp_placement_t placements[1];

    (void) bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = 2U,
        .sort = BP_NEST_SORT_INPUT_ORDER,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = order,
        .scratch_capacity = 2U,
        .placements = placements,
        .placement_capacity = 1U
    });
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_exact_fit();
    s_test_rotation_is_required();
    s_test_oversized_part();
    s_test_sorting_trap();
    s_test_sort_is_stable();
    s_test_clearance_separates_parts();
    s_test_workload_modes_agree();

    t_expect_abort(s_body_duplicate_ids, "duplicate ids");
    t_expect_abort(s_body_short_placement_capacity, "short placements");

    (void) printf("test_bp_nester: all checks passed\n");
    return 0;
}
