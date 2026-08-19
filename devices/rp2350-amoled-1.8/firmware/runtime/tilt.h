/*
 * tilt: which way is down, published once, for every app that wants it.
 *
 * THE POINT OF THIS FILE IS THAT THERE IS ONE OF IT. A spirit level, a
 * tilt-a-ball game and an orientation-aware clock all need the same fact,
 * and before this file existed the QMI8658 was read for exactly one purpose
 * (detecting a shake, for the sketchpad's erase) and app_frame_t carried no
 * gravity at all. Three apps written in the same week would have grown three
 * private IMU shims, which is the "written twice" seam this project has
 * already paid to diagnose twice (docs/decisions/0003 and 0008). An app
 * never reads this header either: it reads app_frame_t.tilt (app.h), the
 * same way it already reads touchDown and nowMs.
 *
 * THERE IS NO MAGNETOMETER ON THIS BOARD. The QMI8658 is a six-axis part,
 * accelerometer plus gyroscope, and nothing else on this board senses the
 * earth's magnetic field. So this signal can tell you which way is DOWN and
 * it can never tell you which way is NORTH. A compass app cannot be built
 * here; a which-way-is-down toy can. If you came looking for a heading,
 * stop now rather than an evening from now (docs/decisions/0010 says the
 * same thing, and this sentence exists because that document predicted
 * someone would find it here instead).
 *
 * WHAT IS PUBLISHED, AND WHY THAT SHAPE
 *
 * One filtered gravity vector in units of g, in the panel's own coordinate
 * space, plus two things derived from it that every consumer would
 * otherwise re-derive slightly differently: the angle from flat, and which
 * edge is up.
 *
 *   +x  toward increasing panel x (to the right of the screen as drawn)
 *   +y  toward increasing panel y (down the screen as drawn)
 *   +z  INTO the glass, away from the person looking at it
 *
 * so lying flat on a table, screen up, reads (0, 0, +1): gravity points
 * straight through the screen into the table. Held upright, portrait, top
 * edge up, reads (0, +1, 0): gravity points at the bottom edge. The axes
 * are right-handed with y down, which is the same handedness the framebuffer
 * already uses, so no app has to hold two coordinate systems in its head.
 *
 * WHAT WAS REJECTED, because each choice pushes work into a different
 * consumer and this one pushes the least:
 *
 *   An angle-and-direction pair (how far from flat, which way downhill).
 *   Reads beautifully for the spirit level and forces the ball game to run
 *   a sin and a cos every tick to get back the vector it actually wanted to
 *   integrate. Kept as a DERIVED field (tiltDeg) rather than as the
 *   representation.
 *
 *   Raw device-axis g, straight off the part. Every consumer would then
 *   carry its own copy of the device-to-screen mapping below, which is the
 *   one piece of this whole signal that no software oracle can check (see
 *   THE AXIS RITUAL). One wrong copy is a spirit level that leans the wrong
 *   way and passes every test this project can ever write.
 *
 *   A four-way orientation only (which of top/right/bottom/left is up).
 *   Enough for the clock, useless for the other two. Kept as a DERIVED
 *   field (up) rather than as the representation, because the hysteresis
 *   that makes it stable is a policy decision and belongs here, once, not
 *   in three apps with three different dead bands.
 *
 *   Screen-space "down" as a unit vector, magnitude thrown away. Cheaper,
 *   and it destroys the one thing the ball game needs when the device is
 *   near flat: a small tilt must produce a small acceleration.
 *
 * FILTERING IS DECIDED HERE, ONCE, AND THE PUBLISHED SIGNAL IS FILTERED
 *
 * An accelerometer in a child's hand is never still. If each app filtered
 * its own way the same hardware would feel different in each, which is
 * worse than any single choice, so this file publishes exactly ONE vector
 * and it is the filtered one. There is deliberately no raw vector on the
 * app-facing side: offering both is offering the divergence back, and the
 * app that picked "raw, for responsiveness" would be shipping the jitter.
 *
 * WHAT THIS DEVICE'S OWN SENSOR ACTUALLY GIVES, measured while building the
 * bubble level (firmware/apps/level.c) rather than assumed: the QMI8658's
 * own low-pass filter is left OFF by a vendor driver bug (AGENTS.md's
 * "Gotchas that bite"), so the part's 1000Hz output data rate folds its
 * full noise bandwidth into every one of core1's 50Hz samples. That
 * measures out to about 2.7mg RMS, 0.15 degrees of apparent tilt - not the
 * problem, even aliased and unfiltered. The problem is that a hand is not a
 * tripod: physiological tremor and wrist wobble put TENS of milli-g of real
 * acceleration into the reading at 1-10Hz, which is one to five degrees of
 * apparent tilt an accelerometer cannot tell apart from a genuine tip. A
 * child's hand is worse than an adult's, not better.
 *
 * A FIXED ONE-POLE LOW PASS CANNOT SEPARATE THOSE TWO. A corner slow enough
 * to hide hand tremor (around 1Hz) lags a deliberate tip by 150ms or more,
 * which reads as the signal being stuck to the hand by a rubber band; a
 * corner fast enough to feel immediate lets the tremor straight through,
 * and a reading that shivers reads as broken rather than as sensitive.
 * Neither end of that trade is acceptable, so this does not pick a point on
 * it - it uses an ADAPTIVE corner (the "one euro" filter, Casiez et al.):
 * smooth hard when the signal is barely moving, barely at all when it is
 * moving fast. On top of that, a magnitude TRUST GATE, because a low pass
 * cannot fix a different problem: an accelerometer measures gravity plus
 * whatever else you are doing to it, and while the device is being carried
 * the vector is not gravity and no amount of smoothing makes it gravity.
 *
 *   TILT_FC_MIN_HZ = 0.9Hz    the corner at rest, tau ~= 177ms. At the
 *                             board's 20ms cadence that is alpha ~= 0.083.
 *                             A 4Hz tremor comes through at about a third
 *                             of its amplitude - 30mg of wobble lands as
 *                             roughly 9mg, half a degree.
 *   TILT_BETA_HZ_PER_G_MS     Hz of extra corner per (g/ms) of measured
 *   = 3000.0                  speed. A deliberate 15-degree tip over 400ms
 *                             moves the reading at about 0.00065 g/ms,
 *                             which lifts the corner to roughly 2.6Hz: tau
 *                             61ms, four frames of lag, invisible to a
 *                             hand. Deliberately not larger - beta is what
 *                             lets a fast tremor argue its own way through
 *                             the filter, and the correct guard against
 *                             that is a small beta plus the low-passed
 *                             derivative below, not a big beta with a
 *                             deadband bolted on afterwards.
 *   TILT_TAU_D_MS = 200ms     the corner used to smooth the derivative
 *                             itself, which is what stops a 4Hz tremor from
 *                             reading as "moving fast" and unlocking the
 *                             filter it is supposed to be hidden by.
 *   TILT_TRUST_FULL_G = 0.15  trust is 1.0 while |a| is within this of 1g -
 *                             genuinely gravity, as far as this is
 *                             concerned.
 *   TILT_TRUST_NONE_G = 0.4   trust falls to 0.0 by the time |a| is this far
 *                             from 1g - the device is being carried, and
 *                             the estimate COASTS on what it last believed
 *                             rather than following a lie. tilt_reading_t's
 *                             `coasting` flag is exactly this: true while
 *                             trust is fully zero.
 *
 * These are milli-g quantities restated in g because this file's gravity IS
 * in g (see "WHAT IS PUBLISHED" above): TILT_BETA_HZ_PER_G_MS is 1000x the
 * number that same trade would want in Hz-per-(milli-g/ms), because it
 * multiplies a speed a thousand times smaller now that its units changed
 * from milli-g to g, and TILT_TRUST_FULL_G/TILT_TRUST_NONE_G are 150mg and
 * 400mg written as 0.15g and 0.4g. The corner-frequency and time constants
 * (TILT_FC_MIN_HZ, TILT_TAU_D_MS) are Hz and ms, unaffected by that choice.
 *
 * This filter and every constant above were ported verbatim, in behaviour,
 * from firmware/apps/level.c's own provisional filter (as of commit
 * c00db2f) - the bubble level was the first app that needed real
 * orientation and, lacking a published signal, measured this trade and
 * built the filter this file now owns instead. The bubble level itself is
 * gone from the app table (the owner had it removed outright, 2026-08-17);
 * this file is what is left of its work.
 *
 * THE COST, STATED RATHER THAN HIDDEN: the signal still lags a deliberate
 * tip near the resting corner (roughly TILT_FC_MIN_HZ's 177ms time
 * constant) before BETA's adaptive term catches up with the hand's own
 * speed. It has NOT been judged on the real device by a real hand yet. It
 * is a handful of constants, in one place, and the first person to hold a
 * spirit level on the board should change them if they feel wrong.
 *
 * The filter is stepped from dt, not from a fixed per-sample constant, on
 * purpose: the board samples every 20ms on core1, the emulator submits once
 * per browser frame at whatever rate the tab is running, and a filter whose
 * time constant depends on the caller's cadence would make the emulator and
 * the board feel different for a reason that has nothing to do with either.
 *
 * WHERE IT RUNS. On the board this is stepped from core1, inside the IMU
 * poll that already runs there, because core1 owns i2c1 (sensors.h's
 * ownership rule) and because core1's 20ms cadence is regular while core0's
 * loop rate is not: pushes cost anywhere from 27us to 12ms, so filtering on
 * core0 would make the time constant depend on how much the app drew last
 * frame. Nothing in here printfs, for the same reason nothing else on
 * core1 does.
 *
 * THE AXIS RITUAL, WHICH IS THE ONLY THING THAT CAN CHECK THE MAPPING
 *
 * device_to_panel() below converts the QMI8658's own axes into the panel
 * axes described above. WHICH WAY THE PART IS MOUNTED ON THIS BOARD WAS NOT
 * KNOWN TO ANY DOCUMENT IN THIS REPOSITORY: the schematic in AGENTS.md gives
 * the pins, not the footprint's rotation, and the vendor demo only ever
 * prints the numbers. RUN, 2026-08-17, on the real board (about two
 * minutes; the board must be awake and plugged in):
 *
 *   1. bun tools/dev.ts tilt        (devlink's TILT command)
 *   2. Lie the puck flat on a table, screen up. Expect g = (0, 0, +1).
 *   3. Hold it upright, portrait, top edge up. Expect g = (0, +1, 0).
 *   4. Turn it a quarter turn so its RIGHT edge is up. Expect
 *      g = (+1, 0, 0), and up = LEFT. (Not (-1, 0, 0): that was this
 *      comment's own earlier guess, and it was wrong - it did not agree
 *      with its own "up = LEFT", which the up-edge code only produces for
 *      a POSITIVE panel X. See tilt.c's device_to_panel() for the
 *      determinant that caught it.)
 *   5. Turn it screen down on the table. Expect g = (0, 0, -1), tilt 180.
 *
 * Every line also prints the RAW device-axis reading, so if a pose comes
 * out wrong, the correction is one edit to device_to_panel() and nothing
 * else in the tree moves. Poses 2-4 above are what was actually measured;
 * device_to_panel() now implements the mapping they fit (swap X and Y,
 * negate Z - tilt.c's own comment has the raw numbers and the arithmetic).
 * Pose 5 was not measured, only checked for self-consistency with the fit
 * from the other three. THE MAPPING IS NOT YET VERIFIED ON THE BOARD: it
 * was derived from readings taken against the OLD (identity) code, and the
 * corrected function has only run in the emulator so far. If a pose ever
 * comes out wrong again on a flashed build, this is still the first place
 * to look.
 *
 * WHAT NO INSTRUMENT HERE CAN SEE (docs/decisions/0010's discipline: say
 * what is blind before writing the code, not after the bug):
 *   - whether the corrected device_to_panel() below is actually right on
 *     the physical board - it has been derived from real readings and
 *     checked for internal consistency (determinant, a fourth independent
 *     pose, tiltDeg at rest), but never itself run on silicon; re-run the
 *     ritual above once it is flashed;
 *   - whether the adaptive filter's constants feel right in a hand, which is
 *     a judgement no emulator can make (emu_abi.h already says timing is not
 *     real there, and the emulator's gravity is a perfectly still,
 *     perfectly unit vector with no tremor and no shake artifacts at all);
 *   - whether the part is level with the case. The IMU is soldered to the
 *     PCB, the PCB sits in a plastic shell, and a couple of degrees of
 *     assembly tilt would show up as a spirit level that never quite reads
 *     zero. Nothing in software can tell that apart from a genuinely
 *     un-level table, so if the owner reports "it never centres", the fix
 *     is a measured offset, not a bigger dead band.
 */
