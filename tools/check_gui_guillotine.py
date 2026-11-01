#!/usr/bin/env python3
"""Check the viewer's panel saw mode in the real window.

Requires a working display and a built bp_gui, but only the Python stdlib.
Renders jobs/saw.job in panel saw mode and checks actual pixels: the stage
colours appear exactly when the saw list replay has reached those cuts, and
the drawn plate has the plate's proportions. Every export is re-checked with
tools/check_cutplan.py, drawings included. All output goes under build/.

    pixi run gui
    python3 tools/check_gui_guillotine.py
"""
import argparse
from collections import Counter
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cutplan import check, check_svg  # noqa: E402
from check_gui import image, require  # noqa: E402

# Canvas area: below the two header lines, right of the control panel.
CANVAS_TOP = 78
CANVAS_LEFT = 365
STAGE_1 = bytes((214, 39, 40))
STAGE_2 = bytes((31, 119, 180))
CURRENT_CUT = bytes((255, 223, 64))
PLATE_EDGE = bytes((144, 144, 144))
JOB = "jobs/saw.job"


def run(binary, *args, expected=0):
    """Run one bounded GUI instance and return its stderr."""
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                            text=True, timeout=90)
    require(result.returncode == expected,
            f"GUI exit {result.returncode}, expected {expected}: {result.stderr}")
    return result.stderr


def canvas(path):
    """Return the canvas pixel counts and the drawn plate's aspect ratio."""
    image(path)
    with path.open("rb") as source:
        source.readline()
        width, height = map(int, source.readline().split())
        source.readline()
        pixels = source.read()
    counts = Counter()
    xs, ys = [], []
    for y in range(CANVAS_TOP, height):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        for x in range(CANVAS_LEFT, width):
            colour = row[x * 3:x * 3 + 3]
            counts[colour] += 1
            if colour == PLATE_EDGE:
                xs.append(x)
                ys.append(y)
    require(xs, f"{path}: no plate edge drawn")
    return counts, (max(xs) - min(xs) + 1) / (max(ys) - min(ys) + 1)


def checked_export(stem):
    """Re-check an exported saw list and its drawings; return the summary."""
    summary = check(Path(f"{stem}.csv").read_text(encoding="ascii"))
    for plate in range(summary["plates"]):
        check_svg(f"{stem}-{plate + 1:03d}.svg", plate, summary)
    return summary


def main():
    """Exercise replay, plate selection, export, refusals and search."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/bp_gui"))
    parser.add_argument("--out", type=Path, default=Path("build/gui-guillotine"))
    args = parser.parse_args()
    binary, out = args.binary, args.out
    require(binary.is_file(), "build the GUI with pixi run gui first")
    require(out.resolve().is_relative_to(Path("build").resolve()),
            "GUI checks must write under build/")
    out.mkdir(parents=True, exist_ok=True)
    saw = [JOB, "--guillotine", "--frames", 8]

    # Export, and the plan the window shows, checked independently.
    run(binary, *saw, "--svg", out / "plan.svg", "--shot", out / "full.ppm")
    summary = checked_export(out / "plan")
    require(summary["placed"] == summary["ordered"] == 32, "not every part sawn")
    full, aspect = canvas(out / "full.ppm")
    require(abs(aspect / (2800 / 2070) - 1.0) < 0.02,
            f"drawn plate aspect {aspect:.3f}, expected {2800 / 2070:.3f}")
    require(full[STAGE_1] > 200 and full[STAGE_2] > 200,
            "a fully cut plate shows no stage 1 and 2 cuts")

    # Step 0: the plate before the saw touches it shows no cut at all.
    run(binary, *saw, "--step", 0, "--shot", out / "step0.ppm")
    before, _ = canvas(out / "step0.ppm")
    require(before[STAGE_1] == before[STAGE_2] == before[CURRENT_CUT] == 0,
            "step 0 draws cuts")
    # Step 9 on this plan: edge trims and stage 1 only, the last one marked.
    run(binary, *saw, "--step", 9, "--shot", out / "step9.ppm")
    partway, _ = canvas(out / "step9.ppm")
    require(partway[STAGE_1] > 200 and partway[STAGE_2] == 0,
            "step 9 should show stage 1 cuts and no stage 2 cut yet")
    require(partway[CURRENT_CUT] > 50, "the current cut is not marked")

    # The last plate holds two parts: far less part colour than plate 1.
    run(binary, *saw, "--sheet", 3, "--shot", out / "plate3.ppm")
    last, _ = canvas(out / "plate3.ppm")
    require(sum(last.values()) == sum(full.values()), "canvas size changed")
    require(last[PLATE_EDGE] > 0, "plate 3 not drawn")
    run(binary, *saw, "--sheet", 4, expected=1)
    run(binary, *saw, "--step", 999, expected=1)

    # A job without a blade width is refused for the saw, with the reason.
    stderr = run(binary, "jobs/demo.job", "--guillotine", "--frames", 2,
                 "--svg", out / "never.svg", expected=1)
    require("kerf" in stderr, "refusal does not name the missing kerf")

    # Apply & Pack on another plate size, and the search, both re-checked.
    stderr = run(binary, *saw, "--apply-sheet-size", 2440, 1220,
                 "--svg", out / "applied.svg")
    require("not applied" not in stderr, f"valid size refused: {stderr}")
    applied = checked_export(out / "applied")
    require(applied["plate_size"] == (244000, 122000), "applied size not exported")
    run(binary, *saw, "--search", 100, "--svg", out / "searched.svg")
    searched = checked_export(out / "searched")
    require(searched["plates"] <= summary["plates"], "search used more plates")

    # The README picture: plate 1 halfway through its saw list.
    run(binary, *saw, "--step", 14, "--shot", out / "readme.ppm")
    image(out / "readme.ppm")
    print(f"GUI panel saw checks passed; previews, lists and drawings in {out}")


if __name__ == "__main__":
    main()
