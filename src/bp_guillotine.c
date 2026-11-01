#include "bp_guillotine.h"

#include "bp_contract.h"

#include <stddef.h>
#include <string.h>

/*
 * Words per item of scratch: the order, the candidate marks, and two plate
 * layouts of ten words per item each (three per strip, three per stack, four
 * per slot; strips and stacks never outnumber items).
 */
#define S_WORDS_PER_ITEM 22U

/*
 * One plate's layout in the local frame of its first axis: u runs along a
 * strip, v across the strips. Strips hold stacks, stacks hold slots, all as
 * contiguous runs, because each level is filled before the next one opens.
 * Kept as plain word arrays inside the caller's scratch.
 */
typedef struct {
    uint32_t *strip_height;
    uint32_t *strip_first;
    uint32_t *strip_count;
    uint32_t *stack_width;
    uint32_t *stack_first;
    uint32_t *stack_count;
    uint32_t *slot_item;
    uint32_t *slot_rotated;
    uint32_t *slot_width;
    uint32_t *slot_height;
    uint32_t strips;
    uint32_t stacks;
    uint32_t slots;
    /* Along v, kerfs between strips included. */
    uint32_t used;
    uint64_t area;
    bp_cut_axis_t axis;
} s_layout_t;

/* What the saw allows, worked out once from the settings. */
typedef struct {
    const bp_cut_settings_t *settings;
    const bp_cut_item_t *items;
    uint32_t item_count;
    const uint32_t *order;
    const uint8_t *placed;
    uint32_t *mark;
    uint32_t stamp;
    uint32_t usable_width;
    uint32_t usable_height;
    uint32_t kerf;
    uint32_t min_waste;
    /* Several stacks per strip: a second stage. */
    int multi_stack;
    /* Several pieces per stack: a third stage. */
    int stacking;
    /* An item narrower than its stack: a fourth stage, or the sizing cut. */
    int narrow;
    /* A stack shorter than its strip leaves material: stage 3 or sizing. */
    int short_stack;
    /* A strip not filled to its length: stage 2, or the sizing cut. */
    int strip_rest;
} s_context_t;

/* Writes plan nodes in the plate's coordinates. */
typedef struct {
    bp_cut_node_t *nodes;
    uint32_t capacity;
    uint32_t count;
    bp_cut_axis_t axis;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t kerf;
} s_build_t;

/* Returns the node capacity a packing of item_count items always fits in. */
uint32_t
bp_guillotine_node_capacity(uint32_t item_count, uint32_t max_plates)
{
    uint32_t plates = bp_guillotine_plate_capacity(item_count, max_plates);

    /* Per plate a root and its waste; per item at most a strip, a stack,
       their waste, a sizing panel, the item and its waste. */
    BP_REQUIRE(item_count <= (UINT32_MAX - 2U * (uint64_t) plates) / 7U);
    return 7U * item_count + 2U * plates;
}

/* Returns the plate capacity a packing can use. */
uint32_t
bp_guillotine_plate_capacity(uint32_t item_count, uint32_t max_plates)
{
    BP_REQUIRE(max_plates >= 1U);
    return item_count < max_plates ? item_count : max_plates;
}

/* Returns the scratch words a packing of item_count items needs. */
uint32_t
bp_guillotine_scratch_words(uint32_t item_count)
{
    BP_REQUIRE(item_count <= UINT32_MAX / S_WORDS_PER_ITEM);
    return S_WORDS_PER_ITEM * item_count;
}

/*
 * Reports whether a leftover can be left: nothing, sawdust within one kerf,
 * or a waste piece at least the minimum wide after the cut that frees it.
 */
static int
s_rest_ok(const s_context_t *context, uint32_t rest)
{
    BP_REQUIRE(context);
    return rest == 0U || rest <= context->kerf
        || rest - context->kerf >= context->min_waste;
}

