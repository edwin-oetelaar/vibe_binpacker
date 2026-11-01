#pragma once

#include <stdint.h>

/*
 * A guillotine cutting plan: what a panel or beam saw cuts, in what order.
 *
 * A saw only makes straight cuts that run from edge to edge of the piece on
 * the table. A plan is therefore a tree. Each plate is first trimmed at its
 * edges; what remains, the usable area, is the root. A panel node is split by
 * a series of parallel cuts into children that each span the whole panel
 * across the cut direction. Leaves are items (a requested part) or waste.
 * Being a tree of such splits is what makes a plan sawable, so the property
 * holds by construction and bp_cutplan_check can prove it node by node.
 *
 * Stages. The root is depth 0. The cuts that split a node at depth d are
 * stage d + 1, and their children are at depth d + 1. Stage 1 cuts run along
 * the plate's first axis and the axis alternates with every stage. A level
 * with a single child and no cut still counts as a stage: that never
 * undercounts what the saw has to do.
 *
 * Lengths are integers in units of 0.01 mm, named _10um. That is exact for
 * the usual blade widths, such as 4.4 mm, and reaches 42.9 km.
 *
 * Kerf. Children of a panel are ordered along the split axis. The first
 * starts at the panel's start, and each next one starts exactly kerf after the
 * previous one ends: that gap is the cut. After the last child the panel may
 * end at once, or within kerf, in which case one more cut turns the rest into
 * sawdust. Material left beyond that must be an explicit waste child.
 *
 * Minimum waste. A saw cannot cleanly cut off a sliver narrower than a
 * minimum width, so waste must be at least min_waste_10um wide across the
 * last cut that cut it off. A level without a cut cuts nothing off, so waste
 * below one is held to the width of the cut above it.
 *
 * Sizing cuts. With non_exact set, one extra stage beyond max_stages may bring
 * a single item to size: a node at depth max_stages may then be split into at
 * most one item and waste. Without it, the plan is exact.
 *
 * All storage belongs to the caller. The module allocates nothing.
 */

/* Most stages a plan may declare; bounds the depth of every walk. */
#define BP_CUTPLAN_MAX_STAGES 8U

/* No node, used for cuts that do not split a node, such as edge trims. */
#define BP_CUTPLAN_NO_NODE UINT32_MAX

typedef enum {
    /* The cut line runs parallel to x, so it splits a panel at a y value. */
    BP_CUT_AXIS_X = 0,
    /* The cut line runs parallel to y, so it splits a panel at an x value. */
    BP_CUT_AXIS_Y = 1
} bp_cut_axis_t;

/* Which way stage 1 may run, a property of the saw. */
typedef enum {
    BP_CUT_FIRST_X = 0,
    BP_CUT_FIRST_Y = 1,
    /* The plan chooses per plate. */
    BP_CUT_FIRST_ANY = 2
} bp_cut_first_t;

typedef enum {
    BP_CUT_NODE_PANEL = 0,
    BP_CUT_NODE_ITEM = 1,
    BP_CUT_NODE_WASTE = 2
} bp_cut_node_kind_t;

/*
 * One rectangle of the tree, in plate coordinates. A panel's children are
 * nodes[first_child] up to nodes[first_child + child_count - 1], ordered
 * along the split axis. An item names the requested part it is and whether it
 * is turned a quarter turn, which swaps the part's width and height.
 */
typedef struct {
    bp_cut_node_kind_t kind;
    uint32_t x_10um;
    uint32_t y_10um;
    uint32_t width_10um;
    uint32_t height_10um;
    /* Panel only. */
    uint32_t first_child;
    uint32_t child_count;
    /* Item only. */
    uint32_t item_index;
    uint8_t rotated;
} bp_cut_node_t;

/* One plate: its usable area's node, and the axis its stage 1 cuts follow. */
typedef struct {
    uint32_t root;
    bp_cut_axis_t first_axis;
} bp_cut_plate_t;

/* One requested part, as ordered. may_rotate is 0 for grain or decor. */
typedef struct {
    uint32_t width_10um;
    uint32_t height_10um;
    uint8_t may_rotate;
} bp_cut_item_t;

/*
 * The saw and the stock. Every plate in a plan has this size. Each edge trim
 * is the total material removed at that edge, blade included, and the usable
 * area must be at least one unit in both directions.
 */
typedef struct {
    uint32_t plate_width_10um;
    uint32_t plate_height_10um;
    uint32_t trim_left_10um;
    uint32_t trim_right_10um;
    uint32_t trim_bottom_10um;
    uint32_t trim_top_10um;
    uint32_t kerf_10um;
    uint32_t min_waste_10um;
    /* 1 to BP_CUTPLAN_MAX_STAGES. */
    uint32_t max_stages;
    bp_cut_first_t first_cut;
    /* 1 allows one sizing cut stage beyond max_stages. */
    uint8_t non_exact;
} bp_cut_settings_t;

