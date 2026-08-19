#include "bp_bitmap.h"

#include "bp_contract.h"

#include <string.h>

#define S_WORD_BITS 64U

/* Counts set bits in one word, using the toolchain builtin where available. */
static uint32_t
s_popcount(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t) __builtin_popcountll(value);
#else
    uint32_t count = 0U;

    while (value != UINT64_C(0)) {
        value &= value - UINT64_C(1);
        ++count;
    }
    return count;
#endif
}

/*
 * Builds a mask of bits [lo, hi) inside one word. Shifting a 64-bit value by
 * 64 is undefined behaviour, so the full-word case is handled separately.
 */
static uint64_t
s_word_mask(uint32_t lo, uint32_t hi)
{
    BP_REQUIRE(lo < hi);
    BP_REQUIRE(hi <= S_WORD_BITS);

    if (hi - lo == S_WORD_BITS)
        return UINT64_MAX;
    return ((UINT64_C(1) << (hi - lo)) - UINT64_C(1)) << lo;
}

/*
 * Clears the padding bits past width_cells in the last word of a row. Every
 * write funnels through here so counts and shifts never see phantom cells.
 */
static void
s_mask_tail(uint64_t *row, size_t stride_words, uint32_t width_cells)
{
    uint32_t tail = width_cells % S_WORD_BITS;

    BP_REQUIRE(row);
    BP_REQUIRE(stride_words > 0U);

    if (tail != 0U)
        row[stride_words - 1U] &= s_word_mask(0U, tail);
}

/* Validates that a span is positive and lies inside one in-bounds row. */
static void
s_require_span(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length)
{
    BP_REQUIRE(bitmap);
    BP_REQUIRE(bitmap->words);
    BP_REQUIRE(y < bitmap->height_cells);
    BP_REQUIRE(length > 0U);
    BP_REQUIRE(x < bitmap->width_cells);
    BP_REQUIRE(length <= bitmap->width_cells - x);
}

/* Returns the number of 64-bit words one row occupies. */
size_t
bp_bitmap_stride_words(uint32_t width_cells)
{
    BP_REQUIRE(width_cells > 0U);
    return ((size_t) width_cells + (S_WORD_BITS - 1U)) / S_WORD_BITS;
}

/* Returns the caller-owned word capacity a bitmap of this size needs. */
size_t
bp_bitmap_required_words(uint32_t width_cells, uint32_t height_cells)
{
    size_t stride;

    BP_REQUIRE(width_cells > 0U);
    BP_REQUIRE(height_cells > 0U);
    stride = bp_bitmap_stride_words(width_cells);
    BP_REQUIRE(stride <= SIZE_MAX / (size_t) height_cells);
    return stride * (size_t) height_cells;
}

/* Initialises an empty bitmap over caller-owned storage. */
bp_bitmap_t
bp_bitmap_init(bp_bitmap_init_args_t args)
{
    bp_bitmap_t bitmap;

    BP_REQUIRE(args.width_cells > 0U);
    BP_REQUIRE(args.height_cells > 0U);
    BP_REQUIRE(args.words);
    BP_REQUIRE(args.word_capacity
        >= bp_bitmap_required_words(args.width_cells, args.height_cells));

    bitmap.width_cells = args.width_cells;
    bitmap.height_cells = args.height_cells;
    bitmap.stride_words = bp_bitmap_stride_words(args.width_cells);
    bitmap.words = args.words;
    bitmap.word_capacity = args.word_capacity;
    bp_bitmap_clear(&bitmap);
    return bitmap;
}

/* Clears every cell so a bitmap can be reused without reallocating. */
void
bp_bitmap_clear(bp_bitmap_t *bitmap)
{
    BP_REQUIRE(bitmap);
    BP_REQUIRE(bitmap->words);
    BP_REQUIRE(bitmap->stride_words > 0U);
    BP_REQUIRE(bitmap->height_cells > 0U);

    memset(bitmap->words, 0,
        bitmap->stride_words * (size_t) bitmap->height_cells
            * sizeof bitmap->words[0]);
}

/* Returns the first word of one in-bounds row, for direct row algebra. */
uint64_t *
bp_bitmap_row(bp_bitmap_t *bitmap, uint32_t y)
{
    BP_REQUIRE(bitmap);
    BP_REQUIRE(bitmap->words);
    BP_REQUIRE(y < bitmap->height_cells);
    return bitmap->words + (size_t) y * bitmap->stride_words;
}

