#include "bp_cutexport.h"

#include "bp_contract.h"

#include <stddef.h>

/* Distinct enough to tell neighbouring parts apart, matching bp_svg. */
static const char *const s_palette[] = {
    "#4c78a8", "#f58518", "#54a24b", "#b279a2",
    "#e45756", "#72b7b2", "#eeca3b", "#9d755d"
};

#define S_PALETTE_SIZE (sizeof s_palette / sizeof s_palette[0])

/* Trims grey, then one colour per stage; deeper stages share the last. */
static const char *const s_stage_colours[] = {
    "#7f7f7f", "#d62728", "#1f77b4", "#2ca02c", "#9467bd"
};

#define S_STAGE_COLOURS (sizeof s_stage_colours / sizeof s_stage_colours[0])

/* Formats 0.01 mm units as millimetres with two decimals, exactly. */
static void
s_mm(FILE *out, uint32_t units)
{
    BP_REQUIRE(out);
    (void) fprintf(out, "%lu.%02lu", (unsigned long) (units / 100U),
        (unsigned long) (units % 100U));
}

/* Writes ",<mm>" so rows read as one field after another. */
static void
s_field_mm(FILE *out, uint32_t units)
{
    BP_REQUIRE(out);
    (void) fputc(',', out);
    s_mm(out, units);
}

/* Returns the CSV word for an axis. */
static const char *
s_axis_name(bp_cut_axis_t axis)
{
    BP_REQUIRE(axis == BP_CUT_AXIS_X || axis == BP_CUT_AXIS_Y);
    return axis == BP_CUT_AXIS_X ? "x" : "y";
}

/* Fills the saw list scratch, which both writers draw from. */
static uint32_t
s_cuts(const bp_cutplan_t *plan, bp_cut_t *cuts, uint32_t capacity)
{
    BP_REQUIRE(plan);
    return bp_cutplan_cuts((bp_cutplan_cuts_args_t) {
        .plan = plan, .cuts = cuts, .cut_capacity = capacity });
}

/* Writes the settings block. */
static void
s_write_settings(FILE *out, const bp_cut_settings_t *settings)
{
    static const char *const first_cut[] = { "x", "y", "any" };
    const struct {
        const char *name;
        uint32_t units;
    } lengths[] = {
        { "plate_width_mm", settings->plate_width_10um },
        { "plate_height_mm", settings->plate_height_10um },
        { "trim_left_mm", settings->trim_left_10um },
        { "trim_right_mm", settings->trim_right_10um },
        { "trim_bottom_mm", settings->trim_bottom_10um },
        { "trim_top_mm", settings->trim_top_10um },
        { "kerf_mm", settings->kerf_10um },
        { "min_waste_mm", settings->min_waste_10um }
    };
    size_t index;

    BP_REQUIRE(out && settings);
    BP_REQUIRE((unsigned) settings->first_cut < 3U);
    (void) fprintf(out, "format,vibe_binpacker-cutlist,1\n");
    for (index = 0U; index < sizeof lengths / sizeof lengths[0]; ++index) {
        (void) fprintf(out, "setting,%s", lengths[index].name);
        s_field_mm(out, lengths[index].units);
        (void) fputc('\n', out);
    }
    (void) fprintf(out, "setting,max_stages,%lu\n",
        (unsigned long) settings->max_stages);
    (void) fprintf(out, "setting,first_cut,%s\n",
        first_cut[settings->first_cut]);
    (void) fprintf(out, "setting,non_exact,%u\n",
        (unsigned) settings->non_exact);
}

/*
 * Writes one part row per label run. Items of a run must be identical; that
 * a label never reappears later is the caller's contract, too costly to
 * re-prove here.
 */
static void
s_write_parts(FILE *out, const bp_cutplan_t *plan, const uint32_t *labels)
{
    uint32_t start = 0U;

    BP_REQUIRE(out && plan);
    BP_REQUIRE(labels || plan->item_count == 0U);
    while (start < plan->item_count) {
        const bp_cut_item_t *item = &plan->items[start];
        uint32_t end = start + 1U;

        while (end < plan->item_count && labels[end] == labels[start]) {
            BP_REQUIRE(plan->items[end].width_10um == item->width_10um);
            BP_REQUIRE(plan->items[end].height_10um == item->height_10um);
            BP_REQUIRE(plan->items[end].may_rotate == item->may_rotate);
            ++end;
        }
        (void) fprintf(out, "part,%lu,%lu", (unsigned long) labels[start],
            (unsigned long) (end - start));
        s_field_mm(out, item->width_10um);
        s_field_mm(out, item->height_10um);
        (void) fprintf(out, ",%u\n", item->may_rotate ? 1U : 0U);
        start = end;
    }
}

