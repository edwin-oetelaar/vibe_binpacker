#!/usr/bin/env python3
"""Check a guillotine cut list by sawing it: every cut must be possible.

Reads the CSV described in docs/cutlist_format.md. It does not use the
solver's tree. It lays each plate on the table, applies the cuts in the order
listed, and requires each one to cross exactly one current piece from edge to
edge. Then every item must be one of the final pieces and everything else must
be waste the saw can cut off. Only the Python stdlib is used.

With --svg it also reads <stem>-001.svg, <stem>-002.svg, ... for each plate
and requires the drawing to show exactly the items and cuts of the list.

    python3 tools/check_cutplan.py build/plan.csv
    python3 tools/check_cutplan.py --svg --quiet build/cases/*.csv
"""
import argparse
import csv
from collections import Counter
from decimal import Decimal, InvalidOperation
import io
import itertools
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

SETTINGS = ("plate_width_mm", "plate_height_mm", "trim_left_mm",
            "trim_right_mm", "trim_bottom_mm", "trim_top_mm", "kerf_mm",
            "min_waste_mm", "max_stages", "first_cut", "non_exact")


class CutlistError(Exception):
    """A broken rule, with the CSV line it was found on (0 for the file)."""

    def __init__(self, line, message):
        super().__init__(f"line {line}: {message}" if line else message)
        self.line = line


def units(text, line):
    """Parse millimetres with at most two decimals into 0.01 mm units."""
    try:
        value = Decimal(text)
    except InvalidOperation:
        raise CutlistError(line, f"not a number: {text!r}") from None
    if not value.is_finite() or value < 0 or value != value.quantize(Decimal("0.01")):
        raise CutlistError(line, f"not a length in 0.01 mm: {text!r}")
    return int(value * 100)


def count(text, line):
    """Parse a non-negative whole number."""
    if not text.isdigit():
        raise CutlistError(line, f"not a whole number: {text!r}")
    return int(text)


def flag(text, line):
    """Parse a 0 or 1 field."""
    if text not in ("0", "1"):
        raise CutlistError(line, f"expected 0 or 1: {text!r}")
    return text == "1"


def axis(text, line):
    """Parse a cut axis."""
    if text not in ("x", "y"):
        raise CutlistError(line, f"axis must be x or y: {text!r}")
    return text


def fields(row, line, expected):
    """Require a record to have exactly its number of fields."""
    if len(row) != expected:
        raise CutlistError(line, f"{row[0]} needs {expected - 1} fields, "
                                 f"has {len(row) - 1}")


class Piece:
    """One rectangle on the table, and how it came to be there."""

    # Serial numbers rather than id(): a cut-up piece is discarded, and
    # Python may hand its id to a later piece.
    serials = itertools.count()

    def __init__(self, x, y, width, height, stage, made_by=None, sizing=None):
        self.serial = next(Piece.serials)
        self.x, self.y, self.width, self.height = x, y, width, height
        self.stage = stage
        # The axis of the cut that produced it; None for an uncut plate.
        self.made_by = made_by
        # The piece a sizing cut started from, to allow one item per sizing.
        self.sizing = sizing
        self.usable = False

    def rect(self):
        """The piece as a comparable tuple."""
        return (self.x, self.y, self.width, self.height)


def stage_axis(first_axis, stage):
    """Odd stages follow the first axis, even stages the other one."""
    other = "y" if first_axis == "x" else "x"
    return first_axis if stage % 2 == 1 else other


