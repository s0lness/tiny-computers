# emulator

A local, interactive emulation of the Waveshare RP2350-Touch-AMOLED-1.8 puck
running the sketchpad from `firmware/main.c`, so the drawing app can be
played with and tuned without the hardware. Built because a live debugging
session was running on the real board; this never touches it (no picotool,
no flashing, no serial).

```
cd emulator
bun install
bun run server.ts     # http://127.0.0.1:5330
```

Quit from the page (there is no console once it is backgrounded): the
**quit** button in the top bar. `POST /api/quit` requires the header
`x-rp2350-emulator: 1`, which a cross-origin page cannot set without a
preflight this server never answers, so that closes the localhost-CSRF hole.

## What this is

The panel (368x448, white paper, black ink) is driven by a line-by-line
TypeScript port of the firmware's stroke pipeline, not a generic canvas
sketch tool. On top of that sits a simulated FT3168 touch controller that
reproduces the *imperfections* the firmware code exists to survive:
dropped contact mid-stroke, occasional stray reports at the wrong position,
and a configurable report rate. All the pen and touch-robustness constants
from `main.c` are exposed as sliders with live values, meant to be tuned by
feel here and pasted back into the firmware (`copy #defines` button in the
top bar).

## Layout

```
emulator/
  server.ts        Bun server, binds 127.0.0.1:5330, /api/quit
  build.ts          bundles src/ into dist/ for markup (see below)
  src/
    index.html       page shell + device markup
    app.css           device chrome (family-budget page + puck body)
    family-budget.css local copy of ~/.claude/design/family-budget.css
    constants.ts       the tunables, defaults, slider metadata
    raster.ts           draw_capsule() port + dirty-rect bookkeeping
    pen.ts               stroke_begin/sample/end port
    touchsim.ts           simulated FT3168 (report rate, dropouts, strays)
    engine.ts              the main-loop touch state machine + boot pattern + shake
    device.ts                drag / rotate / PWR / BOOT wiring
    main.ts                    DOM wiring: canvas, sliders, stats, export, quit
  scripts/verify.ts   headless check (see "How this was verified")
```

## What is faithful

Ported line-by-line from `firmware/main.c`, not reinterpreted:

- **`raster.ts` `Panel.drawCapsule`** = `draw_capsule()` (main.c:136-182).
  Same signed-distance-to-segment capsule, same
  `coverage = clamp(r + 0.5 - d, 0, 1)`, same **MIN composition** (`if (ink
  < cur) write`), not alpha blending. This is the one piece of the pipeline
  the task called essential to get right: alpha blending re-darkens the
  overlap between consecutive segments and turns the line banded and
  pixelated. MIN just unions the ink shapes. Verify it by drawing a slow
  curve and looking for banding at segment joins; there should be none.

- **`pen.ts` `PenState`** = `stroke_begin` / `stroke_sample` / `stroke_end`
  (main.c:232-292) and `pressure_to_radius` (main.c:225-230): streamline
  toward the raw point (`STREAMLINE`), simulated pressure from travel speed
  (`SPEED_MAX`), pressure rate-limited toward its target (`PRESSURE_LERP`),
  `radius = PEN_SIZE * easeOutSine(0.5 - PEN_THINNING * (0.5 - pressure))`,
  a resting-finger dead zone (`DEDUPE_PX`), a start taper over
  `START_TAPER_LEN`, and a three-step end taper. The `bridge` flag (a
  sample immediately after a dropout snaps straight to the report instead
  of streamlining, and does not touch pressure) is reproduced exactly,
  because it is what keeps a bridged dropout from putting a kink or a
  sudden thin patch in the line.

- **`engine.ts` `Engine.tick`** = the touch-handling half of `main()`'s
  `while (true)` loop (main.c:858-993): stroke start requires two
  *agreeing* reports before any ink is laid down (`pendingStart`), a
  mid-stroke jump is accepted only within a speed-derived allowance
  (`MAX_SPEED_PX_PER_MS`, floored by `MIN_JUMP_ALLOW_PX`, capped by
  `MAX_JUMP_PX`), a rejected-but-confirmed jump ends the current stroke and
  starts a new one instead of drawing a line across the gap
  (`CONFIRM_PX`), and a lift is only believed after `LIFT_DEBOUNCE_MS` with
  no contact, so a real dropout continues the same stroke rather than
  taper-and-restart it. The `newReport` dedupe (main.c:895) is reproduced
  too, and matters here for the same reason it matters on the firmware:
  the tick loop runs far faster (every 3ms) than any realistic report
  rate, exactly mirroring the firmware's ~5000Hz loop against a ~60Hz
  controller.

