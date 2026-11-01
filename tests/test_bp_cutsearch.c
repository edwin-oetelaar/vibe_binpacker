#include "test_support.h"

#include "bp_cutexport.h"
#include "bp_cutplan.h"
#include "bp_cutsearch.h"
#include "bp_guillotine.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#define S_CASES_DIRECTORY "build/cutplan-cases"
#define S_MAX_ITEMS 100U
#define S_MAX_NODES (7U * S_MAX_ITEMS + 2U * S_MAX_ITEMS)
#define S_MAX_CUTS (2U * S_MAX_NODES + 4U * S_MAX_ITEMS)
#define S_JOBS 40U

/* One search job and its caller-owned storage. */
typedef struct {
    bp_cut_settings_t settings;
    bp_cut_item_t items[S_MAX_ITEMS];
    uint32_t labels[S_MAX_ITEMS];
    uint32_t item_count;
    uint32_t max_plates;
    bp_cut_node_t nodes[S_MAX_NODES];
    bp_cut_plate_t plates[S_MAX_ITEMS];
    uint8_t placed[S_MAX_ITEMS];
    uint32_t scratch[22U * S_MAX_ITEMS];
    uint32_t orders[4U * S_MAX_ITEMS];
    uint8_t bias[3U * S_MAX_ITEMS];
    uint8_t seen[S_MAX_NODES];
    uint8_t used[S_MAX_ITEMS];
    bp_cut_t cuts[S_MAX_CUTS];
    bp_cutsearch_result_t result;
    bp_cutplan_t plan;
    uint64_t random;
} s_job_t;

static s_job_t job;
static s_job_t again;

/* Returns the packing arguments over a job's storage. */
static bp_guillotine_args_t
s_pack_args(s_job_t *j)
{
    return (bp_guillotine_args_t) {
        .settings = &j->settings, .items = j->items, .item_count = j->item_count,
        .max_plates = j->max_plates,
        .nodes = j->nodes, .node_capacity = S_MAX_NODES,
        .plates = j->plates, .plate_capacity = S_MAX_ITEMS,
        .placed = j->placed,
        .scratch = j->scratch, .scratch_capacity = 22U * S_MAX_ITEMS
    };
}

/*
 * Searches a job and requires what holds for every search: the plan left in
 * the outputs is sawable and is the one the result describes.
 */
static void
s_search(s_job_t *j, uint32_t iterations, double budget, uint64_t seed)
{
    bp_cutplan_check_result_t check;

    j->result = bp_cutsearch_run((bp_cutsearch_args_t) {
        .pack = s_pack_args(j), .iterations = iterations,
        .time_budget_seconds = budget, .seed = seed,
        .scratch_orders = j->orders, .scratch_order_capacity = 4U * S_MAX_ITEMS,
        .scratch_bias = j->bias, .scratch_bias_capacity = 3U * S_MAX_ITEMS
    });
    j->plan = (bp_cutplan_t) {
        .settings = j->settings,
        .nodes = j->nodes, .node_count = j->result.best.node_count,
        .plates = j->plates, .plate_count = j->result.best.plate_count,
        .items = j->items, .item_count = j->item_count
    };
    check = bp_cutplan_check((bp_cutplan_check_args_t) {
        .plan = &j->plan,
        .scratch_node_seen = j->seen, .scratch_node_capacity = S_MAX_NODES,
        .scratch_item_used = j->used, .scratch_item_capacity = S_MAX_ITEMS
    });
    CHECK(check.status == BP_CUTPLAN_OK);
    CHECK(bp_cutplan_cut_count(&j->plan) == j->result.cut_count);
    /* Never worse than where it started. */
    CHECK(j->result.best.placed_count >= j->result.start_placed_count);
    CHECK(j->result.best.placed_count > j->result.start_placed_count
        || j->result.best.plate_count <= j->result.start_plate_count);
}

/* Writes a searched plan where make test hands it to check_cutplan.py. */
static void
s_write(s_job_t *j, const char *stem)
{
    char path[160];
    FILE *out;
    uint32_t plate;

    (void) snprintf(path, sizeof path, "%s.csv", stem);
    out = fopen(path, "w");
    CHECK(out != NULL);
    CHECK(bp_cutexport_csv((bp_cutexport_csv_args_t) {
        .out = out, .plan = &j->plan, .labels = j->labels,
        .scratch_cuts = j->cuts, .scratch_cut_capacity = S_MAX_CUTS
    }).status == BP_CUTEXPORT_OK);
    CHECK(fclose(out) == 0);
    for (plate = 0U; plate < j->plan.plate_count; ++plate) {
        (void) snprintf(path, sizeof path, "%s-%03u.svg", stem,
            (unsigned) plate + 1U);
        out = fopen(path, "w");
        CHECK(out != NULL);
        CHECK(bp_cutexport_svg((bp_cutexport_svg_args_t) {
            .out = out, .plan = &j->plan, .plate = plate, .labels = j->labels,
            .scratch_cuts = j->cuts, .scratch_cut_capacity = S_MAX_CUTS
        }).status == BP_CUTEXPORT_OK);
        CHECK(fclose(out) == 0);
    }
}

/* Returns a value in [low, high]. */
static uint32_t
s_between(s_job_t *j, uint32_t low, uint32_t high)
{
    return low + t_random_below(&j->random, high - low + 1U);
}

/*
 * A job like Martello & Vigo's class VII on a real saw: mostly long, low
 * parts that a greedy order packs badly, a 3 mm blade, three stages with
 * sizing cuts.
 */
