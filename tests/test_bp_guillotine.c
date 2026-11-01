#include "test_support.h"

#include "bp_cutexport.h"
#include "bp_cutplan.h"
#include "bp_guillotine.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#define S_CASES_DIRECTORY "build/cutplan-cases"
#define S_MAX_ITEMS 120U
#define S_MAX_PLATES S_MAX_ITEMS
#define S_MAX_NODES (7U * S_MAX_ITEMS + 2U * S_MAX_PLATES)
#define S_MAX_CUTS (2U * S_MAX_NODES + 4U * S_MAX_PLATES)
#define S_RANDOM_JOBS 300U

/* One packing job and everything its result needs, caller owned. */
typedef struct {
    bp_cut_settings_t settings;
    bp_cut_item_t items[S_MAX_ITEMS];
    uint32_t labels[S_MAX_ITEMS];
    uint32_t item_count;
    uint32_t max_plates;
    bp_cut_node_t nodes[S_MAX_NODES];
    bp_cut_plate_t plates[S_MAX_PLATES];
    uint8_t placed[S_MAX_ITEMS];
    uint32_t scratch[22U * S_MAX_ITEMS];
    bp_guillotine_result_t result;
    bp_cutplan_t plan;
    uint8_t seen[S_MAX_NODES];
    uint8_t used[S_MAX_ITEMS];
    bp_cut_t cuts[S_MAX_CUTS];
    uint64_t random;
} s_job_t;

static s_job_t job;
static s_job_t again;

/* Starts a job on a plate of the given size with an exact three-stage saw. */
static void
s_init(s_job_t *j, uint32_t width, uint32_t height, uint32_t kerf)
{
    memset(j, 0, sizeof *j);
    j->settings = (bp_cut_settings_t) {
        .plate_width_10um = width, .plate_height_10um = height,
        .kerf_10um = kerf, .max_stages = 3U, .first_cut = BP_CUT_FIRST_ANY
    };
    j->max_plates = S_MAX_PLATES;
}

/* Orders count more of one part. */
static void
s_add(s_job_t *j, uint32_t count, uint32_t width, uint32_t height,
    uint8_t may_rotate)
{
    uint32_t copy;

    for (copy = 0U; copy < count; ++copy) {
        CHECK(j->item_count < S_MAX_ITEMS);
        j->items[j->item_count] = (bp_cut_item_t) { width, height, may_rotate };
        j->labels[j->item_count] = j->item_count + 1U;
        ++j->item_count;
    }
}

/*
 * Packs a job and requires what holds for every result: the plan passes the
 * check, the placed flags agree with the tree, the counts add up.
 */
static void
s_pack(s_job_t *j)
{
    bp_cutplan_check_result_t check;
    uint32_t index;
    uint32_t items_in_tree = 0U;
    uint32_t placed = 0U;

    j->result = bp_guillotine_pack((bp_guillotine_args_t) {
        .settings = &j->settings, .items = j->items, .item_count = j->item_count,
        .max_plates = j->max_plates,
        .nodes = j->nodes, .node_capacity = S_MAX_NODES,
        .plates = j->plates, .plate_capacity = S_MAX_PLATES,
        .placed = j->placed,
        .scratch = j->scratch, .scratch_capacity = 22U * S_MAX_ITEMS
    });
    j->plan = (bp_cutplan_t) {
        .settings = j->settings,
        .nodes = j->nodes, .node_count = j->result.node_count,
        .plates = j->plates, .plate_count = j->result.plate_count,
        .items = j->items, .item_count = j->item_count
    };
    check = bp_cutplan_check((bp_cutplan_check_args_t) {
        .plan = &j->plan,
        .scratch_node_seen = j->seen, .scratch_node_capacity = S_MAX_NODES,
        .scratch_item_used = j->used, .scratch_item_capacity = S_MAX_ITEMS
    });
    if (check.status != BP_CUTPLAN_OK) {
        (void) fprintf(stderr, "packed plan refused: %s at plate %u node %u\n",
            bp_cutplan_status_text(check.status), (unsigned) check.plate,
            (unsigned) check.node);
        CHECK(0);
    }
    for (index = 0U; index < j->result.node_count; ++index)
        if (j->nodes[index].kind == BP_CUT_NODE_ITEM) {
            CHECK(j->placed[j->nodes[index].item_index] == 1U);
            ++items_in_tree;
        }
    for (index = 0U; index < j->item_count; ++index)
        placed += j->placed[index];
    CHECK(items_in_tree == placed && placed == j->result.placed_count);
    CHECK(j->result.plate_count <= j->max_plates);
    CHECK(j->result.oversized_count <= j->item_count - placed);
    CHECK((j->result.status == BP_GUILLOTINE_COMPLETE) == (placed == j->item_count));
    CHECK(j->result.node_count
        <= bp_guillotine_node_capacity(j->item_count, j->max_plates));
}

