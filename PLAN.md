# Plan: raster-based 2D nesting for laser cut sheets

Status: draft for review. Nothing is implemented yet.
Language of this document is English to match the sibling project
`~/tmp_codex` (material-nesting baseline). Say the word and it becomes Dutch.

## 1. Problem

Cut a set of simple polygons P out of one rectangular sheet V (1200 x 1200 mm)
with a laser, minimising wasted material. Formally this is the *irregular
two-dimensional bin packing* problem, also called *nesting*: NP-hard, so the
goal is a good solution inside a time budget, not a proven optimum.

Scope, in order of ambition:

1. Axis-aligned rectangles, 0 and 90 degree rotation.
2. Arbitrary simple polygons (L shapes, convex, concave), free rotation.
3. Polygons with holes, where a hole may be filled by one or more other parts.
4. Multiple sheets and reusable remnants.

Hard constraints: no overlap, everything inside V, a kerf/clearance distance
between parts, optional grain/rotation restrictions per part.

## 2. Model: discrete grid, bitboard

The cell size is a **configuration parameter**, not a constant: it costs
nothing to make it one now and is painful to retrofit later. Default 1.0 mm,
so V is a 1200 x 1200 cell grid = 1 440 000 cells. 0.5 mm is the practical
fine setting; 0.1 mm is not viable as a *global search* resolution because the
scan cost scales roughly as the cube of the inverse cell size (about 1000x).
Memory is not the limit, compute is. See RESEARCH.md section 7 for the table,
and for the semi-discrete escape hatch if x accuracy ever becomes the binding
constraint.

- Origin bottom-left, x to the right, y upwards.
- A cell is a half-open square [x, x+1) x [y, y+1). Touching parts do not
  overlap; only shared *cells* are overlap.
- One bit per cell. A row is `(1200 + 63) / 64 = 19` words of `uint64_t`.
  The whole sheet is `1200 * 19 * 8 = 182 400` bytes, which fits in L2 cache.
  This is what makes the pixel approach fast: the entire collision oracle is
  a cache-resident bit array.
- Bit order inside a word: LSB is the smallest x. "Move a shape to the right
  by d" is then a left shift with carry into the next word.

Two derived layers:

- `occ`  : occupied cells (union of placed part masks).
- `free` : `~occ` masked to the usable region (sheet minus border margin).

### Why raster instead of no-fit polygons

The literature standard is the No-Fit Polygon (NFP): compute, for each ordered
pair of shapes, the region where their reference points may not lie. Exact,
resolution independent, and continuous-space optimal, but the NFP construction
is notoriously fiddly for concave shapes, holes and degenerate contacts.

The raster model trades exactness for simplicity and robustness:

| aspect              | raster (this project)          | NFP                        |
| ------------------- | ------------------------------ | -------------------------- |
| concave shapes      | free                           | hard cases, robustness bugs|
| holes / part-in-part| free, falls out of the model   | needs explicit inner NFP   |
| free rotation       | pre-rasterise per angle        | NFP per shape *pair* *angle|
| numerical robustness| integer only, no epsilons      | floating point degeneracies|
| accuracy            | limited by 1 mm cell           | exact                      |
| memory              | 182 KB per sheet layer         | small                      |

At 1 mm on a 1200 mm sheet the quantisation error is 0.08 %, which is well
below a typical laser kerf. That is the right trade for this application.

## 3. Core algorithm

### 3.1 Level 0 - reference scan (the oracle)

Exactly what you described: for y from 0 upwards, for x from 0 rightwards,
test the part mask against `occ` cell by cell, take the first position that
fits. Simple, obviously correct, far too slow (order 1.4e9 cell tests per part
for a 300x200 part). Its purpose is to be the *reference implementation* that
every fast path is tested against, not to be used in production.

### 3.2 Level 1 - jump on collision

Two cheap fixes to Level 0 that already give one or two orders of magnitude:

- Test whole words instead of cells: a candidate test is
  `rows * words_per_row` AND operations.
- On collision, do not advance x by 1. Find the rightmost blocking cell inside
  the overlapping window and jump x to `blocker_x + 1`; every position in
  between is provably infeasible.
- Skip empty rows using a per-row "skyline" (highest occupied y per column) as
  a pre-filter.

### 3.3 Level 2 - erosion, the real engine

