#include "test_support.h"

#include "bp_multisheet.h"

#include <string.h>

#define S_PARTS 12U
#define S_SIDE 128U

/* Fixed caller-owned masks and workspace keep each test independent. */
typedef struct {
    uint32_t offsets[2][2][S_SIDE + 1U];
    bp_span_t spans[2][2][S_SIDE];
    bp_mask_t masks[2][2];
    bp_orientation_t orientations[2];
} s_part_storage_t;

typedef struct {
    uint64_t words[2U * S_SIDE];
    bp_bitmap_t bitmap;
    s_part_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint32_t order[S_PARTS];
    uint32_t best_order[S_PARTS];
    uint8_t assigned[S_PARTS];
    bp_placement_t candidate[S_PARTS];
    bp_placement_t placements[S_PARTS];
    bp_sheet_layout_t candidate_sheets[S_PARTS];
    bp_sheet_layout_t sheets[S_PARTS];
    uint64_t rows[8];
    bp_multisheet_args_t args;
} s_fixture_t;

/* Initialises a reusable solve configuration with explicit buffer capacities. */
static void
s_init(s_fixture_t *fixture, uint32_t width, uint32_t height, uint32_t count)
{
    CHECK(fixture != NULL);
    CHECK(width <= S_SIDE && height <= S_SIDE);
    CHECK(count > 0U && count <= S_PARTS);
    memset(fixture, 0, sizeof *fixture);
    fixture->bitmap = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = width, .height_cells = height,
        .words = fixture->words, .word_capacity = 2U * S_SIDE
    });
    fixture->args = (bp_multisheet_args_t) {
        .sheet = &fixture->bitmap, .parts = fixture->parts, .part_count = count,
        .max_sheets = count, .sort = BP_NEST_SORT_INPUT_ORDER,
        .place_mode = BP_PLACE_JUMP, .seed = 12345U,
        .scratch_order = fixture->order, .scratch_best_order = fixture->best_order,
        .scratch_assigned = fixture->assigned, .scratch_placements = fixture->candidate,
        .scratch_sheets = fixture->candidate_sheets,
        .scratch_rows = fixture->rows, .scratch_row_capacity = 8U,
        .placements = fixture->placements, .sheets = fixture->sheets,
        .part_capacity = S_PARTS, .sheet_capacity = S_PARTS
    };
}

/* Adds rectangle orientations with the same asymmetric clearance as real jobs. */
static void
s_part(s_fixture_t *fixture, uint32_t index, uint32_t width, uint32_t height,
    uint32_t clearance, int turn)
{
    s_part_storage_t *storage;
    uint32_t slot;
    uint32_t count = turn ? 2U : 1U;

    CHECK(fixture != NULL);
    CHECK(index < fixture->args.part_count);
    CHECK(width + 2U * clearance <= S_SIDE);
    CHECK(height + 2U * clearance <= S_SIDE);
    storage = &fixture->storage[index];
    for (slot = 0U; slot < count; ++slot) {
        uint32_t w = slot == 0U ? width : height;
        uint32_t h = slot == 0U ? height : width;
        uint32_t kind;

        for (kind = 0U; kind < 2U; ++kind) {
            storage->masks[slot][kind] = bp_mask_rectangle((bp_mask_rectangle_args_t) {
                .width_cells = w + (kind == 0U ? 0U : 2U * clearance),
                .height_cells = h + (kind == 0U ? 0U : 2U * clearance),
                .row_offsets = storage->offsets[slot][kind],
                .row_offset_capacity = S_SIDE + 1U,
                .spans = storage->spans[slot][kind], .span_capacity = S_SIDE
            });
        }
        storage->orientations[slot] = (bp_orientation_t) {
            .solid = &storage->masks[slot][0], .collision = &storage->masks[slot][1],
            .solid_offset_x_cells = clearance, .solid_offset_y_cells = clearance
        };
    }
    fixture->parts[index] = (bp_part_t) {
        .id = 100U + index, .orientations = storage->orientations, .orientation_count = count
    };
}

