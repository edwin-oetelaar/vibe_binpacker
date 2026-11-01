#include "bp_cutplan.h"

#include "bp_contract.h"

#include <stddef.h>
#include <string.h>

/* A panel's extent split into the axis it is cut along and the one across. */
typedef struct {
    uint64_t along_start;
    uint64_t along_end;
    uint64_t cross_start;
    uint64_t cross_end;
} s_span_t;

/* The walk state of a check, so the recursion carries one pointer. */
typedef struct {
    const bp_cutplan_t *plan;
    uint8_t *seen;
    uint8_t *used;
    bp_cut_axis_t first_axis;
    bp_cutplan_check_result_t result;
} s_check_t;

/* The walk state of the saw list; cuts may be NULL to only count. */
typedef struct {
    const bp_cutplan_t *plan;
    bp_cut_t *cuts;
    uint32_t capacity;
    uint32_t count;
    uint32_t plate;
    bp_cut_axis_t first_axis;
} s_walk_t;

/* Aborts on settings no loader should let through. */
static void
s_require_settings(const bp_cut_settings_t *settings)
{
    BP_REQUIRE(settings);
    BP_REQUIRE(settings->plate_width_10um > 0U);
    BP_REQUIRE(settings->plate_height_10um > 0U);
    BP_REQUIRE((uint64_t) settings->trim_left_10um + settings->trim_right_10um
        < settings->plate_width_10um);
    BP_REQUIRE((uint64_t) settings->trim_bottom_10um + settings->trim_top_10um
        < settings->plate_height_10um);
    BP_REQUIRE(settings->max_stages >= 1U);
    BP_REQUIRE(settings->max_stages <= BP_CUTPLAN_MAX_STAGES);
    BP_REQUIRE(settings->first_cut == BP_CUT_FIRST_X
        || settings->first_cut == BP_CUT_FIRST_Y
        || settings->first_cut == BP_CUT_FIRST_ANY);
    BP_REQUIRE(settings->non_exact <= 1U);
}

/* Aborts on a node no builder should produce; returns it otherwise. */
static const bp_cut_node_t *
s_require_node(const bp_cutplan_t *plan, uint32_t index)
{
    const bp_cut_node_t *node;

    BP_REQUIRE(plan);
    BP_REQUIRE(index < plan->node_count);
    node = &plan->nodes[index];
    BP_REQUIRE(node->kind == BP_CUT_NODE_PANEL || node->kind == BP_CUT_NODE_ITEM
        || node->kind == BP_CUT_NODE_WASTE);
    BP_REQUIRE(node->width_10um > 0U && node->height_10um > 0U);
    BP_REQUIRE((uint64_t) node->x_10um + node->width_10um <= UINT32_MAX);
    BP_REQUIRE((uint64_t) node->y_10um + node->height_10um <= UINT32_MAX);
    if (node->kind == BP_CUT_NODE_PANEL) {
        BP_REQUIRE(node->child_count > 0U);
        BP_REQUIRE((uint64_t) node->first_child + node->child_count
            <= plan->node_count);
    } else if (node->kind == BP_CUT_NODE_ITEM) {
        BP_REQUIRE(node->item_index < plan->item_count);
        BP_REQUIRE(node->rotated <= 1U);
    }
    return node;
}

/* Returns the axis of a stage: the first axis on odd stages, else the other. */
static bp_cut_axis_t
s_stage_axis(bp_cut_axis_t first_axis, uint32_t stage)
{
    BP_REQUIRE(first_axis == BP_CUT_AXIS_X || first_axis == BP_CUT_AXIS_Y);
    BP_REQUIRE(stage >= 1U);
    if (stage % 2U == 1U)
        return first_axis;
    return first_axis == BP_CUT_AXIS_X ? BP_CUT_AXIS_Y : BP_CUT_AXIS_X;
}

/*
 * Splits a node's rectangle by the axis it is cut along. An X cut runs
 * parallel to x, so the pieces follow each other along y.
 */
