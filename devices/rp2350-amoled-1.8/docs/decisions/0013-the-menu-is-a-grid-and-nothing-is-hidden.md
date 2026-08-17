# 0013: The menu is a grid, and nothing is hidden

Date: 2026-08-15
Status: accepted, built

## The problem, and the number that settles it

The menu was a row of full-height columns, one per app, `LAND_W / g_appCount`
wide. The panel is 448px across in landscape and a child's fingertip contact
is about 75px (AGENTS.md), so:

| apps | column | against a 75px fingertip |
|---|---|---|
| 3 | 149px | two fingers, roomy |
| 4 | 112px | one and a half, where it shipped |
| **6** | **74px** | **under one finger** |
| 12 | 37px | half a finger |

The owner has asked for tic-tac-toe, breakout, a spirit level, a clock, the
dinosaur, bowling and a tilt-a-ball. That is twelve apps, and two of them were
being written the night this was decided. **The sixth app crosses the line, not
some future one** (decision 0010, "The menu breaks at six, not twelve").

Decision 0010 also fixed the ordering, and it is the reason this could not
wait: each icon is a hand-converted Lucide silhouette at a fixed 96px, so the
geometry has to be decided before eight icons are drawn for it.

## The decision

**A grid of cells that fills the glass. Every app visible at once. No paging,
no scrolling, no swipe, no wheel, no momentum.**

- A cell is **112px tall, always**, and at most four across, so a cell is
  never narrower than `448 / 4 = 112`.
- Rows are `ceil(n / 4)`, packed to the top of the glass, balanced (five apps
  are 3 then 2, never 4 then 1), each row's cells contiguous edge to edge with
  the last one absorbing the remainder.
- Everything below the grid is the **cancel band**: a release there does
  nothing.
- The gesture does not change. Press, drag with the candidate lit under the
  thumb, release to launch. Same idiom as the sketchpad's palette and Connect
  Four.

What that gives:

| apps | grid | target | fingertips | cancel band |
|---|---|---|---|---|
| 4 | 4x1 | 112 x 112 | 1.5 x 1.5 | 256px |
| **6** | 3x2 | **149 x 112** | 2.0 x 1.5 | 144px |
| **9** | 3x3 | **149 x 112** | 2.0 x 1.5 | 32px |
| **12** | 4x3 | **112 x 112** | 1.5 x 1.5 | 32px |

## Why the arithmetic, not a gesture, is the answer

The panel is about 5.9 fingertips across and 4.9 down: it physically holds
roughly twenty finger-sized targets. The row of columns could only ever offer
five, because it spent the entire 368px vertical axis on a target that needs
75 of it. Using the second axis is a fourfold increase in capacity for no new
input, no new state, and nothing hidden.