/* Replays each sheet, checking identity, clearance, bounds and exact accounting. */
static void
s_check(const s_fixture_t *fixture, bp_multisheet_result_t result)
{
    uint8_t seen[S_PARTS] = { 0 };
    uint64_t words[2U * S_SIDE];
    uint32_t sheet_index;
    uint32_t total = 0U;

    CHECK(fixture != NULL);
    CHECK(result.sheet_count <= fixture->args.max_sheets);
    CHECK(result.placed_count + result.unplaced_count == fixture->args.part_count);
    CHECK((result.status == BP_MULTISHEET_COMPLETE) == (result.unplaced_count == 0U));
    CHECK(bp_bitmap_occupied_count(&fixture->bitmap) == 0U);
    for (sheet_index = 0U; sheet_index < result.sheet_count; ++sheet_index) {
        const bp_sheet_layout_t *layout = &fixture->sheets[sheet_index];
        bp_bitmap_t replay = bp_bitmap_init((bp_bitmap_init_args_t) {
            .width_cells = fixture->bitmap.width_cells,
            .height_cells = fixture->bitmap.height_cells,
            .words = words, .word_capacity = 2U * S_SIDE
        });
        uint32_t min_x = UINT32_MAX;
        uint32_t min_y = UINT32_MAX;
        uint32_t max_x = 0U;
        uint32_t max_y = 0U;
        uint32_t index;
        uint64_t area = 0U;

        CHECK(layout->first_placement == total);
        CHECK(layout->placement_count > 0U);
        for (index = 0U; index < layout->placement_count; ++index) {
            const bp_placement_t *spot = &fixture->placements[total++];
            const bp_orientation_t *option;
            uint32_t x;
            uint32_t y;

            CHECK(spot->part_index < fixture->args.part_count);
            CHECK(!seen[spot->part_index]);
            seen[spot->part_index] = 1U;
            CHECK(spot->part_id == fixture->parts[spot->part_index].id);
            CHECK(spot->orientation < fixture->parts[spot->part_index].orientation_count);
            option = &fixture->parts[spot->part_index].orientations[spot->orientation];
            CHECK(bp_mask_in_bounds(&replay, option->collision, spot->x_cells, spot->y_cells));
            CHECK(!bp_mask_overlaps(&replay, option->collision, spot->x_cells, spot->y_cells));
            x = spot->x_cells + option->solid_offset_x_cells;
            y = spot->y_cells + option->solid_offset_y_cells;
            bp_mask_blit(&replay, option->solid, x, y);
            area += option->solid->area_cells;
            if (x < min_x)
                min_x = x;
            if (y < min_y)
                min_y = y;
            if (x + option->solid->width_cells > max_x)
                max_x = x + option->solid->width_cells;
            if (y + option->solid->height_cells > max_y)
                max_y = y + option->solid->height_cells;
        }
        CHECK(layout->min_x_cells == min_x && layout->min_y_cells == min_y);
        CHECK(layout->width_cells == max_x - min_x);
        CHECK(layout->height_cells == max_y - min_y);
        CHECK(layout->placed_area_cells == area);
        CHECK(bp_bitmap_occupied_count(&replay) == area);
        if (sheet_index + 1U == result.sheet_count)
            CHECK(result.last_box_area_cells
                == (uint64_t) layout->width_cells * layout->height_cells);
    }
    CHECK(total == result.placed_count);
    if (total == 0U)
        CHECK(result.sheet_count == 0U && result.last_box_area_cells == 0U);
}

/* Overflow must be placed exactly once, and a plate limit yields a valid prefix. */
static void
s_test_overflow_and_limit(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t result;
    uint32_t index;

    s_init(&fixture, 6U, 4U, 3U);
    for (index = 0U; index < 3U; ++index)
        s_part(&fixture, index, 6U, 4U, 0U, 0);
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.status == BP_MULTISHEET_COMPLETE && result.sheet_count == 3U);
    fixture.args.max_sheets = 2U;
    fixture.args.iterations = 40U;
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.sheet_count == 2U && result.unplaced_count == 1U);
    CHECK(result.oversized_count == 0U);
}