- **`engine.ts` `showBootPattern`** = `draw_test_pattern()` (main.c:356-373):
  the grey ramp and the four capsules at the pen's radii, shown once at
  boot and cleared on first touch (`haveTouch && patternShown`,
  main.c:876-881).

- **`engine.ts` `shake()`**'s band-by-band wipe = `wipe_erase()`
  (main.c:336-348): 16 horizontal bands, each filled and pushed with a
  15ms delay between them, not an instant blank.

All twelve constants named in the brief are exposed as sliders with the
firmware's exact defaults (`src/constants.ts`, `FIRMWARE_DEFAULTS`, sourced
from main.c:41-82): `STREAMLINE`, `DEDUPE_PX`, `SPEED_MAX`,
`PRESSURE_LERP`, `PEN_SIZE`, `PEN_THINNING`, `START_TAPER_LEN`,
`LIFT_DEBOUNCE_MS`, `MAX_SPEED_PX_PER_MS`, `MIN_JUMP_ALLOW_PX`,
`MAX_JUMP_PX`, `CONFIRM_PX`.

## What is approximated (and why)

- **No RGB565 6-bit-green quantisation.** The firmware stores ink in the
  green channel of an RGB565 pixel because it has no spare 165KB for a
  separate coverage buffer (`px_to_gray`/`gray_to_px`, main.c:116-128);
  reading it back quantises every value to steps of 4 out of 255. This
  emulator keeps ink as a plain 0..255 byte with no such round trip. The
  effect is real but very fine (roughly 1.5% banding on a smooth gradient)
  and does not change stroke shape, taper, or the MIN-vs-alpha behaviour
  the task called out as essential, so it was left out to keep `raster.ts`
  readable. If it ever needs reproducing, `px_to_gray`/`gray_to_px` are
  short and pure; port them into `Panel.drawCapsule`'s read/write.

- **JS float64, not C float32.** Every arithmetic op in the port uses
  JavaScript numbers (float64) where the firmware uses `float` (32-bit).
  This can only matter at the level of individual bits of an edge pixel's
  coverage value and is not visible at 368x448.

- **The touch loop's actual polling rate isn't reproduced, only its
  consequence.** The firmware polls I2C at roughly 5000Hz against a ~60Hz
  controller; this emulator ticks every 3ms (~330Hz) against a configurable
  report rate. The behaviour that rate difference produces — most polls
  see no new report, and the `newReport` dedupe is what makes that safe —
  is fully reproduced; the literal frequency is not, because a browser
  timer cannot usefully go faster than that and there is no I2C bus to
  actually saturate.

- **Shake is a button, not an accelerometer.** `shake_poll_and_check()`
  (main.c:303-334) requires several jolts inside a rolling 700ms window
  before it believes a shake, specifically because one big reading is
  indistinguishable from a firm tap. There is no simulated IMU here, so
  the emulator's shake control fires the erase directly. It still
  reproduces the two behavioural rules that matter for feel: suppressed
  while a finger is down, and a 1200ms cooldown so one shake cannot erase
  twice (`ERASE_COOLDOWN_MS`, both ported unchanged from main.c:96 and used
  in `engine.ts`).

