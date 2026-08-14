// timer: a countdown, set by dragging a ring rather than typing a number.
// See docs/decisions/0002-runtime-architecture.md, "The timer, in detail",
// for the brief this implements; the reasoning below assumes that section
// has been read and only restates it where the code needs to justify a
// specific number.
//
// Setting: the egg-timer twist. A ring surrounds the landscape canvas;
// dragging a finger around it grows a filled arc to the finger's angle,
// snapped to a round value. Running: the arc shrinks. The alarm: a
// full-panel flash, self-limiting, dismissed by anything.
//
// Owner feedback, verbatim, on the first look (square ticks, hollow outline
// for "not yet chosen"): "i'd rather you didn't draw boxes and just a full
// circle, or maybe many small circles with full color so that it looks more
// full (rather than just the outline with no colour filled inside). also
// use the same font, same interlines and spacing as the chronometer". A
// second round then asked for the 65 filled circles that answer to be
// greyed by elapsed/unchosen state and to CRUMBLE continuously rather than
// step: "I do not want it to be 'nothing changes and then all at once a
// whole circle disappears'... I think the dots on the left should be more
// faded." Both rounds were implemented as dots first (see git history for
// that version, including a per-dot interpolation scheme).
//
// THIS FILE THEN IMPLEMENTED THE OWNER'S OWN "OR MAYBE... A FULL CIRCLE"
// ALTERNATIVE INSTEAD, after review: a continuous annulus, track in light
// grey, filled arc in black, Apple-activity-ring style. The crumbling dot
// was a patch over quantisation that never fully went away: even
// interpolated, it was still one distinguished element standing in for a
// continuous quantity, redrawn in discrete steps. An arc has nothing to
// quantise; it moves because the angle is a continuous function of what is
// left, recomputed fresh every frame. It also answers the "hard to see big
// dots vs small dots" complaint more directly than grey-as-primary-cue did:
// a thick black arc against a light grey track is the strongest contrast
// this white-paper-black-ink panel can produce, and it reads as ONE shape
// rather than as a comparison across 65 marks. What the dots gave that a
// bare circle would not (see the old rejection of "just a full circle": "a
// full circle can only ever show ONE number... does not show the step
// size") went, for one iteration, to 12 small tick marks at the 5-minute
// positions outside the ring; the owner then asked for those gone too, on
// the same reasoning Apple's own activity rings follow: the exact value is
// already written large in the middle of the ring, so a graduation nobody
// needs to count adds no information and only breaks the ring's contour.
//
// A dot, and every shape in this file, is a stack of RECTANGLE rows, not
// pixels: gfx rotates rectangles, not pixels (gfx.h). The ring is drawn as
// a stack of 1px-tall horizontal bars, each one a call to
// gfx_fill_rect_land, the technique the old dots used and the one
// firmware/apps/shapes.c offers as shapes_fill_half_width_table /
// shapes_draw_annulus_row: this file uses the table builder for its
// outer/inner half-width tables rather than re-deriving the sqrtf loop, but
// NOT shapes_draw_annulus_row itself, because that draws one row in ONE
// colour and almost every row here is split between a black arc and a grey
// track; the angular clipping that split needs (see "Angle, and where it
// gets fiddly" below) has no other caller yet, so it stays in this file
// rather than being pushed into shapes.c speculatively.
//
// Digits are digits.c's, shared with chrono rather than redrawn here (see
// digits.h): same numerals, same two hard-won corrections, one copy.
//
// =========================================================================
// CORRECTED 2026-08-14: THE COIL.
// =========================================================================
//
// Everything above this line is the ring's own history and stays true of
// the ring's LOOK (a continuous black arc over a grey track, rounded caps,
// no tick marks). What changed on this date is what the arc MEANS and how
// many of it there are - the tiered step table the next section used to
// describe (5s/30s/1m, "half the arc must mean half the time" retired
// deliberately) is gone, replaced by the coil below. That whole tiered
// section is kept, unedited, immediately after this one: it is the record
// of a real, deliberate design (and the reason it was later reversed is
// worth keeping alongside it), not dead prose to delete.
//
// THE PROBLEM WITH THE TIERED RING. Three different step sizes at three
// different radii meant the same finger movement meant three different
// things depending on where on the dial it happened - fast (5s) for the
// first 24 slots, medium (30s) for the next 16, slow (1m) for the rest. An
// adult reading the digits could compensate. A child who cannot yet read
// MM:SS, and who is learning what this object does entirely through her
// hands, cannot: dragging the same distance sometimes buys two seconds,
// sometimes two minutes, and there is nothing on the dial itself - no knee,
// no visible boundary - that says which zone a finger is in. That
// unpredictability, not the numbers themselves, is the owner's complaint,
// and it is what THE COIL exists to remove.
//
// THE OWNER'S DESIGN, taken as given rather than reinterpreted (his own
// words, lightly reformatted):
//
//   - the step is 5 seconds, EVERYWHERE. no tiers, no knees.
//   - pointing directly still works and is the fast gesture, landing in
//     the right neighbourhood.
//   - dragging past the top of the dial adds a revolution, dragging back
//     past it removes one. total = laps*(lap length) + (angle within the
//     current lap) - "a modulo". the mechanism is continuous unwrapped
//     angle tracking across the twelve o'clock branch cut, not the raw
//     angle.
//   - the laps must be visually countable: each revolution is its OWN
//     concentric band, separated by a visible outline (Apple activity
//     rings: two laps means you plainly see two rings, not one thicker
//     one), winding INWARD (lap 1 outermost, lap 2 just inside it, ...) so
//     the dial's outer diameter never grows and it reads as something being
//     coiled.
//   - setting and running still share one mapping (this file already had
//     that property for the tiered ring - see current_fill_deg()'s old
//     header comment, retained below - and it is preserved here for the
//     same reason: a drag and a countdown that could ever visibly disagree
//     about where the arc belongs is unusable, not just inconsistent).
//   - stopping must not force a reset: "if I stop the timer I should be
//     able to directly edit the time without clearing." Pausing then
//     dragging edits from the paused value.
//
// The lap length and lap count themselves went through two revisions the
// same week (ten minutes/six laps, then thirty minutes/two laps - see the
// second dated section below, "30 MINUTES A LAP, TWO LAPS"): what is
// listed above is what stayed constant across both, the actual mechanism.
//
// THE STEP TABLE COLLAPSES TO ONE LINE. TICK_STEP_S = 5, always. seconds =
// ticks*5 and ticks = seconds/5 are now exact linear inverses of each
// other, no piecewise cases, no boundary to get subtly wrong at 2:00 or
// 10:00 - see seconds_for_ticks()/tick_index_for_seconds() below, which
// used to be the two most complicated functions in this file and are now
// each one line.
//
// POINT VS DRAG, AND THE BRANCH CUT. A tap ("point directly") sets the
// SUB-LAP position to the tapped angle while preserving however many laps
// are already wound - it never resets the lap count, so tapping near the
// dial while deep into a lap nudges the value within that lap, it does not
// throw a completed lap away. A continuing drag instead accumulates the
// UNWRAPPED angular delta frame to frame, exactly the mechanism the owner
// named: crossing twelve o'clock going one way adds a lap, going the other
// removes one, because the delta computed across that crossing is a small
// forward (or backward) nudge, not a wraparound one - see point_touch()/
// drag_touch() below, and this file's "Angle, and where it gets fiddly"
// section, which the coil inherits unchanged (the branch cut problem it
// already solved for reading a single angle is the same problem
// drag_touch's delta-unwrap solves for reading a CHANGE in angle). That
// branch cut bit this project once already (see the ring-shrink-residue
// history further down) - repro-ring-shrink-residue.ts is kept and
// extended, not replaced, and a dedicated new test drives a drag through
// twelve o'clock forward and back and checks the landed total exactly.
//
// TRUE ZERO IS NOW REACHABLE BY TOUCH, ON PURPOSE - A DELIBERATE CHANGE.
// The tiered ring's ring_tick_for_touch() could never return a tick of
// exactly zero from a drag (see its own retained comment below): with a
// single absolute angle-to-slot snapshot, a full revolution of slots had no
// slot left over to mean "zero", so "zero" was reserved for the untouched
// default alone. The coil's mechanism is different in kind - a drag is a
// RELATIVE accumulator, not a snapshot - so zero is just the natural floor
// of that accumulator: dragging backward past it holds at zero with no dead
// zone, and dragging forward immediately resumes from there. This is not an
// oversight carried over from the old design; it is a consequence of the
// new one, and it is more intuitive (a scrollbar-style hard stop) than the
// old asymmetry was.
//
// PAUSE, THEN EDIT. TS_PAUSED gained exactly one new touch reaction: a
// touch on the ring converts the frozen paused value into a fresh SETTING
// session (see the TS_PAUSED branch in timer_tick()) - the lap count and
// sub-lap position both carry over from the instant of pausing, then the
// same point/drag mechanism above edits it live. PWR still resumes an
// untouched pause without any of this firing. The state machine gains no
// new state for this: SETTING already means "not committed yet", which is
// exactly what a value someone is actively correcting is.
//
// =========================================================================
// CORRECTED 2026-08-14 (LATER THE SAME DAY): 30 MINUTES A LAP, TWO LAPS,
// A WIDER BAND.
// =========================================================================
//
// Owner, direct quote, after seeing the first coil (ten minutes a lap, six
// laps, six thin bands): "let's make a round trip 30min and a double trip
// 60min and double the width of the coil." Three changes, all mechanical
// consequences of the SAME design (nothing about point-vs-drag, the branch
// cut, "setting and running share one mapping", or pause-then-edit changes
// - only how many laps there are and how much of the panel each one gets):
//
//   - TICKS_PER_LAP: 10 minutes' worth of the flat 5s step (120) -> 30
//     minutes' worth (360).
//   - LAPS_MAX: 6 -> 2, since 2 laps * 30 minutes is still the same 60
//     minute ceiling the owner specified from the start.
//   - the band itself: twice as thick, because two bands need to fit where
//     six used to - see "Ring geometry: the coil" below for the reworked
//     derivation, which lands (by a genuine coincidence of the arithmetic,
//     not forced) on the exact outer radius and margins the ORIGINAL
//     single ring had, before there were bands at all.
//
// TWO CONSEQUENCES OF THE NEW NUMBERS, HANDLED DELIBERATELY:
//
// First, six o'clock is now 15:00, not 5:00 (a 30-minute lap's own half-
// turn). The owner's earlier worked example ("touching the bottom sets 5
// minutes") was specific to a 10-minute lap and no longer holds at 30 - he
// said so explicitly when giving the new numbers, so this is an accepted,
// known consequence, not a silently broken promise. This file's own tests
// check 15:00, not 5:00 - see repro-timer-coil.ts.
//
// Second, and this is the one requiring judgement rather than arithmetic:
// TICKS_PER_LAP going from 120 to 360 makes each tick's own ARC LENGTH
// roughly a third of what it was for a similar-radius lap, because three
// times as many ticks now share the same circumference. At this file's new
// RING_OUTER_R (173, see the geometry section below), one tick is
// 2*pi*173/360 = ~3.0 pixels of arc - within the same order of magnitude
// the owner's own estimate ("roughly 2.6 pixels") landed on, and well
// under anything a fingertip can aim at directly. POINTING therefore still
// lands in the right NEIGHBOURHOOD (see sub_lap_ticks_for_angle, unchanged
// in mechanism) but cannot reliably land on one specific 5-second tick by
// itself any more; WINDING - continuing to drag - is what actually resolves
// the exact value, exactly as the owner said. See drag_touch()'s own
// comment, "FINE-GRAINED DRAG: NO JITTER AT REST, NO RUNAWAY WHEN FAST",
// for what this file does about it and the honest limit of what software
// alone can guarantee here.
//
// SHAKE TO CLEAR, ADDED THE SAME DAY. Owner, direct quote: "also i should
// be able to shake to clear while i'm still in edit mode there." See
// timer_tick()'s own shake-clear branch, and g_timerApp's wantsShake
// comment at the bottom of this file, for the full reasoning (why SETTING
// and PAUSED both qualify as "edit mode" but RUNNING deliberately does
// not, and why this can never double-fire with the alarm's own,
// pre-existing shake dismissal).
//
// =========================================================================
// CORRECTED 2026-08-14 (AFTER REAL-HARDWARE USE): 15 MINUTES A LAP, A
// SECOND WRAP-TO-ZERO FIX, AND A CAP-DRAWING FIX.
// =========================================================================
//
// The owner tested the 30-minute-lap coil above on real hardware, with a
// real finger, and reported three things. Two were bugs; one was a new
// geometry instruction. All three are addressed by this section and the
// code below it.
//
// BUG 1: "je n'arrive pas à enrouler l'anneau, ça recommence à zéro quand
// je passe 30min" - winding past the end of a lap restarted at zero
// instead of adding a lap. THE TESTS ABOVE (repro-timer-coil.ts's branch-
// cut round trip) already proved the unwrap-and-accumulate MATH is correct
// for a clean, mouse-driven drag, and it still is - that was never the
// broken part. The broken part is what decides, frame to frame, whether a
// touch sample continues that accumulator (drag_touch()) or resets it from
// a fresh snapshot (point_touch()): every f->touchPressed used to be
// trusted as a genuine new touch-down. The real touch controller this
// device carries does not report a clean, continuous stream of contact
// while a finger sits on the glass - firmware/apps/sketch.c's own CONFIRM_MS
// comment cites a measured session with 798 dropouts in about 23 seconds of
// continuous contact, roughly one every 30ms, far inside a single drag
// gesture - and runtime_core.c's edge detector (app.h's touchPressed/
// touchReleased) cannot tell a dropout-induced blip apart from a genuine
// lift-and-retouch; it resolves a dropout as exactly that: a real release
// followed by a real press. Every one of those spurious presses called
// point_touch(), which reads the CURRENT raw angle and the CURRENT lap and
// reseeds dragAccumTicks and the sub-lap position from that snapshot -
// correct for an actual new gesture, catastrophic mid-drag, because it
// throws away however much of the current lap the drag had already wound
// in. A dropout landing right as a drag crosses twelve o'clock is exactly
// the reported symptom: the snapshot reads a small angle just past the top
// and a lap count that has not advanced yet, so the coil visibly restarts
// near zero - not perceived, actually thrown away.
//
// Reproduced in the emulator per this task's own instruction, and it did
// NOT reproduce with a clean mouse drag (confirming the branch-cut math
// itself was never the problem) - it took simulating the dropout-heavy
// sample stream (emulator/src/touchsim.ts's TouchSim, already built for
// sketch.c's identical problem) driving a drag across a lap boundary: a
// clamped-at-720-tick drag from 20 to 380 degrees, expected to land near
// 31:40, instead logged "timer: start, 00:55" - the wrap-to-zero,
// reproduced headlessly. THE FIX, entirely in this file (out of scope to
// touch runtime_core.c or sensors.c/h - see this task's own constraints,
// and sketch.c already owns the one app that reads the raw sample stream
// directly, for exactly this same reason): re-derive the same "a lift is
// only believed after a short grace period with no contact" rule sketch.c's
// own LIFT_DEBOUNCE_MS (80ms) applies to the raw stream, but at the
// resolved-edge level this file actually has access to - see
// TOUCH_DROPOUT_GRACE_MS, is_genuine_new_press() and timer_tick()'s TS_
// SETTING branch below, and timer_state_t's touchContextLive/
// lastTouchDownMs fields. A touchPressed within that grace window of the
// last seen touchDown is folded into drag_touch() instead of point_touch()
// - exactly correct for a bridged dropout too, since drag_touch()'s own
// branch-cut unwrap does not care how many frames the gap it is unwrapping
// across spans. Verified against the SAME dropout stream: 30/30 trials land
// correctly at the realistic measured dropout rate (2/s, this device's own
// measured FT3168 rate while drawing); 25/30 at 17x that rate (34/s,
// sketch.c's own worst-case stress-test rate) - the same order of
// improvement sketch.c's own CONFIRM_MS fix achieved on the identical
// hardware, not a 100% guarantee at an intentionally extreme rate, and not
// claimed as one.
//
// BUG 2: "sur le minuteur j'ai des pixels qui stray autour de l'anneau" -
// stray pixels around the coil. THIS ONE REPRODUCED WITH A CLEAN MOUSE
// DRAG, no dropouts involved at all - a different mechanism from bug 1,
// found by scanning the framebuffer for black pixels in the WHITE GAP
// between the two bands (which paint_ring_row's own comment says should
// "stay correct forever" because nothing ever repaints it - true only if
// nothing else ever writes there). A handful of pixels did: a cap drawn
// near 117 degrees left one permanently black at radius 165.9, inside what
// should be the 163-167 gap between band 1's outer edge and band 0's inner
// edge. Root cause: shapes_fill_half_width_table() rounds each row's half-
// width to the nearest pixel, which approximates a circle at the ROW level
// but not at the pixel-CORNER level - the cap's own CAP_TABLE_ROWS=6 table
// rounds sqrt(9-1.5^2)=2.598 UP to 3 at its dy=+-1.5 rows, so their
// outermost pixel sits at local distance sqrt(3^2+1.5^2)=3.354 from the
// cap's own centre, not the nominal BAND_HALF_THICK_PX=3 it was built from
// - a known, inherent property of table-based circle rasterisation, not a
// bug in isolation. Stacked with cap_center()'s own lroundf() snapping the
// cap's true (float) centre to the nearest integer pixel (up to ~0.7px more
// slack on the diagonal), the two roundings combined can push a cap pixel's
// true distance from the RING's own centre - not the cap's centre - about
// 1px past the band's declared inner or outer radius: precisely the
// measured 165.9 against a declared 167.
//
// THE FIX does not try to out-guess the rounding (shrinking the cap's own
// table radius by some fudge factor would either still leave a corner case
// uncovered or visibly flatten the cap). Instead draw_cap() now clips every
// row it draws against s_hwOuter[band]/s_hwInner[band] - the exact same
// per-row bounds paint_band_row() itself reads to decide what belongs to
// that band - so a cap pixel can never disagree with what the rest of this
// file already calls that band's own territory, by construction, regardless
// of either table's own independent rounding. See draw_cap_row_clipped().
// Verified: the same clean-drag scan that found the gap residue finds zero
// after the fix.
//
// NEITHER BUG WAS CAUGHT BY THE EXISTING TESTS, and each is instructive
// about why, separately: repro-timer-coil.ts's branch-cut round trip drives
// a clean, mouse-style touch stream, which never manufactures a spurious
// touchPressed, so it could not see bug 1 - the accumulator math it checks
// was never wrong. repro-ring-shrink-residue.ts's stray-pixel scan checks
// band membership with a +-1px tolerance for exactly the kind of edge-
// rounding noise a table-based circle produces, which is correct in general
// but was wide enough to swallow this specific defect's own ~1px reach
// without tripping MAX_STRAY_PER_BUCKET (the defect adds only a handful of
// pixels per drag, not a dense cluster). Both test files gain new coverage
// alongside this fix rather than having their existing margins loosened
// further - see repro-timer-coil.ts's own new dropout-bridging scenario and
// this file's own report for what changed.
//
// THE NEW GEOMETRY, the owner's own instruction, given after the two bugs
// above: "let's do... 15 minutes for one round, 2 rounds max, and double
// the width of the band again, and yes shrink the digits if needed." Three
// numbers change (TICKS_PER_LAP, the band thickness/gap, and the digit
// metrics); nothing about the MECHANISM does - see "Ring geometry: the
// coil" and the digit-layout section below for the reworked derivations.
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"
#include "shapes.h"
#include "sound.h"

