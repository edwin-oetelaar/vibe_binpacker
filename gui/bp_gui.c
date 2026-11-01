/*
 * Viewer for the nester: load a job, pack it, and look at the result.
 *
 * The point of this window is not decoration. A packing is a spatial thing and
 * a column of numbers hides everything that matters about it - which corner
 * went wrong, which part is stranded, whether the waste is one usable strip or
 * scattered slivers. The step slider replays the placement order, which is the
 * fastest way to see a heuristic make a bad decision.
 *
 * This file is plain C. cimgui is the generated C API for Dear ImGui; the C++
 * behind it is compiled separately and never appears here. Parts are drawn
 * with the ImGui draw list rather than through any graphics API of our own,
 * so there are no shaders, no textures and no resize handling to get wrong:
 * bp_mask_run_height turns a footprint into a handful of rectangles and those
 * go straight into the draw list.
 */
/*
 * _XOPEN_SOURCE rather than _POSIX_C_SOURCE: this toolchain's headers hide
 * realpath behind the X/Open guard, and the file browser needs it to fold the
 * parent steps out of a path before showing it.
 */
#define _XOPEN_SOURCE 700
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS

#include "cimgui.h"
#include "cimgui_impl.h"

#include "bp_contract.h"
#include "bp_job.h"
#include "bp_mask.h"
#include "bp_multisheet.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_score.h"
#include "bp_search.h"
#include "bp_sheet.h"
#include "bp_view.h"

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define igGetIO igGetIO_Nil

#define S_PATH_MAX 512U
#define S_NAME_MAX 256U
#define S_BROWSE_MAX 512U

/* Which modal wants opening; popups can only be opened from inside a frame. */
typedef enum {
    S_POPUP_NONE = 0,
    S_POPUP_OPEN = 1,
    S_POPUP_SAVE = 2,
    S_POPUP_QUIT = 3,
    S_POPUP_ABOUT = 4,
    S_POPUP_SAVE_QUIT = 5
} s_popup_t;

/* One line of the file browser. */
typedef struct {
    char name[S_NAME_MAX];
    int is_directory;
} s_entry_t;

/*
 * A directory listing, filtered to job files. Dear ImGui has no file dialog
 * and does not want one; a browser this small is less code than pulling in a
 * library, and it only ever has to find a .job.
 */
typedef struct {
    /* realpath writes here, and it insists on a full PATH_MAX buffer. */
    char directory[PATH_MAX];
    s_entry_t entries[S_BROWSE_MAX];
    int count;
    int selected;
} s_browser_t;

/* Distinct enough to tell neighbouring parts apart, matching the SVG writer. */
static const ImU32 s_palette[] = {
    0xffa8784c, 0xff1885f5, 0xff4ba254, 0xffa279b2,
    0xff5657e4, 0xffb2b772, 0xff3bcaee, 0xff5d759d
};

#define S_PALETTE_SIZE (sizeof s_palette / sizeof s_palette[0])

/*
 * Plate sizes offered in the geometry combo. The first entry keeps the job
 * file's own sheet line and the last one is whatever was typed; a preset only
 * fills the fields, so choosing one is never more than typing its numbers.
 */
typedef struct {
    const char *label;
    double width_mm;
    double height_mm;
} s_preset_t;

static const s_preset_t s_presets[] = {
    { "from job file", 0.0, 0.0 },
    { "500 x 500 mm", 500.0, 500.0 },
    { "600 x 600 mm", 600.0, 600.0 },
    { "600 x 300 mm", 600.0, 300.0 },
    { "800 x 600 mm - laser", 800.0, 600.0 },
    { "1200 x 1200 mm", 1200.0, 1200.0 },
    { "1250 x 2500 mm", 1250.0, 2500.0 },
    { "custom", 0.0, 0.0 }
};

#define S_PRESET_COUNT ((int) (sizeof s_presets / sizeof s_presets[0]))
#define S_PRESET_JOB 0
#define S_PRESET_CUSTOM (S_PRESET_COUNT - 1)

/* Everything the window shows and everything it lets you change. */
typedef struct {
    char job_path[S_PATH_MAX];
    char status[S_PATH_MAX + 256U];
    char loaded_path[S_PATH_MAX];
    bp_job_t *job;
    /* Allocated with the job, before publishing it; reused by every solve. */
    bp_sheet_layout_t *sheets;
    bp_sheet_layout_t *scratch_sheets;

    /*
     * Geometry controls. They take effect only when a job is (re)built, by
     * Load or by Apply & Pack; until then the published result keeps the size
     * it was packed on.
     */
    int size_preset;
    double sheet_width_mm;
    double sheet_height_mm;
    bool cell_from_job;
    double cell_size_mm;

    /* Packing controls. */
    int sort_index;
    int mode_index;
    bool multiple_sheets;
    int max_sheets;
    bool use_search;
    bool fixed_rounds;
    int search_rounds;
    float time_budget_seconds;
    float compact_fraction;
    float blink_rate;
    int max_ruin;

    /* Published result is independent of pending control edits and bitmap scratch. */
    bp_view_t result;
    char result_settings[192];
    double seconds;
    uint32_t iterations_run;
    uint32_t improvements;

    /* View. */
    float zoom;
    ImVec2 pan;
    int fit_pending;
    bool show_labels;
    bool show_box;

    /* Menus, dialogs and leaving. */
    s_popup_t pending_popup;
    s_browser_t browser;
    char save_path[S_PATH_MAX];
    /* A cancelled or failed save must not complete the pending quit action. */
    int quit_after_save;
    int quit;
} s_app_t;

/* Builds an ImVec2 without needing the C++ constructor. */
static ImVec2
s_vec(float x, float y)
{
    ImVec2 value;

    value.x = x;
    value.y = y;
    return value;
}

/* Returns the preset with exactly these dimensions, or the custom entry. */
static int
s_preset_for(double width_mm, double height_mm)
{
    int index;

    for (index = S_PRESET_JOB + 1; index < S_PRESET_CUSTOM; ++index)
        if (s_presets[index].width_mm == width_mm
            && s_presets[index].height_mm == height_mm)
            return index;
    return S_PRESET_CUSTOM;
}

/*
 * Shows the job file's own values in the fields that are set to follow it, so
 * switching to custom starts from what is actually in force.
 */
static void
s_sync_geometry_fields(s_app_t *app)
{
    BP_REQUIRE(app && app->job);
    if (app->size_preset == S_PRESET_JOB) {
        app->sheet_width_mm = app->job->file_sheet_width_mm;
        app->sheet_height_mm = app->job->file_sheet_height_mm;
    }
    if (app->cell_from_job)
        app->cell_size_mm = app->job->file_cell_size_mm;
}

/*
 * Builds a job from path with the geometry controls, and commits it only when
 * everything is allocated; any failure retains the old job and view.
 *
 * A typed zero would mean "no override" to the loader, so typed values are
 * checked here first: an empty field must never quietly mean the job file.
 */
