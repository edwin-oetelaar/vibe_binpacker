# vibe_binpacker

Raster-based 2D nesting for sheet material. Given a set of simple polygons and
a rectangular plate, it decides where to cut each part so that as little
material as possible is wasted.

The core is deterministic C11 with no allocation in the hot path. Python does
tooling (DXF/SVG import, reporting, plots) and a separate binary does the GUI.
The solver itself never needs either.

Status: **phases P0 to P3 done** - there is a working nester for arbitrary
simple polygons, with holes, free rotation and a guaranteed gap between parts,
plus a CLI and SVG output. The search layer that makes it *good* is still
ahead, but the primitive it needs is in place.
[PLAN.md](PLAN.md) has the design, the algorithm and the phased roadmap;
[RESEARCH.md](RESEARCH.md) has the literature review that shaped them.

## Approach

The plate is a grid of bits, cell size configurable and 1 mm by default. At
1 mm a 1200 x 1200 sheet is 19 `uint64_t` words per row and 182 KB in total,
so the collision oracle lives in L2 cache. Parts are conservatively rasterised
into bit masks stored as row spans, one mask per allowed rotation. Concave
shapes and polygons with holes need no special code in that representation,
and a part nesting *inside another part's hole* falls straight out of it. The
price is a 1 mm quantisation, which is below the laser kerf.

Clearance works the way the literature does it: each part carries two masks,
the true geometry and a copy grown by the requested gap. The search tests the
grown one and the sheet records the true one, which leaves exactly the gap you
asked for rather than twice it. The growth is measured between cell *squares*
rather than cell centres, because two cells a diagonal step apart have centres
sqrt(2) apart but squares that touch at a corner.

Asking where a part fits has two very different answers. For the *first* free
spot, a scan that skips the stretch a blocker rules out wins, because it
usually kills a candidate on its first word. For **every** free spot at a
height, the answer is computed rather than searched: intersecting one run mask
per span of the part gives the whole row of legal positions in a handful of
64-bit operations, eleven to thirty times faster than scanning for the same
set. That second answer is the one a continuous method cannot give at all, and
it is what a scoring layer needs. `pixi run benchmark` reports both.

What exists today is **bottom-left-fill**: each part goes to the lowest, then
leftmost free spot. That is the published baseline and the standard
initialiser, but on its own it plateaus. What comes next, and where the
material savings are, is the winning family from the literature: not a
metaheuristic over part order, but **overlap minimisation**. Parts are allowed
to overlap temporarily and a guided local search drives total penetration back
to zero, wrapped in a squeeze loop that deliberately makes the layout
infeasible and then repairs it. The same bitboard serves that directly - a
per-row prefix popcount answers "how much does this part overlap here" in one
subtraction per row span, which is the raster penetration map of Sato et al.
(2019).

## Try it

```sh
pixi run demo
```

That packs three jobs onto a 1200 x 1200 mm plate with 1 mm between parts and
writes an SVG of each:

- [jobs/demo.job](jobs/demo.job) - 36 rectangles.
- [jobs/shapes.job](jobs/shapes.job) - L shapes, triangles, a square frame and
  a five-sided part offered at every 45 degrees.
- [jobs/nested_hole.job](jobs/nested_hole.job) - a frame and a square that can
  only go inside the frame's hole.

Each result is then re-checked with
[tools/check_layout.py](tools/check_layout.py), which reads the SVG back and
verifies independently that nothing overlaps, nothing leaves the sheet and the
clearance holds. That check is not decoration: it is what caught the clearance
being measured between cell centres instead of cell squares.

```sh
pixi run benchmark
```

sweeps cell size and placement mode and prints CSV, then times enumerating
every feasible position for a part against doing it by scanning.

The job format is line based on purpose, so the solver needs no parser
dependency. DXF and SVG import belong in Python tooling that writes these
files.

    sheet 1200 1200        # plate size in mm
    cell 1.0               # grid resolution in mm
    clearance 1.0          # gap to leave between parts, in mm

    rect 1 2 600 300 1     # id, quantity, width, height, may turn 90 degrees

    poly 2 4 every:90      # id, quantity, angles: a list like 0,90,180 or
                           # every:<step> for a full turn in equal steps
    ring 6  0 0  300 0  300 80  120 80  120 200  0 200
    ring 4  40 40  80 40  80 80  40 80     # further rings are holes

A `rect` line is sugar for a four-vertex polygon, so there is only one
geometry path. Ring winding does not matter: filling uses the even-odd rule,
because real CAD input arrives wound arbitrarily and guessing at it would be
worse than ignoring it.

## Layout

Built and tested:

    bp_contract.h     fail-fast contract macro
    bp_bitmap.[ch]    bitboard rows, shifts, span queries, counts
    bp_mask.[ch]      part footprint as row spans, blit, overlap
    bp_polygon.[ch]   rings, validation, simplicity, bounds, rotation
    bp_raster.[ch]    conservative fill, growth for clearance
    bp_place.[ch]     placement search in three modes, and every feasible
                      position at a height
    bp_nester.[ch]    sequence decoder, bottom-left-fill
    bp_job.[ch]       job file loading and mask construction
    bp_svg.[ch]       solution to SVG
    bp_pack.c         CLI
    bp_bench.c        repeatable benchmark
    tools/            Python tooling

Still to come:

    bp_penetration.[ch] prefix popcount, penetration map
    bp_separate.[ch]  guided local search over overlap
    bp_search.[ch]    squeeze / separate / accept outer loop
    gui/              cimgui (the C API for Dear ImGui) + sokol; last phase

## Build

Everything runs through Pixi on Linux:

```sh
pixi run test
```

```sh
pixi run test-sanitize
```

The other tasks are `build`, `demo`, `benchmark` and `clean`.

Compiler flags are fixed at `-std=c11 -Wall -Wextra -Werror -pedantic`, with
AddressSanitizer and UndefinedBehaviorSanitizer runs required for any change
to indexing or bit shifting.

## Conventions

C style follows Pieter Hintjens' Scalable C, as used in the sibling project
`~/tmp_codex`: one header and one implementation file per module, `bp_` for
public symbols and `s_` for file-local statics, caller-owned buffers, no
global mutable state, units in names (`width_cells`, `kerf_mm`). Invalid
caller input is a programming defect and fails fast; expected runtime failures
return explicit tagged results.

## Related work

Nesting is a well-studied NP-hard problem. The mainstream approach uses no-fit
polygons; this project deliberately takes the raster route for robustness on
concave shapes and holes. References are listed at the end of
[PLAN.md](PLAN.md).