/* ---------------------------------------------------------------------
 * Tick geometry and the seconds-per-tick mapping - SUPERSEDED 2026-08-14,
 * kept verbatim as the record of a real, deliberate design this project
 * shipped and then reversed. See this file's header, "CORRECTED
 * 2026-08-14: THE COIL", for why: the owner's brief for the redesign is
 * "the step is 5 seconds, everywhere. no tiers, no knees" - the opposite of
 * what this section built. The code this section used to justify
 * (FINE_TICKS/MID_TICKS/COARSE_TICKS and the piecewise
 * seconds_for_ticks()/tick_index_for_seconds() below them) has been
 * removed; only the comment stays, because the REASONING here (the
 * fingertip-size arithmetic, the "last two minutes stretch and slow"
 * upside) was real and might matter again if a future step scheme is ever
 * considered.
 *
 * Owner requirement (2026-08-13): "the timer must be settable in
 * increments of 5 seconds." A flat 5s step collides with the old 60-minute
 * cap - 3600/5 = 720 positions around a ring whose circumference, at this
 * file's RING_RADIUS (165, see the ring-geometry section below), is
 * 2*pi*165 = ~1036.7px, is ~1.4px per step, well under AGENTS.md's ~75px
 * child-fingertip figure. A first pass traded the cap down to 10 minutes to
 * keep a flat step. The owner was offered that trade and picked a
 * different one instead, kept here:
 *
 *   0:00  -  2:00   5s  per step  (24 steps)
 *   2:00  - 10:00  30s  per step  (16 steps)
 *  10:00  - 60:00   1m  per step  (50 steps)
 *
 * TICK_COUNT = 24+16+50 = 90, and 24*5 + 16*30 + 50*60 = 120+480+3000 =
 * 3600s, so the old 60-minute cap survives. 1036.7/90 = ~11.5px per step,
 * comfortably clear of the ~75px fingertip figure (and finer than the flat-
 * step alternative's 8.6px/step at a 10-minute cap, because the coarse tail
 * spends far fewer of the ring's 90 slots per minute of real time than a
 * flat step would).
 *
 * THE CONSEQUENCE, accepted deliberately, not stumbled into: this makes the
 * seconds-per-degree of arc NON-UNIFORM across the dial, which the very
 * first version of this file explicitly rejected ("half the arc must mean
 * half the time"). That rule is retired by this instruction, not violated
 * by accident - the last two minutes occupy 24/90 = ~27%, about a quarter,
 * of the ring, while being only 120/3600 = ~3.3%, about three percent, of
 * the maximum - a countdown that visibly slows and stretches its ending,
 * which reads as more useful than one that thins out linearly and
 * vanishes, because the end is the part anyone is actually watching. That
 * upside was not the design goal (the goal was reaching 5s resolution on a
 * 75px finger without shrinking the cap); it is a side effect worth knowing
 * about before "fixing" the non-uniformity back out.
 *
 * WHY IT WAS REVERSED: the coil's flat 5s-everywhere step buys back exactly
 * the "same finger movement means different things in different places"
 * problem this section's own three tiers created, at the cost of the
 * "stretched ending" upside above - a trade the owner made explicitly, see
 * this file's header.
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Tick geometry: the coil. One flat step, everywhere. - SUPERSEDED
 * 2026-08-14 (real-hardware pass), kept verbatim as the record of the
 * 30-minute-lap coil this project shipped and tested before the owner's
 * real-finger session below. See this file's header, "CORRECTED 2026-08-14
 * (AFTER REAL-HARDWARE USE)", for why: "15 minutes for one round, 2 rounds
 * max" replaces "a round trip 30min... a double trip 60min", same mechanism,
 * a further-shortened lap.
 *
 * TICK_STEP_S = 5, unconditionally - see this file's header for why the
 * three-tier table above no longer applies.
 *
 * TICKS_PER_LAP is 30 minutes' worth of that flat step (1800s / 5s = 360) -
 * SUPERSEDED FROM 120 (10 minutes) on 2026-08-14, same day, per the
 * owner's "let's make a round trip 30min" - the lap length is specified
 * directly by the owner both times, not derived, so there is no
 * independent arithmetic to check here beyond TICKS_PER_LAP*TICK_STEP_S
 * landing on exactly 1800.
 *
 * LAPS_MAX = 2 - SUPERSEDED FROM 6, same correction: "a double trip 60min"
 * means two 30-minute laps, not six 10-minute ones. 2 laps * 30 minutes is
 * still the owner's original 60-minute ceiling exactly (MAX_TICKS and
 * TIMER_MAX_SECONDS both still come out whole, same "the numbers happen to
 * land exactly" property the original 90-tick table and the first coil
 * both had).
 *
 * Per-step arc length: TICKS_PER_LAP steps around a circle of circumference
 * 2*pi*RING_OUTER_R (173, see the ring-geometry section below) is
 * 2*pi*173/360 = ~3.0px per step. This is a THIRD of what a 120-tick lap
 * at a similar radius would give (three times as many ticks sharing the
 * same circumference) - well under a fingertip's own resolving power, and
 * close to (a little more generous than) the owner's own estimate of
 * "roughly 2.6 pixels". See this file's header, "Second, and this is the
 * one requiring judgement", and drag_touch()'s own comment for what this
 * means for dragging in practice and what this file does about it.
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Tick geometry: the coil, CURRENT (2026-08-14, real-hardware pass). 15
 * minutes a lap, 2 laps, a 30-minute ceiling.
 *
 * TICK_STEP_S = 5, unchanged - the flat step itself was never in question,
 * only the lap length.
 *
 * TICKS_PER_LAP is now 15 minutes' worth of the flat step (900s / 5s = 180)
 * - SUPERSEDED FROM 360 (30 minutes), the owner's own instruction after
 * testing the 30-minute coil on hardware ("15 minutes for one round"), not
 * derived from anything else.
 *
 * LAPS_MAX stays 2 ("2 rounds max", unchanged from the previous pass) -
 * 2 laps * 15 minutes is a 30-minute ceiling, HALF the previous 60-minute
 * one (MAX_TICKS = 360, TIMER_MAX_SECONDS = 1800, both exact - same
 * "TICKS_PER_LAP*TICK_STEP_S lands on a whole number of minutes" property
 * every earlier version of this table had).
 *
 * Per-step arc length: 180 steps around the SAME circumference as before
 * (2*pi*RING_OUTER_R, RING_OUTER_R still 173 - see "Ring geometry: the
 * coil, CURRENT" below for why the outer radius itself did not move) is
 * 2*pi*173/180 = ~6.0px per step - DOUBLE the previous ~3.0px, since half
 * as many ticks now share the same circumference. This is the "one welcome
 * consequence" the owner's own brief for this pass named: a 5-second tick a
 * fingertip could resolve even less precisely than before is now resolved
 * MORE precisely, for free, as a side effect of the shorter lap - see
 * DRAG_COMMIT_HYSTERESIS_TICKS below for what this buys back on the fine-
 * drag jitter question the previous pass left open.
 * ------------------------------------------------------------------- */
#define TICK_STEP_S       5
#define TICKS_PER_LAP     180                               // 15:00 / 5s - SUPERSEDED FROM 360
#define LAPS_MAX          2
#define MAX_TICKS         (TICKS_PER_LAP * LAPS_MAX)        // 360, i.e. 30:00 - SUPERSEDED FROM 720
#define TIMER_MAX_SECONDS (MAX_TICKS * TICK_STEP_S)         // 1800, 30 minutes - SUPERSEDED FROM 3600

// Ring centre, in LANDSCAPE coordinates (448 wide x 368 tall). Centred on
// the canvas, same as every earlier version of this file (the coil's
// centre does not move; only what surrounds it does).
#define RING_CX      224   // LAND_W / 2
#define RING_CY      184   // LAND_H / 2