static bp_job_load_result_t
s_load(s_app_t *app, const char *path)
{
    char chosen[S_PATH_MAX];
    char explanation[BP_SHEET_EXPLAIN_CAPACITY];
    int sheet_override;
    bp_job_load_result_t loaded;
    bp_sheet_layout_t *sheets;
    bp_sheet_layout_t *scratch;

    BP_REQUIRE(app);
    BP_REQUIRE(path && strlen(path) < sizeof chosen);
    /* path may be app->loaded_path, which a successful load overwrites. */
    memcpy(chosen, path, strlen(path) + 1U);
    sheet_override = app->size_preset != S_PRESET_JOB;
    if (sheet_override && (!isfinite(app->sheet_width_mm)
        || !isfinite(app->sheet_height_mm) || !(app->sheet_width_mm > 0.0)
        || !(app->sheet_height_mm > 0.0))) {
        (void) snprintf(app->status, sizeof app->status, "not applied: %s",
            bp_sheet_status_text(BP_SHEET_BAD_SIZE));
        return (bp_job_load_result_t) { .status = BP_JOB_BAD_OVERRIDE };
    }
    if (!app->cell_from_job && (!isfinite(app->cell_size_mm)
        || !(app->cell_size_mm > 0.0))) {
        (void) snprintf(app->status, sizeof app->status, "not applied: %s",
            bp_sheet_status_text(BP_SHEET_BAD_CELL_SIZE));
        return (bp_job_load_result_t) { .status = BP_JOB_BAD_OVERRIDE };
    }
    loaded = bp_job_load_with((bp_job_load_args_t) {
        .path = chosen,
        .sheet_width_mm = sheet_override ? app->sheet_width_mm : 0.0,
        .sheet_height_mm = sheet_override ? app->sheet_height_mm : 0.0,
        .cell_size_mm = app->cell_from_job ? 0.0 : app->cell_size_mm,
        .limits = bp_sheet_default_limits()
    });
    if (loaded.status != BP_JOB_OK) {
        if (loaded.line_number > 0U) {
            (void) snprintf(app->status, sizeof app->status, "%s:%u: %s",
                chosen, loaded.line_number, bp_job_status_text(loaded.status));
        } else if (loaded.sheet_plan.status != BP_SHEET_OK) {
            bp_sheet_explain(&loaded.sheet_plan, bp_sheet_default_limits(),
                explanation, sizeof explanation);
            (void) snprintf(app->status, sizeof app->status,
                "not applied: %s", explanation);
        } else {
            (void) snprintf(app->status, sizeof app->status, "%s: %s",
                chosen, bp_job_status_text(loaded.status));
        }
        return loaded;
    }
    /* ImGui's local replay and integer controls use int, not uint32_t. */
    if (loaded.job->part_count > INT_MAX) {
        bp_job_destroy(&loaded.job);
        loaded.status = BP_JOB_TOO_LARGE;
        (void) snprintf(app->status, sizeof app->status, "too many parts for the viewer");
        return loaded;
    }
    sheets = calloc(loaded.job->part_count, sizeof *sheets);
    scratch = calloc(loaded.job->part_count, sizeof *scratch);
    if (!sheets || !scratch) {
        free(sheets);
        free(scratch);
        bp_job_destroy(&loaded.job);
        loaded.status = BP_JOB_OUT_OF_MEMORY;
        (void) snprintf(app->status, sizeof app->status, "out of memory for plate results");
        return loaded;
    }
    app->result = (bp_view_t) { 0 };
    if (app->job)
        bp_job_destroy(&app->job);
    free(app->sheets);
    free(app->scratch_sheets);
    app->job = loaded.job;
    app->sheets = sheets;
    app->scratch_sheets = scratch;
    memcpy(app->loaded_path, chosen, strlen(chosen) + 1U);
    app->result_settings[0] = '\0';
    s_sync_geometry_fields(app);
    app->fit_pending = 1;
    (void) snprintf(app->status, sizeof app->status,
        "loaded %u parts on %.6g x %.6g mm%s at %.6g mm/cell%s",
        (unsigned) app->job->part_count, app->job->sheet_width_mm,
        app->job->sheet_height_mm, sheet_override ? " (override)" : "",
        app->job->cell_size_mm, app->cell_from_job ? "" : " (override)");
    return loaded;
}

/*
 * Reports whether the geometry controls ask for something other than the job
 * in force. Compared on the effective sizes rather than on the controls, so
 * choosing a preset equal to the current size is not a pending change.
 */
static int
s_geometry_pending(const s_app_t *app)
{
    const bp_job_t *job;
    double width_mm;
    double height_mm;
    double cell_size_mm;

    BP_REQUIRE(app);
    job = app->job;
    if (!job)
        return 0;
    width_mm = app->size_preset == S_PRESET_JOB ? job->file_sheet_width_mm
        : app->sheet_width_mm;
    height_mm = app->size_preset == S_PRESET_JOB ? job->file_sheet_height_mm
        : app->sheet_height_mm;
    cell_size_mm = app->cell_from_job ? job->file_cell_size_mm
        : app->cell_size_mm;
    return width_mm != job->sheet_width_mm || height_mm != job->sheet_height_mm
        || cell_size_mm != job->cell_size_mm;
}

/* Checks the packing controls, so a refused pack never costs the old result. */
static int
s_pack_controls_valid(s_app_t *app)
{
    BP_REQUIRE(app);
    if ((app->multiple_sheets && app->max_sheets <= 0)
        || (app->use_search && app->fixed_rounds && app->search_rounds <= 0)) {
        (void) snprintf(app->status, sizeof app->status, "plate limit and search rounds must be positive");
        return 0;
    }
    if (app->use_search && !app->fixed_rounds && (!isfinite(app->time_budget_seconds)
        || app->time_budget_seconds <= 0.0f)) {
        (void) snprintf(app->status, sizeof app->status, "search budget must be finite and positive");
        return 0;
    }
    return 1;
}

/* Runs one pass or the full search, leaving a packed placement list. */
static void
s_pack(s_app_t *app)
{
    static const bp_nest_sort_t sorts[] = {
        BP_NEST_SORT_AREA_DESCENDING,
        BP_NEST_SORT_LONGEST_SIDE_DESCENDING,
        BP_NEST_SORT_INPUT_ORDER
    };
    static const bp_place_mode_t modes[] = {
        BP_PLACE_JUMP, BP_PLACE_EROSION, BP_PLACE_REFERENCE
    };
    bp_job_t *job;
    clock_t started;
    uint32_t placement_count;
    uint32_t sheet_count;
    uint32_t iterations;

    BP_REQUIRE(app);
    job = app->job;
    if (!job || !s_pack_controls_valid(app))
        return;
    started = clock();
    iterations = app->use_search
        ? (app->fixed_rounds ? (uint32_t) app->search_rounds : UINT32_MAX) : 0U;
    app->iterations_run = 0U;
    app->improvements = 0U;

    if (app->multiple_sheets) {
        bp_multisheet_result_t result = bp_multisheet_solve((bp_multisheet_args_t) {
            .sheet = &job->sheet, .parts = job->parts, .part_count = job->part_count,
            .max_sheets = (uint32_t) app->max_sheets,
            .sort = sorts[app->sort_index], .place_mode = modes[app->mode_index],
            .iterations = iterations,
            .time_budget_seconds = app->use_search && !app->fixed_rounds
                ? (double) app->time_budget_seconds : 0.0,
            .seed = UINT64_C(0x5deece66d),
            .scratch_order = job->order, .scratch_best_order = job->scratch_weights,
            .scratch_assigned = job->scratch_placed, .scratch_placements = job->scratch_best,
            .scratch_sheets = app->scratch_sheets,
            .scratch_rows = job->scratch_rows, .scratch_row_capacity = job->scratch_row_capacity,
            .placements = job->placements, .sheets = app->sheets,
            .part_capacity = job->part_count, .sheet_capacity = job->part_count
        });

        placement_count = result.placed_count;
        sheet_count = result.sheet_count;
        app->iterations_run = result.iterations_run;
        app->improvements = result.improvements;
    } else if (app->use_search) {
        bp_search_result_t result;
        uint32_t index;

        result = bp_search_solve((bp_search_args_t) {
            .sheet = &job->sheet,
            .parts = job->parts,
            .part_count = job->part_count,
            .weights = bp_score_default_weights(),
            .height_window = 1U,
            .max_positions = 48U,
            .max_ruin = (uint32_t) app->max_ruin,
            .blink_rate = (double) app->blink_rate,
            .iterations = iterations,
            .time_budget_seconds = app->fixed_rounds ? 0.0 : (double) app->time_budget_seconds,
            .seed = UINT64_C(0x5deece66d),
            .scratch_order = job->order,
            .scratch_placed = job->scratch_placed,
            .scratch_best_placed = job->scratch_best_placed,
            .scratch_best = job->scratch_best,
            .scratch_capacity = job->part_count,
            .scratch_rows = job->scratch_rows,
            .scratch_row_capacity = job->scratch_row_capacity,
            .scratch_counts = app->compact_fraction > 0.0f
                ? job->scratch_counts
                : NULL,
            .scratch_count_capacity = job->scratch_count_capacity,
            .scratch_prefix = job->scratch_prefix,
            .scratch_prefix_capacity = job->scratch_prefix_capacity,
            .scratch_weights = job->scratch_weights,
            .scratch_overlap = job->scratch_overlap,
            .compact_fraction = (double) app->compact_fraction,
            .placements = job->placements,
            .placed = job->placed,
            .placement_capacity = job->part_count
        });

        /* The drawing loop wants a list, not one entry per part. */
        placement_count = 0U;
        for (index = 0U; index < job->part_count; ++index)
            if (job->placed[index])
                job->placements[placement_count++] =
                    job->placements[index];

        sheet_count = placement_count > 0U ? 1U : 0U;
        app->iterations_run = result.iterations_run;
        app->improvements = result.improvements;
    } else {
        bp_nester_solve_result_t result = bp_nester_solve(
            (bp_nester_solve_args_t) {
                .sheet = &job->sheet,
                .parts = job->parts,
                .part_count = job->part_count,
                .sort = sorts[app->sort_index],
                .place_mode = modes[app->mode_index],
                .scratch_order = job->order,
                .scratch_capacity = job->part_count,
                .scratch_rows = job->scratch_rows,
                .scratch_row_capacity = job->scratch_row_capacity,
                .placements = job->placements,
                .placement_capacity = job->part_count
            });

        placement_count = result.placed_count;
        sheet_count = placement_count > 0U ? 1U : 0U;
    }

    if (!app->multiple_sheets && sheet_count > 0U)
        app->sheets[0] = (bp_sheet_layout_t) { .placement_count = placement_count };
    app->result = bp_view_publish((bp_view_publish_args_t) {
        .parts = job->parts, .labels = job->labels, .part_count = job->part_count,
        .placements = job->placements, .placement_count = placement_count,
        .sheets = app->sheets, .sheet_count = sheet_count, .sheet_capacity = job->part_count,
        .placed = job->placed, .placed_capacity = job->part_count,
        .sheet_width_cells = job->sheet_width_cells,
        .sheet_height_cells = job->sheet_height_cells, .cell_size_mm = job->cell_size_mm,
        .multiple_sheets = app->multiple_sheets
    });
    app->seconds = (double) (clock() - started) / CLOCKS_PER_SEC;
    (void) snprintf(app->result_settings, sizeof app->result_settings,
        "%s; limit %u; %s", app->multiple_sheets ? "multi: count, then last W * H"
            : "single: area, then height",
        app->multiple_sheets ? (unsigned) app->max_sheets : 1U,
        app->use_search ? "optimised" : "construction");
    if (app->use_search) {
        size_t used = strlen(app->result_settings);

        if (app->fixed_rounds)
            (void) snprintf(app->result_settings + used, sizeof app->result_settings - used,
                "; budget %u rounds", (unsigned) app->search_rounds);
        else
            (void) snprintf(app->result_settings + used, sizeof app->result_settings - used,
                "; budget %.1f CPU s", (double) app->time_budget_seconds);
    }
    (void) snprintf(app->status, sizeof app->status,
        "packed %u of %u parts on %u plates in %.3f CPU s",
        (unsigned) placement_count, (unsigned) job->part_count,
        (unsigned) sheet_count, app->seconds);
}