/* Writes a packed job where make test hands it to tools/check_cutplan.py. */
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

/* Parts that tile the plate exactly fill one plate, with no cut wasted. */
static void
s_test_exact_fill(void)
{
    s_init(&job, 100000U, 50000U, 0U);
    s_add(&job, 8U, 25000U, 25000U, 0U);
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_COMPLETE);
    CHECK(job.result.plate_count == 1U);
    s_write(&job, S_CASES_DIRECTORY "/pack-exact");
}

/*
 * The blade costs material: four 250 mm squares do not fit 1000 mm with
 * three 4.4 mm cuts between them, nor two rows into 500 mm.
 */
static void
s_test_kerf_costs_material(void)
{
    s_init(&job, 100000U, 50000U, 440U);
    s_add(&job, 8U, 25000U, 25000U, 0U);
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_COMPLETE);
    CHECK(job.result.plate_count == 3U);
    s_write(&job, S_CASES_DIRECTORY "/pack-kerf");

    /* Out of plates: a valid partial result, nothing oversized. */
    job.max_plates = 2U;
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_PARTIAL);
    CHECK(job.result.plate_count == 2U && job.result.placed_count == 6U);
    CHECK(job.result.oversized_count == 0U);
}

/* A part larger than the plate is reported, unless turning it helps. */
static void
s_test_oversized_and_turned(void)
{
    s_init(&job, 100000U, 130000U, 0U);
    s_add(&job, 1U, 120000U, 1000U, 0U);
    s_add(&job, 1U, 1000U, 1000U, 0U);
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_PARTIAL);
    CHECK(job.result.oversized_count == 1U && job.placed[1] == 1U);
    CHECK(job.placed[0] == 0U);

    job.items[0].may_rotate = 1U;
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_COMPLETE);
    CHECK(job.result.oversized_count == 0U);
}

/*
 * A leftover too narrow to cut off and wider than the blade cannot be left,
 * so a part that would leave one does not go there.
 */
static void
s_test_minimum_waste(void)
{
    s_init(&job, 100000U, 50000U, 0U);
    job.settings.min_waste_10um = 2000U;
    s_add(&job, 1U, 99000U, 50000U, 0U);
    s_pack(&job);
    CHECK(job.result.placed_count == 0U && job.result.oversized_count == 1U);

    job.settings.min_waste_10um = 1000U;
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_COMPLETE);

    /* Within one blade width the rest is sawdust, which is always fine. */
    s_init(&job, 100000U, 50000U, 440U);
    job.settings.min_waste_10um = 2000U;
    s_add(&job, 1U, 99600U, 50000U, 0U);
    s_pack(&job);
    CHECK(job.result.status == BP_GUILLOTINE_COMPLETE);
}

/*
 * What each saw can do: one stage cuts only full-length strips, two cannot
 * stack, three exact cannot cut an item narrower; non-exact and a fourth
 * stage can.
 */