/* ---------------------------------------------------------------------
 * Ring geometry: the coil. - SUPERSEDED 2026-08-14 (real-hardware pass),
 * kept verbatim as the record of the 6px-band/16px-span coil this project
 * shipped and tested before the owner's real-finger session. See this
 * file's header, "CORRECTED 2026-08-14 (AFTER REAL-HARDWARE USE)": "double
 * the width of the coil band again" replaces the previous pass's single
 * doubling, same mechanism, one more doubling of the total radial span.
 *
 * Two bands, wound inward (lap 1 outermost, lap 2 innermost - see this
 * file's header for why inward, not outward: the outer diameter must never
 * grow), each its own annulus (a light grey track under a black arc,
 * exactly the single ring's own look, just two of them), separated by a
 * visible white gap so "at two laps you see two rings" per the owner's
 * Apple-activity-rings reference.
 *
 * WORKED FROM THE PANEL, NOT GUESSED, same two limits every earlier version
 * of this ring was solved against (RING_CY, LAND_H/2, is the tighter canvas
 * half-dimension; the digit block's half-diagonal is the other):
 *
 *   EDGE LIMIT:   RING_CY = 184 (unchanged - LAND_W is 448, LAND_H is 368,
 *                 the latter's half is the binding one).
 *   DIGIT LIMIT:  ~145.0px (DIGIT_BLOCK_W/DIGIT_H's half-diagonal - see the
 *                 digit-layout section below for DIGIT_BLOCK_W's own
 *                 derivation, unchanged since the digits themselves did not
 *                 move: sqrt(132^2 + 60^2) = sqrt(21024) = 144.997, i.e.
 *                 145.0).
 *
 * SUPERSEDED FROM SIX BANDS, same day: "double the width of the coil" plus
 * dropping from six laps to two frees up real room, and the owner's
 * instruction is to spend it, not bank it as extra margin.
 *
 * BAND_THICK_PX = 6, BAND_GAP_PX = 4 - both DOUBLE the six-band coil's own
 * 3px/2px (the owner's literal instruction, "double the width of the coil
 * band"; the gap is scaled by the same factor to keep the same visual
 * proportion between ink and separation that the six-band version already
 * settled on, rather than picking a new, unrelated gap value). Two bands
 * plus one gap between them is 2*6 + 1*4 = 16px of total radial span.
 *
 * RING_OUTER_R = 173: the outer edge of band 0 (lap 1), and therefore the
 * whole coil's outer edge, forever - "the dial's outer diameter never
 * grows" per the owner's brief. Checked against the edge limit:
 * 184 - 173 = 11px margin.
 *
 * RING_INNER_R = 157: the inner edge of band 1 (lap 2, innermost),
 * RING_OUTER_R minus both bands' thickness and the one gap between them
 * (173 - 2*6 - 1*4 = 173 - 12 - 4 = 157). Checked against the digit limit:
 * 157 - 145.0 = 12px margin.
 *
 * NEITHER NUMBER WAS CHOSEN TO MATCH THE ORIGINAL SINGLE RING - the outer
 * radius and both margins (11px edge, 12px digit) come out EXACTLY equal
 * to the single ring's own RING_OUTER_R/RING_INNER_R/margins from before
 * there were bands at all. That is a genuine coincidence of the arithmetic
 * (16px of total span for two 6px bands + one 4px gap is the same total
 * span the single ring's own 16px-thick band occupied, RING_HALF_THICK*2),
 * worth stating plainly rather than passing off as deliberate: this
 * derivation was run the same way as the six-band version (place
 * BAND_THICK_PX/BAND_GAP_PX first, size RING_OUTER_R against the edge
 * limit next, read off the digit margin last) and it happens to land back
 * on numbers this device already shipped and measured once before, which
 * is a reasonable, independent point in this file's favour, not a target
 * that was aimed at.
 *
 * "keep the innermost band legible": no longer the tight case it was for
 * six 3px bands (that flag is superseded along with the six-band geometry
 * it described) - 6px is comfortably thicker than anything else on this
 * panel drawn as a stack of 1px rows except the original 16px ring itself,
 * and the 4px gap between the two bands is wide enough to read as a real
 * boundary rather than a hairline at a glance.
 *
 * band_outer_r(b)/band_inner_r(b)/band_centerline_r(b), b = 0 (outermost,
 * lap 1) .. LAPS_MAX-1 (innermost, lap 2): the per-band radii every other
 * function in this file reads. Functions, not a table, because they are
 * pure arithmetic on a compile-time stride (BAND_STRIDE_PX) and every
 * caller already has its own reasons to loop over bands, so a
 * precomputed table would just be these same three lines evaluated once
 * per band into an array nobody re-reads more than a handful of times per
 * call.
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Ring geometry: the coil, CURRENT (2026-08-14, real-hardware pass).
 * "Double the width of the coil band again", the owner's own instruction
 * after using the 6px-band coil with a real finger - unlike the previous
 * doubling, this one does NOT come with more room freed by fewer laps
 * (LAPS_MAX stays 2), so the extra width has to come out of the digit
 * budget instead, which the owner explicitly authorised: "quitte à réduire
 * la taille du minuteur au milieu" (shrink the timer's centre digits if
 * that is what it costs). See the digit-layout section below for what
 * that shrink actually is and what it trades away.
 *
 * Same two limits as every earlier pass, re-checked against the NEW digit
 * size instead of re-derived from scratch:
 *
 *   EDGE LIMIT:  RING_CY = 184, unchanged (the panel did not change size).
 *   DIGIT LIMIT: ~127.1px - the SHRUNK digit block's own half-diagonal (see
 *                below): sqrt(116^2 + 52^2) = sqrt(16160) = 127.12.
 *
 * BAND_THICK_PX = 12, BAND_GAP_PX = 8 - both DOUBLE the previous pass's own
 * 6px/4px, the owner's literal instruction applied a second time; the gap
 * is scaled by the same factor again, same reasoning as last time (keep the
 * ink-to-separation proportion the previous pass already settled on rather
 * than picking a new, unrelated gap value). Two bands plus one gap between
 * them is now 2*12 + 1*8 = 32px of total radial span - DOUBLE the previous
 * pass's own 16px, exactly matching "double the width... again" taken
 * literally at the level of total span, not just the band's own thickness.
 *
 * RING_OUTER_R stays 173, UNCHANGED, on purpose: "the dial's outer diameter
 * never grows" is a standing constraint, not merely a default, and there is
 * no instruction to shrink it either - the simplest reading that satisfies
 * "never grows" is to leave it exactly where it already was, checked once
 * more against the (unchanged) edge limit: 184 - 173 = 11px margin, same as
 * every earlier pass.
 *
 * RING_INNER_R = 141: RING_OUTER_R minus both bands' thickness and the one
 * gap between them (173 - 2*12 - 1*8 = 173 - 24 - 8 = 141). Checked against
 * the new digit limit: 141 - 127.12 = 13.9px margin - slightly MORE
 * generous than the previous pass's own 12px, not because a bigger margin
 * was targeted, but because the digit shrink below was sized to stay
 * legible first and fit second (see that section for the actual trade);
 * the margin is simply what was left over once a legible digit size was
 * chosen, read off last, same order this derivation has followed every
 * time: bands first, RING_OUTER_R against the edge limit next, the digit
 * margin last.
 *
 * "keep the innermost band legible": even less of a concern than the
 * previous pass's own note - 12px is thicker than anything else on this
 * panel drawn as a stack of 1px rows, including the coil's own outer band
 * at every earlier size, and the 8px gap reads as an unambiguous boundary
 * even glanced at quickly.
 * ------------------------------------------------------------------- */
#define BAND_THICK_PX      12                                     // SUPERSEDED FROM 6
#define BAND_GAP_PX        8                                      // SUPERSEDED FROM 4
#define BAND_STRIDE_PX     (BAND_THICK_PX + BAND_GAP_PX)          // 20, SUPERSEDED FROM 10
#define RING_OUTER_R       173
#define RING_INNER_R       (RING_OUTER_R - LAPS_MAX * BAND_THICK_PX - (LAPS_MAX - 1) * BAND_GAP_PX) // 141, SUPERSEDED FROM 157
#define RING_ROWS          (2 * RING_OUTER_R)                      // 346
#define BAND_HALF_THICK_PX (BAND_THICK_PX / 2)                     // 6, exact (BAND_THICK_PX is even) - SUPERSEDED FROM 3

static inline float band_outer_r(int band) {
    return (float)RING_OUTER_R - (float)band * (float)BAND_STRIDE_PX;
}
static inline float band_inner_r(int band) {
    return band_outer_r(band) - (float)BAND_THICK_PX;
}
static inline float band_centerline_r(int band) {
    return band_outer_r(band) - (float)BAND_HALF_THICK_PX;
}

// Track (the always-visible full ring under the arc) and tick marks: light
// grey, not black, so the black arc is the one thing the eye reads as "how
// much is left" and the track/marks read as calm background structure.
// gray_to_px (gfx.h) takes 0=black..255=white, so this grey's "ink" is
// (255-TRACK_GRAY)/255. 178 is about 30% ink: picked by building and
// looking at the emulator output, carried over unchanged from the dot
// version's elapsed-dot grey (and now the single-ring version's, and the
// coil's - one shared constant across all three, still the same "clearly
// present but clearly secondary" grey each time it was checked).
#define TRACK_GRAY       178

// No tick marks/graduations outside the ring, in either band alike: the
// exact value is already written large in the middle of the coil, so a
// graduation adds no information it does not already give - unchanged
// reasoning from the single-ring version (see its own retained note
// further up this file's history).

#define TIMER_PI       3.14159265358979323846f
#define TIMER_HALF_PI  (TIMER_PI / 2.0f)
#define TIMER_RAD2DEG  (180.0f / TIMER_PI)
#define TIMER_DEG2RAD  (TIMER_PI / 180.0f)

/* ---------------------------------------------------------------------
 * Digit layout, in LANDSCAPE coordinates. - SUPERSEDED 2026-08-14
 * (real-hardware pass), kept verbatim as the record of the "same typeface
 * as chrono" digits this project shipped and tested before the owner's
 * real-finger session forced a trade between digit size and band width.
 * Owner feedback: "use the same font, same interlines and spacing as the
 * chronometer". DIGIT_W, DIGIT_H, SEG_T, SEP_W and the 12px gap below are
 * chrono.c's own constants (DIGIT_W/DIGIT_H/SEG_T/SEP_W and its X_* deltas,
 * which are all +12), copied rather than guessed at a smaller size the way
 * the old 40/90/12 timer digits were. They are not shared via a header:
 * chrono.c does not expose them, and a shared layout header for two apps is
 * a bigger call than this task (see the owner's brief). If chrono.c's
 * metrics ever change, this block has to change with it by hand; that
 * duplication is accepted on purpose, not missed. Unchanged by the
 * dots-to-ring-to-coil rewrites: the digits are pixel-identical to before,
 * every time.
 *
 * MM:SS is 4 digits, not chrono's 6, and one colon, not two, so the block
 * is narrower than chrono's; everything else (digit size, gap, colon
 * width) is identical, which is what makes it read as the same typeface.
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Digit layout, in LANDSCAPE coordinates, CURRENT (2026-08-14,
 * real-hardware pass). No longer pixel-identical to chrono's: "double the
 * width of the coil band again" (see the ring-geometry section above)
 * needed 16px more of radial span than the previous pass's digit budget
 * had spare, and the owner traded it away explicitly - "quitte à réduire la
 * taille du minuteur au milieu" (shrink the centre digits if that is what
 * it costs) - rather than leaving the request unresolved.
 *
 * THE TRADE, stated plainly rather than left implicit: every digit
 * dimension below is 7/8 of chrono's own (DIGIT_W 48->42, DIGIT_H 120->104,
 * SEG_T 18->16 rounded up from 15.75 for a marginally bolder stroke rather
 * than a thinner one, SEP_W 24->20 rounded down to keep DIGIT_BLOCK_W an
 * even number - see its own comment). A 7/8 scale (87.5%) was chosen over a
 * more aggressive shrink (e.g. the 5/6 = 83.3% this file considered and
 * rejected) because 7/8 already clears the digit limit with a margin
 * (13.9px) in line with every earlier pass's own margin discipline (11-12px
 * elsewhere in this file) - there was no need to shrink further just
 * because more margin was available, and a 5/6 digit reads smaller to a
 * child without buying back anything the coil needed. The segments stay
 * roughly the same PROPORTION of their own cell as chrono's (SEG_T/DIGIT_W
 * was 0.375, is now 0.381; SEG_T/DIGIT_H was 0.150, is now 0.154), so the
 * numerals are a scaled-down copy of the same shape, not a visually
 * different, thinner font - the legibility that survives is "same digit,
 * smaller", not "different, harder-to-read digit at the old size".
 *
 * DIGIT_GAP = 11, not an exact 7/8 of chrono's 12 (10.5) - rounded UP for
 * the same reason SEG_T rounded up: a hair more breathing room between
 * digits reads clearer at a smaller size than a hair less would, and the
 * rounding direction was free to choose (10 and 11 both fit the radius
 * budget) so it went toward legibility rather than toward the smallest
 * possible footprint.
 * ------------------------------------------------------------------- */
#define DIGIT_W   42   // SUPERSEDED FROM 48 (chrono.c's own DIGIT_W) - 7/8 scale
#define DIGIT_H   104  // SUPERSEDED FROM 120 (chrono.c's own DIGIT_H) - 7/8 scale, rounded to even
#define SEG_T     16   // SUPERSEDED FROM 18 (chrono.c's own SEG_T) - 7/8 scale (15.75), rounded up
#define SEP_W     20   // SUPERSEDED FROM 24 (chrono.c's own SEP_W) - 7/8 scale (21), rounded down to keep DIGIT_BLOCK_W even
#define DIGIT_GAP 11   // SUPERSEDED FROM 12 (chrono.c's own inter-element gap) - 7/8 scale (10.5), rounded up

// MM:SS block: 2 digits, colon, 2 digits, 4 gaps between the 5 elements.
#define DIGIT_BLOCK_W (4 * DIGIT_W + SEP_W + 4 * DIGIT_GAP)  // 232, SUPERSEDED FROM 264

// Centred on the ring, both axes. DIGIT_Y0 no longer matches chrono.c's own
// Y0 (chrono kept its full-size digits; only timer's shrank) - see the
// digit-layout comment above for why the two are now allowed to diverge.
#define DIGIT_Y0  (RING_CY - DIGIT_H / 2)          // 132, SUPERSEDED FROM 124
#define DIGIT_X0  (RING_CX - DIGIT_BLOCK_W / 2)     // 108, SUPERSEDED FROM 92

#define X_MM_TENS   (DIGIT_X0)
#define X_MM_UNITS  (X_MM_TENS  + DIGIT_W + DIGIT_GAP)
#define X_COLON     (X_MM_UNITS + DIGIT_W + DIGIT_GAP)
#define X_SS_TENS   (X_COLON    + SEP_W   + DIGIT_GAP)
#define X_SS_UNITS  (X_SS_TENS  + DIGIT_W + DIGIT_GAP)

/* ---------------------------------------------------------------------
 * The alarm. Sound now exists (firmware/runtime/sound.h, decision 0002
 * section 7): handle_alarm() calls sound_play(SOUND_ID_TIMER_ALARM) once,
 * on the flash-phase flip that used to carry the TODO, and sound_stop() on
 * every dismissal path - see that function for both.
 *
 * ALARM_MAX_MS = 30000: "stop by itself after about 30 seconds" from the
 * brief, taken literally, not measured.
 * ALARM_FLASH_MS = 250: half of a 500ms full white-black-white cycle, i.e.
 * 2Hz, "noticeable without being frightening" per the brief. A guess at
 * what reads as calm rather than a fast, seizure-risk strobe; not tested
 * against the real panel or a real child. If it reads as too fast or too
 * slow on hardware, this is the constant to change.
 *
 * Corrected 2026-08-13: the owner simplified dismissal after seeing this
 * section's first pass. It used to read "any input... stops on any input"
 * without saying what state that leaves behind, and this file's own
 * handle_alarm() comment (see below) used to say "back to the value that
 * was set, not to zero", mirroring BOOT's behaviour in RUNNING/PAUSED. The
 * owner's actual instruction is simpler and wins: dismissal (any button, a
 * shake, or a touch) silences the sound and the flash immediately and
 * lands in SETTING at 00:00, a clean dial. BOOT still recalls the last
 * value that was set (timer_state_t.lastSetTicks, above) once SETTING is
 * showing zero, so "again is one press" survives in a different shape: a
 * dedicated recall action rather than the dial staying pre-loaded.
 * ------------------------------------------------------------------- */
#define ALARM_MAX_MS    30000
#define ALARM_FLASH_MS  250

/* ---------------------------------------------------------------------
 * State machine. Every state is visible on screen through TWO independent
 * signals, so the same physical inputs (drag, PWR short press, BOOT click)
 * are never ambiguous:
 *
 *   1. The digits' colour, which carries the FULL weight of telling
 *      SETTING, RUNNING and PAUSED apart:
 *        SETTING  light grey ("not committed yet")
 *        RUNNING  solid black ("live, counting down")
 *        PAUSED   darker grey ("frozen")
 *      ALARM does not draw digits at all; see below.
 *
 *   2. The COIL'S SHAPE does not distinguish SETTING from RUNNING/PAUSED,
 *      on purpose, same as every earlier version of this file: a freshly-
 *      dragged SETTING coil and a freshly-started RUNNING coil at the same
 *      value look identical, and that is fine because the digit colour
 *      already answers "which state is this" unambiguously. What the coil
 *      DOES show, in every non-alarm state alike, is one continuous fact
 *      per band: how far around that band's own 360 degrees the current
 *      value has wound it (see compute_band_fill_degs - each band's own
 *      fraction of TICKS_PER_LAP, not a fraction of the whole 30-minute
 *      range). RUNNING and PAUSED differ from each other only in whether
 *      those arcs are currently moving; a single snapshot cannot tell them
 *      apart either, which is again why the digit colour exists.
 *
 *      TS_PAUSED gained two new reactions on 2026-08-14, both scoped to
 *      "the dial is editable": a TOUCH converts it into TS_SETTING,
 *      pre-loaded from the paused value - see this file's header, "PAUSE,
 *      THEN EDIT" - and a SHAKE clears it straight to 00:00 - see
 *      timer_tick()'s shake-clear branch and g_timerApp's wantsShake
 *      comment at the bottom of this file. Everything above still holds
 *      once either happens: the touch reaction works BY becoming SETTING,
 *      not by PAUSED growing a third visual language of its own, and the
 *      shake reaction is the exact same clear-to-zero-and-become-SETTING
 *      shape handle_alarm()'s own dismissal already uses.
 *
 * ALARM is unambiguous by construction: it is the only state that flashes
 * the entire panel, which nothing else ever does.
 * ------------------------------------------------------------------- */
typedef enum {
    TS_SETTING,
    TS_RUNNING,
    TS_PAUSED,
    TS_ALARM,
} timer_state_e;

