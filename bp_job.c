#include "bp_job.h"

#include "bp_contract.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_LINE_MAX 256U

/* One line of the job file, before it is expanded into part instances. */
typedef struct {
    uint32_t id;
    uint32_t quantity;
    double width_mm;
    double height_mm;
    int allow_quarter_turn;
} s_spec_t;

typedef struct {
    s_spec_t *specs;
    uint32_t count;
    uint32_t capacity;
} s_spec_list_t;

/* Returns a short human-readable form of a status. */
const char *
bp_job_status_text(bp_job_status_t status)
{
    switch (status) {
    case BP_JOB_OK:
        return "ok";
    case BP_JOB_CANNOT_OPEN:
        return "cannot open job file";
    case BP_JOB_BAD_LINE:
        return "unrecognised or invalid line";
    case BP_JOB_NO_SHEET:
        return "job has no sheet line";
    case BP_JOB_NO_PARTS:
        return "job has no parts";
    case BP_JOB_SHEET_TOO_SMALL:
        return "sheet is smaller than one cell";
    case BP_JOB_TOO_LARGE:
        return "job is too large for the chosen cell size";
    case BP_JOB_OUT_OF_MEMORY:
        return "out of memory";
    default:
        return "unknown status";
    }
}

/* Rounds up, so a part is never modelled smaller than it really is. */
static uint32_t
s_cells_up(double millimetres, double cell_size_mm, int *too_large)
{
    double cells;

    BP_REQUIRE(millimetres >= 0.0);
    BP_REQUIRE(cell_size_mm > 0.0);
    BP_REQUIRE(too_large);
    cells = ceil(millimetres / cell_size_mm - 1e-9);
    if (cells > 1e8) {
        *too_large = 1;
        return 1U;
    }
    return (uint32_t) cells;
}

/* Rounds down, so the sheet is never modelled larger than it really is. */
static uint32_t
s_cells_down(double millimetres, double cell_size_mm, int *too_large)
{
    double cells;

    BP_REQUIRE(millimetres >= 0.0);
    BP_REQUIRE(cell_size_mm > 0.0);
    BP_REQUIRE(too_large);
    cells = floor(millimetres / cell_size_mm + 1e-9);
    if (cells > 1e8) {
        *too_large = 1;
        return 1U;
    }
    return (uint32_t) cells;
}

/* Appends one specification, growing the array. Returns 0 out of memory. */
static int
s_add_spec(s_spec_list_t *list, s_spec_t spec)
{
    if (list->count == list->capacity) {
        uint32_t capacity = list->capacity ? list->capacity * 2U : 16U;
        s_spec_t *grown = realloc(list->specs, capacity * sizeof *grown);

        if (!grown)
            return 0;
        list->specs = grown;
        list->capacity = capacity;
    }
    list->specs[list->count] = spec;
    ++list->count;
    return 1;
}

/* Parses the file into specifications and the sheet settings. */
static bp_job_status_t
s_parse(const char *path, double cell_size_override_mm, bp_job_t *job,
    s_spec_list_t *list, unsigned *line_number)
{
    FILE *input = fopen(path, "r");
    char line[S_LINE_MAX];
    int have_sheet = 0;

    *line_number = 0U;
    if (!input)
        return BP_JOB_CANNOT_OPEN;

    job->cell_size_mm = 1.0;
    job->clearance_mm = 0.0;

    while (fgets(line, (int) sizeof line, input)) {
        char *comment = strchr(line, '#');
        s_spec_t spec;
        double a;
        double b;
        unsigned id;
        unsigned quantity;
        unsigned turn;

        ++*line_number;
        if (comment)
            *comment = '\0';

        if (sscanf(line, " sheet %lf %lf", &a, &b) == 2) {
            if (a <= 0.0 || b <= 0.0)
                goto bad_line;
            job->sheet_width_mm = a;
            job->sheet_height_mm = b;
            have_sheet = 1;
        } else if (sscanf(line, " cell %lf", &a) == 1) {
            if (a <= 0.0)
                goto bad_line;
            job->cell_size_mm = a;
        } else if (sscanf(line, " clearance %lf", &a) == 1) {
            if (a < 0.0)
                goto bad_line;
            job->clearance_mm = a;
        } else if (sscanf(line, " rect %u %u %lf %lf %u", &id, &quantity, &a,
            &b, &turn) == 5) {
            if (quantity == 0U || a <= 0.0 || b <= 0.0 || turn > 1U)
                goto bad_line;
            spec.id = (uint32_t) id;
            spec.quantity = (uint32_t) quantity;
            spec.width_mm = a;
            spec.height_mm = b;
            spec.allow_quarter_turn = (int) turn;
            if (!s_add_spec(list, spec)) {
                (void) fclose(input);
                return BP_JOB_OUT_OF_MEMORY;
            }
        } else if (strspn(line, " \t\r\n") != strlen(line)) {
            goto bad_line;
        }
    }
    (void) fclose(input);

    /* Past this point the failures are about the file as a whole. */
    *line_number = 0U;
    if (cell_size_override_mm > 0.0)
        job->cell_size_mm = cell_size_override_mm;
    if (!have_sheet)
        return BP_JOB_NO_SHEET;
    if (list->count == 0U)
        return BP_JOB_NO_PARTS;
    return BP_JOB_OK;

bad_line:
    (void) fclose(input);
    return BP_JOB_BAD_LINE;
}

