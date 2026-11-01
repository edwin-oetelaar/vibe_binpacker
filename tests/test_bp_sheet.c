#include "test_support.h"

#include "bp_sheet.h"

#include <math.h>
#include <string.h>

#define S_MIB (1024.0 * 1024.0)

/*
 * The byte figure written out from the buffer layout in
 * docs/plan_plaatformaten.md, independently of the capacity functions the
 * module uses, so a change to either side shows up here.
 */
static uint64_t
s_expected_bytes(uint64_t width_cells, uint64_t height_cells)
{
    uint64_t stride = (width_cells + 63U) / 64U;

    CHECK(width_cells > 0U && height_cells > 0U);
    return 8U * stride * height_cells
        + 4U * width_cells * height_cells
        + 8U * (width_cells + 1U) * height_cells
        + 4U * 8U * stride;
}

/* Plans one size against the default limits. */
static bp_sheet_plan_t
s_plan(double width_mm, double height_mm, double cell_size_mm)
{
    return bp_sheet_plan((bp_sheet_plan_args_t) {
        .width_mm = width_mm,
        .height_mm = height_mm,
        .cell_size_mm = cell_size_mm,
        .limits = bp_sheet_default_limits()
    });
}

/* The defaults are the published guardrails, not a measured maximum. */
static void
s_test_default_limits(void)
{
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    CHECK(limits.max_cells_per_axis == 8192U);
    CHECK(limits.max_total_cells == UINT64_C(16000000));
    CHECK(limits.max_buffer_bytes == UINT64_C(268435456));
}

/* Every row of the table in the plan, so the documentation stays true. */
static void
s_test_plan_table(void)
{
    static const struct {
        double width_mm;
        double height_mm;
        double cell_size_mm;
        uint64_t cells;
        double mib;
    } rows[] = {
        { 500.0, 500.0, 1.0, 250000U, 2.90 },
        { 600.0, 600.0, 1.0, 360000U, 4.17 },
        { 600.0, 300.0, 1.0, 180000U, 2.09 },
        { 800.0, 600.0, 1.0, 480000U, 5.56 },
        { 800.0, 600.0, 0.25, 7680000U, 88.83 },
        { 1250.0, 2500.0, 1.0, 3125000U, 36.16 },
        { 1250.0, 2500.0, 0.5, 12500000U, 144.62 }
    };
    size_t index;

    for (index = 0U; index < sizeof rows / sizeof rows[0]; ++index) {
        bp_sheet_plan_t plan = s_plan(rows[index].width_mm,
            rows[index].height_mm, rows[index].cell_size_mm);

        CHECK(plan.status == BP_SHEET_OK);
        CHECK(plan.total_cells == rows[index].cells);
        CHECK(plan.buffer_bytes
            == s_expected_bytes(plan.width_cells, plan.height_cells));
        CHECK(fabs((double) plan.buffer_bytes / S_MIB - rows[index].mib)
            < 0.005);
    }
    /* The last row of the table is the one the limits are meant to refuse. */
    CHECK(fabs((double) s_expected_bytes(5000U, 10000U) / S_MIB - 578.31)
        < 0.005);
    CHECK(s_plan(1250.0, 2500.0, 0.25).status == BP_SHEET_AXIS_OVER_LIMIT);
}

/* Every preset fits at 1 mm in both orientations, and turning is not free. */
static void
s_test_presets_both_orientations(void)
{
    static const double presets_mm[][2] = {
        { 500.0, 500.0 }, { 600.0, 600.0 }, { 600.0, 300.0 },
        { 800.0, 600.0 }, { 1200.0, 1200.0 }, { 1250.0, 2500.0 }
    };
    size_t index;

    for (index = 0U; index < sizeof presets_mm / sizeof presets_mm[0];
        ++index) {
        bp_sheet_plan_t upright = s_plan(presets_mm[index][0],
            presets_mm[index][1], 1.0);
        bp_sheet_plan_t turned = s_plan(presets_mm[index][1],
            presets_mm[index][0], 1.0);

        CHECK(upright.status == BP_SHEET_OK && turned.status == BP_SHEET_OK);
        CHECK(upright.width_cells == (uint32_t) presets_mm[index][0]);
        CHECK(upright.height_cells == (uint32_t) presets_mm[index][1]);
        CHECK(turned.width_cells == upright.height_cells);
        CHECK(turned.height_cells == upright.width_cells);
        CHECK(turned.total_cells == upright.total_cells);
        /* Word padding is per row, so the orientation changes the bytes. */
        CHECK(turned.buffer_bytes
            == s_expected_bytes(turned.width_cells, turned.height_cells));
    }
    CHECK(s_plan(600.0, 300.0, 1.0).buffer_bytes
        != s_plan(300.0, 600.0, 1.0).buffer_bytes);
}

