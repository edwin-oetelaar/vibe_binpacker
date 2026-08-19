# vibe_binpacker

Raster-based 2D nesting for sheet material. Given a set of simple polygons and
a rectangular plate, it decides where to cut each part so that as little
material as possible is wasted.

The core is deterministic C11 with no allocation in the hot path. Python does
tooling (DXF/SVG import, reporting, plots) and a separate binary does the GUI.
The solver itself never needs either.

Status: **phase P0 done** - build environment, contract macro and the
`bp_bitmap` bit grid with tests. No solver yet. [PLAN.md](PLAN.md) has the
design, the algorithm and the phased roadmap; [RESEARCH.md](RESEARCH.md) has
the literature review that shaped them.

## Approach in one paragraph

The plate is a grid of bits, cell size configurable and 1 mm by default:
1200 x 1200 cells, 19 `uint64_t` words per
row, 182 KB for the whole sheet, so the collision oracle lives in L2 cache.
Parts are conservatively rasterised into bit masks, one per allowed rotation.
Instead of testing candidate positions one pixel at a time, the set of *all*
legal positions for a part is computed as a binary morphological erosion of
the free space by the part mask, using shift-and passes over whole 64-bit
words. That turns "where does this fit" from a scan into a handful of
milliseconds, and because the full feasible set is available, placements can be
*scored* (bottom-left, contact, remnant quality) rather than taken first-fit.
On top of that, the search layer follows what the nesting literature says
actually wins: not a metaheuristic over part order, but **overlap
minimisation**. Parts are allowed to overlap temporarily and a guided local
search drives total penetration back to zero, wrapped in a squeeze loop that
deliberately makes the layout infeasible and then repairs it. The same
bitboard serves that directly: a per-row prefix popcount answers "how much
does this part overlap here" in one subtraction per row span, which is the
raster penetration map of Sato et al. (2019).

Consequences of the pixel model that matter here: concave shapes and polygons
with holes need no special geometry code, and a part nesting *inside another
part's hole* falls straight out of the representation. The price is a 1 mm
quantisation, which is below the laser kerf and therefore acceptable.

## Layout

Implemented so far: `bp_contract.h`, `bp_bitmap.[ch]`, `test_bp_bitmap.c`.

    bp_bitmap.[ch]    bitboard, shifts, run masks
    bp_polygon.[ch]   rings, validation, transforms
    bp_raster.[ch]    conservative scanline fill, dilation
    bp_part.[ch]      per-orientation solid/collision/outline masks
    bp_place.[ch]     feasible positions: reference scan and erosion
    bp_nester.[ch]    sequence decoder
    bp_penetration.[ch] prefix popcount, penetration map
    bp_separate.[ch]  guided local search over overlap
    bp_search.[ch]    squeeze / separate / accept outer loop
    bp_svg.[ch]       solution to SVG
    bp_pack.c         CLI
    tools/            Python: DXF/SVG import, plots, reports
    gui/              cimgui (the C API for Dear ImGui) + sokol; last phase,
                      SVG output comes first

## Build

Everything runs through Pixi on Linux:

```sh
pixi run test
```

```sh
pixi run test-sanitize
```

`pixi run build` and `pixi run clean` are the other tasks. `benchmark` and
`demo` arrive with the first solver phase.

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
