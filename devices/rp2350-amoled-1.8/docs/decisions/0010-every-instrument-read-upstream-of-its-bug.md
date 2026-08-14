# 0010: Every instrument read upstream of its bug

Date: 2026-08-15
Status: accepted as analysis; the builds and decisions it names are owed

Thirteen times in two days, this project shipped something that had passed its
tests and failed in a child's hands, or wrote a test that passed against a
defect a human saw at a glance. Ten were firmware bugs, two were tests that
lied, and the thirteenth arrived while this document was being written: the
pre-flash gate's first run found the palette whitening six framebuffer columns
it never pushes, so the erase happens in memory and never on the glass.

Thirteen is data. This document is after the law.

## The evidence, with one column the incident reports leave out

| What shipped broken | Where the instrument read | Where the bug lived |
|---|---|---|
| core1 died on the first press | injected input, downstream of core1 | core1's flash fetch |
| soaks called it stable | idle device, no fingers | a path only a finger runs |
| 387 of 401 strokes discarded | clean synthetic touch | this panel's real weather |
| palette rebooted the board | an emulator with no clock and no watchdog | render cost in microseconds |
| palette never opened | same clean touch | a still finger's repeated reports |
| animation froze without input | touch-sample arrival | the wall clock |
| animation advanced, never drew | state variables | the framebuffer |
| cells eroded each other | "both paths settle identically" | whether the settled picture is right |
| ink under the case | a framebuffer with no bezel | the plastic |
| tests ran a stale wasm | the previous binary | the source being edited |
| harness diffed 3 apps against 4 | an assumed build | the build actually flashed |
| animation test passed on a frozen screen | call counts | pixels |
| icon test passed on beads-on-a-string | size and ink metrics | the owner's eye |
| palette whitened unpushed columns | the framebuffer | the panel |

## The pattern, tested

The hypothesis this audit was given: almost every bug is an instrument
measuring something adjacent to the thing that matters. Calls instead of
pixels, intent instead of effect, a clean world instead of this panel, a
binary instead of the source.

That holds for all thirteen, and it is underspecified, because "adjacent" has
no direction and these failures all point the same way. State the chain a
change actually travels:

```
source -> binary -> calls -> framebuffer -> pushed window -> panel -> case -> a child's eye
```

and the input chain, which is the same thing reversed:

```
a finger -> the glass -> the FT3168's weather -> i2c1 -> core1 -> the ring -> the app
```

Every instrument reads at exactly one point on one of these chains, and it is
never wrong about that point. The stale-wasm tests were correct about the old
binary. The soaks were correct that an untouched device is stable. The
byte-identical framebuffer assertions were correct about the framebuffer,
including on the day the panel showed something else. What every instrument
does is take the rest of its chain on faith, and **all thirteen bugs lived in
the faith region: downstream of the reading point on the output chain,
upstream of the injection point on the input chain.** Devlink enters below
core1, so core1's death was invisible. Clean touch enters below the FT3168's
weather, so the stray filter never fired. The tests read at the framebuffer,
so the panel's six lit columns were invisible. Nothing read further down its
chain than the bug it missed. Not once, in thirteen.

So the law is directional: **an instrument validates only what is upstream of
where it reads, and every fix this project has made to an instrument has been
to move its reading point one link toward the child.** Calls became pixels.
The framebuffer is now becoming the panel (the gate's new clear-inside-push
rule). Clean touch became the measured profile. An assumed build became a
fingerprint. None of those moves was invented in advance; each was paid for
first.