/* An impossible part must not emit empty sheets or prevent other parts fitting. */
static void
s_test_oversized(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t result;

    s_init(&fixture, 10U, 10U, 2U);
    s_part(&fixture, 0U, 11U, 11U, 0U, 1);
    s_part(&fixture, 1U, 4U, 4U, 0U, 0);
    fixture.args.iterations = 32U;
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.oversized_count == 1U && result.placed_count == 1U);
    CHECK(result.sheet_count == 1U);
    s_part(&fixture, 1U, 12U, 12U, 0U, 0);
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.oversized_count == 2U && result.sheet_count == 0U);
    CHECK(result.iterations_run == 0U);
}

/* Width trials improve true W*H, even when that requires a taller final layout. */
static void
s_test_last_box_and_determinism(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t baseline;
    bp_multisheet_result_t improved;
    bp_multisheet_result_t repeat;
    bp_placement_t saved[S_PARTS];
    bp_sheet_layout_t saved_sheets[S_PARTS];

    s_init(&fixture, 10U, 10U, 3U);
    s_part(&fixture, 0U, 10U, 10U, 0U, 0);
    s_part(&fixture, 1U, 4U, 6U, 0U, 0);
    s_part(&fixture, 2U, 6U, 3U, 0U, 0);
    baseline = bp_multisheet_solve(fixture.args);
    s_check(&fixture, baseline);
    CHECK(baseline.sheet_count == 2U && baseline.last_box_area_cells == 60U);
    fixture.args.iterations = 64U;
    improved = bp_multisheet_solve(fixture.args);
    s_check(&fixture, improved);
    CHECK(improved.sheet_count == 2U && improved.last_box_area_cells == 54U);
    CHECK(fixture.sheets[1].width_cells == 6U && fixture.sheets[1].height_cells == 9U);
    CHECK(improved.improvements > 0U && improved.iterations_run == 64U);
    memcpy(saved, fixture.placements, improved.placed_count * sizeof saved[0]);
    memcpy(saved_sheets, fixture.sheets, improved.sheet_count * sizeof saved_sheets[0]);
    repeat = bp_multisheet_solve(fixture.args);
    CHECK(repeat.last_box_area_cells == improved.last_box_area_cells);
    CHECK(repeat.improvements == improved.improvements);
    CHECK(memcmp(saved, fixture.placements, repeat.placed_count * sizeof saved[0]) == 0);
    CHECK(memcmp(saved_sheets, fixture.sheets, repeat.sheet_count * sizeof saved_sheets[0]) == 0);
    fixture.args.iterations = 128U;
    repeat = bp_multisheet_solve(fixture.args);
    s_check(&fixture, repeat);
    CHECK(repeat.sheet_count <= improved.sheet_count);
    if (repeat.sheet_count == improved.sheet_count)
        CHECK(repeat.last_box_area_cells <= improved.last_box_area_cells);
}

/* Reassignment must beat a smaller last box when it eliminates a whole plate. */
static void
s_test_sheet_count_first(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t baseline;
    bp_multisheet_result_t improved;

    s_init(&fixture, 10U, 10U, 4U);
    s_part(&fixture, 0U, 4U, 4U, 0U, 0);
    s_part(&fixture, 1U, 6U, 6U, 0U, 0);
    s_part(&fixture, 2U, 6U, 4U, 0U, 0);
    s_part(&fixture, 3U, 4U, 6U, 0U, 0);
    baseline = bp_multisheet_solve(fixture.args);
    CHECK(baseline.sheet_count == 2U && baseline.last_box_area_cells == 24U);
    fixture.args.iterations = 128U;
    improved = bp_multisheet_solve(fixture.args);
    s_check(&fixture, improved);
    CHECK(improved.sheet_count == 1U && improved.unplaced_count == 0U);
    CHECK(improved.last_box_area_cells == 100U);
    fixture.args.max_sheets = 1U;
    improved = bp_multisheet_solve(fixture.args);
    s_check(&fixture, improved);
    CHECK(improved.status == BP_MULTISHEET_COMPLETE);
    fixture.args.iterations = 0U;
    fixture.args.sort = BP_NEST_SORT_AREA_DESCENDING;
    improved = bp_multisheet_solve(fixture.args);
    s_check(&fixture, improved);
    CHECK(improved.status == BP_MULTISHEET_COMPLETE);
    fixture.args.sort = BP_NEST_SORT_LONGEST_SIDE_DESCENDING;
    improved = bp_multisheet_solve(fixture.args);
    s_check(&fixture, improved);
    CHECK(improved.status == BP_MULTISHEET_COMPLETE);
}

