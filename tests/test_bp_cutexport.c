#include "test_support.h"

#include "bp_cutexport.h"
#include "bp_cutplan.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#define S_CASES_DIRECTORY "build/cutplan-cases"
#define S_RANDOM_PLANS 200U
#define S_MAX_NODES 512U
#define S_MAX_ITEMS 512U
#define S_MAX_PLATES 3U
#define S_MAX_CUTS 2048U

/*
 * A plan with its storage. The fixture is the plan of tests/test_bp_cutplan.c
 * and tools/test_check_cutplan.py; random plans fill the same structure.
 */
typedef struct {
    bp_cut_node_t nodes[S_MAX_NODES];
    bp_cut_item_t items[S_MAX_ITEMS];
    uint32_t labels[S_MAX_ITEMS];
    bp_cut_plate_t plates[S_MAX_PLATES];
    bp_cutplan_t plan;
    uint8_t seen[S_MAX_NODES];
    uint8_t used[S_MAX_ITEMS];
    bp_cut_t cuts[S_MAX_CUTS];
    uint64_t random;
} s_plan_t;

static s_plan_t plan;

/* Returns a node of the given kind and rectangle. */
static bp_cut_node_t
s_node(bp_cut_node_kind_t kind, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    return (bp_cut_node_t) { .kind = kind, .x_10um = x, .y_10um = y,
        .width_10um = width, .height_10um = height };
}

/* Builds the three-stage fixture, with labels 1 to 4 for parts A, B, D, E. */
static void
s_fixture(s_plan_t *p)
{
    static const uint32_t labels[] = { 1U, 2U, 3U, 3U, 4U };

    memset(p, 0, sizeof *p);
    p->nodes[0] = s_node(BP_CUT_NODE_PANEL, 0U, 0U, 100000U, 50000U);
    p->nodes[0].first_child = 1U;
    p->nodes[0].child_count = 3U;
    p->nodes[1] = s_node(BP_CUT_NODE_PANEL, 0U, 0U, 100000U, 20000U);
    p->nodes[1].first_child = 4U;
    p->nodes[1].child_count = 3U;
    p->nodes[2] = s_node(BP_CUT_NODE_PANEL, 0U, 20440U, 100000U, 10000U);
    p->nodes[2].first_child = 9U;
    p->nodes[2].child_count = 2U;
    p->nodes[3] = s_node(BP_CUT_NODE_WASTE, 0U, 30880U, 100000U, 19120U);
    p->nodes[4] = s_node(BP_CUT_NODE_ITEM, 0U, 0U, 30000U, 20000U);
    p->nodes[5] = s_node(BP_CUT_NODE_PANEL, 30440U, 0U, 20000U, 20000U);
    p->nodes[5].first_child = 7U;
    p->nodes[5].child_count = 2U;
    p->nodes[6] = s_node(BP_CUT_NODE_WASTE, 50880U, 0U, 49120U, 20000U);
    p->nodes[7] = s_node(BP_CUT_NODE_ITEM, 30440U, 0U, 20000U, 9780U);
    p->nodes[7].item_index = 2U;
    p->nodes[8] = s_node(BP_CUT_NODE_ITEM, 30440U, 10220U, 20000U, 9780U);
    p->nodes[8].item_index = 3U;
    p->nodes[9] = s_node(BP_CUT_NODE_ITEM, 0U, 20440U, 30000U, 10000U);
    p->nodes[9].item_index = 1U;
    p->nodes[10] = s_node(BP_CUT_NODE_WASTE, 30440U, 20440U, 69560U, 10000U);
    p->items[0] = (bp_cut_item_t) { 30000U, 20000U, 1U };
    p->items[1] = (bp_cut_item_t) { 30000U, 10000U, 0U };
    p->items[2] = (bp_cut_item_t) { 20000U, 9780U, 0U };
    p->items[3] = (bp_cut_item_t) { 20000U, 9780U, 0U };
    p->items[4] = (bp_cut_item_t) { 5000U, 5000U, 1U };
    memcpy(p->labels, labels, sizeof labels);
    p->plates[0] = (bp_cut_plate_t) { .root = 0U, .first_axis = BP_CUT_AXIS_X };
    p->plan = (bp_cutplan_t) {
        .settings = {
            .plate_width_10um = 100000U, .plate_height_10um = 50000U,
            .kerf_10um = 440U, .min_waste_10um = 2000U, .max_stages = 3U,
            .first_cut = BP_CUT_FIRST_X
        },
        .nodes = p->nodes, .node_count = 11U,
        .plates = p->plates, .plate_count = 1U,
        .items = p->items, .item_count = 5U
    };
}

