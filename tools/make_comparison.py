#!/usr/bin/env python3
"""Render two layout SVGs side by side as a PNG, for slides and posts.

    python3 tools/make_comparison.py before.svg after.svg out.png \
        --left "one pass" --right "10 seconds of search"

Reads the SVGs the solver writes, which contain nothing but rectangles, so no
SVG renderer is needed. The used height of each layout is measured from the
geometry and drawn as a line, because that is the number a workshop cares
about: what is left below it is scrap, what is left above it is a strip you
can still use.
"""
import argparse
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont

SVG_NS = "{http://www.w3.org/2000/svg}"

BACKGROUND = (24, 24, 27)
SHEET = (46, 46, 52)
SHEET_EDGE = (120, 120, 130)
TEXT = (235, 235, 238)
DIM = (150, 150, 158)
MARK = (255, 176, 46)


def load(path):
    """Returns (sheet_width, sheet_height, [(x, y, w, h, colour), ...])."""
    root = ET.parse(path).getroot()
    view = [float(v) for v in root.get("viewBox").split()]
    parts = []
    for group in root.findall(f"{SVG_NS}g"):
        fill = group.get("fill", "#888888").lstrip("#")
        colour = tuple(int(fill[i:i + 2], 16) for i in (0, 2, 4))
        for rect in group.findall(f"{SVG_NS}rect"):
            parts.append((
                float(rect.get("x")), float(rect.get("y")),
                float(rect.get("width")), float(rect.get("height")), colour))
    return view[2], view[3], parts


def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    return ImageFont.truetype(f"/usr/share/fonts/truetype/dejavu/{name}", size)


def draw_panel(image, box, sheet_w, sheet_h, parts, title, subtitle):
    """Draws one layout into the given box, with a line at the used height."""
    draw = ImageDraw.Draw(image)
    left, top, width, height = box
    header = 78
    plot_top = top + header
    plot_height = height - header
    scale = min(width / sheet_w, plot_height / sheet_h)
    offset_x = left + (width - sheet_w * scale) / 2
    offset_y = plot_top + (plot_height - sheet_h * scale) / 2

    draw.text((left, top), title, font=font(30, True), fill=TEXT)
    draw.text((left, top + 40), subtitle, font=font(21), fill=DIM)

    draw.rectangle(
        [offset_x, offset_y, offset_x + sheet_w * scale,
         offset_y + sheet_h * scale], fill=SHEET, outline=SHEET_EDGE)

    # SVG y already grows downwards, so the rectangles map straight through.
    lowest = sheet_h
    for x, y, w, h, colour in parts:
        draw.rectangle(
            [offset_x + x * scale, offset_y + y * scale,
             offset_x + (x + w) * scale, offset_y + (y + h) * scale],
            fill=colour)
        lowest = min(lowest, y)

    used = sheet_h - lowest
    line_y = offset_y + lowest * scale
    draw.line([offset_x, line_y, offset_x + sheet_w * scale, line_y],
              fill=MARK, width=3)
    draw.text((offset_x + 8, line_y - 30), f"{used:.0f} mm used",
              font=font(22, True), fill=MARK)
    return used


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("output")
    parser.add_argument("--left", default="before")
    parser.add_argument("--right", default="after")
    parser.add_argument("--caption", default="")
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=900)
    args = parser.parse_args()

    image = Image.new("RGB", (args.width, args.height), BACKGROUND)
    margin = 48
    panel_width = (args.width - 3 * margin) // 2
    panel_height = args.height - 2 * margin - (60 if args.caption else 0)

    sheet_w, sheet_h, parts = load(args.before)
    left_used = draw_panel(image, (margin, margin, panel_width, panel_height),
                           sheet_w, sheet_h, parts, args.left, "")
    sheet_w, sheet_h, parts = load(args.after)
    right_used = draw_panel(
        image, (2 * margin + panel_width, margin, panel_width, panel_height),
        sheet_w, sheet_h, parts, args.right, "")

    if args.caption:
        draw = ImageDraw.Draw(image)
        gained = left_used - right_used
        text = args.caption.replace("{gained}", f"{gained:.0f}")
        draw.text((margin, args.height - margin - 34), text,
                  font=font(26, True), fill=MARK)

    image.save(args.output, optimize=True)
    print(f"{args.output}: {left_used:.0f} mm -> {right_used:.0f} mm")


if __name__ == "__main__":
    main()
