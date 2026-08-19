#!/usr/bin/env python3
"""Independently verify a layout SVG written by bp_pack.

Checks, without trusting the solver's own assertions, that every part
rectangle lies inside the sheet, that no two parts overlap, and that the gap
between any two parts is at least the requested clearance.

    python3 tools/check_layout.py demo.svg --clearance 1.0
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET

SVG_NS = "{http://www.w3.org/2000/svg}"


def parse(path):
    """Returns (sheet_w, sheet_h, [[rect, ...] per part])."""
    root = ET.parse(path).getroot()
    view = [float(v) for v in root.get("viewBox").split()]
    sheet_w, sheet_h = view[2], view[3]
    parts = []
    for group in root.findall(f"{SVG_NS}g"):
        rects = []
        for rect in group.findall(f"{SVG_NS}rect"):
            rects.append((
                float(rect.get("x")), float(rect.get("y")),
                float(rect.get("width")), float(rect.get("height"))))
        if rects:
            parts.append(rects)
    return sheet_w, sheet_h, parts


def gap(a, b):
    """Chebyshev-style separation between two axis-aligned rectangles."""
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = max(bx - (ax + aw), ax - (bx + bw))
    dy = max(by - (ay + ah), ay - (by + bh))
    if dx >= 0 or dy >= 0:
        return max(dx, dy)
    return -min(-dx, -dy)  # negative: they overlap


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("svg")
    parser.add_argument("--clearance", type=float, default=0.0,
                        help="minimum required gap in mm")
    parser.add_argument("--tolerance", type=float, default=1e-6)
    args = parser.parse_args()

    sheet_w, sheet_h, parts = parse(args.svg)
    problems = []
    area = 0.0

    for index, rects in enumerate(parts):
        for r in rects:
            area += r[2] * r[3]
            if (r[0] < -args.tolerance or r[1] < -args.tolerance
                    or r[0] + r[2] > sheet_w + args.tolerance
                    or r[1] + r[3] > sheet_h + args.tolerance):
                problems.append(f"part {index} rect {r} leaves the sheet")

    worst = float("inf")
    for i in range(len(parts)):
        for j in range(i + 1, len(parts)):
            for a in parts[i]:
                for b in parts[j]:
                    d = gap(a, b)
                    worst = min(worst, d)
                    if d < args.clearance - args.tolerance:
                        problems.append(
                            f"parts {i} and {j} are {d:.3f} mm apart, "
                            f"below the required {args.clearance:.3f} mm")

    print(f"sheet        {sheet_w:.1f} x {sheet_h:.1f} mm")
    print(f"parts        {len(parts)}")
    print(f"placed area  {area:.0f} mm2 ({100 * area / (sheet_w * sheet_h):.2f} %)")
    if worst < float("inf"):
        print(f"closest pair {worst:.3f} mm apart "
              f"(required {args.clearance:.3f} mm)")
    if problems:
        for problem in problems[:20]:
            print("PROBLEM:", problem, file=sys.stderr)
        print(f"{len(problems)} problem(s)", file=sys.stderr)
        return 1
    print("ok: no overlaps, everything inside the sheet, clearance respected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
