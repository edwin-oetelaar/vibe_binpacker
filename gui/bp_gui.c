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

#include "bp_job.h"
#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_score.h"
#include "bp_search.h"
#include "bp_svg.h"

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    S_POPUP_ABOUT = 4
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

/* Everything the window shows and everything it lets you change. */
typedef struct {
    char job_path[S_PATH_MAX];
    char status[S_PATH_MAX + 256U];
    bp_job_t *job;

    /* Controls. */
    float cell_size_mm;
    int sort_index;
    int mode_index;
    bool use_search;
    float time_budget_seconds;
    float compact_fraction;
    float blink_rate;
    int max_ruin;

    /* Result, as a packed list so the drawing loop stays simple. */
    uint32_t placement_count;
    uint32_t placed_count;
    uint32_t unplaced_count;
    uint32_t used_width_cells;
    uint32_t used_height_cells;
    uint64_t placed_area_cells;
    double seconds;
    int step;

    /* View. */
    float zoom;
    ImVec2 pan;
    int fit_pending;
    bool show_labels;

    /* Menus, dialogs and leaving. */
    s_popup_t pending_popup;
    s_browser_t browser;
    char save_path[S_PATH_MAX];
    int saved_since_pack;
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

/* Releases any loaded job and reports why, so the window never lies. */
static void
s_load(s_app_t *app)
{
    bp_job_load_result_t loaded;

    if (app->job)
        bp_job_destroy(&app->job);
    app->placement_count = 0U;
    app->placed_count = 0U;
    app->unplaced_count = 0U;
    app->step = 0;

    loaded = bp_job_load(app->job_path,
        app->cell_size_mm > 0.0f ? (double) app->cell_size_mm : 0.0);
    if (loaded.status != BP_JOB_OK) {
        if (loaded.line_number > 0U)
            (void) snprintf(app->status, sizeof app->status, "%s:%u: %s",
                app->job_path, loaded.line_number,
                bp_job_status_text(loaded.status));
        else
            (void) snprintf(app->status, sizeof app->status, "%s: %s",
                app->job_path, bp_job_status_text(loaded.status));
        return;
    }
    app->job = loaded.job;
    app->cell_size_mm = (float) app->job->cell_size_mm;
    app->fit_pending = 1;
    (void) snprintf(app->status, sizeof app->status,
        "loaded %u parts on %.0f x %.0f mm at %.3f mm/cell",
        (unsigned) app->job->part_count, app->job->sheet_width_mm,
        app->job->sheet_height_mm, app->job->cell_size_mm);
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
    bp_job_t *job = app->job;
    clock_t started;

    if (!job)
        return;
    started = clock();

    if (app->use_search) {
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
            .iterations = UINT32_MAX,
            .time_budget_seconds = (double) app->time_budget_seconds,
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
        app->placement_count = 0U;
        for (index = 0U; index < job->part_count; ++index)
            if (job->placed[index])
                job->placements[app->placement_count++] =
                    job->placements[index];

        app->placed_count = result.placed_count;
        app->unplaced_count = result.unplaced_count;
        app->used_width_cells = result.used_width_cells;
        app->used_height_cells = result.used_height_cells;
        app->placed_area_cells = result.placed_area_cells;
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

        app->placement_count = result.placed_count;
        app->placed_count = result.placed_count;
        app->unplaced_count = result.unplaced_count;
        app->used_width_cells = result.used_width_cells;
        app->used_height_cells = result.used_height_cells;
        app->placed_area_cells = result.placed_area_cells;
    }

    app->seconds = (double) (clock() - started) / CLOCKS_PER_SEC;
    app->step = (int) app->placement_count;
    app->saved_since_pack = 0;
    (void) snprintf(app->status, sizeof app->status,
        "packed %u of %u parts in %.3f s", (unsigned) app->placed_count,
        (unsigned) job->part_count, app->seconds);
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
    s_load(app);
    if (app->job)
        s_pack(app);
}

/* Writes the current layout as SVG, and reports what happened. */
static void
s_save_svg(s_app_t *app)
{
    FILE *out;
    bp_svg_result_t result;

    if (!app->job || app->placement_count == 0U) {
        (void) snprintf(app->status, sizeof app->status,
            "nothing to save: pack something first");
        return;
    }
    out = fopen(app->save_path, "w");
    if (!out) {
        (void) snprintf(app->status, sizeof app->status, "cannot write %s",
            app->save_path);
        return;
    }
    result = bp_svg_write((bp_svg_write_args_t) {
        .out = out,
        .sheet_width_cells = app->job->sheet_width_cells,
        .sheet_height_cells = app->job->sheet_height_cells,
        .cell_size_mm = app->job->cell_size_mm,
        .parts = app->job->parts,
        .part_count = app->job->part_count,
        .placements = app->job->placements,
        .placement_count = app->placement_count,
        .labels = app->job->labels,
        .draw_labels = 1
    });
    if (fclose(out) != 0 || result.status != BP_SVG_OK) {
        (void) snprintf(app->status, sizeof app->status, "SVG write failed: %s",
            app->save_path);
        return;
    }
    app->saved_since_pack = 1;
    (void) snprintf(app->status, sizeof app->status, "wrote %s",
        app->save_path);
}

/*
 * Asks to leave. A packing that took twenty seconds of searching and has not
 * been written anywhere is worth one question; anything else closes at once.
 */
static void
s_request_quit(s_app_t *app)
{
    if (app->placement_count > 0U && !app->saved_since_pack)
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
    } else if (app->pending_popup == S_POPUP_SAVE) {
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
    if (igBeginPopupModal("Save SVG", NULL, 0)) {
        modal_open = 1;
        igText("write the current layout to");
        (void) igInputText("##save", app->save_path, sizeof app->save_path, 0,
            NULL, NULL);
        if (igButton("Save", s_vec(90.0f, 0.0f))) {
            s_save_svg(app);
            igCloseCurrentPopup();
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Cancel", s_vec(90.0f, 0.0f))
            || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            igCloseCurrentPopup();
        igEndPopup();
    }

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Quit", NULL, 0)) {
        modal_open = 1;
        igText("This packing has not been saved.");
        igSpacing();
        if (igButton("Save and quit", s_vec(130.0f, 0.0f))) {
            s_save_svg(app);
            app->quit = 1;
            igCloseCurrentPopup();
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
        igEndPopup();
    }

    igSetNextWindowPos(centre, ImGuiCond_Appearing, s_vec(0.5f, 0.5f));
    if (igBeginPopupModal("Controls", NULL, 0)) {
        modal_open = 1;
        igText("drag              pan the sheet");
        igText("mouse wheel       zoom about the cursor");
        igText("step slider       replay the placement order");
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
                s_load(app);
                if (app->job)
                    s_pack(app);
            }
            if (igMenuItem_Bool("Pack", "F5", false, app->job != NULL))
                s_pack(app);
            igSeparator();
            if (igMenuItem_Bool("Save SVG...", "Ctrl+S", false,
                app->placement_count > 0U))
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
        s_load(app);
        if (app->job)
            s_pack(app);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false)
        && app->placement_count > 0U)
        app->pending_popup = S_POPUP_SAVE;
    if (igIsKeyPressed_Bool(ImGuiKey_F5, false) && app->job)
        s_pack(app);
}