/* Checks a plan with its own scratch. */
static bp_cutplan_check_result_t
s_check(s_plan_t *p)
{
    return bp_cutplan_check((bp_cutplan_check_args_t) {
        .plan = &p->plan,
        .scratch_node_seen = p->seen, .scratch_node_capacity = S_MAX_NODES,
        .scratch_item_used = p->used, .scratch_item_capacity = S_MAX_ITEMS
    });
}

/* Writes the cut list to an open stream. */
static bp_cutexport_result_t
s_csv(s_plan_t *p, FILE *out)
{
    return bp_cutexport_csv((bp_cutexport_csv_args_t) {
        .out = out, .plan = &p->plan, .labels = p->labels,
        .scratch_cuts = p->cuts, .scratch_cut_capacity = S_MAX_CUTS });
}

/* Writes one plate's drawing to an open stream. */
static bp_cutexport_result_t
s_svg(s_plan_t *p, uint32_t plate, FILE *out)
{
    return bp_cutexport_svg((bp_cutexport_svg_args_t) {
        .out = out, .plan = &p->plan, .plate = plate, .labels = p->labels,
        .scratch_cuts = p->cuts, .scratch_cut_capacity = S_MAX_CUTS });
}

/*
 * Writes a plan as <stem>.csv and <stem>-001.svg onwards, the names
 * tools/check_cutplan.py --svg looks for.
 */
static void
s_write_files(s_plan_t *p, const char *stem)
{
    char path[160];
    FILE *out;
    uint32_t plate;

    (void) snprintf(path, sizeof path, "%s.csv", stem);
    out = fopen(path, "w");
    CHECK(out != NULL);
    CHECK(s_csv(p, out).status == BP_CUTEXPORT_OK);
    CHECK(fclose(out) == 0);
    for (plate = 0U; plate < p->plan.plate_count; ++plate) {
        (void) snprintf(path, sizeof path, "%s-%03u.svg", stem,
            (unsigned) plate + 1U);
        out = fopen(path, "w");
        CHECK(out != NULL);
        CHECK(s_svg(p, plate, out).status == BP_CUTEXPORT_OK);
        CHECK(fclose(out) == 0);
    }
}

/* Returns what a writer put into a memory stream, NUL terminated. */
static char *
s_capture_csv(s_plan_t *p)
{
    char *text = NULL;
    size_t size = 0U;
    FILE *out = open_memstream(&text, &size);

    CHECK(out != NULL);
    CHECK(s_csv(p, out).status == BP_CUTEXPORT_OK);
    CHECK(fclose(out) == 0);
    return text;
}

/* The fixture's cut list, byte for byte, in the documented format. */
static void
s_test_fixture_csv(void)
{
    static const char expected[] =
        "format,vibe_binpacker-cutlist,1\n"
        "setting,plate_width_mm,1000.00\n"
        "setting,plate_height_mm,500.00\n"
        "setting,trim_left_mm,0.00\n"
        "setting,trim_right_mm,0.00\n"
        "setting,trim_bottom_mm,0.00\n"
        "setting,trim_top_mm,0.00\n"
        "setting,kerf_mm,4.40\n"
        "setting,min_waste_mm,20.00\n"
        "setting,max_stages,3\n"
        "setting,first_cut,x\n"
        "setting,non_exact,0\n"
        "part,1,1,300.00,200.00,1\n"
        "part,2,1,300.00,100.00,0\n"
        "part,3,2,200.00,97.80,0\n"
        "part,4,1,50.00,50.00,1\n"
        "plate,0,x\n"
        "cut,0,1,x,200.00,4.40,0.00,1000.00\n"
        "cut,0,1,x,304.40,4.40,0.00,1000.00\n"
        "cut,0,2,y,300.00,4.40,0.00,200.00\n"
        "cut,0,2,y,504.40,4.40,0.00,200.00\n"
        "cut,0,3,x,97.80,4.40,304.40,504.40\n"
        "cut,0,2,y,300.00,4.40,204.40,304.40\n"
        "item,0,1,0.00,0.00,300.00,200.00,0\n"
        "item,0,3,304.40,0.00,200.00,97.80,0\n"
        "item,0,3,304.40,102.20,200.00,97.80,0\n"
        "item,0,2,0.00,204.40,300.00,100.00,0\n";
    char *text;

    s_fixture(&plan);
    CHECK(s_check(&plan).status == BP_CUTPLAN_OK);
    text = s_capture_csv(&plan);
    if (strcmp(text, expected) != 0) {
        (void) fprintf(stderr, "cut list differs:\n%s", text);
        CHECK(0);
    }
    free(text);
    s_write_files(&plan, S_CASES_DIRECTORY "/fixture");
}

