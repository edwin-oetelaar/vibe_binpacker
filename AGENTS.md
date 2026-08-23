# Project Instructions

These instructions apply to the complete project tree. Preserve this file when
moving the project. A coding agent must read it before making changes.

## Context

This project is a raster-based 2D nesting solver: it decides where to cut a
set of simple polygons out of a rectangular sheet of material so that as
little as possible is wasted. The sheet is modelled as a grid of cells, one
bit per cell, and collision detection is bitwise.

Read `PLAN.md` for the design and the phased roadmap, and `RESEARCH.md` for
the literature the design is based on. The short version: the bitboard is a
collision detection engine with a clean API, bottom-left-fill is the
initialiser, and the optimiser is overlap minimisation by guided local search.

Target is Linux on a normal x86-64 workstation, built inside a Pixi
environment. The core must stay portable to plain embedded C toolchains.

## Layout

    include/        every public header, one per module
    src/            the solver, one implementation file per header
    apps/           programs: bp_pack, bp_bench
    tests/          one test program per module, plus test_support.h
    gui/            the viewer; the only third-party dependency
    jobs/           example job files
    tools/          Python tooling and the GUI dependency fetcher
    third_party/    fetched, pinned, never committed
    build/          everything the build produces, and nothing else

Keep build output out of the source tree. `make clean` is `rm -rf build`, and
that must remain true.

## C Style

Follow Pieter Hintjens' Scalable C / Code Connected style:

- Keep modules small, cohesive and independently testable.
- Use one public header and one implementation file per module.
- Prefix public types and functions with `bp_`.
- Prefix private file-local helpers with `s_` and declare them `static`.
- Prefer plain structures and direct control flow over abstraction layers.
- Do not add frameworks or external dependencies without a measured need.
- Use C11 and remain compatible with normal embedded C toolchains.
- Use fixed-width integers where representation matters.
- Prefer caller-owned buffers for explicit, deterministic memory use.
- Do not use global mutable state.
- Put units in domain names, such as `width_cells` and `cell_size_mm`.
- Prefer one parameter struct with designated initializers over long positional
  argument lists. Primitive fields inside that struct are allowed.

## Design By Contract

Every function, including private helpers, checks preconditions at its
boundary. Invalid caller input is a programming defect:

- Fail immediately with `BP_REQUIRE`, which stays active in every build.
- Never silently repair, clamp, reorder or ignore invalid caller input.
- Keep code compact after the boundary checks; avoid repeated defensive checks.
- Distinguish contract violations from expected runtime failures. Invalid
  geometry, out-of-range cells and insufficient buffers are contract
  violations. A part that does not fit on the sheet is an expected result and
  is reported, not aborted.
- Document important preconditions and invariants in the public header.

For expected runtime failures, return explicit tagged result structures by
value. Do not encode a new API's failure as `-1`, error `NULL`, or ambient
`errno`. Do not add result machinery to functions with no expected runtime
failure.

## Comments And Documentation

- Add a short comment to every function explaining what it does and why.
- Document ownership, input validity, capacities and limitations in headers.
- Explain decisions and invariants, not obvious syntax.
- Keep `README.md` current with build commands and a minimal usage example.
- Use ASCII unless an existing file requires another character set.

## Testing

- Every public behavior needs focused deterministic tests.
- Test normal and boundary cases, especially around 64-bit word boundaries.
- Test fail-fast contracts on Linux with subprocess death tests.
- Build and run with `pixi run test`. Tests are discovered by `tests/test_*.c`;
  adding a file is enough, no Makefile edit.
- Keep `-std=c11 -Wall -Wextra -Werror -pedantic` enabled.
- Run `pixi run test-sanitize` for every change to indexing, shifting or
  masking. Bit manipulation across word boundaries is exactly the code that
  needs AddressSanitizer and UndefinedBehaviorSanitizer.
- Do not make performance claims without a repeatable benchmark.

## Invariants Of The Bitmap Module

`bp_bitmap` is the foundation everything else rests on:

- Cell `(x, y)` is bit `x % 64` of word `x / 64` in row `y`. The least
  significant bit of a word is the smallest x.
- Rows are word aligned: `stride_words = (width_cells + 63) / 64`.
- **Padding bits beyond `width_cells` in the last word of a row are always
  zero.** Every write keeps this invariant, so popcounts and shifts never see
  phantom cells. Any new write path must mask the tail.
- Storage is caller owned and must outlive the bitmap. The module allocates
  nothing.
- Cell ranges are half-open, so parts that touch do not overlap.

## Change Discipline

- Read existing code and tests before editing.
- Keep changes scoped to requested behavior.
- Do not rewrite working code solely to impose a new style.
- Do not remove contracts, comments or tests to reduce line count.
- Preserve unrelated user changes.
- Report changed files and verification commands exactly.
