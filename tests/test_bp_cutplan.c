#include "test_support.h"

#include "bp_cutplan.h"

#include <string.h>

#define S_NODES 16U
#define S_ITEMS 8U
#define S_PLATES 4U
#define S_CUTS 32U

/*
 * A hand-built three-stage plan on a 1000 x 500 mm plate, kerf 4.4 mm,
 * minimum waste 20 mm, first cut along x. Lengths in 0.01 mm.
 *
 *   0 root                      0..1000 x 0..500
 *     1 strip                   y 0..200
 *       4 item A 300 x 200      x 0..300
 *       5 stack                 x 304.4..504.4
 *         7 item D 200 x 97.8   y 0..97.8
 *         8 item D 200 x 97.8   y 102.2..200
 *       6 waste                 x 508.8..1000
 *     2 strip                   y 204.4..304.4
 *       9 item B 300 x 100      x 0..300
 *      10 waste                 x 304.4..1000
 *     3 waste                   y 308.8..500
 *
 * Item 4 (E) is requested but not placed, which is allowed.
 */
typedef struct {
    bp_cut_node_t nodes[S_NODES];
    bp_cut_plate_t plates[S_PLATES];
    bp_cut_item_t items[S_ITEMS];
    bp_cutplan_t plan;
    uint8_t seen[S_NODES];
    uint8_t used[S_ITEMS];
} s_fixture_t;

static s_fixture_t fixture;

/* Returns a panel node. */
static bp_cut_node_t
s_panel(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    uint32_t first_child, uint32_t child_count)
{
    return (bp_cut_node_t) { .kind = BP_CUT_NODE_PANEL, .x_10um = x,
        .y_10um = y, .width_10um = width, .height_10um = height,
        .first_child = first_child, .child_count = child_count };
}

/* Returns an item node. */
static bp_cut_node_t
s_item(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    uint32_t item_index)
{
    return (bp_cut_node_t) { .kind = BP_CUT_NODE_ITEM, .x_10um = x,
        .y_10um = y, .width_10um = width, .height_10um = height,
        .item_index = item_index };
}

/* Returns a waste node. */
static bp_cut_node_t
s_waste(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    return (bp_cut_node_t) { .kind = BP_CUT_NODE_WASTE, .x_10um = x,
        .y_10um = y, .width_10um = width, .height_10um = height };
}

/* Builds the plan in the comment above. */
static void
s_init(s_fixture_t *f)
{
    memset(f, 0, sizeof *f);
    f->nodes[0] = s_panel(0U, 0U, 100000U, 50000U, 1U, 3U);
    f->nodes[1] = s_panel(0U, 0U, 100000U, 20000U, 4U, 3U);
    f->nodes[2] = s_panel(0U, 20440U, 100000U, 10000U, 9U, 2U);
    f->nodes[3] = s_waste(0U, 30880U, 100000U, 19120U);
    f->nodes[4] = s_item(0U, 0U, 30000U, 20000U, 0U);
    f->nodes[5] = s_panel(30440U, 0U, 20000U, 20000U, 7U, 2U);
    f->nodes[6] = s_waste(50880U, 0U, 49120U, 20000U);
    f->nodes[7] = s_item(30440U, 0U, 20000U, 9780U, 2U);
    f->nodes[8] = s_item(30440U, 10220U, 20000U, 9780U, 3U);
    f->nodes[9] = s_item(0U, 20440U, 30000U, 10000U, 1U);
    f->nodes[10] = s_waste(30440U, 20440U, 69560U, 10000U);
    f->items[0] = (bp_cut_item_t) { 30000U, 20000U, 1U };
    f->items[1] = (bp_cut_item_t) { 30000U, 10000U, 0U };
    f->items[2] = (bp_cut_item_t) { 20000U, 9780U, 0U };
    f->items[3] = (bp_cut_item_t) { 20000U, 9780U, 0U };
    f->items[4] = (bp_cut_item_t) { 5000U, 5000U, 1U };
    f->plates[0] = (bp_cut_plate_t) { .root = 0U, .first_axis = BP_CUT_AXIS_X };
    f->plan = (bp_cutplan_t) {
        .settings = {
            .plate_width_10um = 100000U, .plate_height_10um = 50000U,
            .kerf_10um = 440U, .min_waste_10um = 2000U, .max_stages = 3U,
            .first_cut = BP_CUT_FIRST_X
        },
        .nodes = f->nodes, .node_count = 11U,
        .plates = f->plates, .plate_count = 1U,
        .items = f->items, .item_count = 5U
    };
}

