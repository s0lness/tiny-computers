# 0012: One orientation signal, published before the apps that need it

Date: 2026-08-15
Status: accepted and built, except the axis mapping, which is a hypothesis
until a two-minute ritual is run on hardware

Decision [0010](0010-every-instrument-read-upstream-of-its-bug.md) ends with
one instruction: "Publish the orientation signal and give the emulator its
tilt input, tonight, before the level, tilt-a-ball and compass are started."
Decision [0011](0011-what-this-board-can-actually-do.md), written the same
night, settles what those apps can be: no compass, but a needle that points
downhill, a ball in a bowl, a spirit level. Both land on the same missing
piece, and this is it.

Before this, the QMI8658 was read for exactly one purpose, detecting a shake
for the sketchpad's erase, and `app_frame_t` carried no gravity at all. Three
apps written the same week would have grown three private IMU shims. That is
the written-twice seam decisions 0003 and 0008 already paid to diagnose, and
the value here is not the vector: **it is that there is one of it.**

## What was built

| | |
|---|---|
| `firmware/runtime/tilt.h` / `.c` | the signal: filter, axis mapping, up-edge hysteresis. Portable, compiled into the board image AND into `emu.wasm`, like `runtime_core.c` and `sound_synth.c` |
| `sensors.c` (core1) | feeds it from the IMU read it was already doing for shake. No new i2c traffic |
| `app_frame_t.tilt` | what an app reads, in the app's own drawing space |
| `emu_sensor_vector()` | the emulator's tilt input, and the page control built from the device's own declaration |
| `emu_tilt()` | a test oracle that reads at the app boundary |
| `devlink TILT` / `bun tools/dev.ts tilt` | the ritual instrument, the only check the axis mapping can ever have |

## The representation, and what it is not

**A gravity vector in g, in the app's own coordinate space, plus two derived
fields.** `+x` right, `+y` down the screen as the app drew it, `+z` into the
glass. Flat on a table is `(0, 0, 1)`; upright with the top edge up is
`(0, 1, 0)`.

Three consumers were weighed, from 0010's list, and the vector is the only
form none of them has to undo:

- the **spirit level** wants how far from flat and in which direction: that is
  the in-plane part of this vector, whose length is the tilt and whose
  direction is downhill;
- the **rolling ball** wants an acceleration to integrate: `(gx, gy)` in g,
  scaled by whatever pixels-per-second-squared the game picks;
- the **orientation-aware clock** wants which of four ways is up: published as
  `up`, because the hysteresis that makes it stable is a policy decision (see
  below), not something three apps should each invent.

Rejected, each because it pushes work into a consumer that did not ask for it:

- **an angle and a direction.** Perfect for the level, and it forces the ball
  to run a sin and a cos every tick to recover the vector it wanted. Kept as
  a derived field (`tiltDeg`), not as the representation.
- **raw device-axis g.** Every consumer would then carry its own copy of the
  device-to-screen mapping, which is the single piece of this that no software
  oracle can check. One wrong copy is a level that leans the wrong way and
  passes every test this project can ever build.
- **four-way orientation only.** Enough for the clock, useless for the other
  two.
- **a unit "down" direction with the magnitude thrown away.** Cheaper, and it
  destroys the one thing the ball needs near flat: a small tilt must produce a
  small acceleration.

Two smaller decisions inside that, both of which exist to keep trigonometry
out of app code:

**The runtime rotates it into landscape apps' own space** (`tilt_for_app()` in
`runtime_core.c`), exactly as `gfx_land_rect()` already rotates their
rectangles. Otherwise every landscape app rotates gravity by hand, and the
first one to get it backwards ships a bug no automated check can see.

**`up` holds rather than blanking** while the device is too flat to have an
answer, so a clock laid on a table keeps its orientation instead of
flickering. Thresholds: an in-plane component of at least 0.35g (about 20
degrees off flat) before the answer may change at all, and one axis must beat
the other by 1.3x, which leaves a 15 degree dead band around each diagonal.