/*
 * The Pack action: a pending geometry change rebuilds the job in force first.
 * Every check runs before the old job is touched, so a refused size or a bad
 * packing control leaves the previous result shown and exportable.
 */
static void
s_pack_or_apply(s_app_t *app)
{
    BP_REQUIRE(app);
    if (!app->job || !s_pack_controls_valid(app))
        return;
    if (s_geometry_pending(app)) {
        char path[S_PATH_MAX];

        memcpy(path, app->loaded_path, strlen(app->loaded_path) + 1U);
        if (s_load(app, path).status != BP_JOB_OK)
            return;
    }
    s_pack(app);
}

/*
 * Joins a directory and a name into out, or returns 0 when it will not fit.
 * Written with memcpy rather than snprintf because the compiler cannot see
 * that a truncated path is impossible here, and a silently shortened path is
 * exactly the kind of thing that opens the wrong file.
 */
static int
s_join(char *out, size_t capacity, const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);

    if (directory_length + 1U + name_length + 1U > capacity)
        return 0;
    memcpy(out, directory, directory_length);
    out[directory_length] = '/';
    memcpy(out + directory_length + 1U, name, name_length + 1U);
    return 1;
}

/* Orders the listing: parent first, then directories, then job files. */
static int
s_compare_entries(const void *first, const void *second)
{
    const s_entry_t *a = first;
    const s_entry_t *b = second;

    if (strcmp(a->name, "..") == 0)
        return -1;
    if (strcmp(b->name, "..") == 0)
        return 1;
    if (a->is_directory != b->is_directory)
        return a->is_directory ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Reports whether a name ends in the job file suffix. */
static int
s_is_job(const char *name)
{
    size_t length = strlen(name);

    return length > 4U && strcmp(name + length - 4, ".job") == 0;
}

/* Reads the browser's current directory into its listing. */
static void
s_browser_scan(s_browser_t *browser)
{
    DIR *directory = opendir(browser->directory);
    struct dirent *entry;

    browser->count = 0;
    browser->selected = -1;
    if (!directory)
        return;

    while ((entry = readdir(directory)) != NULL
        && browser->count < (int) S_BROWSE_MAX) {
        char joined[PATH_MAX];
        DIR *probe;
        int is_directory;

        if (strcmp(entry->d_name, ".") == 0)
            continue;
        if (!s_join(joined, sizeof joined, browser->directory, entry->d_name))
            continue;

        /* Opening it is the portable way to ask whether it is a directory. */
        probe = opendir(joined);
        is_directory = probe != NULL;
        if (probe)
            (void) closedir(probe);

        if (!is_directory && !s_is_job(entry->d_name))
            continue;
        (void) snprintf(browser->entries[browser->count].name, S_NAME_MAX,
            "%s", entry->d_name);
        browser->entries[browser->count].is_directory = is_directory;
        ++browser->count;
    }
    (void) closedir(directory);
    qsort(browser->entries, (size_t) browser->count,
        sizeof browser->entries[0], s_compare_entries);
}

/* Points the browser at a directory, resolving any parent steps in it. */
static void
s_browser_enter(s_browser_t *browser, const char *name)
{
    char joined[PATH_MAX];
    char resolved[PATH_MAX];

    if (s_join(joined, sizeof joined, browser->directory, name)
        && realpath(joined, resolved))
        memcpy(browser->directory, resolved, strlen(resolved) + 1U);
    s_browser_scan(browser);
}

/* Points the app at a job file inside the browser's directory and packs it. */
static void
s_choose_job(s_app_t *app, const char *name)
{
    char chosen[PATH_MAX];

    if (!s_join(chosen, sizeof chosen, app->browser.directory, name)
        || strlen(chosen) >= sizeof app->job_path) {
        (void) snprintf(app->status, sizeof app->status,
            "path too long: %s", name);
        return;
    }
    memcpy(app->job_path, chosen, strlen(chosen) + 1U);
    if (s_pack_controls_valid(app) && s_load(app, app->job_path).status == BP_JOB_OK)
        s_pack(app);
}

/* Exports every full plate, retaining unsaved state and the dialog on I/O failure. */
static bp_view_export_result_t
s_save_svg(s_app_t *app)
{
    char path[S_PATH_MAX + 32U];
    bp_view_export_result_t result;

    BP_REQUIRE(app);
    if (!app->result.parts || app->result.placement_count == 0U) {
        (void) snprintf(app->status, sizeof app->status,
            "nothing to save: pack something first");
        return (bp_view_export_result_t) { .status = BP_VIEW_EXPORT_EMPTY };
    }
    result = bp_view_export((bp_view_export_args_t) {
        .view = &app->result, .path = app->save_path,
        .scratch_path = path, .path_capacity = sizeof path
    });
    if (result.status != BP_VIEW_EXPORT_OK) {
        (void) snprintf(app->status, sizeof app->status,
            "export failed after %u plates: %s%s (%s)", (unsigned) result.written_count,
            result.status == BP_VIEW_EXPORT_PATH_TOO_LONG ? "path too long: " : "",
            path[0] ? path : app->save_path,
            result.system_error ? strerror(result.system_error) : "check the output path");
        return result;
    }
    (void) snprintf(app->status, sizeof app->status, "saved all %u plates: %s",
        (unsigned) result.written_count, app->save_path);
    return result;
}

/*
 * Asks to leave. A packing that took twenty seconds of searching and has not
 * been written anywhere is worth one question; anything else closes at once.
 */
static void
s_request_quit(s_app_t *app)
{
    BP_REQUIRE(app);
    if (app->quit_after_save)
        return;
    if (app->result.placement_count > 0U && !app->result.saved)
        app->pending_popup = S_POPUP_QUIT;
    else
        app->quit = 1;
}

/*
 * Draws the modal dialogs and reports whether one is up, so the caller can
 * hold the keyboard shortcuts back. Ctrl+Q firing while the quit dialog asks
 * whether to quit would be its own answer.
 */
static int
s_draw_popups(s_app_t *app)
{
    ImGuiIO *io = igGetIO();
    ImVec2 centre = s_vec(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f);
    int modal_open = 0;

    if (app->pending_popup == S_POPUP_OPEN) {
        s_browser_scan(&app->browser);
        igOpenPopup_Str("Open job", 0);
    } else if (app->pending_popup == S_POPUP_SAVE
        || app->pending_popup == S_POPUP_SAVE_QUIT) {
        app->quit_after_save = app->pending_popup == S_POPUP_SAVE_QUIT;
        igOpenPopup_Str("Save SVG", 0);
    } else if (app->pending_popup == S_POPUP_QUIT) {
        igOpenPopup_Str("Quit", 0);
    } else if (app->pending_popup == S_POPUP_ABOUT) {
        igOpenPopup_Str("Controls", 0);
    }
    app->pending_popup = S_POPUP_NONE;

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Open job", NULL, 0)) {
        int index;

        modal_open = 1;

        igText("%s", app->browser.directory);
        if (igBeginListBox("##entries", s_vec(520.0f, 320.0f))) {
            for (index = 0; index < app->browser.count; ++index) {
                const s_entry_t *entry = &app->browser.entries[index];
                char label[S_NAME_MAX + 8U];

                if (entry->is_directory)
                    (void) snprintf(label, sizeof label, "[%s]", entry->name);
                else
                    (void) snprintf(label, sizeof label, " %s", entry->name);

                if (igSelectable_Bool(label,
                    app->browser.selected == index, 0, s_vec(0.0f, 0.0f)))
                    app->browser.selected = index;
                if (igIsItemHovered(0)
                    && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
                    if (entry->is_directory) {
                        s_browser_enter(&app->browser, entry->name);
                        break;
                    }
                    s_choose_job(app, entry->name);
                    igCloseCurrentPopup();
                    break;
                }
            }
            igEndListBox();
        }
        igText("double click to enter a folder or open a job");
        if (igButton("Open", s_vec(90.0f, 0.0f))
            && app->browser.selected >= 0
            && app->browser.selected < app->browser.count) {
            const s_entry_t *entry =
                &app->browser.entries[app->browser.selected];

            if (entry->is_directory) {
                s_browser_enter(&app->browser, entry->name);
            } else {
                s_choose_job(app, entry->name);
                igCloseCurrentPopup();
            }
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Cancel", s_vec(90.0f, 0.0f))
            || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            igCloseCurrentPopup();
        igEndPopup();
    }

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Save SVG", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        char directory[PATH_MAX];

        modal_open = 1;
        igPushTextWrapPos(560.0f);
        igText("export all %u full plates%s", (unsigned) app->result.sheet_count,
            app->quit_after_save ? " and quit after saving" : "");
        if (getcwd(directory, sizeof directory))
            igTextWrapped("Relative paths are saved in: %s", directory);
        else
            igTextWrapped("Cannot determine the working directory. Choose an absolute path.");
        if (app->result.multiple_sheets)
            igText("base name -> name-001.svg, name-002.svg, ...");
        igText("Output filename or full path (folder must exist):");
        igSetNextItemWidth(520.0f);
        (void) igInputText("##save", app->save_path, sizeof app->save_path, 0,
            NULL, NULL);
        if (igButton("Save", s_vec(90.0f, 0.0f))) {
            if (s_save_svg(app).status == BP_VIEW_EXPORT_OK) {
                igCloseCurrentPopup();
                if (app->quit_after_save)
                    app->quit = 1;
                app->quit_after_save = 0;
            }
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Cancel", s_vec(90.0f, 0.0f))
            || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
            app->quit_after_save = 0;
            igCloseCurrentPopup();
        }
        igTextWrapped("%s", app->status);
        igPopTextWrapPos();
        igEndPopup();
    }

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Quit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        modal_open = 1;
        igText("This packing has not been saved.");
        igSpacing();
        if (igButton("Save and quit", s_vec(130.0f, 0.0f))) {
            app->quit_after_save = 1;
            igCloseCurrentPopup();
            app->pending_popup = S_POPUP_SAVE_QUIT;
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Quit anyway", s_vec(110.0f, 0.0f))) {
            app->quit = 1;
            igCloseCurrentPopup();
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Cancel", s_vec(90.0f, 0.0f))
            || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            igCloseCurrentPopup();
        igTextWrapped("%s", app->status);
        igEndPopup();
    }

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Controls", NULL, 0)) {
        modal_open = 1;
        igText("drag              pan the sheet");
        igText("mouse wheel       zoom about the cursor");
        igText("plate list        select a plate");
        igText("step slider       replay this plate's final placement order");
        igText("yellow box        full-result bounds, not the visible prefix");
        igSpacing();
        igText("Ctrl+O  open      Ctrl+R  reload");
        igText("F5      pack      Ctrl+S  save SVG");
        igText("Ctrl+Q  quit");
        igSpacing();
        if (igButton("Close", s_vec(90.0f, 0.0f))
            || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            igCloseCurrentPopup();
        igEndPopup();
    }
    return modal_open;
}

