#include "bp_job.h"

#include "bp_contract.h"
#include "bp_place.h"
#include "bp_polygon.h"
#include "bp_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_LINE_MAX 16384U
#define S_MAX_ANGLES 72U

/*
 * One part specification from the file. Geometry lives in the shared point
 * and ring pools; a rect line is expanded into a four-vertex polygon so there
 * is only one geometry path to get right.
 */
typedef struct {
    uint32_t id;
    uint32_t quantity;
    uint32_t first_point;
    uint32_t point_count;
    uint32_t first_ring;
    uint32_t ring_count;
    uint32_t angle_count;
    double angles[S_MAX_ANGLES];
} s_spec_t;

/* Growable arrays used only while loading. */
typedef struct {
    s_spec_t *specs;
    uint32_t spec_count;
    uint32_t spec_capacity;
    bp_point_t *points;
    uint32_t point_count;
    uint32_t point_capacity;
    uint32_t *rings;
    uint32_t ring_count;
    uint32_t ring_capacity;
} s_source_t;

/* Grows an array to hold at least `needed` elements. Returns 0 on failure. */
static int
s_reserve(void **items, uint32_t *capacity, uint32_t needed, size_t size)
{
    uint32_t grown = *capacity ? *capacity : 16U;
    void *block;

    if (needed <= *capacity)
        return 1;
    while (grown < needed)
        grown *= 2U;
    block = realloc(*items, (size_t) grown * size);
    if (!block)
        return 0;
    *items = block;
    *capacity = grown;
    return 1;
}

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

/*
 * Parses an angle field: either a comma-separated list of degrees, or
 * "every:<step>" for a full turn in equal steps. Returns 0 when malformed.
 */
static int
s_parse_angles(const char *text, double *angles, uint32_t *count)
{
    double step;
    int consumed = 0;

    *count = 0U;
    if (sscanf(text, "every:%lf%n", &step, &consumed) == 1) {
        double angle;

        if (!(step > 0.0) || step > 360.0)
            return 0;
        for (angle = 0.0; angle < 360.0 - 1e-9; angle += step) {
            if (*count >= S_MAX_ANGLES)
                return 0;
            angles[(*count)++] = angle;
        }
        return *count > 0U;
    }

    while (*text) {
        double angle;

        consumed = 0;
        if (sscanf(text, "%lf%n", &angle, &consumed) != 1)
            return 0;
        if (*count >= S_MAX_ANGLES)
            return 0;
        angles[(*count)++] = angle;
        text += consumed;
        if (*text == ',')
            ++text;
        else if (*text == '\0' || *text == ' ' || *text == '\t')
            break;
        else
            return 0;
    }
    return *count > 0U;
}

/* Appends a vertex to the shared point pool. */
static int
s_push_point(s_source_t *source, double x_mm, double y_mm)
{
    if (!s_reserve((void **) &source->points, &source->point_capacity,
        source->point_count + 1U, sizeof *source->points))
        return 0;
    source->points[source->point_count].x_mm = x_mm;
    source->points[source->point_count].y_mm = y_mm;
    ++source->point_count;
    return 1;
}

/* Appends a ring boundary, which is an index relative to the spec's points. */
static int
s_push_ring(s_source_t *source, uint32_t relative_index)
{
    if (!s_reserve((void **) &source->rings, &source->ring_capacity,
        source->ring_count + 1U, sizeof *source->rings))
        return 0;
    source->rings[source->ring_count] = relative_index;
    ++source->ring_count;
    return 1;
}

/* Starts a new specification and returns it, or NULL out of memory. */
static s_spec_t *
s_begin_spec(s_source_t *source, uint32_t id, uint32_t quantity)
{
    s_spec_t *spec;

    if (!s_reserve((void **) &source->specs, &source->spec_capacity,
        source->spec_count + 1U, sizeof *source->specs))
        return NULL;
    spec = &source->specs[source->spec_count];
    memset(spec, 0, sizeof *spec);
    spec->id = id;
    spec->quantity = quantity;
    spec->first_point = source->point_count;
    spec->first_ring = source->ring_count;
    if (!s_push_ring(source, 0U))
        return NULL;
    ++source->spec_count;
    return spec;
}