/*
 * A complete plan over caller-owned arrays. Structural soundness is the
 * builder's contract: every index in range, every node reached exactly once
 * from exactly one plate, panels with at least one child, no zero-sized node.
 * Breaking it aborts. Whether the plan obeys the saw is what
 * bp_cutplan_check reports.
 */
typedef struct {
    bp_cut_settings_t settings;
    const bp_cut_node_t *nodes;
    uint32_t node_count;
    const bp_cut_plate_t *plates;
    uint32_t plate_count;
    const bp_cut_item_t *items;
    uint32_t item_count;
} bp_cutplan_t;

typedef enum {
    BP_CUTPLAN_OK = 0,
    /* A root is not exactly the plate's usable area. */
    BP_CUTPLAN_BAD_ROOT = 1,
    /* A plate's first axis is not the one the saw allows. */
    BP_CUTPLAN_BAD_FIRST_AXIS = 2,
    /* A child does not span its panel across the cut. */
    BP_CUTPLAN_NOT_EDGE_TO_EDGE = 3,
    /* A panel's first child does not start at the panel's start. */
    BP_CUTPLAN_BAD_START = 4,
    /* Two neighbouring children are not exactly one kerf apart. */
    BP_CUTPLAN_BAD_KERF = 5,
    /* After the last child is more than a kerf, or a child overruns. */
    BP_CUTPLAN_BAD_REMAINDER = 6,
    /* A node is deeper than the saw's stages allow. */
    BP_CUTPLAN_TOO_MANY_STAGES = 7,
    /* A sizing cut yields more than one item, or a panel. */
    BP_CUTPLAN_BAD_SIZING_CUT = 8,
    /* A waste child cut off a panel is narrower than the minimum. */
    BP_CUTPLAN_WASTE_TOO_NARROW = 9,
    /* An item's size is not its part's size in its orientation. */
    BP_CUTPLAN_BAD_ITEM_SIZE = 10,
    /* An item is turned although its part may not be. */
    BP_CUTPLAN_BAD_ITEM_ROTATION = 11,
    /* One requested part appears twice. */
    BP_CUTPLAN_ITEM_REPEATED = 12
} bp_cutplan_status_t;

typedef struct {
    const bp_cutplan_t *plan;
    /* One byte per node and per item; contents on return are unspecified. */
    uint8_t *scratch_node_seen;
    uint32_t scratch_node_capacity;
    uint8_t *scratch_item_used;
    uint32_t scratch_item_capacity;
} bp_cutplan_check_args_t;

/*
 * The first rule a plan breaks, in a deterministic order: plates in order,
 * each tree depth first. plate and node locate it; node is
 * BP_CUTPLAN_NO_NODE when the rule is about the plate as a whole.
 */
typedef struct {
    bp_cutplan_status_t status;
    uint32_t plate;
    uint32_t node;
} bp_cutplan_check_result_t;

/*
 * Checks that a plan can be cut by the saw its settings describe. Breaking a
 * cutting rule is an expected outcome and is reported; a structurally broken
 * plan or invalid settings abort. The scratch capacities must cover
 * node_count and item_count. Unplaced parts are allowed: placing every part
 * is the solver's objective, not a cutting rule.
 */
bp_cutplan_check_result_t
bp_cutplan_check(bp_cutplan_check_args_t args);

/* Returns a short human-readable form of a status, for error messages. */
const char *
bp_cutplan_status_text(bp_cutplan_status_t status);

/*
 * One cut of the saw list. position_10um is where the blade starts across the
 * axis: the y value for an X cut, the x value for a Y cut. The blade removes
 * kerf_10um from there, which is less than the full kerf only for a trim or a
 * sawdust cut at an edge. from_10um and to_10um are where the cut line starts
 * and ends along the axis, edge to edge of the piece it splits. Stage 0 is an
 * edge trim, and node is then BP_CUTPLAN_NO_NODE.
 */
typedef struct {
    uint32_t plate;
    uint32_t stage;
    bp_cut_axis_t axis;
    uint32_t position_10um;
    uint32_t kerf_10um;
    uint32_t from_10um;
    uint32_t to_10um;
    uint32_t node;
} bp_cut_t;

/*
 * Returns how many cuts bp_cutplan_cuts writes, to size its buffer. The plan
 * must be structurally sound and pass bp_cutplan_check.
 */
uint32_t
bp_cutplan_cut_count(const bp_cutplan_t *plan);

typedef struct {
    const bp_cutplan_t *plan;
    bp_cut_t *cuts;
    uint32_t cut_capacity;
} bp_cutplan_cuts_args_t;

/*
 * Writes the saw list in the order an operator works: per plate the edge
 * trims, the cuts parallel to stage 1 first, then depth first through the
 * tree, all cuts of a panel before any cut of its children. Returns the number
 * written. The plan must pass bp_cutplan_check and the capacity must cover
 * bp_cutplan_cut_count.
 */
uint32_t
bp_cutplan_cuts(bp_cutplan_cuts_args_t args);
