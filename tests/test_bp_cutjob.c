#include "test_support.h"

#include "bp_cutjob.h"
#include "bp_job.h"

#include <math.h>
#include <string.h>

#define S_ITEMS 16U

/* Job files live in one scratch directory under build, removed at the end. */
typedef struct {
    char directory[64];
    char path[128];
} s_files_t;

static s_files_t files;

/* The outcome of loading a job text and building its saw input. */
typedef struct {
    bp_job_load_result_t loaded;
    bp_cutjob_result_t built;
    bp_cut_settings_t settings;
    bp_cut_item_t items[S_ITEMS];
} s_run_t;

/* Writes text as the job file every case loads. */
static void
s_write_job(const char *text)
{
    FILE *output = fopen(files.path, "w");

    CHECK(output != NULL);
    CHECK(fputs(text, output) >= 0);
    CHECK(fclose(output) == 0);
}

/* Loads a job for sawing and builds its settings and items. */
static void
s_run(s_run_t *run, const char *text)
{
    memset(run, 0, sizeof *run);
    s_write_job(text);
    run->loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .rectangles_only = 1 });
    CHECK(run->loaded.status == BP_JOB_OK);
    CHECK(run->loaded.job->part_count <= S_ITEMS);
    run->built = bp_cutjob_build((bp_cutjob_args_t) {
        .job = run->loaded.job, .settings = &run->settings,
        .items = run->items, .item_capacity = S_ITEMS });
}

/* Releases a run's job. */
static void
s_done(s_run_t *run)
{
    bp_job_destroy(&run->loaded.job);
}

/* Every saw line is read, and the rounding never favours the plan. */
static void
s_test_settings(void)
{
    s_run_t run;

    s_run(&run, "sheet 2800.009 2070\n"
        "clearance 1\n"
        "kerf 4.4\n"
        "trim 10 12.5 0.001 8\n"
        "stages 4\n"
        "first-cut y\n"
        "min-waste 20.004\n"
        "sizing-cut 1\n"
        "rect 7 2 600.004 300 1\n"
        "rect 9 1 100 50.5 0\n");
    CHECK(run.built.status == BP_CUTJOB_OK);
    /* The plate rounds down, everything that costs material rounds up. */
    CHECK(run.settings.plate_width_10um == 280000U);
    CHECK(run.settings.plate_height_10um == 207000U);
    CHECK(run.settings.kerf_10um == 440U);
    CHECK(run.settings.trim_left_10um == 1000U);
    CHECK(run.settings.trim_right_10um == 1250U);
    CHECK(run.settings.trim_bottom_10um == 1U);
    CHECK(run.settings.trim_top_10um == 800U);
    CHECK(run.settings.min_waste_10um == 2001U);
    CHECK(run.settings.max_stages == 4U);
    CHECK(run.settings.first_cut == BP_CUT_FIRST_Y);
    CHECK(run.settings.non_exact == 1U);
    CHECK(run.items[0].width_10um == 60001U && run.items[0].height_10um == 30000U);
    CHECK(run.items[0].may_rotate == 1U);
    CHECK(run.items[1].width_10um == 60001U && run.items[1].may_rotate == 1U);
    CHECK(run.items[2].width_10um == 10000U && run.items[2].height_10um == 5050U);
    CHECK(run.items[2].may_rotate == 0U);
    CHECK(run.loaded.job->labels[1] == 7U && run.loaded.job->labels[2] == 9U);
    /* Sawing needs no raster, so none was built. */
    CHECK(run.loaded.job->parts == NULL);
    CHECK(run.loaded.job->sheet_width_cells == 0U);
    s_done(&run);
}

