#include "bp_cutjob.h"

#include "bp_contract.h"

#include <math.h>

/*
 * Tolerance in 0.01 mm units for the conversion. 4.4 mm is 440.00000000000006
 * units in floating point; without it a kerf would round up to 4.41 mm.
 */
#define S_EPSILON 1e-6

/* Converts millimetres to 0.01 mm, rounding up. */
int
bp_cutjob_units_up(double millimetres, uint32_t *out)
{
    double units;

    BP_REQUIRE(isfinite(millimetres) && millimetres >= 0.0);
    BP_REQUIRE(out);
    units = ceil(millimetres * 100.0 - S_EPSILON);
    if (units > (double) UINT32_MAX)
        return 0;
    *out = (uint32_t) units;
    return 1;
}

/* Converts millimetres to 0.01 mm, rounding down. */
int
bp_cutjob_units_down(double millimetres, uint32_t *out)
{
    double units;

    BP_REQUIRE(isfinite(millimetres) && millimetres >= 0.0);
    BP_REQUIRE(out);
    units = floor(millimetres * 100.0 + S_EPSILON);
    if (units > (double) UINT32_MAX)
        return 0;
    *out = (uint32_t) units;
    return 1;
}

/*
 * Reports whether the label run starting at part repeats an earlier run.
 * Runs are the job's part lines; comparing run starts is quadratic in lines,
 * not in pieces.
 */
static int
s_label_repeats(const bp_job_t *job, uint32_t part)
{
    uint32_t earlier;

    BP_REQUIRE(job && part < job->part_count);
    for (earlier = 0U; earlier < part; ++earlier)
        if ((earlier == 0U || job->labels[earlier] != job->labels[earlier - 1U])
            && job->labels[earlier] == job->labels[part])
            return 1;
    return 0;
}

/* Fills settings and items from the job. */
bp_cutjob_result_t
bp_cutjob_build(bp_cutjob_args_t args)
{
    const bp_job_t *job = args.job;
    bp_cut_settings_t *settings = args.settings;
    bp_cutjob_result_t result = { BP_CUTJOB_OK, 0U };
    uint32_t part;
    int fits = 1;

    BP_REQUIRE(job && settings);
    BP_REQUIRE(job->labels && job->rect_width_mm && job->rect_height_mm
        && job->rect_may_turn);
    BP_REQUIRE(args.items || job->part_count == 0U);
    BP_REQUIRE(args.item_capacity >= job->part_count);
    BP_REQUIRE(job->max_stages >= 1U && job->max_stages <= BP_CUTPLAN_MAX_STAGES);

    if (!job->has_kerf) {
        result.status = BP_CUTJOB_NO_KERF;
        return result;
    }
    *settings = (bp_cut_settings_t) {
        .max_stages = job->max_stages,
        .first_cut = job->first_cut,
        .non_exact = job->sizing_cut
    };
    fits = bp_cutjob_units_down(job->sheet_width_mm, &settings->plate_width_10um)
        && bp_cutjob_units_down(job->sheet_height_mm,
            &settings->plate_height_10um)
        && bp_cutjob_units_up(job->trim_left_mm, &settings->trim_left_10um)
        && bp_cutjob_units_up(job->trim_right_mm, &settings->trim_right_10um)
        && bp_cutjob_units_up(job->trim_bottom_mm, &settings->trim_bottom_10um)
        && bp_cutjob_units_up(job->trim_top_mm, &settings->trim_top_10um)
        && bp_cutjob_units_up(job->kerf_mm, &settings->kerf_10um)
        && bp_cutjob_units_up(job->min_waste_mm, &settings->min_waste_10um);
    if (!fits) {
        result.status = BP_CUTJOB_TOO_LARGE;
        return result;
    }
    if ((uint64_t) settings->trim_left_10um + settings->trim_right_10um
            >= settings->plate_width_10um
        || (uint64_t) settings->trim_bottom_10um + settings->trim_top_10um
            >= settings->plate_height_10um) {
        result.status = BP_CUTJOB_NO_USABLE_AREA;
        return result;
    }

    for (part = 0U; part < job->part_count; ++part) {
        bp_cut_item_t *item = &args.items[part];

        result.part = part;
        if (!(job->rect_width_mm[part] > 0.0)) {
            result.status = BP_CUTJOB_NOT_RECTANGLE;
            return result;
        }
        /*
         * A label must name one part: a new run may not reuse an earlier
         * label, and a run may not change size or turn halfway, which is
         * what two adjacent lines with one label would do.
         */
        if (part == 0U || job->labels[part] != job->labels[part - 1U]) {
            if (s_label_repeats(job, part)) {
                result.status = BP_CUTJOB_DUPLICATE_LABEL;
                return result;
            }
        } else if (job->rect_width_mm[part] != job->rect_width_mm[part - 1U]
            || job->rect_height_mm[part] != job->rect_height_mm[part - 1U]
            || job->rect_may_turn[part] != job->rect_may_turn[part - 1U]) {
            result.status = BP_CUTJOB_DUPLICATE_LABEL;
            return result;
        }
        if (!bp_cutjob_units_up(job->rect_width_mm[part], &item->width_10um)
            || !bp_cutjob_units_up(job->rect_height_mm[part],
                &item->height_10um)) {
            result.status = BP_CUTJOB_TOO_LARGE;
            return result;
        }
        item->may_rotate = job->rect_may_turn[part];
    }
    result.part = 0U;
    return result;
}

/* Returns a short human-readable form of a status. */
const char *
bp_cutjob_status_text(bp_cutjob_status_t status)
{
    switch (status) {
    case BP_CUTJOB_OK:
        return "ok";
    case BP_CUTJOB_NO_KERF:
        return "job has no kerf line; sawing needs the blade width";
    case BP_CUTJOB_NOT_RECTANGLE:
        return "part is not a rect line; a saw cuts rectangles";
    case BP_CUTJOB_DUPLICATE_LABEL:
        return "two part lines share a label";
    case BP_CUTJOB_NO_USABLE_AREA:
        return "edge trims leave no usable plate";
    case BP_CUTJOB_TOO_LARGE:
        return "a length exceeds 42.9 km";
    default:
        return "unknown status";
    }
}