static s_span_t
s_span(const bp_cut_node_t *node, bp_cut_axis_t axis)
{
    s_span_t span;

    BP_REQUIRE(node);
    if (axis == BP_CUT_AXIS_X) {
        span.along_start = node->y_10um;
        span.along_end = (uint64_t) node->y_10um + node->height_10um;
        span.cross_start = node->x_10um;
        span.cross_end = (uint64_t) node->x_10um + node->width_10um;
    } else {
        span.along_start = node->x_10um;
        span.along_end = (uint64_t) node->x_10um + node->width_10um;
        span.cross_start = node->y_10um;
        span.cross_end = (uint64_t) node->y_10um + node->height_10um;
    }
    return span;
}

/* Records the first broken rule and returns 0, so callers can return it. */
static int
s_fail(s_check_t *check, bp_cutplan_status_t status, uint32_t node)
{
    BP_REQUIRE(check);
    BP_REQUIRE(status != BP_CUTPLAN_OK);
    check->result.status = status;
    check->result.node = node;
    return 0;
}

/* Checks that an item is its part, in an orientation the part allows. */
static int
s_check_item(s_check_t *check, uint32_t index, const bp_cut_node_t *node)
{
    const bp_cut_item_t *item;

    BP_REQUIRE(check && node && node->kind == BP_CUT_NODE_ITEM);
    item = &check->plan->items[node->item_index];
    if (check->used[node->item_index])
        return s_fail(check, BP_CUTPLAN_ITEM_REPEATED, index);
    check->used[node->item_index] = 1U;
    if (node->rotated && !item->may_rotate)
        return s_fail(check, BP_CUTPLAN_BAD_ITEM_ROTATION, index);
    if (node->rotated ? (node->width_10um != item->height_10um
            || node->height_10um != item->width_10um)
        : (node->width_10um != item->width_10um
            || node->height_10um != item->height_10um))
        return s_fail(check, BP_CUTPLAN_BAD_ITEM_SIZE, index);
    return 1;
}

static int
s_check_node(s_check_t *check, uint32_t index, uint32_t depth);

/*
 * Checks how a panel is split: children edge to edge, one kerf apart, no
 * more than a kerf left after the last, waste wide enough to cut off, and
 * not deeper than the saw's stages. Then descends into the children.
 */
static int
s_check_panel(s_check_t *check, uint32_t index, const bp_cut_node_t *panel,
    uint32_t depth)
{
    const bp_cut_settings_t *settings;
    bp_cut_axis_t axis;
    s_span_t outer;
    uint64_t next_start;
    uint32_t child;
    uint32_t items = 0U;
    int has_cut;

    BP_REQUIRE(check && panel && panel->kind == BP_CUT_NODE_PANEL);
    settings = &check->plan->settings;
    if (depth + 1U > settings->max_stages + settings->non_exact)
        return s_fail(check, BP_CUTPLAN_TOO_MANY_STAGES, index);

    axis = s_stage_axis(check->first_axis, depth + 1U);
    outer = s_span(panel, axis);
    next_start = outer.along_start;
    for (child = 0U; child < panel->child_count; ++child) {
        uint32_t child_index = panel->first_child + child;
        s_span_t inner = s_span(s_require_node(check->plan, child_index), axis);

        if (inner.cross_start != outer.cross_start
            || inner.cross_end != outer.cross_end)
            return s_fail(check, BP_CUTPLAN_NOT_EDGE_TO_EDGE, child_index);
        if (inner.along_start != next_start)
            return s_fail(check, child == 0U ? BP_CUTPLAN_BAD_START
                : BP_CUTPLAN_BAD_KERF, child_index);
        if (inner.along_end > outer.along_end)
            return s_fail(check, BP_CUTPLAN_BAD_REMAINDER, child_index);
        next_start = inner.along_end + settings->kerf_10um;
    }
    /* next_start - kerf is where the last child ends. */
    if (outer.along_end - (next_start - settings->kerf_10um)
        > settings->kerf_10um)
        return s_fail(check, BP_CUTPLAN_BAD_REMAINDER, index);

    has_cut = panel->child_count > 1U
        || next_start - settings->kerf_10um < outer.along_end;
    for (child = 0U; child < panel->child_count; ++child) {
        uint32_t child_index = panel->first_child + child;
        const bp_cut_node_t *node = &check->plan->nodes[child_index];
        s_span_t inner = s_span(node, axis);

        if (node->kind == BP_CUT_NODE_ITEM)
            ++items;
        if (depth + 1U > settings->max_stages
            && (node->kind == BP_CUT_NODE_PANEL || items > 1U))
            return s_fail(check, BP_CUTPLAN_BAD_SIZING_CUT, child_index);
        if (node->kind == BP_CUT_NODE_WASTE && has_cut
            && inner.along_end - inner.along_start < settings->min_waste_10um)
            return s_fail(check, BP_CUTPLAN_WASTE_TOO_NARROW, child_index);
    }

    for (child = 0U; child < panel->child_count; ++child)
        if (!s_check_node(check, panel->first_child + child, depth + 1U))
            return 0;
    return 1;
}

