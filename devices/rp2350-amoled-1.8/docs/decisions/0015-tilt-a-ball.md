# 0015: The tilt-a-ball is a bowl, not a Monkey Ball level

Date: 2026-08-15
Status: accepted, built; the axis mapping and the tilt constants are
unverified by a hand (see "What is owed" below, and tilt.h/tilt.c's own
identical caveat, which this app inherits rather than repeats)

## What was asked, and what was decided instead

"Un truc genre monkey ball ou il faut rouler une balle en orientant
l'appareil" - something Monkey-Ball-like where you roll a ball by orienting
the device. Monkey Ball is the reference for the FEELING (a ball that rolls
because you tipped the world), not a request for its difficulty, its
falling-off-the-edge punishment, or its levels. The user is a two-year-old
who cannot read and is the only person who will ever hold this, so this
device does not get a game with a win state, a score, or a fail state - it
gets a toy with one satisfying event in it, that loops.

Concretely: **one round dish, one ball, one fixed hole.** No maze (decision
0009 forbids straight walls outright and names a maze as the obvious,
rejected design), no levels, no lives, no timer. Falling into the hole is a
reward that loops - the ball reappears at the dish's centre a moment later -
not an ending.

## The dish is a bowl, and that decision carries the whole app

The play surface is not a flat plate with a rim wall. It is modelled as a
shallow paraboloid bowl: tip the object and the bowl's own low point shifts
toward the downhill side, and the ball is a damped, driven 2D spring around
that point (`ax = ACCEL_PER_G*gx - BOWL_OMEGA2*(px-CX) - DAMPING*vx`, and the
same for y), not a plain "accelerate by gx,gy and coast" integrator.

This was chosen for one reason that matters specifically for the intended
user: **a spring has a built-in home.** However hard a two-year-old tips the
puck, the restoring term pulls the ball back toward a bounded equilibrium;
there is no tilt that sends it toward infinity. The rim's own soft
containment (a velocity-damped bounce at `BALL_TRAVEL_MAX`) is a second,
independent backstop for the transient overshoot a spring can still produce
under a hard, fast tip - not the primary safety net. Between the two, the
task's own worry about "she tips things much harder than an adult" is
answered structurally, not by hoping the constants are tuned exactly right.

## The tilt-to-acceleration mapping is gentler than the level's, on purpose

