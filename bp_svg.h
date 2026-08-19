#pragma once

#include "bp_nester.h"

#include <stdint.h>
#include <stdio.h>

typedef enum {
    BP_SVG_OK = 0,
    BP_SVG_WRITE_FAILED = 1
} bp_svg_status_t;

typedef struct {
    bp_svg_status_t status;
} bp_svg_result_t;

typedef struct {
    FILE *out;
    uint32_t sheet_width_cells;
    uint32_t sheet_height_cells;
    double cell_size_mm;
    const bp_part_t *parts;
    uint32_t part_count;
    const bp_placement_t *placements;
    uint32_t placement_count;
    /* Optional label per part index; part IDs are used when this is NULL. */
    const uint32_t *labels;
    int draw_labels;
} bp_svg_write_args_t;

/*
 * Writes the layout as an SVG in millimetres, with y increasing upwards as it
 * does everywhere else in this project. This exists so the packing can be
 * inspected long before there is a GUI; it is a viewer, not an exchange
 * format, and the cut geometry belongs in DXF later.
 *
 * Solid geometry is drawn as its row spans, merged vertically so a rectangle
 * comes out as one <rect>. Clearance is not drawn: what you see is what gets
 * cut. A failed write is an expected runtime failure and is reported rather
 * than aborted, because the destination is outside this program's control.
 */
bp_svg_result_t
bp_svg_write(bp_svg_write_args_t args);
