# 0002: A single-binary runtime with immediate-mode apps

Date: 2026-08-11
Status: proposed (revised after review)

## What this device is for

A pocket-sized touchscreen puck, built as a toy for a child. It holds a handful
of small apps: a stopwatch, a drawing pad, and later a bubble level, a dice, a
speedometer. The owner adds apps over USB occasionally. A child uses it daily,
away from anyone who knows how it works.

Four requirements, in the owner's words: **blazing fast, very reactive, very
instinctive, very simple to use.**

That last sentence, "away from anyone who knows how it works", turns out to
drive more of this design than the four requirements do. It is why there is a
crash-recovery section, and why destructive actions are interruptible.

It used to also say "and why the menu is touched rather than chorded". That is
no longer true: section 4b establishes that nothing belonging to the runtime
may appear inside an app, which removes the on-screen affordance and leaves the
chord as the only way in. The requirement did not change, the answer to it did.

## Decision

### 1. One binary, an app table, and switching is a function call

Apps compile into a single image. The runtime owns the screen and the input; an
app is a struct of callbacks in a table. Switching tears down one app's state
and renders the next.

| Path | Cost | Why |
|---|---|---|
| function call | **under one frame** | repaint only |
| slot switch, warm | ~20ms (untested) | reboot, skipping panel init |
| slot switch, cold | **182ms** | reboot plus panel init |

182ms is already the result of replacing the vendor's guessed delays with the
SH8601 datasheet's real ones; it was 775ms. Of what remains, 150ms is the
sleep-out the panel physically requires. A reboot cannot beat a function call.

**App state lives in a runtime-owned arena, and the arena has a size.** With
every app in one image, static buffers coexist at link time: ten apps each
keeping "a bit" of state will eat the budget silently and the linker will not
warn at 99KB, it will fail at 521. Apps get a fixed arena, reinitialised on
switch. **Budget: 64KB.** An app that cannot fit is a design problem, not a
budget problem.

### 2. Two UI primitives, because one does not fit

**Widgets, for declarative screens.** The app declares its screen every tick;
the runtime remembers what each widget produced last tick and repaints only
what changed. Diffing is per widget, not per pixel: a stopwatch repainting six
digit cells at 10ms costs about 90 microseconds of push, which is affordable
and much simpler than cell-level tracking.

```c
static void chrono_render(ui_t *ui) {
    ui_number(ui, UI_BIG, elapsed_cs);
    ui_hint(ui, running ? "STOP" : "START");
}
```

**A canvas surface, for apps whose framebuffer is the document.** The sketchpad
cannot declare its screen: there are no retained vectors, ink is MIN-composited
destructively, and re-rasterising history is impossible by design. It asks the
runtime for a drawing region, draws into it, and reports dirty rectangles.

This split is not a compromise, it is where the constraints actually live. The
three invariants worth centralising are all in the **push path**, not the widget
layer:

- every pushed window's row length must be a multiple of 8 pixels (decision 0001);
- the framebuffer is byte-swapped RGB565;
- landscape apps draw through a rotation, portrait ones do not.

The runtime owns the framebuffer and the push, so both primitives get that
safety and no app can violate it.

Animation and transitions need no special case: render is a function of time.

**Rejected: a retained scene graph.** Objects with properties and invalidation
trees are the textbook answer and are wrong at this size, where screens are a
handful of elements that change every frame anyway.

### 3. Sensors are signals, published by core 1

Core 1 owns `i2c1` exclusively and samples touch, the IMU and the PMIC into
lock-free queues. Core 0 renders. This was a measurement, not a preference: the
touch I2C read costs about 695 microseconds and was roughly 98 percent of frame
time, while rasterising took 5 to 95.

An app never talks to a chip; it reads `touch`, `accel`, `shake`, `buttons`.

**Core 0 must never touch `i2c1`.** The vendor demo guards the shared bus with a
non-atomic `while(lock); lock=1;` on a non-volatile flag, and concurrent access
corrupts transactions in ways that surface as unrelated failures elsewhere.

**The audio codec is on this bus too** (ES8311 at 0x18), so sound is core 1's
problem as well. See section 7.

### 4. Input: touch first, buttons as backup

The device's best input is the touchscreen. An earlier draft of this document
put the menu behind a 1.5 second hold plus a two-button chord, navigated by
buttons. That was written during a week when touch was the broken subsystem and
buttons were the trustworthy one, and it is the wrong design for a child with
small hands.

| Action | Primary | Backup |
|---|---|---|
| open the menu | **BOOT and PWR held together, long press** | none |
| choose an app | **tap its picture** | BOOT / PWR to move, PWR short to launch |
| the app's action | tap, or PWR short | |

The menu shows pictures, not words: reading English should not be the entry fee.
Tapping a picture also resolves what "confirm" means, which the button-only
design never answered.