/* Reports whether an item is free in the candidate being planned. */
static int
s_available(const s_context_t *context, uint32_t item)
{
    BP_REQUIRE(context && item < context->item_count);
    return !context->placed[item] && context->mark[item] != context->stamp;
}

/*
 * Returns an item's size in the local frame for one turn: w along a strip,
 * h across. Returns 0 for a turn the item may not take, or that duplicates
 * the other because the item is square.
 */
static int
s_orient(const s_context_t *context, uint32_t item, bp_cut_axis_t axis,
    uint32_t turn, uint32_t *w, uint32_t *h)
{
    const bp_cut_item_t *part;
    uint32_t dx;
    uint32_t dy;

    BP_REQUIRE(context && item < context->item_count && w && h);
    BP_REQUIRE(turn <= 1U);
    part = &context->items[item];
    if (turn == 1U && (!part->may_rotate
        || part->width_10um == part->height_10um))
        return 0;
    dx = turn ? part->height_10um : part->width_10um;
    dy = turn ? part->width_10um : part->height_10um;
    *w = axis == BP_CUT_AXIS_X ? dx : dy;
    *h = axis == BP_CUT_AXIS_X ? dy : dx;
    return 1;
}

/* Returns the strip length and the plate depth in the local frame. */
static void
s_frame(const s_context_t *context, bp_cut_axis_t axis, uint32_t *length,
    uint32_t *depth)
{
    BP_REQUIRE(context && length && depth);
    *length = axis == BP_CUT_AXIS_X ? context->usable_width
        : context->usable_height;
    *depth = axis == BP_CUT_AXIS_X ? context->usable_height
        : context->usable_width;
}

/*
 * Reports whether a piece of local size w x h may open a strip with
 * strip_room of depth left: it sets the strip's height, so what matters is
 * what it leaves along the strip and below the plate's remaining depth.
 */
static int
s_opens_strip(const s_context_t *context, uint32_t length, uint32_t room,
    uint32_t w, uint32_t h)
{
    BP_REQUIRE(context);
    if (w > length || h > room)
        return 0;
    if (!s_rest_ok(context, room - h) || !s_rest_ok(context, length - w))
        return 0;
    return context->strip_rest || w == length;
}

/* Appends a slot to the newest stack of a layout. */
static void
s_add_slot(s_context_t *context, s_layout_t *layout, uint32_t item,
    uint32_t turn, uint32_t w, uint32_t h)
{
    uint32_t slot;

    BP_REQUIRE(context && layout && layout->stacks > 0U);
    BP_REQUIRE(layout->slots < context->item_count);
    slot = layout->slots++;
    layout->slot_item[slot] = item;
    layout->slot_rotated[slot] = turn;
    layout->slot_width[slot] = w;
    layout->slot_height[slot] = h;
    ++layout->stack_count[layout->stacks - 1U];
    layout->area += (uint64_t) w * h;
    context->mark[item] = context->stamp;
}

/* Opens a stack in the newest strip of a layout, starting with one slot. */
static void
s_add_stack(s_context_t *context, s_layout_t *layout, uint32_t item,
    uint32_t turn, uint32_t w, uint32_t h)
{
    uint32_t stack;

    BP_REQUIRE(context && layout && layout->strips > 0U);
    BP_REQUIRE(layout->stacks < context->item_count);
    stack = layout->stacks++;
    layout->stack_width[stack] = w;
    layout->stack_first[stack] = layout->slots;
    layout->stack_count[stack] = 0U;
    ++layout->strip_count[layout->strips - 1U];
    s_add_slot(context, layout, item, turn, w, h);
}

/*
 * Fills the newest stack upwards: each next piece goes above the last, one
 * kerf apart. The first piece in order that is exactly as wide as the stack
 * wins, so no width is cut away; only when none is does the first narrower
 * piece go in, where the saw can bring it to width. Of an item's two turns
 * the exact one is preferred, then the taller.
 */
