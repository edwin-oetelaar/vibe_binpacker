#include "test_support.h"

#include "bp_bitmap.h"
#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_search.h"

#include <string.h>

#define S_MAX_ROWS 64U
#define S_MAX_SPANS 64U
#define S_PARTS 18U
#define S_SHEET 120U
#define S_SHEET_WORDS (2U * S_SHEET)
#define S_MAX_SCRATCH 64U

/* Caller-owned backing store for one rectangular part and its orientations. */
typedef struct {
    uint32_t offsets[2][S_MAX_ROWS + 1U];
    bp_span_t spans[2][S_MAX_SPANS];
    bp_mask_t masks[2];
    bp_orientation_t orientations[2];
} s_storage_t;

/* Builds a rectangular part that may take a quarter turn. */
static bp_part_t
s_rect_part(uint32_t id, uint32_t width_cells, uint32_t height_cells,
    s_storage_t *storage)
{
    bp_part_t part;
    uint32_t widths[2] = { width_cells, height_cells };
    uint32_t heights[2] = { height_cells, width_cells };
    uint32_t slot;
    uint32_t count = width_cells == height_cells ? 1U : 2U;

    for (slot = 0U; slot < count; ++slot) {
        storage->masks[slot] = bp_mask_rectangle((bp_mask_rectangle_args_t) {
            .width_cells = widths[slot],
            .height_cells = heights[slot],
            .row_offsets = storage->offsets[slot],
            .row_offset_capacity = S_MAX_ROWS + 1U,
            .spans = storage->spans[slot],
            .span_capacity = S_MAX_SPANS
        });
        storage->orientations[slot].collision = &storage->masks[slot];
        storage->orientations[slot].solid = &storage->masks[slot];
        storage->orientations[slot].solid_offset_x_cells = 0U;
        storage->orientations[slot].solid_offset_y_cells = 0U;
    }
    part.id = id;
    part.orientations = storage->orientations;
    part.orientation_count = count;
    return part;
}

/* Dimensions chosen to leave gaps a single pass cannot tidy up. */
static const uint32_t s_sizes[S_PARTS][2] = {
    { 40U, 25U }, { 33U, 33U }, { 18U, 47U }, { 55U, 12U }, { 27U, 21U },
    { 14U, 14U }, { 44U, 30U }, { 22U, 38U }, { 31U, 16U }, { 11U, 52U },
    { 26U, 26U }, { 48U, 19U }, { 17U, 29U }, { 36U, 23U }, { 13U, 41U },
    { 29U, 34U }, { 20U, 20U }, { 45U, 15U }
};

/* Fills in the fixed workload used by every test in this file. */
static void
s_build(s_storage_t *storage, bp_part_t *parts)
{
    uint32_t index;

    for (index = 0U; index < S_PARTS; ++index)
        parts[index] = s_rect_part(index + 1U, s_sizes[index][0],
            s_sizes[index][1], &storage[index]);
}

/* Everything bp_search_solve needs, so the tests stay readable. */
typedef struct {
    uint32_t order[S_PARTS];
    uint8_t placed[S_PARTS];
    uint8_t scratch_placed[S_PARTS];
    uint8_t best_placed[S_PARTS];
    bp_placement_t best[S_PARTS];
    bp_placement_t placements[S_PARTS];
    uint64_t rows[S_MAX_SCRATCH];
} s_scratch_t;

/* Runs the search with the given budget and seed. */
static bp_search_result_t
s_solve(bp_bitmap_t *sheet, const bp_part_t *parts, s_scratch_t *scratch,
    uint32_t iterations, uint64_t seed, double blink_rate)
{
    CHECK(bp_place_scratch_words(sheet) <= S_MAX_SCRATCH);
    return bp_search_solve((bp_search_args_t) {
        .sheet = sheet,
        .parts = parts,
        .part_count = S_PARTS,
        .weights = bp_score_default_weights(),
        .height_window = 1U,
        .max_positions = 32U,
        .max_ruin = 6U,
        .blink_rate = blink_rate,
        .iterations = iterations,
        .time_budget_seconds = 0.0,
        .seed = seed,
        .scratch_order = scratch->order,
        .scratch_placed = scratch->scratch_placed,
        .scratch_best_placed = scratch->best_placed,
        .scratch_best = scratch->best,
        .scratch_capacity = S_PARTS,
        .scratch_rows = scratch->rows,
        .scratch_row_capacity = S_MAX_SCRATCH,
        .placements = scratch->placements,
        .placed = scratch->placed,
        .placement_capacity = S_PARTS
    });
}

/*
 * Replays the reported layout onto a fresh sheet. Every placement must be in
 * bounds and land on free cells, which proves the answer independently of
 * whatever the search believed while it was running.
 */