The set of *all* feasible positions for a part is a binary morphological
erosion of `free` by the part mask. Computing that with bitboards costs
roughly one pass over the sheet per part row, and yields every legal position
at once instead of the first one.

Represent a part mask as row spans (run-length encoding): row `r` contains
spans `(a_i, L_i)` meaning cells `a_i .. a_i + L_i - 1` are solid.

Define, for sheet row `y` and length `L`, the run mask

    R_L(y)[x] = 1  iff  free cells x, x+1, ..., x+L-1 are all free in row y

which is computed by doubling, `log2(L)` shift-and passes over 19 words:

    R_1     = free_row(y)
    R_{2k}  = R_k & (R_k >> k)                 (>> means "towards smaller x")
    R_L     = AND of the R_{2^j} for the set bits of L, with the right offsets

Then the feasible x positions for a part whose bottom row sits at `y` are

    F(y) = AND over all part rows r, over all spans (a,L) of row r,
                of  R_L(y + r) >> a

`F(y) == 0` means the part does not fit anywhere at that height. Otherwise
every set bit is a legal x.

Cost accounting, 300x200 part on the 1200x1200 sheet:

- run masks: computed once per (sheet row, distinct span length) and cached,
  ~1200 rows * a handful of lengths * log2(L) * 19 words - negligible.
- scan: 1200 heights * 200 part rows * 19 words = 4.6e6 word operations.

That is milliseconds per part instead of seconds, and it delivers the full
feasible set, which is what makes real placement *scoring* affordable
(section 3.6).

Cache invalidation: placing a part only dirties the sheet rows it covers, so
only those cached run masks are dropped.

### 3.4 Conservative rasterisation

Polygon to mask by scanline fill, but *conservative*: any cell touched by the
polygon becomes solid. That guarantees the discrete solution is also valid in
continuous space - parts can only be pushed apart by rounding, never merged.

Holes: a part is an outer ring plus zero or more inner rings. Rasterise the
outer ring solid, then clear the inner rings. Hole cells stay `free` in the
sheet, so *any later part simply erodes into the hole with no special code*.
Part-in-part nesting is not a feature that must be added; it is what the model
already does.

### 3.5 Kerf and clearance

Two masks per part orientation:

- `solid`     : the true geometry, written into `occ` on placement and used
                for the output DXF/SVG.
- `collision` : `solid` dilated by `k` cells (Chebyshev or disc structuring
                element), used only for the overlap test.

Testing a dilated candidate against undilated occupancy gives exactly `k` mm
clearance between neighbours, not `2k`. Dilating the solid also shrinks holes
inward by `k`, so clearance inside holes is correct for free. A border margin
is applied by clearing the outer `m` cells of `free` once.

### 3.6 From "first fit" to "best fit"

Because erosion hands us every feasible position, the placement rule becomes a
scoring function over candidates instead of a hard-coded corner rule:

- **BL / BLF**: lowest y, then lowest x. The classic baseline.
- **Contact score**: maximise the length of the part boundary that touches
  already-occupied cells or the sheet border. Parts nest into concavities
  instead of merely stacking. Cheap to compute from the mask outline.
- **Waste score**: penalise creating small isolated free pockets. Approximated
  by counting free cells below/left of the placement that are now unreachable
  by the smallest remaining part.
- **Bounding-box growth**: prefer placements that do not extend the used
  bounding box, so what is left over is a clean rectangular remnant.

This is also where "collision on the outline only" belongs. As a *complete*
test it is unsound: a small already-placed part can sit entirely inside a
large part's interior without touching its outline. It is however a valid
*cheap pre-reject* and the natural input to the contact score, so the outline
is precomputed per orientation and used for scoring, not for correctness.

### 3.7 Penetration map: counting overlap in O(spans)

Erosion answers "does it fit" as a boolean. The search layer (3.8) needs
"by how much does it *not* fit", and counting does not word-parallelise the
way AND does. The fix is an integral-image trick.

Per sheet row `y`, keep the prefix count of occupied cells:

    S_y(x) = number of occupied cells in row y at indices < x

Occupancy in any interval `[p, q)` of that row is then `S_y(q) - S_y(p)`, one
subtraction. With the part stored as row spans `(a, L)`:

    penetration(part at (px, py))
        = sum over rows r, spans (a, L) of
          [ S_{py+r}(px + a + L) - S_{py+r}(px + a) ]