static void
s_fill_stack(s_context_t *context, s_layout_t *layout, bp_cut_axis_t axis,
    uint32_t strip_height, uint32_t stack_width, uint32_t used)
{
    BP_REQUIRE(context && layout);
    while (used + (uint64_t) context->kerf < strip_height) {
        uint32_t room = strip_height - used - context->kerf;
        uint32_t index;
        /* Index 0 holds the first exact piece, index 1 the first narrow. */
        int found[2] = { 0, 0 };
        uint32_t pick_item[2] = { 0U, 0U };
        uint32_t pick_turn[2] = { 0U, 0U };
        uint32_t pick_w[2] = { 0U, 0U };
        uint32_t pick_h[2] = { 0U, 0U };
        uint32_t kind;

        for (index = 0U; index < context->item_count && !found[0]; ++index) {
            uint32_t item = context->order[index];
            uint32_t turn;

            if (!s_available(context, item))
                continue;
            for (turn = 0U; turn < 2U; ++turn) {
                uint32_t w;
                uint32_t h;

                if (!s_orient(context, item, axis, turn, &w, &h))
                    continue;
                if (w > stack_width || h > room || !s_rest_ok(context, room - h))
                    continue;
                kind = w == stack_width ? 0U : 1U;
                if (kind == 1U && (!context->narrow
                    || !s_rest_ok(context, stack_width - w)))
                    continue;
                /* Only the first item of each kind counts; its taller turn. */
                if (found[kind] && (pick_item[kind] != item || h <= pick_h[kind]))
                    continue;
                found[kind] = 1;
                pick_item[kind] = item;
                pick_turn[kind] = turn;
                pick_w[kind] = w;
                pick_h[kind] = h;
            }
        }
        kind = found[0] ? 0U : 1U;
        if (!found[kind])
            return;
        s_add_slot(context, layout, pick_item[kind], pick_turn[kind],
            pick_w[kind], pick_h[kind]);
        used += context->kerf + pick_h[kind];
    }
}

/*
 * Fills the newest strip along its length with stacks. Each stack opens with
 * the first piece in order that fits, turned to fill the strip's height
 * best, and is then filled upwards when the saw has a third stage.
 */
static void
s_fill_strip(s_context_t *context, s_layout_t *layout, bp_cut_axis_t axis,
    uint32_t length, uint32_t strip_height, uint32_t used)
{
    BP_REQUIRE(context && layout);
    while (used + (uint64_t) context->kerf < length) {
        uint32_t room = length - used - context->kerf;
        uint32_t index;
        int found = 0;
        uint32_t best_item = 0U;
        uint32_t best_turn = 0U;
        uint32_t best_w = 0U;
        uint32_t best_h = 0U;

        for (index = 0U; index < context->item_count && !found; ++index) {
            uint32_t item = context->order[index];
            uint32_t turn;

            if (!s_available(context, item))
                continue;
            for (turn = 0U; turn < 2U; ++turn) {
                uint32_t w;
                uint32_t h;

                if (!s_orient(context, item, axis, turn, &w, &h))
                    continue;
                if (w > room || h > strip_height || !s_rest_ok(context, room - w))
                    continue;
                if (h < strip_height && (!context->short_stack
                    || !s_rest_ok(context, strip_height - h)))
                    continue;
                if (!found || h > best_h) {
                    found = 1;
                    best_item = item;
                    best_turn = turn;
                    best_w = w;
                    best_h = h;
                }
            }
        }
        if (!found)
            return;
        s_add_stack(context, layout, best_item, best_turn, best_w, best_h);
        if (context->stacking)
            s_fill_stack(context, layout, axis, strip_height, best_w, best_h);
        used += context->kerf + best_w;
    }
}

/*
 * Plans one plate with one first axis and one opening policy: standing
 * (policy 0) turns strip openers to their larger height, lying (policy 1)
 * to their smaller one.
 */