typedef struct {
    timer_state_e state;

    // 0..MAX_TICKS. Doubles as "the value currently being edited" while
    // SETTING and "the value that was committed" once RUNNING/PAUSED -
    // exactly the role this field always had, only its range and per-tick
    // meaning changed (0..90 piecewise -> 0..720 flat 5s) with the coil.
    int setTicks;
    int remainingSeconds;  // the real countdown, RUNNING/PAUSED only
    uint32_t lastDecMs;    // f->nowMs anchor for the once-per-second decrement

    // The last non-zero setTicks this session had, before a dismissal or a
    // shake-clear zeroed setTicks itself - see handle_alarm()'s "Corrected
    // 2026-08-13" comment and timer_tick()'s shake-clear branch. BOOT,
    // pressed while SETTING is showing a fresh 00:00, recalls this so
    // "again" is still one press, without the dial having to stay non-zero
    // to make that possible.
    int lastSetTicks;

    // Continuous drag state, new for the coil - see point_touch()/
    // drag_touch()'s own comments and this file's header, "POINT VS DRAG,
    // AND THE BRANCH CUT". dragAccumTicks is the float accumulator a drag's
    // per-frame unwrapped deltas add into, initialised to setTicks by the
    // point_touch() that starts (or restarts) every touch context;
    // lastTouchAngleDeg is the previous frame's raw touch angle, the
    // reference drag_touch()'s unwrap needs. Both are meaningless outside
    // an active SETTING touch and are always re-initialised by the next
    // point_touch() before drag_touch() ever reads them - see timer_tick()'s
    // TS_SETTING branch, which never calls drag_touch() on the same frame
    // as a point_touch().
    float dragAccumTicks;
    float lastTouchAngleDeg;

    // CORRECTED 2026-08-14: dropout bridging for the drag itself, not just
    // its rendering. lastTouchDownMs is the nowMs of the most recent frame
    // this file saw f->touchDown true (set after every point_touch()/
    // drag_touch() call in the TS_SETTING branch, and after the TS_PAUSED-
    // touched-to-edit branch's own point_touch()); touchContextLive is false
    // only before the very first touch a SETTING session has ever seen. Read
    // together by the TS_SETTING branch's own dispatch to tell a genuine
    // fresh touchPressed (a real lift, then a new touch-down elsewhere) apart
    // from one the runtime's edge detector manufactured out of the real touch
    // controller dropping contact mid-drag - see that branch's own comment,
    // and this task's report, for the mechanism this exists to close: without
    // it, EVERY dropout-induced touchPressed reached point_touch(), which
    // reseeds dragAccumTicks and the sub-lap position from a fresh snapshot,
    // discarding however much of the current lap the drag had already wound
    // in. That is what "winding past the end of the first lap restarts at
    // zero" actually was - not a wraparound in the accumulator (which the
    // existing branch-cut tests already proved correct), but the accumulator
    // being thrown away and re-seeded mid-gesture by a press that was never a
    // real lift.
    uint32_t lastTouchDownMs;
    bool touchContextLive;

    // One arc angle per band (0..360 each), the coil's generalisation of
    // the single ring's lastFillDeg - see paint_ring_row()/update_ring_to().
    float lastFillDeg[LAPS_MAX];
    int lastDigitSeconds;   // seconds value currently painted in the MM:SS cells

    // True (fractional) remaining seconds, captured the instant PAUSED is
    // entered. PAUSED must not keep deriving this from f->nowMs the way
    // RUNNING does: nowMs keeps advancing while paused but lastDecMs does
    // not, so that formula would run away the longer the pause lasts. This
    // is what lets PAUSED show the frozen arc angles rather than either
    // resetting them or corrupting them, and it is also the value pause-
    // then-edit converts into an initial setTicks - see the TS_PAUSED
    // branch in timer_tick().
    float pausedTrueRemaining;

    uint32_t alarmStartMs;
    bool alarmInverted;    // current flash phase; a push happens only on a flip
} timer_state_t;

// s_state is a pointer into the arena, not the state itself: see chrono.c's
// identical comment on the same pattern, and app.h's arena section for why a
// file-scope struct is the thing that is not acceptable, not a 4-byte
// pointer that has nowhere else to live between enter() and tick().
static timer_state_t *s_state;

/* ---------------------------------------------------------------------
 * Seconds <-> ticks. Flat 5s step, everywhere - see this file's header for
 * why this collapsed from a three-tier table into one line each. Used to
 * turn a tick count (from a drag, or from BOOT's recalled setTicks) into
 * the seconds value the digits show, and back.
 * ------------------------------------------------------------------- */
static int seconds_for_ticks(int ticks) {
    if (ticks <= 0) return 0;
    if (ticks > MAX_TICKS) ticks = MAX_TICKS;
    return ticks * TICK_STEP_S;
}

static float tick_index_for_seconds(float sec) {
    if (sec <= 0.0f) return 0.0f;
    if (sec > (float)TIMER_MAX_SECONDS) sec = (float)TIMER_MAX_SECONDS;
    return sec / (float)TICK_STEP_S;
}

/* ---------------------------------------------------------------------
 * The coil: LAPS_MAX concentric annuli (track + arc each), drawn as stacks
 * of horizontal bars, same technique the single ring used - see this
 * file's header comment.
 *
 * s_hwOuter[b]/s_hwInner[b] are one outer/inner half-width table PER BAND,
 * built once, lazily, via shapes.h's table builder, all sharing the SAME
 * RING_ROWS-tall grid (centred on RING_OUTER_R, the outermost band's own
 * radius - see shapes_fill_half_width_table's "centred at rows/2.0"
 * contract): row 0 is the coil's topmost possible pixel row (band 0's own
 * top), row RING_ROWS-1 its bottommost, and EVERY band's tables are indexed
 * against that one shared grid rather than each having its own - a row that
 * is outside a given (smaller) band's own radius just reads 0 from that
 * band's table, which paint_band_row already treats as "this band does not
 * reach this row".
 * ------------------------------------------------------------------- */
static int16_t s_hwOuter[LAPS_MAX][RING_ROWS];
static int16_t s_hwInner[LAPS_MAX][RING_ROWS];
static bool s_ringTablesReady = false;

static void ensure_ring_tables(void) {
    if (s_ringTablesReady) return;
    for (int b = 0; b < LAPS_MAX; b++) {
        shapes_fill_half_width_table(s_hwOuter[b], RING_ROWS, band_outer_r(b));
        shapes_fill_half_width_table(s_hwInner[b], RING_ROWS, band_inner_r(b));
    }
    s_ringTablesReady = true;
}

/* ---------------------------------------------------------------------
 * Angle, and where it gets fiddly. Unchanged from the single ring - the
 * coil reuses this section's functions exactly as written, per band,
 * because the branch-cut problem they solve (reading ONE angle honestly at
 * dx=0/dy>0) is identical for a coil band and for the single ring that used
 * to be the only one. See this file's header, "POINT VS DRAG, AND THE
 * BRANCH CUT", for the SECOND, related but distinct branch-cut problem the
 * coil newly has to solve (reading a CHANGE in angle across that same cut,
 * for drag_touch()'s lap-crossing) - that one is new code, further down.
 *
 * Convention, kept from the dot version: 0 degrees is 12 o'clock, increasing
 * CLOCKWISE (matching the screen, not math convention - see the derivation
 * this replaces for why: landscape y increases downward, which flips the
 * usual counterclockwise-positive sense). Each band's own arc always starts
 * at 0 and covers [0, fillDeg), so unlike an arbitrary start+sweep there is
 * no generic wraparound to handle: the only question per pixel is whether
 * its own clockwise-from-12 angle is less than that band's fillDeg.
 *
 * phi_deg_at() computes that angle via atan2f(dx, -dy): a "compass bearing"
 * form (0 = north/up, clockwise positive) whose branch cut sits at dx=0,
 * dy>0 - straight down, 6 o'clock - rather than at the more common negative-
 * x-axis cut a plain atan2f(dy,dx)+90 would have (which lands at 9 o'clock,
 * inside the ring's usable area). Every caller below excludes the dx=0
 * column from its two generic bars and handles that one column directly
 * with a hardcoded angle (0 above centre, 180 below), so the branch cut is
 * never evaluated through this function at all - see paint_band_row.
 * ------------------------------------------------------------------- */
static float phi_deg_at(float dx, float dyCenter) {
    float raw = atan2f(dx, -dyCenter) * TIMER_RAD2DEG;
    return raw < 0.0f ? raw + 360.0f : raw;
}

// phi at the centre of pixel COLUMN dx (an integer offset from the ring's
// centre), at the row whose vertical centre is dyCenter.
static float phi_deg_for_col(int dx, float dyCenter) {
    return phi_deg_at((float)dx + 0.5f, dyCenter);
}

// Paints one bar (dxLo..dxHi inclusive, both the same sign - see
// paint_band_row for why dx=0 is never in here) black up to fillDeg and grey
// track beyond it. phi(dx) is monotonic across any such bar (atan2f(dx,-dy)
// is monotonic in dx whenever dx never crosses 0, which is exactly the
// condition every caller guarantees), so there is at most one colour
// transition in the whole bar and a binary search finds it in a handful of
// atan2f calls instead of testing every pixel. Unchanged from the single
// ring: this function does not know or care which band it is painting, it
// only ever sees a dx range, a dyCenter and a fillDeg.
static void paint_ring_bar(int y, int dxLo, int dxHi, float dyCenter, float fillDeg) {
    float phiLo = phi_deg_for_col(dxLo, dyCenter);
    float phiHi = phi_deg_for_col(dxHi, dyCenter);
    bool loBlack = phiLo < fillDeg;
    bool hiBlack = phiHi < fillDeg;

    if (loBlack == hiBlack) {
        // Whole bar reads the same way: no split needed.
        uint16_t c = loBlack ? PX_BLACK : gray_to_px(TRACK_GRAY);
        gfx_fill_rect_land(RING_CX + dxLo, y, dxHi - dxLo + 1, 1, c);
        return;
    }

    int lo = dxLo, hi = dxHi; // lo stays on the loBlack side, hi on the other
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        bool midBlack = phi_deg_for_col(mid, dyCenter) < fillDeg;
        if (midBlack == loBlack) lo = mid; else hi = mid;
    }
    uint16_t cLo = loBlack ? PX_BLACK : gray_to_px(TRACK_GRAY);
    uint16_t cHi = hiBlack ? PX_BLACK : gray_to_px(TRACK_GRAY);
    gfx_fill_rect_land(RING_CX + dxLo, y, lo - dxLo + 1, 1, cLo);
    gfx_fill_rect_land(RING_CX + hi, y, dxHi - hi + 1, 1, cHi);
}

// Paints band `band`'s own portion of landscape row y (rowIdx = y's index
// into the shared RING_ROWS grid), at that band's own fillDeg. Splits into
// up to two bars (left, right) plus the single dx=0 column on rows with no
// hole for THIS band (its own caps, where hwInner is 0): the single-ring
// version's paint_ring_row body, unchanged in substance, just parameterised
// over which band's tables and fillDeg to read. Bands whose hwOuter is 0 at
// this row (the row is outside that band's own radius) are skipped - the
// whole reason LAPS_MAX independent per-band tables exist rather than one
// shared one.
static void paint_band_row(int y, int rowIdx, int band, float fillDeg) {
    int hwOuter = s_hwOuter[band][rowIdx];
    int hwInner = s_hwInner[band][rowIdx];
    if (hwOuter <= 0) return;
    float dyCenter = ((float)rowIdx + 0.5f) - (float)RING_OUTER_R; // shared grid: same formula for every band

    if (hwInner <= 0) {
        float phi0 = dyCenter < 0.0f ? 0.0f : 180.0f;
        uint16_t c0 = phi0 < fillDeg ? PX_BLACK : gray_to_px(TRACK_GRAY);
        gfx_fill_rect_land(RING_CX, y, 1, 1, c0);
    }

    int leftLo = -hwOuter;
    int leftHi = hwInner > 0 ? -hwInner : -1;
    if (leftLo <= leftHi) paint_ring_bar(y, leftLo, leftHi, dyCenter, fillDeg);

    int rightLo = hwInner > 0 ? hwInner : 1;
    int rightHi = hwOuter;
    if (rightLo <= rightHi) paint_ring_bar(y, rightLo, rightHi, dyCenter, fillDeg);
}

// Paints every band's portion of landscape row y, at each band's own
// fillDeg (fillDeg[0] = outermost/lap 1 .. fillDeg[LAPS_MAX-1] =
// innermost/last lap). The white gap between bands is never written by
// this function (only each band's own [hwInner, hwOuter] shell is): it is
// established once, correctly, by the runtime clearing the framebuffer to
// white before enter() (app.h), and stays correct forever because the
// coil's geometry is static - no band ever changes size, only its own
// arc's fillDeg does - so nothing ever needs to repaint the gap.
static void paint_ring_row(int y, const float fillDeg[LAPS_MAX]) {
    int rowIdx = y - (RING_CY - RING_OUTER_R);
    if (rowIdx < 0 || rowIdx >= RING_ROWS) return;
    for (int b = 0; b < LAPS_MAX; b++) paint_band_row(y, rowIdx, b, fillDeg[b]);
}

/* ---------------------------------------------------------------------
 * Rounded caps. Owner feedback, with an Apple Watch activity-ring
 * screenshot as the reference: the arc's ends must be ROUNDED, not the
 * sharp radial cuts paint_band_row/paint_ring_bar produce on their own -
 * unchanged reasoning from the single ring, now applied per band, and
 * genuinely visible again now that BAND_HALF_THICK_PX is back to a real
 * radius (3px) rather than the cramped six-band coil's 1.5px.
 *
 * A rounded cap is a filled disc, radius BAND_HALF_THICK_PX, centred on
 * that band's own CENTRELINE radius (band_centerline_r(band), midway
 * between that band's inner and outer edge) at the cap's angle. Two caps
 * per band with a nonzero fillDeg: one fixed at 0 degrees (every band's arc
 * always starts at 12 o'clock) and one at that band's current fillDeg (its
 * own moving tip). Built from shapes.h's table builder - one table,
 * s_capHw, shared by every band, because every band has the identical
 * BAND_HALF_THICK_PX; only the CENTRE the table is stamped around differs
 * per band and per angle (cap_center()).
 *
 * CORRECTED 2026-08-14: draw_cap() no longer hands s_capHw straight to
 * shapes_draw_annulus_row(). Owner report, verbatim: "sur le minuteur j'ai
 * des pixels qui stray autour de l'anneau" - stray pixels around the coil.
 * Bisected in the emulator (a clean drag, no dropouts, no touch involved at
 * all in the mechanism): a handful of black pixels stay lit permanently in
 * the white GAP between the two bands, each one born the instant a cap
 * first sweeps past that angle and never erased again, because nothing
 * else ever repaints the gap (see paint_ring_row's own comment - "the gap
 * is never written by this function... stays correct forever" - true only
 * if nothing else writes there either).
 *
 * The mechanism: shapes_fill_half_width_table() rounds each row's half-
 * width to the nearest pixel (lroundf), which is a genuine circle at the
 * ROW level but not at the PIXEL-corner level - CAP_TABLE_ROWS=6's own
 * middle rows (dy=+-1.5) round sqrt(9-2.25)=2.598 UP to 3, so their
 * outermost drawn pixel sits at local distance sqrt(3^2+1.5^2)=3.354 from
 * the cap's centre, not the nominal BAND_HALF_THICK_PX=3 - an inherent,
 * known property of table-based circle rasterisation, not a bug on its
 * own (every row-stack shape in this file has some corner rounding; that
 * is what CAP_BULGE_TOLERANCE-style margins elsewhere in this codebase
 * already budget for). Stacked with cap_center()'s own lroundf() snapping
 * the cap's true (float) centre to the nearest integer pixel (up to
 * ~0.7px of further slack on the diagonal), the two roundings COMBINED can
 * push a cap pixel's true distance from the RING's centre - not the cap's
 * own centre - up to about 1px past the band's own declared inner or outer
 * radius. Measured directly: a cap drawn near 117 degrees left exactly one
 * such pixel at radius 165.9, inside RING_INNER_R-adjacent band 0's own
 * territory begins at 167 - a permanent, one-pixel-wide fleck in the gap.
 *
 * THE FIX does not try to out-guess the rounding (shrinking BAND_HALF_
 * THICK_PX's own table radius by some fudge factor would either still
 * leave a corner case or visibly flatten the cap, trading one imprecision
 * for another). Instead, draw_cap() clips every row it draws against
 * s_hwOuter[band]/s_hwInner[band] - the EXACT SAME per-row bounds
 * paint_band_row() itself uses to decide what belongs to this band. A cap
 * pixel can then, by construction, never fall outside what the rest of
 * this file already calls "band X's own territory" at that row, because it
 * is being measured against the identical yardstick - see
 * draw_cap_row_clipped() below.
 * ------------------------------------------------------------------- */
