#!/usr/bin/env python3
"""Measure the guillotine packer: plates against the area bound, and the price
of sawability against free nesting.

Three measurements, all repeatable:

1. The ten standard classes of two-dimensional bin packing, classes I-VI of
   Berkey & Wang (1987) and VII-X of Martello & Vigo (1998), with 20 to 100
   items, ten instances each. The instances are generated here from the class
   definitions with a fixed seed: they are not the published instance files,
   so the numbers compare with the area lower bound, not with the
   literature. Measured with fixed orientation and with quarter turns, no
   kerf, no trims, as in the classical problem.
2. The price of sawability: the rectangle-only example jobs nested freely on
   several plates and cut on a three-stage saw with the job's clearance as
   blade width.
3. jobs/saw.job, the kitchen cabinet example, as a saw shop would run it.

Every cut list is checked with tools/check_cutplan.py before it is counted.
Only the Python stdlib is used; output is Markdown.

    pixi run build
    python3 tools/measure_guillotine.py
"""
import argparse
import math
from pathlib import Path
import resource
import random
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_cutplan import check  # noqa: E402

CLASSES = ("I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X")
SIZES = (20, 40, 60, 80, 100)
INSTANCES = 10


def berkey_wang(name, count, rng):
    """Classes I-VI: square bins, item sides uniform on a range."""
    bin_side, low, high = {"I": (10, 1, 10), "II": (30, 1, 10), "III": (40, 1, 35),
                           "IV": (100, 1, 35), "V": (100, 1, 100),
                           "VI": (300, 1, 100)}[name]
    return bin_side, [(rng.randint(low, high), rng.randint(low, high))
                      for _ in range(count)]


