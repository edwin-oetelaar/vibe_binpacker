# vibe_binpacker

Raster-based 2D nesting for sheet material. Given a set of simple polygons and
a rectangular plate, it decides where to cut each part so that as little
material as possible is wasted.

The core is deterministic C11 with no allocation in the hot path. Python does
tooling (DXF/SVG import, reporting, plots) and a separate binary does the GUI.
The solver itself never needs either.

Status: **phases P0 and P1 done** - there is a working rectangle nester with
a CLI and SVG output. The search layer that makes it good is still ahead.
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
asked for rather than twice it.

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

That packs [jobs/demo.job](jobs/demo.job) - 36 rectangles on a 1200 x 1200 mm
plate with 1 mm between parts - writes `demo.svg`, and then re-checks the
result with [tools/check_layout.py](tools/check_layout.py), which reads the
SVG back and verifies independently that nothing overlaps, nothing leaves the
sheet and the clearance holds.

```sh
pixi run benchmark
```

sweeps cell size and placement mode and prints CSV.

The job format is line based on purpose, so the solver needs no parser
dependency:

    sheet 1200 1200        # plate size in mm
    cell 1.0               # grid resolution in mm
    clearance 1.0          # gap to leave between parts, in mm
    rect 1 2 600 300 1     # id, quantity, width, height, may turn 90 degrees

## Layout

Built and tested:

    bp_contract.h     fail-fast contract macro
    bp_bitmap.[ch]    bitboard rows, shifts, span queries, counts
    bp_mask.[ch]      part footprint as row spans, blit, overlap
    bp_place.[ch]     bottom-left query: reference scan and jump scan
    bp_nester.[ch]    sequence decoder, bottom-left-fill
    bp_job.[ch]       job file loading and mask construction
    bp_svg.[ch]       solution to SVG
    bp_pack.c         CLI
    bp_bench.c        repeatable benchmark
    tools/            Python tooling

Still to come:

    bp_polygon.[ch]   rings, validation, transforms
    bp_raster.[ch]    conservative scanline fill, dilation
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