/* Parses the file into specifications and the sheet settings. */
static bp_job_status_t
s_parse(const char *path, double cell_size_override_mm, bp_job_t *job,
    s_source_t *source, unsigned *line_number)
{
    FILE *input = fopen(path, "r");
    char *line = NULL;
    int have_sheet = 0;
    uint32_t spec_index;
    bp_job_status_t status = BP_JOB_OK;

    *line_number = 0U;
    if (!input)
        return BP_JOB_CANNOT_OPEN;

    line = malloc(S_LINE_MAX);
    if (!line) {
        (void) fclose(input);
        return BP_JOB_OUT_OF_MEMORY;
    }

    job->cell_size_mm = 1.0;
    job->clearance_mm = 0.0;

    while (fgets(line, (int) S_LINE_MAX, input)) {
        char *comment = strchr(line, '#');
        char angle_text[256];
        double a;
        double b;
        unsigned id;
        unsigned quantity;
        unsigned turn;
        unsigned vertices;
        int consumed = 0;

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
            /* A rectangle is sugar for a four-vertex polygon. */
            s_spec_t *spec;

            if (quantity == 0U || a <= 0.0 || b <= 0.0 || turn > 1U)
                goto bad_line;
            spec = s_begin_spec(source, (uint32_t) id, (uint32_t) quantity);
            if (!spec)
                goto out_of_memory;
            if (!s_push_point(source, 0.0, 0.0)
                || !s_push_point(source, a, 0.0)
                || !s_push_point(source, a, b)
                || !s_push_point(source, 0.0, b)
                || !s_push_ring(source, 4U))
                goto out_of_memory;
            spec->point_count = 4U;
            spec->ring_count = 1U;
            spec->angle_count = turn ? 2U : 1U;
            spec->angles[0] = 0.0;
            spec->angles[1] = 90.0;
        } else if (sscanf(line, " poly %u %u %255s", &id, &quantity,
            angle_text) == 3) {
            s_spec_t *spec;

            if (quantity == 0U)
                goto bad_line;
            spec = s_begin_spec(source, (uint32_t) id, (uint32_t) quantity);
            if (!spec)
                goto out_of_memory;
            if (!s_parse_angles(angle_text, spec->angles, &spec->angle_count))
                goto bad_line;
        } else if (sscanf(line, " ring %u%n", &vertices, &consumed) == 1) {
            s_spec_t *spec;
            const char *cursor = line + consumed;
            unsigned index;

            if (source->spec_count == 0U || vertices < 3U)
                goto bad_line;
            spec = &source->specs[source->spec_count - 1U];
            for (index = 0U; index < vertices; ++index) {
                int used = 0;

                if (sscanf(cursor, " %lf %lf%n", &a, &b, &used) != 2)
                    goto bad_line;
                cursor += used;
                if (!s_push_point(source, a, b))
                    goto out_of_memory;
            }
            spec->point_count += vertices;
            ++spec->ring_count;
            if (!s_push_ring(source, spec->point_count))
                goto out_of_memory;
        } else if (strspn(line, " \t\r\n") != strlen(line)) {
            goto bad_line;
        }
    }
    free(line);
    (void) fclose(input);

    /* Past this point the failures are about the file as a whole. */
    *line_number = 0U;
    if (cell_size_override_mm > 0.0)
        job->cell_size_mm = cell_size_override_mm;
    if (!have_sheet)
        return BP_JOB_NO_SHEET;
    if (source->spec_count == 0U)
        return BP_JOB_NO_PARTS;
    /* A poly line with no ring lines after it has no geometry. */
    for (spec_index = 0U; spec_index < source->spec_count; ++spec_index)
        if (source->specs[spec_index].ring_count == 0U)
            return BP_JOB_NO_PARTS;
    return BP_JOB_OK;

bad_line:
    status = BP_JOB_BAD_LINE;
    goto done;
out_of_memory:
    status = BP_JOB_OUT_OF_MEMORY;
