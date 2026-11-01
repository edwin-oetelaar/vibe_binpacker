#!/usr/bin/env python3
"""Measure memory and CPU time of plate sizes, and the GUI reload peak.

Runs the same deterministic workload on 800 x 600 and 1250 x 2500 mm, at 1.0
and 0.5 mm cells, and reports per configuration:

  - the sheet buffers the loader allocates (the bp_sheet budget figure),
  - the peak resident memory of bp_pack, for one construction pass and for a
    fixed number of search rounds, which also touches the penetration field,
  - the median CPU time of each.

Resident memory counts only pages that were written, so it is the real cost
of a run rather than the allocation; the budget figure is the allocation.

With --gui it also measures the reload peak: bp_gui first packs one size and
then applies the other, so both jobs exist at once, against a run that starts
on the target size directly. That needs a display.

Only the Python stdlib is used. Output is a Markdown table on stdout.

    pixi run make all gui
    python3 tools/measure_sheet_sizes.py --gui
"""
import argparse
import os
from pathlib import Path
import platform
import statistics
import subprocess

SIZES_MM = ((800, 600), (1250, 2500))
CELLS_MM = (1.0, 0.5)
JOB = "jobs/demo.job"
SEARCH_ROUNDS = 40
MIB = 1024.0 * 1024.0


def sheet_buffer_bytes(width_cells, height_cells):
    """The loader's per-sheet allocation, as documented in bp_sheet.h."""
    stride = (width_cells + 63) // 64
    return (8 * stride * height_cells + 4 * width_cells * height_cells
            + 8 * (width_cells + 1) * height_cells + 4 * 8 * stride)


def measure(command):
    """Run one process and return (CPU seconds, peak resident MiB) for it alone."""
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
    _, status, usage = os.wait4(process.pid, 0)
    stderr = process.stderr.read().decode()
    process.stderr.close()
    # bp_pack exits 2 for a valid partial result: the small plate cannot
    # take every part, which is part of the workload, not a failure.
    if os.waitstatus_to_exitcode(status) not in (0, 2):
        raise RuntimeError(f"{' '.join(command)} failed: {stderr}")
    # Linux reports ru_maxrss in KiB.
    return usage.ru_utime + usage.ru_stime, usage.ru_maxrss / 1024.0


def repeated(command, repeats):
    """Return the median CPU time and the largest peak over several runs."""
    runs = [measure(command) for _ in range(repeats)]
    return (statistics.median(seconds for seconds, _ in runs),
            max(peak for _, peak in runs))


def pack_table(binary, repeats):
    """Measure bp_pack on every size and cell, construction and search."""
    print(f"bp_pack, `{JOB}`, median of {repeats} runs, peak RSS is the "
          "largest of them\n")
    print("| plate mm | cell mm | raster cells | sheet buffers MiB "
          "| construction CPU s | construction peak MiB "
          f"| {SEARCH_ROUNDS} rounds CPU s | {SEARCH_ROUNDS} rounds peak MiB |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for width_mm, height_mm in SIZES_MM:
        for cell_mm in CELLS_MM:
            width_cells = int(width_mm / cell_mm)
            height_cells = int(height_mm / cell_mm)
            base = [str(binary), "--sheet-size", str(width_mm), str(height_mm),
                    "--cell", str(cell_mm)]
            built = repeated(base + [JOB], repeats)
            searched = repeated(base + ["--search", str(SEARCH_ROUNDS), JOB],
                                repeats)
            print(f"| {width_mm} x {height_mm} | {cell_mm} "
                  f"| {width_cells} x {height_cells} "
                  f"| {sheet_buffer_bytes(width_cells, height_cells) / MIB:.2f} "
                  f"| {built[0]:.3f} | {built[1]:.1f} "
                  f"| {searched[0]:.2f} | {searched[1]:.1f} |")


def gui_table(binary, repeats):
    """Compare a GUI run started on a size with one that applies it later."""
    print(f"\nbp_gui, `{JOB}`, {SEARCH_ROUNDS} search rounds per pack, "
          f"largest peak of {repeats} runs\n")
    print("| cell mm | start plate | applied plate | peak MiB "
          "| peak started on applied plate MiB | reload extra MiB |")
    print("| ---: | --- | --- | ---: | ---: | ---: |")
    for cell_mm in CELLS_MM:
        for start, applied in (SIZES_MM, SIZES_MM[::-1]):
            common = [str(binary), JOB, "--cell", str(cell_mm),
                      "--search", str(SEARCH_ROUNDS), "--frames", "4"]
            direct = repeated(common + ["--sheet-size", *map(str, applied)],
                              repeats)
            reload = repeated(common + ["--sheet-size", *map(str, start),
                                        "--apply-sheet-size",
                                        *map(str, applied)], repeats)
            print(f"| {cell_mm} | {start[0]} x {start[1]} "
                  f"| {applied[0]} x {applied[1]} | {reload[1]:.1f} "
                  f"| {direct[1]:.1f} | {reload[1] - direct[1]:.1f} |")


def main():
    """Print the measurement tables for the README and the plan."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pack", type=Path, default=Path("build/bp_pack"))
    parser.add_argument("--gui-binary", type=Path, default=Path("build/bp_gui"))
    parser.add_argument("--gui", action="store_true",
                        help="also measure the GUI reload peak (needs a display)")
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not args.pack.is_file():
        raise SystemExit("build bp_pack first: pixi run make all")
    if args.gui and not args.gui_binary.is_file():
        raise SystemExit("build the GUI first: pixi run gui")
    print(f"Measured on {platform.machine()}, {os.cpu_count()} logical CPUs, "
          f"{platform.system()} {platform.release()}\n")
    pack_table(args.pack, args.repeats)
    if args.gui:
        gui_table(args.gui_binary, args.repeats)


if __name__ == "__main__":
    main()
