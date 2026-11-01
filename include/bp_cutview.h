#pragma once

#include "bp_cutplan.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A published guillotine plan, independent of ImGui: which plate is shown,
 * how far the saw list is replayed, what the plan uses, and how it is saved.
 * It is the panel saw counterpart of bp_view.
 *
 * The step is plate-local and counts cuts of the saw list in operator order,
 * edge trims first. Step 0 shows the plate before the saw touches it; the
 * full step shows every cut. All pointers are borrowed and must outlive the
 * view; repacking invalidates it, so publish again afterwards. A zeroed view
 * means no plan yet.
 */
typedef struct {
    const bp_cutplan_t *plan;
    const uint32_t *labels;
    /* The whole saw list, plates in order. */
    const bp_cut_t *cuts;
    uint32_t cut_count;
    uint32_t placed_count;
    uint64_t item_area_10um2;
    uint32_t active_plate;
    /* The active plate's cuts are cuts[first_cut .. first_cut + plate_cuts). */
    uint32_t first_cut;
    uint32_t plate_cuts;
    uint32_t visible_cuts;
    int saved;
} bp_cutview_t;

typedef struct {
    /* Must pass bp_cutplan_check. */
    const bp_cutplan_t *plan;
    /* One label per item, as bp_cutexport requires. */
    const uint32_t *labels;
    /* Written: the saw list; capacity must cover bp_cutplan_cut_count. */
    bp_cut_t *cuts;
    uint32_t cut_capacity;
} bp_cutview_publish_args_t;

/*
 * Publishes a plan: writes its saw list and counts what it places. Selection
 * starts at plate 1 with every cut shown, and a new plan is unsaved. A plan
 * with no plates is valid and shows nothing. Allocates nothing.
 */
bp_cutview_t
bp_cutview_publish(bp_cutview_publish_args_t args);

/* Selects an existing zero-based plate and shows all of its cuts. */
void
bp_cutview_select(bp_cutview_t *view, uint32_t plate);

/* Sets how many of the active plate's cuts are shown, 0 to plate_cuts. */
void
bp_cutview_set_visible(bp_cutview_t *view, uint32_t count);

/* Returns one cut of the active plate, local_index below plate_cuts. */
const bp_cut_t *
bp_cutview_cut(const bp_cutview_t *view, uint32_t local_index);

/* Returns the parts' share of all used plate area, 0 with no plates. */
double
bp_cutview_utilisation(const bp_cutview_t *view);

typedef enum {
    BP_CUTVIEW_EXPORT_OK = 0,
    BP_CUTVIEW_EXPORT_EMPTY = 1,
    BP_CUTVIEW_EXPORT_PATH_TOO_LONG = 2,
    BP_CUTVIEW_EXPORT_WRITE_FAILED = 3
} bp_cutview_export_status_t;

typedef struct {
    bp_cutview_export_status_t status;
    /* Files completed before a failure: the list first, then the plates. */
    uint32_t written_count;
    /* errno of the failed open, write or close, or 0 when none was set. */
    int system_error;
} bp_cutview_export_result_t;

typedef struct {
    bp_cutview_t *view;
    /* A stem; a trailing .csv or .svg is removed. */
    const char *path;
    /* Writable path scratch; holds the last attempted name on return. */
    char *scratch_path;
    size_t path_capacity;
    /* Saw list scratch for the writers; capacity must cover the plan's cuts. */
    bp_cut_t *scratch_cuts;
    uint32_t scratch_cut_capacity;
} bp_cutview_export_args_t;

/*
 * Writes <stem>.csv and <stem>-001.svg onwards, the names
 * tools/check_cutplan.py --svg reads, regardless of selection and step. Marks
 * the view saved only once every file was written and closed. On failure the
 * completed files remain and written_count says how many.
 */
bp_cutview_export_result_t
bp_cutview_export(bp_cutview_export_args_t args);