**Corrected 2026-08-13.** This table used to say the menu opened by tapping a
corner glyph, with the chord as backup, and this document's own opening
paragraph used to cite that as an example of touch-first design. Both are now
wrong, and the reason is section 4b: an affordance belonging to the runtime may
not appear inside an app. So the chord is not the backup, it is the only way
in, and the discoverability that the glyph was carrying has to be carried by
the physical object instead.

Everything else here still holds. Touch remains primary INSIDE the menu, which
is where the choosing happens and where a child's finger is the right
instrument.

**No modal state.** If an app can enter a condition where the same input does
something different, with nothing on screen saying so, that is a defect however
fast it renders.

### 4b. Each app is an object, and refers to nothing outside itself

The owner's words, and it is an axiom rather than a preference: *chaque app est
un objet et ne renvoie a rien en dehors de lui.*

A stopwatch is a stopwatch. It is not a stopwatch sitting inside a system. So
nothing belonging to the runtime may appear on an app's screen, ever.

What this forbids, stated concretely because the violation will always arrive
as a reasonable-sounding addition:

- **no battery indicator.** This is the one someone will add. It is the most
  defensible extradiegetic element there is, and it is still forbidden.
- no clock, no app name, no page dots, no back arrow
- no menu affordance. A corner glyph meaning "there are other apps" was
  proposed, drawn, and rejected on exactly this ground: it says something
  about the device rather than about the app, and it would have sat on the
  sketchpad's drawing surface forever.
- no notification, no toast, no status of any kind

The menu is the one exception and only because it is not an app: it is the
drawer, and naming apps is its entire job.

**What this costs, and it is a real cost.** Discoverability has to leave the
screen. The only way into the menu is a gesture (both buttons held, long
press), and nothing on the device says so.

Two things carry it instead. The object teaches the gesture: a mark on the
case beside the two buttons, which is diegetic to the physical thing and
pollutes no app. And the gesture has to be findable by accident, which this
one plausibly is: a child fiddling with a two-button object will hold both of
them long before an adult reasons their way to it. That makes the chord better
suited to the actual user than to the person who built it.

The consequence for the firmware is that the chord must be forgiving. "Both
buttons held together" and "BOOT down at the exact instant PWR's long-press
verdict fires" are not the same gesture, and the difference is felt by small
hands. Verify which one is implemented rather than assuming.

**Raise the PMIC's hard power-off threshold** (register `0x27`, default 6s).
Children hold buttons, and a held PWR is currently 4.5 seconds away from an
unannounced power cut.

**Done, 2026-08-13.** `sensors_init()` (`firmware/runtime/sensors.c`,
`pmic_raise_poweroff_threshold()`) now raises `OFFLEVEL` (register `0x27`
bits 3:2) from its 6s default to `11b` = 10s, the field's maximum, on every
boot, read back and printed once to confirm the write took. `IRQLEVEL` (bits
5:4, the 1.5s long-press verdict the menu gesture waits on) is left at its
default: read-modify-write only touches `OFFLEVEL`'s two bits. Margin between
the gesture and the power cut is now 8.5s instead of 4.5s. AGENTS.md's
recovery procedure was updated to match (hold PWR 12s, not 10s, since the
threshold it is timed against moved from 6s to 10s).

### 5. Destructive actions are per-app and interruptible

**Shake is not a global reset.** Shake-to-erase is charming in the sketchpad
because it is the Etch A Sketch, an affordance older than the child. Promoting
it to a universal destructive verb generalises from one app, and it destroys
exactly the two things a child carries across a room to show someone: the
drawing, and the number on the stopwatch.

- Shake is **opt-in per app**, offered only where reset is the app's identity.
- The stopwatch does not have it. BOOT already resets it.
- Any destructive action must be **interruptible**, never a single threshold
  crossing. The erase wipe already animates over 16 bands; a touch during the
  wipe aborts the remaining ones. Sustained shaking is required, so a social
  shake cannot fire it.

True undo would need a second 330KB buffer that does not exist. Interruptibility
is the affordable substitute.

### 6. It must recover on its own

The manual recovery for a hung app is a ten-second button ritual. That procedure
is for the owner. The child does not have it and the owner will not be in the
room.

- A **watchdog** fed by the runtime tick, rebooting into the menu on hang. The
  182ms cold boot makes this nearly invisible.
- The partition machinery in `store/` is repurposed as a **golden fallback
  image**: a bootloader that counts failed boots and chains a known-good image.
  This replaces its earlier rationale, which contradicted section 8 of this
  document: it was kept for installs without a rebuild, which this design
  rejects. Crash recovery is what it is actually worth.

### 7. Sound is reserved now, not retrofitted

The ES8311 codec and speaker are unused. For a child, sound is half the toy: the
stopwatch beep, the dice clatter, the erase whoosh.