static void
s_plan_plate(s_context_t *context, s_layout_t *layout, bp_cut_axis_t axis,
    uint32_t policy)
{
    uint32_t length;
    uint32_t depth;

    BP_REQUIRE(context && layout && policy <= 1U);
    BP_REQUIRE(context->stamp < UINT32_MAX);
    ++context->stamp;
    layout->strips = layout->stacks = layout->slots = 0U;
    layout->used = 0U;
    layout->area = 0U;
    layout->axis = axis;
    s_frame(context, axis, &length, &depth);

    for (;;) {
        uint32_t gap = layout->strips > 0U ? context->kerf : 0U;
        uint32_t room;
        uint32_t index;
        int found = 0;
        uint32_t open_item = 0U;
        uint32_t open_turn = 0U;
        uint32_t open_w = 0U;
        uint32_t open_h = 0U;
        uint32_t strip;

        if ((uint64_t) layout->used + gap >= depth)
            break;
        room = depth - layout->used - gap;
        for (index = 0U; index < context->item_count && !found; ++index) {
            uint32_t item = context->order[index];
            uint32_t sizes[2][2];
            int valid[2];
            uint32_t pick;

            if (!s_available(context, item))
                continue;
            valid[0] = s_orient(context, item, axis, 0U, &sizes[0][0],
                &sizes[0][1]);
            valid[1] = s_orient(context, item, axis, 1U, &sizes[1][0],
                &sizes[1][1]);
            /* Try the policy's preferred turn first. */
            pick = valid[1] && ((policy == 0U) == (sizes[1][1] > sizes[0][1]))
                ? 1U : 0U;
            if (valid[pick] && s_opens_strip(context, length, room,
                sizes[pick][0], sizes[pick][1]))
                found = 1;
            else if (valid[1U - pick] && s_opens_strip(context, length, room,
                sizes[1U - pick][0], sizes[1U - pick][1])) {
                pick = 1U - pick;
                found = 1;
            }
            if (found) {
                open_item = item;
                open_turn = pick;
                open_w = sizes[pick][0];
                open_h = sizes[pick][1];
            }
        }
        if (!found)
            break;

        BP_REQUIRE(layout->strips < context->item_count);
        strip = layout->strips++;
        layout->strip_height[strip] = open_h;
        layout->strip_first[strip] = layout->stacks;
        layout->strip_count[strip] = 0U;
        s_add_stack(context, layout, open_item, open_turn, open_w, open_h);
        if (context->multi_stack)
            s_fill_strip(context, layout, axis, length, open_h, open_w);
        layout->used += gap + open_h;
    }
}

/* Reports whether an item fits an empty plate in any way the saw allows. */
static int
s_fits_empty(const s_context_t *context, uint32_t item)
{
    bp_cut_axis_t axis;
    uint32_t turn;

    BP_REQUIRE(context);
    for (axis = BP_CUT_AXIS_X; axis <= BP_CUT_AXIS_Y; ++axis) {
        uint32_t length;
        uint32_t depth;

        if (context->settings->first_cut != BP_CUT_FIRST_ANY
            && (int) axis != (int) context->settings->first_cut)
            continue;
        s_frame(context, axis, &length, &depth);
        for (turn = 0U; turn < 2U; ++turn) {
            uint32_t w;
            uint32_t h;

            if (s_orient(context, item, axis, turn, &w, &h)
                && s_opens_strip(context, length, depth, w, h))
                return 1;
        }
    }
    return 0;
}

/* Reserves a contiguous block of child nodes. */
static uint32_t
s_reserve(s_build_t *build, uint32_t count)
{
    uint32_t first;

    BP_REQUIRE(build && count > 0U);
    BP_REQUIRE((uint64_t) build->count + count <= build->capacity);
    first = build->count;
    build->count += count;
    return first;
}

