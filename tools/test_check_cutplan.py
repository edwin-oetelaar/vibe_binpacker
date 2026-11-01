#!/usr/bin/env python3
"""Tests for check_cutplan.py: hand-made cut lists, each rule broken once.

The valid list is the same plan as tests/test_bp_cutplan.c, so the C check
and this independent one are exercised on the same example.

    python3 tools/test_check_cutplan.py
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cutplan import CutlistError, check  # noqa: E402

SETTINGS = {
    "plate_width_mm": "1000.00", "plate_height_mm": "500.00",
    "trim_left_mm": "0", "trim_right_mm": "0",
    "trim_bottom_mm": "0", "trim_top_mm": "0",
    "kerf_mm": "4.40", "min_waste_mm": "20.00", "max_stages": "3",
    "first_cut": "x", "non_exact": "0",
}

PARTS = """\
part,A,1,300.00,200.00,1
part,B,1,300.00,100.00,0
part,D,2,200.00,97.80,0
part,E,1,50.00,50.00,1
"""

PLATE = """\
plate,0,x
cut,0,1,x,200.00,4.40,0.00,1000.00
cut,0,1,x,304.40,4.40,0.00,1000.00
cut,0,2,y,300.00,4.40,0.00,200.00
cut,0,2,y,504.40,4.40,0.00,200.00
cut,0,3,x,97.80,4.40,304.40,504.40
cut,0,2,y,300.00,4.40,204.40,304.40
item,0,A,0.00,0.00,300.00,200.00,0
item,0,D,304.40,0.00,200.00,97.80,0
item,0,D,304.40,102.20,200.00,97.80,0
item,0,B,0.00,204.40,300.00,100.00,0
"""


def cutlist(settings=None, parts=PARTS, plate=PLATE):
    """Assemble a cut list from the fixture, with settings overridden."""
    merged = dict(SETTINGS, **(settings or {}))
    lines = ["format,vibe_binpacker-cutlist,1"]
    lines += [f"setting,{name},{value}" for name, value in merged.items()]
    return "\n".join(lines) + "\n" + parts + plate


def swap(text, old, new):
    """Replace exactly one occurrence, so a typo fails loudly."""
    if text.count(old) != 1:
        raise AssertionError(f"fixture line not unique: {old!r}")
    return text.replace(old, new)


class CheckCutplanTest(unittest.TestCase):
    """Each test breaks one rule of docs/cutlist_format.md."""

    def broken(self, text, fragment):
        """Require the checker to refuse with a message containing fragment."""
        with self.assertRaises(CutlistError) as caught:
            check(text)
        self.assertIn(fragment, str(caught.exception))

    def test_valid_plan(self):
        """The fixture is sawable, with one ordered part left unplaced."""
        summary = check(cutlist())
        self.assertEqual(summary["plates"], 1)
        self.assertEqual(summary["cuts"], 6)
        self.assertEqual((summary["placed"], summary["ordered"]), (4, 5))

    def test_any_first_cut(self):
        """A saw that may start either way accepts the plan's choice."""
        check(cutlist({"first_cut": "any"}))

    def test_first_axis_refused(self):
        """A saw that only starts along y refuses a plate cut along x first."""
        self.broken(cutlist({"first_cut": "y"}), "the saw only y")

    def test_not_edge_to_edge(self):
        """A cut that stops short of the piece's edge crosses no piece."""
        plate = swap(PLATE, "cut,0,2,y,300.00,4.40,0.00,200.00",
                     "cut,0,2,y,300.00,4.40,0.00,199.99")
        self.broken(cutlist(plate=plate), "edge to edge")

    def test_wrong_blade(self):
        """A cut narrower than the blade, away from any edge, is impossible."""
        plate = swap(PLATE, "cut,0,1,x,200.00,4.40,", "cut,0,1,x,200.00,4.30,")
        self.broken(cutlist(plate=plate), "blade removes 4.30 mm")

    def test_stage_axis(self):
        """Stage 2 must run across stage 1."""
        plate = swap(PLATE, "cut,0,2,y,300.00,4.40,0.00,200.00",
                     "cut,0,2,x,100.00,4.40,0.00,1000.00")
        self.broken(cutlist(plate=plate), "must run along y")

    def test_too_many_stages(self):
        """A two-stage saw cannot make the stack's cut."""
        self.broken(cutlist({"max_stages": "2"}), "deeper than the saw allows")

    def test_stage_goes_back(self):
        """A stage 1 cut cannot follow on a stage 2 piece."""
        plate = PLATE + "cut,0,1,x,50.00,4.40,0.00,300.00\n"
        self.broken(cutlist(plate=plate), "goes back")

    def test_sizing_cut(self):
        """With non-exact sawing the third stage may free one item, not two."""
        self.broken(cutlist({"max_stages": "2", "non_exact": "1"}),
                    "more than one item")
        plate = swap(PLATE, "item,0,D,304.40,102.20,200.00,97.80,0\n", "")
        check(cutlist({"max_stages": "2", "non_exact": "1"}, plate=plate))

    def test_dust_at_the_edge(self):
        """A last cut within a blade width of the edge turns the rest to dust."""
        plate = PLATE + "cut,0,2,y,995.60,4.40,204.40,304.40\n"
        check(cutlist(plate=plate))
        plate = PLATE + "cut,0,2,y,999.00,1.00,204.40,304.40\n"
        check(cutlist(plate=plate))
        plate = PLATE + "cut,0,2,y,999.00,0.90,204.40,304.40\n"
        self.broken(cutlist(plate=plate), "blade removes 0.90 mm")
        plate = PLATE + "cut,0,2,y,999.00,4.40,204.40,304.40\n"
        self.broken(cutlist(plate=plate), "runs past the piece")

    def test_cut_at_near_edge(self):
        """Above the trims, a cut at the near edge of a piece cuts nothing off."""
        plate = PLATE + "cut,0,2,y,304.40,4.40,204.40,304.40\n"
        self.broken(cutlist(plate=plate), "near edge")

    def test_minimum_waste(self):
        """The narrowest waste is 191.20 mm: exactly that is fine, more is not."""
        check(cutlist({"min_waste_mm": "191.20"}))
        self.broken(cutlist({"min_waste_mm": "191.21"}), "narrower than the saw")

    def test_item_size(self):
        """An item must have its part's size."""
        parts = swap(PARTS, "part,A,1,300.00,200.00,1", "part,A,1,299.99,200.00,1")
        self.broken(cutlist(parts=parts), "not the size of part 'A'")

    def test_item_rotation(self):
        """A part ordered 200 x 300 may be turned into the slot, unless grain forbids."""
        parts = swap(PARTS, "part,A,1,300.00,200.00,1", "part,A,1,200.00,300.00,1")
        plate = swap(PLATE, "item,0,A,0.00,0.00,300.00,200.00,0",
                     "item,0,A,0.00,0.00,300.00,200.00,1")
        check(cutlist(parts=parts, plate=plate))
        parts = swap(parts, "200.00,300.00,1", "200.00,300.00,0")
        self.broken(cutlist(parts=parts, plate=plate), "may not be turned")

    def test_item_not_a_piece(self):
        """An item must be exactly one final piece."""
        plate = swap(PLATE, "item,0,B,0.00,204.40,", "item,0,B,0.01,204.40,")
        self.broken(cutlist(plate=plate), "not a piece the cuts produce")

    def test_too_many_items(self):
        """A label cannot be cut more often than ordered."""
        parts = swap(PARTS, "part,D,2,", "part,D,1,")
        self.broken(cutlist(parts=parts), "more often than ordered")

    def test_edge_trims(self):
        """Trims come first and leave the usable area; narrower than the blade is dust."""
        settings = {"trim_left_mm": "10.00", "trim_right_mm": "10.00",
                    "trim_bottom_mm": "3.00", "trim_top_mm": "10.00"}
        trims = ("plate,0,x\n"
                 "cut,0,0,x,0.00,3.00,0.00,1000.00\n"
                 "cut,0,0,x,490.00,4.40,0.00,1000.00\n"
                 "cut,0,0,y,5.60,4.40,3.00,490.00\n"
                 "cut,0,0,y,990.00,4.40,3.00,490.00\n")
        check(cutlist(settings, plate=trims))
        missing = trims.replace("cut,0,0,y,990.00,4.40,3.00,490.00\n", "")
        self.broken(cutlist(settings, plate=missing), "usable area")
        late = trims + "cut,0,1,x,100.00,4.40,10.00,990.00\ncut,0,0,x,0.00,3.00,0.00,1000.00\n"
        self.broken(cutlist(settings, plate=late), "edge trim after")

    def test_parse_errors(self):
        """Malformed files are refused with a line number."""
        self.broken("format,other,1\n", "not a vibe_binpacker cut list")
        self.broken(cutlist() + "cut,0,1,x,1.001,4.40,0.00,1000.00\n",
                    "not a length in 0.01 mm")
        self.broken(cutlist() + "unknown,1\n", "unknown record")
        text = cutlist().replace("setting,kerf_mm,4.40\n", "")
        self.broken(text, "missing settings: kerf_mm")


if __name__ == "__main__":
    unittest.main()
