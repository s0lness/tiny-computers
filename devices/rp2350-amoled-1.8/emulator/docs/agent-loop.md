# The agent loop: freeze and annotate

**This layer is optional.** The emulator is fully usable, for every
capability described in [requirements.md](requirements.md), with this layer
absent: launching, iterating on an app, screenshotting, pausing, stepping,
recording and replaying a trace, and debugging all work without anything
described in this document existing. What follows is what gets added on top
for the specific case of a coding agent working alongside the person using
the emulator, and it is deliberately kept removable: deleting this layer's
directory must leave a fully working emulator behind.

## Why this is a separate layer, in its own directory, with no coupling back

The dependency arrow points one way only. This layer reads the emulator
core's state (the event log described in
[requirements.md](requirements.md#ranked-capabilities), the framebuffer, the
device descriptor); the core has no import, no hook, and no awareness that
this layer exists. Concretely: `emulator/wasm/`, `emulator/server.ts`, and
whatever drives the `emu_tick` loop never reference anything under this
layer's directory. This layer imports from the core, never the reverse.

That asymmetry is not a style preference, it is what makes the freeze feature
possible at all. A **freeze** is only worth having because it captures the
input trace that produced the frame, alongside the frame itself. A
screenshot tool built outside the emulator (a browser extension, a separate
CLI that drives Puppeteer against the running page) can capture pixels, but
it has no access to the event log that lives inside the emulator's own
process, so it can only ever produce a picture with no story of how the
device got into that state. The value this whole document is arguing for
comes specifically from freeze living close enough to the core to read that
log, while staying far enough from it that the core owes it nothing.

## The freeze-and-annotate loop

1. **Freeze.** A control in the emulator page (or a hotkey) captures a
   bundle: the current frame as a PNG, plus a JSON sidecar holding
   everything needed to understand how the frame came to look the way it
   does. See [the bundle shape](#the-bundle) below for exactly what that
   JSON contains and why each field is there.
2. **Annotate.** The frozen frame is, by construction, a static image: the
   emulator is paused for the duration of a freeze, so nothing repaints out
   from under a mark. The user draws on it and writes notes exactly the way
   they already do on any other page in this project's fleet, through
   `markup`'s own ink layer (`drawesome`, bundled by `markup`): circle
   what's wrong, right-click to note it, type in place. This is not a second
   annotation tool built for this project; it is the same one already used
   for every other site in the fleet, applied to a frame that happens to
   come from a device panel instead of a web page.
3. **Pick up.** An agent reads the bundle from a predictable path with a
   stable schema, described below, the same way the `markups` skill already
   reads a `journal.json` for every other project in this fleet.

## Following markup's journal format, and where this diverges from it

`markup`'s own journal (`src/types.ts` in that repo, specifically
`JournalPayload`) is the existing, working answer to "hand annotated feedback
to an agent as one JSON document," and it is used everywhere else in this
project's fleet. The freeze bundle should look like that format wherever the
underlying thing is actually the same, and should diverge, explicitly, where
it is not.

**Kept from markup, because the underlying thing is the same:**

- The envelope shape: a versioned payload (`v: 1`), `exportedAt`, and a
  `viewport`.
- `MarkType` (`'f' | 'q' | 'n'`, ink colour is the type: red fix, blue
  question, green new thing) and the `Note`/`Pin` shapes for anything drawn
  or written on the frame. A question drawn on a frozen panel is exactly as
  much a question as one drawn on a web page; there is no reason to invent a
  second vocabulary for it.