/* Sets a node's kind and its rectangle, given in the local frame. */
static void
s_set(s_build_t *build, uint32_t index, bp_cut_node_kind_t kind, uint32_t u,
    uint32_t v, uint32_t w, uint32_t h)
{
    bp_cut_node_t *node;

    BP_REQUIRE(build && index < build->count);
    node = &build->nodes[index];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    if (build->axis == BP_CUT_AXIS_X) {
        node->x_10um = build->origin_x + u;
        node->y_10um = build->origin_y + v;
        node->width_10um = w;
        node->height_10um = h;
    } else {
        node->x_10um = build->origin_x + v;
        node->y_10um = build->origin_y + u;
        node->width_10um = h;
        node->height_10um = w;
    }
}

/* Makes a node a panel over a reserved block of children. */
static void
s_panel(s_build_t *build, uint32_t index, uint32_t first, uint32_t count)
{
    BP_REQUIRE(build && index < build->count);
    build->nodes[index].kind = BP_CUT_NODE_PANEL;
    build->nodes[index].first_child = first;
    build->nodes[index].child_count = count;
}

/* Reports whether a stack is one item that fills it exactly. */
static int
s_stack_is_item(const s_layout_t *layout, uint32_t stack, uint32_t height)
{
    uint32_t slot;

    BP_REQUIRE(layout && stack < layout->stacks);
    slot = layout->stack_first[stack];
    return layout->stack_count[stack] == 1U
        && layout->slot_width[slot] == layout->stack_width[stack]
        && layout->slot_height[slot] == height;
}

/*
 * Emits one slot: the item itself when it spans its stack, otherwise a
 * panel that cuts it to width with the rest as waste or dust.
 */
static void
s_emit_slot(s_build_t *build, const s_layout_t *layout, uint32_t slot,
    uint32_t index, uint32_t u, uint32_t v, uint32_t stack_width)
{
    uint32_t w;
    uint32_t h;
    uint32_t rest;
    uint32_t first;
    uint32_t item_node;

    BP_REQUIRE(build && layout && slot < layout->slots);
    w = layout->slot_width[slot];
    h = layout->slot_height[slot];
    BP_REQUIRE(w <= stack_width);
    item_node = index;
    if (w < stack_width) {
        rest = stack_width - w;
        first = s_reserve(build, rest > build->kerf ? 2U : 1U);
        s_set(build, index, BP_CUT_NODE_PANEL, u, v, stack_width, h);
        s_panel(build, index, first, rest > build->kerf ? 2U : 1U);
        if (rest > build->kerf)
            s_set(build, first + 1U, BP_CUT_NODE_WASTE, u + w + build->kerf, v,
                rest - build->kerf, h);
        item_node = first;
    }
    s_set(build, item_node, BP_CUT_NODE_ITEM, u, v, w, h);
    build->nodes[item_node].item_index = layout->slot_item[slot];
    build->nodes[item_node].rotated = (uint8_t) layout->slot_rotated[slot];
}

/* Emits one stack: its slots one kerf apart, then any waste above them. */
static void
s_emit_stack(s_build_t *build, const s_layout_t *layout, uint32_t stack,
    uint32_t index, uint32_t u, uint32_t v, uint32_t height)
{
    uint32_t width;
    uint32_t count;
    uint32_t used = 0U;
    uint32_t rest;
    uint32_t first;
    uint32_t slot;

    BP_REQUIRE(build && layout && stack < layout->stacks);
    width = layout->stack_width[stack];
    count = layout->stack_count[stack];
    if (s_stack_is_item(layout, stack, height)) {
        s_emit_slot(build, layout, layout->stack_first[stack], index, u, v,
            width);
        return;
    }
    for (slot = 0U; slot < count; ++slot)
        used += (slot > 0U ? build->kerf : 0U)
            + layout->slot_height[layout->stack_first[stack] + slot];
    BP_REQUIRE(used <= height);
    rest = height - used;
    first = s_reserve(build, count + (rest > build->kerf ? 1U : 0U));
    s_set(build, index, BP_CUT_NODE_PANEL, u, v, width, height);
    s_panel(build, index, first, count + (rest > build->kerf ? 1U : 0U));
    if (rest > build->kerf)
        s_set(build, first + count, BP_CUT_NODE_WASTE, u, v + used + build->kerf,
            width, rest - build->kerf);
    used = 0U;
    for (slot = 0U; slot < count; ++slot) {
        uint32_t entry = layout->stack_first[stack] + slot;

        used += slot > 0U ? build->kerf : 0U;
        s_emit_slot(build, layout, entry, first + slot, u, v + used, width);
        used += layout->slot_height[entry];
    }
}

