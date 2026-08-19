# Literature review: what is already known about nesting

Written 2026-08-19. Purpose: decide what to build, based on what actually
works, not on what is easiest to invent. Sources are listed at the bottom.

## 1. The name of the problem

In the Wascher/Haussner/Schumann typology this is a **two-dimensional
irregular open-dimension / bin packing problem**, universally called
**nesting** in the literature and the sheet-metal industry. It is NP-hard, and
it is harder than rectangle packing because the non-overlap constraint itself
is geometrically expensive to evaluate. That second difficulty is the whole
story: nesting research is mostly research into *how to answer "do these two
shapes overlap, and if so by how much" fast enough to do it a billion times*.

The academic benchmark variant is usually **strip packing** (fixed width,
minimise length). Ours is the **fixed-sheet** variant (1200 x 1200, maximise
placed area, spill to the next sheet). The algorithms transfer; only the outer
loop differs, and section 6 explains how.

## 2. Geometry representations, and where ours sits

Four families exist. This is the single most important design axis.

### No-Fit Polygon (NFP)
For an ordered pair of shapes, the NFP is the region where shape B's reference
point may not lie without overlapping A. Placement then reduces to point-in-
polygon tests. Exact and resolution-free, and it is what SVGnest, Deepnest and
libnest2d use. The catch is well documented: NFP construction for concave
shapes with holes and degenerate touching cases is a robustness minefield, and
you need one NFP per ordered pair *per rotation combination*, which explodes
with free rotation.

### Raster / grid (our plan)
Discretise the sheet into cells, rasterise shapes into bit masks, overlap is a
bitwise AND. Goes back to Segenreich & Braga (1986) and Oliveira & Ferreira
(1993). Simple and robust; concave shapes and holes are free.

The standard criticism, stated bluntly in the jagua-rs paper (2025):
*"discretization requires containers to be deflated and items inflated, and low
grid resolution can severely affect solution quality, while increasing
resolution raises computational cost and memory usage."* That criticism is
correct and section 7 quantifies it for our sheet size.

The standard *defence*, from Sato et al. (2019), is equally important:
a grid permits **a complete investigation of the placement space for each
item, which is generally not possible in continuous space**. You can look at
every legal position at once. Continuous methods can only sample. This is
exactly the property our erosion engine exploits, and it is the reason the
raster route is defensible rather than merely convenient.

### Semi-discrete (Chehrazad, Roose & Wauters, EJOR 2022)
A hybrid: pieces and strip are represented by equidistant **vertical line
segments** - discrete in y, continuous in x. Non-overlap becomes a set of
1D segment comparisons. Their C++ BLF runs a full typical instance *in a few
milliseconds*, rotation included. This is the obvious escape hatch if a 1 mm
y-grid turns out to be too coarse in x: keep the row structure, drop the x
quantisation.