/* Draws the menu bar and returns its height, so the panels start below it. */
static float
s_draw_menu(s_app_t *app)
{
    float height = 0.0f;

    if (igBeginMainMenuBar()) {
        height = igGetWindowHeight();
        if (igBeginMenu("File", true)) {
            if (igMenuItem_Bool("Open...", "Ctrl+O", false, true))
                app->pending_popup = S_POPUP_OPEN;
            if (igMenuItem_Bool("Reload", "Ctrl+R", false, true)) {
                if (s_pack_controls_valid(app)
                    && s_load(app, app->job_path).status == BP_JOB_OK)
                    s_pack(app);
            }
            if (igMenuItem_Bool(s_geometry_pending(app) ? "Apply & Pack" : "Pack",
                "F5", false, app->job != NULL))
                s_pack_or_apply(app);
            igSeparator();
            if (igMenuItem_Bool("Save SVG...", "Ctrl+S", false,
                app->result.placement_count > 0U))
                app->pending_popup = S_POPUP_SAVE;
            igSeparator();
            if (igMenuItem_Bool("Quit", "Ctrl+Q", false, true))
                s_request_quit(app);
            igEndMenu();
        }
        if (igBeginMenu("View", true)) {
            if (igMenuItem_Bool("Fit sheet", NULL, false, app->job != NULL))
                app->fit_pending = 1;
            (void) igMenuItem_BoolPtr("Part numbers", NULL, &app->show_labels,
                true);
            (void) igMenuItem_BoolPtr("Final bounding box", NULL, &app->show_box, true);
            igEndMenu();
        }
        if (igBeginMenu("Help", true)) {
            if (igMenuItem_Bool("Controls", NULL, false, true))
                app->pending_popup = S_POPUP_ABOUT;
            igEndMenu();
        }
        igEndMainMenuBar();
    }
    return height;
}

/* Applies the keyboard shortcuts the menu advertises. */
static void
s_handle_shortcuts(s_app_t *app)
{
    ImGuiIO *io = igGetIO();

    /* Not while the path field has focus, or typing would trigger menus. */
    if (io->WantTextInput)
        return;

    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false))
        s_request_quit(app);
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_O, false))
        app->pending_popup = S_POPUP_OPEN;
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_R, false)) {
        if (s_pack_controls_valid(app)
            && s_load(app, app->job_path).status == BP_JOB_OK)
            s_pack(app);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false)
        && app->result.placement_count > 0U)
        app->pending_popup = S_POPUP_SAVE;
    if (igIsKeyPressed_Bool(ImGuiKey_F5, false) && app->job)
        s_pack_or_apply(app);
}