- **Touch controller imperfections are a statistical model, not a port.**
  There is no real FT3168 register trace to replay. `touchsim.ts` draws a
  per-report Bernoulli event (probability = rate x period) for dropouts and
  strays, at rates matching the task brief ("1 to 3 per second while
  drawing" for dropouts). This is close enough to tune the debounce and
  confirm-radius constants by feel, but it is not measured hardware data.

- **PWR/BOOT button electricals are not modelled.** The real PWR button is
  read from the AXP2101 PMIC's interrupt status registers over I2C, not a
  GPIO edge (main.c:404-421, found the hard way — see main.c's own
  "Button hunt" comment). The emulator reproduces the *timing* that
  matters (short press, the 1.5s long-press threshold, the 6s hard
  power-off threshold, all real AXP2101 REG 27H defaults) with plain
  `pointerdown`/`pointerup` timers, not an I2C simulation. BOOT is wired
  inert on purpose: confirmed on hardware that it produces nothing at
  runtime (it is a bootloader-entry strap sampled once at power-on).

## Constants duplicated across two languages (keep these in sync by hand)

Everything in `src/constants.ts` `FIRMWARE_DEFAULTS` mirrors a `#define` in
`firmware/main.c` (lines 41-82) by value and by name. There is no shared
source of truth between C and TypeScript; if a constant changes in one
place it silently drifts from the other until someone notices the drawing
feels different. The **copy #defines** button in the emulator's top bar
exists specifically to close that loop: tune a slider, copy, paste over the
corresponding block in `main.c`.

Also duplicated but *not* exposed as sliders (the task's tunable list did
not name these; they are fixed constants in `pen.ts` and `engine.ts`,
called out in comments there): the pen's start radius factor (0.35) and
start-taper blend (0.35 + 0.65 x arc/START_TAPER_LEN), the three end-taper
scales `[0.7, 0.45, 0.25]` and their 1.2px step, the 1..8px radius clamp,
and `ERASE_COOLDOWN_MS` (1200ms). If any of those change in `main.c`,
update the matching constant in `pen.ts` / `engine.ts` by hand.

## Serving through markup (annotation)

The dev server (`bun run server.ts`) uses Bun's HTML-import bundler to
transpile `src/*.ts` to browser JS on request, which is not something
`markup/serve.ts` does — it is a **plain, read-only static file server**
(see its own comment: "no change to the target site's repo, ever"), so it
cannot bundle TypeScript itself. To annotate this page with markup, build a
static copy first:

```
bun run build.ts                                          # writes dist/
bun ../markup/serve.ts --dir dist --port 5332 --site rp2350-amoled-1.8-emulator
```

The emulator is also registered as a fleet member in
`../markup/sites.json` (port 5332, pointed at `emulator/dist`), so
`bun ../markup/serve.ts` (no `--dir`, serving the whole fleet) picks it up
automatically once `dist/` exists.

Nothing in this page fights markup's overlay:

- Dragging the puck is scoped to `pointerdown` on the bezel element itself
  (`e.target === bezel`, `device.ts`), not a document-wide capture — it only
  fires when the plastic itself was grabbed, never when markup's own ink
  layer or a button is the target.
- The panel canvas uses `setPointerCapture` scoped to its own pointer id,
  standard drawing-surface behaviour, not a page-wide listener.
- There is no fixed overlay covering anything outside the device; the
  slider sidebar and top bar are normal in-flow chrome markup can draw over
  and annotate like any other page content.

## How this was verified

There is no hardware here to compare against, so verification is: does the
emulator itself run, and does the ported pipeline actually produce the
shape of output the firmware's comments describe (anti-aliased edges, a
solid core, MIN composition, dropouts recognised and counted).
`scripts/verify.ts` is a headless Puppeteer check (`bun run verify` /
`npm run verify`, using the same `puppeteer-core` + installed-Chrome
pattern as markup's own `scripts/shot.ts`):

1. boots the server on a scratch port,
2. loads the page and confirms `window.__engine` exists and the boot test
   pattern painted a non-trivial number of pixels (the render path works
   before any input),
3. drives a real synthetic mouse stroke across the panel via
   `page.mouse.move/down/up`,
4. reads back `engine.panel.gray` and asserts the stroke produced both
   fully-black core pixels and partially-covered (anti-aliased) pixels —
   the second is the one that would be missing if MIN composition had
   silently regressed to alpha blending, since alpha blending on a
   sub-Nyquist stroke tends to fully saturate the overlap.

Run it yourself: `cd emulator && bun run scripts/verify.ts`. It manages its
own server process and Chrome instance and cleans both up on exit. Windows
gotcha observed while writing this: the spawned dev server (HMR mode) can
keep its port bound for a while after `server.kill()` even with the
`taskkill /t /f` fallback in the `finally` block; the assertions above all
run and report before that happens, so it does not affect the result, only
the process's exit latency. If port 53309 is ever already in use on a
re-run, find and stop the stale listener (`netstat -ano | findstr 53309`,
then `taskkill /pid <pid> /f`) rather than changing the script's port.

Manual/visual verification (not automated, do this after any change to
`raster.ts` or `pen.ts`): open the page, draw a slow curve and a fast
flick, and look for (a) smooth anti-aliased edges with no banding at
segment joins, (b) width that thins on fast motion and widens on slow
motion, (c) both ends tapering to a point. Toggle dropouts/strays off to
see the "clean" pipeline, then back on to see why the debounce and confirm
constants exist.