/* Writes the item rows of one tree, depth first. */
static void
s_write_items(FILE *out, const bp_cutplan_t *plan, const uint32_t *labels,
    uint32_t plate, uint32_t index, uint32_t depth)
{
    const bp_cut_node_t *node;
    uint32_t child;

    BP_REQUIRE(out && plan && index < plan->node_count);
    BP_REQUIRE(depth <= BP_CUTPLAN_MAX_STAGES + 1U);
    node = &plan->nodes[index];
    if (node->kind == BP_CUT_NODE_ITEM) {
        BP_REQUIRE(node->item_index < plan->item_count);
        (void) fprintf(out, "item,%lu,%lu", (unsigned long) plate,
            (unsigned long) labels[node->item_index]);
        s_field_mm(out, node->x_10um);
        s_field_mm(out, node->y_10um);
        s_field_mm(out, node->width_10um);
        s_field_mm(out, node->height_10um);
        (void) fprintf(out, ",%u\n", node->rotated ? 1U : 0U);
    } else if (node->kind == BP_CUT_NODE_PANEL) {
        for (child = 0U; child < node->child_count; ++child)
            s_write_items(out, plan, labels, plate, node->first_child + child,
                depth + 1U);
    }
}

/* Writes the complete cut list of every plate. */
bp_cutexport_result_t
bp_cutexport_csv(bp_cutexport_csv_args_t args)
{
    bp_cutexport_result_t result = { BP_CUTEXPORT_OK };
    const bp_cutplan_t *plan = args.plan;
    uint32_t count;
    uint32_t index;

    BP_REQUIRE(args.out && plan);
    BP_REQUIRE(args.scratch_cuts);
    count = s_cuts(plan, args.scratch_cuts, args.scratch_cut_capacity);
    s_write_settings(args.out, &plan->settings);
    s_write_parts(args.out, plan, args.labels);
    for (index = 0U; index < plan->plate_count; ++index)
        (void) fprintf(args.out, "plate,%lu,%s\n", (unsigned long) index,
            s_axis_name(plan->plates[index].first_axis));
    for (index = 0U; index < count; ++index) {
        const bp_cut_t *cut = &args.scratch_cuts[index];

        (void) fprintf(args.out, "cut,%lu,%lu,%s", (unsigned long) cut->plate,
            (unsigned long) cut->stage, s_axis_name(cut->axis));
        s_field_mm(args.out, cut->position_10um);
        s_field_mm(args.out, cut->kerf_10um);
        s_field_mm(args.out, cut->from_10um);
        s_field_mm(args.out, cut->to_10um);
        (void) fputc('\n', args.out);
    }
    for (index = 0U; index < plan->plate_count; ++index)
        s_write_items(args.out, plan, args.labels, index,
            plan->plates[index].root, 0U);
    /* Buffered output only fails for certain once it is flushed. */
    if (fflush(args.out) != 0 || ferror(args.out))
        result.status = BP_CUTEXPORT_WRITE_FAILED;
    return result;
}

/* Writes x, y, width and height attributes, with y flipped to point up. */
static void
s_rect_attributes(FILE *out, uint32_t plate_height, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    BP_REQUIRE(out);
    BP_REQUIRE((uint64_t) y + height <= plate_height);
    (void) fputs(" x=\"", out);
    s_mm(out, x);
    (void) fputs("\" y=\"", out);
    s_mm(out, plate_height - y - height);
    (void) fputs("\" width=\"", out);
    s_mm(out, width);
    (void) fputs("\" height=\"", out);
    s_mm(out, height);
    (void) fputc('"', out);
}

/* Draws the items and waste of one tree, depth first. */
static void
s_svg_nodes(FILE *out, const bp_cutplan_t *plan, const uint32_t *labels,
    uint32_t index, uint32_t depth)
{
    const bp_cut_node_t *node;
    uint32_t height = plan->settings.plate_height_10um;
    uint32_t child;

    BP_REQUIRE(out && plan && index < plan->node_count);
    BP_REQUIRE(depth <= BP_CUTPLAN_MAX_STAGES + 1U);
    node = &plan->nodes[index];
    if (node->kind == BP_CUT_NODE_PANEL) {
        for (child = 0U; child < node->child_count; ++child)
            s_svg_nodes(out, plan, labels, node->first_child + child,
                depth + 1U);
    } else if (node->kind == BP_CUT_NODE_WASTE) {
        (void) fputs("  <rect class=\"waste\"", out);
        s_rect_attributes(out, height, node->x_10um, node->y_10um,
            node->width_10um, node->height_10um);
        (void) fputs(" fill=\"url(#waste)\"/>\n", out);
    } else {
        uint32_t label = labels[node->item_index];
        uint32_t smaller = node->width_10um < node->height_10um
            ? node->width_10um : node->height_10um;

        (void) fprintf(out, "  <rect class=\"item\" data-label=\"%lu\" "
            "data-rotated=\"%u\"", (unsigned long) label,
            node->rotated ? 1U : 0U);
        s_rect_attributes(out, height, node->x_10um, node->y_10um,
            node->width_10um, node->height_10um);
        (void) fprintf(out, " fill=\"%s\" fill-opacity=\"0.8\"/>\n",
            s_palette[label % S_PALETTE_SIZE]);
        /* Centred, and small enough to stay inside the item. */
        (void) fprintf(out, "  <text x=\"%.2f\" y=\"%.2f\" font-size=\"%.2f\" "
            "text-anchor=\"middle\" dominant-baseline=\"middle\" "
            "font-family=\"sans-serif\">%lu</text>\n",
            (node->x_10um + node->width_10um / 2.0) / 100.0,
            (height - node->y_10um - node->height_10um / 2.0) / 100.0,
            smaller / 300.0, (unsigned long) label);
    }
}