/* Draws the sheet and the placements, and handles panning and zooming. */
static void
s_draw_sheet(s_app_t *app)
{
    bp_job_t *job = app->job;
    ImDrawList *draw = igGetWindowDrawList();
    ImGuiIO *io = igGetIO();
    ImVec2 origin;
    ImVec2 size;
    float sheet_width;
    float sheet_height;
    uint32_t index;

    if (app->result.sheet_count > 0U) {
        const bp_sheet_layout_t *sheet = bp_view_active(&app->result);
        double cell = app->result.cell_size_mm;

        igText("Plate %u / %u%s", (unsigned) app->result.active_sheet + 1U,
            (unsigned) app->result.sheet_count,
            app->result.active_sheet + 1U == app->result.sheet_count ? " - last plate" : "");
        igText("Final box: %.2f x %.2f mm = %.2f mm2; origin (%.2f, %.2f) mm",
            sheet->width_cells * cell, sheet->height_cells * cell,
            (double) sheet->width_cells * sheet->height_cells * cell * cell,
            sheet->min_x_cells * cell, sheet->min_y_cells * cell);
    } else {
        igText("%s", app->result.parts ? "No used plates - see unplaced parts" : "No packing yet");
    }
    origin = igGetCursorScreenPos();
    size = igGetContentRegionAvail();
    if (size.x < 32.0f)
        size.x = 32.0f;
    if (size.y < 32.0f)
        size.y = 32.0f;
    igInvisibleButton("canvas", size, 0);

    if (!job) {
        ImDrawList_AddText_Vec2(draw, s_vec(origin.x + 12.0f,
            origin.y + 12.0f), 0xff8080ff, "no job loaded", NULL);
        return;
    }

    /* Fit the whole sheet the first time, and whenever a job is loaded. */
    if (app->fit_pending) {
        float by_width = size.x / (float) job->sheet_width_cells;
        float by_height = size.y / (float) job->sheet_height_cells;

        app->zoom = (by_width < by_height ? by_width : by_height) * 0.94f;
        app->pan = s_vec(0.0f, 0.0f);
        app->fit_pending = 0;
    }

    if (igIsItemHovered(0)) {
        if (io->MouseWheel != 0.0f) {
            /* Zoom about the cursor, so what you point at stays put. */
            float before = app->zoom;
            float factor = io->MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f;
            float focus_x = io->MousePos.x - origin.x - app->pan.x;
            float focus_y = io->MousePos.y - origin.y - app->pan.y;

            app->zoom *= factor;
            if (app->zoom < 0.01f)
                app->zoom = 0.01f;
            app->pan.x -= focus_x * (app->zoom / before - 1.0f);
            app->pan.y -= focus_y * (app->zoom / before - 1.0f);
        }
        if (igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
            app->pan.x += io->MouseDelta.x;
            app->pan.y += io->MouseDelta.y;
        }
    }

    sheet_width = (float) job->sheet_width_cells * app->zoom;
    sheet_height = (float) job->sheet_height_cells * app->zoom;

    ImDrawList_PushClipRect(draw, origin,
        s_vec(origin.x + size.x, origin.y + size.y), true);
    ImDrawList_AddRectFilled(draw,
        s_vec(origin.x + app->pan.x, origin.y + app->pan.y),
        s_vec(origin.x + app->pan.x + sheet_width,
            origin.y + app->pan.y + sheet_height),
        0xff2a2a2a, 0.0f, 0);
    /* Signature is (rounding, thickness, flags); swapping the last two is
       a runtime assertion, not a compile error. */
    ImDrawList_AddRect(draw,
        s_vec(origin.x + app->pan.x, origin.y + app->pan.y),
        s_vec(origin.x + app->pan.x + sheet_width,
            origin.y + app->pan.y + sheet_height),
        0xff909090, 0.0f, 1.5f, 0);

    for (index = 0U; index < app->result.visible_count; ++index) {
        const bp_placement_t *spot = bp_view_placement(&app->result, index);
        const bp_part_t *part = &job->parts[spot->part_index];
        const bp_orientation_t *option = &part->orientations[spot->orientation];
        const bp_mask_t *mask = option->solid;
        uint32_t part_x = spot->x_cells + option->solid_offset_x_cells;
        uint32_t part_y = spot->y_cells + option->solid_offset_y_cells;
        ImU32 colour = s_palette[spot->part_index % S_PALETTE_SIZE];
        uint32_t row;

        for (row = 0U; row < mask->height_cells; ++row) {
            uint32_t entry;

            for (entry = mask->row_offsets[row];
                entry < mask->row_offsets[row + 1U]; ++entry) {
                bp_span_t span = mask->spans[entry];
                uint32_t run;
                float left;
                float right;
                float top;
                float bottom;

                /* Already drawn as part of the run starting one row below. */
                if (row > 0U && bp_mask_row_has_span(mask, row - 1U, span))
                    continue;
                run = bp_mask_run_height(mask, row, span);

                left = origin.x + app->pan.x
                    + (float) (part_x + span.x_cells) * app->zoom;
                right = left + (float) span.length_cells * app->zoom;
                /* Screen y grows downwards; sheet y grows upwards. */
                bottom = origin.y + app->pan.y
                    + (float) (job->sheet_height_cells - (part_y + row))
                        * app->zoom;
                top = bottom - (float) run * app->zoom;

                ImDrawList_AddRectFilled(draw, s_vec(left, top),
                    s_vec(right, bottom), colour, 0.0f, 0);
                /*
                 * Outline only what is big enough to have an inside. A
                 * staircase edge is one rectangle per row, and at a few
                 * pixels tall the border is the whole rectangle, which turns
                 * every diagonal part into a black smear.
                 */
                if (right - left >= 4.0f && bottom - top >= 4.0f)
                    ImDrawList_AddRect(draw, s_vec(left, top),
                        s_vec(right, bottom), 0xff101010, 0.0f, 1.0f, 0);
            }
        }

        if (app->show_labels) {
            char label[32];
            float centre_x = origin.x + app->pan.x
                + ((float) part_x + (float) mask->width_cells * 0.5f)
                    * app->zoom;
            float centre_y = origin.y + app->pan.y
                + ((float) job->sheet_height_cells - (float) part_y
                    - (float) mask->height_cells * 0.5f) * app->zoom;

            (void) snprintf(label, sizeof label, "%u",
                (unsigned) job->labels[spot->part_index]);
            ImDrawList_AddText_Vec2(draw,
                s_vec(centre_x - 6.0f, centre_y - 7.0f), 0xff101010, label,
                NULL);
        }
    }
    if (app->show_box && app->result.sheet_count > 0U) {
        const bp_sheet_layout_t *sheet = bp_view_active(&app->result);
        float left = origin.x + app->pan.x + (float) sheet->min_x_cells * app->zoom;
        float right = left + (float) sheet->width_cells * app->zoom;
        float bottom = origin.y + app->pan.y
            + (float) (job->sheet_height_cells - sheet->min_y_cells) * app->zoom;
        float top = bottom - (float) sheet->height_cells * app->zoom;

        /* This is the full solution's score box, even when replay hides parts. */
        ImDrawList_AddRect(draw, s_vec(left, top), s_vec(right, bottom),
            0xff40dfff, 0.0f, 2.0f, 0);
    }
    ImDrawList_PopClipRect(draw);
}

/* Lists completed plates; selection changes only the view, never solver buffers. */
static void
s_draw_plates(s_app_t *app)
{
    bp_view_t *view;
    uint32_t index;

    BP_REQUIRE(app);
    view = &app->result;
    if (view->sheet_count == 0U)
        return;
    if (igBeginTable("plates", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
        s_vec(0.0f, 0.0f), 0.0f)) {
        igTableSetupColumn("Plate", ImGuiTableColumnFlags_WidthStretch, 0.6f, 0);
        igTableSetupColumn("Parts", ImGuiTableColumnFlags_WidthStretch, 0.7f, 0);
        igTableSetupColumn("Used %", ImGuiTableColumnFlags_WidthStretch, 0.8f, 0);
        igTableSetupColumn("Box mm", ImGuiTableColumnFlags_WidthStretch, 1.7f, 0);
        igTableHeadersRow();
        for (index = 0U; index < view->sheet_count; ++index) {
            const bp_sheet_layout_t *sheet = &view->sheets[index];
            char label[32];
            double utilisation = (double) sheet->placed_area_cells
                / ((double) view->sheet_width_cells * view->sheet_height_cells);

            igTableNextRow(0, 0.0f);
            (void) igTableNextColumn();
            (void) snprintf(label, sizeof label, "%u%s", (unsigned) index + 1U,
                index + 1U == view->sheet_count ? "*" : "");
            if (igSelectable_Bool(label, view->active_sheet == index,
                ImGuiSelectableFlags_SpanAllColumns, s_vec(0.0f, 0.0f)))
                bp_view_select(view, index);
            (void) igTableNextColumn();
            igText("%u", (unsigned) sheet->placement_count);
            (void) igTableNextColumn();
            igText("%.1f", utilisation * 100.0);
            (void) igTableNextColumn();
            igText("%.1fx%.1f", sheet->width_cells * view->cell_size_mm,
                sheet->height_cells * view->cell_size_mm);
        }
        igEndTable();
    }
    igText("* last plate");
}

