#include "test_support.h"

#include "bp_view.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

/* Two used plates and one oversized part, with nontrivial original indices. */
typedef struct {
    uint32_t offsets[3][2][17];
    bp_span_t spans[3][2][16];
    bp_mask_t masks[3][2];
    bp_orientation_t orientations[3];
    bp_part_t parts[3];
    uint32_t labels[3];
    bp_placement_t placements[2];
    bp_sheet_layout_t sheets[2];
    uint8_t placed[3];
    bp_view_publish_args_t args;
} s_fixture_t;

/* Supplies caller-owned metadata whose bounds are deliberately not precomputed. */
static void
s_init(s_fixture_t *fixture)
{
    const uint32_t widths[3] = { 4U, 2U, 11U };
    const uint32_t heights[3] = { 5U, 3U, 2U };
    uint32_t index;

    CHECK(fixture != NULL);
    memset(fixture, 0, sizeof *fixture);
    for (index = 0U; index < 3U; ++index) {
        uint32_t kind;

        for (kind = 0U; kind < 2U; ++kind) {
            fixture->masks[index][kind] = bp_mask_rectangle((bp_mask_rectangle_args_t) {
                .width_cells = widths[index] + 2U * kind,
                .height_cells = heights[index] + 2U * kind,
                .row_offsets = fixture->offsets[index][kind], .row_offset_capacity = 17U,
                .spans = fixture->spans[index][kind], .span_capacity = 16U
            });
        }
        fixture->orientations[index] = (bp_orientation_t) {
            .solid = &fixture->masks[index][0], .collision = &fixture->masks[index][1],
            .solid_offset_x_cells = 1U, .solid_offset_y_cells = 1U
        };
        fixture->parts[index] = (bp_part_t) {
            .id = 10U + index, .orientations = &fixture->orientations[index],
            .orientation_count = 1U
        };
        fixture->labels[index] = 701U + index;
        fixture->placed[index] = 255U;
    }
    fixture->placements[0] = (bp_placement_t) { .part_index = 1U, .part_id = 11U };
    fixture->placements[1] = (bp_placement_t) { .part_index = 0U, .part_id = 10U };
    fixture->sheets[0].placement_count = 1U;
    fixture->sheets[1].first_placement = 1U;
    fixture->sheets[1].placement_count = 1U;
    fixture->args = (bp_view_publish_args_t) {
        .parts = fixture->parts, .labels = fixture->labels, .part_count = 3U,
        .placements = fixture->placements, .placement_count = 2U,
        .sheets = fixture->sheets, .sheet_count = 2U, .sheet_capacity = 2U,
        .placed = fixture->placed, .placed_capacity = 3U,
        .sheet_width_cells = 10U, .sheet_height_cells = 10U, .cell_size_mm = 0.5,
        .multiple_sheets = 1
    };
}

/* Plate-local replay must never alias another plate or lose original identity. */
static void
s_test_selection_and_bounds(void)
{
    s_fixture_t fixture;
    bp_view_t view;

    s_init(&fixture);
    view = bp_view_publish(fixture.args);
    CHECK(view.sheet_count == 2U && view.visible_count == 1U);
    CHECK(view.placed_area_cells == 26U && view.oversized_count == 1U);
    CHECK(fixture.placed[0] == 1U && fixture.placed[1] == 1U && fixture.placed[2] == 0U);
    CHECK(fabs(bp_view_utilisation(&view) - 0.13) < 1e-12);
    CHECK(bp_view_active(&view)->min_x_cells == 1U);
    CHECK(bp_view_active(&view)->min_y_cells == 1U);
    CHECK(bp_view_active(&view)->width_cells == 2U);
    CHECK(bp_view_active(&view)->height_cells == 3U);
    CHECK(bp_view_placement(&view, 0U)->part_index == 1U);
    bp_view_set_visible(&view, 0U);
    CHECK(view.visible_count == 0U && view.saved == 0);
    bp_view_select(&view, 1U);
    CHECK(view.visible_count == 1U && view.active_sheet == 1U);
    CHECK(bp_view_active(&view)->width_cells == 4U);
    CHECK(bp_view_active(&view)->height_cells == 5U);
    CHECK(bp_view_placement(&view, 0U)->part_index == 0U);
    view.saved = 1;
    bp_view_select(&view, 0U);
    bp_view_set_visible(&view, 0U);
    CHECK(view.saved == 1);
    bp_view_select(&view, 1U);
    fixture.args.sheet_count = 1U;
    fixture.args.placement_count = 1U;
    fixture.args.multiple_sheets = 0;
    view = bp_view_publish(fixture.args);
    CHECK(view.active_sheet == 0U && view.visible_count == 1U && !view.saved);
    CHECK(view.placed_area_cells == 6U && fixture.placed[0] == 0U);
    fixture.args.sheet_count = 0U;
    fixture.args.placement_count = 0U;
    view = bp_view_publish(fixture.args);
    CHECK(view.sheet_count == 0U && view.visible_count == 0U);
    CHECK(view.placed_area_cells == 0U && bp_view_utilisation(&view) == 0.0);
    CHECK(view.oversized_count == 1U);
}