done:
    free(line);
    (void) fclose(input);
    return status;
}

/* Returns the polygon view of one specification over the shared pools. */
static bp_polygon_t
s_polygon(const s_source_t *source, const s_spec_t *spec)
{
    bp_polygon_t polygon;

    polygon.points = source->points + spec->first_point;
    polygon.ring_offsets = source->rings + spec->first_ring;
    polygon.ring_count = spec->ring_count;
    return polygon;
}

/*
 * Bounds the raster size of any rotation of any part. A shape turned by an
 * arbitrary angle never leaves the circle through its bounding box corners,
 * so the diagonal is a safe size for one reusable scratch bitmap.
 */
static uint32_t
s_scratch_side_cells(const s_source_t *source, const bp_job_t *job,
    int *too_large)
{
    uint32_t index;
    uint32_t side = 1U;

    for (index = 0U; index < source->spec_count; ++index) {
        bp_polygon_t polygon = s_polygon(source, &source->specs[index]);
        bp_box_t box = bp_polygon_bounds(&polygon);
        double width = box.max_x_mm - box.min_x_mm;
        double height = box.max_y_mm - box.min_y_mm;
        uint32_t needed = s_cells_up(sqrt(width * width + height * height),
            job->cell_size_mm, too_large) + 2U * job->clearance_cells + 2U;

        if (needed > side)
            side = needed;
    }
    return side;
}

/* Everything the build pass needs, so the argument lists stay short. */
typedef struct {
    bp_bitmap_t solid;
    bp_bitmap_t grown;
    uint64_t *solid_words;
    uint64_t *grown_words;
    uint64_t *dilate_scratch;
    size_t dilate_capacity;
    bp_point_t *rotated;
    uint32_t rotated_capacity;
    double *crossings;
    size_t crossing_capacity;
    uint32_t side_cells;
} s_workspace_t;

/* Releases a workspace; safe on a partly built one. */
static void
s_workspace_free(s_workspace_t *workspace)
{
    free(workspace->solid_words);
    free(workspace->grown_words);
    free(workspace->dilate_scratch);
    free(workspace->rotated);
    free(workspace->crossings);
    memset(workspace, 0, sizeof *workspace);
}

/* Allocates scratch large enough for every orientation of every part. */
static int
s_workspace_init(s_workspace_t *workspace, const s_source_t *source,
    const bp_job_t *job, int *too_large)
{
    uint32_t index;
    size_t words;

    memset(workspace, 0, sizeof *workspace);
    workspace->side_cells = s_scratch_side_cells(source, job, too_large);
    if (*too_large)
        return 1;

    for (index = 0U; index < source->spec_count; ++index) {
        if (source->specs[index].point_count > workspace->rotated_capacity)
            workspace->rotated_capacity = source->specs[index].point_count;
    }
    workspace->crossing_capacity = workspace->rotated_capacity;
    words = bp_bitmap_required_words(workspace->side_cells,
        workspace->side_cells);
    workspace->dilate_capacity = bp_raster_dilate_scratch_words(
        job->clearance_cells, workspace->side_cells);

    workspace->solid_words = calloc(words, sizeof *workspace->solid_words);
    workspace->grown_words = calloc(words, sizeof *workspace->grown_words);
    workspace->dilate_scratch = calloc(workspace->dilate_capacity,
        sizeof *workspace->dilate_scratch);
    workspace->rotated = calloc(workspace->rotated_capacity,
        sizeof *workspace->rotated);
    workspace->crossings = calloc(workspace->crossing_capacity,
        sizeof *workspace->crossings);

    return workspace->solid_words && workspace->grown_words
        && workspace->dilate_scratch && workspace->rotated
        && workspace->crossings;
}

/* Where one mask's data starts in the pools, so pointers can be fixed later. */
typedef struct {
    uint32_t row_offset_index;
    uint32_t span_index;
} s_mask_slot_t;

