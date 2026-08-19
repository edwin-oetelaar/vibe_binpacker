/*
 * Repeatable benchmark. Runs one job at several cell sizes and with both
 * placement scans, and prints CSV. Timings are CPU seconds on the host that
 * ran them, so comparisons are only meaningful within one report.
 */
#include "bp_bitmap.h"
#include "bp_job.h"
#include "bp_nester.h"
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
        /*
         * The reference scan is quadratic enough that the fine grids would
         * dominate the whole benchmark; one coarse point is enough to show
         * the ratio.
         */
        if (s_cell_sizes_mm[index] >= 1.0
            && !s_run(path, s_cell_sizes_mm[index], BP_PLACE_REFERENCE,
                "reference"))
            return 1;
    }
    return 0;
}
