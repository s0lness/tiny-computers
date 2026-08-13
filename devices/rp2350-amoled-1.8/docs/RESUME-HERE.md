# Resume here, updated 2026-08-14

Read this first if you are picking this device up cold. It says what we are
building, where today stopped, what physical state the hardware is in, and the
exact first three actions.

**Nothing below about the hardware has changed since it was written.** The
device is still off, the `copy_to_ram` fix is still unvalidated, and the
acceptance test is still owed. What happened since was all work that could be
done without touching a board, and it is summarised in "What moved while the
device was off" near the end.

## The objective

A pocket-sized touchscreen puck, built as a toy for a young child. It holds a
handful of small apps she can play with alone, away from anyone who knows how
it works. Four requirements, in the owner's words: **blazing fast, very
reactive, very instinctive, very simple to use.**

"Done" for this device is not a feature list. It is: **she can pick it up,
press a button, and something happens.** Everything below is subordinate to
that, which is why one bug outranked a day of feature work.

Design axioms already settled, do not relitigate them without the owner:
- one binary, all apps, switching is a function call (decision 0002)
- each app is an object and refers to nothing outside itself: no battery
  indicator, no clock, no app name, no back arrow (decision 0002 section 4b)
- the emulator runs the real C compiled to wasm, never a reimplementation
  (decision 0003)

## What works today

- **The apps**: stopwatch, sketchpad, countdown timer, and a menu of three
  hand-drawn icons. All render correctly, verified on the panel.
- **The runtime**: app switching in 15ms (a reboot was 182ms).
- **The timer**: activity-ring style, 5 second steps up to 2 minutes then
  coarser, chime synthesised in C.
- **The chime**: four rising pentatonic notes. The owner approved it.
- **The emulator**: runs the real firmware compiled to WebAssembly, plays the
  same chime through WebAudio, on 127.0.0.1:5330.
- **The tooling**: `tools/dev.ts` drives the board over USB (screenshot,
  touch, drags, buttons, the chord, app switching); `tools/cam.py`
  photographs the panel, selecting the camera by name.

## What does not work

**A real button press kills core1**, which owns every sensor, so the device
then responds to nothing while continuing to render a perfect screen. This is
the only thing standing between the current state and a usable toy.

Root cause and full analysis: `docs/decisions/0005-rca-core1-dies-on-first-button.md`.
Why it took a day, and what each instrument got wrong:
`docs/decisions/0004-the-day-the-instruments-lied.md`. **Read both before
touching anything**, particularly 0004: every measurement we trusted was
wrong in a different way, and the same traps are still in the tree.

In one line: core0 borrows the flash chip select every 50ms to read BOOT,
core1 was executing from that same flash, and a fetch during a borrow returns
garbage. Only rarely-executed code is exposed, because cached code is never
re-fetched, and the PMIC's clear-write runs only when a real button event has
been latched.

## The physical state of the device, right now

**Powered fully OFF.** Rails down, panel dark, charge LED lit, no USB
enumeration, no COM port. This is a genuine PMIC power-off, not the frozen
screen.

**It needs one physical action: hold PWR for about a second.** If that does
not work, unplug and replug USB, which gives the PMIC a VBUS edge.

Nothing can be flashed, measured or validated until that happens. No agent
can do it.

## The first three actions tomorrow

1. **Power it on**, then confirm: a COM port exists and `bun tools/dev.ts app`
   answers.
2. **Flash the control build** `firmware/build/main-control-xip-selftest.uf2`
   (old flash layout, plus a self-test that issues the fatal transaction every
   two seconds). **Expect core1 to die within seconds with nobody touching
   it.** That is the positive control: it proves the mechanism rather than
   assuming it.
3. **Flash the fix** `firmware/build/main-fix-ram-selftest.uf2` (the whole
   binary runs from RAM). Expect it to survive the same hammering.

Then the acceptance test, which has not moved all day and must not be
softened: **press PWR, the stopwatch starts. Press again, it stops. Ten
times, over several minutes.** Surviving a press without acting on it does not
count.

## Before shipping, three things must be undone

- `PMIC_WRITE_SELFTEST` is currently `1` in `sensors.c`. It must go back to 0.
- `sound_init()` is commented out in `runtime.c`. The chime was cleared of
  blame and the owner likes it; re-enable it after the fix validates.
- The temporary diagnostics (phase markers, the i2c live dump, the stack
  canary) can go. **The permanent `core1=<n>/s` in the profiler line must
  stay.** It was deleted once during this investigation and the bug instantly
  became invisible again.

## What moved while the device was off

All of it verifiable without a board, and all of it pushed.

- **The apps were fuzzed through the emulator**, roughly 116,000 ticks plus
  directed repros. One real defect found and fixed: the timer destroyed a PWR
  short-press that arrived in the same tick as a BOOT release, because
  `sensors_key_take()` is read-and-clear and `timer_tick()` returned early.
  `chrono.c` already handled the same collision correctly. Write-up in
  `emulator/docs/findings-app-fuzzing.md`.
- **The push path was checked and held.** No pixel changed outside the union
  of a tick's pushed rectangles, in any app. That is the class of bug that
  looks right in the emulator and is broken on the panel, and it is absent.
  Recorded as a result, not as silence.
- **The invariant checker is built and wired into the build.** It fails on
  `e11fafc` and passes on `4869d00`, so it would have caught the core1 death
  the day it was introduced. Mutation testing caught a real bug in the checker
  itself: pico-sdk links with `--wrap`, so forbidding `printf` by name
  forbade a symbol that never exists in the image. Decisions 0006 and 0007.
- **The emulator was extracted** to `~/projects/puck` and hardened. An
  adversarial pass found it could die silently and go on looking alive, which
  is the same failure mode as decision 0004, in the instrument built to answer
  it. Fixed, with a permanent suite of hostile firmware modules driven through
  real headless Chrome. Not yet public.

## Open, not blocking

- `puck` has no git remote yet. That is deliberate: it goes public once the
  work in flight settles.
- The owner wants the icons redrawn as real ink strokes rather than assembled
  shapes, in the manner of tldraw and Ink and Switch. The capture rig for this
  already exists (`tools/capture-server.ts` serves a real tldraw; the
  reMarkable path also delivers vectors). They tried to send a sketch and it
  did not arrive: the daemon logged `send_empty records=0`, so the pen capture
  needs looking at.
- The power-off gesture works but the shutdown register write has never been
  confirmed to actually cut power. It fails safe.
- `store/README.md` still describes the retired two-slot bootloader as if it
  were current.
