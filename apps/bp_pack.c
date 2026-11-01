/*
 * Command line front end: reads a job description, packs it with
 * bottom-left-fill and reports the result, optionally as SVG.
 */
#include "bp_bitmap.h"
#include "bp_contract.h"
#include "bp_job.h"
#include "bp_multisheet.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_search.h"
#include "bp_sheet.h"
#include "bp_svg.h"

#include <math.h>
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
        "  --mode reference|jump|erosion   placement search (jump)\n"
        "  --cell <mm>                 override the job's cell size\n"
        "  --sheet-size <w_mm> <h_mm>  override the job's sheet size; width\n"
        "                              and height are taken as given\n"
        "  --sheets <count>            identical plates; minimise count, then\n"
        "                              last-sheet bounding-box area\n"
        "  --search <iterations>       search rounds (multi: order/width trials)\n"
        "  --time <seconds>            stop the search after this long\n"
        "  --contact <w>               how many cells of height a perfect\n"
        "                              fit is worth (default 0)\n"
        "  --window <rows>             heights ranked above the lowest (1)\n"
        "  --ruin <parts>              most parts lifted per round (8)\n"
        "  --blink <rate>              chance a position is passed over "
        "(0.08)\n"
        "  --compact <fraction>        share of the budget spent squeezing "
        "(0.4)\n"
        "  --svg <path>                write the layout as SVG\n"
        "                              multi: <stem>-001.svg, -002.svg, ...\n"
        "  --csv                       CSV summary (multi: one row per sheet)\n"
        "  contact/window/ruin/blink/compact apply to single-sheet search only\n");
    exit(1);
}

/*
 * Parses one millimetre value of an override. Only the syntax is checked here:
 * whether the value is usable is the loader's call, so the CLI and the GUI
 * refuse exactly the same sizes with the same message.
 */
static double
s_parse_mm(const char *text)
{
    char *end;
    double value;

    BP_REQUIRE(text);
    value = strtod(text, &end);
    if (end == text || *end != '\0')
        s_usage();
    return value;
}

/*
 * Reports a failed load. A refused sheet gets the shared explanation with
 * its figures and a cell size suggestion, rather than the bare status.
 */
static void
s_report_load_failure(const char *job_path, const bp_job_load_result_t *loaded)
{
    char explanation[BP_SHEET_EXPLAIN_CAPACITY];

    BP_REQUIRE(job_path && loaded);
    if (loaded->line_number > 0U) {
        (void) fprintf(stderr, "bp_pack: %s:%u: %s\n", job_path,
            loaded->line_number, bp_job_status_text(loaded->status));
        return;
    }
    if (loaded->sheet_plan.status != BP_SHEET_OK) {
        bp_sheet_explain(&loaded->sheet_plan, bp_sheet_default_limits(),
            explanation, sizeof explanation);
        (void) fprintf(stderr, "bp_pack: %s: %s: %s\n", job_path,
            bp_job_status_text(loaded->status), explanation);
        return;
    }
    (void) fprintf(stderr, "bp_pack: %s: %s\n", job_path,
        bp_job_status_text(loaded->status));
}

/* Options shared by the multi-sheet solve, report and per-sheet SVG export. */
typedef struct {
    bp_job_t *job;
    uint32_t max_sheets;
    bp_nest_sort_t sort;
    bp_place_mode_t mode;
    uint32_t iterations;
    double time_budget_seconds;
    const char *svg_path;
    int want_csv;
} s_multi_options_t;

