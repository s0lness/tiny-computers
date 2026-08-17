# 0020: Cells grow for a small roster, and the icon inside one is a raster resample, not a redraw

Date: 2026-08-17
Status: accepted, built

## The problem

Decision 0019 cut the on-device menu to five apps. Looked at
(`preview/menu-grid-5.png`, before this decision), it read as a half-empty
screen: five 112px icons huddled at the top in a 448x368 landscape canvas,
39% of the panel (144px) spent on nothing, and the two icons in row 1 sitting
224px apart with a wide strip of bare paper between them. The owner's
verdict: make the cells bigger, close that gap, and do not let the icons
stay small inside bigger boxes - "big empty boxes with small marks in them"
is explicitly the failure mode to avoid.

Two separate questions follow from that, and they turned out to have very
different answers:

1. **How big can a cell honestly get?** Arithmetic - decision 0013's own
   kind of problem, and decision 0013's own invariant (cells never shrink)
   still has to hold.
2. **Does the icon inside it get bigger too?** This is the real work. Every
   icon in `menu.c` (chrono's ring, timer's hourglass, sketch's squiggle,
   and the rest) is built from hand-tuned geometry - literal pixel offsets,
   compile-time-sized lookup tables, stroke widths chosen by eye against a
   rendered PNG - fixed against one constant, `ICON_W`/`ICON_H` = 96. None
   of it was written to be told "draw yourself at 128 instead."

## The decision: two independent mechanisms, not one

### 1. The cell height, `menu_cell_h()`