static void
s_random_job(s_job_t *j, uint64_t seed)
{
    uint32_t index;

    memset(j, 0, sizeof *j);
    j->random = seed;
    j->settings = (bp_cut_settings_t) {
        .plate_width_10um = 100000U, .plate_height_10um = 100000U,
        .kerf_10um = 300U, .max_stages = 3U, .first_cut = BP_CUT_FIRST_ANY,
        .non_exact = 1U
    };
    j->max_plates = S_MAX_ITEMS;
    j->item_count = s_between(j, 20U, 60U);
    for (index = 0U; index < j->item_count; ++index) {
        uint32_t roll = t_random_below(&j->random, 10U);

        j->items[index] = roll < 7U
            ? (bp_cut_item_t) { s_between(j, 66000U, 99000U),
                s_between(j, 1000U, 49000U), 0U }
            : (bp_cut_item_t) { s_between(j, 1000U, 49000U),
                s_between(j, 1000U, 49000U), (uint8_t) (roll & 1U) };
        j->labels[index] = index + 1U;
    }
}

/* With fixed rounds and seed the search is the same every time. */
static void
s_test_repeatable(void)
{
    s_random_job(&job, UINT64_C(12345));
    s_search(&job, 150U, 0.0, UINT64_C(77));
    s_random_job(&again, UINT64_C(12345));
    s_search(&again, 150U, 0.0, UINT64_C(77));
    CHECK(job.result.iterations_run == 150U);
    CHECK(memcmp(&job.result.best, &again.result.best, sizeof job.result.best) == 0);
    CHECK(job.result.cut_count == again.result.cut_count);
    CHECK(memcmp(job.nodes, again.nodes,
        job.result.best.node_count * sizeof job.nodes[0]) == 0);
}

/*
 * Over a fixed set of jobs the search saves plates against the default
 * order, and never loses any on a single job.
 */
static void
s_test_saves_plates(void)
{
    uint32_t round;
    uint32_t start = 0U;
    uint32_t searched = 0U;

    for (round = 0U; round < S_JOBS; ++round) {
        s_random_job(&job, UINT64_C(0x5851f42d4c957f2d) * (round + 1U));
        s_search(&job, 200U, 0.0, round + 1U);
        CHECK(job.result.best.status == BP_GUILLOTINE_COMPLETE);
        start += job.result.start_plate_count;
        searched += job.result.best.plate_count;
        if (round % 4U == 0U) {
            char stem[96];

            (void) snprintf(stem, sizeof stem, S_CASES_DIRECTORY "/search-%03u",
                (unsigned) round);
            s_write(&job, stem);
        }
    }
    (void) printf("  %u jobs: %u plates in default order, %u after search\n",
        (unsigned) S_JOBS, (unsigned) start, (unsigned) searched);
    CHECK(searched < start);
}

/* With too few plates, the search places more parts or keeps the count. */
static void
s_test_plate_limit(void)
{
    s_random_job(&job, UINT64_C(999));
    job.max_plates = 2U;
    s_search(&job, 200U, 0.0, 5U);
    CHECK(job.result.best.plate_count <= 2U);
    CHECK(job.result.best.status == BP_GUILLOTINE_PARTIAL);
}

/* A budget that is spent at once still leaves the first plan, intact. */
static void
s_test_time_budget(void)
{
    s_random_job(&job, UINT64_C(4242));
    s_search(&job, UINT32_MAX, 1e-9, 1U);
    CHECK(job.result.iterations_run <= 1U);
}

/* Zero rounds is the packer's own plan. */
static void
s_test_no_rounds(void)
{
    s_random_job(&job, UINT64_C(31337));
    s_search(&job, 0U, 0.0, 1U);
    CHECK(job.result.iterations_run == 0U);
    CHECK(job.result.best.plate_count == job.result.start_plate_count);
}

/* The search owns the order; passing one is a caller defect. */
static void
s_body_order_given(void)
{
    static uint32_t order[S_MAX_ITEMS];
    bp_cutsearch_args_t args;

    s_random_job(&job, 1U);
    args = (bp_cutsearch_args_t) {
        .pack = s_pack_args(&job), .iterations = 1U,
        .scratch_orders = job.orders, .scratch_order_capacity = 4U * S_MAX_ITEMS,
        .scratch_bias = job.bias, .scratch_bias_capacity = 3U * S_MAX_ITEMS
    };
    args.pack.order = order;
    (void) bp_cutsearch_run(args);
}

/* An order that is not a permutation is a caller defect of the packer. */
static void
s_body_bad_order(void)
{
    static uint32_t order[S_MAX_ITEMS];
    bp_guillotine_args_t args;

    s_random_job(&job, 1U);
    args = s_pack_args(&job);
    memset(order, 0, sizeof order);
    args.order = order;
    (void) bp_guillotine_pack(args);
}

/* Too little order scratch is a caller defect. */
static void
s_body_small_scratch(void)
{
    s_random_job(&job, 1U);
    (void) bp_cutsearch_run((bp_cutsearch_args_t) {
        .pack = s_pack_args(&job), .iterations = 1U,
        .scratch_orders = job.orders, .scratch_order_capacity = 3U * job.item_count,
        .scratch_bias = job.bias, .scratch_bias_capacity = 3U * S_MAX_ITEMS
    });
}

/* Exercises the guillotine search. */
int
main(void)
{
    CHECK(mkdir(S_CASES_DIRECTORY, 0755) == 0 || errno == EEXIST);
    s_test_repeatable();
    s_test_saves_plates();
    s_test_plate_limit();
    s_test_time_budget();
    s_test_no_rounds();
    t_expect_abort(s_body_order_given, "order given to search");
    t_expect_abort(s_body_bad_order, "order not a permutation");
    t_expect_abort(s_body_small_scratch, "small order scratch");
    (void) printf("test_bp_cutsearch: all checks passed\n");
    return 0;
}