/* Visits one node exactly once and checks it by kind. */
static int
s_check_node(s_check_t *check, uint32_t index, uint32_t depth)
{
    const bp_cut_node_t *node;

    BP_REQUIRE(check);
    BP_REQUIRE(depth <= BP_CUTPLAN_MAX_STAGES + 1U);
    node = s_require_node(check->plan, index);
    /* A node reached twice is a broken builder, or a cycle. */
    BP_REQUIRE(!check->seen[index]);
    check->seen[index] = 1U;
    if (node->kind == BP_CUT_NODE_ITEM)
        return s_check_item(check, index, node);
    if (node->kind == BP_CUT_NODE_PANEL)
        return s_check_panel(check, index, node, depth);
    return 1;
}

/* Checks that a plan can be cut by the saw its settings describe. */
bp_cutplan_check_result_t
bp_cutplan_check(bp_cutplan_check_args_t args)
{
    const bp_cutplan_t *plan = args.plan;
    const bp_cut_settings_t *settings;
    s_check_t check;
    uint32_t plate;
    uint32_t index;
    uint32_t usable_x;
    uint32_t usable_y;
    uint32_t usable_width;
    uint32_t usable_height;

    BP_REQUIRE(plan);
    settings = &plan->settings;
    s_require_settings(settings);
    BP_REQUIRE(plan->nodes || plan->node_count == 0U);
    BP_REQUIRE(plan->plates || plan->plate_count == 0U);
    BP_REQUIRE(plan->items || plan->item_count == 0U);
    BP_REQUIRE(args.scratch_node_seen || plan->node_count == 0U);
    BP_REQUIRE(args.scratch_node_capacity >= plan->node_count);
    BP_REQUIRE(args.scratch_item_used || plan->item_count == 0U);
    BP_REQUIRE(args.scratch_item_capacity >= plan->item_count);

    if (plan->node_count > 0U)
        memset(args.scratch_node_seen, 0, plan->node_count);
    if (plan->item_count > 0U)
        memset(args.scratch_item_used, 0, plan->item_count);
    check.plan = plan;
    check.seen = args.scratch_node_seen;
    check.used = args.scratch_item_used;
    check.result = (bp_cutplan_check_result_t) {
        .status = BP_CUTPLAN_OK, .plate = 0U, .node = BP_CUTPLAN_NO_NODE
    };

    usable_x = settings->trim_left_10um;
    usable_y = settings->trim_bottom_10um;
    usable_width = settings->plate_width_10um - settings->trim_left_10um
        - settings->trim_right_10um;
    usable_height = settings->plate_height_10um - settings->trim_bottom_10um
        - settings->trim_top_10um;

    for (plate = 0U; plate < plan->plate_count; ++plate) {
        const bp_cut_plate_t *entry = &plan->plates[plate];
        const bp_cut_node_t *root = s_require_node(plan, entry->root);

        BP_REQUIRE(entry->first_axis == BP_CUT_AXIS_X
            || entry->first_axis == BP_CUT_AXIS_Y);
        check.result.plate = plate;
        check.first_axis = entry->first_axis;
        if (settings->first_cut != BP_CUT_FIRST_ANY
            && (int) entry->first_axis != (int) settings->first_cut) {
            (void) s_fail(&check, BP_CUTPLAN_BAD_FIRST_AXIS,
                BP_CUTPLAN_NO_NODE);
            return check.result;
        }
        if (root->x_10um != usable_x || root->y_10um != usable_y
            || root->width_10um != usable_width
            || root->height_10um != usable_height) {
            (void) s_fail(&check, BP_CUTPLAN_BAD_ROOT, entry->root);
            return check.result;
        }
        if (!s_check_node(&check, entry->root, 0U))
            return check.result;
    }
    /* Only a sound plan gets here; a stray node is a builder defect. */
    for (index = 0U; index < plan->node_count; ++index)
        BP_REQUIRE(check.seen[index]);
    check.result.plate = 0U;
    return check.result;
}