Twelve fits. So the interesting question ("how does a child reach more apps
than fit?") turns out not to arise at the count the owner actually wants, and
the honest answer to it is: **she does not have to, because they all fit.**
That is worth more here than any navigation scheme, because the user cannot
read: a scheme that hides nine apps behind a gesture asks her to remember that
they exist.

The property that matters most is not any single measurement above, it is
this: **from the fourth app to the twelfth, adding an app never shrinks an
existing target.** The cell height is a constant and the cell width bottoms
out at 112 once there are four apps. What the twelfth app spends is
cancel-band height. The old design spent every target on every app.

## What was rejected

**Pages of four, a swipe turning the page.** Decision 0010 itself floated this
as the least new geometry. Rejected on four counts, the first two decisive:

- A swipe is precisely the gesture this panel cannot carry. Contact drops 34
  times a second and a still finger's reported centroid wanders 80-250px for
  up to three reports. Every swipe implementation has to separate "moved" from
  "the controller lied", which is the exact class of bug that made the palette
  ship broken.
- It collides with the one idiom. Press-drag-release means a horizontal drag
  is *aiming*. A swipe is a horizontal drag. The same motion cannot mean both,
  and disambiguating by speed or distance depends on the measurement this
  controller is worst at.
- It needs a "which page is this" signal. A child who cannot read cannot use
  page dots, and page dots are chrome besides (decision 0002 section 4b names
  "page dots" explicitly). Without them, landing on page 2 shows a screen with
  none of her apps on it and no way to know page 1 exists.
- It hides nine of twelve apps to solve a problem arithmetic already solves.

**A wheel or arc a thumb rotates.** Same input objection, plus a worse one: an
angle is a continuous control tracked across a moving contact, and every
dropout is a discontinuity in it. It also shows six or seven icons at most and
puts the rest off the arc, so it is paging with trigonometry. And a wheel that
spins has momentum, which this panel cannot measure.

**Lean on the fact that she uses three apps and ignores nine** (recency, or
favourites promoted to a front screen). This was the most tempting and took
the longest to reject. It fails on stability: it makes the position of an icon
depend on history, so the picture a child reaches for is not where it was
yesterday. For someone who navigates by picture and position and cannot read a
label to re-find something that moved, **a fixed map is worth more than a short
list.** In the grid, "the drawing one is second on the top row" is true
forever. It would also bury whichever app was written most recently, which is
the opposite of what a night of new apps needs.

**A scrolling grid or a scroll bar.** Momentum again, and hidden content with
no indicator a child can read.

**Smaller icons, more columns.** Fails the fingertip immediately, and costs
the eight redraws decision 0010's ordering argument exists to avoid.

**Folders, or a zoom.** Two levels of hierarchy for twelve items, a concept
(a picture that contains pictures) that has to be taught, and a way back out
that would be chrome.

## What it does not break

**Decision 0009 (nothing is drawn with a ruler).** A grid of icons is exactly
where right angles creep back in, and none did: **nothing about the grid is
drawn**. No cell border, no separator, no tile rectangle, no page dot, no
label. The only marks on the glass are the icons (Lucide, that document's own
named exception) and a round halo. The grid exists only in `menu_hit()`.

**Decision 0002 section 4b (no chrome inside an app).** Nothing was added
inside any app. The menu is the shell and is reached by the BOOT+PWR chord, as
before. Note that a paging design would have needed a page indicator, which is
on 4b's forbidden list by name - one more reason it lost.

**Decision 0001 (the 8-pixel rule).** `gfx` swaps width and height mapping a
landscape rectangle to the panel, so a cell's landscape **height** is the
pushed row length. That is why the cell is 112 and not 110 or 120: the cell
size and the push rule are the same constant. Pushes also got cheaper - moving
the halo used to push a 368-tall strip to change 96px of it, and now pushes 112.

## The one thing that was added: a confirmed hover

The same measured profile that gives 34 dropouts a second throws the reported
centroid 80-250px away from a still finger, and holds it there for one to
three reports. On a grid of 112px cells that is a whole cell in any direction,
and a lift during that window launches an app the child never pointed at.

So the lit cell is **confirmed**: a new cell must be the reported one
continuously for 150ms before the halo moves, and the previously lit cell
stays lit until then. Dropouts do not reset the window, because a tick with no
contact is not evidence about position.

The invariant this buys is the one a child is actually told: **what launches is
always what was lit.** The launch verdict reads the same variable the halo is
drawn from, so the device cannot disagree with her about what she was pointing
at.

**No window makes a wrong halo impossible**, and the first guess about why is
wrong. A jitter episode is at most three reports, so 67ms ought to cover it.
It does not: the runtime bridges dropped contact by holding the *last reported
position*, and more than half of all reports are lost at 34 dropouts a second,
so a wrong position survives until the next truthful report arrives. The window
buys a rate.

So the number came off a measurement. Excursions counted over 240
slide-and-lift gestures per setting, where an excursion is the halo *moving* to
a cell the thumb never touched:

| confirm window | excursions |
|---|---|
| 0ms (off) | 14 per 100 gestures |
| 100ms | 4 per 100 |
| **150ms** | **1 per 240** |
| 200ms | 0 per 240 |

150 rather than 200 because the lag lands on the first light too, and past
about 200ms a child gets no answer from the glass and presses again. What
remains is a halo visibly sitting on the wrong picture, which she can see and
slide off - not a silent wrong launch, which additionally needs the lift to
land inside that same window.

**And the counterfactual is the real finding here.** `feature-menu-hover.ts`
originally asserted on which app launched, and read **20/20 at every row of
that table, including 0ms**: a gesture that settles before it lifts recovers on
its own, so an instrument reading the end of a gesture cannot see a defect in
the middle of it. The excursion counter had to be built before anything could
be measured at all. That is decision 0010's law landing on this change while it
was being made, and it is why the table above exists rather than a paragraph
asserting that 100ms is obviously enough.

## What this cannot do, and where it stops

Three rows of 112 is 336 of the panel's 368, so **twelve is the ceiling** at
four columns and a 96px icon. A thirteenth app does not fall off the screen -
rows are clamped and cells narrow instead - but its targets go under a
fingertip, and that is where a second design is owed. A smaller icon buys
sixteen. Past that something genuinely has to be hidden, and the arguments
above about what a non-reader can navigate will have to be re-fought with a
worse hand. Twelve is the owner's whole list.

At twelve apps the cancel band is 32px. That is enough for the gesture it
exists for, because the controller reports a contact *centroid* and a thumb
slid to the bottom edge of the glass has its centroid pinned there - but it is
a real narrowing. At four apps most of the screen cancels; at twelve,
cancelling means a deliberate slide to the bottom edge. That is the cost of
showing everything, and it is paid by the cancel gesture rather than by any
target.

And the last check stays human. No tool in this tree can say whether twelve
pictures at 112px read as twelve *different* pictures to a four-year-old
across a room. `preview/menu-grid-{4,6,12}.png` exist so that question reaches
an eye without a flash cycle.

## Amended 2026-08-17: decisions 0019 and 0020

Two things changed after this decision was written, and neither one
overturns it.

**The menu no longer always shows every app the firmware carries**
(decision 0019: a five-app roster, separate from `g_apps[]`). "Nothing is
hidden," this decision's own title, was always an argument about PAGING -
that a child never has to remember an app exists behind a swipe or a page
number. It was never a promise that every built app appears on the picker;
taking six apps off the menu without deleting them is a product decision
about what a child can reach, made in the open (a decision record, not a
silent change), not a violation of the paging argument above.

**The cell size stopped being pinned to exactly 112 and can now grow**
(decision 0020) when the roster leaves slack - five and six apps, on this
device, land on 144. The property this document actually protects -
"from the fourth app to the twelfth, adding an app never shrinks an
existing target" - still holds exactly: `menu_cell_h()` can only ever
produce 112 or taller, never smaller, and the nine-to-twelve-app numbers in
this document's own tables are unchanged. Read `docs/decisions/0020` for
the arithmetic and for what does and does not scale alongside a grown
cell (the icon inside it is a bilinear resample of a fixed native render,
not a redrawn shape - most of that decision is about why).