/* Widths either side of a word boundary pay for the extra word exactly. */
static void
s_test_word_boundaries(void)
{
    static const uint32_t widths[] = { 1U, 63U, 64U, 65U, 127U, 128U, 129U };
    size_t index;

    for (index = 0U; index < sizeof widths / sizeof widths[0]; ++index) {
        CHECK(bp_sheet_buffer_bytes(widths[index], 3U)
            == s_expected_bytes(widths[index], 3U));
        CHECK(s_plan((double) widths[index], 3.0, 1.0).buffer_bytes
            == s_expected_bytes(widths[index], 3U));
    }
    /* 64 to 65 cells adds a word per row and to each scratch row. */
    CHECK(bp_sheet_buffer_bytes(65U, 3U) - bp_sheet_buffer_bytes(64U, 3U)
        == 8U * 3U + 4U * 3U + 8U * 3U + 32U);
    CHECK(bp_sheet_buffer_bytes(64U, 3U) - bp_sheet_buffer_bytes(63U, 3U)
        == 4U * 3U + 8U * 3U);
}

/* The sheet rounds down and a part of a cell is not a cell. */
static void
s_test_rounding(void)
{
    bp_sheet_plan_t plan;

    plan = s_plan(1250.0, 2500.0, 0.5);
    CHECK(plan.width_cells == 2500U && plan.height_cells == 5000U);
    plan = s_plan(1250.0, 100.0, 0.3);
    CHECK(plan.width_cells == 4166U && plan.height_cells == 333U);
    plan = s_plan(0.3, 1.0, 0.1);
    CHECK(plan.status == BP_SHEET_OK && plan.width_cells == 3U);
    plan = s_plan(1.0, 1.0, 1.0);
    CHECK(plan.status == BP_SHEET_OK && plan.total_cells == 1U);
    CHECK(s_plan(0.999, 10.0, 1.0).status == BP_SHEET_TOO_SMALL);
    CHECK(s_plan(10.0, 0.999, 1.0).status == BP_SHEET_TOO_SMALL);
    CHECK(s_plan(10.0, 10.0, 10.5).status == BP_SHEET_TOO_SMALL);
    plan = s_plan(0.5, 0.5, 1.0);
    CHECK(plan.width_cells == 0U && plan.total_cells == 0U);
    CHECK(plan.buffer_bytes == 0U);
}

/* Bad numbers are reported, whichever field carries them. */
static void
s_test_bad_input(void)
{
    const double bad[] = { 0.0, -0.0, -1.0, NAN, INFINITY, -INFINITY };
    size_t index;

    for (index = 0U; index < sizeof bad / sizeof bad[0]; ++index) {
        bp_sheet_plan_t plan = s_plan(bad[index], 600.0, 1.0);

        CHECK(plan.status == BP_SHEET_BAD_SIZE);
        CHECK(plan.width_cells == 0U && plan.buffer_bytes == 0U);
        CHECK(s_plan(800.0, bad[index], 1.0).status == BP_SHEET_BAD_SIZE);
        CHECK(s_plan(800.0, 600.0, bad[index]).status
            == BP_SHEET_BAD_CELL_SIZE);
    }
    /* A bad size is named before a bad cell, so the message is stable. */
    CHECK(s_plan(NAN, 600.0, NAN).status == BP_SHEET_BAD_SIZE);
    /* A quotient that overflows is too large, not a crash or a wrap. */
    CHECK(s_plan(1e300, 1e300, 1e-300).status == BP_SHEET_AXIS_OVER_LIMIT);
}