/* Counts parts in an exported SVG and verifies the original job label. */
static void
s_check_svg(const char *path, const char *label)
{
    FILE *input;
    char line[512];
    unsigned groups = 0U;
    unsigned labels = 0U;

    CHECK(path != NULL && label != NULL);
    input = fopen(path, "r");
    CHECK(input != NULL);
    while (fgets(line, sizeof line, input)) {
        if (strstr(line, "<g "))
            ++groups;
        if (strstr(line, label))
            ++labels;
    }
    CHECK(fclose(input) == 0);
    CHECK(groups == 1U && labels == 1U);
}

/* All plates export even with a hidden active plate; partial I/O is not saved. */
static void
s_test_export(void)
{
    s_fixture_t fixture;
    bp_view_t view;
    bp_view_export_result_t result;
    char directory[] = "build/view-test-XXXXXX";
    char base[256];
    char first[256];
    char second[256];
    char missing[256];
    char scratch[256];
    bp_view_export_args_t args;

    s_init(&fixture);
    view = bp_view_publish(fixture.args);
    CHECK(mkdtemp(directory) != NULL);
    (void) snprintf(base, sizeof base, "%s/result.svg", directory);
    (void) snprintf(first, sizeof first, "%s/result-001.svg", directory);
    (void) snprintf(second, sizeof second, "%s/result-002.svg", directory);
    (void) snprintf(missing, sizeof missing, "%s/missing/result.svg", directory);
    args = (bp_view_export_args_t) {
        .view = &view, .path = base, .scratch_path = scratch, .path_capacity = sizeof scratch
    };
    bp_view_select(&view, 1U);
    bp_view_set_visible(&view, 0U);
    CHECK(mkdir(second, 0700) == 0);
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_WRITE_FAILED && result.written_count == 1U);
    CHECK(result.system_error == EISDIR);
    CHECK(!view.saved && view.active_sheet == 1U && view.visible_count == 0U);
    CHECK(strcmp(scratch, second) == 0);
    s_check_svg(first, ">702</text>");
    CHECK(rmdir(second) == 0);
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_OK && result.written_count == 2U && view.saved);
    CHECK(result.system_error == 0);
    s_check_svg(second, ">701</text>");
    CHECK(unlink(first) == 0 && unlink(second) == 0);
    fixture.args.sheet_count = 1U;
    fixture.args.placement_count = 1U;
    fixture.args.multiple_sheets = 0;
    view = bp_view_publish(fixture.args);
    args.path_capacity = 2U;
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_PATH_TOO_LONG && !view.saved);
    args.path_capacity = sizeof scratch;
    args.path = missing;
    result = bp_view_export(args);
    errno = ERANGE;  /* Reporting and cleanup must not change the captured failure. */
    CHECK(result.status == BP_VIEW_EXPORT_WRITE_FAILED && result.system_error == ENOENT);
    CHECK(result.written_count == 0U && !view.saved);
    args.path = "/dev/full";
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_WRITE_FAILED && !view.saved);
    CHECK(result.system_error == ENOSPC);
    args.path = base;
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_OK && result.written_count == 1U && view.saved);
    s_check_svg(base, ">702</text>");
    CHECK(unlink(base) == 0);
    fixture.args.sheet_count = 0U;
    fixture.args.placement_count = 0U;
    view = bp_view_publish(fixture.args);
    result = bp_view_export(args);
    CHECK(result.status == BP_VIEW_EXPORT_EMPTY && !view.saved);
    CHECK(rmdir(directory) == 0);
}

/* Duplicate parts indicate a broken result, not a second piece with the same label. */
static void
s_body_duplicate(void)
{
    s_fixture_t fixture;

    s_init(&fixture);
    fixture.placements[1] = fixture.placements[0];
    (void) bp_view_publish(fixture.args);
}

/* Range validation must happen before indexing the placement array. */
static void
s_body_bad_range(void)
{
    s_fixture_t fixture;

    s_init(&fixture);
    fixture.sheets[1].placement_count = UINT32_MAX;
    (void) bp_view_publish(fixture.args);
}

/* Selection outside the published plate list is a caller defect. */
static void
s_body_bad_selection(void)
{
    s_fixture_t fixture;
    bp_view_t view;

    s_init(&fixture);
    view = bp_view_publish(fixture.args);
    bp_view_select(&view, 2U);
}

/* Local steps may not spill onto a following plate. */
static void
s_body_bad_step(void)
{
    s_fixture_t fixture;
    bp_view_t view;

    s_init(&fixture);
    view = bp_view_publish(fixture.args);
    bp_view_set_visible(&view, 2U);
}

/* Exercises the GUI-independent result and export contract. */
int
main(void)
{
    s_test_selection_and_bounds();
    s_test_export();
    t_expect_abort(s_body_duplicate, "duplicate view part");
    t_expect_abort(s_body_bad_range, "bad view range");
    t_expect_abort(s_body_bad_selection, "bad view selection");
    t_expect_abort(s_body_bad_step, "bad view step");
    (void) printf("test_bp_view: all checks passed\n");
    return 0;
}