/*
 * Draws the plate size and raster controls with a live preview of what they
 * cost. The preview runs the loader's own planning, so a size it shows as
 * accepted is one Apply & Pack will accept.
 */
static void
s_draw_geometry(s_app_t *app)
{
    static const char *labels[S_PRESET_COUNT];
    const bp_job_t *job;
    char explanation[BP_SHEET_EXPLAIN_CAPACITY];
    int index;
    int known;

    BP_REQUIRE(app);
    job = app->job;
    for (index = 0; index < S_PRESET_COUNT; ++index)
        labels[index] = s_presets[index].label;

    igText("geometry (applies on Load or Apply & Pack)");
    if (igCombo_Str_arr("plate", &app->size_preset, labels, S_PRESET_COUNT, -1)) {
        if (app->size_preset == S_PRESET_JOB && job) {
            app->sheet_width_mm = job->file_sheet_width_mm;
            app->sheet_height_mm = job->file_sheet_height_mm;
        } else if (app->size_preset != S_PRESET_JOB
            && app->size_preset != S_PRESET_CUSTOM) {
            app->sheet_width_mm = s_presets[app->size_preset].width_mm;
            app->sheet_height_mm = s_presets[app->size_preset].height_mm;
        }
    }
    /*
     * Typing a size is choosing a custom one; nothing is sorted or rounded.
     * One line per setting keeps the panel short enough not to scroll, which
     * matters for keyboard navigation as much as for the eye.
     */
    igSetNextItemWidth(90.0f);
    if (igInputDouble("##width", &app->sheet_width_mm, 0.0, 0.0, "%.2f", 0))
        app->size_preset = S_PRESET_CUSTOM;
    igSameLine(0.0f, 6.0f);
    igText("x");
    igSameLine(0.0f, 6.0f);
    igSetNextItemWidth(90.0f);
    if (igInputDouble("##height", &app->sheet_height_mm, 0.0, 0.0, "%.2f", 0))
        app->size_preset = S_PRESET_CUSTOM;
    igSameLine(0.0f, 6.0f);
    igText("mm");
    igSameLine(0.0f, 8.0f);
    if (igButton("Swap W/H", s_vec(0.0f, 0.0f))) {
        double width_mm = app->sheet_width_mm;

        app->sheet_width_mm = app->sheet_height_mm;
        app->sheet_height_mm = width_mm;
        app->size_preset = S_PRESET_CUSTOM;
    }
    if (igCheckbox("cell from job", &app->cell_from_job) && app->cell_from_job
        && job)
        app->cell_size_mm = job->file_cell_size_mm;
    igSameLine(0.0f, 8.0f);
    igBeginDisabled(app->cell_from_job);
    igSetNextItemWidth(90.0f);
    (void) igInputDouble("##cell", &app->cell_size_mm, 0.0, 0.0, "%.3f", 0);
    igEndDisabled();
    igSameLine(0.0f, 6.0f);
    igText("mm cell");

    /* Without a job, "from job file" has no numbers to preview yet. */
    known = (app->size_preset != S_PRESET_JOB || job) && (!app->cell_from_job || job);
    if (known) {
        bp_sheet_plan_t plan = bp_sheet_plan((bp_sheet_plan_args_t) {
            .width_mm = app->size_preset == S_PRESET_JOB
                ? job->file_sheet_width_mm : app->sheet_width_mm,
            .height_mm = app->size_preset == S_PRESET_JOB
                ? job->file_sheet_height_mm : app->sheet_height_mm,
            .cell_size_mm = app->cell_from_job ? job->file_cell_size_mm
                : app->cell_size_mm,
            .limits = bp_sheet_default_limits()
        });

        bp_sheet_explain(&plan, bp_sheet_default_limits(), explanation,
            sizeof explanation);
        if (plan.status != BP_SHEET_OK)
            igPushStyleColor_U32(ImGuiCol_Text, 0xff6060ff);
        igTextWrapped("%s", explanation);
        if (plan.status != BP_SHEET_OK)
            igPopStyleColor(1);
    } else {
        igTextWrapped("size from the job file, shown once it is loaded");
    }
    if (s_geometry_pending(app)) {
        igPushStyleColor_U32(ImGuiCol_Text, 0xff40dfff);
        igTextWrapped("not applied yet: showing %.6g x %.6g mm at %.6g mm/cell",
            job->sheet_width_mm, job->sheet_height_mm, job->cell_size_mm);
        igPopStyleColor(1);
    }
}

/* Draws the control panel and the numbers that go with a packing. */
static void
s_draw_controls(s_app_t *app)
{
    static const char *const sort_names[] = {
        "area descending", "longest side", "input order"
    };
    static const char *const mode_names[] = {
        "jump scan", "erosion", "reference scan"
    };
    static const char *const packing_names[] = { "single plate", "multiple plates" };
    int packing = app->multiple_sheets ? 1 : 0;

    igText("job file");
    (void) igInputText("##path", app->job_path, sizeof app->job_path, 0, NULL,
        NULL);
    if (igButton("Load", s_vec(80.0f, 0.0f)))
        (void) s_load(app, app->job_path);
    igSameLine(0.0f, 8.0f);
    /* The ### keeps one widget identity while the label changes. */
    if (igButton(s_geometry_pending(app) ? "Apply & Pack###pack" : "Pack###pack",
        s_vec(100.0f, 0.0f)))
        s_pack_or_apply(app);
    igSameLine(0.0f, 8.0f);
    if (igButton("Fit view", s_vec(80.0f, 0.0f)))
        app->fit_pending = 1;

    igSeparator();
    s_draw_geometry(app);

    igSeparator();
    igText("packing (applies on Pack)");
    if (igCombo_Str_arr("packing", &packing, packing_names, 2, -1))
        app->multiple_sheets = packing == 1;
    if (app->multiple_sheets)
        (void) igInputInt("max plates", &app->max_sheets, 1, 4, 0);
    (void) igCheckbox("optimise", &app->use_search);
    if (app->use_search) {
        (void) igCheckbox("fixed rounds", &app->fixed_rounds);
        if (app->fixed_rounds)
            (void) igInputInt("rounds", &app->search_rounds, 1, 100, 0);
        else
            (void) igSliderFloat("CPU seconds", &app->time_budget_seconds, 0.2f,
                30.0f, "%.1f", 0);
        if (!app->multiple_sheets) {
            (void) igSliderFloat("squeeze share", &app->compact_fraction, 0.0f,
                1.0f, "%.2f", 0);
            (void) igSliderFloat("blink", &app->blink_rate, 0.0f, 0.4f, "%.3f", 0);
            (void) igSliderInt("ruin", &app->max_ruin, 1, 32, "%d", 0);
        }
    }
    if (app->multiple_sheets || !app->use_search) {
        (void) igCombo_Str_arr("order", &app->sort_index, sort_names, 3, -1);
        (void) igCombo_Str_arr("search", &app->mode_index, mode_names, 3, -1);
    }

    igSeparator();
    igText("view");
    (void) igCheckbox("part numbers", &app->show_labels);
    (void) igCheckbox("final bounding box", &app->show_box);
    s_draw_plates(app);
    if (app->result.sheet_count > 0U) {
        int step = (int) app->result.visible_count;

        if (igSliderInt("local step", &step, 0,
            (int) bp_view_active(&app->result)->placement_count, "%d", 0))
            bp_view_set_visible(&app->result, (uint32_t) step);
        igTextWrapped("Replay of the selected plate's final order. Yellow box = full result.");
    }

    igSeparator();
    if (app->result.parts) {
        const bp_view_t *view = &app->result;

        igTextWrapped("Result: %s", app->result_settings);
        igText("plates       %u", (unsigned) view->sheet_count);
        igText("placed       %u of %u", (unsigned) view->placement_count,
            (unsigned) view->part_count);
        igText("unplaced     %u (%u oversized)",
            (unsigned) (view->part_count - view->placement_count),
            (unsigned) view->oversized_count);
        igText("total used   %.2f %%", bp_view_utilisation(view) * 100.0);
        if (view->sheet_count > 0U) {
            const bp_sheet_layout_t *last = &view->sheets[view->sheet_count - 1U];

            igText("last W * H   %.2f mm2", (double) last->width_cells * last->height_cells
                * view->cell_size_mm * view->cell_size_mm);
        }
        igText("search       %u rounds, %u improvements", (unsigned) app->iterations_run,
            (unsigned) app->improvements);
        igText("time         %.3f CPU s", app->seconds);
        igText("export       %s", view->saved ? "all plates saved" : "not saved");
        if (view->placement_count < view->part_count
            && igCollapsingHeader_TreeNodeFlags("Unplaced parts", 0)) {
            uint32_t index;

            for (index = 0U; index < view->part_count; ++index)
                if (!view->placed[index])
                    igText("part %u, label %u", (unsigned) index + 1U,
                        (unsigned) view->labels[index]);
        }
    }
    if (app->job)
        igTextWrapped("Loaded: %s", app->loaded_path);
    igSeparator();
    igTextWrapped("%s", app->status);
}