/* Pools that grow while masks are built, then become the job's storage. */
typedef struct {
    uint32_t *row_offsets;
    uint32_t row_offset_count;
    uint32_t row_offset_capacity;
    bp_span_t *spans;
    uint32_t span_count;
    uint32_t span_capacity;
    bp_mask_t *masks;
    s_mask_slot_t *slots;
    uint32_t mask_count;
    bp_orientation_t *orientations;
    uint32_t orientation_count;
} s_build_t;

/*
 * Converts one rasterised bitmap into a mask appended to the pools. Capacity
 * is reserved before the conversion writes, and the pointers are corrected
 * once every pool has stopped moving.
 */
static int
s_append_mask(s_build_t *build, const bp_bitmap_t *source)
{
    uint32_t spans = bp_mask_count_spans(source);
    uint32_t offsets = source->height_cells + 1U;
    s_mask_slot_t slot;

    if (!s_reserve((void **) &build->row_offsets, &build->row_offset_capacity,
        build->row_offset_count + offsets, sizeof *build->row_offsets))
        return 0;
    if (!s_reserve((void **) &build->spans, &build->span_capacity,
        build->span_count + spans, sizeof *build->spans))
        return 0;

    slot.row_offset_index = build->row_offset_count;
    slot.span_index = build->span_count;
    build->masks[build->mask_count] = bp_mask_from_bitmap(
        (bp_mask_from_bitmap_args_t) {
            .source = source,
            .row_offsets = build->row_offsets + slot.row_offset_index,
            .row_offset_capacity = offsets,
            .spans = build->spans + slot.span_index,
            .span_capacity = spans
        });
    build->slots[build->mask_count] = slot;
    ++build->mask_count;
    build->row_offset_count += offsets;
    build->span_count += spans;
    return 1;
}

/*
 * Reports whether two already-built masks describe the same footprint.
 *
 * The comparison goes through the slot indices rather than through the masks'
 * own pointers, because the pools still move under them: appending a mask can
 * reallocate, and the stored pointers are only corrected once every pool has
 * settled.
 */
static int
s_masks_equal(const s_build_t *build, uint32_t first, uint32_t second)
{
    const bp_mask_t *a = &build->masks[first];
    const bp_mask_t *b = &build->masks[second];
    const uint32_t *a_offsets = build->row_offsets
        + build->slots[first].row_offset_index;
    const uint32_t *b_offsets = build->row_offsets
        + build->slots[second].row_offset_index;
    const bp_span_t *a_spans = build->spans + build->slots[first].span_index;
    const bp_span_t *b_spans = build->spans + build->slots[second].span_index;
    uint32_t index;

    if (a->width_cells != b->width_cells || a->height_cells != b->height_cells
        || a->span_count != b->span_count || a->area_cells != b->area_cells)
        return 0;
    for (index = 0U; index <= a->height_cells; ++index)
        if (a_offsets[index] != b_offsets[index])
            return 0;
    for (index = 0U; index < a->span_count; ++index)
        if (a_spans[index].x_cells != b_spans[index].x_cells
            || a_spans[index].length_cells != b_spans[index].length_cells)
            return 0;
    return 1;
}

/*
 * Rasterises every orientation of every specification.
 *
 * Symmetric parts produce identical masks at different angles, and a square
 * offered at four quarter turns would make the search do the same work four
 * times, so duplicates are dropped here rather than tolerated in the hot loop.
 */