/* Emits one strip: its stacks one kerf apart, then any waste beyond them. */
static void
s_emit_strip(s_build_t *build, const s_layout_t *layout, uint32_t strip,
    uint32_t index, uint32_t v, uint32_t length)
{
    uint32_t height;
    uint32_t count;
    uint32_t used = 0U;
    uint32_t rest;
    uint32_t first;
    uint32_t stack;

    BP_REQUIRE(build && layout && strip < layout->strips);
    height = layout->strip_height[strip];
    count = layout->strip_count[strip];
    for (stack = 0U; stack < count; ++stack)
        used += (stack > 0U ? build->kerf : 0U)
            + layout->stack_width[layout->strip_first[strip] + stack];
    BP_REQUIRE(used <= length);
    rest = length - used;
    if (count == 1U && rest == 0U
        && s_stack_is_item(layout, layout->strip_first[strip], height)) {
        s_emit_stack(build, layout, layout->strip_first[strip], index, 0U, v,
            height);
        return;
    }
    first = s_reserve(build, count + (rest > build->kerf ? 1U : 0U));
    s_set(build, index, BP_CUT_NODE_PANEL, 0U, v, length, height);
    s_panel(build, index, first, count + (rest > build->kerf ? 1U : 0U));
    if (rest > build->kerf)
        s_set(build, first + count, BP_CUT_NODE_WASTE, used + build->kerf, v,
            rest - build->kerf, height);
    used = 0U;
    for (stack = 0U; stack < count; ++stack) {
        uint32_t entry = layout->strip_first[strip] + stack;

        used += stack > 0U ? build->kerf : 0U;
        s_emit_stack(build, layout, entry, first + stack, used, v, height);
        used += layout->stack_width[entry];
    }
}

/* Reports whether a strip is one item that fills it exactly. */
static int
s_strip_is_item(const s_layout_t *layout, uint32_t strip, uint32_t length)
{
    uint32_t stack;

    BP_REQUIRE(layout && strip < layout->strips);
    stack = layout->strip_first[strip];
    return layout->strip_count[strip] == 1U
        && layout->stack_width[stack] == length
        && s_stack_is_item(layout, stack, layout->strip_height[strip]);
}

/* Emits a plate's tree from its layout and returns the root's index. */
static uint32_t
s_emit_plate(s_build_t *build, const s_context_t *context,
    const s_layout_t *layout)
{
    uint32_t length;
    uint32_t depth;
    uint32_t root;
    uint32_t rest;
    uint32_t first;
    uint32_t strip;
    uint32_t used = 0U;

    BP_REQUIRE(build && context && layout && layout->strips > 0U);
    s_frame(context, layout->axis, &length, &depth);
    build->axis = layout->axis;
    root = s_reserve(build, 1U);
    BP_REQUIRE(layout->used <= depth);
    rest = depth - layout->used;
    if (layout->strips == 1U && rest == 0U
        && s_strip_is_item(layout, 0U, length)) {
        s_emit_strip(build, layout, 0U, root, 0U, length);
        return root;
    }
    first = s_reserve(build, layout->strips + (rest > build->kerf ? 1U : 0U));
    s_set(build, root, BP_CUT_NODE_PANEL, 0U, 0U, length, depth);
    s_panel(build, root, first, layout->strips + (rest > build->kerf ? 1U : 0U));
    if (rest > build->kerf)
        s_set(build, first + layout->strips, BP_CUT_NODE_WASTE, 0U,
            layout->used + build->kerf, length, rest - build->kerf);
    for (strip = 0U; strip < layout->strips; ++strip) {
        used += strip > 0U ? build->kerf : 0U;
        s_emit_strip(build, layout, strip, first + strip, used, length);
        used += layout->strip_height[strip];
    }
    return root;
}