class Saw:
    """Applies cuts to the pieces of each plate and enforces every rule."""

    def __init__(self, settings, parts):
        self.s = settings
        self.parts = parts
        self.plates = {}
        self.pieces = {}
        self.started = set()
        width, height = settings["plate_width_mm"], settings["plate_height_mm"]
        self.usable = (settings["trim_left_mm"], settings["trim_bottom_mm"],
                       width - settings["trim_left_mm"] - settings["trim_right_mm"],
                       height - settings["trim_bottom_mm"] - settings["trim_top_mm"])
        if self.usable[2] <= 0 or self.usable[3] <= 0:
            raise CutlistError(0, "edge trims leave no usable area")

    def add_plate(self, plate, first_axis, line):
        """Put a fresh plate on the table."""
        if plate in self.plates:
            raise CutlistError(line, f"plate {plate} listed twice")
        if self.s["first_cut"] != "any" and first_axis != self.s["first_cut"]:
            raise CutlistError(line, f"plate {plate} starts with {first_axis} "
                                     f"cuts, the saw only {self.s['first_cut']}")
        self.plates[plate] = first_axis
        self.pieces[plate] = [Piece(0, 0, self.s["plate_width_mm"],
                                    self.s["plate_height_mm"], 0)]

    def start_cutting(self, plate, line):
        """After the edge trims, find the usable area; the rest is scrap."""
        if plate in self.started:
            return
        self.started.add(plate)
        usable = [p for p in self.pieces[plate] if p.rect() == self.usable]
        if len(usable) != 1:
            raise CutlistError(line, f"edge trims of plate {plate} do not "
                                     "leave exactly the usable area")
        usable[0].usable = True

    def find(self, plate, cut_axis, position, kerf, start, end, line):
        """Return the one piece this cut crosses edge to edge."""
        for piece in self.pieces[plate]:
            if cut_axis == "x":
                cross, along, along_size = (piece.x, piece.x + piece.width), piece.y, piece.height
            else:
                cross, along, along_size = (piece.y, piece.y + piece.height), piece.x, piece.width
            if along <= position < along + along_size and cross == (start, end):
                return piece, along, along + along_size
        raise CutlistError(line, "cut does not cross any piece from edge to edge")

    def cut(self, plate, stage, cut_axis, position, kerf, start, end, line):
        """Saw one cut, splitting a piece in two, or in one at an edge."""
        if plate not in self.plates:
            raise CutlistError(line, f"plate {plate} was not listed")
        first_axis = self.plates[plate]
        limit = self.s["max_stages"] + (1 if self.s["non_exact"] else 0)
        if stage == 0:
            if plate in self.started:
                raise CutlistError(line, "edge trim after the first stage cut")
        else:
            self.start_cutting(plate, line)
            if stage > limit:
                raise CutlistError(line, f"stage {stage} is deeper than the saw allows")
            if cut_axis != stage_axis(first_axis, stage):
                raise CutlistError(line, f"stage {stage} must run along "
                                         f"{stage_axis(first_axis, stage)}")
        piece, low, high = self.find(plate, cut_axis, position, kerf, start, end, line)
        blade = self.s["kerf_mm"]
        if stage == 0:
            if piece.stage != 0 or piece.usable:
                raise CutlistError(line, "edge trim on a piece that is not the plate")
        else:
            if piece.stage == 0 and not piece.usable:
                raise CutlistError(line, "cut into edge trim scrap")
            if stage < piece.stage:
                raise CutlistError(line, f"stage {stage} cut on a stage "
                                         f"{piece.stage} piece goes back")
            if position == low:
                raise CutlistError(line, "cut at the near edge cuts nothing off")
        # The blade is kerf wide; it may only overhang where it meets an edge.
        if position + kerf > high:
            raise CutlistError(line, "blade runs past the piece")
        meets_far = position + kerf == high
        meets_near = stage == 0 and position == low
        if kerf != blade and not (kerf < blade and (meets_far or meets_near)):
            raise CutlistError(line, f"blade removes {kerf / 100:.2f} mm, "
                                     f"expected {blade / 100:.2f} mm")
        self.pieces[plate].remove(piece)
        if stage == 0:
            new_stage, sizing = 0, None
        else:
            new_stage = stage
            sizing = piece.sizing
            if stage > self.s["max_stages"] and sizing is None:
                sizing = piece.serial
        for lo, hi in ((low, position), (position + kerf, high)):
            if hi <= lo:
                continue
            if cut_axis == "x":
                child = Piece(piece.x, lo, piece.width, hi - lo, new_stage, "x", sizing)
            else:
                child = Piece(lo, piece.y, hi - lo, piece.height, new_stage, "y", sizing)
            self.pieces[plate].append(child)

    def finish(self, items):
        """Match items to final pieces; everything else must be fair waste."""
        for plate in self.plates:
            self.start_cutting(plate, 0)
        cut_per_label = Counter()
        per_sizing = Counter()
        matched = set()
        for line, plate, label, rect, rotated in items:
            if plate not in self.plates:
                raise CutlistError(line, f"plate {plate} was not listed")
            if label not in self.parts:
                raise CutlistError(line, f"part {label!r} was not ordered")
            quantity, width, height, may_rotate = self.parts[label]
            if rotated and not may_rotate:
                raise CutlistError(line, f"part {label!r} may not be turned")
            size = (height, width) if rotated else (width, height)
            if rect[2:] != size:
                raise CutlistError(line, f"item is not the size of part {label!r}")
            hits = [p for p in self.pieces[plate]
                    if p.rect() == rect and (p.stage > 0 or p.usable)]
            if len(hits) != 1 or hits[0].serial in matched:
                raise CutlistError(line, "item is not a piece the cuts produce")
            matched.add(hits[0].serial)
            cut_per_label[label] += 1
            if cut_per_label[label] > quantity:
                raise CutlistError(line, f"part {label!r} cut more often than ordered")
            if hits[0].sizing is not None:
                per_sizing[hits[0].sizing] += 1
                if per_sizing[hits[0].sizing] > 1:
                    raise CutlistError(line, "sizing cut frees more than one item")
        waste_area = 0
        for plate, pieces in self.pieces.items():
            for piece in pieces:
                if piece.serial in matched or (piece.stage == 0 and not piece.usable):
                    continue
                across = piece.height if piece.made_by == "x" else piece.width
                if piece.made_by and piece.stage > 0 and across < self.s["min_waste_mm"]:
                    raise CutlistError(0, f"waste {across / 100:.2f} mm wide on "
                                          f"plate {plate} is narrower than the "
                                          "saw can cut off")
                waste_area += piece.width * piece.height
        return sum(cut_per_label.values()), waste_area


