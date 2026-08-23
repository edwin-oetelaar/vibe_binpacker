# LinkedIn post — English

**Image:** `docs/comparison_en.png`

---

You can have five circuit boards made today for less than the setup cost used
to be.

That did not happen by itself.

Underneath it sits a pile of optimisation: combining small orders from
different customers onto one production panel, driving down scrap, protecting
lead time. I worked on exactly that for a client in the PCB industry years
ago, when it was still new. Companies like EuroCircuits were early to it. From
the outside it now looks automatic. As an engineer on the floor you know
better: it is serious work, a lot of research, and it has to fit your process
down to the details.

Same problem, different material, built again — this time in the open. A 2D
nesting solver that arranges parts on a sheet so that as little as possible is
left over. The picture: the same 36 parts, the same plate, ten seconds of
search. 99 mm more of the sheet left as one usable strip instead of scattered
offcuts.

I typed almost none of the code. Claude Opus 5 did. I set the direction, chose
the approach, and judged what came out.

But here is what I take from it: six times, a measurement overturned the
obvious approach. The clever shortcut that turned out 69 times slower. The
"real engine" that lost to a plain scan. The clearance that looked right until
something outside the solver checked it. Every time, the tool would have
shipped the plausible version. Thirty years of experience is what insisted on
measuring.

That is the ratio: AI accelerates what you can already do. It does not replace
it.

This week the building opens again. Three days a week I teach at Fontys ICT,
in the ICT InnovationLab on Strijp-T in Eindhoven, and a new intake of students
starts on their assignments. We get four years to land three things that are
harder than any algorithm: that a problem usually has more than one good
answer, that you have to get things wrong to learn anything, and that making
progress usually beats waiting for the perfect answer.

This is what came of my summer holiday, and it is exactly that lesson. It is
all in there, including the results that disappointed.

If you have a problem like this lying around, incidentally, it makes excellent
student material: real constraints, real material, no answer in the back of the
book. I supervise those research projects on content. It goes slower than when
I do it myself, because students have to learn from it and that is rather the
point. What you get back is half a year of watching young engineers work inside
your problem.

→ github.com/edwin-oetelaar/vibe_binpacker

#optimization #operationsresearch #manufacturing #PCB #nesting #engineeringeducation
