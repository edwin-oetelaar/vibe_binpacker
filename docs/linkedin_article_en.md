# The five-euro circuit board did not happen by itself

*What panelisation, sheet metal and thirty years of experience have to do with
each other — and where AI, and a room full of students, fit in.*

**Cover image:** `docs/comparison_en.png`

---

Order five circuit boards today and you pay less than the setup charge alone
used to be. From the outside, that is simply how things go. Prices fall,
technology improves, next question.

Anyone who has stood on the floor knows it is not like that.

## What sits underneath

That price exists because someone solved the problem of getting small orders
from different customers onto one production panel. Not just onto it, but in a
way that holds the lead time, keeps scrap down, and still fits everything
downstream.

I worked on that for a client in the PCB industry, back when it was not yet
ordinary. Companies like EuroCircuits were early to it and showed what became
possible once you took the panel seriously. What stayed with me is not the
algorithm. It is how much sat around it: which orders may be combined at all,
what a test coupon costs you in space, how to handle mixed copper weights and
delivery dates, what happens when an order drops out after the panel is
already scheduled.

An optimisation that ignores all that can be provably the best one and still
cost you money.

## Same problem, different material

Over this summer holiday I built that problem again, in the open this time: a
2D nesting solver that arranges parts on a sheet so that as little as possible is left
over. Arbitrary shapes, holes, quarter turns, and a guaranteed gap between
parts. Sheet metal rather than circuit boards, but it is the same question:
how do you get the most value out of one piece of raw material.

The image above says it in one glance. On the left, one pass of a classic
construction heuristic. On the right, the same after ten seconds of search.
Same parts, same plate, 99 mm more material left as one straight strip instead
of scattered offcuts. That 99 mm is exactly the difference between a remnant
that goes back into stock and a remnant that goes in the skip.

## The four measurements that overturned everything

This is the part I care about, and it is also the part you never see if you
only look at the finished result.

**The clever shortcut was 69 times slower.** When you search for a free spot
and hit an obstacle, you can skip ahead as far as all your information allows.
That sounds obviously better than stopping at the first obstruction. Measured:
the clever version reduced the positions to examine by 0.4 % and cost seventy
times as much work per position. The dumb version won by a factor of 69.

**The "real engine" lost.** The design named morphological erosion as the core
of the method. Measured, it turned out slower than a plain scan for finding
the first free spot — and eleven to thirty times faster for enumerating *every*
free spot. That is a different question. The component stayed, but somewhere
else in the whole than where it was meant to go.

**The clearance was only correct once something outside checked it.** The
solver measured the distance between parts from cell centre to cell centre.
Two cells sitting diagonally next to each other are √2 apart centre to centre —
and zero apart material to material. They touch at a corner. None of my own
tests caught it, because I had written them against the same wrong idea. A
separate checker that read the output back and measured it independently
caught it immediately.

**A greedy repair loop never moves.** One of the search layers lifts parts out
and puts them back. For five thousand rounds that produced exactly zero
improvements. It only came loose once the removal cleared one contiguous
region *and* the replacement was allowed to skip a candidate now and then.

Four out of six. The other two are in the repository, with the numbers.

## And that is where AI comes into the story

I typed almost none of the code for this project. Claude Opus 5 did, at a pace
I cannot come close to by hand. I set the direction, chose the approach, and
judged what came out.

So you could say: then anyone can do this.

Look at those four measurements again. In every one of those cases the obvious
approach was convincing, well argued, and wrong. The tool would have produced
the plausible version each time, and it would have compiled, run and looked
entirely fine. What made the difference was not the typing. It was the habit of
saying: show me a number. And it was knowing *which* number to ask for.

That is thirty years of experience, and this tool does not make it redundant.
If anything it makes it worth more, because the time between "I suspect
something" and "I know" drops from days to minutes. I learned more about
nesting in this project than in the years I did it by hand, simply because I
could try far more things.

