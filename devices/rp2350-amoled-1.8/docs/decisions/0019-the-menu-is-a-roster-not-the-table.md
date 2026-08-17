# 0019: The menu is a roster, not the table

Date: 2026-08-17
Status: accepted, built

## The problem

The owner asked to cut the device's picker down to five apps - chrono, draw,
timer, four, tables - and take clock, morpion, dino, bowling, tiltball and
breakout off it. Asked explicitly whether that meant deleting the six from
the repo, he said no: only take them off the device. Their source, their
icons, their tests, their previews and decision 0015 (which is entirely
about one of them) all stay.

Before this decision, `g_apps[]` (`firmware/runtime/runtime_core.c`) was
doing two jobs at once: it was both "every app this firmware compiles" and
"every app `menu.c` draws a cell for", because `menu.c` read `g_appCount` and
indexed `g_apps[i]` directly. Every regression test under
`emulator/wasm/tests/` also reaches an app by its position in that same
table (`APP_CLOCK = 4`, `APP_MORPION = 5`, and so on - see
`tools/gate/contracts.ts`'s `checkTestAppConstants`, which cross-checks every
one of those constants against the live firmware on every gate run).

Shrinking `g_apps[]` itself to five entries would have solved "the menu shows
five apps" and broken the second job silently: `APP_CLOCK` would now name
whatever app happened to land at index 4, six test files would start driving
the wrong app or an out-of-range index, and - this is the part decision 0010
and the gate's own posture ("a check that cannot fail is worse than no
check") exist to name - most of those tests would very likely keep printing
`PASS`, just about nothing real.

## The decision

**Two lists, not one.** `g_apps[]`/`g_appCount` (`app.h`, defined in
`runtime_core.c`) stays exactly what it always was: the full, index-stable
table of every app this firmware compiles, in the order every existing test
already addresses it by. A new, separate table sits beside it:

```c
// firmware/runtime/runtime_core.c
const int g_menuAppIndex[] = {
    0,  // chrono
    1,  // sketch ("draw")
    2,  // timer
    3,  // four
    10, // tables
};
const int g_menuAppCount = (int)(sizeof(g_menuAppIndex) / sizeof(g_menuAppIndex[0]));
```

`g_menuAppIndex[slot]` is an index INTO `g_apps[]`, never an app pointer, so
naming an app in the roster costs one integer and touches nothing about that
app's own file. `firmware/apps/menu.c` now builds its grid from
`g_menuAppCount`/`g_menuAppIndex` instead of `g_appCount`/`g_apps[]`
directly: `menu_rows()`, `menu_row_span()`, `menu_paint_all()` and
`render_cell()` all read the roster; a slot's icon is
`g_apps[g_menuAppIndex[slot]]`, and launching a slot calls
`app_switch_to(g_menuAppIndex[slot])`. Everything else in `g_apps[]` -
including the six retired apps - is untouched: same file, same index, same
`app_switch_to()` entry point every test already uses.

An option considered and rejected: a per-app `bool onMenu` flag on `app_t`
itself, set `true` on the five kept apps. Cheaper to read, but it would have
meant editing five apps' own struct literals to add the flag - including
`firmware/apps/four.c`, which another agent had open at the same time this
change was made. The two-list shape needs zero edits to any `apps/*.c` file;
the whole roster lives in one place, `runtime_core.c`, beside the table it
is a view of.

## What this buys, checked against the firmware itself

**The menu shows exactly the five apps kept**, verified by name against the
compiled firmware, not assumed: `feature-menu-hover.ts` now asserts
`[chrono, draw, timer, four, TABLES]` (`g_apps[]`'s own `.name` strings,
resolved through `g_menuAppIndex` the same way `menu.c` resolves them) before
any of its existing gesture checks run. Confirmed to be load-bearing by
deliberately swapping slot 4 to point at clock instead of tables and
rebuilding: the assertion failed, printing `menu = [chrono, draw, timer,
four, clock]`, before the change was reverted.

**Every retired app's own test still runs and still exercises real code.**
`feature-clock.ts`, `feature-morpion.ts`, `feature-dino.ts`,
`feature-bowling.ts` (+ `feature-bowling-provisional-release.ts`),
`feature-tiltball.ts`, `feature-breakout.ts`, and every one of their
`repro-touch-dropout-*` partners still call `app_switch_to()` with the exact
same `g_apps[]` index they always did, because `g_apps[]` never moved. All
40 files under `emulator/wasm/tests/` pass. `tools/gate/run.ts` also drives
all 11 real apps directly, by table index, never through the menu - unaffected
by the roster and still green.

**Decision 0013 still holds for the five that remain.** The grid's own rule
("a cell is 112px tall, always, and at most four across, so a cell is never
narrower than 112") is a function of `g_menuAppCount`, and it does not care
whether that number is smaller than `g_appCount`. At five: two rows (3 then
2, balanced, the same rule that gave four apps 3-then-2 rather than 4-then-1
at decision 0013's original six), cells 149x112 (row 0) and 224x112 (row 1),
cancel band 144px. Every cell is comfortably above the 112px floor - nothing
narrowed, because five is fewer than the four-app baseline the floor is
measured from, not more.

**"Nothing hidden" still describes what IS on the menu.** Decision 0013's
title is about paging: with five apps on one screen and no swipe, nothing on
the picker is one gesture away from being missed by a child who cannot read
a page number. It was never a promise that every app the firmware carries
appears on the picker - that promise does not exist anywhere in this
project, and taking an app off the menu is a product decision (what a child
should be able to reach), not a violation of an arithmetic rule about paging.

## What the picture actually looks like

`preview/menu-grid-5.png` and `preview/menu-grid-5-lit.png`
(`tools/preview-menu-grid.ts`, rebuilt against the real firmware). Read
plainly: this is a noticeably emptier screen than the eleven- or twelve-app
captures. The cancel band is 144px, 39% of the panel's 368px landscape
height, and row 1's two cells (four, tables) sit 224px apart centre to
centre with a wide gap of bare paper between the two icons - there is no
version of five icons at generously-sized targets that fills a screen built
for twelve. Every individual target is well above the 112px floor and the
grid is still legible and uncluttered, so this is not a regression against
decision 0013's own rules; it is simply what "fewer apps, same panel" looks
like, and it is a look the owner can revisit by lengthening the roster
again - one line in `g_menuAppIndex`, nothing else.

## The one thing NOT done here

`stubapps.c`'s screenshot fixture (`MENU_STUB_APPS`) still exists to let
`tools/preview-menu-grid.ts` render hypothetical six- and twelve-app
layouts through the real `menu_enter()`. Under that build define,
`g_menuAppIndex` is the identity mapping over the padded `g_apps[]` table
(every real app plus the stub, in order) rather than the fixed five-entry
roster, so that fixture keeps doing the job described in its own header
comment. This branch is never compiled into a real build - `CMakeLists.txt`
never lists `stubapps.c`, and nothing here defines `MENU_STUB_APPS` on its
own.
