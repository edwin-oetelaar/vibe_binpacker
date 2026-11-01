#include "test_support.h"

#include "bp_cutplan.h"
#include "bp_cutview.h"
#include "bp_guillotine.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#define S_CASES_DIRECTORY "build/cutplan-cases"
#define S_ITEMS 12U
#define S_NODES (7U * S_ITEMS + 2U * S_ITEMS)
#define S_CUTS (2U * S_NODES + 4U * S_ITEMS)

/* A packed three-plate job and its published view. */
typedef struct {
    bp_cut_settings_t settings;
    bp_cut_item_t items[S_ITEMS];
    uint32_t labels[S_ITEMS];
    bp_cut_node_t nodes[S_NODES];
    bp_cut_plate_t plates[S_ITEMS];
    uint8_t placed[S_ITEMS];
    uint32_t scratch[22U * S_ITEMS];
    bp_cut_t cuts[S_CUTS];
    bp_cut_t export_cuts[S_CUTS];
    bp_cutplan_t plan;
    bp_cutview_t view;
} s_fixture_t;

static s_fixture_t fixture;

/*
 * Packs twelve 400 x 300 parts on 1000 x 700 plates with a 4 mm blade and
 * 10 mm trims: two strips of two on the 980 x 680 mm usable area, so four to
 * a plate and three plates.
 */
static void
s_init(s_fixture_t *f)
{
    bp_guillotine_result_t result;
    uint32_t index;

    memset(f, 0, sizeof *f);
    f->settings = (bp_cut_settings_t) {
        .plate_width_10um = 100000U, .plate_height_10um = 70000U,
        .trim_left_10um = 1000U, .trim_right_10um = 1000U,
        .trim_bottom_10um = 1000U, .trim_top_10um = 1000U,
        .kerf_10um = 400U, .max_stages = 3U, .first_cut = BP_CUT_FIRST_X
    };
    for (index = 0U; index < S_ITEMS; ++index) {
        f->items[index] = (bp_cut_item_t) { 40000U, 30000U, 0U };
        f->labels[index] = 7U;
    }
    result = bp_guillotine_pack((bp_guillotine_args_t) {
        .settings = &f->settings, .items = f->items, .item_count = S_ITEMS,
        .max_plates = S_ITEMS, .nodes = f->nodes, .node_capacity = S_NODES,
        .plates = f->plates, .plate_capacity = S_ITEMS, .placed = f->placed,
        .scratch = f->scratch, .scratch_capacity = 22U * S_ITEMS });
    CHECK(result.status == BP_GUILLOTINE_COMPLETE);
    CHECK(result.plate_count == 3U);
    f->plan = (bp_cutplan_t) {
        .settings = f->settings, .nodes = f->nodes,
        .node_count = result.node_count, .plates = f->plates,
        .plate_count = result.plate_count, .items = f->items,
        .item_count = S_ITEMS
    };
    f->view = bp_cutview_publish((bp_cutview_publish_args_t) {
        .plan = &f->plan, .labels = f->labels, .cuts = f->cuts,
        .cut_capacity = S_CUTS });
}

/* Publishing selects plate 1 fully cut; plates partition the saw list. */
static void
s_test_publish_and_select(void)
{
    uint32_t plate;
    uint32_t total = 0U;

    s_init(&fixture);
    CHECK(fixture.view.placed_count == S_ITEMS);
    CHECK(fixture.view.item_area_10um2 == (uint64_t) S_ITEMS * 40000U * 30000U);
    CHECK(fixture.view.cut_count == bp_cutplan_cut_count(&fixture.plan));
    CHECK(fixture.view.active_plate == 0U && fixture.view.first_cut == 0U);
    CHECK(fixture.view.visible_cuts == fixture.view.plate_cuts);
    CHECK(!fixture.view.saved);
    for (plate = 0U; plate < 3U; ++plate) {
        uint32_t index;

        bp_cutview_select(&fixture.view, plate);
        CHECK(fixture.view.first_cut == total);
        CHECK(fixture.view.visible_cuts == fixture.view.plate_cuts);
        /* Four edge trims open every plate's list. */
        CHECK(fixture.view.plate_cuts > 4U);
        for (index = 0U; index < fixture.view.plate_cuts; ++index) {
            const bp_cut_t *cut = bp_cutview_cut(&fixture.view, index);

            CHECK(cut->plate == plate);
            CHECK((cut->stage == 0U) == (index < 4U));
        }
        total += fixture.view.plate_cuts;
    }
    CHECK(total == fixture.view.cut_count);
    /* 12 parts of 0.12 m2 on three 0.7 m2 plates: 1.44 / 2.1. */
    CHECK(bp_cutview_utilisation(&fixture.view) > 0.6856
        && bp_cutview_utilisation(&fixture.view) < 0.6858);
}