/*
 * Picks the windowing backend and returns the name it settled on.
 *
 * GLFW prefers Wayland when WAYLAND_DISPLAY is set, and under WSLg that is the
 * broken path: its EGL cannot find a driver, falls back to zink, finds no
 * device, and the window ends up in the taskbar drawing nothing. Rendering is
 * fine either way - a framebuffer capture comes out correct - so the failure
 * is only in presenting, which makes it maddening to diagnose from inside.
 *
 * X11 through XWayland has none of that and works on native Wayland desktops
 * too, so it is the default. Pass --platform to override.
 */
static const char *
s_select_platform(const char *requested)
{
#ifdef GLFW_PLATFORM
    if (requested && strcmp(requested, "auto") == 0) {
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        return "auto";
    }
    if (requested && strcmp(requested, "wayland") == 0) {
        if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND)) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
            return "wayland";
        }
        (void) fprintf(stderr, "bp_gui: no Wayland support, using default\n");
        return "auto";
    }
    if (glfwPlatformSupported(GLFW_PLATFORM_X11)) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        return "x11";
    }
#else
    (void) requested;
#endif
    return "auto";
}

/*
 * Prints what the window and the driver actually are.
 *
 * A viewer runs on someone else's machine, and when nothing appears there is
 * nothing to go on from the inside: the render loop looks healthy, the
 * framebuffer reads back correct, and the screen stays empty. This prints the
 * handful of facts that separate the possible causes - which backend, which
 * driver, where the window thinks it is, and whether frames are actually being
 * produced.
 */
static void
s_diagnose(GLFWwindow *window, const char *platform, int frames, double seconds)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    int alpha_bits = 0;
    GLFWmonitor *monitor;

    (void) fprintf(stderr, "platform     %s\n", platform);
    (void) fprintf(stderr, "GL vendor    %s\n", (const char *) glGetString(GL_VENDOR));
    (void) fprintf(stderr, "GL renderer  %s\n", (const char *) glGetString(GL_RENDERER));
    (void) fprintf(stderr, "GL version   %s\n", (const char *) glGetString(GL_VERSION));

    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glGetIntegerv(GL_ALPHA_BITS, &alpha_bits);
    (void) fprintf(stderr, "window       %d x %d at (%d, %d)\n", width, height,
        x, y);
    (void) fprintf(stderr, "framebuffer  %d x %d, %d alpha bits\n",
        framebuffer_width, framebuffer_height, alpha_bits);
    (void) fprintf(stderr, "visible      %d, iconified %d, focused %d\n",
        glfwGetWindowAttrib(window, GLFW_VISIBLE),
        glfwGetWindowAttrib(window, GLFW_ICONIFIED),
        glfwGetWindowAttrib(window, GLFW_FOCUSED));

    monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        int area_x = 0;
        int area_y = 0;
        int area_width = 0;
        int area_height = 0;

        glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_width,
            &area_height);
        (void) fprintf(stderr, "monitor      %d x %d at (%d, %d)\n",
            area_width, area_height, area_x, area_y);
    } else {
        (void) fprintf(stderr, "monitor      none reported\n");
    }

    if (seconds > 0.0)
        (void) fprintf(stderr, "frames       %d in %.1f s (%.1f per second)\n",
            frames, seconds, frames / seconds);
}

/*
 * Writes the framebuffer as a binary PPM and reports whether it worked.
 *
 * PPM because it is eight lines of code and no dependency; the Python tooling
 * converts it to anything else. This exists so a window can be checked from a
 * script: a GUI that builds is not a GUI that draws, and the difference is
 * only visible in pixels.
 */
static int
s_write_shot(const char *path, int width, int height)
{
    FILE *out;
    unsigned char *pixels;
    int row;

    if (width <= 0 || height <= 0)
        return 0;
    pixels = malloc((size_t) width * (size_t) height * 3U);
    if (!pixels)
        return 0;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    out = fopen(path, "wb");
    if (!out) {
        free(pixels);
        return 0;
    }
    (void) fprintf(out, "P6\n%d %d\n255\n", width, height);
    /* OpenGL reads bottom row first; PPM wants the top row first. */
    for (row = height - 1; row >= 0; --row)
        if (fwrite(pixels + (size_t) row * (size_t) width * 3U, 1U,
            (size_t) width * 3U, out) != (size_t) width * 3U)
            break;
    free(pixels);
    {
        int failed = ferror(out);
        int closed = fclose(out);

        return !failed && closed == 0;
    }
}

/* Parses automation counters without truncation, overflow or ignored suffixes. */
static int
s_parse_count(const char *text)
{
    char *end;
    unsigned long count;

    BP_REQUIRE(text);
    count = strtoul(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || *end != '\0' || count > INT_MAX) {
        (void) fprintf(stderr, "bp_gui: invalid counter: %s\n", text);
        exit(1);
    }
    return (int) count;
}

/*
 * Parses one millimetre value from the command line. Only the syntax is
 * checked: whether the size is usable is decided by the same load path the
 * controls use, so the command line cannot accept what the window refuses.
 */
static double
s_parse_mm(const char *text)
{
    char *end;
    double value;

    BP_REQUIRE(text);
    value = strtod(text, &end);
    if (end == text || *end != '\0') {
        (void) fprintf(stderr, "bp_gui: invalid millimetre value: %s\n", text);
        exit(1);
    }
    return value;
}

/* Sets the geometry controls to a typed size, naming its preset if it has one. */
static void
s_set_sheet_size(s_app_t *app, double width_mm, double height_mm)
{
    BP_REQUIRE(app);
    app->sheet_width_mm = width_mm;
    app->sheet_height_mm = height_mm;
    app->size_preset = s_preset_for(width_mm, height_mm);
}