That is the second finding, and the uncomfortable one: this project's
regression machinery is excellent and its anticipation machinery does not
exist. The invariant checker says so about itself ("it prevents regressions
of known hazards, it does not discover new ones") and the gate inherits the
same honesty. Every rule in both encodes a bug already paid for. All thirteen
entries above were first-of-class. The machinery cannot help with the
fourteenth, and the fourteenth is arriving tonight with eight new apps, four
of which need things this firmware has never done. The only leverage on a
first-of-class bug is to place the seams and oracles before the app is
written, which is the second half of this document.

One amendment before that. Decision 0008 located "the seam" at `sensors.h`
and proposed moving it down to the register boundary. That was right and
incomplete: it named the input chain's seam and said nothing about the output
chain's, which sits at `gfx_push`'s return instead of at the glass. The
thirteenth bug is that gap collecting its fee. 0008 should be read as one
instance of the law above, not as the location of the problem.

## What to build that the gate does not

The gate (tools/gate/) already covers: per-tick budgets, pixels changing
outside pushed rectangles, clears outside pushed rectangles, the 8-pixel
rule, the bezel band, animations that must advance without input,
byte-identical settling, realistic touch by default, stale-binary refusal,
and the build fingerprint. Nothing below duplicates that list. Ranked by what
each would have caught against what it costs.

### 1. A panel buffer in the emulator, and it becomes the assertion target

Keep two buffers: the framebuffer the firmware writes, and a panel buffer
updated only by pushed rectangles, masked by the bezel. Every test assertion
of record (the hashes, the sampled pixels, the settling comparisons) moves
from the framebuffer to the panel buffer.

The gate's clear-inside-push rule closes the specific hole the palette found.
This closes the class: any future divergence between memory and glass, in
either direction, fails every test that touches the region, instead of
needing its own rule after its own incident. It is 0008's "move the seam
down" applied to the output chain.

Would have caught: the whitened columns, and the "correct in the emulator,
stale on the panel" family the gate's README already names as four-times
shipped. Costs: an emulator harness change plus migrating the oracle in
sixteen test files, and the discipline that no new test ever asserts on the
framebuffer directly.

### 2. A contact sheet, because no metric confirms a picture

Every app, every settled state, rendered from the panel buffer into one PNG
grid under `preview/`, regenerated whenever the gate runs. The owner looks at
it. That is the whole tool.

The icon test that passed on beads-on-a-string is the honest limit of this
project's oracles: size and ink metrics catch an outlier and cannot confirm a
match, and decision 0009's rule about rulers is checked by no tool in this
tree (the gate says so itself). The settled-path erosion shipped the same
way: both settle paths agreed byte-for-byte on a wrong picture. The only
oracle for "is this the right picture" is an eye, so put the pictures where
the eye is, in one place, current, without a flash cycle.

Would have caught: the eroded cells, the beads icon, and it would have
shortened the advanced-but-never-drawn palette to the first glance. Costs: a
script and a minute of the owner's attention per batch, forever. What it
cannot do: judge feel, timing, or anything that moves.

### 3. An on-board effect ritual: the picture must change, the counters must move

A devlink script, run after every flash, that measures effects where the
board can show them:

- **The picture must change.** While the stopwatch runs, framebuffer hashes
  taken 500ms apart must differ. A frozen device and a waiting one render the
  same pixels (decision 0004's third trap), and the animation test that
  passed on a frozen screen counted calls; this counts the picture.
- **Every counter must be seen to move.** Each diagnostic (touch reads,
  key events, the liveness figure) gets provoked once through its selftest
  gate, the way `PMIC_WRITE_SELFTEST` already does for the fatal write shape,
  and a counter that cannot be made to move fails the ritual. The stack
  canary that reported 2048 of 2048 was believed because nobody had ever
  seen it read anything else; "touch reads=0" was dismissed because zero was
  its only observed value. An instrument is trusted only after it has been
  watched failing.
- **It ends by printing what only a thumb can test.** The honest gaps are
  already written down, scattered across `sensors.h`'s honesty requirements
  and `tools/README-devlink.md`: injection cannot test the PMIC decode, the
  chip-select read, the panel's own threshold. The ritual prints that list as
  its last output, so a green run hands the owner the exact residue his ten
  real presses exist to cover, instead of looking like completeness.

Would have caught: the frozen-screen test, the lying canary, and it shrinks
the class the first evidence row belongs to (it cannot close it: injection
still enters downstream of core1, and this document will not pretend
otherwise). Costs: a script, selftest gates on counters that lack one, and
about a minute per flash.

## Before the eight apps, not after

Eight apps are coming: tic-tac-toe, breakout, a spirit level, a clock, the
dinosaur, bowling, tilt-a-ball, possibly a compass. Four capabilities in that
list have never existed here, and by the law above, each arrives with a chain
nobody has placed an instrument on. This section is what breaks and what must
be decided first. These are the fourteenth bug's candidate homes.

### Orientation: the signal does not exist and no oracle knows which way is up

Three of the eight (level, tilt-a-ball, compass) are built on the
accelerometer, and today **apps have no accelerometer signal at all**:
`sensors.h` publishes shake and nothing else, and `app_frame_t` carries no
gravity vector. The emulator has no tilt input of any kind. So tonight's
authors will either block, or invent a per-app shim, which is the
written-twice seam of decision 0008 being recreated the same week it was
diagnosed.

Build first: a published gravity/orientation signal in `sensors.h` (core1
already polls the QMI8658 at 20Hz), its emulator counterpart fed through the
same path (a tilt control, plus recorded traces once the record-and-replay
plumbing exists), and **the axis convention decided once, against the case,
verified on the board with a one-off ritual**: hold the puck level, read the
vector, write the mapping into `sensors.h`'s comment. A spirit level with a
flipped axis passes every automated check this project can ever build,
because no oracle in software knows which way is up; only the calibration
ritual and the contact-sheet eye stand between that bug and the child.

And say it now rather than mid-app: **the QMI8658 is a six-axis part,
accelerometer and gyroscope, no magnetometer.** A compass that finds north
cannot be built from this board. Verify against the datasheet, then cut it or
redefine it (a which-way-is-down toy is buildable; a heading is not). That
sentence costs nothing tonight and costs an evening if discovered by the
person writing the app.

### Persistent storage: the next decision-0005 lives here

High scores (breakout, dino, bowling), the sketch save already promised in
decision 0002 section 9, the clock's configuration. The first app that wants
one will call `flash_range_program`, and flash writes are the same substrate
as the bug that cost this project its worst day: the chip select `bootbtn.c`
borrows is the bus a write needs, a sector erase stalls the loop for tens of
milliseconds, and a power cut mid-erase (a child's finger on PWR, the exact
user this device has) corrupts the sector silently. Eight apps improvising
that individually is eight rolls of the dice.

Decide first: **one runtime-owned storage service, and apps never touch
flash.** A reserved region far from the image, named in the linker script;
two slots with a sequence number and a CRC so a torn write loses one
generation, never the store; writes only at defined moments (leave(), long
idle, low battery), never per-frame; and an invariant-checker rule, in the
tool built for exactly this, that no flash-write symbol is reachable from
app code. That rule is cheap (the machinery exists) and converts the whole
class from a debugging day into a build failure.

### The clock: the first app that is wrong while looking right

Three separate traps, all placeable now. The PCF85063 sits on `i2c1`, so it
is core1's chip and time is a published signal with a core0-to-core1 set
request, the `sensors_request_poweroff()` shape; an app reading the RTC
directly violates the ownership rule the day it is written. Setting the time
has no UI and should never get one: **devlink sets the RTC from the host
clock on every connection**, so the clock is right because the owner plugs
the device in sometimes, and the child never sees a setting screen (VRTC
survives soft power-off per the AXP2101, so it holds across the power cycle).
And the emulator's clock is the browser's, which is always right, so the
failure that matters (an unset or reset RTC showing a confident wrong time)
is invisible below the seam by construction; a frozen stopwatch at 00:00:00
taught this project that a wrong screen and a right screen can be identical,
and a wrong clock is that lesson as a product.

Separately: the clock is the first app designed to be left on for hours, and
decision 0002 section 10 (dim, sleep, burn-in) is still open with zero
current measurements. The clock app is blocked on at least the dim-on-idle
half of that decision, or it is a burn-in fixture.

### The game loop: speed must not depend on push cost

Breakout, the dinosaur and tilt-a-ball integrate motion every tick. Today's
tick rate is "as fast as the loop spins", pushes cost anywhere from 27
microseconds to 12 milliseconds, and the emulator's tick is the browser's,
so a game tuned in the emulator plays at a different speed on the board, and
at a different speed depending on how much it drew last frame. The fuzzing
pass already found `app_frame_t.dtMs` dead: written by the runtime, read by
nobody, its clamp protecting nothing. Games will copy whatever pattern
exists, so fix the pattern first: either delete `dtMs` or make it the
contract, and provide one fixed-timestep accumulator helper (integrate in
fixed quanta of `nowMs`, render once) that all three games share. Then put
the achieved tick rate per app in the profiler line, because the gate
honestly cannot see time and says so; "is breakout smooth" must be a number
from the board, not an impression from the emulator, which is decision
0003's forbidden question and will get asked anyway unless a number exists.

The dinosaur also scrolls its whole ground plane, which will meet the gate's
per-tick push budget honestly: when it does, the budget gets renegotiated in
the rules file with a written reason, not bumped to green.

### The menu breaks at six, not twelve

The menu is full-height columns over a 448px landscape width. At four apps a
column is 112px; a child's fingertip is about 75px. At six apps a column is
74px, already under the finger; at twelve it is 37px, half a finger. Tonight's
batch crosses the line, not some future one.

What replaces it is the owner's call, and the constraint set is already
written in AGENTS.md: about four finger-widths across and six down, targets
forgiving in at least one dimension, pictures only, no text, no runtime
chrome inside apps. Pages of four (columns keep their 112px, a swipe turns
the page) fit those constraints with the least new geometry; a grid gives
more per screen at 74px cells, which the finger table says is the limit
exactly. What this document insists on is only the ordering: **the geometry
decision comes before the icons are drawn**, because each icon is a
hand-converted Lucide silhouette at a fixed size, and eight icons drawn for
the wrong layout are eight redraws.

### Sound, briefly

The sound service plays one synthesized chime. Games want short event sounds
(a bounce, a jump, a strike) at low latency. The constraint worth preserving
is the one `sound.h` already argues: control-plane i2c exactly once at boot,
play/stop as pure data-plane. A small synth voice API (a few envelope and
pitch presets over the existing synthesis) keeps that property; stored PCM
per app does not fit the SRAM story and should not start. Lower stakes than
the four above; it just should not be decided eight times.

## What this document cannot do

It cannot name the fourteenth seam. Every seam listed here was named after
it was paid for, and the honest reading of thirteen-for-thirteen is that this
project discovers chains by shipping bugs down them. The one portable
discipline it can leave is small: a new app, or a new capability, opens with
a paragraph naming what cannot see it, which instruments are blind to it and
why, the way `sensors.h` already carries its honesty requirements. That
paragraph does not prevent the bug. It prevents the day of believing four
green instruments while the device sits broken in a child's hand, because
the blindness was written down before the code was.

And the last links of the chain stay human, on purpose. The ritual ends at a
thumb on a real button and the contact sheet ends at the owner's eye, not
because automation ran out of budget, but because the thumb and the eye are
the only instruments that read at the end of the chain, and the law this
document exists to record is: believe the instrument closest to the child.

## If only one thing is done

Publish the orientation signal and give the emulator its tilt input, tonight,
before the level, tilt-a-ball and compass are started. Three of the eight
apps land on that missing link within hours, the board is asleep and cannot
absorb their iteration, and without the signal each author will improvise a
seam this project has already paid twice to learn not to have. Storage is a
close second and costs more when it finally fires; orientation fires first.