def check(text):
    """Check one cut list; return a summary dict, or raise CutlistError."""
    rows = list(csv.reader(io.StringIO(text)))
    if not rows or rows[0] != ["format", "vibe_binpacker-cutlist", "1"]:
        raise CutlistError(1, "not a vibe_binpacker cut list, version 1")
    settings, parts, plates, cuts, items = {}, {}, [], [], []
    for line, row in enumerate(rows[1:], start=2):
        if not row:
            continue
        kind = row[0]
        if kind == "setting":
            fields(row, line, 3)
            name, value = row[1], row[2]
            if name not in SETTINGS or name in settings:
                raise CutlistError(line, f"unknown or repeated setting {name!r}")
            if name == "first_cut":
                if value not in ("x", "y", "any"):
                    raise CutlistError(line, "first_cut must be x, y or any")
                settings[name] = value
            elif name == "max_stages":
                settings[name] = count(value, line)
                if not 1 <= settings[name] <= 8:
                    raise CutlistError(line, "max_stages must be 1 to 8")
            elif name == "non_exact":
                settings[name] = flag(value, line)
            else:
                settings[name] = units(value, line)
        elif kind == "part":
            fields(row, line, 6)
            if row[1] in parts:
                raise CutlistError(line, f"part {row[1]!r} listed twice")
            parts[row[1]] = (count(row[2], line), units(row[3], line),
                             units(row[4], line), flag(row[5], line))
        elif kind == "plate":
            fields(row, line, 3)
            plates.append((line, count(row[1], line), axis(row[2], line)))
        elif kind == "cut":
            fields(row, line, 8)
            cuts.append((line, count(row[1], line), count(row[2], line),
                         axis(row[3], line), units(row[4], line),
                         units(row[5], line), units(row[6], line),
                         units(row[7], line)))
        elif kind == "item":
            fields(row, line, 8)
            items.append((line, count(row[1], line), row[2],
                          tuple(units(value, line) for value in row[3:7]),
                          flag(row[7], line)))
        else:
            raise CutlistError(line, f"unknown record {kind!r}")
    missing = [name for name in SETTINGS if name not in settings]
    if missing:
        raise CutlistError(0, f"missing settings: {', '.join(missing)}")
    saw = Saw(settings, parts)
    for line, plate, first_axis in plates:
        saw.add_plate(plate, first_axis, line)
    for line, plate, stage, cut_axis, position, kerf, start, end in cuts:
        saw.cut(plate, stage, cut_axis, position, kerf, start, end, line)
    placed, waste_area = saw.finish(items)
    plate_area = settings["plate_width_mm"] * settings["plate_height_mm"]
    items_by_plate = {plate: sorted((label, rect, rotated) for _, p, label, rect, rotated
                                    in items if p == plate)
                      for _, plate, _ in plates}
    return {"plates": len(plates), "cuts": len(cuts), "placed": placed,
            "ordered": sum(part[0] for part in parts.values()),
            "waste_share": waste_area / (plate_area * len(plates)) if plates else 0.0,
            "plate_size": (settings["plate_width_mm"], settings["plate_height_mm"]),
            "items_by_plate": items_by_plate,
            "cuts_by_plate": Counter(cut[1] for cut in cuts)}


