# Plan: raster-based 2D nesting for laser cut sheets

Design notes and roadmap. Written as the project went along, so the sections
that measurement overturned say so rather than being quietly rewritten.

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

### 3.3 Level 2 - erosion, for the complete feasible set

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

**Measured outcome, and it is not what this section originally predicted.**
For merely finding the bottom-left position, erosion *loses* to the jump scan
of 3.2, on every shape type and every resolution:

| job, 1 mm | jump | erosion |
| --------- | ---- | ------- |
| `demo.job`, rectangles | 0.007 s | 0.078 s |
| `shapes.job`, irregular | 0.063 s | 0.073 s |
| over-subscribed sheet, 33 parts rejected | 0.023 s | 0.170 s |

The reason is structural. The jump scan is output sensitive: it usually kills
a candidate on the candidate's first span, touching one word, and it stops at
the first position that works. Erosion computes a whole row of positions -
about log2(L) row operations per span - before it can conclude anything, even
when the answer is "nothing fits at this height". Early exit as soon as the
intersection empties helps, and visiting the widest mask row first helps more,
but a row operation is still two orders of magnitude more work than a word
test.

Where erosion wins is the question it was really built for: **every** feasible
position, at every height, which is what a scoring layer needs and what a
continuous method cannot produce at all.

| job, complete feasible set | full scan | erosion | ratio |
| -------------------------- | --------- | ------- | ----- |
| `demo.job` at 1 mm | 0.122 s | 0.011 s | 11x |
| `shapes.job` at 1 mm | 0.519 s | 0.017 s | 31x |
| `shapes.job` at 0.5 mm | 4.450 s | 0.138 s | 32x |

Both produce the identical set, which `bp_bench` checks before reporting the
times. So the conclusion is a split, not a replacement: `BP_PLACE_JUMP` stays
the default for construction, and `bp_place_feasible_row` is the primitive the
search layer of 3.8 is built on.

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

Because `bp_place_feasible_row` hands us every feasible position at a height
for a tenth to a thirtieth of what enumerating them costs by scanning, the
placement rule can become a scoring function over candidates instead of a
hard-coded corner rule:

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

### 3.8 Search layer, first form: ruin and recreate

The literature's winner is overlap minimisation, and that is still where this
is going (3.9). What is built first is **ruin and recreate**, because it is the
natural consumer of the one thing this project has measured itself to be good
at: producing every feasible position at a height for a tenth to a thirtieth of
what enumerating them costs otherwise. It also gives overlap minimisation
something honest to be measured against.

The loop keeps a feasible layout at all times:

1. **Construct** by placing parts largest first, each at its best-ranked
   position.
2. **Ruin**: empty one region of the sheet by lifting out the parts nearest a
   randomly chosen one.
3. **Recreate**: put those back, largest first, together with anything still
   unplaced, ranking positions again.
4. **Accept** when the result is better; otherwise restore.

Better means more material placed; at equal material a lower used height; at
equal height a lower centre of mass. All randomness comes from one explicitly
seeded PRNG, so a run replays from `(input, seed, budget)`.

**Three things had to be right, and two obvious choices were wrong.** All of
this came out of measurement, not reading:

- *Ruin has to be spatially clustered.* Lifting parts at random scatters small
  holes that a greedy recreate refills exactly as they were. Measured over
  5000 rounds: **zero** improvements. Emptying one contiguous region instead
  gives the recreate something to rearrange.
- *The recreate has to blink.* A purely greedy recreate is a fixed point even
  with clustered ruin, for the same reason. Passing over a small random
  fraction of candidate positions - a "blink" - is what lets the layout drift,
  and it costs nothing.
- *The randomness must not go into the insertion order.* Putting parts back in
  random order lost to plain bottom-left outright, because a big part arriving
  after the small ones have taken the good corners is simply worse. Greedy
  order, random ruin, blinked positions.
- *The objective needs a gradient.* Used height alone only changes when the
  single topmost part moves, so nearly every rearrangement scores identically
  and there is nothing to climb. Adding the centre of mass underneath it makes
  every part that settles lower count for something.
- *Contact scoring did not earn its default.* Any positive weight overwhelms
  the left-hand tie-break and turns the rule into "hug whatever you can
  reach", which placed five fewer parts on an over-subscribed sheet than plain
  bottom-left. The term is implemented and exposed, and defaults to zero.