`firmware/apps/level.c`'s dial reaches its own full deflection at 15 degrees
- level.c's own comment calls that "a deliberate, controlled gesture" an
adult's careful wrist produces. This app does not reuse that scale. Its
three constants (`BALL_ACCEL_PER_G`, `BALL_BOWL_OMEGA2`, `BALL_DAMPING`,
`firmware/apps/tiltball.c`) are chosen so a careful 15-degree tip moves the
ball only partway - short of the fixed hole - and reaching the hole wants a
real, decisive tilt closer to 30+ degrees, comfortably inside a toddler's
natural range rather than an adult's fine-motor one. Point 1 of the brief
this app was built against ("a mapping tuned on a careful wrist will be
uncontrollable in her hands") is the reason the number is different here,
not an oversight against level.c's own precedent.

**Unverified by a hand**, stated the same way tilt.c's own filter constants
already are: these three numbers came from the arithmetic in
`tiltball.c`'s header comment, not from anyone tipping the physical board
while watching the ball. If the first person to hold it says it is too
twitchy or too dead, these three `#define`s are where to change it - never
the shared filter in `tilt.c`, which every orientation-aware app on this
device depends on staying one thing (decision 0012).

## What the ball does under `!valid` and `coasting`

`!valid` (no IMU reading yet, or it has gone stale - `tilt.h`'s
`TILT_STALE_MS`): the external tilt term drops to zero, but the bowl's own
restoring pull keeps running, because that pull is a fact about the dish's
fixed shape, not a claim about the sensor. A stationary bowl slopes to its
middle whether or not anyone is reading an accelerometer, so letting the
ball drift back toward the centre while the signal is unavailable draws
nothing that depends on an untrustworthy reading - the same standard
`tilt.h` already sets for `valid`, applied to a continuous physics term
instead of a discrete verdict.

`coasting` (the filter's magnitude trust gate has fully given up on the
current raw sample - the device is being carried - and `gx/gy` holds its
last filtered belief): not specially handled, the same as every other app
today (no shipped app reads this flag yet). The reasoning for why that is
fine here specifically: `gx/gy` is still `valid` while coasting, and a ball
resting on a dish rigidly attached to the object being carried genuinely
still feels the last tilt gravity could be told apart from motion at - so
driving it from a held belief is physically honest, not a confident lie the
way drawing a level's verdict from a coasting reading would be.

## The axis mapping: inherited, not re-derived

This app has no orientation code of its own - no sign flip, no swap,
nothing. It reads `f->tilt.gx/gy`, already filtered and already rotated into
its own landscape space by `runtime_core.c`'s `tilt_for_app()`. The
device-to-panel mapping in `tilt.c` is a documented HYPOTHESIS (identity)
until the on-board axis ritual (`tilt.h`, `bun tools/dev.ts tilt`) settles it
by hand - **this has not been run.** Because this app touches nothing
upstream of `app_frame_t.tilt`, if that mapping is ever found flipped or
transposed, the fix is one edit to `device_to_panel()` in `tilt.c` and this
file does not change. The practical consequence today, if the hypothesis is
wrong and undiscovered: the ball would roll toward the wrong physical edge
for a given real-world tip - direction wrong, feel and magnitude unaffected,
corrected for every orientation-aware app at once the moment the ritual
runs.

## The arrival gets more care than the physics

Per the task's own framing: the hole is where the effort goes. The instant
the ball's centre comes within `CAPTURE_TRIGGER_R` of the hole (well inside
the hole's own radius, so it reads as "it went in" rather than "it grazed
the edge"), three things fire together, on that one tick and nowhere else:

- a falling-pitch sound (`sound_synth_capture_sample`, `sound_synth.c`) -
  descending, not the alarm's rising phrase, because a falling glide is the
  standard shorthand for something dropping in;
- the ball slides and shrinks toward the hole's centre on an ease-in curve
  (slow to start, plunging at the end - reads as falling, not fading);
- a ripple ring expands from the hole and fades.

A short pause, then the ball grows back at the dish's centre and control
returns. Total non-interactive time is under a second (`CAPTURE_ANIM_MS +
HIDDEN_PAUSE_MS + RESPAWN_ANIM_MS` = 900ms) - long enough to register as an
event, short enough that a two-year-old is not left waiting.

## Rejected: colour

Considered (a warm ball against a grey dish) and rejected. Every app on this
device, including the level this one borrows its drawing technique from, is
one ink on one paper (decision 0009's own framing), and the panel's
anti-aliasing convention (`gray_to_px`, `gfx.h`) is built entirely around the
green channel doubling as an 8-bit coverage value - a monochrome assumption
every `shade()` call in this app relies on. A colour ball would need its own
RGB anti-aliasing path for one app's decoration, not for something the toy
actually needs: a two-year-old does not need the ball to be orange to find
it, she needs it to move when she tips the puck.

## Drawing, and what it costs

Same technique as `level.c`, copied rather than shared (decision 0002
section 4b - "each app is an object and refers to nothing outside itself"):
`paint_rect()` recomputes every pixel of a rectangle from the model (rim,
hole, ripple, ball, darkest wins) rather than erasing and redrawing, so
residue is impossible by construction; the dirty rectangle pushed is
pre-rounded to `gfx_push`'s 8-pixel row rule by a file-local copy of
`align_for_push()`. Measured over a wide orbit, a slam across the dish, and
a full capture cycle (`emulator/wasm/tests/repro-tiltball-residue.ts`):
worst frame 6.6% of the panel, average pushing frame 2.5%, zero pixels and
zero pushes once the ball has settled and the device is still (a settle
SNAP - see `tiltball.c`'s `BALL_SETTLE_POS_EPS`/`BALL_SETTLE_VEL_EPS` -
makes this hold for a spring the same way it already holds for the level's
own dial, which needs no such snap because it is memoryless).

## What is owed

- **The axis ritual.** Two minutes with the board awake
  (`bun tools/dev.ts tilt`); this app inherits whatever it finds, unchanged.
- **The three tilt constants, judged by a hand.** Nobody has tipped the
  physical board and watched this ball roll yet.
- **The fixture-count shift.** Six real apps now, not five:
  `runtime_core.c`'s `MENU_STUB_APPS` thresholds and `stubapps.c`'s stub
  count both moved by one to keep the screenshot fixture's ceiling at twelve
  (decision 0013) rather than thirteen - done here, but worth naming so the
  next app added does not have to rediscover the pattern.
- **The board build.** Compiled clean as part of this change
  (`cmake --build firmware/build`); not flashed - the board is asleep on the
  owner's desk, per this project's standing rule.