#define CAP_TABLE_ROWS (2 * BAND_HALF_THICK_PX) // 6, exact - BAND_HALF_THICK_PX is a
                                                 // whole number this time (BAND_THICK_PX
                                                 // is even), so unlike the six-band
                                                 // coil's cramped 1.5px half-thickness
                                                 // this needs no headroom row for a
                                                 // float ceil() the preprocessor cannot
                                                 // do - same exact sizing the ORIGINAL
                                                 // single ring used for its own
                                                 // 2*RING_HALF_THICK.
static int16_t s_capHw[CAP_TABLE_ROWS];
static bool s_capTableReady = false;

static void ensure_cap_table(void) {
    if (s_capTableReady) return;
    shapes_fill_half_width_table(s_capHw, CAP_TABLE_ROWS, (float)BAND_HALF_THICK_PX);
    s_capTableReady = true;
}

static void cap_center(int band, float deg, int *cx, int *cy) {
    float mathAngle = (deg - 90.0f) * TIMER_DEG2RAD;
    float r = band_centerline_r(band);
    *cx = RING_CX + (int)lroundf(r * cosf(mathAngle));
    *cy = RING_CY + (int)lroundf(r * sinf(mathAngle));
}

// Draws the portion of [dxLo, dxHi] (RING_CX-relative dx, inclusive) at
// landscape row y that band `band` actually owns at that row, per
// s_hwOuter[band]/s_hwInner[band] - the same tables paint_band_row() reads,
// so a cap can never disagree with what the rest of this file considers
// that band's own territory. Mirrors paint_band_row's own two-bars-or-solid
// logic (see that function's comment) rather than calling it, because that
// function paints EVERY band at a row unconditionally; this one draws only
// the single band a single cap belongs to, clipped to an arbitrary input
// range rather than the row's own full width.
static void draw_cap_row_clipped(int band, int y, int rowIdx, int dxLo, int dxHi, uint16_t color) {
    if (rowIdx < 0 || rowIdx >= RING_ROWS) return; // off the shared grid entirely - defensive, should not happen
    int hwOuter = s_hwOuter[band][rowIdx];
    int hwInner = s_hwInner[band][rowIdx];
    if (hwOuter <= 0) return; // this row is outside band `band` altogether
    if (hwInner <= 0) {
        // Past this band's own inner radius: no hole at this row (same case
        // paint_band_row's own `if (hwInner <= 0)` branch handles), so the
        // whole [-hwOuter, hwOuter] strip is valid, dx=0 included.
        int lo = dxLo < -hwOuter ? -hwOuter : dxLo;
        int hi = dxHi > hwOuter ? hwOuter : dxHi;
        if (lo <= hi) gfx_fill_rect_land(RING_CX + lo, y, hi - lo + 1, 1, color);
        return;
    }
    // Two valid strips, [-hwOuter,-hwInner] and [hwInner,hwOuter], with a
    // hole in between - clip the requested range against each independently
    // so a cap that happens to straddle dx=0 (only possible right at the
    // fixed start cap's own 12 o'clock, or a moving cap that lands exactly
    // at 6 o'clock) still gets both its left and right halves drawn.
    int leftLo = dxLo < -hwOuter ? -hwOuter : dxLo;
    int leftHi = dxHi > -hwInner ? -hwInner : dxHi;
    if (leftLo <= leftHi) gfx_fill_rect_land(RING_CX + leftLo, y, leftHi - leftLo + 1, 1, color);
    int rightLo = dxLo < hwInner ? hwInner : dxLo;
    int rightHi = dxHi > hwOuter ? hwOuter : dxHi;
    if (rightLo <= rightHi) gfx_fill_rect_land(RING_CX + rightLo, y, rightHi - rightLo + 1, 1, color);
}

static void draw_cap(int band, float deg) {
    ensure_cap_table();
    int cx, cy;
    cap_center(band, deg, &cx, &cy);
    int cxRel = cx - RING_CX;
    for (int row = 0; row < CAP_TABLE_ROWS; row++) {
        int hw = s_capHw[row];
        if (hw <= 0) continue;
        int y = cy - CAP_TABLE_ROWS / 2 + row;
        int rowIdx = y - (RING_CY - RING_OUTER_R);
        draw_cap_row_clipped(band, y, rowIdx, cxRel - hw, cxRel + hw - 1, PX_BLACK);
    }
}

// Both caps for one band, or none: with no arc for that band (fillDeg <= 0,
// meaning that lap has not been reached yet) there is nothing to round the
// end of. Same "both or none" rule the single ring used, per band.
static void draw_arc_caps(int band, float fillDeg) {
    if (fillDeg <= 0.0f) return;
    draw_cap(band, 0.0f);
    draw_cap(band, fillDeg);
}

// Paints every band at its own fillDeg across the coil's full bounding box,
// plus every band's caps. Used for enter() and every state transition - see
// redraw_full().
static void paint_ring_full(const float fillDeg[LAPS_MAX]) {
    int yTop = RING_CY - RING_OUTER_R;
    int yBot = RING_CY + RING_OUTER_R - 1;
    for (int y = yTop; y <= yBot; y++) paint_ring_row(y, fillDeg);
    for (int b = 0; b < LAPS_MAX; b++) draw_arc_caps(b, fillDeg[b]);
}

// How far a cap's disc reaches, in DEGREES as seen from the ring's own
// centre, past its exact centre angle - unchanged mechanism from the single
// ring (see its own retained history below for the real hardware bug this
// exists to prevent: a shrinking arc's moving cap paints a little past its
// exact edge on purpose, for the rounded-end look, and an incremental
// repaint that does not account for that overshoot leaves a stuck sliver of
// stale ink behind it).
//
// RECOMPUTED 2026-08-14 (real-hardware pass) for this file's current
// geometry (BAND_HALF_THICK_PX=6, smallest radius at the innermost band,
// b=1: band_centerline_r(1) = 173 - 1*20 - 6 = 147 - SUPERSEDED FROM 160).
// Same tangent-line bound as before, max half-angle = asin(BAND_HALF_
// THICK_PX / radius), evaluated at the SMALLEST radius on purpose (asin
// grows as radius shrinks for a fixed half-thickness, so the innermost band
// is every band's worst case, and one shared conservative constant for all
// bands is simpler and safer than one per band for a quantity this cheap to
// over-provision): asin(6/147) = asin(0.04082) = 2.339 degrees - SUPERSEDED
// FROM 1.074, roughly double, tracking BAND_HALF_THICK_PX's own doubling.
// Not computed at runtime, same reason as before - asinf is not in this
// project's emulator ABI (see emulator/wasm/shim/math.h's header comment) -
// so this is again one more number computed once by reasoning. Rounded up
// to 3.5 degrees (roughly 1.5x the analytic value, the same headroom
// discipline every earlier pass of this constant used). Also worth noting:
// this margin's OWN job shrank with this pass's cap-clipping fix
// (draw_cap_row_clipped(), see this file's header) - a cap can no longer
// paint outside its own band's radius regardless of this margin, so
// CAP_SWEEP_MARGIN_DEG now only has to be wide enough to sweep the rows a
// cap's TANGENTIAL reach touches, not to prevent a radial leak; kept
// generously sized anyway rather than trimmed to the new, narrower job, on
// the same "cheap to over-provision" reasoning as always. Verified
// sufficient by this file's own regression test (repro-ring-shrink-
// residue.ts, extended for the coil and again for this pass's own gap-
// residue defect - see that file), which drives the same drag-up-then-down
// and smooth-countdown scenarios the single ring's version did, now scanning
// both bands.
#define CAP_SWEEP_MARGIN_DEG 3.5f