/* Each limit admits exactly its value and refuses one more. */
static void
s_test_limits_exact(void)
{
    bp_sheet_limits_t limits = bp_sheet_default_limits();
    bp_sheet_plan_args_t args = {
        .width_mm = 8192.0, .height_mm = 1.0, .cell_size_mm = 1.0,
        .limits = limits
    };
    bp_sheet_plan_t plan;

    CHECK(bp_sheet_plan(args).status == BP_SHEET_OK);
    args.width_mm = 8193.0;
    plan = bp_sheet_plan(args);
    CHECK(plan.status == BP_SHEET_AXIS_OVER_LIMIT && plan.width_cells == 0U);
    args.width_mm = 1.0;
    args.height_mm = 8193.0;
    CHECK(bp_sheet_plan(args).status == BP_SHEET_AXIS_OVER_LIMIT);

    /* 4000 x 4000 is the default cell limit to the cell. */
    args.width_mm = 4000.0;
    args.height_mm = 4000.0;
    CHECK(bp_sheet_plan(args).status == BP_SHEET_OK);
    args.width_mm = 4001.0;
    plan = bp_sheet_plan(args);
    CHECK(plan.status == BP_SHEET_CELLS_OVER_LIMIT);
    CHECK(plan.width_cells == 4001U && plan.height_cells == 4000U);
    CHECK(plan.total_cells == UINT64_C(16004000) && plan.buffer_bytes == 0U);

    args.width_mm = 100.0;
    args.height_mm = 100.0;
    args.limits.max_buffer_bytes = s_expected_bytes(100U, 100U);
    CHECK(bp_sheet_plan(args).status == BP_SHEET_OK);
    args.limits.max_buffer_bytes -= 1U;
    plan = bp_sheet_plan(args);
    CHECK(plan.status == BP_SHEET_BYTES_OVER_LIMIT);
    CHECK(plan.total_cells == 10000U);
    CHECK(plan.buffer_bytes == s_expected_bytes(100U, 100U));
}

/* The suggestion is the finest ladder step that fits, never applied. */
static void
s_test_suggestion(void)
{
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    CHECK(bp_sheet_suggest_cell_size_mm(1250.0, 2500.0, limits) == 0.5);
    CHECK(bp_sheet_suggest_cell_size_mm(800.0, 600.0, limits) == 0.2);
    CHECK(bp_sheet_suggest_cell_size_mm(500.0, 500.0, limits) == 0.2);
    CHECK(bp_sheet_suggest_cell_size_mm(200.0, 200.0, limits) == 0.1);
    CHECK(bp_sheet_suggest_cell_size_mm(1e6, 1e6, limits) == 0.0);
}

/* Explains a plan against the default limits into a fresh buffer. */
static void
s_explain(const bp_sheet_plan_t *plan, char *text)
{
    memset(text, 'x', BP_SHEET_EXPLAIN_CAPACITY);
    bp_sheet_explain(plan, bp_sheet_default_limits(), text,
        BP_SHEET_EXPLAIN_CAPACITY);
    CHECK(strlen(text) < BP_SHEET_EXPLAIN_CAPACITY);
}