static void
s_test_stages(void)
{
    /*
     * Two parts of different size on one plate, cut along x first. One stage
     * needs full-length strips, so only the sizing cut gets them on; two
     * stages put them in separate strips; three and four stack freely.
     */
    s_init(&job, 100000U, 50000U, 0U);
    s_add(&job, 1U, 40000U, 30000U, 0U);
    s_add(&job, 1U, 30000U, 20000U, 0U);
    job.settings.first_cut = BP_CUT_FIRST_X;
    job.max_plates = 1U;

    job.settings.max_stages = 1U;
    s_pack(&job);
    CHECK(job.result.placed_count == 0U);
    job.settings.non_exact = 1U;
    s_pack(&job);
    CHECK(job.result.placed_count == 2U);

    job.settings.non_exact = 0U;
    job.settings.max_stages = 2U;
    s_pack(&job);
    CHECK(job.result.placed_count == 2U);

    job.settings.max_stages = 3U;
    s_pack(&job);
    CHECK(job.result.placed_count == 2U);
    job.settings.max_stages = 4U;
    s_pack(&job);
    CHECK(job.result.placed_count == 2U);
    s_write(&job, S_CASES_DIRECTORY "/pack-stages");
}

/* Fixed first cut directions are kept on every plate. */
static void
s_test_first_cut(void)
{
    uint32_t plate;

    s_init(&job, 100000U, 50000U, 300U);
    s_add(&job, 6U, 45000U, 20000U, 1U);
    job.settings.first_cut = BP_CUT_FIRST_Y;
    s_pack(&job);
    for (plate = 0U; plate < job.result.plate_count; ++plate)
        CHECK(job.plates[plate].first_axis == BP_CUT_AXIS_Y);
}

/* Returns a value in [low, high]. */
static uint32_t
s_between(s_job_t *j, uint32_t low, uint32_t high)
{
    CHECK(low <= high);
    return low + t_random_below(&j->random, high - low + 1U);
}

/* Builds a random job: any saw, any stock, parts of mixed sizes. */
static void
s_random_job(s_job_t *j, uint64_t seed)
{
    uint32_t count;
    uint32_t width;
    uint32_t height;

    memset(j, 0, sizeof *j);
    j->random = seed;
    width = s_between(j, 20000U, 300000U);
    height = s_between(j, 20000U, 300000U);
    j->settings = (bp_cut_settings_t) {
        .plate_width_10um = width, .plate_height_10um = height,
        .kerf_10um = t_random_below(&j->random, 4U) == 0U ? 0U
            : s_between(j, 1U, 600U),
        .min_waste_10um = t_random_below(&j->random, 3U) == 0U ? 0U
            : s_between(j, 1U, 3000U),
        .max_stages = s_between(j, 1U, 4U),
        .first_cut = (bp_cut_first_t) t_random_below(&j->random, 3U),
        .non_exact = (uint8_t) t_random_below(&j->random, 2U)
    };
    if (t_random_below(&j->random, 2U) == 0U) {
        j->settings.trim_left_10um = s_between(j, 0U, 1500U);
        j->settings.trim_right_10um = s_between(j, 0U, 1500U);
        j->settings.trim_bottom_10um = s_between(j, 0U, 1500U);
        j->settings.trim_top_10um = s_between(j, 0U, 1500U);
    }
    j->max_plates = t_random_below(&j->random, 4U) == 0U
        ? s_between(j, 1U, 3U) : S_MAX_PLATES;
    count = s_between(j, 1U, S_MAX_ITEMS);
    while (j->item_count < count) {
        /* Mostly parts that fit, some that tile, a few that never fit. */
        uint32_t kind = t_random_below(&j->random, 10U);
        uint32_t part_width = kind == 0U ? s_between(j, 1U, width + 5000U)
            : kind < 3U ? width / s_between(j, 1U, 6U)
            : s_between(j, 100U, width / 2U);
        uint32_t part_height = kind == 0U ? s_between(j, 1U, height + 5000U)
            : s_between(j, 100U, height / 2U);
        uint32_t copies = s_between(j, 1U, 4U);

        if (part_width == 0U)
            part_width = 1U;
        if (copies > count - j->item_count)
            copies = count - j->item_count;
        s_add(j, copies, part_width, part_height,
            (uint8_t) t_random_below(&j->random, 2U));
    }
}