static int
s_build_masks(bp_job_t *job, const s_source_t *source, s_build_t *build,
    s_workspace_t *workspace)
{
    uint32_t index;
    uint32_t instance = 0U;

    for (index = 0U; index < source->spec_count; ++index) {
        const s_spec_t *spec = &source->specs[index];
        bp_polygon_t polygon = s_polygon(source, spec);
        bp_polygon_t turned = polygon;
        uint32_t first_orientation = build->orientation_count;
        uint32_t angle;
        uint32_t copy;

        bp_polygon_require_simple(&polygon);
        turned.points = workspace->rotated;

        for (angle = 0U; angle < spec->angle_count; ++angle) {
            bp_raster_extent_t extent;
            uint32_t grown_width;
            uint32_t grown_height;
            uint32_t existing;
            uint32_t solid_index;
            int duplicate = 0;

            bp_polygon_rotate(workspace->rotated,
                workspace->rotated_capacity, &polygon, spec->angles[angle]);
            extent = bp_raster_extent(&turned, job->cell_size_mm);
            grown_width = extent.width_cells + 2U * job->clearance_cells;
            grown_height = extent.height_cells + 2U * job->clearance_cells;
            BP_REQUIRE(grown_width <= workspace->side_cells);
            BP_REQUIRE(grown_height <= workspace->side_cells);

            workspace->solid = bp_bitmap_init((bp_bitmap_init_args_t) {
                .width_cells = extent.width_cells,
                .height_cells = extent.height_cells,
                .words = workspace->solid_words,
                .word_capacity = bp_bitmap_required_words(extent.width_cells,
                    extent.height_cells)
            });
            bp_raster_fill((bp_raster_fill_args_t) {
                .target = &workspace->solid,
                .polygon = &turned,
                .cell_size_mm = job->cell_size_mm,
                .origin_mm = extent.origin_mm,
                .scratch_crossings = workspace->crossings,
                .scratch_capacity = workspace->crossing_capacity
            });

            workspace->grown = bp_bitmap_init((bp_bitmap_init_args_t) {
                .width_cells = grown_width,
                .height_cells = grown_height,
                .words = workspace->grown_words,
                .word_capacity = bp_bitmap_required_words(grown_width,
                    grown_height)
            });
            bp_raster_dilate(&workspace->grown, &workspace->solid,
                job->clearance_cells, workspace->dilate_scratch,
                workspace->dilate_capacity);

            solid_index = build->mask_count;
            if (!s_append_mask(build, &workspace->solid))
                return 0;
            for (existing = first_orientation;
                existing < build->orientation_count; ++existing) {
                uint32_t other = (uint32_t) (build->orientations[existing].solid
                    - build->masks);

                if (s_masks_equal(build, solid_index, other)) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                /* Roll the pools back over the mask just appended. */
                --build->mask_count;
                build->row_offset_count = build->slots[solid_index]
                    .row_offset_index;
                build->span_count = build->slots[solid_index].span_index;
                continue;
            }
            if (!s_append_mask(build, &workspace->grown))
                return 0;

            build->orientations[build->orientation_count].solid =
                &build->masks[solid_index];
            build->orientations[build->orientation_count].collision =
                &build->masks[solid_index + 1U];
            build->orientations[build->orientation_count]
                .solid_offset_x_cells = job->clearance_cells;
            build->orientations[build->orientation_count]
                .solid_offset_y_cells = job->clearance_cells;
            ++build->orientation_count;
        }

        BP_REQUIRE(build->orientation_count > first_orientation);
        for (copy = 0U; copy < spec->quantity; ++copy) {
            job->owned_parts[instance].id = instance + 1U;
            job->owned_parts[instance].orientations =
                &build->orientations[first_orientation];
            job->owned_parts[instance].orientation_count =
                build->orientation_count - first_orientation;
            job->owned_labels[instance] = spec->id;
            ++instance;
        }
    }
    BP_REQUIRE(instance == job->part_count);
    return 1;
}

/* Releases the loader's temporary source pools. */
static void
s_source_free(s_source_t *source)
{
    free(source->specs);
    free(source->points);
    free(source->rings);
    memset(source, 0, sizeof *source);
}