/* Without saw lines: the documented defaults, and a single trim for all edges. */
static void
s_test_defaults(void)
{
    s_run_t run;

    s_run(&run, "sheet 1000 500\nkerf 3.2\ntrim 10\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_OK);
    CHECK(run.settings.max_stages == 3U);
    CHECK(run.settings.first_cut == BP_CUT_FIRST_ANY);
    CHECK(run.settings.min_waste_10um == 0U);
    CHECK(run.settings.non_exact == 0U);
    CHECK(run.settings.kerf_10um == 320U);
    CHECK(run.settings.trim_left_10um == 1000U && run.settings.trim_top_10um == 1000U);
    s_done(&run);
}

/* What a saw cannot do is reported, with the piece concerned. */
static void
s_test_refusals(void)
{
    s_run_t run;

    s_run(&run, "sheet 1000 500\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_NO_KERF);
    s_done(&run);

    s_run(&run, "sheet 1000 500\nkerf 4\nrect 1 2 10 10 0\n"
        "poly 2 1 0\nring 3 0 0 10 0 0 10\n");
    CHECK(run.built.status == BP_CUTJOB_NOT_RECTANGLE);
    CHECK(run.built.part == 2U);
    s_done(&run);

    /* One label for two different parts, apart or next to each other. */
    s_run(&run, "sheet 1000 500\nkerf 4\nrect 1 1 10 10 0\nrect 2 1 20 20 0\n"
        "rect 1 1 30 30 0\n");
    CHECK(run.built.status == BP_CUTJOB_DUPLICATE_LABEL);
    CHECK(run.built.part == 2U);
    s_done(&run);
    s_run(&run, "sheet 1000 500\nkerf 4\nrect 1 2 10 10 0\nrect 1 1 30 30 0\n");
    CHECK(run.built.status == BP_CUTJOB_DUPLICATE_LABEL);
    CHECK(run.built.part == 2U);
    s_done(&run);
    /* The same part on two lines is just a larger quantity. */
    s_run(&run, "sheet 1000 500\nkerf 4\nrect 1 2 10 10 0\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_OK);
    s_done(&run);

    s_run(&run, "sheet 1000 500\nkerf 4\ntrim 500 500 0 0\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_NO_USABLE_AREA);
    s_done(&run);
    /* Trims round up, so 499.995 takes the last 0.01 mm. */
    s_run(&run, "sheet 1000 500\nkerf 4\ntrim 499.995 500 0 0\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_NO_USABLE_AREA);
    s_done(&run);
    s_run(&run, "sheet 1000 500\nkerf 4\ntrim 499.99 500 0 0\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_OK);
    s_done(&run);

    /* Far beyond what the raster budget allows, and still sawable. */
    s_run(&run, "sheet 6000 3210\nkerf 4\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_OK);
    CHECK(run.settings.plate_width_10um == 600000U);
    s_done(&run);
    s_run(&run, "sheet 50000000 500\nkerf 4\nrect 1 1 10 10 0\n");
    CHECK(run.built.status == BP_CUTJOB_TOO_LARGE);
    s_done(&run);
}

/* Malformed saw lines are bad lines, with their line number. */
static void
s_test_bad_lines(void)
{
    static const char *const lines[] = {
        "kerf -1\n", "kerf nan\n", "trim 1 2\n", "trim 1 2 3\n", "trim -1\n",
        "trim inf\n", "stages 0\n", "stages 9\n", "first-cut z\n",
        "min-waste -0.5\n", "sizing-cut 2\n", "rect 1 1 nan 10 0\n"
    };
    size_t index;

    for (index = 0U; index < sizeof lines / sizeof lines[0]; ++index) {
        char text[128];
        bp_job_load_result_t loaded;

        (void) snprintf(text, sizeof text, "sheet 1000 500\n%srect 1 1 10 10 0\n",
            lines[index]);
        s_write_job(text);
        loaded = bp_job_load_with((bp_job_load_args_t) {
            .path = files.path, .rectangles_only = 1 });
        if (loaded.status != BP_JOB_BAD_LINE || loaded.line_number != 2U) {
            (void) fprintf(stderr, "accepted: %s", lines[index]);
            CHECK(0);
        }
    }
}

/* Free nesting still loads the same file, saw lines and all. */
static void
s_test_free_nesting_unaffected(void)
{
    bp_job_load_result_t loaded;

    s_write_job("sheet 1000 500\nkerf 4\ntrim 10\nstages 2\nrect 1 2 100 50 1\n");
    loaded = bp_job_load_with((bp_job_load_args_t) { .path = files.path });
    CHECK(loaded.status == BP_JOB_OK);
    CHECK(loaded.job->parts != NULL && loaded.job->part_count == 2U);
    CHECK(loaded.job->rect_width_mm[1] == 100.0);
    CHECK(loaded.job->rect_may_turn[1] == 1U);
    CHECK(loaded.job->labels[1] == 1U);
    bp_job_destroy(&loaded.job);
}

/* Unit conversion at its edges. */
static void
s_test_units(void)
{
    uint32_t units = 7U;

    CHECK(bp_cutjob_units_up(0.0, &units) && units == 0U);
    CHECK(bp_cutjob_units_up(4.4, &units) && units == 440U);
    CHECK(bp_cutjob_units_up(0.001, &units) && units == 1U);
    CHECK(bp_cutjob_units_down(0.009, &units) && units == 0U);
    CHECK(bp_cutjob_units_down(1250.0, &units) && units == 125000U);
    CHECK(bp_cutjob_units_down(42949672.95, &units) && units == UINT32_MAX);
    units = 7U;
    CHECK(!bp_cutjob_units_up(42949672.96, &units) && units == 7U);
}

/* Every status has its own message. */
static void
s_test_status_text(void)
{
    int status;

    for (status = BP_CUTJOB_OK; status <= BP_CUTJOB_TOO_LARGE; ++status)
        CHECK(strcmp(bp_cutjob_status_text((bp_cutjob_status_t) status),
            "unknown status") != 0);
}

/* A converter given a negative length is a caller defect. */
static void
s_body_negative_units(void)
{
    uint32_t units;

    (void) bp_cutjob_units_up(-1.0, &units);
}

/* Too few item slots for the job's pieces is a caller defect. */
static void
s_body_small_items(void)
{
    s_run_t run;
    bp_job_load_result_t loaded;

    s_write_job("sheet 1000 500\nkerf 4\nrect 1 3 10 10 0\n");
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .rectangles_only = 1 });
    (void) bp_cutjob_build((bp_cutjob_args_t) {
        .job = loaded.job, .settings = &run.settings, .items = run.items,
        .item_capacity = 2U });
}

/* Exercises the job lines for sawing and their conversion. */
int
main(void)
{
    (void) snprintf(files.directory, sizeof files.directory,
        "build/cutjob-test-XXXXXX");
    CHECK(mkdtemp(files.directory) != NULL);
    (void) snprintf(files.path, sizeof files.path, "%s/test.job",
        files.directory);

    s_test_settings();
    s_test_defaults();
    s_test_refusals();
    s_test_bad_lines();
    s_test_free_nesting_unaffected();
    s_test_units();
    s_test_status_text();
    t_expect_abort(s_body_negative_units, "negative units");
    t_expect_abort(s_body_small_items, "small item buffer");

    CHECK(unlink(files.path) == 0);
    CHECK(rmdir(files.directory) == 0);
    (void) printf("test_bp_cutjob: all checks passed\n");
    return 0;
}