def check_svg(path, plate, summary):
    """Require one plate's drawing to show exactly the list's items and cuts."""
    width, height = summary["plate_size"]
    root = ET.parse(path).getroot()
    svg = "{http://www.w3.org/2000/svg}"
    name = Path(path).name
    if root.get("viewBox") != f"0 0 {width / 100:.2f} {height / 100:.2f}":
        raise CutlistError(0, f"{name}: viewBox is not the plate")
    if root.get("width") != f"{width / 100:.2f}mm" or root.get("data-plate") != str(plate):
        raise CutlistError(0, f"{name}: size or plate number differs")
    drawn, cuts = [], 0
    for rect in root.iter(svg + "rect"):
        if rect.get("class") == "cut":
            cuts += 1
        if rect.get("class") != "item":
            continue
        x, y, w, h = (units(rect.get(key), 0) for key in ("x", "y", "width", "height"))
        # The drawing has y down; the list has y up.
        drawn.append((rect.get("data-label"), (x, height - y - h, w, h),
                      rect.get("data-rotated") == "1"))
    if sorted(drawn) != summary["items_by_plate"][plate]:
        raise CutlistError(0, f"{name}: drawn items differ from the cut list")
    if cuts != summary["cuts_by_plate"][plate]:
        raise CutlistError(0, f"{name}: {cuts} cuts drawn, "
                              f"{summary['cuts_by_plate'][plate]} listed")


def main():
    """Check cut list files and report the first broken rule or a summary."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cutlists", nargs="+")
    parser.add_argument("--svg", action="store_true",
                        help="also check <stem>-NNN.svg for every plate")
    parser.add_argument("--quiet", action="store_true",
                        help="print one line for all files instead of a summary each")
    args = parser.parse_args()
    for path in args.cutlists:
        with open(path, encoding="ascii") as source:
            text = source.read()
        try:
            summary = check(text)
            if args.svg:
                stem = Path(path).with_suffix("")
                for plate in range(summary["plates"]):
                    check_svg(f"{stem}-{plate + 1:03d}.svg", plate, summary)
        except (CutlistError, OSError, ET.ParseError) as error:
            print(f"{path}: {error}", file=sys.stderr)
            return 1
        if args.quiet:
            continue
        print(f"{path}")
        print(f"plates       {summary['plates']}")
        print(f"cuts         {summary['cuts']}")
        print(f"items        {summary['placed']} of {summary['ordered']} ordered")
        print(f"waste        {summary['waste_share'] * 100:.2f} % of the plate area, "
              "edge trims not counted")
        print("ok: every cut is a guillotine cut, every item is its part")
    if args.quiet:
        print(f"ok: {len(args.cutlists)} cut lists"
              f"{' and their drawings' if args.svg else ''} pass every rule")
    return 0


if __name__ == "__main__":
    sys.exit(main())