Measured against the P3 bottom-left baseline:

| job | baseline | searched | budget |
| --- | -------- | -------- | ------ |
| `demo.job` used height | 1134 mm | **1035 mm** | 5 s |
| `demo.job` used height | 1134 mm | **1022 mm** | 14 s, wider ruin |
| `dense.job` utilisation | 95.17 % | **95.87 %** | 33 s |

The over-subscribed sheet moves slowly because it starts near the practical
limit; the gain there is in what gets placed, not in tidiness. The sparsely
filled `shapes.job` barely moves at all, which is expected: everything already
fits and there is nothing to win.

### 3.9 Search layer, second form: overlap minimisation

Built. The layout is squeezed into a shorter band, which forces parts to
overlap, and a guided local search walks the overlap back to zero. Success is
a strictly shorter layout; failure is rolled back and the next attempt asks
for less.

**The data-structure question this section used to leave open is settled.**
Per-cell counts with per-row running totals cost O(area) to add or remove a
part against the bitboard's O(area / 64), but they buy a probe that costs one
subtraction per span instead of one test per cell. Since a move makes one
removal, one insertion and of the order of a hundred probes, the probe side
wins: roughly 150 microseconds a move on a 1200 x 1200 sheet, against several
milliseconds for anything that walks cells. The field is `uint32` counts and
`uint64` totals, about 17 MB at 1 mm, and that is the term that scales badly
with resolution.

**Three findings, and all three were bugs in the obvious design.**

*The field must hold true geometry and be probed with the grown footprint.*
Grown masks are not mutually exclusive: two parts exactly one clearance apart
each contain the gap between them, so a field built from grown masks reports a
perfectly legal layout as thousands of cells of conflict. Measured on the
36-part sheet, shrinking the band by a single cell appeared to create 7706
cells of overlap, essentially all of it this artefact, and zero was
unreachable by construction. The clearance rule is asymmetric on purpose - one
grown against the others solid - and the separation has to be asymmetric the
same way. A part's violation is then how much *other* material lies inside its
grown footprint, and zero everywhere means every pair is far enough apart.

*Always moving the worst offender deadlocks.* A part whose best position is no
better than the one it has does not move; its penalty then rises, which makes
it the worst offender again, and the search spends its whole budget on the one
part it cannot help. On four squares that fit a sheet easily, that left 23
cells unresolved for two hundred straight iterations while the part that could
have moved was never picked. Drawing the part at random, weighted by how badly
it does, resolved the same case in 53.

*The penalty has to be in the cost, not only in the choice.* Weighting which
part moves is not guided local search, it only looks like it. With penalties
steering selection alone, no squeeze on a tight sheet ever resolved: the
search stalled after a handful of moves because no single part had anywhere
better to go and the landscape never changed. Adding each part's weight into
the field itself - so sitting on a repeat offender genuinely costs more - made
every squeeze resolve, in 15 to 114 iterations instead of stalling at a
thousand. This is the whole point of the method and it is easy to implement in
a way that quietly does nothing.

Measured against the P4 result, both phases now exist and they win on
different instances:

| job | baseline | ruin and recreate | plus squeezing |
| --- | -------- | ----------------- | -------------- |
| `demo.job` used height | 1134 mm | **1035 mm** | 1035 mm |
| `shapes.job` used height | 504 mm | 504 mm | **483 mm** |
| `dense.job` utilisation | 95.17 % | **95.83 %** | 95.63 % |

Reshuffling wins where parts can be reordered into a better stack; squeezing
wins where the order is already right and the layout merely needs to settle.
Neither dominates, the split between them is a knob, and on `dense.job` the
budget spent squeezing would have been better spent reshuffling. A per-instance
or adaptive split is the obvious next thing to try.

Still missing from the sparrow recipe: inserting parts that did not fit at all
by placing them overlapping and separating, which is what would move
`dense.job`; and per-*pair* penalties rather than per-part.

### 3.10 Objective

Primary: maximise placed area / sheet area on a fixed sheet.
Secondary, and practically important for a workshop: minimise the used
bounding box height so the unused part of the plate stays a usable rectangular
remnant. A packing at 82 % utilisation with a clean 1200x200 strip left is
worth more than 84 % with waste sprayed everywhere.

### 3.11 Multiple sheets and final-sheet area