/* The step replays the saw list from the untouched plate to the last cut. */
static void
s_test_step(void)
{
    s_init(&fixture);
    bp_cutview_select(&fixture.view, 1U);
    bp_cutview_set_visible(&fixture.view, 0U);
    CHECK(fixture.view.visible_cuts == 0U);
    bp_cutview_set_visible(&fixture.view, fixture.view.plate_cuts);
    bp_cutview_set_visible(&fixture.view, 3U);
    /* Selecting again shows the plate fully cut. */
    bp_cutview_select(&fixture.view, 1U);
    CHECK(fixture.view.visible_cuts == fixture.view.plate_cuts);
}

/* Export writes the list and one drawing per plate, then marks it saved. */
static void
s_test_export(void)
{
    char directory[] = "build/cutview-test-XXXXXX";
    char path[128];
    char scratch[160];
    char missing[160];
    bp_cutview_export_result_t result;
    bp_cutview_export_args_t args;
    struct stat info;
    uint32_t plate;

    s_init(&fixture);
    CHECK(mkdtemp(directory) != NULL);
    (void) snprintf(path, sizeof path, "%s/plan.svg", directory);
    args = (bp_cutview_export_args_t) {
        .view = &fixture.view, .path = path, .scratch_path = scratch,
        .path_capacity = sizeof scratch, .scratch_cuts = fixture.export_cuts,
        .scratch_cut_capacity = S_CUTS
    };
    bp_cutview_set_visible(&fixture.view, 0U);
    result = bp_cutview_export(args);
    CHECK(result.status == BP_CUTVIEW_EXPORT_OK && result.written_count == 4U);
    CHECK(result.system_error == 0 && fixture.view.saved);
    /* The step is a view, not an edit: the selection is untouched. */
    CHECK(fixture.view.visible_cuts == 0U);
    (void) snprintf(scratch, sizeof scratch, "%s/plan.csv", directory);
    CHECK(stat(scratch, &info) == 0 && info.st_size > 0);
    CHECK(unlink(scratch) == 0);
    for (plate = 1U; plate <= 3U; ++plate) {
        (void) snprintf(scratch, sizeof scratch, "%s/plan-%03u.svg", directory,
            (unsigned) plate);
        CHECK(stat(scratch, &info) == 0 && info.st_size > 0);
        CHECK(unlink(scratch) == 0);
    }

    /* A missing folder fails at the first file, with its errno. */
    fixture.view.saved = 0;
    (void) snprintf(missing, sizeof missing, "%s/missing/plan", directory);
    args.path = missing;
    result = bp_cutview_export(args);
    CHECK(result.status == BP_CUTVIEW_EXPORT_WRITE_FAILED);
    CHECK(result.written_count == 0U && result.system_error == ENOENT);
    CHECK(!fixture.view.saved);

    /* A name that does not fit is refused, not shortened. */
    args.path = path;
    args.path_capacity = strlen(path);
    result = bp_cutview_export(args);
    CHECK(result.status == BP_CUTVIEW_EXPORT_PATH_TOO_LONG);
    CHECK(!fixture.view.saved);
    CHECK(rmdir(directory) == 0);

    /* For make test: the same plan where check_cutplan.py reads it. */
    args.path = S_CASES_DIRECTORY "/view.csv";
    args.path_capacity = sizeof scratch;
    result = bp_cutview_export(args);
    CHECK(result.status == BP_CUTVIEW_EXPORT_OK);
}

/* A view without plates has nothing to save. */
static void
s_test_empty(void)
{
    bp_cutview_t empty;
    char scratch[64];
    bp_cut_t cuts[1];

    memset(&empty, 0, sizeof empty);
    CHECK(bp_cutview_utilisation(&empty) == 0.0);
    CHECK(bp_cutview_export((bp_cutview_export_args_t) {
        .view = &empty, .path = "nothing", .scratch_path = scratch,
        .path_capacity = sizeof scratch, .scratch_cuts = cuts,
        .scratch_cut_capacity = 1U }).status == BP_CUTVIEW_EXPORT_EMPTY);
}

/* Selecting a plate that does not exist is a caller defect. */
static void
s_body_bad_plate(void)
{
    s_init(&fixture);
    bp_cutview_select(&fixture.view, 3U);
}

/* Replaying past the last cut is a caller defect. */
static void
s_body_bad_step(void)
{
    s_init(&fixture);
    bp_cutview_set_visible(&fixture.view, fixture.view.plate_cuts + 1U);
}

/* Exercises the GUI-independent view of a guillotine plan. */
int
main(void)
{
    CHECK(mkdir(S_CASES_DIRECTORY, 0755) == 0 || errno == EEXIST);
    s_test_publish_and_select();
    s_test_step();
    s_test_export();
    s_test_empty();
    t_expect_abort(s_body_bad_plate, "bad plate");
    t_expect_abort(s_body_bad_step, "bad step");
    (void) printf("test_bp_cutview: all checks passed\n");
    return 0;
}
