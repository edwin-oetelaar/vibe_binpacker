/*
 * Repeatable benchmark. Runs one job at several cell sizes and with both
 * placement scans, and prints CSV. Timings are CPU seconds on the host that
 * ran them, so comparisons are only meaningful within one report.
 */
#include "bp_bitmap.h"
#include "bp_job.h"
#include "bp_nester.h"
#include "bp_mask.h"
#include "bp_place.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Cell sizes to sweep, coarse to fine, to show how the scan cost scales. */
static const double s_cell_sizes_mm[] = { 2.0, 1.0, 0.5, 0.25 };

#define S_CELL_COUNT (sizeof s_cell_sizes_mm / sizeof s_cell_sizes_mm[0])

/* Runs one configuration and prints its CSV row. */
static int
s_run(const char *path, double cell_size_mm, bp_place_mode_t mode,
    const char *mode_name)
{
    bp_job_load_result_t loaded = bp_job_load(path, cell_size_mm);
    bp_job_t *job;
    bp_nester_solve_result_t result;
    clock_t started;
    double seconds;
    double sheet_area_cells;

    if (loaded.status != BP_JOB_OK) {
        (void) fprintf(stderr, "bp_bench: %s: %s\n", path,
            bp_job_status_text(loaded.status));
        return 0;
    }
    job = loaded.job;

    started = clock();
    result = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &job->sheet,
        .parts = job->parts,
        .part_count = job->part_count,
        .sort = BP_NEST_SORT_AREA_DESCENDING,
        .place_mode = mode,
        .scratch_order = job->order,
        .scratch_capacity = job->part_count,
        .scratch_rows = job->scratch_rows,
        .scratch_row_capacity = job->scratch_row_capacity,
        .placements = job->placements,
        .placement_capacity = job->part_count
    });
    seconds = (double) (clock() - started) / CLOCKS_PER_SEC;

    sheet_area_cells = (double) job->sheet_width_cells
        * job->sheet_height_cells;
    (void) printf("%.2f,%s,%u,%u,%u,%.4f,%llu,%.4f\n", cell_size_mm,
        mode_name, (unsigned) job->sheet_width_cells,
        (unsigned) job->part_count, (unsigned) result.placed_count,
        (double) result.placed_area_cells / sheet_area_cells,
        (unsigned long long) result.candidate_checks, seconds);

    bp_job_destroy(&job);
    return 1;
}

/*
 * Times the question the erosion actually answers: not "where is the first
 * free spot" but "where are all of them". A scoring layer needs the complete
 * set at each height, and that is the comparison worth reporting, because the
 * bottom-left sweep above says the opposite about first-fit.
 *
 * The sheet is filled with every part but the last, so the measurement runs
 * against a realistically cluttered sheet rather than an empty one.
 */
static int
s_run_feasible_sets(const char *path, double cell_size_mm)
{
    bp_job_load_result_t loaded = bp_job_load(path, cell_size_mm);
    bp_job_t *job;
    const bp_mask_t *mask;
    uint32_t max_x;
    uint32_t max_y;
    uint32_t y;
    uint64_t erosion_positions = 0U;
    uint64_t scan_positions = 0U;
    clock_t started;
    double erosion_seconds;
    double scan_seconds;

    if (loaded.status != BP_JOB_OK) {
        (void) fprintf(stderr, "bp_bench: %s: %s\n", path,
            bp_job_status_text(loaded.status));
        return 0;
    }
    job = loaded.job;
    if (job->part_count < 2U) {
        bp_job_destroy(&job);
        return 1;
    }

    (void) bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &job->sheet,
        .parts = job->parts,
        .part_count = job->part_count - 1U,
        .sort = BP_NEST_SORT_AREA_DESCENDING,
        .place_mode = BP_PLACE_JUMP,
        .scratch_order = job->order,
        .scratch_capacity = job->part_count,
        .scratch_rows = job->scratch_rows,
        .scratch_row_capacity = job->scratch_row_capacity,
        .placements = job->placements,
        .placement_capacity = job->part_count
    });

    mask = job->parts[job->part_count - 1U].orientations[0].collision;
    if (!bp_mask_in_bounds(&job->sheet, mask, 0U, 0U)) {
        bp_job_destroy(&job);
        return 1;
    }
    max_x = job->sheet.width_cells - mask->width_cells;
    max_y = job->sheet.height_cells - mask->height_cells;

    started = clock();
    for (y = 0U; y <= max_y; ++y)
        erosion_positions += bp_place_feasible_row(
            (bp_place_feasible_args_t) {
                .sheet = &job->sheet,
                .mask = mask,
                .y_cells = y,
                .scratch = job->scratch_rows,
                .scratch_capacity = job->scratch_row_capacity
            }).count;
    erosion_seconds = (double) (clock() - started) / CLOCKS_PER_SEC;

    started = clock();
    for (y = 0U; y <= max_y; ++y) {
        uint32_t x;

        for (x = 0U; x <= max_x; ++x)
            if (!bp_mask_overlaps(&job->sheet, mask, x, y))
                ++scan_positions;
    }
    scan_seconds = (double) (clock() - started) / CLOCKS_PER_SEC;

    /* Both must find the same set, or the timings mean nothing. */
    if (erosion_positions != scan_positions) {
        (void) fprintf(stderr, "bp_bench: feasible sets disagree: "
            "%llu against %llu\n", (unsigned long long) erosion_positions,
            (unsigned long long) scan_positions);
        bp_job_destroy(&job);
        return 0;
    }

    (void) printf("%.2f,erosion,%u,1,0,0.0000,%llu,%.4f\n", cell_size_mm,
        (unsigned) job->sheet.width_cells,
        (unsigned long long) erosion_positions, erosion_seconds);
    (void) printf("%.2f,full-scan,%u,1,0,0.0000,%llu,%.4f\n", cell_size_mm,
        (unsigned) job->sheet.width_cells,
        (unsigned long long) scan_positions, scan_seconds);

    bp_job_destroy(&job);
    return 1;
}

/* Sweeps cell size and placement mode over one job file. */
int
main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "jobs/demo.job";
    size_t index;

    (void) printf("cell_mm,mode,sheet_cells,parts,placed,utilisation,"
        "candidate_checks,seconds\n");
    for (index = 0U; index < S_CELL_COUNT; ++index) {
        if (!s_run(path, s_cell_sizes_mm[index], BP_PLACE_JUMP, "jump"))
            return 1;
        if (!s_run(path, s_cell_sizes_mm[index], BP_PLACE_EROSION, "erosion"))
            return 1;
        /*
         * The reference scan is slow enough that the fine grids would
         * dominate the whole benchmark; one coarse point is enough to show
         * the ratio.
         */
        if (s_cell_sizes_mm[index] >= 1.0
            && !s_run(path, s_cell_sizes_mm[index], BP_PLACE_REFERENCE,
                "reference"))
            return 1;
    }

    (void) printf("\nevery feasible position for one part at each height\n");
    (void) printf("cell_mm,method,sheet_cells,parts,placed,utilisation,"
        "positions,seconds\n");
    for (index = 0U; index < S_CELL_COUNT; ++index)
        if (s_cell_sizes_mm[index] >= 0.5
            && !s_run_feasible_sets(path, s_cell_sizes_mm[index]))
            return 1;
    return 0;
}