/* Returns a short human-readable form of a status. */
const char *
bp_cutplan_status_text(bp_cutplan_status_t status)
{
    switch (status) {
    case BP_CUTPLAN_OK:
        return "ok";
    case BP_CUTPLAN_BAD_ROOT:
        return "root is not the plate's usable area";
    case BP_CUTPLAN_BAD_FIRST_AXIS:
        return "first cut runs the way the saw cannot";
    case BP_CUTPLAN_NOT_EDGE_TO_EDGE:
        return "piece does not span its panel across the cut";
    case BP_CUTPLAN_BAD_START:
        return "first piece does not start at the panel's edge";
    case BP_CUTPLAN_BAD_KERF:
        return "neighbouring pieces are not one kerf apart";
    case BP_CUTPLAN_BAD_REMAINDER:
        return "more than a kerf left after the last piece, or an overrun";
    case BP_CUTPLAN_TOO_MANY_STAGES:
        return "more stages than the saw allows";
    case BP_CUTPLAN_BAD_SIZING_CUT:
        return "sizing cut yields more than one item or a panel";
    case BP_CUTPLAN_WASTE_TOO_NARROW:
        return "waste narrower than the saw can cut off";
    case BP_CUTPLAN_BAD_ITEM_SIZE:
        return "item size differs from its part";
    case BP_CUTPLAN_BAD_ITEM_ROTATION:
        return "item turned although its part may not be";
    case BP_CUTPLAN_ITEM_REPEATED:
        return "part placed twice";
    default:
        return "unknown status";
    }
}

/* Appends one cut, or only counts it when there is no output buffer. */
static void
s_emit(s_walk_t *walk, bp_cut_t cut)
{
    BP_REQUIRE(walk);
    if (walk->cuts) {
        BP_REQUIRE(walk->count < walk->capacity);
        walk->cuts[walk->count] = cut;
    }
    BP_REQUIRE(walk->count < UINT32_MAX);
    ++walk->count;
}

/*
 * Emits one plate's edge trims. A trim removes its whole width, blade
 * included, so the blade ends exactly at the usable area; a trim narrower than
 * the blade is all sawdust. Trims parallel to stage 1 come first and run the
 * full plate; the other two run between them.
 */