/* Reads a job file and builds everything a solve needs. */
bp_job_load_result_t
bp_job_load(const char *path, double cell_size_override_mm)
{
    bp_job_load_result_t result = { BP_JOB_OK, 0U, NULL };
    s_source_t source;
    s_build_t build;
    s_workspace_t workspace;
    bp_job_t *job;
    int too_large = 0;
    size_t sheet_words;
    uint32_t index;
    uint32_t mask_bound = 0U;
    uint32_t orientation_bound = 0U;
    uint32_t instances = 0U;

    BP_REQUIRE(path);
    BP_REQUIRE(cell_size_override_mm >= 0.0);
    memset(&source, 0, sizeof source);
    memset(&build, 0, sizeof build);
    memset(&workspace, 0, sizeof workspace);

    job = calloc(1U, sizeof *job);
    if (!job) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        return result;
    }

    result.status = s_parse(path, cell_size_override_mm, job, &source,
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
    if (too_large) {
        result.status = BP_JOB_TOO_LARGE;
        goto failed;
    }
    if (job->sheet_width_cells == 0U || job->sheet_height_cells == 0U) {
        result.status = BP_JOB_SHEET_TOO_SMALL;
        goto failed;
    }

    for (index = 0U; index < source.spec_count; ++index) {
        orientation_bound += source.specs[index].angle_count;
        mask_bound += 2U * source.specs[index].angle_count;
        instances += source.specs[index].quantity;
    }
    job->part_count = instances;

    if (!s_workspace_init(&workspace, &source, job, &too_large)) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        goto failed;
    }
    if (too_large) {
        result.status = BP_JOB_TOO_LARGE;
        goto failed;
    }

    sheet_words = bp_bitmap_required_words(job->sheet_width_cells,
        job->sheet_height_cells);
    job->owned_sheet_words = calloc(sheet_words,
        sizeof *job->owned_sheet_words);
    job->owned_parts = calloc(instances, sizeof *job->owned_parts);
    job->owned_labels = calloc(instances, sizeof *job->owned_labels);
    job->order = calloc(instances, sizeof *job->order);
    job->placed = calloc(instances, sizeof *job->placed);
    job->scratch_placed = calloc(instances, sizeof *job->scratch_placed);
    job->scratch_best_placed = calloc(instances,
        sizeof *job->scratch_best_placed);
    job->scratch_best = calloc(instances, sizeof *job->scratch_best);
    job->placements = calloc(instances, sizeof *job->placements);
    build.masks = calloc(mask_bound, sizeof *build.masks);
    build.slots = calloc(mask_bound, sizeof *build.slots);
    build.orientations = calloc(orientation_bound, sizeof *build.orientations);

    if (!job->owned_sheet_words || !job->owned_parts || !job->owned_labels
        || !job->order || !job->placements || !job->placed
        || !job->scratch_placed || !job->scratch_best_placed
        || !job->scratch_best || !build.masks || !build.slots
        || !build.orientations) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        goto failed;
    }

    job->sheet = bp_bitmap_init((bp_bitmap_init_args_t) {
        .width_cells = job->sheet_width_cells,
        .height_cells = job->sheet_height_cells,
        .words = job->owned_sheet_words,
        .word_capacity = sheet_words
    });

    job->scratch_row_capacity = bp_place_scratch_words(&job->sheet);
    job->scratch_rows = calloc(job->scratch_row_capacity,
        sizeof *job->scratch_rows);
    if (!job->scratch_rows) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        goto failed;
    }

    if (!s_build_masks(job, &source, &build, &workspace)) {
        result.status = BP_JOB_OUT_OF_MEMORY;
        goto failed;
    }

    /* The pools have stopped moving, so the mask pointers can be settled. */
    for (index = 0U; index < build.mask_count; ++index) {
        build.masks[index].row_offsets = build.row_offsets
            + build.slots[index].row_offset_index;
        build.masks[index].spans = build.spans + build.slots[index].span_index;
    }

    job->owned_row_offsets = build.row_offsets;
    job->owned_spans = build.spans;
    job->owned_masks = build.masks;
    job->owned_orientations = build.orientations;
    job->parts = job->owned_parts;
    job->labels = job->owned_labels;

    free(build.slots);
    s_source_free(&source);
    s_workspace_free(&workspace);
    result.job = job;
    return result;

failed:
    free(build.row_offsets);
    free(build.spans);
    free(build.masks);
    free(build.slots);
    free(build.orientations);
    s_source_free(&source);
    s_workspace_free(&workspace);
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
    free((*job)->placed);
    free((*job)->scratch_placed);
    free((*job)->scratch_best_placed);
    free((*job)->scratch_best);
    free((*job)->scratch_rows);
    free((*job)->placements);
    free(*job);
    *job = NULL;
}