// Bounding landscape row range [*yLo, *yHi] that could contain any pixel,
// AT THE GIVEN BAND'S OWN RADII, whose angle lies in [fromDeg, toDeg]
// (fromDeg <= toDeg): the two edges of that angular wedge at both radii,
// PLUS any of the four axis angles (0/90/180/270, where dy is at its
// extreme for a given radius) that fall inside the wedge, since the wedge
// can bulge past a straight line between its two edges at those points.
// Generalised from the single ring's own version only by taking outerR/
// innerR as parameters instead of reading the single pair of file-scope
// constants it used to - the geometry and the reasoning are otherwise
// identical, band by band.
static void ring_sweep_row_range(float fromDeg, float toDeg, float outerR, float innerR, int *yLo, int *yHi) {
    float minDy = 1e9f, maxDy = -1e9f;
    float sampleDegs[6];
    int n = 0;
    sampleDegs[n++] = fromDeg;
    sampleDegs[n++] = toDeg;
    static const float axisDegs[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
    for (int i = 0; i < 4; i++) {
        if (axisDegs[i] > fromDeg && axisDegs[i] < toDeg) sampleDegs[n++] = axisDegs[i];
    }
    for (int i = 0; i < n; i++) {
        float mathAngle = (sampleDegs[i] - 90.0f) * TIMER_DEG2RAD;
        float s = sinf(mathAngle);
        float dyOuter = outerR * s;
        float dyInner = innerR * s;
        if (dyOuter < minDy) minDy = dyOuter;
        if (dyOuter > maxDy) maxDy = dyOuter;
        if (dyInner < minDy) minDy = dyInner;
        if (dyInner > maxDy) maxDy = dyInner;
    }
    int lo = RING_CY + (int)floorf(minDy) - 1;
    int hi = RING_CY + (int)ceilf(maxDy) + 1;
    int rangeLo = RING_CY - RING_OUTER_R;
    int rangeHi = RING_CY + RING_OUTER_R - 1;
    *yLo = lo < rangeLo ? rangeLo : lo;
    *yHi = hi > rangeHi ? rangeHi : hi;
}

// Moves the coil from its last-painted per-band angles (s->lastFillDeg[]) to
// newFillDeg[], touching only the rows any CHANGED band's swept wedge could
// have touched, redrawing every band's own caps that are either changed or
// merely at risk (their own cap sits inside the union of rows this call is
// about to repaint anyway - see the loop below), and pushing one rectangle
// covering all of it. Used for BOTH SETTING's point/drag (a point preserves
// every band but the current one; a drag can, and does at a lap boundary,
// move both bands' fillDeg in the same call - see drag_touch()) and
// RUNNING's per-frame shrink (ordinarily one band, the one currently
// unwinding, except exactly at the lap boundary where the band that just
// finished emptying and the band that becomes newly active both move by a
// hair in the same call): the row-range bound handles all of these the same
// way, same as the single ring's own version - correctness does not depend
// on how many bands changed or how far, only the SIZE of the work does.
//
// WHY EVERY AT-RISK BAND'S CAPS ARE REDRAWN, NOT JUST THE CHANGED ONES'
// OWN: because bands are concentric, a single landscape row can carry ink
// from BOTH bands at once (near RING_CY, where |dy| is small, both bands'
// own hwOuter/hwInner tables are still nonzero). A sweep triggered by band
// X's change repaints every band's plain bars at the rows it touches
// (paint_ring_row always loops over all LAPS_MAX bands per row, not just
// the one that moved), which is correct ink for the OTHER band wherever its
// own fillDeg did not move - EXCEPT at the specific rows where its own
// rounded cap used to sit, which a plain bar repaint would flatten back to
// a sharp cut. So after painting the swept rows, every band whose own
// cap-row-span intersects [yLo, yHi] - changed or not - gets its caps
// redrawn on top, the direct per-band generalisation of the single ring's
// "both caps redrawn on every call... simpler to get right than proving the
// sweep already covers them". This costs nothing extra in push-rectangle
// size when the band's cap was already inside [yLo, yHi] (the whole reason
// it is "at risk"); it only grows the push rectangle for the rarer case of
// a band's FIXED start cap sitting outside the current sweep, the same case
// the single ring's own version already had to grow the rectangle for.
static void update_ring_to(timer_state_t *s, const float newFillDeg[LAPS_MAX]) {
    float clamped[LAPS_MAX];
    bool changed[LAPS_MAX];
    bool anyChanged = false;
    for (int b = 0; b < LAPS_MAX; b++) {
        float nd = newFillDeg[b];
        if (nd < 0.0f) nd = 0.0f;
        if (nd > 360.0f) nd = 360.0f;
        clamped[b] = nd;
        changed[b] = (nd != s->lastFillDeg[b]);
        if (changed[b]) anyChanged = true;
    }
    if (!anyChanged) return;

    int yLo = RING_CY + RING_OUTER_R; // widened below; starts inverted/empty
    int yHi = RING_CY - RING_OUTER_R;

    for (int b = 0; b < LAPS_MAX; b++) {
        if (!changed[b]) continue;
        float oldDeg = s->lastFillDeg[b], newDeg = clamped[b];
        float fromDeg = newDeg < oldDeg ? newDeg : oldDeg;
        float toDeg = newDeg < oldDeg ? oldDeg : newDeg;
        float sweepFromDeg = fromDeg - CAP_SWEEP_MARGIN_DEG;
        float sweepToDeg = toDeg + CAP_SWEEP_MARGIN_DEG;
        if (sweepFromDeg < 0.0f) sweepFromDeg = 0.0f;
        if (sweepToDeg > 360.0f) sweepToDeg = 360.0f;
        int by0, by1;
        ring_sweep_row_range(sweepFromDeg, sweepToDeg, band_outer_r(b), band_inner_r(b), &by0, &by1);
        if (by0 < yLo) yLo = by0;
        if (by1 > yHi) yHi = by1;
    }

    for (int y = yLo; y <= yHi; y++) paint_ring_row(y, clamped);

    for (int b = 0; b < LAPS_MAX; b++) {
        if (clamped[b] <= 0.0f) continue; // nothing drawn for this band, nothing to protect
        bool atRisk = changed[b];
        int cx0, cy0, cx1, cy1;
        cap_center(b, 0.0f, &cx0, &cy0);
        cap_center(b, clamped[b], &cx1, &cy1);
        int capLo = (cy0 < cy1 ? cy0 : cy1) - CAP_TABLE_ROWS / 2;
        int capHi = (cy0 > cy1 ? cy0 : cy1) + CAP_TABLE_ROWS / 2 - 1;
        if (!atRisk) atRisk = !(capHi < yLo || capLo > yHi);
        if (!atRisk) continue;
        draw_arc_caps(b, clamped[b]);
        if (capLo < yLo) yLo = capLo;
        if (capHi > yHi) yHi = capHi;
    }

    // Width is 2*RING_OUTER_R + 1, not 2*RING_OUTER_R: a bar's dx range is
    // [-hwOuter, +hwOuter] inclusive, and hwOuter reaches exactly
    // RING_OUTER_R at the rows nearest the coil's own vertical centre
    // (shapes_fill_half_width_table's dy is never exactly 0 - the nearest
    // row centres are +-0.5 - so sqrt(RING_OUTER_R^2 - 0.25) still rounds
    // up to RING_OUTER_R itself). A width of 2*RING_OUTER_R covers dx in
    // [-RING_OUTER_R, RING_OUTER_R - 1] and silently drops the single
    // rightmost column, which the no-pixel-outside-pushed-rectangles
    // invariant (this task's own test, emulator/docs/findings-app-fuzzing.md
    // section 2) caught directly: band 0's outermost column really does
    // flip colour on an incremental update and really was landing outside
    // this rectangle before the +1.
    gfx_push_land(RING_CX - RING_OUTER_R, yLo, 2 * RING_OUTER_R + 1, yHi - yLo + 1);
    for (int b = 0; b < LAPS_MAX; b++) s->lastFillDeg[b] = clamped[b];
}

/* ---------------------------------------------------------------------
 * Touch: raw angle, point, drag. f->touchX/Y arrive in PANEL (portrait)
 * coordinates (app.h), not landscape ones: gfx only rotates rectangles, not
 * points (gfx.h's note on gfx_land_rect), so a landscape app reading a touch
 * position has to invert that mapping itself. gfx_land_rect's corner math
 * says landscape (lx, ly) -> panel (PANEL_W-1-ly, lx); inverted, panel
 * (px, py) -> landscape (lx=py, ly=PANEL_W-1-px). Unchanged from the single
 * ring's own ring_tick_for_touch, which this replaces.
 * ------------------------------------------------------------------- */

// Continuous touch angle, 0..360, 0 at 12 o'clock, clockwise - same
// convention and derivation as the single ring's ring_tick_for_touch, minus
// that function's final snap-to-slot step: the coil needs the RAW angle for
// two different reasons (an absolute point, and a delta for a drag), so
// snapping happens in the two callers below instead of in here.
static float raw_touch_angle_deg(int touchPanelX, int touchPanelY) {
    int lx = touchPanelY;
    int ly = PANEL_W - 1 - touchPanelX;

    float dx = (float)(lx - RING_CX);
    float dy = (float)(ly - RING_CY);
    float norm = atan2f(dy, dx) + TIMER_HALF_PI; // 0 at 12 o'clock
    if (norm < 0.0f) norm += 2.0f * TIMER_PI;
    if (norm >= 2.0f * TIMER_PI) norm -= 2.0f * TIMER_PI;
    return norm * TIMER_RAD2DEG;
}

// The sub-lap tick (0..TICKS_PER_LAP-1) a direct "point" touch at this
// angle selects: the nearest of TICKS_PER_LAP equal slots around the dial,
// rounded (not floored - a tap should land on whichever slot it is
// genuinely closer to, not always the one clockwise-behind it), wrapping
// TICKS_PER_LAP itself back to 0 (an angle essentially AT 12 o'clock
// selects the START of a lap, not an out-of-range slot inside it - unlike
// the single ring, the coil has no "true zero must stay unreachable"
// constraint to preserve here; see this file's header, "TRUE ZERO IS NOW
// REACHABLE BY TOUCH", for why that old constraint does not carry over).
//
// AT TICKS_PER_LAP=180 (SUPERSEDED FROM 360, see this file's header,
// "CORRECTED 2026-08-14 (AFTER REAL-HARDWARE USE)"), THIS IS A TWO-DEGREE-
// PER-TICK MAPPING: a tap can select any of 180 slots, and two adjacent
// slots are ~6.0px of arc apart at this file's RING_OUTER_R (double the
// previous ~3.0px - see the tick-geometry section above) - still not
// reliably choosable by a real fingertip one slot at a time, so the same
// reasoning holds: a point is described as landing "in the right
// neighbourhood", and it is drag_touch(), immediately below, that actually
// resolves the exact 5-second step from there.
static int sub_lap_ticks_for_angle(float angleDeg) {
    int slot = (int)lroundf(angleDeg / 360.0f * (float)TICKS_PER_LAP);
    if (slot >= TICKS_PER_LAP) slot -= TICKS_PER_LAP;
    if (slot < 0) slot = 0;
    return slot;
}

// "Point directly": the fast gesture. Sets the position WITHIN the current
// lap to match the tapped angle, preserving however many laps are already
// wound - see this file's header for why the lap is preserved rather than
// reset (a tap while deep into a lap nudges the value within that lap; to
// change which lap, drag past twelve o'clock instead, see drag_touch()
// below). From zero (lap 0), tapping six o'clock (180 degrees, half a
// turn) sets exactly half of TICKS_PER_LAP's worth of time - AT THIS
// FILE'S CURRENT 15-minute lap length that is 7:30 (SUPERSEDED FROM 15:00
// at the previous 30-minute lap, SUPERSEDED FROM 5:00 before that, at the
// original 10-minute lap): the worked example keeps moving with the lap
// length, on the same terms the header already accepted once - see this
// file's header, "First, six o'clock is now 15:00, not 5:00", for the
// original reasoning, which applies again unchanged.
//
// Called on every fresh touch context: a genuine f->touchPressed, AND the
// one frame TS_PAUSED converts into TS_SETTING (see timer_tick()) - both
// are "start a new pointing/dragging gesture from wherever the value
// currently sits", the only difference being where that starting value
// came from (an existing setTicks either way; TS_PAUSED's touch branch just
// writes the paused value into setTicks a few lines before calling this).
static void point_touch(timer_state_t *s, int touchX, int touchY) {
    float angle = raw_touch_angle_deg(touchX, touchY);
    int lap = s->setTicks / TICKS_PER_LAP; // integer division; see below for lap==LAPS_MAX
    int newTotal = lap * TICKS_PER_LAP + sub_lap_ticks_for_angle(angle);
    // lap can be exactly LAPS_MAX only when setTicks is exactly MAX_TICKS
    // (360/180 = 2, SUPERSEDED FROM 720/360): there is no lap beyond the
    // last one to preserve a position within, so newTotal comes out >=
    // MAX_TICKS regardless of the tapped angle and the clamp below holds it
    // at the ceiling - tapping the coil while already at 30:00 (SUPERSEDED
    // FROM 60:00) stays at 30:00, whichever angle is tapped. Reads as the
    // ceiling behaving like a ceiling, not as a bug.
    if (newTotal > MAX_TICKS) newTotal = MAX_TICKS;
    if (newTotal < 0) newTotal = 0;
    s->setTicks = newTotal;
    s->dragAccumTicks = (float)newTotal;
    s->lastTouchAngleDeg = angle;
}

// FINE-GRAINED DRAG: NO JITTER AT REST, NO RUNAWAY WHEN FAST.
//
// Continues an active drag: reads THIS frame's raw angle, unwraps its delta
// against the LAST frame's angle across the twelve o'clock branch cut (a
// jump of more than 180 degrees either direction is treated as having gone
// the SHORT way around instead, exactly the standard technique for
// integrating an angle continuously through its own wraparound), and
// accumulates the result into dragAccumTicks - see this file's header,
// "POINT VS DRAG, AND THE BRANCH CUT", for why this, not a fresh angle-to-
// slot snapshot every frame, is what actually implements "dragging past
// twelve o'clock adds a lap". dragAccumTicks is clamped to [0, MAX_TICKS]
// as it goes, not only once at the end, so pushing past either limit and
// then reversing resumes immediately with no dead zone (NO RUNAWAY: a fast
// drag that overshoots the ceiling or floor never has to "travel back" the
// overshoot before the displayed value starts moving again).
//
// NO JITTER AT REST is the harder half, and new with TICKS_PER_LAP=360 (one
// tick is now only ~3px of arc, a THIRD of the six-band coil's own ~9px -
// see this file's header, "Second, and this is the one requiring
// judgement"). dragAccumTicks itself already cannot jitter from a truly
// unmoving finger: raw_touch_angle_deg() is a pure function of the reported
// touch pixel, so two identical samples produce delta == 0.0 exactly, and
// nothing accumulates. The real risk is the DISPLAYED tick (setTicks,
// which drives both the digits and the arc) flickering between two
// adjacent values if the touch CONTROLLER itself reports a slightly
// different pixel between two consecutive samples for a physically still
// finger - ordinary sensor noise, not a code bug, and something this
// emulator cannot reproduce (its touch is mouse-driven and pixel-exact, so
// there is no noise here to test against; see emu_abi.h's own "input
// device defects" caveat). Rather than snap the DISPLAY at the naive
// halfway point between two ticks (round-to-nearest), this commits a new
// displayed tick only once the accumulator has moved
// DRAG_COMMIT_HYSTERESIS_TICKS past that halfway point - a Schmitt-
// trigger-style dead band around each commit boundary, on the OUTPUT side
// only. This never blocks or slows genuine motion: dragAccumTicks keeps
// accumulating every real delta exactly regardless of the hysteresis, so a
// deliberate drag that keeps moving in one direction always eventually
// clears the wider threshold and the display catches all the way up (via
// lroundf(dragAccumTicks), not a single-step increment) the moment it
// does. It only adds reluctance for a value that is hovering near a
// boundary rather than committing to either side of it.
//
// THE HONEST LIMIT, AND WHY THIS PASS HALVES THE VALUE RATHER THAN CARRYING
// IT FORWARD UNEXAMINED. DRAG_COMMIT_HYSTERESIS_TICKS was 0.3 at the
// previous (30-minute-lap, 360-tick) geometry - see this file's header,
// "CORRECTED 2026-08-14 (AFTER REAL-HARDWARE USE)": one raw pixel of touch-
// coordinate noise at RING_OUTER_R (173, UNCHANGED by this pass - see the
// ring-geometry section above) is still roughly 1/173 radian, about 0.33
// degrees - that arithmetic has not moved. What DID move is how many ticks
// that 0.33 degrees is worth: at the OLD 360-tick lap, 1 degree was exactly
// 1 tick, so 0.33 degrees was 0.33 ticks - close to a full tick's own
// commit granularity, which is what made noise a genuine risk and motivated
// a hysteresis of comparable size. At the NEW 180-tick lap, 1 degree is
// only 0.5 ticks, so the SAME 0.33 degrees of pixel noise is now only
// 0.33 * 0.5 = 0.166 ticks - HALF as much noise relative to a tick's own
// width, because the same physical arc length (~6.0px, see the tick-
// geometry section above) now buys twice as much angular margin per tick as
// it did before. This is the "one welcome consequence" the owner's own
// brief for this pass named directly: finer-grained drag jitter, flagged as
// a genuine open question at the old 3px-per-tick geometry, should be
// materially better at the new ~6px-per-tick one - and the arithmetic here
// says specifically HOW much better (2x), not just "probably some".
//
// DRAG_COMMIT_HYSTERESIS_TICKS = 0.15, HALVED from 0.3, tracking the halved
// noise-to-tick ratio exactly rather than being left at the old value out
// of caution: the old 0.3 was chosen to sit just under the old 0.33-degree
// noise figure (see the git history of this constant), so the new value
// sits just under the new 0.166-degree-equivalent figure the same way,
// preserving the SAME relative safety margin this file has used at every
// tick geometry rather than accumulating unnecessary lag as the geometry
// gets kinder. A single pixel of real controller noise is no longer
// comparable in size to one tick's own commit granularity the way it was
// before, so less software dead band is needed to absorb it without also
// eating genuine slow motion by the same amount. What this hysteresis DOES
// guarantee: a perfectly still finger (identical reported pixel every
// frame, which is what this emulator's own mouse-driven touch always
// produces) never flickers, because delta is exactly zero and nothing ever
// approaches a commit boundary in the first place. What it does NOT
// guarantee, because nothing at the software layer can, is that real
// hardware touch noise of a full pixel or more will always be absorbed -
// if the physical sensor turns out noisier than this margin on real
// hardware, that is a genuine, reportable finding about this device's touch
// resolution at a 180-tick lap, not something to quietly round away, and
// the fix at that point is hardware-level averaging or a coarser DISPLAY
// step (not what the coil actually STORES, which stays exact 5s ticks
// either way) rather than a bigger software hysteresis, which would just
// trade flicker for sluggishness. Flagged here, not silently assumed
// solved - see this task's own report for the same flag, and
// repro-timer-coil.ts for the jitter-relevant coverage this emulator CAN
// provide (a perfectly still simulated finger produces zero displayed-value
// changes across many ticks).
#define DRAG_COMMIT_HYSTERESIS_TICKS 0.15f

static void drag_touch(timer_state_t *s, int touchX, int touchY) {
    float angle = raw_touch_angle_deg(touchX, touchY);
    float delta = angle - s->lastTouchAngleDeg;
    if (delta > 180.0f) delta -= 360.0f;
    else if (delta < -180.0f) delta += 360.0f;
    s->lastTouchAngleDeg = angle;

    s->dragAccumTicks += delta / 360.0f * (float)TICKS_PER_LAP;
    if (s->dragAccumTicks < 0.0f) s->dragAccumTicks = 0.0f;
    if (s->dragAccumTicks > (float)MAX_TICKS) s->dragAccumTicks = (float)MAX_TICKS;

    float diff = s->dragAccumTicks - (float)s->setTicks;
    if (diff >= 0.5f + DRAG_COMMIT_HYSTERESIS_TICKS || diff <= -(0.5f + DRAG_COMMIT_HYSTERESIS_TICKS)) {
        s->setTicks = (int)lroundf(s->dragAccumTicks);
    }
}

// BRIDGING A DROPPED-CONTACT touchPressed - CORRECTED 2026-08-14, the actual
// fix for "winding past the end of the first lap restarts at zero".
//
// app.h's touchPressed is the runtime's own edge detector (runtime_core.c,
// "down && !g_touchWasDown"), computed from whichever raw sample happened to
// be queued last when this app's tick ran. The real touch controller this
// device carries does not report a clean, continuous stream of contact
// while a finger sits on the glass: sketch.c's own CONFIRM_MS comment cites
// a measured session with 798 dropouts (runs of zero-finger reports) in
// about 23 seconds of continuous drawing - one roughly every 30ms, far
// inside a single human drag gesture. A dropout that happens to land on its
// own tick reads, at the resolved touchDown/touchPressed level this file
// consumes, as a genuine touchReleased followed by a genuine touchPressed
// a frame or two later, indistinguishable from an actual lift-and-retouch
// UNLESS something tracks how recently contact was last seen.
//
// That something used to be nothing: every touchPressed in the TS_SETTING
// branch below called point_touch(), which reads the CURRENT angle and
// s->setTicks' CURRENT lap and reseeds dragAccumTicks from that snapshot -
// correct for a genuine new gesture, catastrophic for a bridged dropout
// mid-drag, because it throws away the continuous unwrap that is the only
// thing that knows a lap boundary was ever crossed. A dropout landing right
// as a drag passes twelve o'clock is exactly the reported symptom: the
// snapshot reads a small angle just past the top and a lap count that has
// not advanced yet, so the "restart" is real, not perceived - point_touch()
// really did throw the wound lap away and start over near zero.
//
// The fix cannot live in runtime_core.c (out of scope for this change - see
// this task's own constraints, and sketch.c already owns the one app that
// reads the raw sample stream directly for exactly this reason, app.h's
// touchDown/X/Y comment). So this file re-derives the same "a lift is only
// believed after a short grace period with no contact" rule sketch.c's own
// LIFT_DEBOUNCE_MS applies to the raw stream, but at the resolved-edge level
// this file actually has: if the previous frame this file saw touchDown was
// inside TOUCH_DROPOUT_GRACE_MS of now, a touchPressed this frame is treated
// as the SAME gesture continuing, and dispatched to drag_touch() (whose own
// branch-cut unwrap already handles a multi-frame gap correctly - it just
// computes one larger delta against s->lastTouchAngleDeg, same as it would
// for several small ones) instead of point_touch().
//
// TOUCH_DROPOUT_GRACE_MS = 80, identical to sketch.c's LIFT_DEBOUNCE_MS: same
// controller, same "how long a dropout can plausibly last" question, so
// there is no independent number to derive here - see that file's own
// comment for the reasoning (a real lift takes noticeably longer than a
// dropout in every measured session).
#define TOUCH_DROPOUT_GRACE_MS 80u

// True when this frame's touchPressed should be believed as a REAL fresh
// touch-down rather than folded into the drag that was already in progress.
// touchContextLive is false only before this SETTING session's very first
// touch (the zeroed-arena default), which guards the one case elapsed-time
// alone cannot: at nowMs close to zero, "no previous touch" and "a touch
// bridged from very recently" are not otherwise distinguishable.
static bool is_genuine_new_press(const timer_state_t *s, uint32_t nowMs) {
    if (!s->touchContextLive) return true;
    return (nowMs - s->lastTouchDownMs) >= TOUCH_DROPOUT_GRACE_MS;
}

/* ---------------------------------------------------------------------
 * Digits.
 * ------------------------------------------------------------------- */
static uint16_t digit_color_for_state(timer_state_e state) {
    switch (state) {
        case TS_SETTING: return gray_to_px(180); // light: not committed yet
        case TS_PAUSED:  return gray_to_px(110); // darker: frozen
        case TS_RUNNING:
        default:         return PX_BLACK;        // solid: live, counting down
    }
}

// What the digits should currently show, as a function of state alone.
// Always a whole number of seconds. ALARM never calls this: it does not
// draw digits at all while it is flashing.
static int digit_seconds_for(const timer_state_t *s) {
    if (s->state == TS_SETTING) return seconds_for_ticks(s->setTicks);
    return s->remainingSeconds;
}

// RUNNING's true (fractional) remaining seconds at `nowMs`: remainingSeconds
// is only ever updated once per whole second (the while loop in timer_tick),
// so between those updates real time has kept moving within the current
// second; this reads that sub-second position back out via lastDecMs rather
// than waiting for the next whole-second tick to notice it, which is what
// makes the arc move smoothly instead of once a second. Also used, once, to
// CAPTURE the value the instant PAUSED is entered (see the PWR-short handler
// in timer_tick): at that call site s->state has not flipped to TS_PAUSED
// yet, so this still reads as the live RUNNING formula, which is exactly
// the value that needs freezing.
static float running_true_remaining(const timer_state_t *s, uint32_t nowMs) {
    uint32_t elapsedMs = nowMs - s->lastDecMs;
    // Defensive floor, not something expected to trigger: the RUNNING branch
    // in timer_tick always drains any >=1000ms backlog through its own while
    // loop before this ever runs.
    if (elapsedMs > 999) elapsedMs = 999;
    float t = (float)s->remainingSeconds - (float)elapsedMs / 1000.0f;
    return t < 0.0f ? 0.0f : t;
}

// The coil's current fill angle PER BAND, 0..360 each, as a function of
// state alone - the direct generalisation of the single ring's own
// current_fill_deg(), which this replaces. Reads a continuous total-ticks
// value (SETTING: setTicks itself; PAUSED: the frozen pausedTrueRemaining;
// RUNNING: running_true_remaining()'s live fractional seconds), same three
// state branches as before, then - because the coil's step is flat - turns
// total ticks into each band's own fraction of TICKS_PER_LAP directly,
// with no piecewise tick_index_for_seconds() dance the tiered ring needed
// (that function still exists, unchanged in role, for the seconds<->ticks
// conversion the digits and BOOT/lastSetTicks handling still need; it just
// no longer has to feed this function, because ticks and seconds are now
// the same curve up to a constant factor).
//
// bandStartTicks = b*TICKS_PER_LAP for band b: band b has not been reached
// at all while totalTicks <= bandStartTicks (outDeg 0, pure grey track),
// is FULLY wound once totalTicks >= bandStartTicks + TICKS_PER_LAP (outDeg
// 360, solid black, "completed"), and in between shows the live fraction -
// this one formula produces every case this file's header describes: both
// bands empty at exactly 0:00, both full at exactly 30:00 (SUPERSEDED FROM
// 60:00), and a smooth,
// continuous transition through the one lap boundary in between, with no
// special-casing of any boundary anywhere in this function (see this
// file's header, "SETTING AND RUNNING STILL SHARE ONE MAPPING" - SETTING
// and RUNNING/PAUSED all funnel through this exact function, same
// invariant the single ring had for current_fill_deg()).
static void compute_band_fill_degs(const timer_state_t *s, uint32_t nowMs, float outDeg[LAPS_MAX]) {
    float remainSec;
    switch (s->state) {
        case TS_SETTING: remainSec = (float)seconds_for_ticks(s->setTicks); break;
        case TS_PAUSED:  remainSec = s->pausedTrueRemaining; break;
        case TS_RUNNING:
        default:         remainSec = running_true_remaining(s, nowMs); break;
    }
    float ticks = tick_index_for_seconds(remainSec);
    if (ticks < 0.0f) ticks = 0.0f;
    if (ticks > (float)MAX_TICKS) ticks = (float)MAX_TICKS;

    for (int b = 0; b < LAPS_MAX; b++) {
        float within = ticks - (float)b * (float)TICKS_PER_LAP;
        if (within <= 0.0f) outDeg[b] = 0.0f;
        else if (within >= (float)TICKS_PER_LAP) outDeg[b] = 360.0f;
        else outDeg[b] = (within / (float)TICKS_PER_LAP) * 360.0f;
    }
}

static void draw_all_digits(int seconds, uint16_t color) {
    int mm = seconds / 60;
    int ss = seconds % 60;
    digits_clear(X_MM_TENS,  DIGIT_Y0, DIGIT_W, DIGIT_H, PX_WHITE);
    digits_clear(X_MM_UNITS, DIGIT_Y0, DIGIT_W, DIGIT_H, PX_WHITE);
    digits_clear(X_SS_TENS,  DIGIT_Y0, DIGIT_W, DIGIT_H, PX_WHITE);
    digits_clear(X_SS_UNITS, DIGIT_Y0, DIGIT_W, DIGIT_H, PX_WHITE);
    digits_draw(X_MM_TENS,  DIGIT_Y0, DIGIT_W, DIGIT_H, SEG_T, mm / 10, color);
    digits_draw(X_MM_UNITS, DIGIT_Y0, DIGIT_W, DIGIT_H, SEG_T, mm % 10, color);
    digits_draw(X_SS_TENS,  DIGIT_Y0, DIGIT_W, DIGIT_H, SEG_T, ss / 10, color);
    digits_draw(X_SS_UNITS, DIGIT_Y0, DIGIT_W, DIGIT_H, SEG_T, ss % 10, color);
    digits_draw_colon(X_COLON, DIGIT_Y0, SEP_W, DIGIT_H, SEG_T, color);
}

static void redraw_digit_cell(int x, int value, uint16_t color) {
    digits_clear(x, DIGIT_Y0, DIGIT_W, DIGIT_H, PX_WHITE);
    digits_draw(x, DIGIT_Y0, DIGIT_W, DIGIT_H, SEG_T, value, color);
    gfx_push_land(x, DIGIT_Y0, DIGIT_W, DIGIT_H);
}

// Repaints and pushes only the digit cells whose value actually changed
// since the last call, same discipline as chrono's per-cell diff.
static void update_digits_if_changed(timer_state_t *s, int seconds) {
    if (seconds == s->lastDigitSeconds) return;
    int mm = seconds / 60, ss = seconds % 60;
    int lastMm = s->lastDigitSeconds / 60, lastSs = s->lastDigitSeconds % 60;
    uint16_t color = digit_color_for_state(s->state);
    if (mm / 10 != lastMm / 10) redraw_digit_cell(X_MM_TENS,  mm / 10, color);
    if (mm % 10 != lastMm % 10) redraw_digit_cell(X_MM_UNITS, mm % 10, color);
    if (ss / 10 != lastSs / 10) redraw_digit_cell(X_SS_TENS,  ss / 10, color);
    if (ss % 10 != lastSs % 10) redraw_digit_cell(X_SS_UNITS, ss % 10, color);
    s->lastDigitSeconds = seconds;
}

// Full repaint of the coil (both bands, track + arc, with rounded caps) and
// the digits, for enter() and for every state transition. Does not push:
// callers that run after enter() (i.e. every transition) follow this with
// gfx_push_all(), since a transition changes the digits' colour, which is
// cheaper as one push than as several smaller ones.
static void redraw_full(timer_state_t *s, uint32_t nowMs) {
    ensure_ring_tables();
    float fillDeg[LAPS_MAX];
    compute_band_fill_degs(s, nowMs, fillDeg);
    paint_ring_full(fillDeg);
    for (int b = 0; b < LAPS_MAX; b++) s->lastFillDeg[b] = fillDeg[b];

    int seconds = digit_seconds_for(s);
    draw_all_digits(seconds, digit_color_for_state(s->state));
    s->lastDigitSeconds = seconds;
}

/* ---------------------------------------------------------------------
 * The alarm. Runs entirely inside this function; timer_tick() calls it
 * first, before anything else, and returns, so an input frame that was
 * actually "make it stop" is never also processed as a coil drag, a
 * shake-clear, or a button toggle - see timer_tick()'s own header comment
 * for why that same early, unconditional return is also what guarantees
 * the alarm's own shake dismissal can never double-fire into the new
 * shake-clear reaction on the same tick. Unchanged by the coil redesign:
 * everything here reads s->setTicks/seconds_for_ticks() generically, with
 * no dependency on how many ticks there are or how they map onto the dial.
 * ------------------------------------------------------------------- */
static void handle_alarm(timer_state_t *s, const app_frame_t *f) {
    // "Any input", read as literally every event app_frame_t can carry, per
    // the brief: a child reaching for a beeping object should not have to
    // remember which button, or that it has to be a button at all. f->shaken
    // joined this list per the owner's explicit correction (see this app's
    // ALARM_MAX_MS/ALARM_FLASH_MS comment above and g_timerApp's wantsShake
    // comment below for why shake is safe to read here AND, since
    // 2026-08-14, in exactly one other place in this file).
    bool anyInput = f->touchPressed || f->touchDown || f->touchReleased ||
                    f->bootClicked || (f->key != 0) || f->shaken;
    bool timedOut = (f->nowMs - s->alarmStartMs) >= ALARM_MAX_MS;

    if (anyInput || timedOut) {
        // Silence the sound FIRST, before anything else in this branch: the
        // owner's instruction is that dismissal is immediate, not "stops
        // once the current visual/audio step finishes" - see sound_stop()'s
        // own comment for why this actually is instant (it zeroes the
        // in-flight DMA buffers directly) rather than merely changing what
        // gets synthesised next.
        sound_stop();

        // Corrected 2026-08-13 (see this app's ALARM_MAX_MS comment above):
        // the owner simplified this from "back to the value that was set" to
        // a clean SETTING at 00:00. lastSetTicks remembers what setTicks was
        // so BOOT can still recall it in one press (see the BOOT branch in
        // timer_tick()) without the alarm state itself carrying a non-zero
        // dial forward.
        if (s->setTicks > 0) s->lastSetTicks = s->setTicks;
        s->setTicks = 0;
        s->touchContextLive = false; // see touchContextLive's own struct comment: the alarm
                                      // just wiped setTicks, not a drag, so any touch after
                                      // dismissal - however soon - starts a fresh gesture

        // Clear the whole panel first: the alarm's last flash frame may have
        // left it solid black (see the fill below, PX_BLACK on the inverted
        // phase), and redraw_full() only repaints the coil and the digit
        // cells, not every pixel in between. Skipping this leaves black
        // showing through the gaps between digit cells and everywhere
        // outside the coil - found by actually looking at a captured
        // dismiss frame, not by reasoning about the code. Pre-existing (the
        // alarm's flash-then-partial-redraw shape predates this file's
        // dots-to-ring-to-coil rewrites); fixed here because it is squarely
        // this function's own bug. Same "whole rect needs no landscape
        // rotation" reasoning as the flash fill just below.
        gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
        s->state = TS_SETTING;
        redraw_full(s, f->nowMs);
        gfx_push_all();
        int recallSec = seconds_for_ticks(s->lastSetTicks);
        printf("timer: alarm %s, back to 00:00 (BOOT recalls %02d:%02d)\r\n",
               anyInput ? "dismissed" : "timed out", recallSec / 60, recallSec % 60);
        return;
    }

    bool wantInverted = (((f->nowMs - s->alarmStartMs) / ALARM_FLASH_MS) % 2u) == 1u;
    if (wantInverted == s->alarmInverted) return;
    s->alarmInverted = wantInverted;

    // A full-panel fill needs no landscape rotation: a rectangle covering
    // the whole screen is the same rectangle whichever way it is rotated, so
    // this is the only place in this file that draws in native panel space
    // rather than through gfx_*_land.
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, s->alarmInverted ? PX_BLACK : PX_WHITE);
    gfx_push_all();
}

