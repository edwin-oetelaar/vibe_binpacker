#include "bp_cutsearch.h"

#include "bp_contract.h"
#include "bp_cutplan.h"

#include <string.h>
#include <time.h>

/* Order words per item: current, candidate, best, and the movable list. */
#define S_ORDER_ARRAYS 4U

/* What the plans are compared on, most important first. */
typedef struct {
    uint32_t placed;
    uint32_t plates;
    uint64_t remnant;
    uint32_t cuts;
} s_score_t;

/* The search's working state over the caller's scratch. */
typedef struct {
    bp_cutsearch_args_t args;
    uint32_t count;
    uint32_t *current;
    uint32_t *candidate;
    uint32_t *best;
    /* Items on the last plate or unplaced, in the current plan. */
    uint32_t *movable;
    uint32_t movable_count;
    uint8_t *current_bias;
    uint8_t *candidate_bias;
    uint8_t *best_bias;
    uint64_t random;
} s_search_t;

/* Returns the order scratch words a search over item_count items needs. */
uint32_t
bp_cutsearch_order_words(uint32_t item_count)
{
    BP_REQUIRE(item_count <= UINT32_MAX / S_ORDER_ARRAYS);
    return S_ORDER_ARRAYS * item_count;
}

/* xorshift64star: deterministic, and enough for choosing moves. */
static uint64_t
s_next(s_search_t *search)
{
    uint64_t value;

    BP_REQUIRE(search);
    value = search->random;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    search->random = value;
    return value * UINT64_C(2685821657736338717);
}

/* Returns a value in [0, bound). */
static uint32_t
s_below(s_search_t *search, uint32_t bound)
{
    BP_REQUIRE(bound > 0U);
    return (uint32_t) (s_next(search) % bound);
}

/* Reports whether plan a is strictly better than plan b. */
static int
s_better(s_score_t a, s_score_t b)
{
    if (a.placed != b.placed)
        return a.placed > b.placed;
    if (a.plates != b.plates)
        return a.plates < b.plates;
    if (a.remnant != b.remnant)
        return a.remnant > b.remnant;
    return a.cuts < b.cuts;
}

/* Returns the plan the outputs hold, as bp_cutplan_t. */
static bp_cutplan_t
s_plan(const s_search_t *search, const bp_guillotine_result_t *result)
{
    const bp_guillotine_args_t *pack = &search->args.pack;

    BP_REQUIRE(search && result);
    return (bp_cutplan_t) {
        .settings = *pack->settings,
        .nodes = pack->nodes, .node_count = result->node_count,
        .plates = pack->plates, .plate_count = result->plate_count,
        .items = pack->items, .item_count = pack->item_count
    };
}

/*
 * Returns the full strip left on the last plate: the root's last child when
 * it is waste. Children of a root span it, so that piece is one stage 1 cut
 * away from going back on the rack.
 */
static uint64_t
s_remnant(const s_search_t *search, const bp_guillotine_result_t *result)
{
    const bp_guillotine_args_t *pack = &search->args.pack;
    const bp_cut_node_t *root;
    const bp_cut_node_t *last;

    BP_REQUIRE(search && result);
    if (result->plate_count == 0U)
        return 0U;
    root = &pack->nodes[pack->plates[result->plate_count - 1U].root];
    if (root->kind != BP_CUT_NODE_PANEL)
        return 0U;
    last = &pack->nodes[root->first_child + root->child_count - 1U];
    return last->kind == BP_CUT_NODE_WASTE
        ? (uint64_t) last->width_10um * last->height_10um : 0U;
}

/* Packs with an order and biases into the outputs and scores the plan. */
static s_score_t
s_decode(s_search_t *search, const uint32_t *order, const uint8_t *bias,
    bp_guillotine_result_t *result)
{
    bp_guillotine_args_t pack;
    bp_cutplan_t plan;
    s_score_t score;

    BP_REQUIRE(search && order && bias && result);
    pack = search->args.pack;
    pack.order = order;
    pack.turn_bias = bias;
    *result = bp_guillotine_pack(pack);
    plan = s_plan(search, result);
    score.placed = result->placed_count;
    score.plates = result->plate_count;
    score.remnant = s_remnant(search, result);
    score.cuts = bp_cutplan_cut_count(&plan);
    return score;
}

/* Collects the items a targeted move may pull forward. */
static void
s_collect_movable(s_search_t *search, const bp_guillotine_result_t *result)
{
    const bp_guillotine_args_t *pack = &search->args.pack;
    uint32_t first_node = 0U;
    uint32_t index;

    BP_REQUIRE(search && result);
    search->movable_count = 0U;
    /* The last plate's nodes are the last ones emitted, from its root on. */
    if (result->plate_count > 0U)
        first_node = pack->plates[result->plate_count - 1U].root;
    for (index = first_node; index < result->node_count; ++index)
        if (pack->nodes[index].kind == BP_CUT_NODE_ITEM)
            search->movable[search->movable_count++] =
                pack->nodes[index].item_index;
    for (index = 0U; index < search->count; ++index)
        if (!pack->placed[index])
            search->movable[search->movable_count++] = index;
}

/* Returns where an item sits in an order. */
static uint32_t
s_position(const uint32_t *order, uint32_t count, uint32_t item)
{
    uint32_t index;

    for (index = 0U; index < count; ++index)
        if (order[index] == item)
            return index;
    BP_REQUIRE(0);
    return 0U;
}