def martello_vigo(name, count, rng):
    """Classes VII-X: 100 x 100 bins, four item types, one of them at 70 %."""
    side = 100

    def item(kind):
        """Draw one item of the four Martello & Vigo types."""
        if kind == 1:
            return rng.randint(2 * side // 3, side), rng.randint(1, side // 2)
        if kind == 2:
            return rng.randint(1, side // 2), rng.randint(2 * side // 3, side)
        if kind == 3:
            return rng.randint(side // 2, side), rng.randint(side // 2, side)
        return rng.randint(1, side // 2), rng.randint(1, side // 2)

    main = {"VII": 1, "VIII": 2, "IX": 3, "X": 4}[name]
    items = []
    for _ in range(count):
        roll = rng.random()
        others = [kind for kind in (1, 2, 3, 4) if kind != main]
        kind = main if roll < 0.7 else others[min(2, int((roll - 0.7) / 0.1))]
        items.append(item(kind))
    return side, items


def run(command):
    """Run bp_pack; return (exit status, stdout, its reported CPU seconds)."""
    process = subprocess.run(command, capture_output=True, text=True)
    if process.returncode not in (0, 2):
        raise RuntimeError(f"{' '.join(command)} failed: {process.stderr}")
    found = re.search(r"time\s+([0-9.]+) CPU s", process.stdout)
    return process.returncode, process.stdout, float(found.group(1)) if found else 0.0


def plates_used(out):
    """Read the plate count from bp_pack's guillotine summary."""
    return int(re.search(r"plates\s+(\d+) used", out).group(1))


def saw(binary, job, cutlist):
    """Pack a job on the saw, check its cut list, return (plates, summary)."""
    status, out, seconds = run([str(binary), "--guillotine", "--cutlist",
                                str(cutlist), str(job)])
    summary = check(cutlist.read_text(encoding="ascii"))
    if summary["plates"] != plates_used(out):
        raise RuntimeError(f"{job}: summary and cut list disagree")
    return status, summary, seconds


def benchmark(binary, work):
    """Measurement 1: the ten classes, oriented and with quarter turns."""
    print("## Standard classes against the area bound\n")
    print(f"{INSTANCES} instances per class and size, sizes "
          f"{', '.join(map(str, SIZES))}; plates summed over all 50 instances "
          "of a class. Ratio = plates / area lower bound; 1.000 would be "
          "provably optimal.\n")
    print("| class | bin | bound | oriented | ratio | turns | ratio |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    totals = [0, 0, 0]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    for name in CLASSES:
        rng = random.Random(f"bp-guillotine-{name}")
        bound = 0
        plates = [0, 0]
        bin_side = 0
        for count in SIZES:
            for instance in range(INSTANCES):
                if CLASSES.index(name) < 6:
                    bin_side, items = berkey_wang(name, count, rng)
                else:
                    bin_side, items = martello_vigo(name, count, rng)
                area = sum(w * h for w, h in items)
                bound += math.ceil(area / (bin_side * bin_side))
                for turn in (0, 1):
                    job = work / f"{name}-{count}-{instance}-{turn}.job"
                    lines = [f"sheet {bin_side} {bin_side}", "kerf 0",
                             "stages 8", "first-cut any"]
                    lines += [f"rect {i + 1} 1 {w} {h} {turn}"
                              for i, (w, h) in enumerate(items)]
                    job.write_text("\n".join(lines) + "\n", encoding="ascii")
                    status, summary, _ = saw(binary, job, job.with_suffix(".csv"))
                    if status != 0:
                        raise RuntimeError(f"{job}: not every item placed")
                    plates[turn] += summary["plates"]
        totals[0] += bound
        totals[1] += plates[0]
        totals[2] += plates[1]
        print(f"| {name} | {bin_side} | {bound} | {plates[0]} | "
              f"{plates[0] / bound:.3f} | {plates[1]} | {plates[1] / bound:.3f} |")
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    spent = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    print(f"| all | | {totals[0]} | {totals[1]} | {totals[1] / totals[0]:.3f} | "
          f"{totals[2]} | {totals[2] / totals[0]:.3f} |\n")
    print(f"All {2 * len(CLASSES) * len(SIZES) * INSTANCES} runs together took "
          f"{spent:.2f} CPU s, process start-up included.\n")


def free_plates(binary, job):
    """Plates and placed parts when nesting freely on up to 20 plates."""
    process = subprocess.run([str(binary), "--sheets", "20", "--search", "128",
                              "--csv", str(job)], capture_output=True, text=True)
    if process.returncode not in (0, 2):
        raise RuntimeError(f"free nesting of {job} failed: {process.stderr}")
    rows = [line for line in process.stdout.splitlines()[1:] if line.strip()]
    return len(rows), sum(int(row.split(",")[1]) for row in rows)


def price(binary, work):
    """Measurement 2: free nesting against a three-stage saw."""
    print("## The price of sawability\n")
    print("Rectangle-only example jobs. Free: `--sheets 20 --search 128` on the "
          "1 mm raster. Saw: three stages plus sizing cuts, first cut either "
          "way, the job's clearance as blade width, no trims.\n")
    print("| job | parts | free plates | saw plates | parts' share of the saw plates |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for name in ("demo", "dense", "multisheet"):
        source = Path("jobs") / f"{name}.job"
        text = source.read_text(encoding="ascii")
        clearance = re.search(r"^clearance\s+([0-9.]+)", text, re.M).group(1)
        job = work / f"price-{name}.job"
        job.write_text(text + f"\nkerf {clearance}\nstages 3\nsizing-cut 1\n",
                       encoding="ascii")
        free, free_parts = free_plates(binary, source)
        status, summary, _ = saw(binary, job, work / f"price-{name}.csv")
        placed = summary["placed"]
        note = "" if placed == summary["ordered"] else f" ({placed} placed)"
        free_note = "" if free_parts == summary["ordered"] else f" ({free_parts} placed)"
        print(f"| {name} | {summary['ordered']} | {free}{free_note} | "
              f"{summary['plates']}{note} | {summary['item_share'] * 100:.1f} % |")
    print()


def kitchen(binary, work):
    """Measurement 3: the kitchen cabinet example."""
    _, summary, _ = saw(binary, Path("jobs/saw.job"), work / "saw.csv")
    print("## jobs/saw.job\n")
    print(f"{summary['placed']} of {summary['ordered']} parts on "
          f"{summary['plates']} plates of 2800 x 2070 mm, {summary['cuts']} cuts; "
          f"the parts cover {summary['item_share'] * 100:.1f} % of the plate area, "
          f"trims and sawdust excluded.\n")


def main():
    """Run all three measurements into build/guillotine-bench."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=Path("build/bp_pack"))
    parser.add_argument("--work", type=Path, default=Path("build/guillotine-bench"))
    args = parser.parse_args()
    if not args.binary.is_file():
        raise SystemExit("build bp_pack first: pixi run build")
    args.work.mkdir(parents=True, exist_ok=True)
    benchmark(args.binary, args.work)
    price(args.binary, args.work)
    kitchen(args.binary, args.work)


if __name__ == "__main__":
    main()
