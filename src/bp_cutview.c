#include "bp_cutview.h"

#include "bp_contract.h"
#include "bp_cutexport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Publishes a plan: writes its saw list and counts what it places. */
bp_cutview_t
bp_cutview_publish(bp_cutview_publish_args_t args)
{
    bp_cutview_t view;
    uint32_t index;

    BP_REQUIRE(args.plan && args.cuts);
    BP_REQUIRE(args.labels || args.plan->item_count == 0U);
    memset(&view, 0, sizeof view);
    view.plan = args.plan;
    view.labels = args.labels;
    view.cuts = args.cuts;
    view.cut_count = bp_cutplan_cuts((bp_cutplan_cuts_args_t) {
        .plan = args.plan, .cuts = args.cuts,
        .cut_capacity = args.cut_capacity });
    for (index = 0U; index < args.plan->node_count; ++index) {
        const bp_cut_node_t *node = &args.plan->nodes[index];

        if (node->kind == BP_CUT_NODE_ITEM) {
            ++view.placed_count;
            view.item_area_10um2 += (uint64_t) node->width_10um
                * node->height_10um;
        }
    }
    if (args.plan->plate_count > 0U)
        bp_cutview_select(&view, 0U);
    return view;
}

/* Selects an existing plate and shows all of its cuts. */
void
bp_cutview_select(bp_cutview_t *view, uint32_t plate)
{
    uint32_t index;

    BP_REQUIRE(view && view->plan);
    BP_REQUIRE(plate < view->plan->plate_count);
    view->active_plate = plate;
    view->first_cut = 0U;
    view->plate_cuts = 0U;
    /* The saw list holds the plates in order, so one pass finds the range. */
    for (index = 0U; index < view->cut_count; ++index) {
        if (view->cuts[index].plate < plate)
            view->first_cut = index + 1U;
        else if (view->cuts[index].plate == plate)
            ++view->plate_cuts;
    }
    view->visible_cuts = view->plate_cuts;
}

/* Sets how many of the active plate's cuts are shown. */
void
bp_cutview_set_visible(bp_cutview_t *view, uint32_t count)
{
    BP_REQUIRE(view && view->plan && view->plan->plate_count > 0U);
    BP_REQUIRE(count <= view->plate_cuts);
    view->visible_cuts = count;
}

/* Returns one cut of the active plate. */
const bp_cut_t *
bp_cutview_cut(const bp_cutview_t *view, uint32_t local_index)
{
    BP_REQUIRE(view && local_index < view->plate_cuts);
    return &view->cuts[view->first_cut + local_index];
}

/* Returns the parts' share of all used plate area. */
double
bp_cutview_utilisation(const bp_cutview_t *view)
{
    const bp_cut_settings_t *settings;

    BP_REQUIRE(view);
    if (!view->plan || view->plan->plate_count == 0U)
        return 0.0;
    settings = &view->plan->settings;
    return (double) view->item_area_10um2 / ((double) settings->plate_width_10um
        * settings->plate_height_10um * view->plan->plate_count);
}

/*
 * Writes <stem><suffix> into scratch. Returns 0 when it does not fit, which
 * is reported rather than truncated: a shortened path writes the wrong file.
 */
static int
s_name(const bp_cutview_export_args_t *args, size_t stem, const char *suffix)
{
    size_t length = strlen(suffix);

    BP_REQUIRE(args && suffix);
    if (stem + length + 1U > args->path_capacity)
        return 0;
    memmove(args->scratch_path, args->path, stem);
    memcpy(args->scratch_path + stem, suffix, length + 1U);
    return 1;
}

/* Opens, writes and closes one file, keeping the first errno on failure. */
static int
s_write(const bp_cutview_export_args_t *args, uint32_t plate, int is_list,
    int *system_error)
{
    FILE *out;
    bp_cutexport_result_t result;
    int closed;

    BP_REQUIRE(args && system_error);
    errno = 0;
    out = fopen(args->scratch_path, "w");
    if (!out) {
        *system_error = errno;
        return 0;
    }
    if (is_list)
        result = bp_cutexport_csv((bp_cutexport_csv_args_t) {
            .out = out, .plan = args->view->plan, .labels = args->view->labels,
            .scratch_cuts = args->scratch_cuts,
            .scratch_cut_capacity = args->scratch_cut_capacity });
    else
        result = bp_cutexport_svg((bp_cutexport_svg_args_t) {
            .out = out, .plan = args->view->plan, .plate = plate,
            .labels = args->view->labels,
            .scratch_cuts = args->scratch_cuts,
            .scratch_cut_capacity = args->scratch_cut_capacity });
    if (result.status != BP_CUTEXPORT_OK)
        *system_error = errno;
    errno = 0;
    closed = fclose(out);
    if (closed != 0 && *system_error == 0)
        *system_error = errno;
    return result.status == BP_CUTEXPORT_OK && closed == 0;
}

/* Writes the saw list and one drawing per plate. */
bp_cutview_export_result_t
bp_cutview_export(bp_cutview_export_args_t args)
{
    bp_cutview_export_result_t result = { BP_CUTVIEW_EXPORT_OK, 0U, 0 };
    size_t stem;
    uint32_t plate;

    BP_REQUIRE(args.view && args.path && args.scratch_path);
    BP_REQUIRE(args.path_capacity > 0U);
    BP_REQUIRE(args.scratch_cuts);
    args.scratch_path[0] = '\0';
    if (!args.view->plan || args.view->plan->plate_count == 0U) {
        result.status = BP_CUTVIEW_EXPORT_EMPTY;
        return result;
    }
    stem = strlen(args.path);
    if (stem >= 4U && (strcmp(args.path + stem - 4U, ".csv") == 0
        || strcmp(args.path + stem - 4U, ".svg") == 0))
        stem -= 4U;

    if (!s_name(&args, stem, ".csv")) {
        result.status = BP_CUTVIEW_EXPORT_PATH_TOO_LONG;
        return result;
    }
    if (!s_write(&args, 0U, 1, &result.system_error)) {
        result.status = BP_CUTVIEW_EXPORT_WRITE_FAILED;
        return result;
    }
    ++result.written_count;
    for (plate = 0U; plate < args.view->plan->plate_count; ++plate) {
        char suffix[24];

        (void) snprintf(suffix, sizeof suffix, "-%03lu.svg",
            (unsigned long) plate + 1UL);
        if (!s_name(&args, stem, suffix)) {
            result.status = BP_CUTVIEW_EXPORT_PATH_TOO_LONG;
            return result;
        }
        if (!s_write(&args, plate, 0, &result.system_error)) {
            result.status = BP_CUTVIEW_EXPORT_WRITE_FAILED;
            return result;
        }
        ++result.written_count;
    }
    args.view->saved = 1;
    return result;
}
