#include "bp_svg.h"

#include "bp_contract.h"

/* Distinct enough to tell neighbouring parts apart on a white sheet. */
static const char *const s_palette[] = {
    "#4c78a8", "#f58518", "#54a24b", "#b279a2",
    "#e45756", "#72b7b2", "#eeca3b", "#9d755d"
};

#define S_PALETTE_SIZE (sizeof s_palette / sizeof s_palette[0])

/*
 * Emits one span as a rectangle, extended upwards over every following row
 * that repeats it. A span whose row below already repeats it was emitted
 * there, so each maximal run is drawn exactly once and a plain rectangle
 * collapses to a single element.
 */
static void
s_write_run(FILE *out, const bp_mask_t *mask, uint32_t row, uint32_t index,
    uint32_t origin_x_cells, uint32_t origin_y_cells,
    uint32_t sheet_height_cells, double cell_size_mm)
{
    bp_span_t span = mask->spans[index];
    uint32_t run;
    double x_mm;
    double y_mm;
    double width_mm;
    double height_mm;

    if (row > 0U && bp_mask_row_has_span(mask, row - 1U, span))
        return;
    run = bp_mask_run_height(mask, row, span);

    x_mm = (origin_x_cells + span.x_cells) * cell_size_mm;
    width_mm = span.length_cells * cell_size_mm;
    height_mm = run * cell_size_mm;
    /* SVG y grows downwards, so flip against the sheet height. */
    y_mm = (sheet_height_cells - (origin_y_cells + row + run)) * cell_size_mm;

    (void) fprintf(out,
        "    <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\"/>\n",
        x_mm, y_mm, width_mm, height_mm);
}

/* Writes the layout as an SVG in millimetres. */
bp_svg_result_t
bp_svg_write(bp_svg_write_args_t args)
{
    bp_svg_result_t result = { BP_SVG_OK };
    double sheet_width_mm;
    double sheet_height_mm;
    uint32_t placement;

    BP_REQUIRE(args.out);
    BP_REQUIRE(args.sheet_width_cells > 0U);
    BP_REQUIRE(args.sheet_height_cells > 0U);
    BP_REQUIRE(args.cell_size_mm > 0.0);
    BP_REQUIRE(args.parts);
    BP_REQUIRE(args.part_count > 0U);
    BP_REQUIRE(args.placements || args.placement_count == 0U);

    sheet_width_mm = args.sheet_width_cells * args.cell_size_mm;
    sheet_height_mm = args.sheet_height_cells * args.cell_size_mm;

    (void) fprintf(args.out,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "width=\"%.3fmm\" height=\"%.3fmm\" viewBox=\"0 0 %.3f %.3f\">\n",
        sheet_width_mm, sheet_height_mm, sheet_width_mm, sheet_height_mm);
    (void) fprintf(args.out,
        "  <rect x=\"0\" y=\"0\" width=\"%.3f\" height=\"%.3f\" "
        "fill=\"#f4f4f4\" stroke=\"#333333\" stroke-width=\"%.3f\"/>\n",
        sheet_width_mm, sheet_height_mm, sheet_width_mm / 600.0);

    for (placement = 0U; placement < args.placement_count; ++placement) {
        const bp_placement_t *spot = &args.placements[placement];
        const bp_part_t *part;
        const bp_orientation_t *option;
        const bp_mask_t *mask;
        uint32_t origin_x;
        uint32_t origin_y;
        uint32_t row;
        uint32_t label;
        double centre_x_mm;
        double centre_y_mm;
        double font_mm;

        BP_REQUIRE(spot->part_index < args.part_count);
        part = &args.parts[spot->part_index];
        BP_REQUIRE(spot->orientation < part->orientation_count);
        option = &part->orientations[spot->orientation];
        mask = option->solid;
        origin_x = spot->x_cells + option->solid_offset_x_cells;
        origin_y = spot->y_cells + option->solid_offset_y_cells;

        (void) fprintf(args.out,
            "  <g fill=\"%s\" fill-opacity=\"0.75\" stroke=\"#111111\" "
            "stroke-width=\"%.3f\">\n",
            s_palette[spot->part_index % S_PALETTE_SIZE],
            args.cell_size_mm * 0.5);
        for (row = 0U; row < mask->height_cells; ++row) {
            uint32_t index;

            for (index = mask->row_offsets[row];
                index < mask->row_offsets[row + 1U]; ++index)
                s_write_run(args.out, mask, row, index, origin_x, origin_y,
                    args.sheet_height_cells, args.cell_size_mm);
        }
        (void) fprintf(args.out, "  </g>\n");

        if (!args.draw_labels)
            continue;

        label = args.labels ? args.labels[spot->part_index] : spot->part_id;
        centre_x_mm = (origin_x + mask->width_cells * 0.5)
            * args.cell_size_mm;
        centre_y_mm = sheet_height_mm
            - (origin_y + mask->height_cells * 0.5) * args.cell_size_mm;
        /* Sized to the part so the label stays inside it. */
        font_mm = (mask->width_cells < mask->height_cells
            ? mask->width_cells
            : mask->height_cells) * args.cell_size_mm * 0.35;
        if (font_mm > sheet_width_mm * 0.02)
            font_mm = sheet_width_mm * 0.02;

        (void) fprintf(args.out,
            "  <text x=\"%.3f\" y=\"%.3f\" font-size=\"%.3f\" "
            "font-family=\"sans-serif\" fill=\"#111111\" "
            "text-anchor=\"middle\" dominant-baseline=\"central\">%u</text>\n",
            centre_x_mm, centre_y_mm, font_mm, (unsigned) label);
    }

    (void) fprintf(args.out, "</svg>\n");

    /* One check at the end: a broken pipe or full disk is not a caller bug. */
    if (ferror(args.out))
        result.status = BP_SVG_WRITE_FAILED;
    return result;
}