/* The drawing flips y and keeps every value exact. */
static void
s_test_fixture_svg(void)
{
    char *text = NULL;
    size_t size = 0U;
    FILE *out;
    const char *cursor;
    unsigned cuts = 0U;

    s_fixture(&plan);
    out = open_memstream(&text, &size);
    CHECK(out != NULL);
    CHECK(s_svg(&plan, 0U, out).status == BP_CUTEXPORT_OK);
    CHECK(fclose(out) == 0);
    CHECK(strstr(text, "viewBox=\"0 0 1000.00 500.00\" data-plate=\"0\"") != NULL);
    /* Part A at the bottom left of the plate is at the top left of the SVG. */
    CHECK(strstr(text, "data-label=\"1\" data-rotated=\"0\" x=\"0.00\" "
        "y=\"300.00\" width=\"300.00\" height=\"200.00\"") != NULL);
    for (cursor = text; (cursor = strstr(cursor, "class=\"cut\"")) != NULL;
        ++cursor)
        ++cuts;
    CHECK(cuts == 6U);
    free(text);
}

/* A full disk is reported, not ignored. */
static void
s_test_write_failure(void)
{
    FILE *out = fopen("/dev/full", "w");

    s_fixture(&plan);
    CHECK(out != NULL);
    CHECK(s_csv(&plan, out).status == BP_CUTEXPORT_WRITE_FAILED);
    (void) fclose(out);
    out = fopen("/dev/full", "w");
    CHECK(out != NULL);
    CHECK(s_svg(&plan, 0U, out).status == BP_CUTEXPORT_WRITE_FAILED);
    (void) fclose(out);
}

/* Returns a value in [low, high]. */
static uint32_t
s_between(s_plan_t *p, uint32_t low, uint32_t high)
{
    CHECK(low <= high);
    return low + t_random_below(&p->random, high - low + 1U);
}

/* Makes a leaf an item: a new part of exactly its size, maybe turned. */
static void
s_make_item(s_plan_t *p, bp_cut_node_t *node)
{
    bp_cut_item_t *item;
    uint32_t index = p->plan.item_count;

    CHECK(index < S_MAX_ITEMS);
    item = &p->items[index];
    node->kind = BP_CUT_NODE_ITEM;
    node->item_index = index;
    node->rotated = (uint8_t) t_random_below(&p->random, 2U);
    item->width_10um = node->rotated ? node->height_10um : node->width_10um;
    item->height_10um = node->rotated ? node->width_10um : node->height_10um;
    item->may_rotate = node->rotated ? 1U : (uint8_t) t_random_below(
        &p->random, 2U);
    /* An identical neighbour is the same part, so quantities get exercised. */
    p->labels[index] = index > 0U
        && p->items[index - 1U].width_10um == item->width_10um
        && p->items[index - 1U].height_10um == item->height_10um
        && p->items[index - 1U].may_rotate == item->may_rotate
        ? p->labels[index - 1U] : index + 1U;
    ++p->plan.item_count;
}

/*
 * Grows a random sawable subtree under a node whose rectangle is set. Every
 * choice stays inside the rules: children one kerf apart, at most a kerf of
 * dust after the last, waste only where it is wide enough to cut off, panels
 * only where a stage is left, and a sizing cut only as one item plus dust or
 * waste.
 */