static void
s_walk_trims(s_walk_t *walk)
{
    const bp_cut_settings_t *settings;
    uint32_t width;
    uint32_t height;
    uint32_t kerf;
    int pass;

    BP_REQUIRE(walk);
    settings = &walk->plan->settings;
    width = settings->plate_width_10um;
    height = settings->plate_height_10um;
    kerf = settings->kerf_10um;
    for (pass = 0; pass < 2; ++pass) {
        bp_cut_axis_t axis = pass == 0 ? walk->first_axis
            : s_stage_axis(walk->first_axis, 2U);
        bp_cut_t cut = { .plate = walk->plate, .stage = 0U, .axis = axis,
            .node = BP_CUTPLAN_NO_NODE };
        uint32_t low = axis == BP_CUT_AXIS_X ? settings->trim_bottom_10um
            : settings->trim_left_10um;
        uint32_t high = axis == BP_CUT_AXIS_X ? settings->trim_top_10um
            : settings->trim_right_10um;
        uint32_t extent = axis == BP_CUT_AXIS_X ? height : width;

        /* The first pass runs edge to edge; the second between its trims. */
        if (axis == BP_CUT_AXIS_X) {
            cut.from_10um = pass == 0 ? 0U : settings->trim_left_10um;
            cut.to_10um = pass == 0 ? width : width - settings->trim_right_10um;
        } else {
            cut.from_10um = pass == 0 ? 0U : settings->trim_bottom_10um;
            cut.to_10um = pass == 0 ? height : height - settings->trim_top_10um;
        }
        if (low > 0U) {
            cut.kerf_10um = low < kerf ? low : kerf;
            cut.position_10um = low - cut.kerf_10um;
            s_emit(walk, cut);
        }
        if (high > 0U) {
            cut.kerf_10um = high < kerf ? high : kerf;
            cut.position_10um = extent - high;
            s_emit(walk, cut);
        }
    }
}

/* Emits a panel's cuts, then descends into its children in order. */
static void
s_walk_node(s_walk_t *walk, uint32_t index, uint32_t depth)
{
    const bp_cut_node_t *panel;
    bp_cut_axis_t axis;
    s_span_t outer;
    uint32_t child;
    uint64_t last_end = 0U;

    BP_REQUIRE(walk);
    panel = s_require_node(walk->plan, index);
    if (panel->kind != BP_CUT_NODE_PANEL)
        return;
    /* A checked plan has no panel below the last stage. */
    BP_REQUIRE(depth < BP_CUTPLAN_MAX_STAGES + 1U);
    axis = s_stage_axis(walk->first_axis, depth + 1U);
    outer = s_span(panel, axis);
    for (child = 0U; child < panel->child_count; ++child) {
        s_span_t inner = s_span(s_require_node(walk->plan,
            panel->first_child + child), axis);

        last_end = inner.along_end;
        if (child + 1U < panel->child_count || last_end < outer.along_end) {
            uint64_t rest = outer.along_end - last_end;
            uint32_t kerf = walk->plan->settings.kerf_10um;

            s_emit(walk, (bp_cut_t) {
                .plate = walk->plate, .stage = depth + 1U, .axis = axis,
                .position_10um = (uint32_t) last_end,
                /* Only the last cut can meet the edge within a kerf. */
                .kerf_10um = rest < kerf ? (uint32_t) rest : kerf,
                .from_10um = (uint32_t) outer.cross_start,
                .to_10um = (uint32_t) outer.cross_end,
                .node = index
            });
        }
    }
    for (child = 0U; child < panel->child_count; ++child)
        s_walk_node(walk, panel->first_child + child, depth + 1U);
}

/* Walks every plate, counting or writing the saw list. */
static uint32_t
s_walk(const bp_cutplan_t *plan, bp_cut_t *cuts, uint32_t capacity)
{
    s_walk_t walk = { .plan = plan, .cuts = cuts, .capacity = capacity };

    BP_REQUIRE(plan);
    s_require_settings(&plan->settings);
    BP_REQUIRE(plan->plates || plan->plate_count == 0U);
    for (walk.plate = 0U; walk.plate < plan->plate_count; ++walk.plate) {
        walk.first_axis = plan->plates[walk.plate].first_axis;
        s_walk_trims(&walk);
        s_walk_node(&walk, plan->plates[walk.plate].root, 0U);
    }
    return walk.count;
}

/* Returns how many cuts bp_cutplan_cuts writes. */
uint32_t
bp_cutplan_cut_count(const bp_cutplan_t *plan)
{
    return s_walk(plan, NULL, 0U);
}

/* Writes the saw list in the order an operator works. */
uint32_t
bp_cutplan_cuts(bp_cutplan_cuts_args_t args)
{
    BP_REQUIRE(args.plan);
    BP_REQUIRE(args.cuts);
    BP_REQUIRE(args.cut_capacity >= bp_cutplan_cut_count(args.plan));
    return s_walk(args.plan, args.cuts, args.cut_capacity);
}
