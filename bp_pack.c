/*
 * Command line front end: reads a job description, packs it with
 * bottom-left-fill and reports the result, optionally as SVG.
 */
#include "bp_bitmap.h"
#include "bp_job.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_svg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Prints usage and stops. */
static void
s_usage(void)
{
    (void) fprintf(stderr,
        "usage: bp_pack [options] <job-file>\n"
        "  --sort input|area|longest   part order (default: area)\n"
        "  --mode reference|jump       placement scan (default: jump)\n"
        "  --cell <mm>                 override the job's cell size\n"
        "  --svg <path>                write the layout as SVG\n"
        "  --csv                       one machine-readable summary line\n");
    exit(1);
}

/* Reads a job, packs it, reports the result and optionally writes an SVG. */
int
main(int argc, char **argv)
{
    bp_nest_sort_t sort = BP_NEST_SORT_AREA_DESCENDING;
    bp_place_mode_t mode = BP_PLACE_JUMP;
    const char *job_path = NULL;
    const char *svg_path = NULL;
    double cell_override_mm = 0.0;
    int want_csv = 0;
    int argument;
    bp_job_load_result_t loaded;
    bp_job_t *job;
    bp_nester_solve_result_t result;
    clock_t started;
    double seconds;
    double sheet_area_cells;
    int exit_code;

    for (argument = 1; argument < argc; ++argument) {
        const char *item = argv[argument];

        if (strcmp(item, "--sort") == 0 && argument + 1 < argc) {
            const char *value = argv[++argument];

            if (strcmp(value, "input") == 0)
                sort = BP_NEST_SORT_INPUT_ORDER;
            else if (strcmp(value, "area") == 0)
                sort = BP_NEST_SORT_AREA_DESCENDING;
            else if (strcmp(value, "longest") == 0)
                sort = BP_NEST_SORT_LONGEST_SIDE_DESCENDING;
            else
                s_usage();
        } else if (strcmp(item, "--mode") == 0 && argument + 1 < argc) {
            const char *value = argv[++argument];

            if (strcmp(value, "reference") == 0)
                mode = BP_PLACE_REFERENCE;
            else if (strcmp(value, "jump") == 0)
                mode = BP_PLACE_JUMP;
            else
                s_usage();
        } else if (strcmp(item, "--cell") == 0 && argument + 1 < argc) {
            cell_override_mm = strtod(argv[++argument], NULL);
            if (!(cell_override_mm > 0.0))
                s_usage();
        } else if (strcmp(item, "--svg") == 0 && argument + 1 < argc) {
            svg_path = argv[++argument];
        } else if (strcmp(item, "--csv") == 0) {
            want_csv = 1;
        } else if (item[0] == '-') {
            s_usage();
        } else if (!job_path) {
            job_path = item;
        } else {
            s_usage();
        }
    }
    if (!job_path)
        s_usage();

    loaded = bp_job_load(job_path, cell_override_mm);
    if (loaded.status != BP_JOB_OK) {
        if (loaded.line_number > 0U)
            (void) fprintf(stderr, "bp_pack: %s:%u: %s\n", job_path,
                loaded.line_number, bp_job_status_text(loaded.status));
        else
            (void) fprintf(stderr, "bp_pack: %s: %s\n", job_path,
                bp_job_status_text(loaded.status));
        return 1;
    }
    job = loaded.job;

    started = clock();
    result = bp_nester_solve((bp_nester_solve_args_t) {
        .sheet = &job->sheet,
        .parts = job->parts,
        .part_count = job->part_count,
        .sort = sort,
        .place_mode = mode,
        .scratch_order = job->order,
        .scratch_capacity = job->part_count,
        .placements = job->placements,
        .placement_capacity = job->part_count
    });
    seconds = (double) (clock() - started) / CLOCKS_PER_SEC;

    sheet_area_cells = (double) job->sheet_width_cells
        * job->sheet_height_cells;
    if (want_csv) {
        (void) printf(
            "parts,placed,unplaced,sheet_utilisation,used_width_mm,"
            "used_height_mm,candidate_checks,seconds\n");
        (void) printf("%u,%u,%u,%.4f,%.2f,%.2f,%llu,%.4f\n",
            (unsigned) job->part_count, (unsigned) result.placed_count,
            (unsigned) result.unplaced_count,
            (double) result.placed_area_cells / sheet_area_cells,
            result.used_width_cells * job->cell_size_mm,
            result.used_height_cells * job->cell_size_mm,
            (unsigned long long) result.candidate_checks, seconds);
    } else {
        (void) printf("sheet        %.1f x %.1f mm at %.3f mm/cell "
            "(%u x %u cells)\n",
            job->sheet_width_mm, job->sheet_height_mm, job->cell_size_mm,
            (unsigned) job->sheet_width_cells,
            (unsigned) job->sheet_height_cells);
        (void) printf("clearance    %.2f mm (%u cells)\n",
            job->clearance_mm, (unsigned) job->clearance_cells);
        (void) printf("parts        %u requested, %u placed, %u unplaced\n",
            (unsigned) job->part_count, (unsigned) result.placed_count,
            (unsigned) result.unplaced_count);
        (void) printf("utilisation  %.2f %% of the sheet\n",
            100.0 * (double) result.placed_area_cells / sheet_area_cells);
        (void) printf("used box     %.1f x %.1f mm\n",
            result.used_width_cells * job->cell_size_mm,
            result.used_height_cells * job->cell_size_mm);
        (void) printf("candidates   %llu positions tested in %.3f s\n",
            (unsigned long long) result.candidate_checks, seconds);
    }

    exit_code = result.unplaced_count == 0U ? 0 : 2;

    if (svg_path) {
        FILE *out = fopen(svg_path, "w");
        bp_svg_result_t svg;

        if (!out) {
            (void) fprintf(stderr, "bp_pack: cannot write %s\n", svg_path);
            bp_job_destroy(&job);
            return 1;
        }
        svg = bp_svg_write((bp_svg_write_args_t) {
            .out = out,
            .sheet_width_cells = job->sheet_width_cells,
            .sheet_height_cells = job->sheet_height_cells,
            .cell_size_mm = job->cell_size_mm,
            .parts = job->parts,
            .part_count = job->part_count,
            .placements = job->placements,
            .placement_count = result.placed_count,
            .labels = job->labels,
            .draw_labels = 1
        });
        if (fclose(out) != 0 || svg.status != BP_SVG_OK) {
            (void) fprintf(stderr, "bp_pack: SVG write failed: %s\n",
                svg_path);
            bp_job_destroy(&job);
            return 1;
        }
    }

    bp_job_destroy(&job);
    return exit_code;
}