/* ---------------------------------------------------------------------
 * enter(): draws the initial SETTING screen (empty coil, both bands pure
 * track, 00:00, light grey digits) into the white framebuffer the runtime
 * has just cleared. Does not push: the runtime pushes the whole panel once
 * after this returns.
 * ------------------------------------------------------------------- */
static void timer_enter(void) {
    s_state = APP_STATE(timer_state_t);
    // APP_STATE zeroes the allocation: state == TS_SETTING (0), setTicks ==
    // 0, dragAccumTicks == 0, lastFillDeg[] all 0, everything else 0/false -
    // exactly the "nothing set yet, every band empty" starting point.
    redraw_full(s_state, 0); // nowMs unused: SETTING ignores it (compute_band_fill_degs's switch)
    printf("timer: entered, drag the coil to set a time\r\n");
}

/* ---------------------------------------------------------------------
 * tick(): one state transition or one incremental update per call, never
 * both, so every visible change traces to exactly one input - with two
 * deliberate exceptions, both the same idiom applied to a different pair of
 * inputs:
 *
 *   1. a BOOT release and a PWR short-press verdict landing on the same
 *      tick both apply, in that order (see the bootClicked branch below).
 *      That is not two inputs pretending to be one; it is two real,
 *      distinct inputs that genuinely arrived together, so both events get
 *      to act rather than one silently eating the other.
 *   2. TS_PAUSED's touch-to-edit reaction (see the TS_PAUSED branch below)
 *      changes s->state to TS_SETTING and then falls into the TS_SETTING
 *      branch on the SAME call - one physical input (this frame's touch)
 *      genuinely causing two logical effects (the state transition, and
 *      that same touch immediately placing the coil), rather than
 *      deferring the second effect to a frame that has not happened yet.
 *
 * The shake-clear branch, new 2026-08-14, is NOT a third instance of this:
 * it always returns immediately after acting, same as every other
 * transition in this function, and is reachable only from TS_SETTING or
 * TS_PAUSED - never from TS_ALARM, because the unconditional `return;`
 * right after handle_alarm() above means TS_ALARM never reaches ANY of the
 * code below it, on any tick, regardless of what handle_alarm() itself
 * changed s->state to. That is what guarantees an alarm-dismissing shake
 * can never also clear "something else" on the very same tick: by the time
 * this function would otherwise re-examine s->state for the shake-clear
 * branch, it has already returned.
 * ------------------------------------------------------------------- */