Two reads and a subtraction **per span**, independent of the part's area. A
300x200 rectangle costs 200 subtractions per candidate position instead of
60 000 cell tests. The table is 1200 x 1200 x 2 bytes = 2.9 MB at 1 mm and is
rebuilt only for rows a placement dirtied.

Evaluated over a window, this is the *raster penetration map* of Sato et al.
(2019), also called the Voronoi mountain: the overlap cost at every grid
position at once, whose global minimum is where the part wants to go.

### 3.8 Search layer

The literature is unambiguous about what wins, and it is not a metaheuristic
over the part order. It is **overlap minimisation**: allow parts to overlap
temporarily and minimise total penetration, so feasibility becomes an
objective you can descend rather than a wall you bounce off. See RESEARCH.md
section 3 for the evidence. Structure, following sparrow (2025):

1. **Initialise** with BLF over the complete feasible set (3.3 + 3.6).
2. **Squeeze**: make the problem infeasible on purpose - add the next part, or
   shrink the usable region by a fraction of a percent - which forces
   collisions.
3. **Separate**: resolve those collisions by guided local search. Per colliding
   pair keep a GLS weight; repeatedly take a colliding part and move it to the
   minimum of its weighted penetration map (3.7); raise weights on collisions
   that persist, decay them once resolved, so the search is pushed out of local
   optima instead of oscillating.
4. **Accept or roll back**. On success the squeezed layout becomes the
   incumbent; on repeated failure, disrupt by swapping two large parts.
5. **Compress**: in the last part of the time budget, retry from the best
   layout with progressively smaller squeeze steps, restoring the incumbent
   before each attempt so changes stay local.

Simple sorting plus multi-start remains the cheap baseline that steps 2-5 must
beat, and it is what the benchmark reports against.

All randomness comes from one explicitly seeded PRNG so any run is
reproducible from `(input, seed, time budget)`.

### 3.9 Objective

Primary: maximise placed area / sheet area on a fixed sheet.
Secondary, and practically important for a workshop: minimise the used
bounding box height so the unused part of the plate stays a usable rectangular
remnant. A packing at 82 % utilisation with a clean 1200x200 strip left is
worth more than 84 % with waste sprayed everywhere.

## 4. Module layout

Pure C11 core, no allocation in the hot path, caller-owned buffers, `bp_`
prefix for public symbols and `s_` for file-local statics, following the
`AGENTS.md` rules already in use in `~/tmp_codex`.

| module         | responsibility                                              |
| -------------- | ----------------------------------------------------------- |
| `bp_contract`  | `BP_REQUIRE`, fail-fast macros                              |
| `bp_bitmap`    | bitboard rows, shift/and/or, run masks, occupancy count     |
| `bp_polygon`   | rings, validation, transform, area, winding                 |
| `bp_raster`    | conservative scanline fill, hole clearing, dilation         |
| `bp_part`      | part = orientations, each with solid/collision/outline mask |
| `bp_place`     | feasible-position query: reference scan and erosion path    |
| `bp_score`     | candidate scoring functions                                 |
| `bp_nester`    | sequence decoder, produces a placement list                 |
| `bp_penetration`| per-row prefix popcount, penetration map queries            |
| `bp_separate`  | guided local search that resolves overlap                    |
| `bp_search`    | squeeze/separate/accept outer loop, time budget              |
| `bp_job`       | job/solution text I/O                                       |
| `bp_svg`       | solution to SVG, for eyeballing without a GUI               |
| `bp_pack`      | CLI front end                                               |

`bp_bitmap` can start as a port of `mt_bitmap` from `~/tmp_codex`, extended
with shifted-AND, run masks and mask blitting.

## 5. Data and file formats

The C core reads and writes a plain line-based ASCII format so it needs no
parser dependency and stays diff-friendly:

    sheet 1200 1200 kerf 1 margin 2
    part 7 qty 4 rot 0,90,180,270
      ring 0 4  0 0  300 0  300 200  0 200
      ring 1 4  50 50  100 50  100 100  50 100

