# Project update to share - the saw solver

Upload these two images in this order:

1. [A plate cut from the saw list](guillotine_plate.png)
2. [The viewer in panel saw mode](gui_guillotine.png)

The first is 1600 x 1182 pixels, the GUI capture 1400 x 900. Copy the text
between the lines below. The explanation with measurements is in the README
under [Cutting on a panel saw](../README.md#cutting-on-a-panel-saw).

---

Robots need crates.

At Fontys we build robots, and to move them we needed crates. The students
designed them and made a cut list for the panels. But they had not yet heard
of the guillotine method, and so the Hornbach store where we had the panels
cut had quite a challenge getting all the pieces out.

A panel saw cuts straight through, edge to edge. A cut list that ignores that
leaves the puzzle to whoever stands at the saw.

We could have avoided that with our own open-source solver. So it now has two
sides, for two kinds of machine:

- free nesting for laser, plasma, waterjet and router;
- guillotine cutting for panel and circular saws: rectangles, every cut edge
  to edge, and a saw list in the order the operator works.

Every saw list is then re-sawn, cut by cut, by a separate program that does
not trust the solver. And it is measurable: on 500 standard instances the
search uses 1.7 to 2.6 % more plates than PackingSolver, the current state of
the art, in about a nine-hundredth of the computing time. That comparison
belongs in the story; without a measurement it is an opinion.

That closes the circle: we ran into a problem, and we solved it.

It is also exactly the kind of problem our engineers at Fontys can help with.
They do not have to invent the algorithms or prove them; the literature has
done that. Their work is to find out what exists, choose, build it into
something that runs, and make measurable what works and what does not. This
part, too, was largely written by Claude; the choices and the measurements
were the work. That is a different kind of creativity.

Got a problem like that on your shop floor? It makes excellent student
material: real constraints, real material, and no answer at the back of the
book.

https://github.com/edwin-oetelaar/vibe_binpacker

#cutlist #optimisation #manufacturing #education #opensource

---

## Alt text for the images

1. A 2800 x 2070 mm plate of kitchen cabinet parts, cut in strips. The cuts
   are coloured by saw stage: red for the full-width strips, blue for the
   cuts within them, green for the third stage. Waste is hatched.
2. Screenshot of the viewer in panel saw mode. Plate 1 of 3 is at cut 14 of
   30; the current cut is outlined in yellow. On the left: the saw settings,
   the plate list with parts, utilisation and cuts per plate, and the step
   slider through the saw list.
