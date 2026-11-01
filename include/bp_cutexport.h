#pragma once

#include "bp_cutplan.h"

#include <stdint.h>
#include <stdio.h>

/*
 * Writes a guillotine cutting plan for the people who saw it: the cut list as
 * CSV, in the format of docs/cutlist_format.md, and one SVG per plate with
 * the saw lines drawn by stage.
 *
 * Lengths are written from the integer 0.01 mm units, never through floating
 * point, so the file says exactly what the plan says. The plan must pass
 * bp_cutplan_check: writing an invalid plan is a contract violation. A failed
 * write is an expected runtime failure, because the destination is outside
 * this program's control; opening and closing the file is the caller's.
 */

typedef enum {
    BP_CUTEXPORT_OK = 0,
    BP_CUTEXPORT_WRITE_FAILED = 1
} bp_cutexport_status_t;

typedef struct {
    bp_cutexport_status_t status;
} bp_cutexport_result_t;

typedef struct {
    FILE *out;
    const bp_cutplan_t *plan;
    /*
     * One label per item. Items with one label must be contiguous and of
     * identical size and turn, as bp_cutjob_build produces them; each label
     * becomes one part row with the run's length as its quantity.
     */
    const uint32_t *labels;
    /* Saw list scratch; capacity must cover bp_cutplan_cut_count. */
    bp_cut_t *scratch_cuts;
    uint32_t scratch_cut_capacity;
} bp_cutexport_csv_args_t;

/* Writes the complete cut list of every plate. */
bp_cutexport_result_t
bp_cutexport_csv(bp_cutexport_csv_args_t args);

typedef struct {
    FILE *out;
    const bp_cutplan_t *plan;
    /* The plate to draw; must exist. */
    uint32_t plate;
    const uint32_t *labels;
    bp_cut_t *scratch_cuts;
    uint32_t scratch_cut_capacity;
} bp_cutexport_svg_args_t;

/*
 * Writes one plate as an SVG in millimetres, y upwards as everywhere in this
 * project. Items are labelled rectangles, waste is hatched, and every cut is
 * drawn as the band its blade removes, coloured by stage; edge trims are
 * grey. Elements carry class and data attributes so a checker can read the
 * drawing back.
 */
bp_cutexport_result_t
bp_cutexport_svg(bp_cutexport_svg_args_t args);
