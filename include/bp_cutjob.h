#pragma once

#include "bp_cutplan.h"
#include "bp_job.h"

#include <stdint.h>

/*
 * Turns a loaded job into what guillotine cutting works with: the saw's
 * settings and one bp_cut_item_t per piece, in 0.01 mm.
 *
 * Rounding stays conservative, as everywhere else: parts, blade, trims and
 * minimum waste round up, the plate rounds down. A job a saw cannot cut is an
 * expected result and is reported: no blade width, a part that is not a
 * rectangle, a label shared by two part lines, or no usable plate.
 */

typedef enum {
    BP_CUTJOB_OK = 0,
    /* The job has no kerf line; a blade width is never assumed. */
    BP_CUTJOB_NO_KERF = 1,
    /* A part is a poly line; a saw cuts rect lines only. */
    BP_CUTJOB_NOT_RECTANGLE = 2,
    /* Two part lines share a label, so a saw list could not tell them apart. */
    BP_CUTJOB_DUPLICATE_LABEL = 3,
    /* The plate, less its edge trims, has no area left. */
    BP_CUTJOB_NO_USABLE_AREA = 4,
    /* A length does not fit 32 bits of 0.01 mm, which is 42.9 km. */
    BP_CUTJOB_TOO_LARGE = 5
} bp_cutjob_status_t;

typedef struct {
    /* Any loaded job; a rectangles_only load is enough. */
    const bp_job_t *job;
    /* Written on success. */
    bp_cut_settings_t *settings;
    /* One per piece; capacity must cover the job's part_count. */
    bp_cut_item_t *items;
    uint32_t item_capacity;
} bp_cutjob_args_t;

/* The outcome, and the piece it concerns when that is one piece. */
typedef struct {
    bp_cutjob_status_t status;
    uint32_t part;
} bp_cutjob_result_t;

/*
 * Fills settings and items from the job. Items follow the job's part
 * indices, so pieces of one label are contiguous and identical, which is what
 * bp_cutexport relies on. On failure the outputs are unspecified.
 */
bp_cutjob_result_t
bp_cutjob_build(bp_cutjob_args_t args);

/* Returns a short human-readable form of a status, for error messages. */
const char *
bp_cutjob_status_text(bp_cutjob_status_t status);

/*
 * Converts millimetres to 0.01 mm, rounding up or down. The value must be
 * finite and not negative. Returns 0 and leaves out untouched when the result
 * does not fit 32 bits.
 */
int
bp_cutjob_units_up(double millimetres, uint32_t *out);

int
bp_cutjob_units_down(double millimetres, uint32_t *out);