/* Read-only variant of bp_bitmap_row. */
const uint64_t *
bp_bitmap_row_const(const bp_bitmap_t *bitmap, uint32_t y)
{
    BP_REQUIRE(bitmap);
    BP_REQUIRE(bitmap->words);
    BP_REQUIRE(y < bitmap->height_cells);
    return bitmap->words + (size_t) y * bitmap->stride_words;
}

/* Reports whether one in-bounds cell is occupied. */
int
bp_bitmap_get(const bp_bitmap_t *bitmap, uint32_t x, uint32_t y)
{
    const uint64_t *row;

    BP_REQUIRE(bitmap);
    BP_REQUIRE(x < bitmap->width_cells);
    row = bp_bitmap_row_const(bitmap, y);
    return (row[x / S_WORD_BITS] >> (x % S_WORD_BITS)) & UINT64_C(1)
        ? 1
        : 0;
}

/* Occupies cells [x, x + length) of row y, leaving other cells untouched. */
void
bp_bitmap_set_span(bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length)
{
    uint64_t *row;
    uint32_t end;
    uint32_t first;
    uint32_t last;
    uint32_t word;

    s_require_span(bitmap, y, x, length);
    row = bp_bitmap_row(bitmap, y);
    end = x + length;
    first = x / S_WORD_BITS;
    last = (end - 1U) / S_WORD_BITS;

    for (word = first; word <= last; ++word) {
        uint32_t lo = (word == first) ? x % S_WORD_BITS : 0U;
        uint32_t hi = (word == last)
            ? ((end - 1U) % S_WORD_BITS) + 1U
            : S_WORD_BITS;

        row[word] |= s_word_mask(lo, hi);
    }
}

/* Frees cells [x, x + length) of row y, used to cut holes out of a mask. */
void
bp_bitmap_clear_span(bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length)
{
    uint64_t *row;
    uint32_t end;
    uint32_t first;
    uint32_t last;
    uint32_t word;

    s_require_span(bitmap, y, x, length);
    row = bp_bitmap_row(bitmap, y);
    end = x + length;
    first = x / S_WORD_BITS;
    last = (end - 1U) / S_WORD_BITS;

    for (word = first; word <= last; ++word) {
        uint32_t lo = (word == first) ? x % S_WORD_BITS : 0U;
        uint32_t hi = (word == last)
            ? ((end - 1U) % S_WORD_BITS) + 1U
            : S_WORD_BITS;

        row[word] &= ~s_word_mask(lo, hi);
    }
}

/* Reports whether any cell in [x, x + length) of row y is occupied. */
int
bp_bitmap_span_occupied(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length)
{
    const uint64_t *row;
    uint32_t end;
    uint32_t first;
    uint32_t last;
    uint32_t word;

    s_require_span(bitmap, y, x, length);
    row = bp_bitmap_row_const(bitmap, y);
    end = x + length;
    first = x / S_WORD_BITS;
    last = (end - 1U) / S_WORD_BITS;

    for (word = first; word <= last; ++word) {
        uint32_t lo = (word == first) ? x % S_WORD_BITS : 0U;
        uint32_t hi = (word == last)
            ? ((end - 1U) % S_WORD_BITS) + 1U
            : S_WORD_BITS;

        if ((row[word] & s_word_mask(lo, hi)) != UINT64_C(0))
            return 1;
    }
    return 0;
}

/* Counts occupied cells in [x, x + length) of row y. */
uint32_t
bp_bitmap_span_count(const bp_bitmap_t *bitmap, uint32_t y, uint32_t x,
    uint32_t length)
{
    const uint64_t *row;
    uint32_t end;
    uint32_t first;
    uint32_t last;
    uint32_t word;
    uint32_t count = 0U;

    s_require_span(bitmap, y, x, length);
    row = bp_bitmap_row_const(bitmap, y);
    end = x + length;
    first = x / S_WORD_BITS;
    last = (end - 1U) / S_WORD_BITS;

    for (word = first; word <= last; ++word) {
        uint32_t lo = (word == first) ? x % S_WORD_BITS : 0U;
        uint32_t hi = (word == last)
            ? ((end - 1U) % S_WORD_BITS) + 1U
            : S_WORD_BITS;

        count += s_popcount(row[word] & s_word_mask(lo, hi));
    }
    return count;
}