/* Checks the fixture's plan with exactly sized scratch. */
static bp_cutplan_check_result_t
s_check(s_fixture_t *f)
{
    return bp_cutplan_check((bp_cutplan_check_args_t) {
        .plan = &f->plan,
        .scratch_node_seen = f->seen, .scratch_node_capacity = f->plan.node_count,
        .scratch_item_used = f->used, .scratch_item_capacity = f->plan.item_count
    });
}

/* Requires one status at one node of plate 0. */
static void
s_expect(s_fixture_t *f, bp_cutplan_status_t status, uint32_t node)
{
    bp_cutplan_check_result_t result = s_check(f);

    if (result.status != status || result.node != node) {
        (void) fprintf(stderr, "got %s at node %u, expected %s at node %u\n",
            bp_cutplan_status_text(result.status), (unsigned) result.node,
            bp_cutplan_status_text(status), (unsigned) node);
        CHECK(0);
    }
}

/* Writes the saw list and returns its length. */
static uint32_t
s_cuts(s_fixture_t *f, bp_cut_t *cuts)
{
    uint32_t count = bp_cutplan_cut_count(&f->plan);

    CHECK(count <= S_CUTS);
    CHECK(bp_cutplan_cuts((bp_cutplan_cuts_args_t) {
        .plan = &f->plan, .cuts = cuts, .cut_capacity = count }) == count);
    return count;
}

/* Requires one cut of a saw list. */
static void
s_expect_cut(const bp_cut_t *cut, uint32_t stage, bp_cut_axis_t axis,
    uint32_t position, uint32_t kerf, uint32_t from, uint32_t to, uint32_t node)
{
    CHECK(cut->stage == stage);
    CHECK(cut->axis == axis);
    CHECK(cut->position_10um == position);
    CHECK(cut->kerf_10um == kerf);
    CHECK(cut->from_10um == from && cut->to_10um == to);
    CHECK(cut->node == node);
}

/* The hand-built plan is sawable and its saw list is in operator order. */
static void
s_test_valid_plan(void)
{
    bp_cut_t cuts[S_CUTS];
    bp_cutplan_check_result_t result;

    s_init(&fixture);
    result = s_check(&fixture);
    CHECK(result.status == BP_CUTPLAN_OK);
    CHECK(result.plate == 0U && result.node == BP_CUTPLAN_NO_NODE);
    CHECK(s_cuts(&fixture, cuts) == 6U);
    s_expect_cut(&cuts[0], 1U, BP_CUT_AXIS_X, 20000U, 440U, 0U, 100000U, 0U);
    s_expect_cut(&cuts[1], 1U, BP_CUT_AXIS_X, 30440U, 440U, 0U, 100000U, 0U);
    s_expect_cut(&cuts[2], 2U, BP_CUT_AXIS_Y, 30000U, 440U, 0U, 20000U, 1U);
    s_expect_cut(&cuts[3], 2U, BP_CUT_AXIS_Y, 50440U, 440U, 0U, 20000U, 1U);
    s_expect_cut(&cuts[4], 3U, BP_CUT_AXIS_X, 9780U, 440U, 30440U, 50440U, 5U);
    s_expect_cut(&cuts[5], 2U, BP_CUT_AXIS_Y, 30000U, 440U, 20440U, 30440U, 2U);
    CHECK(cuts[0].plate == 0U && cuts[5].plate == 0U);

    /* A saw that may start either way accepts the plan's choice. */
    fixture.plan.settings.first_cut = BP_CUT_FIRST_ANY;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
}