/* Points a layout's arrays into scratch, ten words per item. */
static s_layout_t
s_layout(uint32_t *words, uint32_t item_count)
{
    s_layout_t layout;

    memset(&layout, 0, sizeof layout);
    layout.strip_height = words;
    layout.strip_first = words + item_count;
    layout.strip_count = words + 2U * item_count;
    layout.stack_width = words + 3U * item_count;
    layout.stack_first = words + 4U * item_count;
    layout.stack_count = words + 5U * item_count;
    layout.slot_item = words + 6U * item_count;
    layout.slot_rotated = words + 7U * item_count;
    layout.slot_width = words + 8U * item_count;
    layout.slot_height = words + 9U * item_count;
    return layout;
}

/* Returns the longer side of an item, the order's first key. */
static uint32_t
s_long_side(const bp_cut_item_t *item)
{
    return item->width_10um > item->height_10um ? item->width_10um
        : item->height_10um;
}

/*
 * Orders items longest side first, then shortest side first among equals,
 * then by index. Insertion sort, stable and deterministic without a context
 * pointer for qsort.
 */
static void
s_sort(uint32_t *order, const bp_cut_item_t *items, uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; ++index)
        order[index] = index;
    for (index = 1U; index < count; ++index) {
        uint32_t item = order[index];
        uint32_t long_side = s_long_side(&items[item]);
        uint32_t short_side = items[item].width_10um + items[item].height_10um
            - long_side;
        uint32_t slot = index;

        while (slot > 0U) {
            const bp_cut_item_t *other = &items[order[slot - 1U]];
            uint32_t other_long = s_long_side(other);
            uint32_t other_short = other->width_10um + other->height_10um
                - other_long;

            if (other_long > long_side
                || (other_long == long_side && other_short >= short_side))
                break;
            order[slot] = order[slot - 1U];
            --slot;
        }
        order[slot] = item;
    }
}

