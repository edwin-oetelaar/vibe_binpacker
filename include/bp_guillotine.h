#pragma once

#include "bp_cutplan.h"

#include <stdint.h>

/*
 * Constructive guillotine packing: builds a sawable bp_cutplan, plate by
 * plate, the way a panel saw works a plate.
 *
 * Every plate is built from three levels. Stage 1 cuts the usable area into
 * strips across the full plate; the first piece of a strip sets its height.
 * Stage 2 cuts a strip into stacks; the first piece of a stack sets its width.
 * Stage 3 cuts a stack into pieces stacked along it. How much of that a plan
 * may use follows the saw: with one stage a strip holds one item, with two a
 * stack holds one item, from three on stacks hold several. An item narrower
 * than its stack needs a fourth stage or, on a three stage saw, the sizing cut
 * of non_exact; an item shorter than its strip on a two stage saw likewise.
 *
 * Every leftover the plan leaves is 0, at most one kerf (sawdust), or wide
 * enough to cut off as waste: a placement that would leave anything in
 * between is not made. That is how the minimum waste rule holds without
 * repair afterwards.
 *
 * Items are taken largest side first. For each plate the packer builds every
 * variant the saw allows, the first cut along x and along y when first_cut
 * is any, and with strip openers standing up or lying down, and keeps the
 * one that places the most area. Ties keep the larger strip of plate left
 * over at the end, which is the remnant a saw shop puts back on the rack.
 * The result is deterministic.
 *
 * The order and the turn preferences can come from the caller instead, which
 * is how bp_cutsearch uses this packer as the decoder of its search.
 */

typedef enum {
    /* Every item is placed. */
    BP_GUILLOTINE_COMPLETE = 0,
    /* Some items are not: too large for an empty plate, or out of plates. */
    BP_GUILLOTINE_PARTIAL = 1
} bp_guillotine_status_t;

typedef struct {
    const bp_cut_settings_t *settings;
    const bp_cut_item_t *items;
    uint32_t item_count;
    /* Most plates to use, at least 1. */
    uint32_t max_plates;
    /*
     * Optional: the order items are taken in, a permutation of 0 to
     * item_count - 1. NULL uses bp_guillotine_default_order.
     */
    const uint32_t *order;
    /*
     * Optional, per item: 1 makes the packer try the turn it would otherwise
     * not prefer, wherever it picks a turn. NULL biases nothing.
     */
    const uint8_t *turn_bias;
    /* Written: the plan's trees and plates. */
    bp_cut_node_t *nodes;
    uint32_t node_capacity;
    bp_cut_plate_t *plates;
    uint32_t plate_capacity;
    /* Written: 1 per item that was placed, 0 otherwise. */
    uint8_t *placed;
    /* Working storage, bp_guillotine_scratch_words(item_count) words. */
    uint32_t *scratch;
    uint32_t scratch_capacity;
} bp_guillotine_args_t;

typedef struct {
    bp_guillotine_status_t status;
    uint32_t placed_count;
    uint32_t plate_count;
    uint32_t node_count;
    /* Items no orientation of fits an empty plate at all. */
    uint32_t oversized_count;
} bp_guillotine_result_t;

/* Returns the node capacity a packing of item_count items always fits in. */
uint32_t
bp_guillotine_node_capacity(uint32_t item_count, uint32_t max_plates);

/* Returns the plate capacity a packing can use: never more than its items. */
uint32_t
bp_guillotine_plate_capacity(uint32_t item_count, uint32_t max_plates);

/*
 * Writes the default order: longest side first, then shortest side first,
 * then by index. order must hold count entries.
 */
void
bp_guillotine_default_order(const bp_cut_item_t *items, uint32_t count,
    uint32_t *order);

/* Returns the scratch words a packing of item_count items needs. */
uint32_t
bp_guillotine_scratch_words(uint32_t item_count);

/*
 * Packs the items onto at most max_plates identical plates. The settings
 * must be valid for bp_cutplan_check, and the plan formed from settings,
 * nodes, plates and items passes it. Capacities below the documented sizes
 * abort; item counts large enough to overflow them are a caller defect.
 */
bp_guillotine_result_t
bp_guillotine_pack(bp_guillotine_args_t args);