static void timer_tick(const app_frame_t *f) {
    timer_state_t *s = s_state;

    if (s->state == TS_ALARM) {
        handle_alarm(s, f);
        return;
    }

    if (f->bootClicked) {
        // Reset to the set value, from any state; a no-op in SETTING itself,
        // since it is already showing the set value. "Again" is one press
        // away, per the brief, exactly like the old stopwatch's
        // BOOT-resets-to-zero except the target is setTicks, not zero.
        if (s->state != TS_SETTING) {
            s->state = TS_SETTING;
            // Invalidates any drag-continuation context left over from a
            // previous SETTING session (see touchContextLive's own struct
            // comment): setTicks was just set by BOOT, not by a drag, so the
            // next touchPressed - however soon it arrives - must be believed
            // as genuinely fresh, never bridged into whatever gesture last
            // touched the ring before RUNNING/PAUSED started.
            s->touchContextLive = false;
            redraw_full(s, f->nowMs);
            gfx_push_all();
            int sec = seconds_for_ticks(s->setTicks);
            printf("timer: BOOT reset to %02d:%02d\r\n", sec / 60, sec % 60);
        } else if (s->setTicks == 0 && s->lastSetTicks > 0) {
            // Recall, added 2026-08-13 alongside the alarm's dismiss-to-zero
            // correction (see ALARM_MAX_MS's comment above): SETTING can now
            // show a clean 00:00 after a dismissal, with nothing left on the
            // dial to re-drag from. This is the one-press "again" that used
            // to come for free from the dial staying pre-loaded; it only
            // fires when there is genuinely nothing set (setTicks == 0), so
            // it never fights a finger already mid-drag to a real value.
            s->setTicks = s->lastSetTicks;
            s->touchContextLive = false; // same reasoning as the reset branch above
            redraw_full(s, f->nowMs);
            gfx_push_all();
            int sec = seconds_for_ticks(s->setTicks);
            printf("timer: BOOT recalled %02d:%02d\r\n", sec / 60, sec % 60);
        }
        // NOT an unconditional `return;` here any more - that was the bug
        // (see docs/findings-app-fuzzing.md section 1, commit 88cabe6).
        // runtime_core.c's sensors_key_take() is read-and-clear, called
        // exactly once per tick regardless of whether this app goes on to
        // look at KEY_SHORT. BOOT is polled at only ~20Hz (bootbtn.h), so a
        // BOOT release and a PWR short-press verdict landing in the same
        // tick is easy to trigger by releasing both buttons together, and an
        // unconditional return here used to throw the KEY_SHORT bit away
        // for good: no log line, no state change, unrecoverable. chrono.c's
        // bootClicked branch (chrono_tick) has no early return for the
        // identical reason and this follows the same idiom: apply BOOT,
        // then let a same-tick KEY_SHORT below act on the state BOOT
        // produced (recall, then start; reset-to-setting, then start-from-
        // there).
        //
        // Still return when there is no KEY_SHORT pending this tick, exactly
        // as before: a plain BOOT click on its own must not newly fall
        // through into the touch-drag dispatch further down (SETTING's
        // `if (!f->touchDown) return;` block), which chrono has no
        // equivalent of - chrono's own fallthrough only ever reaches an
        // unconditional digit redraw, never live touch input. Gating the
        // fallthrough on KEY_SHORT keeps that path exactly as narrow as the
        // bug being fixed.
        if (!(f->key & KEY_SHORT)) return;
    }

    if (f->key & KEY_SHORT) {
        if (s->state == TS_SETTING) {
            // Nothing to start at zero: PWR here would toggle into a
            // RUNNING state whose digits read 00:00 and immediately alarm,
            // which is a startle, not a feature. Left as a silent no-op
            // rather than handled, since there is nothing wrong to report.
            if (s->setTicks <= 0) return;
            s->state = TS_RUNNING;
            s->remainingSeconds = seconds_for_ticks(s->setTicks);
            s->lastDecMs = f->nowMs;
            redraw_full(s, f->nowMs);
            gfx_push_all();
            printf("timer: start, %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        } else if (s->state == TS_RUNNING) {
            // Freeze the arcs' exact fractional position BEFORE flipping
            // state: running_true_remaining() reads remainingSeconds and
            // lastDecMs, both still valid pre-flip, using f->nowMs one last
            // time while it still means "how far into this second are we".
            // Once state is TS_PAUSED, compute_band_fill_degs() stops
            // consulting nowMs at all, so this is the only chance to
            // capture it; skipping this would either snap the coil back to
            // a whole tick on pause or let it drift while frozen.
            s->pausedTrueRemaining = running_true_remaining(s, f->nowMs);
            s->state = TS_PAUSED;
            redraw_full(s, f->nowMs); // arcs freeze exactly where they were; digit colour changes
            gfx_push_all();
            printf("timer: pause at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        } else { // TS_PAUSED
            s->state = TS_RUNNING;
            // Reset the decrement anchor rather than reusing the old one: a
            // long pause must not dump a backlog of missed seconds into the
            // countdown the instant it resumes. This also means each arc's
            // sub-second position resets to 0 rather than resuming from
            // pausedTrueRemaining's exact fraction; the discrepancy is at
            // most one second, which - the coil's flat step, unlike the old
            // tiered ring, makes this the SAME number everywhere on the
            // dial rather than a range - is 360/TICKS_PER_LAP/TICK_STEP_S =
            // 0.2 degrees of arc within whichever band is currently active,
            // well under 0.1% of that band's full sweep: reads as
            // continuous, not as a jump.
            s->lastDecMs = f->nowMs;
            redraw_full(s, f->nowMs);
            gfx_push_all();
            printf("timer: resume at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        }
        return;
    }

    // Shake-to-clear, added 2026-08-14. Owner, direct quote: "also i should
    // be able to shake to clear while i'm still in edit mode there."
    //
    // SCOPE: TS_SETTING and TS_PAUSED, both because both are "the dial is
    // editable" - SETTING obviously, and PAUSED because pause-then-edit
    // (see this file's header) already means a paused countdown can be
    // dragged without touching first, so it counts as "editable" even
    // before any touch has happened. TS_RUNNING is DELIBERATELY excluded:
    // a running countdown is counting something real, and a knock against
    // the table should not be able to destroy that progress the way it can
    // destroy an unstarted or already-stopped value. This is a judgement
    // call the owner explicitly handed to this file's own reading of the
    // state machine, not a spec'd rule - flagged as such in this task's own
    // report.
    //
    // EFFECT: identical shape to handle_alarm()'s own dismissal (preserve
    // lastSetTicks if there was something to preserve, zero setTicks, land
    // in TS_SETTING at 00:00, full redraw+push) - reusing that exact
    // pattern rather than inventing a second one, so "shake wipes it, but
    // BOOT can still recall it in one press" behaves identically whichever
    // way the dial got cleared.
    //
    // WHY THIS CANNOT DOUBLE-FIRE WITH THE ALARM'S OWN SHAKE DISMISSAL: see
    // this function's own header comment - handle_alarm() always returns
    // unconditionally, so a shake that dismisses the alarm never reaches
    // this branch on the same tick, regardless of what state it left
    // behind.
    //
    // WHY THE EXISTING SHAKE THRESHOLD LOOKS RIGHT AS-IS, NOT RETUNED:
    // firmware/runtime/sensors.c's imu_poll_core1() (out of scope for this
    // change - see this task's own constraints) already requires several
    // jolts (JOLT_MIN_COUNT, currently 4) inside a short window
    // (JOLT_WINDOW_MS, 700ms), specifically so "a social shake or a knock
    // cannot fire it" per that file's own comment. A single accidental
    // bump is exactly what that threshold was built to reject, for the
    // sketchpad's deliberate-wipe gesture; reusing it here for a second
    // deliberate-wipe gesture asks nothing new of it. Reported as checked
    // and appropriate rather than silently reused unexamined, per the
    // owner's own instruction to say so if it looked too easy - it does
    // not.
    if ((s->state == TS_SETTING || s->state == TS_PAUSED) && f->shaken) {
        if (s->setTicks > 0) s->lastSetTicks = s->setTicks;
        s->setTicks = 0;
        s->state = TS_SETTING;
        s->touchContextLive = false; // see touchContextLive's own struct comment: setTicks
                                      // was just wiped by the shake, not by a drag, so any
                                      // touch afterward - however soon - starts fresh
        redraw_full(s, f->nowMs);
        gfx_push_all();
        int recallSec = seconds_for_ticks(s->lastSetTicks);
        printf("timer: shake cleared to 00:00 (BOOT recalls %02d:%02d)\r\n", recallSec / 60, recallSec % 60);
        return;
    }

    // TS_PAUSED, touched: "if I stop the timer I should be able to directly
    // edit the time without clearing" - see this file's header, "PAUSE,
    // THEN EDIT". Gated on f->touchDown, not f->touchPressed, deliberately:
    // that is the same gate the TS_SETTING branch below already uses for
    // its own touch dispatch, and it means a finger already resting on the
    // coil at the exact instant PWR pauses starts editing right away too,
    // rather than requiring a lift-and-retouch first. Converts the frozen
    // pausedTrueRemaining into setTicks (rounded to the nearest 5s tick,
    // same rounding every other seconds->ticks conversion in this file
    // uses), flips the state, and falls through - no `return` here, see
    // this function's own header comment on why, and chrono.c's/this
    // function's bootClicked branch above for the established precedent.
    if (s->state == TS_PAUSED && f->touchDown) {
        int pausedTicks = (int)lroundf(s->pausedTrueRemaining / (float)TICK_STEP_S);
        if (pausedTicks < 0) pausedTicks = 0;
        if (pausedTicks > MAX_TICKS) pausedTicks = MAX_TICKS;
        s->setTicks = pausedTicks;
        s->state = TS_SETTING;

        // The very touch that triggered this transition is ALSO this
        // context's first touch: point_touch() unconditionally, exactly as
        // TS_SETTING's own branch does for a genuine f->touchPressed (see
        // point_touch()'s own comment on why both call sites are "start a
        // new pointing/dragging gesture" alike). This IS a full redraw
        // (state changed, digit colour must flip from PAUSED's darker grey
        // to SETTING's lighter one), so it does not go through
        // update_ring_to()'s incremental path the way a later drag_touch()
        // in this same TS_SETTING session will.
        point_touch(s, f->touchX, f->touchY);
        s->lastTouchDownMs = f->nowMs;
        s->touchContextLive = true;
        redraw_full(s, f->nowMs);
        gfx_push_all();
        int sec = seconds_for_ticks(s->setTicks);
        printf("timer: paused value now editable, %02d:%02d\r\n", sec / 60, sec % 60);
        return;
    }

    if (s->state == TS_SETTING) {
        if (!f->touchDown) return;

        int beforeTicks = s->setTicks;
        // f->touchPressed alone is NOT enough to decide point_touch() vs
        // drag_touch() any more - see is_genuine_new_press()'s own comment
        // for why (the real touch controller's dropouts make the runtime's
        // touchPressed edge fire mid-drag, and point_touch() reseeding on
        // one of those is what "restarts at zero" crossing a lap actually
        // was). A touchPressed this file believes is genuine gets
        // point_touch(); everything else - a continuing drag sample, OR a
        // touchPressed close enough behind the last seen touchDown to be a
        // bridged dropout - gets drag_touch(), which is exactly correct for
        // a bridged sample too (its own branch-cut unwrap does not care how
        // many frames the gap it is unwrapping across spans).
        if (f->touchPressed && is_genuine_new_press(s, f->nowMs)) {
            point_touch(s, f->touchX, f->touchY);
        } else {
            drag_touch(s, f->touchX, f->touchY);
        }
        s->lastTouchDownMs = f->nowMs;
        s->touchContextLive = true;
        if (s->setTicks == beforeTicks) return;

        int newSeconds = seconds_for_ticks(s->setTicks);
        // compute_band_fill_degs(), not a hand-rolled per-band formula at
        // this call site: see that function's header comment for why
        // SETTING and RUNNING/PAUSED must all funnel through the exact same
        // function. Going through it here is what guarantees a drag and the
        // coil it draws never compute the angle two different ways.
        float fillDeg[LAPS_MAX];
        compute_band_fill_degs(s, f->nowMs, fillDeg);
        update_ring_to(s, fillDeg);
        update_digits_if_changed(s, newSeconds);
        return;
    }

    if (s->state == TS_RUNNING) {
        bool changed = false;
        while (f->nowMs - s->lastDecMs >= 1000 && s->remainingSeconds > 0) {
            s->remainingSeconds--;
            s->lastDecMs += 1000;
            changed = true;
        }
        if (s->remainingSeconds <= 0) {
            s->state = TS_ALARM;
            s->alarmStartMs = f->nowMs;
            s->alarmInverted = false;
            // Exactly once, here, at the instant ringing starts - NOT inside
            // handle_alarm()'s flash-flip (which runs every ALARM_FLASH_MS,
            // 250ms, for the whole time the alarm rings): sound_play()
            // restarts the chime's phrase from its own beginning each call
            // (sound.h), so calling it every flip would retrigger the motif
            // every 250ms instead of letting it repeat on its own ~1.5s
            // period (sound_synth.c) - audibly chopped, not "repeated
            // gently". One call here is the whole hook.
            sound_play(SOUND_ID_TIMER_ALARM);
            printf("timer: ringing\r\n");
            return;
        }
        // Every RUNNING frame, not just on a whole-second `changed`: the
        // coil moves at sub-second granularity (that is the entire point of
        // a continuous arc over the old stepped dots), so gating it on
        // `changed` would reintroduce once-a-second stepping. update_ring_to
        // is itself a no-op when no band's angle has moved enough to touch
        // a new pixel, so most calls here do nothing.
        float fillDeg[LAPS_MAX];
        compute_band_fill_degs(s, f->nowMs, fillDeg);
        update_ring_to(s, fillDeg);
        if (changed) update_digits_if_changed(s, s->remainingSeconds);
        return;
    }

    // TS_PAUSED, not touched and not shaken: nothing changes on its own;
    // only BOOT, PWR and (above) a touch or a shake ever move it.
}

// wantsShake is true, added 2026-08-13 for the alarm's own dismissal, and
// read from a SECOND place since 2026-08-14 (the shake-clear branch in
// timer_tick(), above) for the owner's "shake to clear while editing"
// addition. Both readings share the same underlying signal and the same
// opt-in rule (sensors.h: shake is opt-in per app precisely so it cannot
// become a universal destructive verb - decision 0002 section 5), and
// between them they cover every state that reads f->shaken at all:
//
//   TS_ALARM                     handle_alarm() - dismiss, silence, clear.
//   TS_SETTING / TS_PAUSED       timer_tick()'s shake-clear branch - wipe
//                                 to 00:00, same shape as a dismissal.
//   TS_RUNNING                   f->shaken is NOT read here, on purpose -
//                                 see the shake-clear branch's own comment
//                                 for why a running countdown is excluded.
//
// If a future change ever reads f->shaken from a THIRD place in this file,
// stop and check it against both of the above first: the two existing
// readers were each scoped deliberately (one to "the alarm is ringing", one
// to "the dial is editable but nothing is counting down"), and a new reader
// that does not fit either description is very likely the opt-in rule being
// widened by accident rather than by a real, considered instruction the way
// both of these were.
const app_t g_timerApp = {
    .name       = "timer",
    .enter      = timer_enter,
    .tick       = timer_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = true,
};