static void
s_grow(s_plan_t *p, uint32_t index, uint32_t depth, int waste_ok, int sizing)
{
    const bp_cut_settings_t *settings = &p->plan.settings;
    bp_cut_node_t *node = &p->nodes[index];
    uint32_t limit = settings->max_stages + settings->non_exact;
    uint32_t kerf = settings->kerf_10um;
    bp_cut_axis_t axis;
    uint32_t along;
    uint32_t count;
    uint32_t rest;
    uint32_t free_length;
    uint32_t lengths[4];
    uint32_t position;
    uint32_t child;
    int has_cut;

    if (sizing || depth >= limit || p->plan.node_count + 4U > S_MAX_NODES
        || t_random_below(&p->random, 10U) < 4U) {
        if (waste_ok && (sizing || t_random_below(&p->random, 3U) == 0U))
            node->kind = BP_CUT_NODE_WASTE;
        else
            s_make_item(p, node);
        return;
    }
    axis = (depth + 1U) % 2U == 1U ? p->plates[0].first_axis
        : (p->plates[0].first_axis == BP_CUT_AXIS_X ? BP_CUT_AXIS_Y
            : BP_CUT_AXIS_X);
    along = axis == BP_CUT_AXIS_X ? node->height_10um : node->width_10um;
    /* The last stage before a sizing cut: one item, then dust or waste. */
    sizing = depth + 1U > settings->max_stages;
    count = sizing ? s_between(p, 1U, 2U) : s_between(p, 1U, 4U);
    while (count > 1U && (uint64_t) count + (uint64_t) (count - 1U) * kerf
        + (sizing ? settings->min_waste_10um : 0U) > along)
        --count;
    free_length = along - count - (count - 1U) * kerf;
    for (child = 0U; child < count; ++child)
        lengths[child] = 1U;
    if (sizing && count == 2U) {
        /* The waste piece must be wide enough to cut off; reserve it first. */
        lengths[1] = settings->min_waste_10um > 1U ? settings->min_waste_10um : 1U;
        free_length -= lengths[1] - 1U;
    }
    rest = s_between(p, 0U, free_length < kerf ? free_length : kerf);
    free_length -= rest;
    for (child = 0U; child + 1U < count; ++child) {
        uint32_t extra = s_between(p, 0U, free_length);

        lengths[child] += extra;
        free_length -= extra;
    }
    lengths[count - 1U] += free_length;
    has_cut = count > 1U || rest > 0U;

    node->kind = BP_CUT_NODE_PANEL;
    node->first_child = p->plan.node_count;
    node->child_count = count;
    p->plan.node_count += count;
    position = axis == BP_CUT_AXIS_X ? node->y_10um : node->x_10um;
    for (child = 0U; child < count; ++child) {
        bp_cut_node_t *piece = &p->nodes[node->first_child + child];

        *piece = *node;
        piece->kind = BP_CUT_NODE_WASTE;
        piece->child_count = 0U;
        if (axis == BP_CUT_AXIS_X) {
            piece->y_10um = position;
            piece->height_10um = lengths[child];
        } else {
            piece->x_10um = position;
            piece->width_10um = lengths[child];
        }
        position += lengths[child] + kerf;
    }
    for (child = 0U; child < count; ++child) {
        uint32_t piece = node->first_child + child;
        /* Without a cut nothing is cut off: the parent's width still rules. */
        int wide = has_cut ? lengths[child] >= settings->min_waste_10um
            : waste_ok;

        if (sizing)
            /* First piece the item, the second, if any, waste. */
            s_grow(p, piece, depth + 1U, child == 1U, 1);
        else
            s_grow(p, piece, depth + 1U, wide, 0);
    }
}