/* Each geometric rule of the tree, broken once. */
static void
s_test_geometry_rules(void)
{
    s_init(&fixture);
    fixture.nodes[0].width_10um = 99999U;
    s_expect(&fixture, BP_CUTPLAN_BAD_ROOT, 0U);

    s_init(&fixture);
    fixture.plan.settings.first_cut = BP_CUT_FIRST_Y;
    s_expect(&fixture, BP_CUTPLAN_BAD_FIRST_AXIS, BP_CUTPLAN_NO_NODE);

    s_init(&fixture);
    fixture.nodes[1].width_10um = 99999U;
    s_expect(&fixture, BP_CUTPLAN_NOT_EDGE_TO_EDGE, 1U);

    s_init(&fixture);
    fixture.nodes[4].x_10um = 1U;
    s_expect(&fixture, BP_CUTPLAN_BAD_START, 4U);

    /* One unit closer or further than the kerf is wrong either way. */
    s_init(&fixture);
    fixture.nodes[5].x_10um = 30439U;
    s_expect(&fixture, BP_CUTPLAN_BAD_KERF, 5U);
    s_init(&fixture);
    fixture.nodes[5].x_10um = 30441U;
    s_expect(&fixture, BP_CUTPLAN_BAD_KERF, 5U);

    s_init(&fixture);
    fixture.nodes[6].width_10um = 49121U;
    s_expect(&fixture, BP_CUTPLAN_BAD_REMAINDER, 6U);
}

/*
 * Material after the last piece: up to one kerf is sawn into dust by a last
 * cut that meets the edge, one unit more must be an explicit waste piece.
 */
static void
s_test_remainder_at_the_edge(void)
{
    bp_cut_t cuts[S_CUTS];
    uint32_t count;

    s_init(&fixture);
    fixture.nodes[10].width_10um = 69560U - 440U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    count = s_cuts(&fixture, cuts);
    CHECK(count == 7U);
    s_expect_cut(&cuts[6], 2U, BP_CUT_AXIS_Y, 99560U, 440U, 20440U, 30440U, 2U);

    fixture.nodes[10].width_10um = 69560U - 100U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    count = s_cuts(&fixture, cuts);
    s_expect_cut(&cuts[6], 2U, BP_CUT_AXIS_Y, 99900U, 100U, 20440U, 30440U, 2U);

    fixture.nodes[10].width_10um = 69560U - 441U;
    s_expect(&fixture, BP_CUTPLAN_BAD_REMAINDER, 2U);
}

/* Waste must be wide enough to cut off: exactly the minimum is enough. */
static void
s_test_minimum_waste(void)
{
    s_init(&fixture);
    fixture.plan.settings.min_waste_10um = 19120U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    fixture.plan.settings.min_waste_10um = 19121U;
    s_expect(&fixture, BP_CUTPLAN_WASTE_TOO_NARROW, 3U);

    /*
     * Waste below a level without a cut was still cut off one level up, at
     * that width. Found by the random cross-check against check_cutplan.py.
     */
    s_init(&fixture);
    fixture.nodes[3] = s_panel(0U, 30880U, 100000U, 19120U, 11U, 1U);
    fixture.nodes[11] = s_waste(0U, 30880U, 100000U, 19120U);
    fixture.plan.node_count = 12U;
    fixture.plan.settings.min_waste_10um = 19120U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    fixture.plan.settings.min_waste_10um = 19121U;
    s_expect(&fixture, BP_CUTPLAN_WASTE_TOO_NARROW, 11U);

    /* Waste that no cut produced is not cut off, so it has no minimum. */
    s_init(&fixture);
    fixture.nodes[0] = s_panel(0U, 0U, 100000U, 50000U, 1U, 1U);
    fixture.nodes[1] = s_waste(0U, 0U, 100000U, 50000U);
    fixture.plan.node_count = 2U;
    fixture.plan.settings.min_waste_10um = 60000U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    CHECK(bp_cutplan_cut_count(&fixture.plan) == 0U);
}

