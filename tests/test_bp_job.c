#include "test_support.h"

#include "bp_job.h"
#include "bp_penetration.h"
#include "bp_place.h"

#include <math.h>
#include <string.h>

/* Job files live in one scratch directory under build, removed at the end. */
typedef struct {
    char directory[64];
    char path[128];
} s_files_t;

static s_files_t files;

/* Writes text as the job file every case loads. */
static void
s_write_job(const char *text)
{
    FILE *output = fopen(files.path, "w");

    CHECK(output != NULL);
    CHECK(fputs(text, output) >= 0);
    CHECK(fclose(output) == 0);
}

/* Returns the job file's bytes, to prove an override leaves the file alone. */
static void
s_read_job(char *text, size_t capacity)
{
    FILE *input = fopen(files.path, "r");
    size_t length;

    CHECK(input != NULL);
    length = fread(text, 1U, capacity - 1U, input);
    CHECK(fclose(input) == 0);
    text[length] = '\0';
}

/*
 * Checks that every buffer sized by the sheet matches the sheet actually in
 * use, which is the point of applying an override before allocation.
 */
static void
s_check_sheet_buffers(const bp_job_load_result_t *loaded, uint32_t width_cells,
    uint32_t height_cells)
{
    const bp_job_t *job = loaded->job;

    CHECK(loaded->status == BP_JOB_OK && job != NULL);
    CHECK(job->sheet_width_cells == width_cells);
    CHECK(job->sheet_height_cells == height_cells);
    CHECK(job->sheet.width_cells == width_cells);
    CHECK(job->sheet.height_cells == height_cells);
    CHECK(job->sheet.stride_words == (width_cells + 63U) / 64U);
    CHECK(job->sheet.word_capacity
        == bp_bitmap_required_words(width_cells, height_cells));
    CHECK(job->scratch_row_capacity == bp_place_scratch_words(&job->sheet));
    CHECK(job->scratch_count_capacity
        == bp_penetration_count_capacity(width_cells, height_cells));
    CHECK(job->scratch_prefix_capacity
        == bp_penetration_prefix_capacity(width_cells, height_cells));
    CHECK(loaded->sheet_plan.status == BP_SHEET_OK);
    CHECK(loaded->sheet_plan.width_cells == width_cells);
    CHECK(loaded->sheet_plan.height_cells == height_cells);
    CHECK(loaded->sheet_plan.buffer_bytes
        == bp_sheet_buffer_bytes(width_cells, height_cells));
}

/* Without overrides the file decides, as it always has. */
static void
s_test_size_from_file(void)
{
    bp_job_load_result_t loaded;

    s_write_job("sheet 800 600\ncell 1\nclearance 1\nrect 7 2 100 50 1\n");
    loaded = bp_job_load_with((bp_job_load_args_t) { .path = files.path });
    s_check_sheet_buffers(&loaded, 800U, 600U);
    CHECK(loaded.job->sheet_width_mm == 800.0);
    CHECK(loaded.job->cell_size_mm == 1.0);
    CHECK(loaded.job->file_sheet_width_mm == 800.0);
    CHECK(loaded.job->file_cell_size_mm == 1.0);
    CHECK(loaded.job->clearance_cells == 1U);
    CHECK(loaded.job->part_count == 2U && loaded.job->labels[1] == 7U);
    bp_job_destroy(&loaded.job);
    CHECK(loaded.job == NULL);
}

/* An override replaces the file's size and leaves the file untouched. */
static void
s_test_sheet_override(void)
{
    const char *text = "sheet 1200 1200\nrect 1 1 100 50 0\n";
    char after[256];
    bp_job_load_result_t loaded;

    s_write_job(text);
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 800.0, .sheet_height_mm = 600.0
    });
    s_check_sheet_buffers(&loaded, 800U, 600U);
    CHECK(loaded.job->sheet_width_mm == 800.0);
    CHECK(loaded.job->sheet_height_mm == 600.0);
    bp_job_destroy(&loaded.job);

    /* Width and height are taken as given, never sorted. */
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 600.0, .sheet_height_mm = 800.0
    });
    s_check_sheet_buffers(&loaded, 600U, 800U);
    bp_job_destroy(&loaded.job);

    /* Word boundaries are where a stale stride would show. */
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 65.0, .sheet_height_mm = 3.0
    });
    s_check_sheet_buffers(&loaded, 65U, 3U);
    bp_job_destroy(&loaded.job);
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 64.0, .sheet_height_mm = 3.0
    });
    s_check_sheet_buffers(&loaded, 64U, 3U);
    bp_job_destroy(&loaded.job);

    s_read_job(after, sizeof after);
    CHECK(strcmp(after, text) == 0);
}