It is reserved now rather than added later because the codec sits on `i2c1`, so
its configuration belongs to core 1, which is the most delicate part of this
architecture. Retrofitting audio means reopening exactly the code least worth
reopening. The runtime gets a sound service (play a sample by id from the 16MB
flash) on the same signals model as the sensors.

**Done, 2026-08-13, and two predictions above turned out wrong in ways worth
recording rather than editing away.** `firmware/runtime/sound.h`/`sound.c`
give apps `sound_play(id)`/`sound_stop()`, called by the timer's alarm
(`apps/timer.c`).

- **Not core 1's problem, in the end.** The codec's control registers are
  configured exactly once, from core0, in a new `sound_init()` (`runtime.c`)
  called right after `sensors_init()` and before `sensors_start()` - the same
  window where PMIC/touch/IMU are already brought up single-threaded. Once
  set, the codec is left unmuted at a fixed volume permanently, so
  `sound_play()`/`sound_stop()` (called at arbitrary times afterward) never
  touch `i2c1` at all: they only change whether the already-running I2S
  stream carries the chime or silence. So sound never had to reopen
  sensors.c/sensors.h, the file this section predicted would be "the code
  least worth reopening" - it was not reopened.
- **Synthesised, not stored.** "Play a sample by id from the 16MB flash"
  assumed PCM in flash/RAM; a phrase of stored audio at a usable quality
  costs tens of KB, more than this document's entire SRAM "headroom" line
  (section 10's memory table) on its own. `sound_synth.c` computes the chime
  sample-by-sample instead (sine plus an exponential-decay envelope), which
  costs CPU during the ~30s the alarm actually rings and no flash or RAM for
  the waveform itself - see that file for the reasoning and the actual
  fixed sound (a four-note major-pentatonic rising motif, C5-E5-G5-C6).

I2S itself is driven by PIO (the RP2350 has no dedicated I2S peripheral) on
`pio1`, not `pio0`: the display's own QSPI PIO program uses `pio0` without
ever registering its claim with the SDK, so trusting `pio0`'s claim state
for a "free" state machine would risk silently colliding with the display -
see `sound.c`'s header comment.

### 8. Rejected: apps as data, or a scripting layer

Adding apps without a rebuild contradicts requirement one: an interpreter
between a sensor and a pixel is the latency this design exists to remove.
Recompiling costs the owner a minute and the child nothing.

### 9. Persistence, decided

- **Across sleep**, the framebuffer survives for free: SRAM stays powered.
- **Across power-off**, the sketch is saved to flash on long idle and on low
  battery. A drawing that vanishes is the shake problem by another route.
- **The stopwatch forgets, and that is correct.**

### 10. Power management

The largest hole in the first draft, which asserted a stance on redraw policy
with no power data at all while spinning both cores at 60Hz driving an AMOLED.

Required before this is settled: **one real current measurement**, idle and
drawing. Everything below is a policy to be checked against it, not a
conclusion.

- Dim on short idle, sleep on long idle, wake on touch or button.
- Sleep must not cost the panel's 150ms sleep-out on every wake if it can be
  avoided; dimming does not, which is why dim comes first.
- Battery gauge and charge state from the AXP2101, surfaced to the runtime.
- Low battery saves the sketch before it dies.
- AMOLED burn-in is real (see `AGENTS.md`); anything static needs to move,
  dim, or sleep.

## The apps, and what each one proves

The app list is not a wish list: each early app is chosen to exercise a
different part of the runtime, so the abstractions are shaped by real use
rather than by guesswork.

| App | Exercises | Input |
|---|---|---|
| stopwatch | widgets, per-widget diffing, precise timing | PWR start/stop, BOOT reset |
| sketchpad | the canvas surface, dirty rects, opt-in shake | touch |
| **timer** | **setting a value by touch, and the alarm path: sound, animation, and an interruption the child must be able to dismiss** | drag to set, PWR start/pause |
| bubble level | sensor signals at frame rate | none, it just responds |
| dice | shake as a non-destructive verb | shake |

### The timer, in detail

A countdown is the first app that has to ask the child for a *value*, and the
first that interrupts her. Both are new problems.

**Setting it: drag a ring, do not type a number.** A child cannot be asked to
enter minutes and seconds on two buttons, and should not have to read a form.
The affordance already exists in the world and is older than she is: the
kitchen egg timer you twist. A ring that fills as the finger drags around it,
with the time shown large in the middle, is the same gesture and needs no
explanation. Snap to sensible steps (30s below 5 minutes, a minute above) so a
small imprecise finger still lands somewhere round.

**Running: the ring empties.** The remaining time is a shrinking arc, legible
across a room without reading the digits, which matters because a child will
glance at it from wherever she has left it. PWR pauses and resumes. BOOT resets
to the value that was set, not to zero, so "again" is one press.

**The alarm is an interruption, and interruptions on a toy have rules.** It must
be noticeable without being frightening: the screen animates, and it beeps once
the sound service exists (section 7). It must stop on *any* input, not a
specific one, because a child reaching for a beeping object should not have to
remember which button. And it must stop by itself after a while rather than
beeping until the battery dies.

This app is why the sound service is reserved now rather than retrofitted: a
timer with no sound is a timer that has to be watched, which defeats it.

**Corrected 2026-08-13, sound bring-up.** Two clarifications the owner made
once section 7's sound service actually existed and this section's
"any input" had to become real code, both tightening rather than changing
the requirement above:

- "Any input" is literally any of the three input kinds this device has:
  a button, a touch, OR a shake. Shake is normally opt-in per app precisely
  so it cannot become a universal destructive verb (section 5) - the timer
  now opts in, but `firmware/apps/timer.c`'s `handle_alarm()` is the ONLY
  place it reads that signal; outside the alarm a shake still does nothing
  to this app, exactly as before.
- Dismissal (any of the three) now silences the sound AND the flash
  immediately, and returns to SETTING at **00:00**, not to the value that
  was set - simpler than this section's original phrasing, which mirrored
  BOOT's "reset to the value that was set" behaviour without saying so
  explicitly. BOOT, pressed while SETTING shows a fresh 00:00, still recalls
  the last value in one press (`timer_state_t.lastSetTicks`), so "again is
  one press" survives as a deliberate recall action rather than as the dial
  staying pre-loaded through the alarm itself.

See `docs/decisions/`'s own convention (further down this section, and in
the runtime rewrite this document is titled after): record the correction
with its date rather than silently editing the original sentence away.

