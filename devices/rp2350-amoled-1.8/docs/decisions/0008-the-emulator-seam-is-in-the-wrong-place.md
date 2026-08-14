# 0008: The emulator's seam is in the wrong place, and its input is too kind

Date: 2026-08-14
Status: proposed

## What got through

The sketchpad did not draw. The owner had been reporting it since the first
day, and it survived every layer of testing this project has: a fuzzing pass
of 116,000 ticks, an adversarial pass with hostile firmware modules, an
invariant checker over the linked image, and a differential harness.

Measured on hardware, the cause turned out to be two things stacked:

- The FT3168's touch detection threshold is never written. `FT3168_Init()`
  sets the power mode and nothing else, so the panel runs on a power-on
  default that needs a hard press.
- The real controller repeats the same coordinates about sixty times before
  producing a new one, and drops contact 798 times in a single session. The
  sketchpad's stray rejection, which requires a second report within 25px to
  confirm a stroke, therefore discarded **387 of 401 stroke starts**. A 3.5
  percent success rate.

Neither half was reachable from the emulator. That is the thing worth fixing,
because the next bug of this shape will hide in the same place.

## Why it was unreachable, precisely

`emulator/wasm/emu_shim.c` **implements `sensors.h` in full**. It provides the
touch queue, the key bits, the BOOT state and the shake counter.

So the seam between "shared source" and "written twice" sits at `sensors.h`:

| Above the seam, one source | Below the seam, written twice |
| --- | --- |
| every app | core1's loop |
| `runtime_core.c` | the FT3168 driver and its registers |
| `gfx.c` and the 8-pixel rule | the two-ring timestamp merge |
| | the `Touch_INT_PIN` gating |
| | the PMIC key decoding |

The first half of the bug (a register nobody writes) lives below the seam and
is invisible there by construction. `emu_shim.c` has no registers to forget.

**This is the same argument decision 0003 used to kill the TypeScript port,
reappearing one level down.** We removed the reimplementation at the app layer
and left one at the sensor layer, where it drifted exactly as predicted. Two
implementations of one thing agree on the day the second is written.

The second half is worse, because it was reachable and we missed it anyway.

## The part that is not about the seam at all

`sketch.c`'s stray rejection is **above** the seam. It is the same source in
both targets. The emulator ran it thousands of times and it behaved perfectly.

It behaved perfectly because nothing ever fed it a controller as bad as the
real one. `src/touchsim.ts` can generate dropouts and stray contacts, at
configurable rates, and that is precisely why it was kept when the rest of the
old port was thrown away. Its defaults were chosen by feel, before anyone had
measured the real thing.

So the emulator had the mechanism and pointed it at gentle weather.

**A filter that never fires in testing is not tested.** On hardware the stray
rejection discarded 97 percent of input. In the emulator it discarded
essentially none. That gap is a number, it was always computable, and nobody
computed it.

## Three changes

### 1. Move the seam down to the register boundary

Compile `sensors.c` itself for wasm, and fake only what actually touches
silicon: the i2c read and write, `gpio_get`, and the timer. Everything above
that becomes one source again: the ring merge, the INT gating, the recovery
path, the key decoding.

The i2c and GPIO calls are a far narrower and far more stable interface than
`sensors.h`, and they are the real hardware boundary rather than a convenient
software one. A fake i2c device can then be given the FT3168's actual
behaviour, including a register map, which means a register nobody writes
becomes visible in the emulator.

This does not make the emulator run core1 as a second core, and it should not
try. Core1's loop can run as a stepped function inside the single-threaded
tick. Concurrency stays a hardware question, as it already is.

### 2. Record reality, replay it

The device already streams over USB. Add a capture of the raw sensor stream
during real use: timestamps, finger counts, coordinates, INT levels, key bits.
Replay it in the emulator through the same path.

That converts the emulator's touch input from "a clean mouse drag" into "what
actually happened on the panel while a child was playing with it". The harness
built today already replays traces and compares frames, so this is mostly
plumbing to an existing mechanism.

A recorded session is also the only honest way to set `touchsim`'s defaults.
The first numbers to load are the ones measured here: roughly 60 repeated
reports per new position, and dropouts at the rate a real session shows.

### 3. Every path that discards input must be reachable, and counted

The sketchpad already counts `glitches`, `dropouts`, `strays` and `splits`.
Those counters existed and were never read until a hardware diagnostic printed
them.

Make them assertable from the emulator, and write tests that put realistic
input in one end and assert the discard rates that come out. A test suite where
a rejection rule can only ever be dormant proves the rule compiles.

This generalises past touch. Any rule that throws input away is a rule that can
throw away everything, silently, while every screen still renders correctly.

## What stays out of reach, and must not be pretended otherwise

Timing, the panel, and the analog reality of a fingertip on glass. Decision
0003 already says this and it remains true. The change here does not make the
emulator able to answer "does it feel responsive" or "is the threshold right
for a five year old's finger". Those are answered by a person holding the
device, and the diagnostic counters are how that answer gets a number attached.

## Cost, honestly

Moving the seam means `sensors.c` has to compile without the pico-sdk, which
means its hardware calls need a thin abstraction that does not exist yet. That
is real work and it will make `sensors.c` slightly less direct to read.

The alternative is keeping a second implementation of the sensor layer and
promising to keep it in sync. That promise has now been broken once, in the
exact way this project already wrote down and swore off.
