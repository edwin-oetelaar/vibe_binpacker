#!/usr/bin/env python3
"""Render deterministic GUI cases and check plate isolation, bounds and exports.

Requires a working display and a built bp_gui, but only the Python stdlib.
All screenshots, PNG previews and fixture files are written under build/.

    pixi run gui
    python3 tools/check_gui.py
"""
import argparse
from collections import Counter
from pathlib import Path
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib


def require(condition, message):
    """Fail with context instead of relying on Python's optional assertions."""
    if not condition:
        raise RuntimeError(message)


def run(binary, *args, expected=0):
    """Run an isolated GUI instance with a bounded lifetime and checked status."""
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                            text=True, timeout=90)
    require(result.returncode == expected,
            f"GUI exit {result.returncode}, expected {expected}: {result.stderr}")


def image(path):
    """Read our P6 framebuffer and write a lossless PNG for visual inspection."""
    with path.open("rb") as source:
        require(source.readline() == b"P6\n", "not a P6 capture")
        width, height = map(int, source.readline().split())
        require(source.readline() == b"255\n", "unexpected PPM depth")
        pixels = source.read()
    require(len(pixels) == width * height * 3, "incomplete framebuffer capture")

    def chunk(kind, payload):
        """Encode one PNG chunk, including the mandatory CRC."""
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload)))

    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    path.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    # Restrict colour checks to the canvas, excluding controls and headers.
    counts = Counter()
    yellow = []
    for y in range(110, height):
        for x in range(365, width):
            offset = (y * width + x) * 3
            colour = pixels[offset:offset + 3]
            counts[colour] += 1
            if colour == bytes((255, 223, 64)):
                yellow.append((x, y))
    return counts, yellow


def labels(path):
    """Read full exported part identities, independently of GUI selection."""
    root = ET.parse(path).getroot()
    return sorted(int(text.text) for text in
                  root.findall("{http://www.w3.org/2000/svg}text"))


def check_layout(path):
    """Recheck geometry and clearance with the project's independent verifier."""
    subprocess.run([sys.executable, "tools/check_layout.py", str(path),
                    "--clearance", "1"], check=True, timeout=60)


def main():
    """Exercise actual rendered plates, replay, single mode and expected failures."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/bp_gui"))
    parser.add_argument("--out", type=Path, default=Path("build/gui-check"))
    args = parser.parse_args()
    require(args.binary.is_file(), "build the GUI with pixi run gui first")
    require(args.out.resolve().is_relative_to(Path("build").resolve()),
            "GUI checks must write under build/")
    args.out.mkdir(parents=True, exist_ok=True)
    common = ["jobs/multisheet.job", "--sheets", "2", "--search", "128",
              "--frames", "8"]
    blue, orange, green = map(bytes, [(76, 120, 168), (245, 133, 24), (84, 162, 75)])
    run(args.binary, *common, "--sheet", 1, "--svg", args.out / "multi.svg",
        "--shot", args.out / "plate1.ppm")
    run(args.binary, *common, "--sheet", 2, "--shot", args.out / "plate2.ppm")
    first, _ = image(args.out / "plate1.ppm")
    second, box = image(args.out / "plate2.ppm")
    require(first[blue] > 10000 and first[orange] == first[green] == 0,
            "plate 1 does not isolate its blue part")
    require(second[blue] == 0 and second[orange] > 10000 and second[green] > 10000,
            "plate 2 does not isolate its orange and green parts")
    require(len(box) > 500, "final bounding box was not drawn")
    box_width = max(x for x, _ in box) - min(x for x, _ in box)
    box_height = max(y for _, y in box) - min(y for _, y in box)
    require(abs(box_width / box_height - 60 / 91) < 0.02,
            "rendered box differs from the 60 x 91 mm result")
    require(labels(args.out / "multi-001.svg") == [1], "plate 1 export identities")
    require(labels(args.out / "multi-002.svg") == [2, 3], "plate 2 export identities")
    check_layout(args.out / "multi-001.svg")
    check_layout(args.out / "multi-002.svg")
    run(args.binary, *common, "--sheet", 2, "--step", 0,
        "--svg", args.out / "hidden.svg", "--shot", args.out / "hidden.ppm")
    hidden, hidden_box = image(args.out / "hidden.ppm")
    require(hidden[blue] == hidden[orange] == hidden[green] == 0,
            "zero local step still draws parts")
    require(len(hidden_box) > 500, "replay changed the final-result box")
    require(labels(args.out / "hidden-002.svg") == [2, 3], "replay truncated export")
    run(args.binary, "jobs/nested_hole.job", "--svg", args.out / "single.svg",
        "--shot", args.out / "single.ppm", "--frames", 8)
    image(args.out / "single.ppm")
    require(len(labels(args.out / "single.svg")) == 2, "single-plate regression")
    check_layout(args.out / "single.svg")
    run(args.binary, "jobs/multisheet.job", "--sheets", 1,
        "--svg", args.out / "partial.svg", "--frames", 2)
    require(labels(args.out / "partial-001.svg") == [1], "partial-result regression")
    oversized = args.out / "oversized.job"
    oversized.write_text("sheet 10 10\nrect 1 1 20 20 0\n", encoding="ascii")
    run(args.binary, oversized, "--sheets", 2, "--shot", args.out / "empty.ppm",
        "--frames", 8)
    empty, _ = image(args.out / "empty.ppm")
    require(empty[blue] == empty[orange] == empty[green] == 0, "empty result draws material")
    run(args.binary, *common, "--sheet", 3, expected=1)
    run(args.binary, *common, "--sheet", 2, "--step", 3, expected=1)
    run(args.binary, *common, "--svg", args.out / "absent" / "result.svg", expected=1)
    print(f"GUI checks passed; PNG previews and SVGs in {args.out}")


if __name__ == "__main__":
    main()
