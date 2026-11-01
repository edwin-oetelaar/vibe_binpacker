# Cut list format

A guillotine cutting plan as a saw operator and a checker both read it: one
CSV file, one record per row, the record type in the first column. It opens
in a spreadsheet and needs no parser beyond CSV.

`tools/check_cutplan.py` verifies a file in this format without using the
solver's own tree: it lays each plate on the table, applies the cuts in the
order listed, and requires every cut to cross exactly one piece from edge to
edge. The C model it mirrors is `include/bp_cutplan.h`.

## Lengths and coordinates

Millimetres with at most two decimals, so every value is a whole number of
0.01 mm units, the unit `bp_cutplan` works in. The origin is the bottom-left
corner of the plate, x to the right, y up.

An `x` cut runs parallel to x and splits a piece at a y value; a `y` cut runs
parallel to y and splits at an x value.

## Records

The first row identifies the format and its version:

    format,vibe_binpacker-cutlist,1

Settings, each exactly once:

    setting,plate_width_mm,2800.00
    setting,plate_height_mm,2070.00
    setting,trim_left_mm,10.00
    setting,trim_right_mm,10.00
    setting,trim_bottom_mm,10.00
    setting,trim_top_mm,10.00
    setting,kerf_mm,4.40
    setting,min_waste_mm,20.00
    setting,max_stages,3
    setting,first_cut,x          # x, y or any
    setting,non_exact,0          # 1 allows one sizing cut stage

An edge trim is the total material removed at that edge, blade included.

Parts as ordered, one row per label:

    part,<label>,<quantity>,<width_mm>,<height_mm>,<may_rotate>

Plates, one row each, numbered from 0:

    plate,<plate>,<first_axis>   # x or y; must match first_cut unless any

Cuts, in saw order within each plate:

    cut,<plate>,<stage>,<axis>,<position_mm>,<kerf_mm>,<from_mm>,<to_mm>

- `stage` 0 is an edge trim; the cuts after them are stage 1 upwards.
- `position_mm` is where the blade starts across the axis; it removes
  `kerf_mm` from there.
- `from_mm` and `to_mm` are where the cut line starts and ends along the
  axis. They must be the edges of the piece being cut.
- `kerf_mm` is the blade width, or less where the blade meets the edge of the
  piece within one blade width: that rest becomes sawdust. Only an edge trim
  may overhang the near edge.

Items, one row per piece cut out:

    item,<plate>,<label>,<x_mm>,<y_mm>,<width_mm>,<height_mm>,<rotated>

`rotated` is 1 when the part is turned a quarter turn, so its width and
height are swapped.

## Rules the checker applies

1. Settings are complete and the trims leave a usable area.
2. Every plate's first axis is allowed by `first_cut`.
3. Edge trims come before the other cuts of their plate and leave exactly the
   usable area.
4. Every cut crosses exactly one current piece from edge to edge, with the
   blade inside it, and splits it in two, or in one where the blade meets an
   edge.
5. Stage 1 cuts run along the plate's first axis, and the axis alternates
   with each stage. A cut continues its piece's stage or goes deeper, never
   back. The deepest stage is `max_stages`, plus one sizing stage with
   `non_exact`, which may free at most one item from each piece it cuts.
6. Every item is exactly one of the final pieces, has its part's size in its
   orientation, is turned only if the part may be, and no label is cut more
   often than ordered.
7. Every other final piece is waste, and waste that a cut produced is at
   least `min_waste_mm` wide across the last cut that produced it.

## Drawings

`bp_cutexport_svg` draws one plate per file, named `<stem>-001.svg`,
`<stem>-002.svg`, ... next to `<stem>.csv`. Sizes are millimetres, the
viewBox is the plate, and y points down as SVG requires; the cut list's y
points up, so a drawn y is the plate height minus the list's y and height.

- `<svg ... data-plate="0">` names the plate.
- `<rect class="item" data-label="7" data-rotated="0" .../>` is an item.
- `<rect class="waste" .../>` is hatched waste.
- `<rect class="cut" data-stage="2" .../>` is the band one blade removes,
  coloured by stage; stage 0 is an edge trim.

`tools/check_cutplan.py --svg` requires each drawing to show exactly the
items of its plate in the list, and as many cuts.
