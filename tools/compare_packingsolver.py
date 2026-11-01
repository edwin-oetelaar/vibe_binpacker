#!/usr/bin/env python3
"""Compare bp_pack --guillotine with PackingSolver on identical instances.

PackingSolver (Fontan & Libralesso, MIT licence) is the state of the art for
staged guillotine packing. It is used here as an external yardstick only, not
as a dependency: install it in a separate environment and run this script
with that environment's Python.

    python3 -m venv build/ps-venv
    build/ps-venv/bin/pip install packingsolver
    build/ps-venv/bin/python tools/compare_packingsolver.py

Both solvers get the same instances, from tools/measure_guillotine.py, and the
same saw: three stages plus a sizing cut (non-exact, the ROADEF definition),
first cut either way, no blade width. bp_pack's cut lists are checked with
tools/check_cutplan.py; PackingSolver's solutions with its own feasibility
check. The bound is the strongest available per instance: our simple bound or
PackingSolver's own bin packing bound.

PackingSolver runs in its deterministic mode with a time limit per instance;
its time is wall-clock seconds as it reports them, ours CPU seconds summed
over the bp_pack processes.
"""
import argparse
from pathlib import Path
import random
import resource
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cutplan import check  # noqa: E402
from measure_guillotine import (CLASSES, INSTANCES, SIZES, berkey_wang,  # noqa: E402
                                lower_bound, martello_vigo)

import packingsolver as ps  # noqa: E402
import packingsolver.rectangleguillotine as rg  # noqa: E402


def instances(sizes):
    """Yield (class, side, items) in the order measure_guillotine uses."""
    for name in CLASSES:
        rng = random.Random(f"bp-guillotine-{name}")
        for count in SIZES:
            for _ in range(INSTANCES):
                if CLASSES.index(name) < 6:
                    side, items = berkey_wang(name, count, rng)
                else:
                    side, items = martello_vigo(name, count, rng)
                if count in sizes:
                    yield name, side, items


def ours(binary, work, tag, side, items, turn, rounds):
    """Run bp_pack on the saw; return its checked plate count."""
    job = work / f"{tag}.job"
    lines = [f"sheet {side} {side}", "kerf 0", "stages 3", "sizing-cut 1",
             "first-cut any"]
    lines += [f"rect {i + 1} 1 {w} {h} {turn}" for i, (w, h) in enumerate(items)]
    job.write_text("\n".join(lines) + "\n", encoding="ascii")
    cutlist = job.with_suffix(f".{rounds}.csv")
    search = ["--search", str(rounds)] if rounds else []
    process = subprocess.run([str(binary), "--guillotine", *search, "--cutlist",
                              str(cutlist), str(job)], capture_output=True, text=True)
    if process.returncode != 0:
        raise RuntimeError(f"{job}: {process.stderr}")
    return check(cutlist.read_text(encoding="ascii"))["plates"]


def theirs(side, items, turn, time_limit):
    """Run PackingSolver; return (plates, its bound, seconds)."""
    builder = rg.InstanceBuilder()
    builder.set_objective(ps.Objective.BinPacking)
    builder.set_number_of_stages(3)
    builder.set_cut_type(rg.CutType.NonExact)
    builder.set_first_stage_orientation(rg.CutOrientation.Any)
    builder.add_bin_type(side, side, copies=len(items))
    for w, h in items:
        builder.add_item_type(w, h, oriented=not turn, copies=1)
    parameters = rg.OptimizeParameters()
    parameters.optimization_mode = ps.OptimizationMode.NotAnytimeDeterministic
    parameters.verbosity_level = 0
    parameters.time_limit = time_limit
    output = rg.optimize(builder.build(), parameters)
    solution = output.solution
    if not solution.feasible() or solution.number_of_items() != len(items):
        raise RuntimeError("PackingSolver returned an incomplete solution")
    return solution.number_of_bins(), output.bin_packing_bound, output.time


def main():
    """Run both solvers on every instance and print a Markdown table."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=Path("build/bp_pack"))
    parser.add_argument("--work", type=Path, default=Path("build/packingsolver-compare"))
    parser.add_argument("--rounds", type=int, default=300)
    parser.add_argument("--time-limit", type=float, default=30.0,
                        help="PackingSolver seconds per instance")
    parser.add_argument("--sizes", type=int, nargs="+", default=list(SIZES))
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)

    print("| class | turns | bound | bp_pack | bp_pack searched | PackingSolver |")
    print("| --- | --- | ---: | ---: | ---: | ---: |")
    totals = {}
    their_seconds = 0.0
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    rows = {}
    for index, (name, side, items) in enumerate(instances(set(args.sizes))):
        for turn in (0, 1):
            tag = f"{name}-{index}-{turn}"
            built = ours(args.binary, args.work, tag, side, items, turn, 0)
            searched = ours(args.binary, args.work, tag, side, items, turn,
                            args.rounds)
            plates, bound, seconds = theirs(side, items, turn, args.time_limit)
            their_seconds += seconds
            row = rows.setdefault((name, turn), [0, 0, 0, 0])
            for slot, value in enumerate((max(bound, lower_bound(side, items)),
                                          built, searched, plates)):
                row[slot] += value
            print(f"{tag}: bound {row[0]}, ours {built}/{searched}, "
                  f"theirs {plates}", file=sys.stderr, flush=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    for (name, turn), row in rows.items():
        print(f"| {name} | {'yes' if turn else 'no'} | {row[0]} | "
              + " | ".join(f"{value} ({value / row[0]:.3f})" for value in row[1:])
              + " |")
        key = "turns" if turn else "fixed"
        totals.setdefault(key, [0, 0, 0, 0])
        for slot in range(4):
            totals[key][slot] += row[slot]
    for key, row in totals.items():
        print(f"| all | {key} | {row[0]} | "
              + " | ".join(f"{value} ({value / row[0]:.3f})" for value in row[1:])
              + " |")
    ours_seconds = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    print(f"\nbp_pack: {ours_seconds:.1f} CPU s for all runs, construction and "
          f"search, process start-up included. PackingSolver: {their_seconds:.1f} s "
          f"as it reports, time limit {args.time_limit:g} s per instance.")


if __name__ == "__main__":
    main()
