# Project update to share - multiple plates

Upload these three images in this order:

1. [Overview: the complete job on two plates](multisheet_overview_en.png)
2. [Before/after: the final plate's bounding box](multisheet_comparison_en.png)
3. [The updated GUI](gui_multisheet.png)

The first two are 1600 x 1120 pixels; the actual GUI capture is 1400 x 900.
Copy the text between the rules below. See the
[illustrated update](update_multisheet_en.md) for details.

---

What happens to the parts that do not fit on the first plate?

The next step in my open-source nesting project: a job now continues across
multiple plates. The GUI shows the whole job too.

In the example: 76 parts. The first plate takes 43; the other 33 carry over
to plate 2. All placed, with 1 mm clearance.

The final plate deserves attention as well. What remains could become the
starting material for another job.

The solver therefore has three objectives, in a fixed order:

- Place as many requested pieces as possible.
- Use fewer plates.
- Minimise the enclosing rectangle on the final plate.

That last objective is width times height. In the small controlled example
in image 2, the rectangle changes from 101 x 60 to 60 x 91 mm:
6060 to 5460 mm2, 9.9% smaller. Same parts, same plate count. Part material
area stays the same; the shape of the remaining space changes. The narrower
layout leaves a wider free strip beside the work.

That is the interesting step for me: from a full first plate to a view of
the complete job and the shape of what remains.

The viewer now lets you select each plate, inspect its bounding box and
utilisation, and export every plate together as SVG. The images come from
actual solver output and a GUI capture; the layouts were independently
checked for overlap, plate bounds and clearance.

The code and examples are freely available. Feedback from people who work
with sheet material every day is welcome.

https://github.com/edwin-oetelaar/vibe_binpacker

#nesting #optimisation #manufacturing #opensource

---

## Image alt text

1. A 76-part nesting job on two 1200 x 1200 mm plates: 43 parts on the first
   and 33 on the second. Yellow outlines mark their bounding boxes.
2. The same two parts on the final plate before and after 128 search rounds.
   The enclosing rectangle changes from 101 x 60 to 60 x 91 mm, reducing its
   area by 9.9 percent. A wider strip appears beside the narrower layout.
3. Screenshot of the updated GUI with plate 2 selected and 33 parts inside
   a yellow bounding box. The left panel shows the plate table, utilisation,
   local replay slider and a total of 76 placed parts.
