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
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS

#include "cimgui.h"
#include "cimgui_impl.h"

#include "bp_job.h"
#include "bp_mask.h"
#include "bp_nester.h"
#include "bp_place.h"
#include "bp_score.h"
#include "bp_search.h"

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define igGetIO igGetIO_Nil

#define S_PATH_MAX 512U

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
    (void) snprintf(app->status, sizeof app->status,
        "packed %u of %u parts in %.3f s", (unsigned) app->placed_count,
        (unsigned) job->part_count, app->seconds);
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

    while (!glfwWindowShouldClose(window)) {
        int width = 0;
        int height = 0;

        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        igNewFrame();

        glfwGetFramebufferSize(window, &width, &height);
        igSetNextWindowPos(s_vec(0.0f, 0.0f), ImGuiCond_Always,
            s_vec(0.0f, 0.0f));
        igSetNextWindowSize(s_vec(360.0f, (float) height), ImGuiCond_Always);
        if (igBegin("controls", NULL,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoCollapse))
            s_draw_controls(&app);
        igEnd();

        igSetNextWindowPos(s_vec(360.0f, 0.0f), ImGuiCond_Always,
            s_vec(0.0f, 0.0f));
        igSetNextWindowSize(s_vec((float) width - 360.0f, (float) height),
            ImGuiCond_Always);
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