/* Writes one plate as an SVG in millimetres. */
bp_cutexport_result_t
bp_cutexport_svg(bp_cutexport_svg_args_t args)
{
    bp_cutexport_result_t result = { BP_CUTEXPORT_OK };
    const bp_cutplan_t *plan = args.plan;
    const bp_cut_settings_t *settings;
    uint32_t count;
    uint32_t index;
    double line_mm;

    BP_REQUIRE(args.out && plan && args.scratch_cuts);
    BP_REQUIRE(args.plate < plan->plate_count);
    BP_REQUIRE(args.labels || plan->item_count == 0U);
    settings = &plan->settings;
    count = s_cuts(plan, args.scratch_cuts, args.scratch_cut_capacity);
    line_mm = settings->plate_width_10um / 100.0 / 800.0;

    (void) fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"", args.out);
    s_mm(args.out, settings->plate_width_10um);
    (void) fputs("mm\" height=\"", args.out);
    s_mm(args.out, settings->plate_height_10um);
    (void) fputs("mm\" viewBox=\"0 0 ", args.out);
    s_mm(args.out, settings->plate_width_10um);
    (void) fputc(' ', args.out);
    s_mm(args.out, settings->plate_height_10um);
    (void) fprintf(args.out, "\" data-plate=\"%lu\">\n",
        (unsigned long) args.plate);
    (void) fprintf(args.out, "  <defs><pattern id=\"waste\" "
        "patternUnits=\"userSpaceOnUse\" width=\"%.2f\" height=\"%.2f\" "
        "patternTransform=\"rotate(45)\"><line x1=\"0\" y1=\"0\" x2=\"0\" "
        "y2=\"%.2f\" stroke=\"#9a9a9a\" stroke-width=\"%.3f\"/></pattern>"
        "</defs>\n", line_mm * 12.0, line_mm * 12.0, line_mm * 12.0,
        line_mm * 2.0);
    /* The whole plate grey: what the trims take shows around the edge. */
    (void) fputs("  <rect class=\"plate\" x=\"0\" y=\"0\" width=\"", args.out);
    s_mm(args.out, settings->plate_width_10um);
    (void) fputs("\" height=\"", args.out);
    s_mm(args.out, settings->plate_height_10um);
    (void) fputs("\" fill=\"#d0d0d0\"/>\n", args.out);
    s_svg_nodes(args.out, plan, args.labels, plan->plates[args.plate].root, 0U);

    for (index = 0U; index < count; ++index) {
        const bp_cut_t *cut = &args.scratch_cuts[index];
        const char *colour = s_stage_colours[cut->stage < S_STAGE_COLOURS
            ? cut->stage : S_STAGE_COLOURS - 1U];

        if (cut->plate != args.plate)
            continue;
        (void) fprintf(args.out, "  <rect class=\"cut\" data-stage=\"%lu\"",
            (unsigned long) cut->stage);
        if (cut->axis == BP_CUT_AXIS_X)
            s_rect_attributes(args.out, settings->plate_height_10um,
                cut->from_10um, cut->position_10um,
                cut->to_10um - cut->from_10um, cut->kerf_10um);
        else
            s_rect_attributes(args.out, settings->plate_height_10um,
                cut->position_10um, cut->from_10um, cut->kerf_10um,
                cut->to_10um - cut->from_10um);
        /* The stroke keeps a zero-width blade visible. */
        (void) fprintf(args.out, " fill=\"%s\" stroke=\"%s\" "
            "stroke-width=\"%.3f\"/>\n", colour, colour, line_mm);
    }
    (void) fputs("</svg>\n", args.out);
    if (fflush(args.out) != 0 || ferror(args.out))
        result.status = BP_CUTEXPORT_WRITE_FAILED;
    return result;
}
