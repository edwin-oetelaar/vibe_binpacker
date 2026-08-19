#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Row-aligned bit grid over caller-owned storage: one bit per sheet cell.
 *
 * Cell (x, y) is bit x % 64 of word x / 64 in row y; the least significant
 * bit of a word is the smallest x, so moving a shape towards larger x is a
 * left shift. Rows start on a word boundary, so a row can be shifted and
 * combined without touching its neighbours.
 *
 * Invariant: padding bits at or beyond width_cells in the last word of a row
 * are always zero. Every write in this module restores that, so popcounts and
 * shifts never see phantom cells. New write paths must keep it.
 *
 * Cell ranges are half-open, so parts whose edges touch do not overlap.
 * The module allocates nothing and owns no caller memory.
 */
typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    size_t stride_words;
    uint64_t *words;
    size_t word_capacity;
} bp_bitmap_t;

typedef struct {
    uint32_t width_cells;
    uint32_t height_cells;
    uint64_t *words;
    size_t word_capacity;
} bp_bitmap_init_args_t;

/* Returns the number of 64-bit words one row occupies. Width must be positive. */
size_t
bp_bitmap_stride_words(uint32_t width_cells);

/*
 * Returns the caller-owned uint64_t capacity a bitmap of this size needs.
 * Width and height must be positive and the product must not overflow.
 */
size_t
bp_bitmap_required_words(uint32_t width_cells, uint32_t height_cells);

/*
 * Initialises an empty bitmap over caller-owned storage, which must outlive
 * the bitmap. Capacity must cover bp_bitmap_required_words.
 */
bp_bitmap_t
bp_bitmap_init(bp_bitmap_init_args_t args);

/* Clears every cell so an existing bitmap can be reused deterministically. */
void
bp_bitmap_clear(bp_bitmap_t *bitmap);

/* Returns the first word of one in-bounds row, for direct row algebra. */
uint64_t *
bp_bitmap_row(bp_bitmap_t *bitmap, uint32_t y);

/* Read-only variant of bp_bitmap_row. */
const uint64_t *
bp_bitmap_row_const(const bp_bitmap_t *bitmap, uint32_t y);

/* Reports whether one in-bounds cell is occupied, as 0 or 1. */
int
bp_bitmap_get(const bp_bitmap_t *bitmap, uint32_t x, uint32_t y);

/*
 * Occupies cells [x, x + length) of row y. Already occupied cells stay
 * occupied; overlap is a decision for the caller, not an error here.
 * The span must be positive and lie inside the row.
 */
void
bp_bitmap_set_span(bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length);

/* Frees cells [x, x + length) of row y, used to cut holes out of a mask. */
void
bp_bitmap_clear_span(bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length);

/* Reports whether any cell in [x, x + length) of row y is occupied. */
int
bp_bitmap_span_occupied(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length);

/*
 * Counts occupied cells in [x, x + length) of row y. This is the primitive
 * the penetration measure is built on, so it stays exact rather than early
 * out like bp_bitmap_span_occupied.
 */
uint32_t
bp_bitmap_span_count(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length);

/*
 * Finds the largest occupied cell in [x, x + length) of row y. Returns 1 and
 * writes that cell to out_x, or returns 0 and leaves out_x untouched when the
 * span is free. The bottom-left scan uses this to jump past a blocker instead
 * of advancing one cell at a time.
 */
int
bp_bitmap_span_last_occupied(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length, uint32_t *out_x);

/* Counts occupied cells in one in-bounds row. */
uint32_t
bp_bitmap_row_count(const bp_bitmap_t *bitmap, uint32_t y);

/* Counts occupied cells in the whole bitmap, for utilisation reporting. */
uint64_t
bp_bitmap_occupied_count(const bp_bitmap_t *bitmap);

/*
 * Row algebra over raw word arrays of stride_words words each, representing a
 * row of width_cells cells. These are the primitives the erosion scan is
 * built from; they take words rather than bitmaps so a candidate row can be
 * combined with scratch that is not part of any sheet.
 */

/* dst = a & b. dst may alias a or b. */
void
bp_row_and(uint64_t *dst, const uint64_t *a, const uint64_t *b,
    size_t stride_words);

/* dst = a | b. dst may alias a or b. */
void
bp_row_or(uint64_t *dst, const uint64_t *a, const uint64_t *b,
    size_t stride_words);

/*
 * Copies src into dst with the pattern moved along x: a positive delta moves
 * towards larger x, a negative delta towards smaller x. Bits pushed past
 * either end of the row are dropped, and the padding invariant is restored.
 * dst must not alias src, and stride_words must match width_cells.
 */
void
bp_row_translate(uint64_t *dst, const uint64_t *src, size_t stride_words,
    uint32_t width_cells, int32_t delta_cells);
