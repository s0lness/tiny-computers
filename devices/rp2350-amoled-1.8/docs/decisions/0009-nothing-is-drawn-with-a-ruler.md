# 0009: Nothing on this device is drawn with a ruler

Date: 2026-08-14
Status: accepted

## The rule

The owner, after a day of correcting one icon: **"j'aimerais que sur toutes nos
apps on ait pas de bords trop durs, tu vois, ou d'angles trop droits ou de
traits trop droits"**.

That applies to every app, not to the icon that provoked it. A hard edge, a
right angle, or a dead-straight line is a defect on this device in the same
way an off-by-one is a defect elsewhere.

## Why it is not decoration

This is a toy for a child who cannot read yet. Everything she learns about it,
she learns from how it looks and how it answers her hand. The product's whole
visual language is ink on paper: the sketchpad's strokes, the timer's wound
coil, the menu's three drawn icons. A geometric primitive dropped into that
reads as a different object from a different program, and she has no words to
explain why it feels wrong.

It also happens to be honest about the hardware. The panel is about 322 ppi
and every shape here is anti-aliased by coverage, so a curve costs nothing a
straight line does not. There is no performance argument for the ruler.

## How to obey it, concretely

The primitives in `shapes.h` divide cleanly, and the division is the rule:

- **Float primitives can carry it.** `shapes_fill_capsule_aa_land`,
  `shapes_fill_tapered_quad_aa_land`, `shapes_fill_disc_aa_land` and
  `shapes_fill_annulus_aa_land` take float coordinates and anti-alias their
  own edges. Anything drawn with these can bow, round off and soften.
- **`shapes_fill_between_curves_aa_land` cannot.** It takes integer column
  arrays, so an edge has no sub-pixel position to anti-alias against. A
  vertical edge is clean because it never changes column; **any** deviation
  quantises into a staircase, at any amplitude.

That second point cost three rounds on the stopwatch's wedge. A generated
per-row jitter looked like a saw. A gentle half-sine bow looked like six
notches instead of twenty. Only moving the shape onto the float brush
(sweeping capsules from its apex, then bowing its two edges with quadratics)
produced what the owner had drawn on the render with a red pen.

**So: if a shape needs to be anything other than straight, it must be painted
with the float brush, not filled by rows.** Reaching for a smaller amplitude
is the wrong move and has already been tried twice.

## What this does not mean

Not wobble for its own sake. A generated random jitter is not handwriting, and
at icon scale it reads as a tear rather than as a hand. What a hand actually
does over a short line is bow it slightly and round its corners. Prefer one
deliberate curve over many small random ones.

Anti-aliasing greys at an edge are not a violation of "black and white": the
design is one ink on one paper, and the greys are how a curve exists at all on
a pixel grid.

## Consequences

- New shapes start from the float primitives unless there is a reason not to.
- A right angle anywhere in an app is worth a second look, including in
  affordances that are not "art": a palette's squares, a menu's hit feedback,
  a dialog's edge.
- `shapes_fill_between_curves_aa_land` stays, because a genuinely straight
  edge is still the cheapest way to draw a genuinely straight edge. Its
  header should say what it cannot do.