#ifndef TILT_H
#define TILT_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h" // for TILT_UP_*, which are the app-facing names for the
                 // same four codes this file computes. One set of
                 // constants, defined where an app author will read them.

/* ---- the published reading ---------------------------------------------
 *
 * PANEL space (see the header comment). runtime_core.c rotates this into a
 * landscape app's own drawing space before it reaches app_frame_t.tilt, so
 * an app never sees this struct and never rotates anything itself.
 */
typedef struct {
    // Filtered gravity, g units, panel axes.
    float gx, gy, gz;

    // Angle between the panel's inward normal and gravity, degrees:
    // 0 = flat, screen up. 90 = on edge. 180 = flat, screen down.
    float tiltDeg;

    // Which panel edge is up: TILT_UP_TOP / RIGHT / BOTTOM / LEFT (app.h).
    // Hysteretic, and HELD (not cleared) while the device is too flat to
    // have an answer - see the header comment on why this is published
    // rather than left to three apps to derive.
    uint8_t up;

    // False until the first sample arrives, and false again if no sample
    // has arrived for TILT_STALE_MS. An app that draws a level from an
    // invalid reading draws a confident lie, which is the failure decision
    // 0010 describes for a clock with an unset RTC. Check it.
    bool valid;

    // True while the magnitude trust gate has fully given up on the current
    // raw sample (the device is being carried, dropped, or shaken rather
    // than held still) and the filtered vector is holding its last belief
    // instead of tracking. See "FILTERING" above for the gate itself
    // (TILT_TRUST_FULL_G / TILT_TRUST_NONE_G in tilt.c). Not the same
    // condition as `valid`: a coasting reading is still the best guess this
    // file has and is still safe to draw, it is just not currently being
    // updated by the sensor. No app reads this yet; it is published because
    // an app that wants to say so (dim a level, freeze a verdict) needs
    // somewhere to read it from, and that has to be here, next to the gate
    // that computes it, not re-derived downstream.
    bool coasting;

    // RAW, unfiltered, in the QMI8658's OWN axes, for the axis ritual and
    // for devlink's TILT line. Deliberately not carried into app_frame_t:
    // an app that reads device axes is an app that has to know how the
    // part is mounted, which is exactly the knowledge this file exists to
    // hold in one place.
    float rawX, rawY, rawZ;
} tilt_reading_t;

