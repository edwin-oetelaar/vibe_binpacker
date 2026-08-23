#include "bp_score.h"

#include "bp_contract.h"

/*
 * Plain bottom-left by default, because that is what measured best.
 *
 * Any positive contact weight overwhelms the left penalty and turns the rule
 * into "hug whatever you can reach", which on an over-subscribed sheet placed
 * five fewer parts than bottom-left. The term is kept and exposed because it
 * is the classic heuristic and may well earn its place with a waste measure
 * beside it, but it does not earn a nonzero default on the evidence here.
 */
bp_score_weights_t
bp_score_default_weights(void)
{
    bp_score_weights_t weights;

    weights.contact_weight = 0.0;
    weights.height_penalty = 1.0;
    weights.left_penalty = 0.001;
    return weights;
}

/* xorshift64star, so a blinking run replays exactly from its seed. */
static uint64_t
s_random_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

/* Reports whether this candidate should be passed over. */
static int
s_blinks(const bp_score_best_args_t *args)
{
    if (args->blink_rate <= 0.0)
        return 0;
    return (double) (s_random_next(args->random_state) >> 11)
        / 9007199254740992.0 < args->blink_rate;
}

/* Returns the contact a part fully surrounded on every side would have. */
uint32_t
bp_score_contact_capacity(const bp_mask_t *mask)
{
    uint32_t row;
    uint32_t capacity = 0U;

    bp_mask_require_valid(mask);
    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index)
            capacity += 2U + 2U * mask->spans[index].length_cells;
    }
    return capacity;
}

/* Counts occupied cells in a sheet span, treating outside the sheet as solid. */
static uint32_t
s_solid_count(const bp_bitmap_t *sheet, int64_t y, uint32_t x, uint32_t length)
{
    if (y < 0 || y >= (int64_t) sheet->height_cells)
        return length;
    return bp_bitmap_span_count(sheet, (uint32_t) y, x, length);
}

/* Counts the part's boundary cells that meet material or the sheet edge. */
uint32_t
bp_score_contact(const bp_bitmap_t *sheet, const bp_mask_t *mask, uint32_t x,
    uint32_t y)
{
    uint32_t row;
    uint32_t contact = 0U;

    BP_REQUIRE(bp_mask_in_bounds(sheet, mask, x, y));

    for (row = 0U; row < mask->height_cells; ++row) {
        uint32_t index;

        for (index = mask->row_offsets[row]; index < mask->row_offsets[row + 1U];
            ++index) {
            const bp_span_t *span = &mask->spans[index];
            uint32_t left = x + span->x_cells;
            uint32_t right = left + span->length_cells;

            /* The cell beyond each end of the run, or the sheet edge. */
            if (left == 0U || bp_bitmap_get(sheet, left - 1U, y + row))
                ++contact;
            if (right == sheet->width_cells
                || bp_bitmap_get(sheet, right, y + row))
                ++contact;

            /*
             * Cells under and over the run. Anything solid there belongs to
             * another part, because a free position means the sheet is clear
             * everywhere the part itself covers.
             */
            contact += s_solid_count(sheet, (int64_t) y + row - 1,
                left, span->length_cells);
            contact += s_solid_count(sheet, (int64_t) y + row + 1,
                left, span->length_cells);
        }
    }
    return contact;
}

/*
 * Ranks one candidate; larger is better. Height is penalised on its absolute
 * value, not relative to the lowest usable row, so scores stay comparable
 * between two orientations that reach different heights.
 */
static double
s_score(const bp_score_best_args_t *args, uint32_t x, uint32_t y,
    uint32_t contact_capacity)
{
    double fraction = (double) bp_score_contact(args->sheet, args->mask, x, y)
        / (double) contact_capacity;

    return args->weights.contact_weight * fraction
        - args->weights.height_penalty * (double) y
        - args->weights.left_penalty * (double) x;
}