/* Counts occupied cells in one in-bounds row. */
uint32_t
bp_bitmap_row_count(const bp_bitmap_t *bitmap, uint32_t y)
{
    const uint64_t *row;
    size_t word;
    uint32_t count = 0U;

    BP_REQUIRE(bitmap);
    row = bp_bitmap_row_const(bitmap, y);

    for (word = 0U; word < bitmap->stride_words; ++word)
        count += s_popcount(row[word]);
    return count;
}

/* Counts occupied cells in the whole bitmap, for utilisation reporting. */
uint64_t
bp_bitmap_occupied_count(const bp_bitmap_t *bitmap)
{
    uint32_t y;
    uint64_t count = 0U;

    BP_REQUIRE(bitmap);
    BP_REQUIRE(bitmap->words);
    BP_REQUIRE(bitmap->height_cells > 0U);

    for (y = 0U; y < bitmap->height_cells; ++y)
        count += bp_bitmap_row_count(bitmap, y);
    return count;
}

/* dst = a & b over one row of words. */
void
bp_row_and(uint64_t *dst, const uint64_t *a, const uint64_t *b,
    size_t stride_words)
{
    size_t index;

    BP_REQUIRE(dst);
    BP_REQUIRE(a);
    BP_REQUIRE(b);
    BP_REQUIRE(stride_words > 0U);

    for (index = 0U; index < stride_words; ++index)
        dst[index] = a[index] & b[index];
}

/* dst = a | b over one row of words. */
void
bp_row_or(uint64_t *dst, const uint64_t *a, const uint64_t *b,
    size_t stride_words)
{
    size_t index;

    BP_REQUIRE(dst);
    BP_REQUIRE(a);
    BP_REQUIRE(b);
    BP_REQUIRE(stride_words > 0U);

    for (index = 0U; index < stride_words; ++index)
        dst[index] = a[index] | b[index];
}

/*
 * Copies one row with the pattern moved along x. Positive delta moves towards
 * larger x, which is a left shift because bit 0 is the smallest x. Bits pushed
 * past either end are dropped, which is what makes this safe to use as the
 * candidate-offset step of the erosion scan.
 */
void
bp_row_translate(uint64_t *dst, const uint64_t *src, size_t stride_words,
    uint32_t width_cells, int32_t delta_cells)
{
    uint32_t magnitude;
    size_t word_shift;
    uint32_t bit_shift;
    size_t index;

    BP_REQUIRE(dst);
    BP_REQUIRE(src);
    BP_REQUIRE(dst != src);
    BP_REQUIRE(width_cells > 0U);
    BP_REQUIRE(stride_words == bp_bitmap_stride_words(width_cells));

    /* Negating through int64_t keeps INT32_MIN from overflowing. */
    magnitude = (delta_cells < 0)
        ? (uint32_t) -(int64_t) delta_cells
        : (uint32_t) delta_cells;

    if (magnitude >= width_cells) {
        memset(dst, 0, stride_words * sizeof dst[0]);
        return;
    }

    word_shift = magnitude / S_WORD_BITS;
    bit_shift = magnitude % S_WORD_BITS;

    if (delta_cells >= 0) {
        for (index = stride_words; index-- > 0U;) {
            uint64_t value = UINT64_C(0);

            if (index >= word_shift) {
                value = src[index - word_shift] << bit_shift;
                if (bit_shift != 0U && index > word_shift)
                    value |= src[index - word_shift - 1U]
                        >> (S_WORD_BITS - bit_shift);
            }
            dst[index] = value;
        }
    } else {
        for (index = 0U; index < stride_words; ++index) {
            uint64_t value = UINT64_C(0);

            if (index + word_shift < stride_words) {
                value = src[index + word_shift] >> bit_shift;
                if (bit_shift != 0U
                    && index + word_shift + 1U < stride_words)
                    value |= src[index + word_shift + 1U]
                        << (S_WORD_BITS - bit_shift);
            }
            dst[index] = value;
        }
    }

    s_mask_tail(dst, stride_words, width_cells);
}