/* Packs the items onto at most max_plates identical plates. */
bp_guillotine_result_t
bp_guillotine_pack(bp_guillotine_args_t args)
{
    const bp_cut_settings_t *settings = args.settings;
    bp_guillotine_result_t result = { BP_GUILLOTINE_COMPLETE, 0U, 0U, 0U, 0U };
    s_context_t context;
    s_layout_t layouts[2];
    s_build_t build;
    uint32_t item;
    uint32_t max_stages;

    BP_REQUIRE(settings);
    BP_REQUIRE(settings->plate_width_10um > 0U && settings->plate_height_10um > 0U);
    BP_REQUIRE((uint64_t) settings->trim_left_10um + settings->trim_right_10um
        < settings->plate_width_10um);
    BP_REQUIRE((uint64_t) settings->trim_bottom_10um + settings->trim_top_10um
        < settings->plate_height_10um);
    BP_REQUIRE(settings->max_stages >= 1U
        && settings->max_stages <= BP_CUTPLAN_MAX_STAGES);
    BP_REQUIRE(settings->first_cut <= BP_CUT_FIRST_ANY);
    BP_REQUIRE(settings->non_exact <= 1U);
    BP_REQUIRE(args.items || args.item_count == 0U);
    BP_REQUIRE(args.max_plates >= 1U);
    BP_REQUIRE(args.nodes || args.item_count == 0U);
    BP_REQUIRE(args.node_capacity
        >= bp_guillotine_node_capacity(args.item_count, args.max_plates));
    BP_REQUIRE(args.plates || args.item_count == 0U);
    BP_REQUIRE(args.plate_capacity
        >= bp_guillotine_plate_capacity(args.item_count, args.max_plates));
    BP_REQUIRE(args.placed || args.item_count == 0U);
    BP_REQUIRE(args.scratch || args.item_count == 0U);
    BP_REQUIRE(args.scratch_capacity
        >= bp_guillotine_scratch_words(args.item_count));
    for (item = 0U; item < args.item_count; ++item)
        BP_REQUIRE(args.items[item].width_10um > 0U
            && args.items[item].height_10um > 0U);

    if (args.item_count == 0U)
        return result;
    memset(args.placed, 0, args.item_count);
    max_stages = settings->max_stages;
    context = (s_context_t) {
        .settings = settings,
        .items = args.items,
        .item_count = args.item_count,
        .order = args.scratch,
        .placed = args.placed,
        .mark = args.scratch + args.item_count,
        .stamp = 0U,
        .usable_width = settings->plate_width_10um - settings->trim_left_10um
            - settings->trim_right_10um,
        .usable_height = settings->plate_height_10um - settings->trim_bottom_10um
            - settings->trim_top_10um,
        .kerf = settings->kerf_10um,
        .min_waste = settings->min_waste_10um,
        .multi_stack = max_stages >= 2U,
        .stacking = max_stages >= 3U,
        .narrow = max_stages >= 4U || (max_stages == 3U && settings->non_exact),
        .short_stack = max_stages >= 3U
            || (max_stages == 2U && settings->non_exact),
        .strip_rest = max_stages >= 2U || settings->non_exact
    };
    s_sort(args.scratch, args.items, args.item_count);
    memset(context.mark, 0, (size_t) args.item_count * sizeof *context.mark);
    layouts[0] = s_layout(args.scratch + 2U * args.item_count, args.item_count);
    layouts[1] = s_layout(args.scratch + 12U * args.item_count, args.item_count);
    build = (s_build_t) {
        .nodes = args.nodes, .capacity = args.node_capacity,
        .origin_x = settings->trim_left_10um,
        .origin_y = settings->trim_bottom_10um,
        .kerf = settings->kerf_10um
    };

    while (result.placed_count < args.item_count
        && result.plate_count < args.max_plates) {
        s_layout_t *best = &layouts[0];
        s_layout_t *candidate = &layouts[1];
        int have_best = 0;
        bp_cut_axis_t axis;
        uint32_t policy;
        uint32_t slot;

        for (axis = BP_CUT_AXIS_X; axis <= BP_CUT_AXIS_Y; ++axis) {
            if (settings->first_cut != BP_CUT_FIRST_ANY
                && (int) axis != (int) settings->first_cut)
                continue;
            for (policy = 0U; policy < 2U; ++policy) {
                uint32_t best_left = 0U;
                uint32_t candidate_left;
                uint32_t length;
                uint32_t depth;

                s_plan_plate(&context, candidate, axis, policy);
                s_frame(&context, candidate->axis, &length, &depth);
                candidate_left = depth - candidate->used;
                if (have_best) {
                    s_frame(&context, best->axis, &length, &depth);
                    best_left = depth - best->used;
                }
                /* Most area first, then the larger strip left at the end. */
                if (!have_best || candidate->area > best->area
                    || (candidate->area == best->area
                        && candidate_left > best_left)) {
                    s_layout_t *swap = best;

                    best = candidate;
                    candidate = swap;
                    have_best = 1;
                }
            }
        }
        if (best->slots == 0U)
            break;
        for (slot = 0U; slot < best->slots; ++slot)
            args.placed[best->slot_item[slot]] = 1U;
        result.placed_count += best->slots;
        args.plates[result.plate_count].root = s_emit_plate(&build, &context,
            best);
        args.plates[result.plate_count].first_axis = best->axis;
        ++result.plate_count;
    }

    result.node_count = build.count;
    for (item = 0U; item < args.item_count; ++item)
        if (!args.placed[item] && !s_fits_empty(&context, item))
            ++result.oversized_count;
    if (result.placed_count < args.item_count)
        result.status = BP_GUILLOTINE_PARTIAL;
    return result;
}