/* Sums the storage every mask of every specification will need. */
static void
s_measure(const bp_job_t *job, const s_spec_list_t *list,
    size_t *row_offsets, size_t *spans, uint32_t *masks,
    uint32_t *orientations, uint32_t *instances, int *too_large)
{
    uint32_t index;

    *row_offsets = 0U;
    *spans = 0U;
    *masks = 0U;
    *orientations = 0U;
    *instances = 0U;

    for (index = 0U; index < list->count; ++index) {
        const s_spec_t *spec = &list->specs[index];
        uint32_t width = s_cells_up(spec->width_mm, job->cell_size_mm,
            too_large);
        uint32_t height = s_cells_up(spec->height_mm, job->cell_size_mm,
            too_large);
        uint32_t heights[2] = { height, width };
        uint32_t option;
        uint32_t option_count = (spec->allow_quarter_turn && width != height)
            ? 2U
            : 1U;

        for (option = 0U; option < option_count; ++option) {
            uint32_t solid_rows = heights[option];
            uint32_t grown_rows = solid_rows + 2U * job->clearance_cells;

            *row_offsets += (size_t) solid_rows + 1U;
            *spans += solid_rows;
            *row_offsets += (size_t) grown_rows + 1U;
            *spans += grown_rows;
            *masks += 2U;
            ++*orientations;
        }
        *instances += spec->quantity;
    }
}

/*
 * Builds the masks. Instances of the same specification share their masks:
 * masks are read-only during a solve, so copies would only cost memory.
 */
static void
s_build_parts(bp_job_t *job, const s_spec_list_t *list)
{
    size_t used_row_offsets = 0U;
    size_t used_spans = 0U;
    uint32_t used_masks = 0U;
    uint32_t used_orientations = 0U;
    uint32_t instance = 0U;
    uint32_t index;
    int too_large = 0;

    for (index = 0U; index < list->count; ++index) {
        const s_spec_t *spec = &list->specs[index];
        uint32_t width = s_cells_up(spec->width_mm, job->cell_size_mm,
            &too_large);
        uint32_t height = s_cells_up(spec->height_mm, job->cell_size_mm,
            &too_large);
        uint32_t widths[2] = { width, height };
        uint32_t heights[2] = { height, width };
        uint32_t option_count = (spec->allow_quarter_turn && width != height)
            ? 2U
            : 1U;
        uint32_t first_orientation = used_orientations;
        uint32_t option;
        uint32_t copy;

        for (option = 0U; option < option_count; ++option) {
            uint32_t solid_height = heights[option];
            uint32_t grown_height = solid_height + 2U * job->clearance_cells;
            bp_mask_t *solid = &job->owned_masks[used_masks++];
            bp_mask_t *collision = &job->owned_masks[used_masks++];

            *solid = bp_mask_rectangle((bp_mask_rectangle_args_t) {
                .width_cells = widths[option],
                .height_cells = solid_height,
                .row_offsets = job->owned_row_offsets + used_row_offsets,
                .row_offset_capacity = (size_t) solid_height + 1U,
                .spans = job->owned_spans + used_spans,
                .span_capacity = solid_height
            });
            used_row_offsets += (size_t) solid_height + 1U;
            used_spans += solid_height;

            *collision = bp_mask_rectangle((bp_mask_rectangle_args_t) {
                .width_cells = widths[option] + 2U * job->clearance_cells,
                .height_cells = grown_height,
                .row_offsets = job->owned_row_offsets + used_row_offsets,
                .row_offset_capacity = (size_t) grown_height + 1U,
                .spans = job->owned_spans + used_spans,
                .span_capacity = grown_height
            });
            used_row_offsets += (size_t) grown_height + 1U;
            used_spans += grown_height;

            job->owned_orientations[used_orientations].collision = collision;
            job->owned_orientations[used_orientations].solid = solid;
            job->owned_orientations[used_orientations].solid_offset_x_cells =
                job->clearance_cells;
            job->owned_orientations[used_orientations].solid_offset_y_cells =
                job->clearance_cells;
            ++used_orientations;
        }

        for (copy = 0U; copy < spec->quantity; ++copy) {
            job->owned_parts[instance].id = instance + 1U;
            job->owned_parts[instance].orientations =
                &job->owned_orientations[first_orientation];
            job->owned_parts[instance].orientation_count = option_count;
            job->owned_labels[instance] = spec->id;
            ++instance;
        }
    }
    BP_REQUIRE(instance == job->part_count);
}