## Filtering: one signal, filtered, 150ms

An accelerometer in a child's hand is never still. If every app filtered its
own way, the same hardware would feel different in each, which is worse than
any single choice. So the published signal is filtered, and there is
deliberately **no raw vector on the app-facing side**: offering both is
offering the divergence back, and the app that picked "raw, for
responsiveness" would be shipping the jitter.

One-pole exponential, time constant **150ms**:

- at the board's 20ms IMU cadence that is a step of exactly 0.125 toward each
  new sample;
- corner frequency 1.06Hz. Deliberate tilting is under 1Hz; hand tremor at 5Hz
  and above comes through about 5x smaller, broadband sensor noise about 4x;
- the cost, stated rather than hidden: **the signal lags the hand.** A step
  reaches 63 percent in 150ms and 95 percent in 450ms. Too little and a
  bubble buzzes; too much and it swims behind the hand.

The filter is stepped from elapsed time, not from a per-sample constant, so
the board (a regular 20ms on core1) and the emulator (whatever the browser
manages) settle at the same speed. A filter whose time constant depended on
the caller's cadence would be 0010's "speed must not depend on push cost"
trap hiding inside a sensor. The headless test asserts this directly: the
same 150ms cut into 3 ticks lands within 0.0005 of the same 150ms cut into 15.

**150ms has not been judged by a hand.** It is one constant, in one place,
and the first person to hold a spirit level on the real board should change it
if it feels wrong.

## The axis mapping is the one thing nothing here can check

`device_to_panel()` in `tilt.c` converts the QMI8658's own axes to the panel's.
**Which way the part is mounted on this PCB is not recorded anywhere in this
repository**: the schematic gives pins, not footprint rotation, and the vendor
demo only prints numbers. The mapping shipped is a hypothesis (identity,
chosen because it has the fewest independent ways to be wrong, so the ritual
either confirms all of it or produces one obvious total correction).

No test can falsify it, on either target, because no software oracle knows
which way is up. Only hands do. `tilt.h` carries the five-pose ritual;
`bun tools/dev.ts tilt` prints the two columns it compares (what the part
reported, and what the app was handed). It takes about two minutes with the
board awake, and it settles the question permanently.

This is written down here rather than left as a comment because it is the
highest-consequence unverified line in the change, and because 0010's rule
applies to it exactly: an instrument validates only what is upstream of where
it reads, and every instrument in this project reads upstream of the physical
case.

## What is owed

- **The ritual**, on hardware, and then delete the word HYPOTHESIS.
- **The filter constant**, judged by a hand rather than by a corner frequency.
- **A build-time rule that no app can reach the IMU.** Today nothing but
  `sensors.c` reads the part, and the headers say so in the places an author
  will look, but that is a convention, not a check. The invariant checker is
  built for exactly this (0010 proposes the same rule for flash writes): a
  reachability rule from the app roots, forbidding the i2c1 read helpers and
  every `QMI8658_*` symbol. It was not added tonight because it needs a linked
  ARM image to run against and the board is asleep, so the rule could not be
  watched failing, and 0010's own standard is that an instrument is trusted
  only after it has been seen reading something other than green.

## What no instrument here can see

Written before the code, per 0010's closing discipline:

- the axis mapping, per above;
- whether 150ms feels right in a hand. The emulator's gravity is perfectly
  still and exactly one g in length; a real accelerometer is neither;
- whether the part is level with the case. A couple of degrees of assembly
  tilt reads as a level that never quite centres, and nothing in software can
  tell that apart from an un-level table. If the owner reports it, the fix is
  a measured offset, not a bigger dead band;
- the staleness path (`TILT_STALE_MS`, which turns the signal invalid when the
  IMU goes quiet). `emu_tick()` submits a sample every frame by construction,
  so "the IMU went silent" cannot be reproduced in the emulator at all. On the
  board it is what makes an app show nothing rather than a confident lie.