/* Draws the sheet and the placements, and handles panning and zooming. */
static void
s_draw_sheet(s_app_t *app)
{
    bp_job_t *job = app->job;
    ImDrawList *draw = igGetWindowDrawList();
    ImGuiIO *io = igGetIO();
    ImVec2 origin = igGetCursorScreenPos();
    ImVec2 size = igGetContentRegionAvail();
    float sheet_width;
    float sheet_height;
    uint32_t index;

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

    for (index = 0U; index < (uint32_t) app->step
        && index < app->placement_count; ++index) {
        const bp_placement_t *spot = &job->placements[index];
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
    ImDrawList_PopClipRect(draw);
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

    igText("job file");
    (void) igInputText("##path", app->job_path, sizeof app->job_path, 0, NULL,
        NULL);
    if (igButton("Load", s_vec(80.0f, 0.0f)))
        s_load(app);
    igSameLine(0.0f, 8.0f);
    if (igButton("Pack", s_vec(80.0f, 0.0f)))
        s_pack(app);
    igSameLine(0.0f, 8.0f);
    if (igButton("Fit view", s_vec(80.0f, 0.0f)))
        app->fit_pending = 1;

    igSeparator();
    igText("geometry");
    /* Takes effect on the next Load, because it rebuilds every mask. */
    (void) igSliderFloat("cell mm", &app->cell_size_mm, 0.25f, 4.0f, "%.2f",
        0);
    igText("cell size applies when you press Load");

    igSeparator();
    igText("search");
    (void) igCheckbox("ruin and recreate", &app->use_search);
    if (app->use_search) {
        (void) igSliderFloat("seconds", &app->time_budget_seconds, 0.2f,
            30.0f, "%.1f", 0);
        (void) igSliderFloat("squeeze share", &app->compact_fraction, 0.0f,
            1.0f, "%.2f", 0);
        (void) igSliderFloat("blink", &app->blink_rate, 0.0f, 0.4f, "%.3f", 0);
        (void) igSliderInt("ruin", &app->max_ruin, 1, 32, "%d", 0);
    } else {
        (void) igCombo_Str_arr("order", &app->sort_index, sort_names, 3, -1);
        (void) igCombo_Str_arr("search", &app->mode_index, mode_names, 3, -1);
    }

    igSeparator();
    igText("view");
    (void) igCheckbox("part numbers", &app->show_labels);
    if (app->placement_count > 0U) {
        (void) igSliderInt("step", &app->step, 0,
            (int) app->placement_count, "%d", 0);
        igText("drag the step slider to replay the placement order");
    }

    igSeparator();
    if (app->job && app->placement_count > 0U) {
        double sheet_area = (double) app->job->sheet_width_cells
            * app->job->sheet_height_cells;

        igText("placed       %u of %u", (unsigned) app->placed_count,
            (unsigned) app->job->part_count);
        igText("utilisation  %.2f %%",
            100.0 * (double) app->placed_area_cells / sheet_area);
        igText("used box     %.0f x %.0f mm",
            app->used_width_cells * app->job->cell_size_mm,
            app->used_height_cells * app->job->cell_size_mm);
        igText("time         %.3f s", app->seconds);
    }
    igSeparator();
    igText("%s", app->status);
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
        (void) fwrite(pixels + (size_t) row * (size_t) width * 3U, 1U,
            (size_t) width * 3U, out);
    (void) fclose(out);
    free(pixels);
    return 1;
}

/* Opens a window, packs a job and shows it until the window is closed. */
int
main(int argc, char **argv)
{
    static s_app_t app;
    const char *shot_path = NULL;
    int shot_after_frames = 0;
    int frame = 0;
    int argument;
    GLFWwindow *window;
    ImGuiIO *io;

    app.cell_size_mm = 1.0f;
    app.time_budget_seconds = 3.0f;
    app.compact_fraction = 0.4f;
    app.blink_rate = 0.08f;
    app.max_ruin = 8;
    app.zoom = 0.5f;
    app.show_labels = true;
    (void) snprintf(app.save_path, sizeof app.save_path, "%s",
        "build/nest.svg");
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
            shot_after_frames = atoi(argv[++argument]);
        } else if (argv[argument][0] == '-') {
            (void) fprintf(stderr,
                "usage: bp_gui [job-file] [--shot <file.ppm>] [--frames <n>]\n");
            return 1;
        } else {
            (void) snprintf(app.job_path, sizeof app.job_path, "%s",
                argv[argument]);
        }
    }

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

    s_load(&app);
    if (app.job)
        s_pack(&app);

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
        if (shot_path && ++frame >= shot_after_frames) {
            if (!s_write_shot(shot_path, width, height))
                (void) fprintf(stderr, "bp_gui: cannot write %s\n",
                    shot_path);
            glfwSwapBuffers(window);
            break;
        }
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (app.job)
        bp_job_destroy(&app.job);
    return 0;
}