/* The one line the CLI and the GUI both show, for every outcome. */
static void
s_test_explain(void)
{
    char text[BP_SHEET_EXPLAIN_CAPACITY];
    bp_sheet_limits_t limits = bp_sheet_default_limits();
    bp_sheet_plan_t plan;

    plan = s_plan(800.0, 600.0, 1.0);
    CHECK(plan.width_mm == 800.0 && plan.height_mm == 600.0);
    CHECK(plan.cell_size_mm == 1.0);
    s_explain(&plan, text);
    CHECK(strcmp(text, "raster 800 x 600 cells, sheet buffers about 5.56 MiB")
        == 0);

    plan = s_plan(1250.0, 2500.0, 0.25);
    s_explain(&plan, text);
    CHECK(strcmp(text, "1250 x 2500 mm at 0.25 mm is 5000 x 10000 cells, "
        "more than 8192 along an axis; try 0.5 mm cells") == 0);

    plan = s_plan(4001.0, 4000.0, 1.0);
    s_explain(&plan, text);
    CHECK(strcmp(text, "4001 x 4000 mm at 1 mm is 16004000 cells, more than "
        "16000000; try 2 mm cells") == 0);

    limits.max_buffer_bytes = 1024U * 1024U;
    plan = bp_sheet_plan((bp_sheet_plan_args_t) {
        .width_mm = 800.0, .height_mm = 600.0, .cell_size_mm = 1.0,
        .limits = limits
    });
    bp_sheet_explain(&plan, limits, text, sizeof text);
    CHECK(strcmp(text, "800 x 600 mm at 1 mm needs 5.56 MiB of sheet "
        "buffers, more than 1.00 MiB; try 5 mm cells") == 0);

    plan = s_plan(0.5, 600.0, 1.0);
    s_explain(&plan, text);
    CHECK(strcmp(text, "0.5 x 600 mm holds less than one 1 mm cell along an "
        "axis") == 0);
    plan = s_plan(NAN, 600.0, 1.0);
    s_explain(&plan, text);
    CHECK(strcmp(text, "sheet size must be positive and finite") == 0);
    plan = s_plan(800.0, 600.0, -1.0);
    s_explain(&plan, text);
    CHECK(strcmp(text, "cell size must be positive and finite") == 0);

    /* The widest figures still fit the documented capacity. */
    plan = s_plan(1e300, 1e300, 1e-300);
    s_explain(&plan, text);
    CHECK(strstr(text, "no cell size up to 10 mm fits") != NULL);
    plan = s_plan(1e6, 1e6, 1.0);
    s_explain(&plan, text);
    CHECK(strstr(text, "no cell size up to 10 mm fits") != NULL);
}

/* Every status has its own message. */
static void
s_test_status_text(void)
{
    int status;

    for (status = BP_SHEET_OK; status <= BP_SHEET_BYTES_OVER_LIMIT; ++status) {
        const char *text = bp_sheet_status_text((bp_sheet_status_t) status);

        CHECK(text != NULL && strcmp(text, "unknown status") != 0);
    }
}

/* A zero limit is a configuration defect, not a user input. */
static void
s_body_zero_limit(void)
{
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    limits.max_total_cells = 0U;
    (void) bp_sheet_plan((bp_sheet_plan_args_t) {
        .width_mm = 1.0, .height_mm = 1.0, .cell_size_mm = 1.0,
        .limits = limits
    });
}

/* A cell limit the byte arithmetic cannot represent is refused up front. */
static void
s_body_unrepresentable_limit(void)
{
    bp_sheet_limits_t limits = bp_sheet_default_limits();

    limits.max_total_cells = UINT64_MAX;
    (void) bp_sheet_plan((bp_sheet_plan_args_t) {
        .width_mm = 1.0, .height_mm = 1.0, .cell_size_mm = 1.0,
        .limits = limits
    });
}

/* The byte helper takes cells, which the caller has already validated. */
static void
s_body_zero_width_bytes(void)
{
    (void) bp_sheet_buffer_bytes(0U, 10U);
}

/* A suggestion for a size that is not even valid is a caller defect. */
static void
s_body_suggest_nan(void)
{
    (void) bp_sheet_suggest_cell_size_mm(NAN, 10.0,
        bp_sheet_default_limits());
}

/* A buffer smaller than the documented capacity is a caller defect. */
static void
s_body_small_explain_buffer(void)
{
    char text[BP_SHEET_EXPLAIN_CAPACITY - 1U];
    bp_sheet_plan_t plan = s_plan(800.0, 600.0, 1.0);

    bp_sheet_explain(&plan, bp_sheet_default_limits(), text, sizeof text);
}

/* Exercises sheet planning, its limits and its contracts. */
int
main(void)
{
    s_test_default_limits();
    s_test_plan_table();
    s_test_presets_both_orientations();
    s_test_word_boundaries();
    s_test_rounding();
    s_test_bad_input();
    s_test_limits_exact();
    s_test_suggestion();
    s_test_status_text();
    s_test_explain();
    t_expect_abort(s_body_small_explain_buffer, "small explain buffer");
    t_expect_abort(s_body_zero_limit, "zero sheet limit");
    t_expect_abort(s_body_unrepresentable_limit, "unrepresentable limit");
    t_expect_abort(s_body_zero_width_bytes, "zero width buffer bytes");
    t_expect_abort(s_body_suggest_nan, "NaN suggestion");
    (void) printf("test_bp_sheet: all checks passed\n");
    return 0;
}