/* Stage limits, and the one sizing cut a non-exact saw may add. */
static void
s_test_stages(void)
{
    s_init(&fixture);
    fixture.plan.settings.max_stages = 2U;
    s_expect(&fixture, BP_CUTPLAN_TOO_MANY_STAGES, 5U);

    /* A sizing cut may free one item; two are a third stage in disguise. */
    fixture.plan.settings.non_exact = 1U;
    s_expect(&fixture, BP_CUTPLAN_BAD_SIZING_CUT, 8U);

    fixture.nodes[8] = s_waste(30440U, 10220U, 20000U, 9780U);
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    fixture.plan.settings.non_exact = 0U;
    s_expect(&fixture, BP_CUTPLAN_TOO_MANY_STAGES, 5U);

    /* A sizing cut yields no panel: the strip may size, the stack not. */
    s_init(&fixture);
    fixture.plan.settings.max_stages = 1U;
    fixture.plan.settings.non_exact = 1U;
    s_expect(&fixture, BP_CUTPLAN_BAD_SIZING_CUT, 5U);
}

/* Items are their parts, in an orientation the part allows, at most once. */
static void
s_test_items(void)
{
    s_init(&fixture);
    fixture.items[0].width_10um = 29999U;
    s_expect(&fixture, BP_CUTPLAN_BAD_ITEM_SIZE, 4U);

    s_init(&fixture);
    fixture.nodes[9].rotated = 1U;
    s_expect(&fixture, BP_CUTPLAN_BAD_ITEM_ROTATION, 9U);

    /* A part ordered as 200 x 300 and turned fits the 300 x 200 slot. */
    s_init(&fixture);
    fixture.items[0] = (bp_cut_item_t) { 20000U, 30000U, 1U };
    fixture.nodes[4].rotated = 1U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    fixture.items[0].may_rotate = 0U;
    s_expect(&fixture, BP_CUTPLAN_BAD_ITEM_ROTATION, 4U);
    fixture.items[0].may_rotate = 1U;
    fixture.nodes[4].rotated = 0U;
    s_expect(&fixture, BP_CUTPLAN_BAD_ITEM_SIZE, 4U);

    s_init(&fixture);
    fixture.nodes[8].item_index = 2U;
    s_expect(&fixture, BP_CUTPLAN_ITEM_REPEATED, 8U);
}

/*
 * Edge trims remove their whole width, blade included, and come first in the
 * saw list: those parallel to stage 1 across the full plate, then the others
 * between them. A trim narrower than the blade is all dust.
 */
static void
s_test_edge_trims(void)
{
    bp_cut_t cuts[S_CUTS];

    s_init(&fixture);
    fixture.plan.settings.trim_left_10um = 1000U;
    fixture.plan.settings.trim_right_10um = 1000U;
    fixture.plan.settings.trim_bottom_10um = 300U;
    fixture.plan.settings.trim_top_10um = 1000U;
    fixture.nodes[0] = s_waste(1000U, 300U, 98000U, 48700U);
    fixture.plan.node_count = 1U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    CHECK(s_cuts(&fixture, cuts) == 4U);
    s_expect_cut(&cuts[0], 0U, BP_CUT_AXIS_X, 0U, 300U, 0U, 100000U,
        BP_CUTPLAN_NO_NODE);
    s_expect_cut(&cuts[1], 0U, BP_CUT_AXIS_X, 49000U, 440U, 0U, 100000U,
        BP_CUTPLAN_NO_NODE);
    s_expect_cut(&cuts[2], 0U, BP_CUT_AXIS_Y, 560U, 440U, 300U, 49000U,
        BP_CUTPLAN_NO_NODE);
    s_expect_cut(&cuts[3], 0U, BP_CUT_AXIS_Y, 99000U, 440U, 300U, 49000U,
        BP_CUTPLAN_NO_NODE);

    fixture.plates[0].first_axis = BP_CUT_AXIS_Y;
    fixture.plan.settings.first_cut = BP_CUT_FIRST_Y;
    CHECK(s_cuts(&fixture, cuts) == 4U);
    s_expect_cut(&cuts[0], 0U, BP_CUT_AXIS_Y, 560U, 440U, 0U, 50000U,
        BP_CUTPLAN_NO_NODE);
    s_expect_cut(&cuts[2], 0U, BP_CUT_AXIS_X, 0U, 300U, 1000U, 99000U,
        BP_CUTPLAN_NO_NODE);

    /* The root is the usable area, not the plate. */
    fixture.nodes[0] = s_waste(0U, 0U, 100000U, 50000U);
    s_expect(&fixture, BP_CUTPLAN_BAD_ROOT, 0U);
}