/* Rotation, margin-free bounds and shortened word strides must agree in all modes. */
static void
s_test_clearance_and_word_boundary(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t result;
    uint32_t mode;

    s_init(&fixture, 65U, 12U, 2U);
    s_part(&fixture, 0U, 10U, 63U, 1U, 1);
    s_part(&fixture, 1U, 10U, 63U, 1U, 1);
    fixture.args.iterations = 40U;
    for (mode = 0U; mode < 3U; ++mode) {
        fixture.args.place_mode = (bp_place_mode_t) mode;
        result = bp_multisheet_solve(fixture.args);
        s_check(&fixture, result);
        CHECK(result.sheet_count == 2U && result.placed_count == 2U);
        CHECK(result.last_box_area_cells == 630U);
        CHECK(fixture.placements[0].orientation == 1U);
        CHECK(fixture.sheets[1].min_x_cells == 1U && fixture.sheets[1].min_y_cells == 1U);
    }
    s_init(&fixture, 65U, 24U, 2U);
    s_part(&fixture, 0U, 30U, 10U, 1U, 0);
    s_part(&fixture, 1U, 30U, 10U, 1U, 0);
    fixture.args.iterations = 40U;
    fixture.args.place_mode = BP_PLACE_EROSION;
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.sheet_count == 1U);
    CHECK(result.last_box_area_cells <= 610U);
}

/* A tiny budget still returns a complete baseline rather than partial work. */
static void
s_test_budget(void)
{
    s_fixture_t fixture;
    bp_multisheet_result_t result;

    s_init(&fixture, 10U, 10U, 1U);
    s_part(&fixture, 0U, 4U, 4U, 0U, 0);
    fixture.args.iterations = 100U;
    fixture.args.time_budget_seconds = 1e-9;
    result = bp_multisheet_solve(fixture.args);
    s_check(&fixture, result);
    CHECK(result.status == BP_MULTISHEET_COMPLETE);
    CHECK(result.iterations_run <= 100U);
}

/* Undersized buffers are programmer defects, not an overflow result. */
static void
s_body_short_capacity(void)
{
    s_fixture_t fixture;

    s_init(&fixture, 10U, 10U, 2U);
    s_part(&fixture, 0U, 4U, 4U, 0U, 0);
    s_part(&fixture, 1U, 4U, 4U, 0U, 0);
    fixture.args.sheet_capacity = 1U;
    (void) bp_multisheet_solve(fixture.args);
}

/* A zero-sheet request is an invalid configuration. */
static void
s_body_zero_sheets(void)
{
    s_fixture_t fixture;

    s_init(&fixture, 10U, 10U, 1U);
    s_part(&fixture, 0U, 4U, 4U, 0U, 0);
    fixture.args.max_sheets = 0U;
    (void) bp_multisheet_solve(fixture.args);
}

/* An offset outside the collision box must not pass via unsigned wraparound. */
static void
s_body_invalid_offset(void)
{
    s_fixture_t fixture;

    s_init(&fixture, 10U, 10U, 1U);
    s_part(&fixture, 0U, 4U, 4U, 0U, 0);
    fixture.storage[0].orientations[0].solid_offset_x_cells = 5U;
    (void) bp_multisheet_solve(fixture.args);
}

/* Runs focused multi-sheet behavior and fail-fast contract checks. */
int
main(void)
{
    s_test_overflow_and_limit();
    s_test_oversized();
    s_test_last_box_and_determinism();
    s_test_sheet_count_first();
    s_test_clearance_and_word_boundary();
    s_test_budget();
    t_expect_abort(s_body_short_capacity, "short sheet capacity");
    t_expect_abort(s_body_zero_sheets, "zero sheets");
    t_expect_abort(s_body_invalid_offset, "invalid solid offset");
    (void) printf("test_bp_multisheet: all checks passed\n");
    return 0;
}