**Corrected 2026-08-14, the coil.** "Snap to sensible steps (30s below 5
minutes, a minute above)" above described the tiered ring; it no longer
holds. The owner: dragging near the fast end of that ring bought seconds,
dragging near the slow end bought minutes, and nothing on the dial itself
told a child which zone her finger was in - unpredictable by design for
someone who cannot yet read MM:SS. The replacement is a flat 5-second step
everywhere, wound onto the dial like a coil: a full lap is a fixed length
of time (30 minutes, after a same-day revision from an initial 10), the
60-minute ceiling is two laps, each lap is its own concentric band so the
lap count is visible (Apple-activity-rings style, winding inward so the
outer diameter never grows), and dragging past twelve o'clock adds or
removes a lap via continuous unwrapped angle tracking rather than a
snapshot. Pointing directly still works as the fast gesture; at this lap
length it lands in the right neighbourhood rather than on one exact
5-second tick, and continuing to drag is what resolves the exact value -
see `firmware/apps/timer.c`'s own header for the full derivation, the
fine-grained-drag jitter/hysteresis reasoning, and why. The same change
also added a shake-to-clear reaction, scoped to SETTING and a paused
countdown (both "the dial is editable"); a running countdown is
deliberately left alone, so a knock against the table cannot destroy time
actually being counted.

End-to-end, phrased as a child would notice them rather than as frame counts.

| Path | Budget | Notes |
|---|---|---|
| ink trails the finger | under 8 px at normal drawing speed | the felt lag is the smoothing constant, not the frame rate |
| touch report to pixel | 1 frame | measure the real rate; the driver now asks for 100Hz and the earlier 60Hz figure is stale |
| gesture to menu visible | under 250ms | budget the gesture, not the render |
| app switch | 1 frame | tear down, render, push |
| wake to lit | under 250ms | the one a child reads as "it's broken" |
| cold start | under 250ms | 182ms today, 150ms of it mandated by the panel |

Memory, of 520KB SRAM:

| | |
|---|---|
| framebuffer, shared | 330KB |
| app arena | 64KB |
| runtime | under 40KB |
| headroom | keep 80KB |

## Corrections to the first draft

Recorded because the errors are instructive:

- The FT3168 report rate was quoted as a 60Hz floor. The firmware already asks
  for 100Hz via register `0x88`, and a comparable project reports that this
  register does nothing. Measure it rather than quoting either.
- "The IMU outruns the display" was false as built: `IMU_POLL_MS` is 20, slower
  than a frame.
- "An app cannot invent its own dialect" holds for buttons only. Apps see raw
  touch, necessarily, so touch conventions remain a matter of discipline.
- Event-driven redraw was dismissed on power grounds with no power data.

## Open questions

- Whether TE sync (GPIO16) is worth its complexity before an app visibly tears.
- What "reset" means for an app with no obvious zero state. The runtime should
  not offer the gesture rather than do something surprising.
- Whether the app arena should be a union of per-app structs, checked at compile
  time, rather than a byte array with a runtime bound.