## Four years, and more than one right answer

This week the building opens again and a new intake starts on their
assignments. Three days a week I teach at Fontys ICT, in software engineering
and information technology. Not on a campus, but in the Fontys ICT
InnovationLab
on Strijp-T in Eindhoven: some 4,000 m² where education, research and industry
sit on the same floor, and where students work on real questions alongside
researchers and outside partners. I am mostly in the Smart Systems Lab,
working on robots and on perception with cameras and AI.

The other two days I build, at the edge of what is possible, and what I enjoy
most is when that reaches the factory floor. I have kept that split for years,
because each half keeps the other honest. You cannot explain what is changing
if you have not had it in your hands.

We get four years with these students. Four years to land three things that
are considerably harder than any algorithm: that a problem usually has more
than one good answer, that you have to get things wrong in order to learn
anything, and that making progress is generally worth more than waiting for
the perfect answer.

This project is what came of my summer holiday, and it is that lesson with the
numbers attached. Six times the obvious
approach turned out to be wrong. All of it is written down, because the wrong
turns are precisely the part you learn from. Anyone who only ever sees
finished results learns that software comes out right first time. That is the
most expensive illusion you can hand someone.

We sit in Brainport, where the theory of computer science and mathematics
borders on a region that actually makes things. Practical engineers are what
connects those two worlds, and that is where the impact is. I do it alongside
a team of international colleagues who have all worked in industry themselves,
which is why they can tell students what really happens rather than what is
supposed to happen. It is enjoyable work, and it is serious work.

If you are in the region and would like to see what goes on in that lab: it is
an open environment, and you are welcome to drop by.

## Where an algorithm stops

A nesting algorithm optimises one thing: how the parts sit on one sheet. That
is rarely where most of the money is.

Where it actually is: which orders you produce together, which formats you buy,
whether a remnant comes back with a usable shape or disappears, grain
direction, cutting sequence, clamping zones, material batches and traceability.
And always: what your operators really do when the plan does not suit them.

And then it still has to land somewhere — in your ERP or MES, your CAM
postprocessor, your drawing conventions. That part is not glamorous and it is
most of the work.

That is my trade. I studied industrial engineering and operations research, and
I still enjoy these puzzles after all these years. But the return is not in the
puzzle. It is in understanding the process around it, and in measuring whether
it works.

### Two ways to go about it

If you have a well-defined challenge and want a concrete result quickly, I do
it myself. That is commercial work, in my own time, and it goes the way this
project went: measure, show what comes out, and only claim something once there
is a number under it.

The other route is a student research project, with me supervising the
content. That works differently, and it is only fair to say so up front:
students have to learn from it first and foremost. They need time to explore
the fundamentals and to get to know your domain, and that takes months where I
would take weeks. What you get in return is something hiring cannot give you:
you watch young engineers work on your real problem for half a year, which is
one of the better ways there is to scout new people.

Which route fits depends on what you need. If there is time pressure, or the
result is critical, the first one is the sensible choice. If you have room and
you are also looking for new talent, the second is worth considering.

The whole repository is open, under the AGPL, including the results that
disappointed. A repository that shows only what worked tells you very little
about how to get there.

**github.com/edwin-oetelaar/vibe_binpacker** — with a README in
[English](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.md) and
[Dutch](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.nl.md).

Do you cut parts from stock material, or combine orders on a panel, and
suspect there is something left on the table? I am happy to talk it through.

**Edwin van den Oetelaar** — lecturer at Fontys ICT, inventor and independent
software developer

Commercial work: edwin@oetelaar.com · +31 6 5332 9077 ·
[oetelaar.com](https://www.oetelaar.com/) ·
[LinkedIn](https://www.linkedin.com/in/oetelaar/)

Student projects: e.vandenoetelaar@fontys.nl · Fontys ICT InnovationLab,
building TQ, Achtseweg Zuid 151C, 5651 GW Eindhoven, Netherlands