/*
 * The same plan with x and y exchanged, cut along y first. Every rule must be
 * symmetric, so it is just as valid and its saw list is the mirror image.
 */
static void
s_test_transposed(void)
{
    bp_cut_t cuts[S_CUTS];
    uint32_t index;

    s_init(&fixture);
    for (index = 0U; index < fixture.plan.node_count; ++index) {
        bp_cut_node_t *node = &fixture.nodes[index];
        uint32_t swap = node->x_10um;

        node->x_10um = node->y_10um;
        node->y_10um = swap;
        swap = node->width_10um;
        node->width_10um = node->height_10um;
        node->height_10um = swap;
    }
    for (index = 0U; index < fixture.plan.item_count; ++index) {
        uint32_t swap = fixture.items[index].width_10um;

        fixture.items[index].width_10um = fixture.items[index].height_10um;
        fixture.items[index].height_10um = swap;
    }
    fixture.plan.settings.plate_width_10um = 50000U;
    fixture.plan.settings.plate_height_10um = 100000U;
    fixture.plan.settings.first_cut = BP_CUT_FIRST_Y;
    fixture.plates[0].first_axis = BP_CUT_AXIS_Y;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    CHECK(s_cuts(&fixture, cuts) == 6U);
    s_expect_cut(&cuts[0], 1U, BP_CUT_AXIS_Y, 20000U, 440U, 0U, 100000U, 0U);
    s_expect_cut(&cuts[4], 3U, BP_CUT_AXIS_Y, 9780U, 440U, 30440U, 50440U, 5U);
}

/* Several plates are checked in order and the failing one is named. */
static void
s_test_plates(void)
{
    bp_cut_t cuts[S_CUTS];

    s_init(&fixture);
    fixture.nodes[11] = s_waste(0U, 0U, 100000U, 50000U);
    fixture.plates[1] = (bp_cut_plate_t) { .root = 11U,
        .first_axis = BP_CUT_AXIS_X };
    fixture.plan.node_count = 12U;
    fixture.plan.plate_count = 2U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    CHECK(s_cuts(&fixture, cuts) == 6U);

    fixture.nodes[11].height_10um = 49999U;
    {
        bp_cutplan_check_result_t result = s_check(&fixture);

        CHECK(result.status == BP_CUTPLAN_BAD_ROOT);
        CHECK(result.plate == 1U && result.node == 11U);
    }
}

/* Zero kerf, as for a plan that is only checked for structure. */
static void
s_test_zero_kerf(void)
{
    s_init(&fixture);
    fixture.plan.settings.kerf_10um = 0U;
    fixture.nodes[0] = s_panel(0U, 0U, 100000U, 50000U, 1U, 2U);
    fixture.nodes[1] = s_item(0U, 0U, 100000U, 20000U, 0U);
    fixture.nodes[2] = s_waste(0U, 20000U, 100000U, 30000U);
    fixture.items[0] = (bp_cut_item_t) { 100000U, 20000U, 0U };
    fixture.plan.node_count = 3U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    fixture.nodes[2].height_10um = 29999U;
    s_expect(&fixture, BP_CUTPLAN_BAD_REMAINDER, 0U);
}

/* A plate near the top of the length range does not overflow. */
static void
s_test_large_plate(void)
{
    bp_cut_t cuts[S_CUTS];

    s_init(&fixture);
    fixture.plan.settings.plate_width_10um = 4000000000U;
    fixture.plan.settings.trim_right_10um = 1000U;
    fixture.nodes[0] = s_panel(0U, 0U, 3999999000U, 50000U, 1U, 2U);
    fixture.nodes[1] = s_item(0U, 0U, 3000000000U, 50000U, 0U);
    fixture.nodes[2] = s_waste(3000000440U, 0U, 999998560U, 50000U);
    fixture.items[0] = (bp_cut_item_t) { 3000000000U, 50000U, 0U };
    fixture.plates[0].first_axis = BP_CUT_AXIS_Y;
    fixture.plan.settings.first_cut = BP_CUT_FIRST_Y;
    fixture.plan.node_count = 3U;
    s_expect(&fixture, BP_CUTPLAN_OK, BP_CUTPLAN_NO_NODE);
    CHECK(s_cuts(&fixture, cuts) == 2U);
    s_expect_cut(&cuts[0], 0U, BP_CUT_AXIS_Y, 3999999000U, 440U, 0U, 50000U,
        BP_CUTPLAN_NO_NODE);
    s_expect_cut(&cuts[1], 1U, BP_CUT_AXIS_Y, 3000000000U, 440U, 0U, 50000U,
        0U);
}