/* Both overrides apply together and win over both file lines. */
static void
s_test_override_priority(void)
{
    bp_job_load_result_t loaded;

    s_write_job("sheet 1200 1200\ncell 2\nclearance 1\nrect 1 1 10 10 0\n");
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 300.0, .sheet_height_mm = 200.0,
        .cell_size_mm = 0.5
    });
    s_check_sheet_buffers(&loaded, 600U, 400U);
    CHECK(loaded.job->cell_size_mm == 0.5);
    /* The file's own values stay visible next to the overrides. */
    CHECK(loaded.job->file_sheet_width_mm == 1200.0);
    CHECK(loaded.job->file_sheet_height_mm == 1200.0);
    CHECK(loaded.job->file_cell_size_mm == 2.0);
    CHECK(loaded.sheet_plan.width_mm == 300.0);
    CHECK(loaded.sheet_plan.cell_size_mm == 0.5);
    /* Clearance follows the cell size in force, not the file's. */
    CHECK(loaded.job->clearance_cells == 2U);
    bp_job_destroy(&loaded.job);

    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .cell_size_mm = 0.5
    });
    s_check_sheet_buffers(&loaded, 2400U, 2400U);
    bp_job_destroy(&loaded.job);
}

/* The old entry point keeps its behaviour and its lack of guardrails. */
static void
s_test_legacy_wrapper(void)
{
    bp_job_load_result_t loaded;

    s_write_job("sheet 800 600\ncell 1\nrect 1 1 10 10 0\n");
    loaded = bp_job_load(files.path, 0.0);
    s_check_sheet_buffers(&loaded, 800U, 600U);
    bp_job_destroy(&loaded.job);
    loaded = bp_job_load(files.path, 0.5);
    s_check_sheet_buffers(&loaded, 1600U, 1200U);
    bp_job_destroy(&loaded.job);

    /* Over the default axis limit, which the old API never enforced. */
    s_write_job("sheet 8193 1\nrect 1 1 1 1 0\n");
    loaded = bp_job_load(files.path, 0.0);
    s_check_sheet_buffers(&loaded, 8193U, 1U);
    bp_job_destroy(&loaded.job);
    loaded = bp_job_load_with((bp_job_load_args_t) { .path = files.path });
    CHECK(loaded.status == BP_JOB_TOO_LARGE && loaded.job == NULL);
    CHECK(loaded.sheet_plan.status == BP_SHEET_AXIS_OVER_LIMIT);
}

/* A bad override is reported before the file is even opened. */
static void
s_test_bad_overrides(void)
{
    const double bad[] = { -1.0, NAN, INFINITY, -INFINITY };
    bp_job_load_result_t loaded;
    size_t index;

    s_write_job("sheet 800 600\nrect 1 1 10 10 0\n");
    for (index = 0U; index < sizeof bad / sizeof bad[0]; ++index) {
        loaded = bp_job_load_with((bp_job_load_args_t) {
            .path = files.path, .sheet_width_mm = bad[index],
            .sheet_height_mm = 600.0
        });
        CHECK(loaded.status == BP_JOB_BAD_OVERRIDE && loaded.job == NULL);
        loaded = bp_job_load_with((bp_job_load_args_t) {
            .path = files.path, .sheet_width_mm = 800.0,
            .sheet_height_mm = bad[index]
        });
        CHECK(loaded.status == BP_JOB_BAD_OVERRIDE && loaded.job == NULL);
        loaded = bp_job_load_with((bp_job_load_args_t) {
            .path = files.path, .cell_size_mm = bad[index]
        });
        CHECK(loaded.status == BP_JOB_BAD_OVERRIDE && loaded.job == NULL);
        CHECK(loaded.sheet_plan.status == BP_SHEET_OK);
        CHECK(loaded.sheet_plan.buffer_bytes == 0U);
    }
    /* Half a size is not quietly completed from the file. */
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 800.0
    });
    CHECK(loaded.status == BP_JOB_BAD_OVERRIDE);
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = "build/no-such-dir/no.job", .cell_size_mm = -1.0
    });
    CHECK(loaded.status == BP_JOB_BAD_OVERRIDE);
}

/* Sizes outside the budget are refused with the reason, never clamped. */
static void
s_test_rejected_sizes(void)
{
    bp_job_load_result_t loaded;
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    s_write_job("sheet 800 600\nrect 1 1 10 10 0\n");
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 1250.0,
        .sheet_height_mm = 2500.0, .cell_size_mm = 0.25
    });
    CHECK(loaded.status == BP_JOB_TOO_LARGE && loaded.job == NULL);
    CHECK(loaded.sheet_plan.status == BP_SHEET_AXIS_OVER_LIMIT);
    CHECK(bp_sheet_suggest_cell_size_mm(1250.0, 2500.0, limits) == 0.5);

    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 0.5, .sheet_height_mm = 600.0
    });
    CHECK(loaded.status == BP_JOB_SHEET_TOO_SMALL && loaded.job == NULL);
    CHECK(loaded.sheet_plan.status == BP_SHEET_TOO_SMALL);

    limits.max_buffer_bytes = bp_sheet_buffer_bytes(800U, 600U) - 1U;
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .limits = limits
    });
    CHECK(loaded.status == BP_JOB_TOO_LARGE && loaded.job == NULL);
    CHECK(loaded.sheet_plan.status == BP_SHEET_BYTES_OVER_LIMIT);
    CHECK(loaded.sheet_plan.buffer_bytes == bp_sheet_buffer_bytes(800U, 600U));
    limits.max_buffer_bytes += 1U;
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .limits = limits
    });
    s_check_sheet_buffers(&loaded, 800U, 600U);
    bp_job_destroy(&loaded.job);
}