/* Reads a job file and builds everything a solve needs. */
bp_job_load_result_t
bp_job_load(const char *path, double cell_size_override_mm)
{
    bp_job_load_result_t result = { BP_JOB_OK, 0U, NULL };
    s_spec_list_t list = { NULL, 0U, 0U };
    bp_job_t *job;
    size_t row_offsets = 0U;
    size_t spans = 0U;
    uint32_t masks = 0U;
    uint32_t orientations = 0U;
    uint32_t instances = 0U;
    int too_large = 0;
    size_t sheet_words;

    BP_REQUIRE(path);
    BP_REQUIRE(cell_size_override_mm >= 0.0);

    job = calloc(1U, sizeof *job);
    if (!job) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        return result;
    }

    result.status = s_parse(path, cell_size_override_mm, job, &list,
        &result.line_number);
    if (result.status != BP_JOB_OK)
        goto failed;
    result.line_number = 0U;

    job->clearance_cells = s_cells_up(job->clearance_mm, job->cell_size_mm,
        &too_large);
    job->sheet_width_cells = s_cells_down(job->sheet_width_mm,
        job->cell_size_mm, &too_large);
    job->sheet_height_cells = s_cells_down(job->sheet_height_mm,
        job->cell_size_mm, &too_large);
    s_measure(job, &list, &row_offsets, &spans, &masks, &orientations,
        &instances, &too_large);

    if (too_large) {
        result.status = BP_JOB_TOO_LARGE;
        goto failed;
    }
    if (job->sheet_width_cells == 0U || job->sheet_height_cells == 0U) {
        result.status = BP_JOB_SHEET_TOO_SMALL;
        goto failed;
    }

    job->part_count = instances;
    sheet_words = bp_bitmap_required_words(job->sheet_width_cells,
        job->sheet_height_cells);

    job->owned_sheet_words = calloc(sheet_words, sizeof *job->owned_sheet_words);
    job->owned_row_offsets = calloc(row_offsets, sizeof *job->owned_row_offsets);
    job->owned_spans = calloc(spans, sizeof *job->owned_spans);
    job->owned_masks = calloc(masks, sizeof *job->owned_masks);
    job->owned_orientations = calloc(orientations,
        sizeof *job->owned_orientations);
    job->owned_parts = calloc(instances, sizeof *job->owned_parts);
    job->owned_labels = calloc(instances, sizeof *job->owned_labels);
    job->order = calloc(instances, sizeof *job->order);
    job->placements = calloc(instances, sizeof *job->placements);

    if (!job->owned_sheet_words || !job->owned_row_offsets || !job->owned_spans
        || !job->owned_masks || !job->owned_orientations || !job->owned_parts
        || !job->owned_labels || !job->order || !job->placements) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        goto failed;
    }

    job->sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = job->sheet_width_cells,
        .height_cells = job->sheet_height_cells,
        .words = job->owned_sheet_words,
        .word_capacity = sheet_words
    });
    s_build_parts(job, &list);
    job->parts = job->owned_parts;
    job->labels = job->owned_labels;

    free(list.specs);
    result.job = job;
    return result;

failed:
    free(list.specs);
    bp_job_destroy(&job);
    return result;
}

/* Releases the job and clears the caller's pointer. */
void
bp_job_destroy(bp_job_t **job)
{
    BP_REQUIRE(job);
    if (!*job)
        return;

    free((*job)->owned_sheet_words);
    free((*job)->owned_row_offsets);
    free((*job)->owned_spans);
    free((*job)->owned_masks);
    free((*job)->owned_orientations);
    free((*job)->owned_parts);
    free((*job)->owned_labels);
    free((*job)->order);
    free((*job)->placements);
    free(*job);
    *job = NULL;
}
