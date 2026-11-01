#!/usr/bin/env python3
"""Build verified multi-plate update illustrations from actual solver output.

Requires built bp_pack/bp_gui, a working display, Pillow and DejaVu Sans
(the same image/font dependencies as make_comparison.py). Intermediate files
and a measurement manifest live under build/multisheet-update/. --publish
copies only the five curated PNG documentation assets into docs/.

    pixi run build
    pixi run gui
    python3 tools/make_multisheet_update.py --publish
"""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys

# Reusing the image helper must not leave generated files under tools/.
sys.dont_write_bytecode = True

from PIL import Image, ImageDraw

from make_comparison import font, load

OUT = Path("build/multisheet-update")
WIDTH, HEIGHT = 1600, 1120
BG = (16, 22, 33)
CARD = (25, 34, 48)
SHEET = (41, 49, 61)
EDGE = (111, 130, 151)
TEXT = (242, 246, 250)
DIM = (176, 192, 211)
ACCENT = (94, 227, 193)
BOX = (255, 214, 92)


def require(condition, message):
    """Stop asset publication when the data no longer supports the copy."""
    if not condition:
        raise RuntimeError(message)


def pack(job, stem, sheets, rounds=0, expected=0):
    """Produce SVGs and measured CSV using a deterministic search budget."""
    command = ["./build/bp_pack", "--sheets", str(sheets), "--search", str(rounds),
               "--csv", "--svg", str(OUT / f"{stem}.svg"), job]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    require(result.returncode == expected, f"{command}: {result.stderr}")
    (OUT / f"{stem}.csv").write_text(result.stdout, encoding="ascii")
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    for row in rows:
        path = OUT / f"{stem}-{int(row['sheet']):03}.svg"
        subprocess.run([sys.executable, "tools/check_layout.py", str(path),
                        "--clearance", "1"], check=True, timeout=60)
    return rows


def bounds(rects):
    """Measure tight solid bounds directly from the independently read SVG."""
    require(bool(rects), "illustrated plate is empty")
    return (min(r[0] for r in rects), min(r[1] for r in rects),
            max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects))


def panel(image, x, title, subtitle, path, footer):
    """Draw an actual layout at a common plate scale, with its true solid box."""
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((x, 200, x + 720, 936), radius=20, fill=CARD)
    draw.text((x + 24, 221), title, font=font(32, True), fill=TEXT)
    draw.text((x + 24, 268), subtitle, font=font(24), fill=DIM)
    sheet_w, sheet_h, rects = load(path)
    scale = min(672 / sheet_w, 570 / sheet_h)
    left = x + (720 - sheet_w * scale) / 2
    top = 312 + (570 - sheet_h * scale) / 2
    draw.rectangle((left, top, left + sheet_w * scale, top + sheet_h * scale),
                   fill=SHEET, outline=EDGE, width=2)
    for rx, ry, rw, rh, colour in rects:
        draw.rectangle((left + rx * scale, top + ry * scale,
                        left + (rx + rw) * scale, top + (ry + rh) * scale),
                       fill=colour, outline=BG, width=1)
    x0, y0, x1, y1 = bounds(rects)
    draw.rectangle((left + x0 * scale, top + y0 * scale,
                    left + x1 * scale, top + y1 * scale), outline=BOX, width=4)
    draw.text((x + 24, 896), footer, font=font(23), fill=BOX)


def header(image, title, subtitle):
    """Add a readable social-card heading without altering the measured layouts."""
    draw = ImageDraw.Draw(image)
    draw.text((56, 25), "VIBE_BINPACKER  /  PROJECT UPDATE", font=font(19, True), fill=ACCENT)
    draw.text((56, 63), title, font=font(49, True), fill=TEXT)
    draw.text((56, 132), subtitle, font=font(26), fill=DIM)


def number(value, language, decimals=0):
    """Keep Dutch and English figure labels consistent with their accompanying text."""
    text = f"{value:.{decimals}f}"
    return text.replace(".", ",") if language == "nl" else text


def overview(rows, language):
    """Show overflow as a complete two-plate job rather than an abandoned remainder."""
    image = Image.new("RGB", (WIDTH, HEIGHT), BG)
    nl = language == "nl"
    header(image, "Wat niet past, schuift door." if nl else "What does not fit moves to the next plate.",
           "76 onderdelen. 2 platen van 1200 x 1200 mm. 1 mm tussenruimte."
           if nl else "76 parts. Two 1200 x 1200 mm plates. 1 mm clearance.")
    for index, row in enumerate(rows):
        count = int(row["placed"])
        utilisation = number(float(row["sheet_utilisation"]) * 100, language, 2)
        width = number(float(row["bbox_width_mm"]), language)
        height = number(float(row["bbox_height_mm"]), language)
        title = f"Plaat {index + 1}" if nl else f"Plate {index + 1}"
        subtitle = (f"{count} onderdelen  |  {utilisation}% benut" if nl
                    else f"{count} parts  |  {utilisation}% used")
        footer = f"Bounding box: {width} x {height} mm"
        panel(image, 56 + index * 768, title, subtitle,
              OUT / f"dense-{index + 1:03}.svg", footer)
    draw = ImageDraw.Draw(image)
    draw.text((56, 963), "Eerste plaat: 43 geplaatst, 33 door naar plaat 2."
              if nl else "First plate: 43 placed, 33 continue on plate 2.",
              font=font(32, True), fill=ACCENT)
    draw.text((56, 1015), "Doel: alles plaatsen > minder platen > kleinere laatste box"
              if nl else "Goal: place all parts > fewer plates > smaller final box",
              font=font(25), fill=TEXT)
    draw.text((56, 1060), "jobs/dense.job | 64 zoekrondes | geel = omsluitende rechthoek"
              if nl else "jobs/dense.job | 64 search rounds | yellow = bounding box",
              font=font(21), fill=DIM)
    image.save(OUT / f"multisheet_overview_{language}.png", optimize=True)