/* Builds random settings and plates, each tree grown from its usable area. */
static void
s_random_plan(s_plan_t *p, uint64_t seed)
{
    bp_cut_settings_t *settings = &p->plan.settings;
    uint32_t plate;

    memset(p, 0, sizeof *p);
    p->random = seed;
    p->plan.nodes = p->nodes;
    p->plan.plates = p->plates;
    p->plan.items = p->items;
    settings->plate_width_10um = s_between(p, 1000U, 300000U);
    settings->plate_height_10um = s_between(p, 1000U, 300000U);
    settings->kerf_10um = t_random_below(&p->random, 4U) == 0U ? 0U
        : s_between(p, 1U, 600U);
    if (t_random_below(&p->random, 2U) == 0U) {
        settings->trim_left_10um = s_between(p, 0U, 1500U);
        settings->trim_right_10um = s_between(p, 0U, 1500U);
        settings->trim_bottom_10um = s_between(p, 0U, 1500U);
        settings->trim_top_10um = s_between(p, 0U, 1500U);
    }
    /* Trims that would eat a small plate are not a saw anyone owns. */
    if (settings->trim_left_10um + settings->trim_right_10um + 100U
            > settings->plate_width_10um
        || settings->trim_bottom_10um + settings->trim_top_10um + 100U
            > settings->plate_height_10um) {
        settings->trim_left_10um = settings->trim_right_10um = 0U;
        settings->trim_bottom_10um = settings->trim_top_10um = 0U;
    }
    settings->min_waste_10um = t_random_below(&p->random, 3U) == 0U ? 0U
        : s_between(p, 1U, 3000U);
    settings->max_stages = s_between(p, 1U, 4U);
    settings->first_cut = (bp_cut_first_t) t_random_below(&p->random, 3U);
    settings->non_exact = (uint8_t) t_random_below(&p->random, 2U);
    p->plan.plate_count = s_between(p, 1U, S_MAX_PLATES);

    for (plate = 0U; plate < p->plan.plate_count; ++plate) {
        bp_cut_node_t *root;

        /* Every plate shares the stage axes the grower reads from plate 0. */
        p->plates[plate].first_axis = settings->first_cut == BP_CUT_FIRST_ANY
            ? (plate == 0U ? (bp_cut_axis_t) t_random_below(&p->random, 2U)
                : p->plates[0].first_axis)
            : (bp_cut_axis_t) settings->first_cut;
        p->plates[plate].root = p->plan.node_count;
        root = &p->nodes[p->plan.node_count++];
        *root = s_node(BP_CUT_NODE_WASTE, settings->trim_left_10um,
            settings->trim_bottom_10um,
            settings->plate_width_10um - settings->trim_left_10um
                - settings->trim_right_10um,
            settings->plate_height_10um - settings->trim_bottom_10um
                - settings->trim_top_10um);
        s_grow(p, p->plates[plate].root, 0U, 1, 0);
    }
    /* Ordered parts that were not placed, as a real job would have. */
    while (p->plan.item_count < S_MAX_ITEMS
        && t_random_below(&p->random, 3U) == 0U) {
        uint32_t index = p->plan.item_count++;

        p->items[index] = (bp_cut_item_t) { s_between(p, 1U, 5000U),
            s_between(p, 1U, 5000U), 1U };
        p->labels[index] = index + 1U;
    }
}

/*
 * Random sawable plans: the C check must accept each, the writer must produce
 * files the independent Python check accepts, and one unit of drift anywhere
 * must be refused. The files land in build/cutplan-cases for make test.
 */
static void
s_test_random_plans(void)
{
    uint32_t round;

    for (round = 0U; round < S_RANDOM_PLANS; ++round) {
        uint64_t seed = UINT64_C(0x9e3779b97f4a7c15) * (round + 1U);
        bp_cutplan_check_result_t result;
        char stem[96];
        uint32_t target;

        s_random_plan(&plan, seed);
        result = s_check(&plan);
        if (result.status != BP_CUTPLAN_OK) {
            (void) fprintf(stderr, "round %u: generated plan refused: %s at "
                "plate %u node %u\n", (unsigned) round,
                bp_cutplan_status_text(result.status), (unsigned) result.plate,
                (unsigned) result.node);
            CHECK(0);
        }
        (void) snprintf(stem, sizeof stem, S_CASES_DIRECTORY "/case-%03u",
            (unsigned) round);
        s_write_files(&plan, stem);

        target = t_random_below(&plan.random, plan.plan.node_count);
        plan.nodes[target].x_10um += 1U;
        CHECK(s_check(&plan).status != BP_CUTPLAN_OK);
    }
}

/* Writing a plan the check would refuse is a caller defect. */
static void
s_body_mixed_label_run(void)
{
    s_fixture(&plan);
    plan.labels[1] = 1U;
    (void) s_capture_csv(&plan);
}

/* A plate that does not exist cannot be drawn. */
static void
s_body_missing_plate(void)
{
    s_fixture(&plan);
    (void) s_svg(&plan, 1U, stdout);
}

/* Exercises the cut list and drawing writers against the format. */
int
main(void)
{
    CHECK(mkdir(S_CASES_DIRECTORY, 0755) == 0 || errno == EEXIST);
    s_test_fixture_csv();
    s_test_fixture_svg();
    s_test_write_failure();
    s_test_random_plans();
    t_expect_abort(s_body_mixed_label_run, "mixed label run");
    t_expect_abort(s_body_missing_plate, "missing plate");
    (void) printf("test_bp_cutexport: all checks passed (%u random plans "
        "in " S_CASES_DIRECTORY ")\n", (unsigned) S_RANDOM_PLANS);
    return 0;
}