- The reply loop's semantics (`MarkStatus`: `open | ack | resolved |
  dismissed`, a `thread` of messages rather than overloading status), if and
  when freeze bundles grow the same "answer later" lifecycle markup's cloud
  sink gives its marks. Not required for a first version, worth keeping the
  same shape for if it is added, rather than inventing a second state
  machine.
- The consuming convention: an agent groups marks by type the way
  `report.py` already does (answer the `?`s, act on the `!`s, treat the
  `+`s as scoped work), even though the specific script differs, since
  there is no in-place source text to rewrite here the way `apply.py`
  rewrites a web page's copy.

**Diverges from markup, and why:**

- **No `strokesByPage` map.** markup's journal is keyed by page because one
  journal covers an entire, multi-page, live website (`Record<string,
  Stroke[]>`, one entry per page name). A freeze bundle is one frame, frozen
  at one instant; there is exactly one "page," so the freeze bundle's marks
  are a flat list, not a map keyed by a concept that does not exist here.
- **No `TextEdit`.** markup's in-place copy editing (`before`/`after` text,
  reapplied on every load via a `MutationObserver`) exists because a web
  page has editable DOM text nodes. A device panel's framebuffer is pixels
  a firmware wrote, not text a browser laid out; there is nothing to retype
  in place. A `!` mark on a frozen panel is a note pointing at a rectangle
  of pixels, never a text rewrite.
- **No `Frame`, `AttachedElement`, or `CapturedState`.** These exist in
  markup specifically because a live page reflows and repaints, so a mark
  needs enough context (a CSS selector, a resolved box, a fingerprint of
  what else was on screen) to be re-anchored correctly after the page
  changes shape. A frozen frame never reflows: it is a fixed-size PNG at a
  fixed pixel size, by definition, for as long as it exists. A mark's
  position on it is just `(x, y)` in that PNG's own pixel space, and it
  never needs re-anchoring because nothing about the frame ever moves under
  it.
- **New fields markup's schema has no reason to carry**, because they are
  specific to what makes a firmware frame debuggable rather than what makes
  a web page correctable: `device` (the `emu_device()` descriptor), `app`
  (the current app id, when the firmware declares apps), `pushWindows`
  (the recent push rectangles from the same event log described in
  requirements.md), `inputTrace` (the recorded sequence of touch/button/
  sensor events and tick timestamps that produced this exact frame), and
  `log` (the guest's own log lines up to the freeze). These are the entire
  reason freeze exists for this project: an agent debugging a firmware
  regression needs the trace and the pushes at least as much as it needs
  the pixels, and markup, built for annotating web copy, has no field for
  any of them because no web page has ever needed one.

## The bundle

One freeze produces two files, written together:

- **A PNG** of the panel at freeze time, at the panel's native resolution
  (`emu_device()`'s declared `panel.w` x `panel.h`), unrotated, matching what
  `emu_fb()` actually holds rather than whatever rotation the page happens
  to be displaying it at.
- **A JSON sidecar** holding:
  - `v`, `exportedAt`, `viewport`: the envelope, matching markup's shape.
  - `device`: the full `emu_device()` JSON, verbatim, so an agent reading
    the bundle in isolation knows what device this is without cross
    referencing anything else.
  - `app`: the current app id, if the firmware declares apps (omitted
    entirely when it does not, matching how `emu_app_current`/
    `emu_app_switch` are themselves optional in the ABI).
  - `pushWindows`: the push rectangles from the tick(s) immediately
    preceding the freeze, the same data the live push overlay draws, so a
    partial-refresh bug like the one behind
    [decision 0001](../../docs/decisions/0001-push-min-width.md) is visible
    in the bundle itself, not only on screen at the moment someone was
    looking.
  - `inputTrace`: the recorded `(nowMs, event)` sequence, from the later of
    `emu_init()` or the last freeze, that produced this frame. This is what
    makes the bundle actionable rather than merely descriptive: an agent (or
    a person) can replay it against a rebuilt module and watch the same
    thing happen again, deterministically, per
    [requirements.md](requirements.md#record-and-replay-of-input-traces).
  - `log`: the guest's `js_log` lines emitted since the same starting point
    as `inputTrace`, so the agent has the firmware's own diagnostic output
    lined up against the same window of time.
  - `notes`, `pins`: markup-shaped, added after the freeze once the user has
    drawn on the frame.
  - `svg`: an optional per-frame SVG of the ink, the same convention markup
    already uses so a mark can be composited over the PNG without
    re-rendering anything.

## Where an agent finds it

A predictable path, gitignored and transient, the same convention markup
already uses for its own `journals/<site>/` (`AGENTS.md`: "`.markup/` is
transient, gitignore it in the target repo"):

```
emulator/.freeze/
  latest.png
  latest.json
  history/
    <timestamp>.png
    <timestamp>.json
```

`latest.json` / `latest.png` are overwritten on every freeze, so an agent
that just wants "whatever was last frozen" has one fixed path to read with
no directory listing and no timestamp parsing. `history/` keeps every prior
freeze for the cases that need more than the most recent one (bisecting
across a session, or an agent that was pointed at a specific freeze by
timestamp rather than "the latest"). This mirrors, deliberately, the same
"one stable current path plus a history directory" shape markup itself uses
for its journals, rather than inventing a third convention for the same
problem.