def comparison(before, after, language):
    """Illustrate W*H improvement while explicitly keeping material and plate count equal."""
    image = Image.new("RGB", (WIDTH, HEIGHT), BG)
    nl = language == "nl"
    header(image, "De laatste plaat telt ook." if nl else "The last plate matters too.",
           "Compacter groeperen verandert de vorm van het restmateriaal."
           if nl else "A more compact layout changes the shape of the remaining material.")
    for index, (row, stem) in enumerate(((before, "before"), (after, "after"))):
        width = float(row["bbox_width_mm"])
        height = float(row["bbox_height_mm"])
        area = float(row["bbox_area_mm2"])
        title = ("Plaat 2 - zonder zoeken" if index == 0 else "Plaat 2 - na 128 rondes") if nl else (
            "Plate 2 - construction" if index == 0 else "Plate 2 - after 128 rounds")
        subtitle = f"{number(width, language)} x {number(height, language)} mm"
        panel(image, 56 + index * 768, title, subtitle, OUT / f"{stem}-002.svg",
              f"W x H = {number(area, language)} mm2")
    reduction = (1 - float(after["bbox_area_mm2"]) / float(before["bbox_area_mm2"])) * 100
    draw = ImageDraw.Draw(image)
    draw.text((56, 963), f"{number(reduction, language, 1)}% kleinere bounding box"
              if nl else f"{number(reduction, language, 1)}% smaller bounding box",
              font=font(37, True), fill=ACCENT)
    draw.text((56, 1017), "Dezelfde onderdelen. Dezelfde 2 platen. Dezelfde 1 mm tussenruimte."
              if nl else "Same parts. Same 2 plates. Same 1 mm clearance.",
              font=font(25), fill=TEXT)
    draw.text((56, 1062), "Klein controlevoorbeeld (104 x 104 mm). Materiaaloppervlak blijft gelijk."
              if nl else "Small controlled example (104 x 104 mm). Part material area is unchanged.",
              font=font(21), fill=DIM)
    image.save(OUT / f"multisheet_comparison_{language}.png", optimize=True)


def main():
    """Verify numerical claims, render publication assets and retain their provenance."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publish", action="store_true",
                        help="copy the five curated PNG assets into docs/")
    args = parser.parse_args()
    require(Path("build/bp_pack").is_file() and Path("build/bp_gui").is_file(),
            "run pixi run build and pixi run gui first")
    require(not args.publish or Path("docs").is_dir(), "docs/ is missing")
    OUT.mkdir(parents=True, exist_ok=True)
    single = pack("jobs/dense.job", "single", 1, expected=2)
    dense = pack("jobs/dense.job", "dense", 4, rounds=64)
    before = pack("jobs/multisheet.job", "before", 2)
    after = pack("jobs/multisheet.job", "after", 2, rounds=128)
    require(len(single) == 1 and int(single[0]["placed"]) == 43, "single-plate claim changed")
    require(len(dense) == 2 and [int(r["placed"]) for r in dense] == [43, 33],
            "76-part / two-plate claim changed")
    require(len(before) == len(after) == 2, "controlled example changed plate count")
    require(float(before[1]["bbox_area_mm2"]) == 6060
            and float(after[1]["bbox_area_mm2"]) == 5460, "box-area claim changed")
    require([int(r["placed"]) for r in before] == [int(r["placed"]) for r in after],
            "comparison changed the part count")
    for stem in ("before", "after"):
        _, _, rects = load(OUT / f"{stem}-002.svg")
        require(sum(w * h for _, _, w, h, _ in rects) == 4200, "part material changed")
    for language in ("nl", "en"):
        overview(dense, language)
        comparison(before[1], after[1], language)
    subprocess.run(["./build/bp_gui", "jobs/dense.job", "--sheets", "4", "--search", "64",
                    "--sheet", "2", "--svg", str(OUT / "gui-dense.svg"),
                    "--shot", str(OUT / "gui.ppm"), "--frames", "8"],
                   check=True, timeout=120)
    for index, row in enumerate(dense, 1):
        _, _, gui_rects = load(OUT / f"gui-dense-{index:03}.svg")
        x0, y0, x1, y1 = bounds(gui_rects)
        require(x1 - x0 == float(row["bbox_width_mm"])
                and y1 - y0 == float(row["bbox_height_mm"]), "GUI and CLI bounds disagree")
    with Image.open(OUT / "gui.ppm") as capture:
        capture.save(OUT / "gui_multisheet.png", optimize=True)
    manifest = {
        "dense": dense, "single_plate": single, "before": before, "after": after,
        "search_rounds": {"dense": 64, "controlled_example": 128},
        "inputs_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in map(Path, ("jobs/dense.job", "jobs/multisheet.job"))},
        "comparison_part_material_mm2": 4200,
        "bounding_box_reduction_percent": (6060 - 5460) / 6060 * 100,
        "images": ["multisheet_overview_nl.png", "multisheet_overview_en.png",
                   "multisheet_comparison_nl.png", "multisheet_comparison_en.png",
                   "gui_multisheet.png"]
    }
    (OUT / "measurements.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")
    if args.publish:
        for name in manifest["images"]:
            shutil.copyfile(OUT / name, Path("docs") / name)
    print(f"Verified update images and measurements: {OUT}")
    if args.publish:
        print("Published five documentation images in docs/")


if __name__ == "__main__":
    main()