The first multi-sheet implementation is `bp_multisheet`, exposed by
`bp_pack --sheets <limit>`. Identical plates share one reusable bitmap;
results retain the original part indices and a placement range per plate.
Oversized parts and an exhausted plate limit produce an explicit partial
result. No empty plates are emitted.

The multi-sheet objective differs from section 3.10: fewer unplaced pieces,
then fewer plates, then a smaller **W * H** of the last plate's tight solid
bounding box. Equal area prefers smaller perimeter, then smaller height.
Clearance margins are not part of the reported solid box.

Construction carries overflow onto the next plate. Search alternates order
perturbations (global restarts and local swaps, changing plate assignments)
with last-container width trials (changing the final layout's aspect ratio).
Candidates replace the incumbent only when the lexicographic objective
improves. Buffers are caller owned; the core allocates nothing. A fixed seed
and iteration count reproduce a run; CPU-time limits are soft and checked
between complete candidates.

This establishes a constructive multi-sheet baseline, not a proof of
optimality. It does not yet run separation or overlap-based insertions across
plates. Reusing that machinery for plate evacuation and two-axis compaction
is a later refinement. DXF and a persistent remnant inventory are still open.

## 4. Module layout

Pure C11 core, no allocation in the hot path, caller-owned buffers, `bp_`
prefix for public symbols and `s_` for file-local statics, following the
rules in `AGENTS.md`.

Headers live in `include/`, implementations in `src/`, programs in `apps/`,
tests in `tests/`, and every build product under `build/`.

Modules marked **done** exist and are tested.

| module         | responsibility                                              |
| -------------- | ----------------------------------------------------------- |
| `bp_contract`  | **done** `BP_REQUIRE`, fail-fast macro                      |
| `bp_bitmap`    | **done** bitboard rows, shifts, span queries, counts        |
| `bp_mask`      | **done** part footprint as row spans, blit, overlap         |
| `bp_place`     | **done** bottom-left query in three modes, plus the complete feasible set per height |
| `bp_nester`    | **done** sequence decoder, produces a placement list        |
| `bp_job`       | **done** job file loading, mask construction, ownership     |
| `bp_svg`       | **done** solution to SVG, for eyeballing without a GUI      |
| `bp_pack`      | **done** CLI front end                                      |
| `bp_bench`     | **done** repeatable benchmark over cell sizes and modes     |
| `bp_polygon`   | **done** rings, validation, simplicity, bounds, rotation    |
| `bp_raster`    | **done** conservative fill, clearance growth                |
| `bp_score`     | **done** contact measure and ranked position choice         |
| `bp_search`    | **done** ruin and recreate with a time budget               |
| `bp_penetration`| **done** weighted cell counts with per-row running totals  |
| `bp_separate`  | **done** guided local search that resolves overlap          |
| `bp_multisheet`| **done** overflow, order search, final-sheet W * H objective |
| `bp_view`      | **done** published plate ranges, selection, solid bounds, full-batch SVG export |

The erosion path of section 3.3 will slot in behind the existing `bp_place`
API as a third mode, tested against the same reference scan.

One measured correction to section 3.2, from `bp_bench`: taking the *largest*
jump over all spans of the mask is a bad trade. It cuts candidate positions by
only 0.4 % over stopping at the first blocking span, and costs about seventy
times more work per position, because most candidates are rejected by their
very first span. `bp_place` stops at the first blocker.

A second measurement settles when the erosion path becomes worth building.
The jump scan is output sensitive, so on rectangles it scales gently, but a
staircase polygon has one span per row and that cost grows with resolution:

| cell | rectangles (`demo.job`) | irregular parts (`shapes.job`) |
| ---- | ----------------------- | ------------------------------ |
| 1.00 mm | 0.007 s | 0.070 s |
| 0.50 mm | 0.019 s | 0.411 s |
| 0.25 mm | 0.068 s | 3.34 s |

Roughly 2.8x per halving on rectangles against 8x on polygons, and 50x the
absolute cost at 0.25 mm. One construction pass is still affordable, but a
search doing thousands of them at a fine grid is not. That is the case for
section 3.3.

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

Built with the first of these:

1. **cimgui + GLFW + OpenGL 3** - `cimgui` is the generated **C** API for Dear
   ImGui (not to be confused with `CImGui.jl`, the Julia package that wraps it;
   no Julia is involved). GLFW and the GL headers come from the Pixi
   environment, cimgui and Dear ImGui from a pinned checkout that is not
   committed. No texture is needed at all: parts go straight into the ImGui
   draw list as merged rectangles, which is fewer moving parts than a texture
   upload and scales fine at a few thousand rectangles.
2. **Dear ImGui + GLFW + OpenGL3** - the mainstream path, but the GUI file
   becomes C++ calling the core through `extern "C"`. More examples online.
3. **raylib** - least code to first pixel, weaker for panels and controls.

Step zero, before any GUI: `bp_svg` writes the solution to SVG and Python
renders comparison sheets. That is enough to make progress on the algorithm
and it costs an afternoon, not a week.

## 8. Roadmap

The viewer now consumes single- and multi-sheet results through `bp_view`.
It renders one selected plate, a plate-local placement prefix, and the full
result's tight solid bounding box. The published result's settings and units
are distinct from pending controls. Plate metadata is allocated when a job
loads, and a failed replacement load retains the old job and view. Export
writes every complete plate and commits saved state only after the final
successful close; partial files survive a failure and are reported.
`tools/check_gui.py` renders deterministic plate selections, checks actual
pixels and SVG identities, and rechecks clearance. It needs a real display;
the portable view/range/export tests run with the normal test suite.

Each phase ends with tests green and a benchmark number recorded.

| phase | content                                                        | done when |
| ----- | -------------------------------------------------------------- | --------- |
| P0 | pixi env, Makefile, `bp_contract`, `bp_bitmap` + tests            | **done**: `pixi run test` and `pixi run test-sanitize` green |
| P1 | rectangles, reference scan + jump scan, 90 deg, SVG out, CLI      | **done**: `pixi run demo` packs 36 parts at 78.5 % with verified 1 mm clearance |
| P2 | erosion engine, equivalence property test, benchmark              | **done**: equivalent to the reference on 4000 random cases; 11-31x faster than scanning for the complete feasible set, and slower than the jump scan for first-fit, which is recorded rather than hidden |
| P3 | polygon rasteriser, arbitrary angles, kerf and margin             | **done**: `pixi run demo` packs L shapes, triangles and a frame at free angles, clearance verified from the SVG |
| P4 | scoring, ruin and recreate, time budget                            | **done**: 8.7 % lower used height on `demo.job` in 5 s, 95.17 to 95.87 % utilisation on `dense.job` |
| P5 | holes and part-in-part nesting                                    | a part provably placed inside another's hole |
| P6 | penetration field, guided local search, squeeze loop (3.9)        | **done**: `shapes.job` used height 504 to 483 mm; wins where reshuffling does not, and the reverse |
| P7 | GUI viewer and stepper                                            | **done**: `pixi run gui`, pan and zoom, step slider, live re-pack |
| P8 | multiple sheets, remnant tracking, DXF import/export              | **partial**: identical sheets and final-box-area search built; remnant inventory and DXF remain |
| P8a | selectable plate sizes, presets, raster budget                   | **done**: `--sheet-size` in CLI and viewer, budget enforced, memory and time measured ([plan](docs/plan_plaatformaten.md)) |
| P9 | guillotine cutting: layouts made only of edge-to-edge saw lines   | **G0-G4 done**: `bp_pack --guillotine [--search]` writes a checked saw list and drawings, and the viewer replays it cut by cut; 1.07x a lower bound on ten standard classes with search ([plan](docs/plan_guillotine.md)) |

## 9. Open questions

Selectable plate sizes are built: presets (500x500, 600x600, 600x300,
800x600, 1200x1200 and 1250x2500 mm), custom dimensions and shared
preallocation limits in `bp_sheet`, used by `bp_pack`, `bp_gui` and
`bp_job_load_with`. The scope is a freely selected rectangular plate per run,
not a stock cutting plan for a smaller machine bed. The cell and memory caps
are product guardrails, enforced and measured on representative workloads;
see [the implementation plan](docs/plan_plaatformaten.md) for the design and
the measurements.

1. ~~Grid resolution~~ - settled: configurable, default 1.0 mm, 0.5 mm fine
   setting, 0.1 mm rejected as a global search resolution (RESEARCH.md 7).
2. ~~Which angles for free rotation?~~ - settled: quarter turns. Most parts
   for this workshop have right angles, so `every:90` is the working default.
   Arbitrary angles stay supported and tested, and cost nothing when unused;
   they are a mask per angle per part when they are.
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