Everything else lives in Python tooling: DXF/SVG import (`ezdxf`, `svgpathtools`),
arc and spline flattening to polylines, part deduplication, and conversion to
the job format. The solver also writes a JSON solution for the GUI and for
scripted reporting. Python never does geometry that the solver depends on.

## 6. Verification

- Property test: for random small sheets and random parts, the erosion path
  must return exactly the same feasible set as the reference scan. This is the
  single most valuable test in the project.
- Invariants on every solution: no two placements share a cell, every cell is
  inside the sheet, the clearance between any two parts is >= kerf, and
  `occupied_cells == sum of part areas`.
- Contract death tests on Linux via subprocess, as in the sibling project.
- `-std=c11 -Wall -Wextra -Werror -pedantic`, plus ASan/UBSan runs for every
  change to indexing or shifting. Bit shifting across word boundaries is
  exactly the kind of code that needs a sanitiser.
- Benchmark emitting CSV: parts, placed, utilisation, used bbox, candidate
  tests, wall time. No performance claim without a repeatable benchmark.

## 7. GUI

The core stays headless and deterministic; the GUI is a separate binary that
links it. Requirements: show the sheet with placed parts, step through the
placement sequence, visualise the feasible-position mask for the part being
placed (that picture makes the erosion idea obvious and is a superb debugging
tool), tweak parameters and re-run.

Options, in order of preference:

1. **cimgui + sokol_app/sokol_gfx** - `cimgui` is the generated **C** API for
   Dear ImGui (not to be confused with `CImGui.jl`, the Julia package that
   wraps it; no Julia is involved). On top of single-header C backends,
   everything stays C, vendored in `third_party/`, no package manager
   surprises. The sheet is one texture upload, part outlines are ImGui
   draw-list polylines.
2. **Dear ImGui + GLFW + OpenGL3** - the mainstream path, but the GUI file
   becomes C++ calling the core through `extern "C"`. More examples online.
3. **raylib** - least code to first pixel, weaker for panels and controls.

Step zero, before any GUI: `bp_svg` writes the solution to SVG and Python
renders comparison sheets. That is enough to make progress on the algorithm
and it costs an afternoon, not a week.

## 8. Roadmap

Each phase ends with tests green and a benchmark number recorded.

| phase | content                                                        | done when |
| ----- | -------------------------------------------------------------- | --------- |
| P0 | pixi env, Makefile, `bp_contract`, `bp_bitmap` + tests            | **done**: `pixi run test` and `pixi run test-sanitize` green |
| P1 | rectangles, reference scan + jump scan, 90 deg, SVG out, CLI      | reproduces the `mt_nester` reference cases |
| P2 | erosion engine, equivalence property test, benchmark              | measured speedup vs P1 on the same host |
| P3 | polygon rasteriser, arbitrary angles, kerf and margin             | L shapes pack with verified clearance |
| P4 | penetration map + guided local search separation, squeeze loop     | utilisation beats the P3 BLF baseline on a fixed suite |
| P5 | holes and part-in-part nesting                                    | a part provably placed inside another's hole |
| P6 | GUI viewer and stepper                                            | can watch a pack being built |
| P7 | multiple sheets, remnant tracking, DXF import/export              | end to end from a real DXF |

## 9. Open questions

1. ~~Grid resolution~~ - settled: configurable, default 1.0 mm, 0.5 mm fine
   setting, 0.1 mm rejected as a global search resolution (RESEARCH.md 7).
2. Which angles for free rotation: 90 deg only, 15 deg steps, or continuous?
   Each extra angle is another rasterised mask per part, so this drives memory
   and search size. This is now the biggest open design question.
3. Real kerf value and whether the cut line is on the part boundary or offset.
   Clearance itself is settled: dilate the candidate only (3.5).
4. Cutting-order constraints (thermal, common-line cutting, tipping parts) -
   out of scope now, but common-line cutting would change the model, so it is
   worth knowing whether you want it eventually.
5. Typical instance size: 20 parts or 500? That decides how much search
   machinery is justified.

## 10. Literature

Done, see [RESEARCH.md](RESEARCH.md). Short version: BLF is a real published
baseline from the 1990s and still the standard initialiser, but every method
that beats it belongs to the overlap-minimisation family, and the current
state of the art is sparrow (2025). Our raster model has a published
competitive representative in Sato et al. (2019), whose raster penetration map
is what section 3.7 implements.