/* Runs a complete multi-sheet job, preserving original part indices in exports. */
static int
s_run_multisheet(s_multi_options_t options)
{
    bp_job_t *job = options.job;
    uint32_t capacity;
    bp_sheet_layout_t *sheets;
    bp_sheet_layout_t *scratch;
    bp_multisheet_result_t result;
    char *svg_path = NULL;
    size_t stem_length = 0U;
    uint32_t index;
    int exit_code = 1;

    BP_REQUIRE(job);
    BP_REQUIRE(options.max_sheets > 0U);
    capacity = options.max_sheets < job->part_count
        ? options.max_sheets : job->part_count;
    sheets = calloc(capacity, sizeof *sheets);
    scratch = calloc(capacity, sizeof *scratch);
    if (!sheets || !scratch) {
        (void) fprintf(stderr, "bp_pack: out of memory for multi-sheet layout\n");
        goto done;
    }
    if (options.svg_path) {
        size_t length = strlen(options.svg_path);

        svg_path = malloc(length + 32U);
        if (!svg_path) {
            (void) fprintf(stderr, "bp_pack: out of memory for SVG path\n");
            goto done;
        }
        stem_length = length >= 4U && strcmp(options.svg_path + length - 4U, ".svg") == 0
            ? length - 4U : length;
        memcpy(svg_path, options.svg_path, stem_length);
    }
    result = bp_multisheet_solve((bp_multisheet_args_t) {
        .sheet = &job->sheet,
        .parts = job->parts,
        .part_count = job->part_count,
        .max_sheets = options.max_sheets,
        .sort = options.sort,
        .place_mode = options.mode,
        .iterations = options.iterations,
        .time_budget_seconds = options.time_budget_seconds,
        .seed = UINT64_C(0x5deece66d),
        .scratch_order = job->order,
        .scratch_best_order = job->scratch_weights,
        .scratch_assigned = job->scratch_placed,
        .scratch_placements = job->scratch_best,
        .scratch_sheets = scratch,
        .scratch_rows = job->scratch_rows,
        .scratch_row_capacity = job->scratch_row_capacity,
        .placements = job->placements,
        .sheets = sheets,
        .part_capacity = job->part_count,
        .sheet_capacity = capacity
    });
    if (options.want_csv) {
        (void) printf("sheet,placed,bbox_x_mm,bbox_y_mm,bbox_width_mm,"
            "bbox_height_mm,bbox_area_mm2,sheet_utilisation\n");
    } else {
        (void) printf("sheets       %u used, %u available (%.1f x %.1f mm each)\n",
            (unsigned) result.sheet_count, (unsigned) options.max_sheets,
            job->sheet_width_mm, job->sheet_height_mm);
        (void) printf("parts        %u requested, %u placed, %u unplaced (%u oversized)\n",
            (unsigned) job->part_count, (unsigned) result.placed_count,
            (unsigned) result.unplaced_count, (unsigned) result.oversized_count);
        (void) printf("search       %u trials, %u improvements, %.3f CPU s\n",
            (unsigned) result.iterations_run, (unsigned) result.improvements, result.seconds);
    }
    for (index = 0U; index < result.sheet_count; ++index) {
        const bp_sheet_layout_t *layout = &sheets[index];
        double cell = job->cell_size_mm;
        double area_mm2 = (double) layout->width_cells * layout->height_cells * cell * cell;
        double utilisation = (double) layout->placed_area_cells
            / ((double) job->sheet_width_cells * job->sheet_height_cells);

        if (options.want_csv) {
            (void) printf("%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.6f\n",
                (unsigned) index + 1U, (unsigned) layout->placement_count,
                layout->min_x_cells * cell, layout->min_y_cells * cell,
                layout->width_cells * cell, layout->height_cells * cell,
                area_mm2, utilisation);
        } else {
            (void) printf("sheet %-5u  %u parts, box %.1f x %.1f mm = %.1f mm2, %.2f %% used\n",
                (unsigned) index + 1U, (unsigned) layout->placement_count,
                layout->width_cells * cell, layout->height_cells * cell,
                area_mm2, utilisation * 100.0);
        }
        if (svg_path) {
            FILE *out;
            bp_svg_result_t svg;

            (void) snprintf(svg_path + stem_length, 32U, "-%03u.svg", (unsigned) index + 1U);
            out = fopen(svg_path, "w");
            if (!out) {
                (void) fprintf(stderr, "bp_pack: cannot write %s\n", svg_path);
                goto done;
            }
            svg = bp_svg_write((bp_svg_write_args_t) {
                .out = out,
                .sheet_width_cells = job->sheet_width_cells,
                .sheet_height_cells = job->sheet_height_cells,
                .cell_size_mm = cell,
                .parts = job->parts,
                .part_count = job->part_count,
                .placements = job->placements + layout->first_placement,
                .placement_count = layout->placement_count,
                .labels = job->labels,
                .draw_labels = 1
            });
            if (fclose(out) != 0 || svg.status != BP_SVG_OK) {
                (void) fprintf(stderr, "bp_pack: SVG write failed: %s\n", svg_path);
                goto done;
            }
        }
    }
    if (result.unplaced_count > 0U) {
        memset(job->placed, 0, job->part_count);
        for (index = 0U; index < result.placed_count; ++index)
            job->placed[job->placements[index].part_index] = 1U;
        for (index = 0U; index < job->part_count; ++index)
            if (!job->placed[index])
                (void) fprintf(stderr, "bp_pack: unplaced part %u (label %u)\n",
                    (unsigned) index + 1U, (unsigned) job->labels[index]);
    }
    exit_code = result.status == BP_MULTISHEET_COMPLETE ? 0 : 2;
done:
    free(svg_path);
    free(scratch);
    free(sheets);
    return exit_code;
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
    double sheet_width_mm = 0.0;
    double sheet_height_mm = 0.0;
    unsigned long search_iterations = 0U;
    double time_budget_seconds = 0.0;
    bp_score_weights_t weights = bp_score_default_weights();
    uint32_t height_window = 1U;
    uint32_t max_ruin = 8U;
    double blink_rate = 0.08;
    double compact_fraction = 0.4;
    int want_csv = 0;
    uint32_t max_sheets = 0U;
    int single_sheet_options = 0;
    int argument;
    bp_job_load_result_t loaded;
    bp_job_t *job;
    bp_nester_solve_result_t result = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint32_t placement_count = 0U;
    uint32_t iterations_run = 0U;
    uint32_t improvements = 0U;
    uint32_t squeezes = 0U;
    uint32_t squeezes_resolved = 0U;
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
            else if (strcmp(value, "erosion") == 0)
                mode = BP_PLACE_EROSION;
            else
                s_usage();
        } else if (strcmp(item, "--sheets") == 0 && argument + 1 < argc) {
            char *end;
            const char *text = argv[++argument];
            unsigned long value = strtoul(text, &end, 10);

            if (text[0] < '0' || text[0] > '9' || *end != '\0'
                || value == 0UL || value > UINT32_MAX)
                s_usage();
            max_sheets = (uint32_t) value;
        } else if (strcmp(item, "--search") == 0 && argument + 1 < argc) {
            search_iterations = (unsigned long) strtoul(argv[++argument],
                NULL, 10);
        } else if (strcmp(item, "--time") == 0 && argument + 1 < argc) {
            time_budget_seconds = strtod(argv[++argument], NULL);
            if (!isfinite(time_budget_seconds) || !(time_budget_seconds > 0.0))
                s_usage();
        } else if (strcmp(item, "--contact") == 0 && argument + 1 < argc) {
            single_sheet_options = 1;
            weights.contact_weight = strtod(argv[++argument], NULL);
            if (weights.contact_weight < 0.0)
                s_usage();
        } else if (strcmp(item, "--window") == 0 && argument + 1 < argc) {
            single_sheet_options = 1;
            height_window = (uint32_t) strtoul(argv[++argument], NULL, 10);
            if (height_window == 0U)
                s_usage();
        } else if (strcmp(item, "--ruin") == 0 && argument + 1 < argc) {
            single_sheet_options = 1;
            max_ruin = (uint32_t) strtoul(argv[++argument], NULL, 10);
            if (max_ruin == 0U)
                s_usage();
        } else if (strcmp(item, "--blink") == 0 && argument + 1 < argc) {
            single_sheet_options = 1;
            blink_rate = strtod(argv[++argument], NULL);
            if (blink_rate < 0.0 || blink_rate >= 1.0)
                s_usage();
        } else if (strcmp(item, "--compact") == 0 && argument + 1 < argc) {
            single_sheet_options = 1;
            compact_fraction = strtod(argv[++argument], NULL);
            if (compact_fraction < 0.0 || compact_fraction > 1.0)
                s_usage();
        } else if (strcmp(item, "--cell") == 0 && argument + 1 < argc) {
            cell_override_mm = strtod(argv[++argument], NULL);
            if (!(cell_override_mm > 0.0))
                s_usage();
        } else if (strcmp(item, "--sheet-size") == 0 && argument + 2 < argc) {
            sheet_width_mm = s_parse_mm(argv[++argument]);
            sheet_height_mm = s_parse_mm(argv[++argument]);
            /* Zero means "no override" to the loader, so refuse it here. */
            if (sheet_width_mm == 0.0 || sheet_height_mm == 0.0)
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
    if (search_iterations > UINT32_MAX || (max_sheets > 0U && single_sheet_options))
        s_usage();

    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = job_path,
        .sheet_width_mm = sheet_width_mm,
        .sheet_height_mm = sheet_height_mm,
        .cell_size_mm = cell_override_mm,
        .limits = bp_sheet_default_limits()
    });
    if (loaded.status != BP_JOB_OK) {
        s_report_load_failure(job_path, &loaded);
        return 1;
    }
    job = loaded.job;

    if (max_sheets > 0U) {
        exit_code = s_run_multisheet((s_multi_options_t) {
            .job = job,
            .max_sheets = max_sheets,
            .sort = sort,
            .mode = mode,
            .iterations = search_iterations > 0UL ? (uint32_t) search_iterations
                : time_budget_seconds > 0.0 ? UINT32_MAX : 0U,
            .time_budget_seconds = time_budget_seconds,
            .svg_path = svg_path,
            .want_csv = want_csv
        });
        bp_job_destroy(&job);
        return exit_code;
    }

    started = clock();
    if (search_iterations > 0U || time_budget_seconds > 0.0) {
        bp_search_result_t search = bp_search_solve((bp_search_args_t) {
            .sheet = &job->sheet,
            .parts = job->parts,
            .part_count = job->part_count,
            .weights = weights,
            .height_window = height_window,
            .max_positions = 48U,
            .max_ruin = max_ruin,
            .blink_rate = blink_rate,
            .iterations = search_iterations > 0U
                ? (uint32_t) search_iterations
                : UINT32_MAX,
            .time_budget_seconds = time_budget_seconds,
            .seed = UINT64_C(0x5deece66d),
            .scratch_order = job->order,
            .scratch_placed = job->scratch_placed,
            .scratch_best_placed = job->scratch_best_placed,
            .scratch_best = job->scratch_best,
            .scratch_capacity = job->part_count,
            .scratch_rows = job->scratch_rows,
            .scratch_row_capacity = job->scratch_row_capacity,
            .scratch_counts = compact_fraction > 0.0
                ? job->scratch_counts
                : NULL,
            .scratch_count_capacity = job->scratch_count_capacity,
            .scratch_prefix = job->scratch_prefix,
            .scratch_prefix_capacity = job->scratch_prefix_capacity,
            .scratch_weights = job->scratch_weights,
            .scratch_overlap = job->scratch_overlap,
            .compact_fraction = compact_fraction,
            .placements = job->placements,
            .placed = job->placed,
            .placement_capacity = job->part_count
        });
        uint32_t index;

        /* The SVG writer wants a packed list, not one entry per part. */
        placement_count = 0U;
        for (index = 0U; index < job->part_count; ++index)
            if (job->placed[index])
                job->placements[placement_count++] = job->placements[index];

        result.placed_count = search.placed_count;
        result.unplaced_count = search.unplaced_count;
        result.placed_area_cells = search.placed_area_cells;
        result.used_width_cells = search.used_width_cells;
        result.used_height_cells = search.used_height_cells;
        result.candidate_checks = search.iterations_run;
        iterations_run = search.iterations_run;
        improvements = search.improvements;
        squeezes = search.squeezes;
        squeezes_resolved = search.squeezes_resolved;
    } else {
        result = bp_nester_solve((bp_nester_solve_args_t) {
            .sheet = &job->sheet,
            .parts = job->parts,
            .part_count = job->part_count,
            .sort = sort,
            .place_mode = mode,
            .scratch_order = job->order,
            .scratch_capacity = job->part_count,
            .scratch_rows = job->scratch_rows,
            .scratch_row_capacity = job->scratch_row_capacity,
            .placements = job->placements,
            .placement_capacity = job->part_count
        });
        placement_count = result.placed_count;
    }
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
        if (iterations_run > 0U)
            (void) printf("search       %u rounds, %u improvements, "
                "%u/%u squeezes, %.3f s\n", (unsigned) iterations_run,
                (unsigned) improvements, (unsigned) squeezes_resolved,
                (unsigned) squeezes, seconds);
        else
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
            .placement_count = placement_count,
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