/*
 * A part whose raster scratch is over budget refuses the load even when the
 * sheet fits; the OK sheet plan is how a caller tells the two apart. A part
 * that merely does not fit on the sheet still loads, to be reported unplaced.
 */
static void
s_test_part_budget(void)
{
    bp_job_load_result_t loaded;
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    s_write_job("sheet 10 10\nrect 1 1 1000 1000 0\n");
    loaded = bp_job_load_with((bp_job_load_args_t) { .path = files.path });
    s_check_sheet_buffers(&loaded, 10U, 10U);
    bp_job_destroy(&loaded.job);

    limits.max_buffer_bytes = 10000U;
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .limits = limits
    });
    CHECK(loaded.status == BP_JOB_TOO_LARGE && loaded.job == NULL);
    CHECK(loaded.sheet_plan.status == BP_SHEET_OK);
}

/* Non-finite numbers in the file are a bad line, not an abort. */
static void
s_test_non_finite_file_values(void)
{
    static const char *const texts[] = {
        "sheet nan 600\nrect 1 1 10 10 0\n",
        "sheet 800 inf\nrect 1 1 10 10 0\n",
        "sheet 800 600\ncell nan\nrect 1 1 10 10 0\n",
        "sheet 800 600\ncell inf\nrect 1 1 10 10 0\n",
        "sheet 800 600\nclearance nan\nrect 1 1 10 10 0\n",
        "sheet 800 600\nclearance inf\nrect 1 1 10 10 0\n"
    };
    size_t index;

    for (index = 0U; index < sizeof texts / sizeof texts[0]; ++index) {
        bp_job_load_result_t loaded;

        s_write_job(texts[index]);
        loaded = bp_job_load_with((bp_job_load_args_t) { .path = files.path });
        CHECK(loaded.status == BP_JOB_BAD_LINE && loaded.job == NULL);
        CHECK(loaded.line_number == (index < 2U ? 1U : 2U));
    }
}

/* An override replaces a sheet line; it does not stand in for a missing one. */
static void
s_test_override_needs_sheet_line(void)
{
    bp_job_load_result_t loaded;

    s_write_job("rect 1 1 10 10 0\n");
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = files.path, .sheet_width_mm = 800.0, .sheet_height_mm = 600.0
    });
    CHECK(loaded.status == BP_JOB_NO_SHEET && loaded.job == NULL);
}

/* Every status has its own message. */
static void
s_test_status_text(void)
{
    int status;

    for (status = BP_JOB_OK; status <= BP_JOB_BAD_OVERRIDE; ++status) {
        const char *text = bp_job_status_text((bp_job_status_t) status);

        CHECK(text != NULL && strcmp(text, "unknown status") != 0);
    }
}

/* A missing path is a caller defect. */
static void
s_body_null_path(void)
{
    (void) bp_job_load_with((bp_job_load_args_t) { .path = NULL });
}

/* Limits are all given or all defaulted, never partly zero. */
static void
s_body_partial_limits(void)
{
    (void) bp_job_load_with((bp_job_load_args_t) {
        .path = files.path,
        .limits = { .max_cells_per_axis = 100U }
    });
}

/* The old entry point still treats a negative override as a defect. */
static void
s_body_legacy_negative_cell(void)
{
    (void) bp_job_load(files.path, -1.0);
}

/* Exercises loading, sheet and cell overrides and the budget. */
int
main(void)
{
    (void) snprintf(files.directory, sizeof files.directory,
        "build/job-test-XXXXXX");
    CHECK(mkdtemp(files.directory) != NULL);
    (void) snprintf(files.path, sizeof files.path, "%s/test.job",
        files.directory);

    s_test_size_from_file();
    s_test_sheet_override();
    s_test_override_priority();
    s_test_legacy_wrapper();
    s_test_bad_overrides();
    s_test_rejected_sizes();
    s_test_part_budget();
    s_test_non_finite_file_values();
    s_test_override_needs_sheet_line();
    s_test_status_text();
    t_expect_abort(s_body_null_path, "NULL job path");
    t_expect_abort(s_body_partial_limits, "partial load limits");
    t_expect_abort(s_body_legacy_negative_cell, "negative legacy cell");

    CHECK(unlink(files.path) == 0);
    CHECK(rmdir(files.directory) == 0);
    (void) printf("test_bp_job: all checks passed\n");
    return 0;
}