/* Every status has its own message. */
static void
s_test_status_text(void)
{
    int status;

    for (status = BP_CUTPLAN_OK; status <= BP_CUTPLAN_ITEM_REPEATED; ++status)
        CHECK(strcmp(bp_cutplan_status_text((bp_cutplan_status_t) status),
            "unknown status") != 0);
}

/* Trims that leave no usable area are invalid settings. */
static void
s_body_trims_too_wide(void)
{
    s_init(&fixture);
    fixture.plan.settings.trim_left_10um = 50000U;
    fixture.plan.settings.trim_right_10um = 50000U;
    (void) s_check(&fixture);
}

/* A saw with no stages, or more than the module walks, is invalid. */
static void
s_body_bad_stages(void)
{
    s_init(&fixture);
    fixture.plan.settings.max_stages = BP_CUTPLAN_MAX_STAGES + 1U;
    (void) s_check(&fixture);
}

/* Two plates sharing a root is not a forest of trees. */
static void
s_body_shared_root(void)
{
    s_init(&fixture);
    fixture.plates[1] = fixture.plates[0];
    fixture.plan.plate_count = 2U;
    (void) s_check(&fixture);
}

/* A node no plate reaches is a builder defect. */
static void
s_body_stray_node(void)
{
    s_init(&fixture);
    fixture.nodes[11] = s_waste(0U, 0U, 10U, 10U);
    fixture.plan.node_count = 12U;
    (void) s_check(&fixture);
}

/* A panel without children is not a split. */
static void
s_body_empty_panel(void)
{
    s_init(&fixture);
    fixture.nodes[5].child_count = 0U;
    (void) s_check(&fixture);
}

/* An item must name a requested part. */
static void
s_body_bad_item_index(void)
{
    s_init(&fixture);
    fixture.nodes[4].item_index = 5U;
    (void) s_check(&fixture);
}

/* Scratch must cover every node. */
static void
s_body_small_scratch(void)
{
    s_init(&fixture);
    (void) bp_cutplan_check((bp_cutplan_check_args_t) {
        .plan = &fixture.plan,
        .scratch_node_seen = fixture.seen,
        .scratch_node_capacity = fixture.plan.node_count - 1U,
        .scratch_item_used = fixture.used,
        .scratch_item_capacity = fixture.plan.item_count
    });
}

/* The saw list buffer must hold every cut. */
static void
s_body_small_cut_buffer(void)
{
    bp_cut_t cuts[S_CUTS];

    s_init(&fixture);
    (void) bp_cutplan_cuts((bp_cutplan_cuts_args_t) {
        .plan = &fixture.plan, .cuts = cuts, .cut_capacity = 5U });
}

/* Exercises the cutting plan model, its check and its saw list. */
int
main(void)
{
    s_test_valid_plan();
    s_test_geometry_rules();
    s_test_remainder_at_the_edge();
    s_test_minimum_waste();
    s_test_stages();
    s_test_items();
    s_test_edge_trims();
    s_test_transposed();
    s_test_plates();
    s_test_zero_kerf();
    s_test_large_plate();
    s_test_status_text();
    t_expect_abort(s_body_trims_too_wide, "trims too wide");
    t_expect_abort(s_body_bad_stages, "bad stage count");
    t_expect_abort(s_body_shared_root, "shared root");
    t_expect_abort(s_body_stray_node, "stray node");
    t_expect_abort(s_body_empty_panel, "empty panel");
    t_expect_abort(s_body_bad_item_index, "bad item index");
    t_expect_abort(s_body_small_scratch, "small scratch");
    t_expect_abort(s_body_small_cut_buffer, "small cut buffer");
    (void) printf("test_bp_cutplan: all checks passed\n");
    return 0;
}