### Quadtree + surrogate shapes (jagua-rs, 2025)
The current best-engineered continuous approach. A quadtree spatially prunes
candidate pairs, cheap surrogate primitives (inscribed circles, "poles of
inaccessibility") reject most remaining pairs, and exact polygon tests run only
for the survivors. It is deliberately packaged as a reusable **collision
detection engine**, decoupled from the optimiser.

That decoupling is the design lesson to steal regardless of representation:
**our bitboard is a collision detection engine, and the optimiser must not
know how it works.**

## 3. Algorithmic families, and which ones win

### 3.1 Constructive: bottom-left and bottom-left-fill
Place parts one at a time in a fixed order, each at the lowest-then-leftmost
feasible position. Jakobs (1996) with a GA over the order; Dowsland, Vaid &
Dowsland (2002) refined the placement policy; Burke, Hellier, Kendall &
Whitwell (2006, *Operations Research* 54(3)) produced the reference BLF for
irregular shapes using a hybrid raster/geometry representation.

This is exactly the algorithm you sketched in your first message. It is a
legitimate, published, still-used baseline. It is also, on its own, roughly
5-10 % worse in density than what follows - which is the honest answer to
"I thought I had something clever": you did, it is called BLF, it is from the
1990s, and the field has moved past it as a *final* answer while still using
it as the *starting* point of every modern method.

### 3.2 Metaheuristics over the sequence
Wrap BLF in a GA or SA that permutes part order and orientation. Cheap to
build, quickly plateaus: the decoder is the bottleneck, because no reordering
can produce a layout BLF cannot express.

### 3.3 Overlap minimisation / separation - the family that wins
The decisive idea, and it is counter-intuitive: **allow parts to overlap
temporarily, and minimise total penetration instead of forbidding it.**
Feasibility becomes an optimisation objective, which turns a brutal
combinatorial placement problem into a smooth-ish continuous descent.

- Egeblad, Nielsen & Odgaard (2007): fast translation-based neighbourhood -
  for a chosen piece and axis, compute the position minimising overlap in one
  sweep.
- Umetani, Yagiura, Imahori, Imamichi, Nonobe & Ibaraki (2009): **guided local
  search** over the overlap minimisation problem, penalty weights per colliding
  pair increase while a collision persists, which pushes the search out of
  local optima.
- Imamichi, Yagiura & Nagamochi (2009): iterated local search with a nonlinear
  programming separation step.
- **Sato, Martins, Gomes & Tsuzuki (2019), EJOR 279(2)**: the *raster* version.
  Their **raster penetration map** (also called the Voronoi mountain) gives,
  for one piece, the overlap cost at *every* grid position at once; the piece
  jumps to the global minimum of that map. Reported as very competitive with
  the continuous methods. This paper is the single most relevant one for us:
  it is our data structure, doing the winning algorithm.

### 3.4 Current state of the art: sparrow (2025)
Gardeyn, Vanden Berghe & Wauters, *An open-source heuristic to reboot 2D
nesting research* (arXiv 2509.13329), built on jagua-rs. Open source, MPL-2.0.
It beats the previous state of the art on the standard benchmarks by roughly
1-5 % density, and about 5 % on the hardest instance (SWIM). The loop:

1. Construct an initial layout with **bottom-left-fill**.
2. **Exploration phase** (80 % of the time budget): shrink the strip by 0.1 %,
   which forces collisions, then run the **separation** procedure to resolve
   them. Success replaces the incumbent; failures go into a pool. When
   progress stalls, disrupt by swapping two large items.
3. **Compression phase** (20 %): from the best layout, retry with ever smaller
   shrink steps, 0.05 % down to 0.001 %, restoring the incumbent before each
   attempt so changes stay local.
4. **Separation** = guided local search. Collision cost is a proxy for
   penetration depth built from the shapes' poles of inaccessibility; per-pair
   GLS weights grow while a collision persists and decay once resolved. Each
   colliding item is repositioned by sampling candidate positions (random over
   the strip plus local around its current position), evaluating the weighted
   collision cost, and refining the best candidates by adaptive coordinate
   descent. A "strike" counter restarts from the best layout and eventually
   declares the feasibility problem failed.

Note what this is: **BLF as the initialiser, overlap minimisation as the
engine, and a monotone container squeeze as the outer loop.** Three parts. All
three are implementable in the raster model.

### 3.5 Exact methods
Toledo, Carravilla, Ribeiro, Oliveira & Gomes (2013), **the Dotted Board
Model**: a MIP where binary variables sit on *discrete grid points* of the
board - literally our model, solved to optimality. It handles 16-56 pieces,
with precision bounded by the discretisation. Useful to us in one specific
way: on small instances it gives a provable optimum to measure our heuristic
against. Leao, Toledo, Oliveira & Carravilla (EJOR 2020) review all such
models.

## 4. What this means for us

The plan from the first pass was "BLF + scoring + simulated annealing over the
order". The literature says that is the 1990s answer and it plateaus. The
correction:

> Keep the bitboard. Keep BLF, but only as the initialiser. Build the real
> engine as **raster overlap minimisation with guided local search**, and wrap
> it in the sparrow-style feasibility-sequence outer loop.

The good news is that our bitboard supports this better than it supports the
old plan, because of two data-structure results in the next section.

## 5. The two tricks that make the raster model competitive

### 5.1 Complete feasible set by erosion - O(rows x words) per height
As in PLAN.md section 3.3: all legal positions of a part are the morphological
erosion of the free space by the part mask, computed by shift-AND passes over
64-bit words. Roughly 4.6e6 word operations for a 300x200 part on a 1 mm
1200x1200 sheet. This is the "complete investigation of the placement space"
that Sato et al. name as the raster model's unique advantage.

### 5.2 Penetration in O(number of spans) by per-row prefix popcount
The overlap minimisation family needs a *count*, not a boolean, and counting
does not word-parallelise the way AND does. The fix is an integral-image trick.

For each sheet row `y`, keep the prefix count of occupied cells:

    S_y(x) = number of occupied cells in row y at indices < x

Then occupied cells in any interval `[p, q)` of that row is `S_y(q) - S_y(p)`,
one subtraction. A part is stored as row spans `(a, L)`, so

    penetration(part at (px, py))
        = sum over rows r, spans (a, L) of
          [ S_{py+r}(px + a + L) - S_{py+r}(px + a) ]

That is **two array reads and one subtraction per span**, independent of how
many cells the part covers. A 300x200 rectangle costs 200 subtractions per
candidate position instead of 60 000 cell tests. The prefix table is
1200 x 1200 x 2 bytes = 2.9 MB at 1 mm, rebuilt only for rows a placement
dirtied.

With that, the raster penetration map over a k x k sampling window costs
`k^2 x spans`, and a full-board map for a 200-row part is about 2.9e8
subtractions - a fraction of a second, and easily restricted to a plausible
y band. That is Sato's Voronoi mountain, affordable, in plain C.

These two together are the argument that we are not building a toy version of
the state of the art. We are building the raster branch of it, with the right
data structures.

## 6. Fixed sheet instead of a strip

Almost all benchmarks are strip packing (minimise length). We have a fixed
1200 x 1200 plate. The sparrow decomposition still applies, with the squeeze
moved to a different variable:

- **Strip packing**: shrink the strip length, re-separate, repeat.
- **Our case, single sheet**: the feasibility question is *"do these N+1 parts
  fit in the fixed sheet?"* Add the next part, re-separate, keep it if the
  collisions resolve. This is the knapsack variant and it is the same loop.
- **Our case, multiple sheets**: fill sheet 1 to failure, carry the remainder
  to sheet 2. Secondary objective on the last sheet: minimise the used
  bounding box, so the leftover is a rectangular remnant a workshop can
  actually re-use. This is a real requirement that the literature largely
  ignores, and it is cheap for us to add as a scoring term.

## 7. Grid resolution: what the numbers say

You asked whether 0.1 mm is affordable now that memory is not scarce. Memory
is not the constraint. Scan cost is.

| resolution | cells | bitboard | prefix table | erosion cost per part, relative |
| ---------- | ------------ | -------- | ------------ | ------- |
| 1.0 mm | 1.44e6 | 182 KB | 2.9 MB | 1x |
| 0.5 mm | 5.76e6 | 720 KB | 11.5 MB (u32) | ~8x |
| 0.25 mm | 2.30e7 | 2.8 MB | 92 MB (u32) | ~64x |
| 0.1 mm | 1.44e8 | 18 MB | 576 MB (u32) | ~1000x |

Erosion cost scales as sheet_height x part_rows x words_per_row, so halving
the cell size costs roughly 8x. At 0.1 mm a single full-board scan goes from
milliseconds to seconds, and a metaheuristic doing millions of them is dead.

The other cost of a coarse grid is conservative rasterisation: every part is
inflated by up to one cell all around. For a typical job - say 20 parts of
1000 mm perimeter - that is about `20 x 1000 x 0.5 mm = 10 000 mm2`, roughly
**0.7 % of a 1200 x 1200 sheet at 1 mm**, and 0.35 % at 0.5 mm. It is not
nothing, but it is smaller than the gap between a mediocre and a good
algorithm, which is the right thing to spend effort on first.

Measured, on this workstation, with the `jobs/demo.job` 36-part workload and
the bottom-left-fill jump scan (`pixi run benchmark`):

| cell | sheet cells | candidates | seconds | utilisation |
| ---- | ----------- | ---------- | ------- | ----------- |
| 2.00 mm | 600 x 600 | 248 531 | 0.004 | 78.66 % |
| 1.00 mm | 1200 x 1200 | 425 187 | 0.006 | 78.53 % |
| 0.50 mm | 2400 x 2400 | 849 802 | 0.018 | 78.53 % |
| 0.25 mm | 4800 x 4800 | 1 699 032 | 0.065 | 78.53 % |

Two things to read from that. First, the *construction* heuristic scales far
better than the erosion figures above: about 3x per halving rather than 8x,
because a bottom-left scan is output sensitive and stops as soon as it finds a
spot. It is the metaheuristic doing millions of scans that makes fine grids
expensive, not one pass. Second, the coarse grid flatters itself: utilisation
is counted in cells, and at 2 mm every part is rounded up more, so the 78.66 %
is measuring inflated parts rather than a better packing.

Recommendation: **make the cell size a configuration parameter from day one**
(it costs nothing now and is painful to retrofit), default 1.0 mm, and expect
0.5 mm to be the practical fine setting. Do not plan on 0.1 mm as a global
search resolution. If that accuracy turns out to matter, the correct answer is
not a finer grid, it is the semi-discrete representation of Chehrazad et al. -
keep the 1 mm rows, go continuous in x - or a final continuous nudge of the
finished layout.

## 8. Clearance and kerf: your idea is the standard one

Your suggestion - expand the polygons, do collision on the expanded version,
place the unexpanded original - is exactly right and is what the literature
calls inflation. Two details:

1. Dilate **only the candidate** and test it against undilated occupancy.
   Dilating both gives `2k` separation where you asked for `k`.
2. Dilating the solid mask automatically shrinks any **holes** inward by `k`,
   so clearance inside a hole is handled by the same operation. No special
   case.

sparrow exposes exactly this as a configurable "minimum item separation"
parameter, which confirms it is the industry-standard formulation.

## 9. What to build, concretely

Revised from PLAN.md, in dependency order:

1. **Bitboard collision engine** (`bp_bitmap`), configurable cell size. Erosion
   with shift-AND, per-row prefix popcount, dirty-row invalidation. This is the
   jagua-rs lesson: the engine is a component with a clean API, and the
   optimiser never reaches inside it.
2. **Conservative rasteriser** (`bp_raster`) with dilation for clearance, holes
   handled as cleared inner rings.
3. **BLF over the complete feasible set** (`bp_place`, `bp_nester`). Baseline,
   SVG output, benchmark numbers. Everything later is measured against this.
4. **Raster penetration map + guided local search** (`bp_separate`). The real
   engine, following Sato et al. and Umetani et al.: per-pair GLS weights,
   move the worst-colliding part to the minimum of its penetration map,
   escalate weights on persistent collisions.
5. **Feasibility-sequence outer loop** (`bp_search`), sparrow-style: add the
   next part / shrink the used region, separate, accept or roll back; a
   disruption move when stalled; explicit time budget and seeded PRNG so runs
   are reproducible.
6. **Remnant-aware scoring** for the last sheet.
7. Holes and part-in-part; multi-sheet; DXF via Python tooling; GUI last.

Steps 1-3 are a working nester. Steps 4-5 are where the material savings are.

## 10. Open source worth reading before writing code

- **sparrow** - github.com/JeroenGar/sparrow, MPL-2.0. Current state of the
  art, Rust. Read the separation loop.
- **jagua-rs** - github.com/JeroenGar/jagua-rs, MPL-2.0. The collision engine
  underneath it. Read the API boundary, not the geometry.
- **Deepnest / SVGnest** - MIT, JS/Electron, NFP + GA. The practical
  laser-cutting workflow (DXF import, common-line cutting) and its known
  weaknesses on large files.
- **libnest2d** - used by Cura and PrusaSlicer. C++, NFP based.
- **rectpack**, and Jylanki's *A Thousand Ways to Pack the Bin* - only relevant
  to the rectangle-only phase: MaxRects, Skyline, Guillotine, Shelf.

## Sources

- Leao, Toledo, Oliveira, Carravilla, Alvarez-Valdes, *Irregular packing
  problems: a review of mathematical models*, EJOR 282(3):803-822, 2020.
  https://www.sciencedirect.com/science/article/abs/pii/S0377221719303820
- Toledo, Carravilla, Ribeiro, Oliveira, Gomes, *The Dotted-Board Model: a new
  MIP model for nesting irregular shapes*, Int. J. Production Economics, 2013.
  https://www.sciencedirect.com/science/article/abs/pii/S0925527313001722
- Sato, Martins, Gomes, Tsuzuki, *Raster penetration map applied to the
  irregular packing problem*, EJOR 279(2):657-671, 2019.
  https://ideas.repec.org/a/eee/ejores/v279y2019i2p657-671.html
- Chehrazad, Roose, Wauters, *A fast and scalable bottom-left-fill algorithm to
  solve nesting problems using a semi-discrete representation*, EJOR
  300(3):809-826, 2022. https://arxiv.org/abs/2103.08739
- Gardeyn, Vanden Berghe, Wauters, *An open-source heuristic to reboot 2D
  nesting research*, arXiv:2509.13329, 2025. https://arxiv.org/abs/2509.13329
- Gardeyn et al., *Decoupling geometry from optimization in 2D irregular
  cutting and packing problems: an open-source collision detection engine*,
  arXiv:2508.08341, 2025. https://arxiv.org/abs/2508.08341
- Umetani, Yagiura, Imahori, Imamichi, Nonobe, Ibaraki, *Solving the irregular
  strip packing problem via guided local search for overlap minimization*,
  ITOR, 2009.
  https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1475-3995.2009.00707.x
- Imamichi, Yagiura, Nagamochi, *An iterated local search algorithm based on
  nonlinear programming for the irregular strip packing problem*, Discrete
  Optimization, 2009.
  https://www.sciencedirect.com/science/article/pii/S1572528609000218
- Burke, Hellier, Kendall, Whitwell, *A new bottom-left-fill heuristic
  algorithm for the two-dimensional irregular packing problem*, Operations
  Research 54(3):587-601, 2006.
  https://ideas.repec.org/a/inm/oropre/v54y2006i3p587-601.html
- Bettinelli et al., *Study of the grid size impact on a raster based strip
  packing problem solution*, IFAC-PapersOnLine, 2016.
  https://www.sciencedirect.com/science/article/pii/S2405896316328476
- Jylanki, *A Thousand Ways to Pack the Bin*, 2010.
- sparrow: https://github.com/JeroenGar/sparrow
- jagua-rs: https://github.com/JeroenGar/jagua-rs