Grown past `MENU_CELL_FLOOR` (112, decision 0013's own number, unchanged)
when there is slack, bounded by the SMALLER of:

- `MENU_AVAIL_H / rows` - what fits at all, so growth can never push a row
  into the reserved cancel band or under a newly-added top inset (below);
- the narrowest row's own cell WIDTH - so a cell never drifts far past
  square. Nine-to-twelve apps still pack four-wide rows (112px), so this
  term alone holds the three-row case at exactly 112, unchanged from
  decision 0013's own table.

Rounded DOWN to a multiple of 8 before use (decision 0001: a cell's
landscape height is the pushed row length), never below the floor.

**MENU_TOP_INSET, added the same day.** `gfx.h`'s `PANEL_BEZEL_MARGIN_PX`
(10px, found 2026-08-14) is the band the case hides along every edge, and
every other app that draws near an edge already keeps clear of it
(`four.c`, `morpion.c`, `bowling.c`, `breakout.c`, `tables.c` each define
their own `SAFE_Y0` from it). The menu never did - it packed icons to
landscape `ly=0` from the day the grid was built (2026-08-15), one day
after the bezel was even found, and nobody carried the rule over. Checked
against the mapping (`gfx.h`: landscape `(lx,ly)` -> panel
`(PANEL_W-1-ly, lx)`), row 0's icon ink was landing at panel `x=359`, one
pixel inside the nominal margin band (`x>=358`) - a real, if narrow,
near-miss. With row 0's icons about to get taller and closer to that same
edge, this was the moment to fix it rather than leave a second gap for
later. `MENU_CANCEL_FLOOR` dropped from decision 0013's 32 to 22 to pay
for it (`MENU_AVAIL_H = LAND_H - MENU_TOP_INSET - MENU_CANCEL_FLOOR = 336`,
the same 336 decision 0013 built its own three-row ceiling from).
`menu_hit()` now treats the top inset strip the same way it already treated
the cancel band below the grid: a release there does nothing.

**What this actually produces**, checked against the real compiled firmware
by `tools/preview-menu-grid.ts` (all six counts regenerated, all read
correctly - see "What this looks like" below):

| apps | rows | cell (w x h) | cancel band | changed from before this decision |
|---|---|---|---|---|
| 4 | 1 | 112 x 112 | 246px | no (shifted 10px for the top inset) |
| 5 | 2 (3+2) | 149/224 x **144** | 70px | **yes - grown** |
| 6 | 2 (3+3) | 149 x **144** | 70px | **yes - grown** |
| 7 | 2 (4+3) | 112/149 x 112 | 134px | no (shifted) |
| 11 | 3 | 112/149 x 112 | 22px | no (shifted) |
| 12 | 3 | 112 x 112 | 22px | no (shifted) |

Only five and six apps actually grow, and it falls out of the arithmetic
rather than being special-cased for five: both are the only counts in this
device's range where two things are true together - two rows are needed,
and the narrowest row in that layout still has slack past 112 (five is 3
then 2, six is 3 then 3; seven's four-wide first row pins it back down to
112, same as nine-to-twelve's four-wide rows do). The five-app case this
was written for gets a 144px cell, up from 112 - a 29% taller target - and
a 70px cancel band, comfortably above decision 0013's own accepted-fine
32px.

### 2. The icon, `menu_icon_capture()` / `menu_icon_blit()`

**What was checked before deciding this, not assumed.** `chrono`'s icon
(`draw_icon_chrono`) sizes its ring from `ICON_W`-derived macros
(`CHRONO_R_OUT = ICON_W * 17 / 48`), which would in principle track a
resized `ICON_W` - but its crown and tab are hand-wobbled float tables
(`s_chronoCrownDx/Dy`, `s_chronoTabAlong/Perp`) of literal pixel offsets
that would not. `timer`'s icon (`draw_icon_timer`) is built entirely
against a compile-time-sized lookup table,
`s_timerBulbHw[TIMER_CHAMBER_H]`, and a set of literal constants tuned by
eye (`dipRows = 5`, `sliverPx = 2`, a four-entry heap table
`{1, 2, 3, 4}`) that have no relationship to `ICON_W` at all. Re-deriving
eleven icons' worth of this - much of it already fought over pixel by
pixel against rendered PNGs, chrono's own header comment records THREE
rounds of owner correction - to accept a runtime size parameter was judged
not to be a one-day pass's job, and was very likely to visibly break
something that took real iteration to get right the first time.

**So the icon is never asked to draw itself bigger.** `menu_icon_capture()`
draws it once, at its normal native 96x96, exactly as `draw_icon_for()`
always has, then reads the result straight back off `gfx_fb` (the same
landscape pixel mapping `shapes.c`'s own `aa_composite_land()` uses,
`(lx,ly) -> (PANEL_W-1-ly, lx)`) into a small capture buffer.
`menu_icon_blit()` then repaints that capture into the grown footprint with
a bilinear resample - a raster operation on the already-anti-aliased ink,
the same "read the framebuffer back, widen it, recompose" move `sketch.c`'s
own anti-aliasing already makes (see that file's header comment on why the
green channel doubles as an 8-bit coverage value), applied at icon scale
instead of at stroke scale.

Composition stays MIN throughout (`shapes.h`'s own argument, unchanged): a
resampled pixel only ever darkens the destination, never brightens it, so
the halo (drawn separately, at the cell's own real size - a disc is
resolution-independent by construction, nothing to resample) can be drawn
either before or after the icon blit with an identical result.
`render_cell()` draws it in between the capture and the blit, which is
simplest, not because order matters to the composite.

**A small guard margin** (4px, captured but never blitted past the icon's
own `size` box) exists only so a bilinear sample exactly at the
destination's own edge has a real neighbour to interpolate against, rather
than running off a plain 96x96 capture. The write itself stays bounded to
EXACTLY `size` x `size`, centred the same way the native capture was -
never wider, so a grown icon cannot repeat the "halo painted into the
neighbour" bug `menu.c`'s own halo comment already tells the story of
(250 outside-push violations, the first time that shipped).

`MENU_HALO_R_MAX` was raised from 58 to 72 in the same pass: the per-cell
`min(bw,bh)` computation already contains the halo inside whichever cell it
lives in on its own, so 58 was never load-bearing for safety, only for how
big the halo was ALLOWED to look - and at a grown cell's own natural size
(69, computed from the smaller half-extent minus the gap) it was clipping
the halo back down to the old twelve-app number for no reason once cells
started growing. Cells that stayed at 112 are still bound by their own size
long before 72 is ever reached, so nothing changes for them.

## What was rejected

**Re-deriving every icon's geometry to accept a size parameter.** The
honest one, and the one actually asked for first ("check what the icon
drawing does... if it's fixed size, that's the real work"). Rejected for
this pass specifically because it is eleven icons' worth of hand-tuned,
already-fought-over pixel work, not because it is wrong in principle - a
future pass that wants genuinely crisp vector scaling (rather than a
bilinear resample, which does soften an edge slightly under magnification)
would still need to do this, icon by icon, checked against a rendered PNG
each time the way chrono's own three rounds already were.

**A per-row icon size, sized independently for each row's own cell
dimensions.** Considered so row 1's wider (but same-height) cells at five
apps could show an even bigger icon than row 0's. Rejected: `menu_cell_h()`
is one number for the whole grid on purpose (uniform pushes, uniform halo
math, decision 0013's own choice), and letting the ICON size differ
per-row while the CELL height stays uniform would put a visibly
different-sized icon in row 0 versus row 1 for no reason a child could
read as meaningful. `menu_icon_size()` uses `min(cellW, cellH)`, which
already comes out identical for every row when the height is what binds
(true at every app count this device has ever shipped, five apps
included), so this was never actually needed - noted here so a future
five-that-are-not-3+2 split does not resurrect the temptation without
re-arguing it.

**Letting cell height grow unbounded for very few apps** (one row, e.g. a
hypothetical single-app menu, would have room for a ~336px cell under the
height bound alone). Rejected by the second bound in `menu_cell_h()` - the
narrowest row's own WIDTH - which pins the height back to roughly square
for exactly this reason: a 112-wide, 336-tall cell would spend almost all
of the extra height as blank margin around an icon that is still capped by
the narrow width, not as anything visible. This is why four apps (all
112px-wide columns) do not grow at all under this decision, and it is the
correct, arithmetic answer, not an oversight.

## What this cannot do, and where it stops

**A bilinear resample is not a redrawn icon**, and the first version of
this decision claimed that cost was invisible at today's modest scale
factor (96 to 128, 1.33x) "judged by eye against the rendered PNG" - true
of the eye that looked, and not thorough enough: it looked at the whole
grid at a glance and missed that four's icon was visibly broken. The owner
caught it on a second look (`preview/menu-grid-5.png`, before the fix
below): two clean rings at 112px had become two solid discs and two ragged
white CRESCENTS at 144.

**That was a real bug in this file, not an inherent limit of resampling at
this scale, and it is fixed now.** `menu_icon_capture()` draws the icon
once at its native 96px to read it back - and left that native render
sitting on `gfx_fb` afterward. Composition here is MIN everywhere
(`shapes.h`'s own rule: a pixel only ever darkens, never brightens), so the
native render was never erased before `menu_icon_blit()` painted the
resampled version on top - it stayed underneath as a second, smaller copy
of the icon. For an icon whose features sit on the icon box's own centre
(chrono's ring, timer's hourglass, tables' X) the native and resampled
copies are concentric and the smaller sits entirely inside the bigger's own
ink, invisible. Four's icon draws four sub-circles, each offset from the
box centre - so its native and resampled copies of the SAME ring land at
two different positions, and where a ring is drawn as an ANNULUS (the two
empty cells) the two copies' holes only partly overlap: a crescent, worked
out precisely in `repro-menu-icon-resample-ghost.ts`'s own header comment
(two circles of different radii, both centred on the same ray out from the
box centre but not on each other, closer together than either radius, so
the arithmetic intersection is a lens shape, not a hole). `render_cell()`
now repaints the native 96px footprint to white immediately after the
capture, before the halo or the blit ever touch it - see that call site's
own comment.

**Checked per icon after the fix, by eye, against the native (unresampled)
render at the same crop size - not assumed clean because the bug that broke
one was found and fixed.** All five icons the real device menu shows today
(`preview/menu-icon-{chrono,sketch,timer,four,TABLES}-128-4x.png` against
their `-96-4x.png` native counterparts, generated by
`tools/preview-menu-icons.ts` from an 11-app stub build for a same-size
comparison): chrono's ring and wedge, sketch's pencil (including its small
eraser-tip notch), timer's hourglass crossing, four's four circles (both
solid and both rings, post-fix), and tables' X all matched their native
version's shape with no cropping, thinning or displaced feature - left
resampled, on this evidence, all five. `repro-menu-icon-resample-ghost.ts`
turns the specific defect that was actually found (not a generic
"resampling might lose detail" placeholder) into a standing regression
check: it samples the exact pixel where the native ring's far edge lands
inside the resampled ring's hole, confirmed to fail before the fix (reading
pure ink, gray 0) and pass after, by deliberately reintroducing the bug and
reverting it - not assumed from the arithmetic alone, since a first attempt
at the same sample point's geometry was wrong in the OTHER direction (see
that file's own comment) and would have passed whether the bug was present
or not.

**Still worth naming as a real, if smaller, limit**: a bilinear resample
does soften an edge slightly compared to drawing it natively at the larger
size, visible under close magnification (the AA edges in the 128px crops
read a touch less crisp than the 96px native ones at the same zoom) but not
at the sizes and viewing distance this device is actually used at. Would be
worth re-checking by eye again if a future roster cut ever pushed the scale
factor much higher than today's 1.33x.

**The stub screenshot fixture changed shape to keep covering this.**
`firmware/apps/stubapps.c`'s `MENU_STUB_APPS` used to only ever GROW the
table (11 real apps plus one stub, reaching 12). It now also TRUNCATES:
`g_menuAppCount` becomes `min(MENU_STUB_APPS, array length)`, so
`MENU_STUB_APPS=4`, `6` or `7` shows the menu at that many apps too, built
from the first few real apps in table order - their identity does not
matter for judging layout geometry, only their count does. One mechanism
now covers every count `tools/preview-menu-grid.ts`'s regression sweep
needs (4, 5, 6, 7, 11, 12) rather than a padding-only fixture that could
only ever reach 11 or 12.

## What this looks like

`preview/menu-grid-{4,5,6,7,11,12}.png` and their `-lit` pairs, all
regenerated from the real compiled firmware and opened, not just measured:

- **Five apps now reads as a considered screen, not a half-empty one.** The
  icons are visibly bigger, row 1's gap is tighter (128px icons in 224px
  cells rather than 96px icons in the same cells - the whitespace between
  them dropped from 128px to 96px, about a quarter less), and the blank
  band below the grid is a third of what it was (70px, down from 144).
  There is still a real gap in row 1 - five apps in a 3-then-2 split cannot
  make every cell the same width - and that is said plainly rather than
  claimed fixed: it is smaller, not gone. Checked whether closing it
  further falls out of the ghost-render fix above: it does not, and
  neither cell size nor icon size changed by a single pixel in that fix -
  `menu_cell_h()` and `menu_icon_size()` are untouched, only what got drawn
  at the size they already compute. Closing it further needs one of two
  things this same pass does not: icon size varying per ROW rather than
  uniformly across the grid (considered and rejected above, under "What was
  rejected" - a visibly different icon size between row 0 and row 1 for no
  reason a child could read as meaningful), or a different row/column split
  than the balanced one decision 0013 established and this file's own
  tests already exercise. Left alone; said so rather than claimed fixed.
- **Six apps grew the same way**, landing on perfectly square 149x149
  cells (both rows pack three across, so `menu_cell_h()`'s width bound and
  height bound agree exactly) - the cleanest-looking capture of the six.
- **Four, seven, eleven and twelve are visually unchanged** except for
  sitting 10px lower (the new top inset) - confirmed by eye against the
  previous captures, not just by the numbers in the table above matching.

`feature-menu-hover.ts` gained one new assertion checking the roster by
NAME against the compiled firmware's own `g_apps[]` (`[chrono, draw,
timer, four, TABLES]`), confirmed load-bearing by temporarily pointing
slot 4 at clock and rebuilding - the assertion failed, printing `menu =
[chrono, draw, timer, four, clock]` - before being reverted. The positional
constants this decision touches (`MENU_TOP_INSET`) were checked the same
way: mismatching the test's own copy against the firmware's by 17px made
every corner probe fail immediately, confirming the corner-probe test is
still reading the firmware's own hit test rather than agreeing with
itself (decision 0010's own standard). A third mutation - skipping the
round-to-8 step in `menu_cell_h()` - did NOT produce a gate failure, which
is itself worth recording rather than quietly dropping: `gfx_push_land`'s
own rounding already widens a misaligned window to the next multiple of 8
before it is ever recorded, and since the full grid is always painted
before any push happens, the extra few pixels sent are already correct.
The round-down in `menu_cell_h()` is still kept - it matches this file's
own established discipline ("the cell size and the push rule are the same
constant") and avoids sending pixels nobody asked for - but it is not
load-bearing for correctness the way `MENU_TOP_INSET`'s positioning is,
and this document says so rather than implying a red/green result that
this particular mutation did not actually produce.
