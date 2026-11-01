#!/usr/bin/env python3
"""Check plate size selection in the real GUI: presets, apply, refusal, export.

Requires a working display and a built bp_gui, but only the Python stdlib.
Every case runs the window, so what is checked is what a user would see and
save. All screenshots, PNG previews and SVGs are written under build/.

    pixi run gui
    python3 tools/check_gui_sheet_size.py
"""
import argparse
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

from check_gui import check_layout, image, labels, require

# The sheet fill in the viewer; with every part hidden it covers the sheet.
SHEET_FILL = (42, 42, 42)
# Below the two plate header lines and right of the control panel.
CANVAS_TOP = 78
CANVAS_LEFT = 365


def run(binary, *args, expected=0):
    """Run one bounded GUI instance and return its stderr for checking."""
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                            text=True, timeout=90)
    require(result.returncode == expected,
            f"GUI exit {result.returncode}, expected {expected}: {result.stderr}")
    return result.stderr


def svg_size(path):
    """Return the exported plate size in mm, from the viewBox and from the units."""
    root = ET.parse(path).getroot()
    _, _, width, height = map(float, root.get("viewBox").split())
    require(root.get("width") == f"{width:.3f}mm"
            and root.get("height") == f"{height:.3f}mm",
            f"{path}: physical size differs from the viewBox")
    return width, height


def drawn_aspect(path):
    """Return width / height of the drawn sheet, measured in screen pixels."""
    with path.open("rb") as source:
        require(source.readline() == b"P6\n", "not a P6 capture")
        width, height = map(int, source.readline().split())
        require(source.readline() == b"255\n", "unexpected PPM depth")
        pixels = source.read()
    fill = bytes(SHEET_FILL)
    xs = []
    ys = []
    for y in range(CANVAS_TOP, height):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        for x in range(CANVAS_LEFT, width):
            if row[x * 3:x * 3 + 3] == fill:
                xs.append(x)
                ys.append(y)
    require(xs, f"{path}: no sheet drawn")
    return (max(xs) - min(xs) + 1) / (max(ys) - min(ys) + 1)


def check_shown(binary, out, name, job, width_mm, height_mm, *args):
    """Pack with a size, then require the window and the SVG to agree on it."""
    run(binary, job, *args, "--frames", 8, "--svg", out / f"{name}.svg")
    require(svg_size(out / f"{name}.svg") == (width_mm, height_mm),
            f"{name}: SVG is not {width_mm} x {height_mm} mm")
    check_layout(out / f"{name}.svg")
    # Hide the parts so the sheet fill shows its full extent.
    run(binary, job, *args, "--frames", 8, "--step", 0,
        "--shot", out / f"{name}.ppm")
    image(out / f"{name}.ppm")
    aspect = drawn_aspect(out / f"{name}.ppm")
    require(abs(aspect / (width_mm / height_mm) - 1.0) < 0.02,
            f"{name}: drawn sheet aspect {aspect:.3f}, expected "
            f"{width_mm / height_mm:.3f}")


def main():
    """Exercise presets, orientation, apply, refusal and plate selection."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/bp_gui"))
    parser.add_argument("--out", type=Path, default=Path("build/gui-sheet-size"))
    args = parser.parse_args()
    binary = args.binary
    out = args.out
    require(binary.is_file(), "build the GUI with pixi run gui first")
    require(out.resolve().is_relative_to(Path("build").resolve()),
            "GUI checks must write under build/")
    out.mkdir(parents=True, exist_ok=True)

    # The job file's own size, then a preset in both orientations.
    check_shown(binary, out, "file", "jobs/demo.job", 1200.0, 1200.0)
    check_shown(binary, out, "laser", "jobs/demo.job", 800.0, 600.0,
                "--sheet-size", 800, 600)
    check_shown(binary, out, "laser_turned", "jobs/demo.job", 600.0, 800.0,
                "--sheet-size", 600, 800)
    # The large plate inside the budget at both resolutions.
    check_shown(binary, out, "large", "jobs/demo.job", 1250.0, 2500.0,
                "--sheet-size", 1250, 2500)
    check_shown(binary, out, "large_fine", "jobs/demo.job", 1250.0, 2500.0,
                "--sheet-size", 1250, 2500, "--cell", 0.5)
    # A rectangular remnant typed by hand. The sheet rounds down to whole
    # cells, so the model and therefore the SVG are 730 mm, never wider than
    # the real 730.5 mm remnant.
    check_shown(binary, out, "remnant", "jobs/demo.job", 730.0, 412.0,
                "--sheet-size", 730.5, 412)

    # The window screenshot for the README, 800 x 600 mm selected.
    run(binary, "jobs/demo.job", "--sheet-size", 800, 600, "--frames", 8,
        "--shot", out / "readme_laser.ppm")
    image(out / "readme_laser.ppm")

    # Apply & Pack replaces the result with one on the new size.
    stderr = run(binary, "jobs/demo.job", "--apply-sheet-size", 800, 600,
                 "--frames", 4, "--svg", out / "applied.svg")
    require("not applied" not in stderr, f"valid size refused: {stderr}")
    require(svg_size(out / "applied.svg") == (800.0, 600.0),
            "Apply & Pack did not export the applied size")
    check_layout(out / "applied.svg")

    # A refused size keeps the previous result, shown and exportable.
    baseline = labels(out / "file.svg")
    for size in ((9000, 600), (0, 600), (-800, 600), ("nan", 600)):
        stderr = run(binary, "jobs/demo.job", "--apply-sheet-size", *size,
                     "--frames", 4, "--svg", out / "kept.svg")
        require("not applied" in stderr, f"{size}: refusal not reported")
        require(svg_size(out / "kept.svg") == (1200.0, 1200.0),
                f"{size}: refusal changed the exported size")
        require(labels(out / "kept.svg") == baseline,
                f"{size}: refusal changed the result")
    stderr = run(binary, "jobs/demo.job", "--apply-sheet-size", 9000, 600,
                 "--frames", 4)
    require("try 2 mm cells" in stderr, "refusal without a suggestion")

    # Fewer plates after applying: selection restarts at plate 1.
    multi = ["jobs/multisheet.job", "--sheets", 2, "--search", 128]
    run(binary, *multi, "--frames", 4, "--svg", out / "two.svg")
    require(labels(out / "two-002.svg") == [2, 3], "baseline needs two plates")
    run(binary, *multi, "--apply-sheet-size", 208, 104, "--frames", 4,
        "--svg", out / "one.svg")
    require(sorted(labels(out / "one-001.svg")) == [1, 2, 3],
            "the wider plate should take every part")
    require(not (out / "one-002.svg").exists(), "a stale second plate was saved")
    run(binary, *multi, "--apply-sheet-size", 208, 104, "--sheet", 2,
        "--frames", 2, expected=1)
    run(binary, *multi, "--apply-sheet-size", 208, 104, "--sheet", 1,
        "--frames", 2)

    # A size refused before anything was packed fails an automated export.
    for size in ((0, 600), (9000, 600)):
        stderr = run(binary, "jobs/demo.job", "--sheet-size", *size,
                     "--frames", 2, "--svg", out / "never.svg", expected=1)
        require("not applied" in stderr, f"{size}: start refusal not reported")
        require(not (out / "never.svg").exists(), "a refused size was saved")
    run(binary, "jobs/demo.job", "--sheet-size", "abc", 600, "--frames", 2,
        expected=1)
    print(f"GUI sheet size checks passed; previews and SVGs in {out}")


if __name__ == "__main__":
    main()