static void
s_check_layout(const bp_part_t *parts, const s_scratch_t *scratch,
    const bp_search_result_t *result)
{
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t replay = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    uint32_t index;
    uint32_t placed = 0U;
    uint64_t area = 0U;

    for (index = 0U; index < S_PARTS; ++index) {
        const bp_orientation_t *option;
        const bp_placement_t *spot;

        if (!scratch->placed[index])
            continue;
        spot = &scratch->placements[index];
        CHECK(spot->part_index == index);
        CHECK(spot->part_id == parts[index].id);
        CHECK(spot->orientation < parts[index].orientation_count);
        option = &parts[index].orientations[spot->orientation];

        CHECK(bp_mask_in_bounds(&replay, option->solid, spot->x_cells,
            spot->y_cells));
        CHECK(!bp_mask_overlaps(&replay, option->solid, spot->x_cells,
            spot->y_cells));
        bp_mask_blit(&replay, option->solid, spot->x_cells, spot->y_cells);
        area += option->solid->area_cells;
        ++placed;
    }
    CHECK(placed == result->placed_count);
    CHECK(area == result->placed_area_cells);
    CHECK(placed + result->unplaced_count == S_PARTS);
}

/* The same seed must give the same layout, or nothing here is reproducible. */
static void
s_test_deterministic(void)
{
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    s_scratch_t first;
    s_scratch_t second;
    bp_search_result_t a;
    bp_search_result_t b;

    s_build(storage, parts);
    a = s_solve(&sheet, parts, &first, 120U, UINT64_C(12345), 0.08);
    b = s_solve(&sheet, parts, &second, 120U, UINT64_C(12345), 0.08);

    CHECK(a.placed_count == b.placed_count);
    CHECK(a.placed_area_cells == b.placed_area_cells);
    CHECK(a.used_height_cells == b.used_height_cells);
    CHECK(a.lift_cells == b.lift_cells);
    CHECK(memcmp(first.placed, second.placed, sizeof first.placed) == 0);
    CHECK(memcmp(first.placements, second.placements,
        sizeof first.placements) == 0);
}

/*
 * The search only ever keeps an improvement, so more rounds can never make
 * the answer worse. Checked on the objective the search actually uses, in the
 * order it uses it.
 */
static void
s_test_never_worse_than_construction(void)
{
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    s_scratch_t built;
    s_scratch_t searched;
    bp_search_result_t construction;
    bp_search_result_t improved;

    s_build(storage, parts);
    construction = s_solve(&sheet, parts, &built, 0U, UINT64_C(7), 0.08);
    improved = s_solve(&sheet, parts, &searched, 400U, UINT64_C(7), 0.08);

    s_check_layout(parts, &built, &construction);
    s_check_layout(parts, &searched, &improved);

    CHECK(improved.placed_area_cells >= construction.placed_area_cells);
    if (improved.placed_area_cells == construction.placed_area_cells) {
        CHECK(improved.used_height_cells <= construction.used_height_cells);
        if (improved.used_height_cells == construction.used_height_cells)
            CHECK(improved.lift_cells <= construction.lift_cells);
    }
    CHECK(improved.iterations_run == 400U);
}

/*
 * Different seeds must explore differently. Without this the randomness could
 * be inert and every test above would still pass.
 */
static void
s_test_seeds_differ(void)
{
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    s_scratch_t scratch;
    uint64_t lifts[4];
    uint32_t seed;
    int differ = 0;

    s_build(storage, parts);
    for (seed = 0U; seed < 4U; ++seed) {
        bp_search_result_t result = s_solve(&sheet, parts, &scratch, 300U,
            UINT64_C(1000) + seed, 0.08);

        s_check_layout(parts, &scratch, &result);
        lifts[seed] = result.lift_cells;
        if (seed > 0U && lifts[seed] != lifts[0])
            differ = 1;
    }
    CHECK(differ);
}

/* Contract bodies each build their own state so nothing is shared. */

static void
s_body_short_row_scratch(void)
{
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    s_scratch_t scratch;

    s_build(storage, parts);
    (void) bp_search_solve((bp_search_args_t) {
        .sheet = &sheet,
        .parts = parts,
        .part_count = S_PARTS,
        .weights = bp_score_default_weights(),
        .height_window = 1U,
        .max_positions = 32U,
        .max_ruin = 6U,
        .iterations = 1U,
        .seed = 1U,
        .scratch_order = scratch.order,
        .scratch_placed = scratch.scratch_placed,
        .scratch_best_placed = scratch.best_placed,
        .scratch_best = scratch.best,
        .scratch_capacity = S_PARTS,
        .scratch_rows = scratch.rows,
        .scratch_row_capacity = 1U,
        .placements = scratch.placements,
        .placed = scratch.placed,
        .placement_capacity = S_PARTS
    });
}

static void
s_body_blink_out_of_range(void)
{
    s_storage_t storage[S_PARTS];
    bp_part_t parts[S_PARTS];
    uint64_t words[S_SHEET_WORDS];
    bp_bitmap_t sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = S_SHEET,
        .height_cells = S_SHEET,
        .words = words,
        .word_capacity = S_SHEET_WORDS
    });
    s_scratch_t scratch;

    s_build(storage, parts);
    (void) s_solve(&sheet, parts, &scratch, 1U, 1U, 1.0);
}

/* Runs every test; any failure exits non-zero with the failing expectation. */
int
main(void)
{
    s_test_deterministic();
    s_test_never_worse_than_construction();
    s_test_seeds_differ();

    t_expect_abort(s_body_short_row_scratch, "short row scratch");
    t_expect_abort(s_body_blink_out_of_range, "blink out of range");

    (void) printf("test_bp_search: all checks passed\n");
    return 0;
}