/* Moves the entry at from to to, shifting the ones between. */
static void
s_move(uint32_t *order, uint32_t from, uint32_t to)
{
    uint32_t item = order[from];

    if (from < to)
        memmove(order + from, order + from + 1U,
            (size_t) (to - from) * sizeof *order);
    else if (to < from)
        memmove(order + to + 1U, order + to,
            (size_t) (from - to) * sizeof *order);
    order[to] = item;
}

/* Applies one random move to the candidate. */
static void
s_perturb(s_search_t *search)
{
    uint32_t count = search->count;
    uint32_t kind = s_below(search, 4U);

    if (kind == 2U && search->movable_count > 0U) {
        /* Pull an item from the last plate, or an unplaced one, forward. */
        uint32_t item = search->movable[s_below(search, search->movable_count)];
        uint32_t from = s_position(search->candidate, count, item);

        if (from > 0U) {
            s_move(search->candidate, from, s_below(search, from));
            return;
        }
        kind = 1U;
    }
    if (kind == 3U) {
        uint32_t item = s_below(search, count);

        if (search->args.pack.items[item].may_rotate) {
            search->candidate_bias[item] ^= 1U;
            return;
        }
        kind = 0U;
    }
    if (kind == 0U || kind == 2U) {
        uint32_t a = s_below(search, count);
        uint32_t b = s_below(search, count);
        uint32_t swap = search->candidate[a];

        search->candidate[a] = search->candidate[b];
        search->candidate[b] = swap;
        return;
    }
    s_move(search->candidate, s_below(search, count), s_below(search, count));
}

/* Returns the CPU seconds since started. */
static double
s_elapsed(clock_t started)
{
    return (double) (clock() - started) / CLOCKS_PER_SEC;
}

/* Searches for a better plan and leaves the best one in the outputs. */
bp_cutsearch_result_t
bp_cutsearch_run(bp_cutsearch_args_t args)
{
    bp_cutsearch_result_t result;
    s_search_t search;
    bp_guillotine_result_t packed;
    s_score_t current;
    s_score_t best;
    clock_t started = clock();
    uint32_t count = args.pack.item_count;
    uint32_t round;

    BP_REQUIRE(args.pack.order == NULL && args.pack.turn_bias == NULL);
    BP_REQUIRE(args.pack.items || count == 0U);
    BP_REQUIRE(args.scratch_orders || count == 0U);
    BP_REQUIRE(args.scratch_order_capacity >= bp_cutsearch_order_words(count));
    BP_REQUIRE(args.scratch_bias || count == 0U);
    BP_REQUIRE(count <= UINT32_MAX / 3U);
    BP_REQUIRE(args.scratch_bias_capacity >= 3U * count);
    BP_REQUIRE(args.time_budget_seconds >= 0.0);

    memset(&result, 0, sizeof result);
    if (count == 0U) {
        result.best = bp_guillotine_pack(args.pack);
        return result;
    }
    search = (s_search_t) {
        .args = args, .count = count,
        .current = args.scratch_orders,
        .candidate = args.scratch_orders + count,
        .best = args.scratch_orders + 2U * count,
        .movable = args.scratch_orders + 3U * count,
        .current_bias = args.scratch_bias,
        .candidate_bias = args.scratch_bias + count,
        .best_bias = args.scratch_bias + 2U * count,
        /* A zero state would stay zero. */
        .random = args.seed ? args.seed : UINT64_C(0x9e3779b97f4a7c15)
    };
    bp_guillotine_default_order(args.pack.items, count, search.current);
    memset(search.current_bias, 0, count);
    current = s_decode(&search, search.current, search.current_bias, &packed);
    s_collect_movable(&search, &packed);
    best = current;
    memcpy(search.best, search.current, count * sizeof *search.best);
    memcpy(search.best_bias, search.current_bias, count);
    result.start_plate_count = packed.plate_count;
    result.start_placed_count = packed.placed_count;

    for (round = 0U; round < args.iterations; ++round) {
        s_score_t candidate;

        if (args.time_budget_seconds > 0.0
            && s_elapsed(started) >= args.time_budget_seconds)
            break;
        memcpy(search.candidate, search.current, count * sizeof *search.candidate);
        memcpy(search.candidate_bias, search.current_bias, count);
        s_perturb(&search);
        candidate = s_decode(&search, search.candidate, search.candidate_bias,
            &packed);
        ++result.iterations_run;
        if (s_better(current, candidate))
            continue;
        /* Not worse: walk on, also across plateaus. */
        current = candidate;
        memcpy(search.current, search.candidate, count * sizeof *search.current);
        memcpy(search.current_bias, search.candidate_bias, count);
        s_collect_movable(&search, &packed);
        if (s_better(candidate, best)) {
            best = candidate;
            memcpy(search.best, search.candidate, count * sizeof *search.best);
            memcpy(search.best_bias, search.candidate_bias, count);
            ++result.improvements;
        }
    }

    /* Leave the best plan in the outputs; the decoder is deterministic. */
    best = s_decode(&search, search.best, search.best_bias, &result.best);
    result.cut_count = best.cuts;
    result.remnant_area_10um2 = best.remnant;
    result.seconds = s_elapsed(started);
    return result;
}