/*
 * Random jobs on random saws: every result is sawable by the C check, is the
 * same when packed twice, and never beats the area bound. A third of them go
 * to build/cutplan-cases for the independent Python check.
 */
static void
s_test_random_jobs(void)
{
    uint32_t round;

    for (round = 0U; round < S_RANDOM_JOBS; ++round) {
        uint64_t seed = UINT64_C(0x2545f4914f6cdd1d) * (round + 3U);
        uint64_t usable;
        uint64_t area = 0U;
        uint32_t index;

        s_random_job(&job, seed);
        s_pack(&job);
        s_random_job(&again, seed);
        s_pack(&again);
        CHECK(memcmp(&job.result, &again.result, sizeof job.result) == 0);
        CHECK(memcmp(job.nodes, again.nodes,
            job.result.node_count * sizeof job.nodes[0]) == 0);
        CHECK(memcmp(job.plates, again.plates,
            job.result.plate_count * sizeof job.plates[0]) == 0);

        usable = (uint64_t) (job.settings.plate_width_10um
                - job.settings.trim_left_10um - job.settings.trim_right_10um)
            * (job.settings.plate_height_10um - job.settings.trim_bottom_10um
                - job.settings.trim_top_10um);
        for (index = 0U; index < job.item_count; ++index)
            if (job.placed[index])
                area += (uint64_t) job.items[index].width_10um
                    * job.items[index].height_10um;
        CHECK(area <= usable * job.result.plate_count);

        if (round % 3U == 0U) {
            char stem[96];

            (void) snprintf(stem, sizeof stem, S_CASES_DIRECTORY "/pack-%03u",
                (unsigned) round);
            s_write(&job, stem);
        }
    }
}

/* An empty order is complete at once, on no plate. */
static void
s_test_empty(void)
{
    bp_guillotine_result_t result;

    s_init(&job, 1000U, 1000U, 0U);
    result = bp_guillotine_pack((bp_guillotine_args_t) {
        .settings = &job.settings, .max_plates = 1U });
    CHECK(result.status == BP_GUILLOTINE_COMPLETE);
    CHECK(result.plate_count == 0U && result.node_count == 0U);
}

/* Too little node storage is a caller defect. */
static void
s_body_small_nodes(void)
{
    s_init(&job, 100000U, 50000U, 0U);
    s_add(&job, 4U, 1000U, 1000U, 0U);
    (void) bp_guillotine_pack((bp_guillotine_args_t) {
        .settings = &job.settings, .items = job.items, .item_count = 4U,
        .max_plates = 4U, .nodes = job.nodes, .node_capacity = 10U,
        .plates = job.plates, .plate_capacity = 4U, .placed = job.placed,
        .scratch = job.scratch, .scratch_capacity = 22U * 4U });
}

/* No plates at all is a caller defect. */
static void
s_body_no_plates(void)
{
    (void) bp_guillotine_node_capacity(4U, 0U);
}

/* Exercises the constructive guillotine packer. */
int
main(void)
{
    CHECK(mkdir(S_CASES_DIRECTORY, 0755) == 0 || errno == EEXIST);
    s_test_exact_fill();
    s_test_kerf_costs_material();
    s_test_oversized_and_turned();
    s_test_minimum_waste();
    s_test_stages();
    s_test_first_cut();
    s_test_empty();
    s_test_random_jobs();
    t_expect_abort(s_body_small_nodes, "small node buffer");
    t_expect_abort(s_body_no_plates, "no plates");
    (void) printf("test_bp_guillotine: all checks passed (%u random jobs)\n",
        (unsigned) S_RANDOM_JOBS);
    return 0;
}