// How long a published reading stays trustworthy without a fresh sample.
// 25 missed samples at the board's 20ms cadence: long enough that a couple
// of i2c timeouts (sensors_stats_t.imuTimeouts) do not blink an app's
// display, short enough that a genuinely dead part is visible rather than
// frozen at its last pose. The filter's own constants
// (TILT_FC_MIN_HZ, TILT_BETA_HZ_PER_G_MS, TILT_TAU_D_MS,
// TILT_TRUST_FULL_G, TILT_TRUST_NONE_G) live in tilt.c, next to the filter
// that uses them, and are documented in full in the "FILTERING" section
// above.
#define TILT_STALE_MS 500u

/* ---- the producer side --------------------------------------------------
 *
 * One raw accelerometer reading, in the QMI8658's own axes, in g. Called
 * from core1 on the board (sensors.c's imu_poll_core1, which already does
 * this i2c read for the shake detector, so this adds no bus traffic at all)
 * and from emu_tick() in the emulator (emu_shim.c). nowMs is the same clock
 * the rest of the frame uses; dt between calls drives the filter.
 *
 * Single writer, always: on the board this is core1 and only core1, per
 * sensors.h's ownership rule.
 */
void tilt_submit_device_g(float dx, float dy, float dz, uint32_t nowMs);

/* ---- the consumer side --------------------------------------------------
 *
 * Reads the published signal. Called from core0 on the board (once per
 * frame, by runtime_core.c) and from devlink's TILT command. Never blocks,
 * never fails: if it catches the writer mid-update it returns the previous
 * consistent snapshot rather than a torn vector (see tilt.c's seqlock).
 *
 * nowMs is passed in rather than read, because the staleness check is the
 * consumer's clock question and because rtcore_tick()'s nowMs is the only
 * time source the emulator has (emu_abi.h).
 */
void tilt_read(uint32_t nowMs, tilt_reading_t *out);

#endif // TILT_H