/* Finds the best-scoring position for the mask. */
bp_score_best_result_t
bp_score_best_position(bp_score_best_args_t args)
{
    bp_score_best_result_t result = { 0, 0U, 0U, 0.0, 0U };
    bp_place_result_t lowest;
    uint32_t contact_capacity;
    uint32_t max_y;
    uint32_t last_y;
    uint32_t y;

    BP_REQUIRE(args.sheet);
    BP_REQUIRE(args.height_window > 0U);
    BP_REQUIRE(args.max_positions > 0U);
    bp_mask_require_valid(args.mask);

    if (!bp_mask_in_bounds(args.sheet, args.mask, 0U, 0U))
        return result;

    /*
     * Find the lowest usable height with the jump scan and enumerate
     * positions with the erosion. That split is what the benchmarks say to
     * do: a scan that usually rejects a candidate on its first word is the
     * cheapest way to answer "is there room down here at all", and erosion is
     * the cheapest way to answer "and where exactly", once the answer to the
     * first is yes.
     */
    lowest = bp_place_bottom_left((bp_place_args_t) {
        .sheet = args.sheet,
        .mask = args.mask,
        .mode = BP_PLACE_JUMP,
        .scratch = args.scratch,
        .scratch_capacity = args.scratch_capacity
    });
    if (!lowest.found)
        return result;

    /*
     * Nothing to rank when the caller only wants the bottom-left position.
     * The score still has to be filled in: a caller choosing between two
     * orientations compares scores, and leaving them all equal would silently
     * pin every part to its first orientation.
     */
    if (args.weights.contact_weight == 0.0 && args.height_window == 1U
        && args.blink_rate <= 0.0) {
        result.found = 1;
        result.x_cells = lowest.x_cells;
        result.y_cells = lowest.y_cells;
        result.score = -args.weights.height_penalty * (double) lowest.y_cells
            - args.weights.left_penalty * (double) lowest.x_cells;
        result.positions_examined = 1U;
        return result;
    }

    if (args.blink_rate > 0.0)
        BP_REQUIRE(args.random_state);
    contact_capacity = bp_score_contact_capacity(args.mask);
    BP_REQUIRE(contact_capacity > 0U);
    max_y = args.sheet->height_cells - args.mask->height_cells;
    last_y = lowest.y_cells + args.height_window - 1U;
    if (last_y > max_y || last_y < lowest.y_cells)
        last_y = max_y;

    for (y = lowest.y_cells; y <= last_y; ++y) {
        bp_place_feasible_result_t feasible;
        uint32_t stride_step;
        uint32_t taken = 0U;
        uint32_t x = 0U;

        feasible = bp_place_feasible_row((bp_place_feasible_args_t) {
            .sheet = args.sheet,
            .mask = args.mask,
            .y_cells = y,
            .scratch = args.scratch,
            .scratch_capacity = args.scratch_capacity
        });
        if (feasible.count == 0U)
            continue;

        /*
         * Rank a bounded sample spread across the row rather than all of it.
         * The leftmost position is always among them, so ranking can never do
         * worse than bottom-left for want of looking.
         */
        stride_step = feasible.count > args.max_positions
            ? feasible.count / args.max_positions
            : 1U;

        while (bp_row_next_set(feasible.positions, feasible.stride_words, x,
            &x)) {
            double score;

            ++taken;
            if (s_blinks(&args)) {
                if (taken >= args.max_positions)
                    break;
                x += stride_step;
                if (x >= args.sheet->width_cells)
                    break;
                continue;
            }

            score = s_score(&args, x, y, contact_capacity);
            ++result.positions_examined;
            if (!result.found || score > result.score) {
                result.found = 1;
                result.score = score;
                result.x_cells = x;
                result.y_cells = y;
            }
            if (taken >= args.max_positions)
                break;
            x += stride_step;
            if (x >= args.sheet->width_cells)
                break;
        }
    }

    /*
     * Blinking can pass over every candidate it saw. The jump scan already
     * proved a position exists, so fall back to it rather than reporting that
     * a part does not fit when it plainly does.
     */
    if (!result.found) {
        result.found = 1;
        result.x_cells = lowest.x_cells;
        result.y_cells = lowest.y_cells;
        result.score = -args.weights.height_penalty * (double) lowest.y_cells;
    }
    return result;
}