/* Opens a window, packs a job and shows it until the window is closed. */
int
main(int argc, char **argv)
{
    static s_app_t app;
    const char *shot_path = NULL;
    const char *requested_platform = NULL;
    const char *platform;
    int diagnose = 0;
    double diagnose_started = 0.0;
    int shot_after_frames = 0;
    int frame = 0;
    int argument;
    int selected_sheet = 1;
    int selected_explicitly = 0;
    int requested_step = -1;
    int want_export = 0;
    int want_apply = 0;
    double apply_width_mm = 0.0;
    double apply_height_mm = 0.0;
    int exit_code = 0;
    GLFWwindow *window;
    ImGuiIO *io;

    app.size_preset = S_PRESET_JOB;
    app.sheet_width_mm = 1200.0;
    app.sheet_height_mm = 1200.0;
    app.cell_from_job = true;
    app.cell_size_mm = 1.0;
    app.time_budget_seconds = 3.0f;
    app.compact_fraction = 0.4f;
    app.blink_rate = 0.08f;
    app.max_ruin = 8;
    app.max_sheets = 4;
    app.search_rounds = 128;
    app.zoom = 0.5f;
    app.show_labels = true;
    app.show_box = true;
    (void) snprintf(app.save_path, sizeof app.save_path, "%s",
        "nest.svg");
    if (!realpath(".", app.browser.directory))
        (void) snprintf(app.browser.directory, sizeof app.browser.directory,
            "%s", ".");
    (void) snprintf(app.job_path, sizeof app.job_path, "%s", "jobs/demo.job");
    (void) snprintf(app.status, sizeof app.status, "press Load to begin");

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--shot") == 0 && argument + 1 < argc) {
            shot_path = argv[++argument];
            if (shot_after_frames == 0)
                shot_after_frames = 4;
        } else if (strcmp(argv[argument], "--frames") == 0
            && argument + 1 < argc) {
            shot_after_frames = s_parse_count(argv[++argument]);
            if (shot_after_frames == 0)
                return 1;
        } else if (strcmp(argv[argument], "--sheets") == 0 && argument + 1 < argc) {
            app.max_sheets = s_parse_count(argv[++argument]);
            if (app.max_sheets == 0)
                return 1;
            app.multiple_sheets = true;
        } else if (strcmp(argv[argument], "--sheet") == 0 && argument + 1 < argc) {
            selected_sheet = s_parse_count(argv[++argument]);
            if (selected_sheet == 0)
                return 1;
            selected_explicitly = 1;
        } else if (strcmp(argv[argument], "--search") == 0 && argument + 1 < argc) {
            app.search_rounds = s_parse_count(argv[++argument]);
            app.use_search = app.search_rounds > 0;
            app.fixed_rounds = true;
        } else if (strcmp(argv[argument], "--time") == 0 && argument + 1 < argc) {
            char *end;

            app.time_budget_seconds = (float) strtod(argv[++argument], &end);
            if (*end != '\0' || !isfinite(app.time_budget_seconds)
                || app.time_budget_seconds <= 0.0f)
                return 1;
            app.use_search = true;
            app.fixed_rounds = false;
        } else if (strcmp(argv[argument], "--sheet-size") == 0
            && argument + 2 < argc) {
            double width_mm = s_parse_mm(argv[++argument]);

            s_set_sheet_size(&app, width_mm, s_parse_mm(argv[++argument]));
        } else if (strcmp(argv[argument], "--cell") == 0 && argument + 1 < argc) {
            app.cell_size_mm = s_parse_mm(argv[++argument]);
            app.cell_from_job = false;
        } else if (strcmp(argv[argument], "--apply-sheet-size") == 0
            && argument + 2 < argc) {
            apply_width_mm = s_parse_mm(argv[++argument]);
            apply_height_mm = s_parse_mm(argv[++argument]);
            want_apply = 1;
        } else if (strcmp(argv[argument], "--step") == 0 && argument + 1 < argc) {
            requested_step = s_parse_count(argv[++argument]);
        } else if (strcmp(argv[argument], "--svg") == 0 && argument + 1 < argc) {
            const char *path = argv[++argument];

            if (strlen(path) >= sizeof app.save_path) {
                (void) fprintf(stderr, "bp_gui: export path too long\n");
                return 1;
            }
            memcpy(app.save_path, path, strlen(path) + 1U);
            want_export = 1;
        } else if (strcmp(argv[argument], "--platform") == 0
            && argument + 1 < argc) {
            requested_platform = argv[++argument];
        } else if (strcmp(argv[argument], "--diagnose") == 0) {
            diagnose = 1;
        } else if (argv[argument][0] == '-') {
            (void) fprintf(stderr,
                "usage: bp_gui [job-file] [--platform x11|wayland|auto]\n"
                "              [--diagnose] [--shot <file.ppm>] "
                "[--frames <n>]\n"
                "              [--sheets <limit>] [--sheet <1-based index>]\n"
                "              [--sheet-size <w_mm> <h_mm>] [--cell <mm>]\n"
                "              [--apply-sheet-size <w_mm> <h_mm>]\n"
                "              [--search <rounds> | --time <CPU seconds>]\n"
                "              [--step <local count>] [--svg <base path>]\n");
            return 1;
        } else {
            if (strlen(argv[argument]) >= sizeof app.job_path) {
                (void) fprintf(stderr, "bp_gui: job path too long\n");
                return 1;
            }
            memcpy(app.job_path, argv[argument], strlen(argv[argument]) + 1U);
        }
    }

    platform = s_select_platform(requested_platform);
    if (!glfwInit()) {
        (void) fprintf(stderr, "bp_gui: cannot initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(1400, 900, "vibe_binpacker", NULL, NULL);
    if (!window) {
        (void) fprintf(stderr, "bp_gui: cannot open a window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    (void) igCreateContext(NULL);
    io = igGetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /*
     * No imgui.ini. Both windows are positioned and sized every frame, so
     * there is nothing to remember, and a settings file dropped next to
     * whatever directory you happened to run from is just litter.
     */
    io->IniFilename = NULL;
    igStyleColorsDark(NULL);
    (void) ImGui_ImplGlfw_InitForOpenGL(window, true);
    (void) ImGui_ImplOpenGL3_Init("#version 130");

    if (s_load(&app, app.job_path).status == BP_JOB_OK)
        s_pack(&app);
    else if (shot_path || want_export || selected_explicitly || want_apply) {
        (void) fprintf(stderr, "bp_gui: %s\n", app.status);
        exit_code = 1;
        app.quit = 1;
    }
    /*
     * Automation for the checks: pick a size after the first packing and
     * press Apply & Pack, exactly as a user would. A refusal is an expected
     * outcome to be checked, so it is reported but does not fail the run.
     */
    if (!app.quit && want_apply) {
        s_set_sheet_size(&app, apply_width_mm, apply_height_mm);
        s_pack_or_apply(&app);
        if (s_geometry_pending(&app))
            (void) fprintf(stderr, "bp_gui: %s\n", app.status);
    }
    if (!app.quit && selected_explicitly) {
        if ((uint32_t) selected_sheet > app.result.sheet_count) {
            (void) fprintf(stderr, "bp_gui: plate %d does not exist (%u used)\n",
                selected_sheet, (unsigned) app.result.sheet_count);
            exit_code = 1;
            app.quit = 1;
        } else {
            bp_view_select(&app.result, (uint32_t) selected_sheet - 1U);
        }
    }
    if (!app.quit && requested_step >= 0) {
        if (app.result.sheet_count == 0U
            || (uint32_t) requested_step > bp_view_active(&app.result)->placement_count) {
            (void) fprintf(stderr, "bp_gui: step outside selected plate\n");
            exit_code = 1;
            app.quit = 1;
        } else {
            bp_view_set_visible(&app.result, (uint32_t) requested_step);
        }
    }
    if (!app.quit && want_export && s_save_svg(&app).status != BP_VIEW_EXPORT_OK) {
        (void) fprintf(stderr, "bp_gui: %s\n", app.status);
        exit_code = 1;
        app.quit = 1;
    }
    /* Worth showing: it is the first thing to change when nothing appears. */
    (void) snprintf(app.status + strlen(app.status),
        sizeof app.status - strlen(app.status), "  [%s]", platform);

    if (diagnose)
        diagnose_started = glfwGetTime();

    while (!app.quit) {
        int width = 0;
        int height = 0;
        int modal_open = 0;
        float menu_height;

        glfwPollEvents();
        if (glfwWindowShouldClose(window)) {
            /* The window button goes through the same question as Quit. */
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            s_request_quit(&app);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        igNewFrame();

        menu_height = s_draw_menu(&app);

        glfwGetFramebufferSize(window, &width, &height);
        igSetNextWindowPos(s_vec(0.0f, menu_height), ImGuiCond_Always,
            s_vec(0.0f, 0.0f));
        igSetNextWindowSize(s_vec(360.0f, (float) height - menu_height),
            ImGuiCond_Always);
        if (igBegin("controls", NULL,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoCollapse)) {
            s_draw_controls(&app);
            /*
             * Opening and drawing a popup have to happen in the same ID
             * scope, and the menu bar is not one: both calls live here, and
             * the menu only raises a flag.
             */
            modal_open = s_draw_popups(&app);
        }
        igEnd();

        if (!modal_open)
            s_handle_shortcuts(&app);

        igSetNextWindowPos(s_vec(360.0f, menu_height), ImGuiCond_Always,
            s_vec(0.0f, 0.0f));
        igSetNextWindowSize(s_vec((float) width - 360.0f,
            (float) height - menu_height), ImGuiCond_Always);
        if (igBegin("sheet", NULL,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoBringToFrontOnFocus))
            s_draw_sheet(&app);
        igEnd();

        igRender();
        glViewport(0, 0, width, height);
        glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());

        /* Capture before the swap, while the frame is still the back buffer. */
        if (shot_path && frame + 1 >= shot_after_frames) {
            if (!s_write_shot(shot_path, width, height)) {
                (void) fprintf(stderr, "bp_gui: cannot write %s\n", shot_path);
                exit_code = 1;
            }
            glfwSwapBuffers(window);
            break;
        }
        glfwSwapBuffers(window);
        ++frame;
        if (shot_after_frames > 0 && frame >= shot_after_frames)
            break;

        /* Report after a few seconds of real frames, then leave. */
        if (diagnose && glfwGetTime() - diagnose_started >= 3.0) {
            s_diagnose(window, platform, frame,
                glfwGetTime() - diagnose_started);
            break;
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (app.job)
        bp_job_destroy(&app.job);
    free(app.sheets);
    free(app.scratch_sheets);
    return exit_code;
}
