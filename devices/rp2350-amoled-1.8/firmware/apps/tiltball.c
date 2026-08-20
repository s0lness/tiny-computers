// tiltball: a ball rolling in a round dish. Tip the puck, the ball rolls;
// roll it into the hole and it falls in, with a sound and a little ripple,
// then comes back and the dish is hers again.
//
// It is the sixth app in g_apps[] (runtime_core.c). Nothing here reads touch
// or a button: the only way to change what is on screen is to move the
// object, exactly like the bubble level (firmware/apps/level.c), whose
// residue-free repaint technique this file borrows wholesale (see "WHAT ONE
// FRAME COSTS" below).
//
// =========================================================================
// WHAT THE OWNER ASKED FOR, AND WHAT HE DID NOT
// =========================================================================
//
// "Un truc genre monkey ball ou il faut rouler une balle en orientant
// l'appareil" - something Monkey-Ball-like where you roll a ball by
// orienting the device. Monkey Ball is the reference for the FEELING (a ball
// that rolls because you tipped the world), not a request for its
// difficulty, its falling-off-the-edge punishment, or its levels.
//
// The user is a two-year-old who cannot read and is the only person who
// will ever hold this. So this is not a game with a win state, a score or a
// fail state - it is a toy with one satisfying event in it. There is no
// level to clear, no lives, no timer, no way to lose the ball for good: the
// dish's own shape (see "THE DISH IS A BOWL" below) makes it impossible to
// leave the ball somewhere out of reach, and falling into the hole is a
// reward that loops, not an ending. That is the actual answer to "what is
// this for a two-year-old": rolling is the toy, and the hole is what makes
// rolling worth doing, not a level to beat.
//
// =========================================================================
// THE DISH IS A BOWL, NOT A FLAT PLATE WITH WALLS
// =========================================================================
//
// Decision 0009 forbids straight walls outright (mazes are exactly what it
// names as the obvious, forbidden design), and the task's own framing asks
// for a landscape, not a level. So the play surface is a shallow round
// bowl: its floor slopes back toward the middle from every direction, the
// way a real wooden bowl does. Tip the object and the LOW POINT of that
// bowl shifts toward the downhill side; the ball rolls toward wherever that
// point currently is and gently overshoots and settles around it, the way a
// marble in an actual bowl does, not the way a dot glides to a target on a
// dial (level.c's simpler, non-inertial motion).
//
// Physically this is a driven, damped 2D harmonic oscillator - the standard
// small-angle model for a ball in a paraboloid under a tilted uniform
// gravity field - and it was chosen over "accelerate by gx,gy and coast"
// for a reason that matters for a two-year-old specifically: A SPRING HAS A
// BUILT-IN HOME. However hard she tips it, the restoring term
// (-BALL_BOWL_OMEGA2 * displacement) always pulls the ball back toward a
// bounded equilibrium; there is no tilt, however extreme, that sends the
// ball off toward infinity the way pure "gx,gy is an acceleration" would.
// The rim clamp below is a second, independent backstop for the transient
// overshoot a spring can still produce, not the primary safety net.
//
// =========================================================================
// THE TILT-TO-ACCELERATION MAPPING, AND HOW ITS SCALE WAS CHOSEN
// =========================================================================
//
// Three constants decide the whole feel:
//
//   BALL_ACCEL_PER_G   px/s^2 of push per g of in-plane tilt (f->tilt.gx/gy)
//   BALL_BOWL_OMEGA2   the bowl's own restoring stiffness, rad^2/s^2
//   BALL_DAMPING       velocity damping, 1/s (rolling friction)
//
// The task's own point 1 is the reason these are NOT tuned the way
// level.c's dial is (LEVEL_FULL_SCALE_DEG = 15, an adult's careful,
// deliberate tip): "a two-year-old tips things much harder than an adult,
// so a mapping tuned on a careful wrist will be uncontrollable in her
// hands." So this file's numbers are chosen so that a careful ~15 degree
// tip - level.c's own reference for "a deliberate, controlled gesture" -
// moves the ball only partway (well under the distance to the hole,
// HOLE_DIST), and reaching the hole wants something closer to a real,
// decisive 30+ degree tip sustained for a moment, which is squarely in a
// toddler's natural range rather than an adult's fine-motor one. The result
// reads as GENTLE for a small motion and CAPABLE for a big one, rather than
// twitchy at every motion, which is what a mapping copied from level.c's
// 15-degree scale would have been in her hands.
//
// At equilibrium (holding a tilt steady), the maths are simple enough to
// state and check by eye: offset = BALL_ACCEL_PER_G * g_inplane /
// BALL_BOWL_OMEGA2, clamped to BALL_TRAVEL_MAX. With the constants below,
// that gives roughly 37px at 10 degrees, 56px at 15 degrees (both short of
// the 92px hole - an idle wobble cannot back into a capture by accident),
// and past the hole by roughly 30 degrees - all well inside the range a
// two-year-old's arm produces without effort. BALL_DAMPING is deliberately
// on the underdamped side (zeta ~= 0.66) rather than critical: a small
// overshoot-and-settle when a tilt is released reads as the ball having
// weight and momentum, which is the entire "feeling" the owner asked to
// borrow from Monkey Ball; a critically-damped ball that glides straight to
// rest with no wobble at all would look more like level.c's dot than like
// a rolling ball.
//
// UNVERIFIED BY A HAND, stated the same way tilt.c's own filter constants
// are: these three numbers were chosen from the arithmetic above, not from
// anyone actually tipping the physical board while watching the ball. If
// the first person to hold it says it is too twitchy or too dead, these
// three #defines are where to change it - not the shared filter in tilt.c,
// which every orientation-aware app on this device relies on staying one
// thing (see "WHICH WAY IS DOWN" below).
//
// =========================================================================
// !valid AND coasting: WHAT THE BALL DOES WHEN THE SIGNAL IS NOT TRUSTWORTHY
// =========================================================================
//
// !valid (no IMU reading yet, or it has gone quiet - tilt.h's TILT_STALE_MS):
// the external tilt term (BALL_ACCEL_PER_G * gx/gy) is treated as ZERO, not
// held at its last value and not guessed. The ball does not freeze either -
// the bowl's own restoring term keeps running, because that term is a fact
// about the dish's fixed shape, not a claim about the sensor. A physical
// bowl slopes to its middle whether or not anyone is reading an
// accelerometer, so letting the ball drift gently back toward the centre
// while the signal is unavailable draws nothing that is not true regardless
// of what the IMU says. That is the same standard tilt.h itself sets for
// `valid` (draw nothing that depends on an untrustworthy reading) applied to
// a continuous physics term instead of a discrete verdict.
//
// coasting (the filter's magnitude trust gate has fully given up on the
// current raw sample - the device is being carried - and gx/gy is holding
// its last filtered belief rather than tracking, per tilt.h): NOT specially
// handled here, the same as every other app that reads tilt today (no
// shipped app reads this flag yet - app.h's own comment on
// app_tilt_t.coasting). The reasoning for why that is fine here
// specifically, rather than just "nobody has gotten to it yet": gx/gy is
// still `valid` while coasting, and coasting means "the filter is holding
// the last orientation it trusted", which for a ball resting on a dish
// rigidly attached to the object being carried is not a fabricated value at
// all - it is what the dish's tilt genuinely still is, frozen at the last
// moment gravity could be told apart from motion. Driving the ball from a
// held belief is therefore physically honest, not a confident lie the way
// drawing a level's verdict from a coasting reading would be.
//
// =========================================================================
// THE AXIS MAPPING: READ, NEVER ASSUMED, EXACTLY LIKE LEVEL.C
// =========================================================================
//
// This file never touches the IMU and never guesses which way the QMI8658
// is soldered - it reads f->tilt.gx/gy, already filtered and already
// rotated into this app's own landscape space by runtime_core.c's
// tilt_for_app() (firmware/runtime/tilt.h, docs/decisions/0012). The
// device-to-panel mapping in tilt.c has moved past the identity hypothesis
// this paragraph originally described: it is now a measured fit (swap X/Y,
// negate Z), corrected again 2026-08-20 after the owner found horizontal
// tilt inverted on real silicon (device_to_panel()'s own header comment in
// tilt.c has the full history and the arithmetic). Synced into this repo by
// tools/sync-pack.ts. Because this app has no orientation code of its own -
// no sign flip, no swap, nothing - none of that history ever touched this
// file: the fix was one edit to `device_to_panel()` in tilt.c, same as this
// paragraph always said it would be. If a future axis correction ever lands
// there, the practical consequence for THIS app, if any, is that the ball
// would roll toward a different physical edge for a given real-world tip -
// direction only, magnitude and feel unaffected, and corrected for every
// app at once the moment the
// ritual is run.
//
// =========================================================================
// WHAT ONE FRAME COSTS, AND WHY THE DRAWING IS BUILT THIS WAY
// =========================================================================
//
// Same discipline as level.c, for the same reason: a moving object over a
// static background is exactly the shape that has left residue on this
// device four times (AGENTS.md). paint_rect() below recomputes EVERY pixel
// of a given rectangle from the model (dish rim, hole, capture ripple, ball,
// in that order, composited darkest-wins) rather than erasing a box and
// redrawing - so the old ball cannot survive a repaint of the ground it
// stood on, by construction. tick() commits the new model FIRST, then
// repaints only the union of where the ball/ripple WERE and where they ARE
// NOW, pre-rounded to gfx_push's 8-pixel row rule by align_for_push() (a
// second, file-local copy of level.c's own helper - apps refer to nothing
// outside themselves, decision 0002 section 4b).
//
// Measured budget: see repro-tiltball-residue.ts, which prints the worst
// frame and the average pushing frame the way repro-level-bubble-residue.ts
// does for the level, over a rolling motion and a full capture cycle.
//
// =========================================================================
// SOUND, AND WHY IT GETS MORE CARE THAN THE PHYSICS
// =========================================================================
//
// The task's own framing: "rolling a ball into a hole is satisfying because
// of what the hole does when the ball arrives... that moment is worth more
// of your effort than the physics that got it there." So the arrival is
// three things at once, all triggered on the single tick capture begins,
// nowhere else: a falling-pitch sound (sound_synth.c's
// sound_synth_capture_sample - descending, not the alarm's rising phrase,
// because a falling glide is the standard shorthand for something dropping
// in), a shrinking, sliding ball (an ease-in slide toward the hole's
// centre, not a straight fade, so it reads as FALLING rather than
// vanishing), and an expanding, fading ripple ring centred on the hole -
// three senses agreeing about the same instant. The ball then reappears at
// the dish's own centre after a short pause, so nothing is ever lost and
// there is always a next roll.
//
// =========================================================================
// COLOUR, AND WHY THIS FILE STAYS MONOCHROME
// =========================================================================
//
// The task notes colour is available and worth using if it earns its
// place. It was considered here (a warm ball against a grey dish) and
// rejected: every app on this device, including its sibling level.c, is
// drawn in one ink on one paper (decision 0009's own framing), and the
// panel's anti-aliasing convention (gray_to_px, gfx.h) is built entirely
// around the green channel doubling as an 8-bit coverage value - a
// monochrome assumption baked into every shade() call below. A colour ball
// would need its own RGB anti-aliasing path, breaking that shared
// convention for one app's decoration rather than for something the toy
// actually needs: a two-year-old does not need the ball to be orange to
// find it, she needs it to move when she tips the puck.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"
#include "sound.h"

/* ---------------------------------------------------------------------
 * Geometry, LANDSCAPE coordinates (448 wide x 368 tall), same convention
 * as level.c: this app maps landscape to panel itself in paint_rect(), the
 * same deliberate exception shapes.c's aa_composite_land makes.
 *
 * The dish is the SAME size as level.c's dial (identical centre, identical
 * rim radius and thickness) rather than a new, unproven set of numbers:
 * level.c's clearance from the bezel (12.5px past PANEL_BEZEL_MARGIN_PX=10,
 * gfx.h) already carries over exactly, and a family resemblance between the
 * level and this app (two round dishes, drawn the same way) is a feature,
 * not a coincidence.
 * ------------------------------------------------------------------- */
#define BALL_CX 224.0f
#define BALL_CY 184.0f

#define DISH_RIM_R     168.0f  // the dish rim's mid-radius (= level.c's LEVEL_RIM_R)
#define DISH_RIM_HALF    3.5f  // half its thickness
#define DISH_GAP         4.0f  // clear air between the ball and the rim at full travel

#define BALL_R 26.0f  // the ball, at rest. 52px across, against a child
                       // fingertip measured at ~75px (AGENTS.md) - smaller
                       // than the fingertip on purpose, since this is an
                       // object to watch roll, not a target to press.

// How far the ball's centre can travel before the rim's soft containment
// takes over: right up to the rim, minus the ball's own radius and a gap so
// it never visually collides with the rim's ink.
#define BALL_TRAVEL_MAX (DISH_RIM_R - DISH_RIM_HALF - BALL_R - DISH_GAP) // 134.5

// The hole. A fixed offset along this app's own +x (landscape "right" as it
// drew it) rather than a polar angle: since there is no meaningful "up" on
// a puck held flat (the task's own point 3), any fixed direction is as good
// as any other, and an axis-aligned offset needs no trig to place. Well
// inside BALL_TRAVEL_MAX, so it never touches the rim or the bezel.
#define HOLE_DIST 92.0f
#define HOLE_X (BALL_CX + HOLE_DIST)
#define HOLE_Y (BALL_CY)
#define HOLE_R 30.0f  // generous on purpose - see the task's own point about
                       // precision targets not fitting this hardware; this
                       // is not touched by a finger, but "forgiving" is the
                       // house style regardless of which input drives it.

// The hole is drawn as an OPENING, not as a disc. It was a solid black disc
// first, which made it and the ball two black circles of nearly the same
// size sitting side by side on an empty dish: nothing in the picture said
// which one the child was moving and which one she was aiming at. Every
// test passed, because a test counts pixels and cannot read a picture.
//
// A hole is an absence, so it is drawn as one: a rim with a pale recess
// inside it, leaving the ball as the only solid black thing on the dish.
// That also makes the capture animation read correctly for free - the
// shrinking ball stays visible against the pale recess as it goes down,
// instead of one black shape dissolving into another.
//
// Sized off HOLE_R inward so the physics above is untouched: the capture
// radius, the ripple's start radius and every rectangle this app pushes
// still key off the same HOLE_R they always did.
#define HOLE_RIM_HALF 4.5f                          // 9px rim, the house weight
#define HOLE_RIM_R (HOLE_R - HOLE_RIM_HALF)         // 25.5: rim spans 21..30
#define HOLE_IN_R (HOLE_R - 2.0f * HOLE_RIM_HALF)   // 21: the recess
#define HOLE_FILL_GRAY 214.0f                       // clearly not paper, and
                                                     // just as clearly not the
                                                     // ball's ink

// How close the ball's CENTRE must come to the hole's centre to fall in.
// Well inside HOLE_R (30): by the time this fires the ball is already deep
// inside the hole's own disc, so the trigger reads as "it went in", not
// "it grazed the rim of the hole and vanished".
#define CAPTURE_TRIGGER_R 20.0f

/* ---------------------------------------------------------------------
 * Physics. See this file's header comment, "THE TILT-TO-ACCELERATION
 * MAPPING", for the full derivation and honesty about what is and is not
 * verified by a hand.
 * ------------------------------------------------------------------- */
#define BALL_ACCEL_PER_G   5200.0f  // px/s^2 of push per g of in-plane tilt
#define BALL_BOWL_OMEGA2     24.0f  // the dish's own restoring stiffness (rad^2/s^2)
#define BALL_DAMPING          6.5f  // velocity damping, 1/s (rolling friction)
#define BALL_MAX_SPEED     1100.0f  // px/s, safety clamp against a dt spike
#define BALL_RESTITUTION      0.35f // fraction of outward speed kept on a rim bounce

// Sub-step size for the physics integration. frame.dtMs can spike as high
// as 250ms (runtime_core.c's clamp) after a slow tick or the very first
// frame; integrating a stiff spring in one 250ms step would visibly
// overshoot and could momentarily punch the ball past the rim before the
// containment clamp catches it. Chopping into <=16ms sub-steps (the board's
// own ordinary frame period) keeps the integration looking the same
// whether it runs in one tick or several - the same "speed must not depend
// on push cost" discipline tilt.c's own filter already follows for the
// signal this app reads (docs/decisions/0010, 0012).
#define BALL_SUBSTEP_MS 16u

// Once the ball is this close to its current equilibrium and moving this
// slowly, SNAP to the exact equilibrium and zero the velocity. Without
// this, a damped spring only approaches rest asymptotically and never
// produces two bit-identical frames in a row, which would mean a
// perfectly still puck keeps costing a repaint forever - exactly what
// level.c's own "a still puck costs nothing" property forbids for its dot,
// and there is no reason a ball should be held to a lower standard.
#define BALL_SETTLE_POS_EPS 0.4f
#define BALL_SETTLE_VEL_EPS 3.0f

// Repaint granularity, same convention and same value as level.c's
// LEVEL_POS_QUANT: the drawn position/radius are snapped to this, so a
// frame that would repaint the same picture is skipped.
#define BALL_POS_QUANT 0.5f

/* ---------------------------------------------------------------------
 * The capture sequence: rolling in, falling, a pause, coming back.
 * ------------------------------------------------------------------- */
#define CAPTURE_ANIM_MS   380u  // ball slides+shrinks into the hole
#define HIDDEN_PAUSE_MS   260u  // nothing at the hole but the ripple's tail
#define RESPAWN_ANIM_MS   260u  // ball grows back at the dish's centre
#define RIPPLE_DURATION_MS (CAPTURE_ANIM_MS + HIDDEN_PAUSE_MS) // the ripple
                       // keeps expanding across both phases, so the ring
                       // is still visibly moving right up to the moment
                       // the ball reappears rather than finishing early
                       // and leaving a dead pause on screen.
#define RIPPLE_MAX_R 46.0f  // > HOLE_R (30): the ring visibly grows past
                             // the hole's own edge before fading out
#define RIPPLE_HALF   3.0f  // half the ripple ring's stroke thickness

/* ---------------------------------------------------------------------
 * State: one struct from the arena (app.h), never file-scope - the same
 * rule level.c's own state follows.
 * ------------------------------------------------------------------- */
typedef enum {
    BALL_ROLLING = 0,   // ordinary play: physics, capture check
    BALL_CAPTURED,      // sliding + shrinking into the hole
    BALL_HIDDEN,        // a short pause; only the ripple's tail is moving
    BALL_RESPAWNING,     // growing back at the dish's centre
} ball_phase_t;

typedef struct {
    ball_phase_t phase;
    uint32_t     phaseStartMs;

    // Physics state, meaningful during BALL_ROLLING (and carried through
    // the other phases unused, since they drive position from the phase
    // clock instead).
    float px, py;
    float vx, vy;

    // Where the ball was the instant it was captured, so BALL_CAPTURED can
    // interpolate from there to the hole rather than snapping.
    float ballFromX, ballFromY;

    // What is actually ON SCREEN right now. paint_rect() renders exactly
    // this, which is what makes an incremental frame identical to a
    // from-scratch one - see level.c's own comment on the same property.
    float drawnBallX, drawnBallY, drawnBallR; // drawnBallR <= 0: no ball drawn
    float drawnRippleR;                       // 0: no ripple drawn
} tiltball_state_t;

static tiltball_state_t *s_state;

/* ---------------------------------------------------------------------
 * Drawing. One function paints; nothing erases. Same shape as level.c's
 * own paint_rect/shade/align_for_push - duplicated here rather than shared,
 * per decision 0002 section 4b ("each app is an object and refers to
 * nothing outside itself").
 * ------------------------------------------------------------------- */

typedef struct { int x0, y0, x1, y1; } rect_t; // inclusive, landscape

// Coverage (0..1) of one shape, over white, as this device's 8-bit ink
// level - identical convention to level.c and sketch.c: a signed distance
// turned into coverage by 0.5 - d, composited by MIN (darkest wins).
static inline uint8_t shade(float coverage, float targetGray) {
    if (coverage <= 0.0f) return 255;
    if (coverage > 1.0f) coverage = 1.0f;
    return (uint8_t)(targetGray * coverage + 255.0f * (1.0f - coverage) + 0.5f);
}

// The ripple ring's shade, derived purely from its own radius rather than
// carried as a second stored field: radius grows linearly with elapsed time
// (ripple_radius_for() below) over a fixed range [HOLE_R, RIPPLE_MAX_R], so
// that range doubles as the fade's own progress axis with no extra state.
static inline float ripple_gray_for(float r) {
    float p = (r - HOLE_R) / (RIPPLE_MAX_R - HOLE_R);
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return 40.0f + p * (255.0f - 40.0f); // near-black at birth, fades to invisible
}

static float ripple_radius_for(uint32_t elapsedMs) {
    if (elapsedMs >= RIPPLE_DURATION_MS) return 0.0f;
    float p = (float)elapsedMs / (float)RIPPLE_DURATION_MS;
    return HOLE_R + (RIPPLE_MAX_R - HOLE_R) * p;
}

// Repaints every pixel of an inclusive landscape rectangle from the model
// in `s->drawn*`, plus the dish's two FIXED features (the rim, the hole),
// which never move and are cheap to re-test on every call (one squared
// distance each). Order: rim, hole, ripple, ball - darkest wins, so the
// ball visually sits "in front of" everything else, which is exactly right
// while it is rolling on top of the dish.
static void paint_rect(const tiltball_state_t *s, rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > LAND_W - 1) r.x1 = LAND_W - 1;
    if (r.y1 > LAND_H - 1) r.y1 = LAND_H - 1;
    if (r.x0 > r.x1 || r.y0 > r.y1) return;

    const float rimOut = DISH_RIM_R + DISH_RIM_HALF + 1.0f;
    const float rimIn  = DISH_RIM_R - DISH_RIM_HALF - 1.0f;
    const float rimOut2 = rimOut * rimOut, rimIn2 = rimIn * rimIn;

    const float holeOut = HOLE_R + 1.0f;
    const float holeOut2 = holeOut * holeOut;

    const bool hasRipple = s->drawnRippleR > 0.0f;
    const float rippleOut = hasRipple ? s->drawnRippleR + RIPPLE_HALF + 1.0f : 0.0f;
    const float rippleInn = hasRipple ? s->drawnRippleR - RIPPLE_HALF - 1.0f : 0.0f;
    const float rippleOut2 = rippleOut * rippleOut, rippleInn2 = rippleInn * rippleInn;
    const float rippleGray = hasRipple ? ripple_gray_for(s->drawnRippleR) : 0.0f;

    const bool hasBall = s->drawnBallR > 0.01f;
    const float ballOut = s->drawnBallR + 1.0f;
    const float ballOut2 = ballOut * ballOut;

    for (int lx = r.x0; lx <= r.x1; lx++) {
        float fx = (float)lx + 0.5f;
        float dxc = fx - BALL_CX;
        float dxh = fx - HOLE_X;
        float dxb = fx - s->drawnBallX;
        int idx = lx * PANEL_W + (PANEL_W - 1 - r.y0);
        for (int ly = r.y0; ly <= r.y1; ly++, idx--) {
            float fy = (float)ly + 0.5f;
            float dyc = fy - BALL_CY;
            float dyh = fy - HOLE_Y;

            uint8_t ink = 255;

            float rc2 = dxc * dxc + dyc * dyc;
            if (rc2 <= rimOut2 && rc2 >= rimIn2) {
                float d = fabsf(sqrtf(rc2) - DISH_RIM_R) - DISH_RIM_HALF;
                uint8_t v = shade(0.5f - d, 0.0f);
                if (v < ink) ink = v;
            }

            float rh2 = dxh * dxh + dyh * dyh;
            if (rh2 <= holeOut2) {
                float rh = sqrtf(rh2);
                // The recess first, then the rim over it: MIN composition
                // means the darker rim wins wherever the two overlap, so
                // the order here is for reading, not for correctness.
                uint8_t w = shade(0.5f - (rh - HOLE_IN_R), HOLE_FILL_GRAY);
                if (w < ink) ink = w;
                float d = fabsf(rh - HOLE_RIM_R) - HOLE_RIM_HALF;
                uint8_t v = shade(0.5f - d, 0.0f);
                if (v < ink) ink = v;
            }

            if (hasRipple && rh2 <= rippleOut2 && rh2 >= rippleInn2) {
                float d = fabsf(sqrtf(rh2) - s->drawnRippleR) - RIPPLE_HALF;
                uint8_t v = shade(0.5f - d, rippleGray);
                if (v < ink) ink = v;
            }

            if (hasBall) {
                float dyb = fy - s->drawnBallY;
                float rb2 = dxb * dxb + dyb * dyb;
                if (rb2 <= ballOut2) {
                    float d = sqrtf(rb2) - s->drawnBallR;
                    uint8_t v = shade(0.5f - d, 0.0f);
                    if (v < ink) ink = v;
                }
            }

            gfx_fb[idx] = gray_to_px(ink);
        }
    }
}

// Pre-rounds a rectangle to exactly what gfx_push would round it to anyway
// (decision 0001), so the window PAINTED and the window PUSHED are the same
// rectangle. Byte-for-byte the same helper as level.c's align_for_push - see
// that file's comment for the full reasoning (landscape height becomes the
// pushed row length, so it is what must land on a multiple of 8).
static rect_t align_for_push(rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > LAND_W - 1) r.x1 = LAND_W - 1;
    if (r.y1 > LAND_H - 1) r.y1 = LAND_H - 1;

    r.y0 &= ~1;
    int h = r.y1 - r.y0 + 1;
    int need = (8 - (h & 7)) & 7;
    if (need > 0) {
        int room = (LAND_H - 1) - r.y1;
        int grow = need < room ? need : room;
        r.y1 += grow;
        need -= grow;
        if (need > 0) {
            int back = r.y0 < need ? r.y0 : need;
            back &= ~1;
            r.y0 -= back;
        }
    }

    r.x0 &= ~1;
    if (((r.x1 - r.x0 + 1) & 1) != 0) {
        if (r.x1 < LAND_W - 1) r.x1++;
        else r.x0--;
    }
    return r;
}

static void paint_and_push(const tiltball_state_t *s, rect_t r) {
    r = align_for_push(r);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;
    paint_rect(s, r);
    gfx_push_land(r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
}

static rect_t disc_rect(float cx, float cy, float rad) {
    rect_t r;
    r.x0 = (int)floorf(cx - rad - 1.5f);
    r.y0 = (int)floorf(cy - rad - 1.5f);
    r.x1 = (int)ceilf(cx + rad + 1.5f);
    r.y1 = (int)ceilf(cy + rad + 1.5f);
    return r;
}

static long rect_area(rect_t r) {
    if (r.x0 > r.x1 || r.y0 > r.y1) return 0;
    return (long)(r.x1 - r.x0 + 1) * (long)(r.y1 - r.y0 + 1);
}

static rect_t rect_union(rect_t a, rect_t b) {
    rect_t r;
    r.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}

static float quantise(float v) {
    return floorf(v / BALL_POS_QUANT + 0.5f) * BALL_POS_QUANT;
}

/* ---------------------------------------------------------------------
 * Physics: one damped, driven 2D spring - see this file's header comment,
 * "THE TILT-TO-ACCELERATION MAPPING".
 * ------------------------------------------------------------------- */

// Where the equilibrium for a given (possibly zero) in-plane tilt sits,
// clamped to the dish's own travel radius - used both by the settle check
// below and, implicitly, by the bowl's own restoring term every sub-step.
static void equilibrium_for(float gx, float gy, float *ex, float *ey) {
    float eqx = BALL_ACCEL_PER_G * gx / BALL_BOWL_OMEGA2;
    float eqy = BALL_ACCEL_PER_G * gy / BALL_BOWL_OMEGA2;
    float r = sqrtf(eqx * eqx + eqy * eqy);
    if (r > BALL_TRAVEL_MAX && r > 0.0001f) {
        eqx *= BALL_TRAVEL_MAX / r;
        eqy *= BALL_TRAVEL_MAX / r;
    }
    *ex = eqx;
    *ey = eqy;
}

static void step_physics(tiltball_state_t *s, const app_frame_t *f) {
    // !valid: no external push, but the bowl's own shape still pulls -
    // see this file's header comment, "!valid AND coasting".
    float gx = 0.0f, gy = 0.0f;
    if (f->tilt.valid) {
        gx = f->tilt.gx;
        gy = f->tilt.gy;
    }

    uint32_t remaining = f->dtMs; // already clamped to <=250ms by runtime_core.c
    while (remaining > 0) {
        uint32_t stepMs = remaining < BALL_SUBSTEP_MS ? remaining : BALL_SUBSTEP_MS;
        float dt = (float)stepMs / 1000.0f;

        float ax = BALL_ACCEL_PER_G * gx - BALL_BOWL_OMEGA2 * (s->px - BALL_CX) - BALL_DAMPING * s->vx;
        float ay = BALL_ACCEL_PER_G * gy - BALL_BOWL_OMEGA2 * (s->py - BALL_CY) - BALL_DAMPING * s->vy;

        s->vx += ax * dt;
        s->vy += ay * dt;

        float speed = sqrtf(s->vx * s->vx + s->vy * s->vy);
        if (speed > BALL_MAX_SPEED) {
            float k = BALL_MAX_SPEED / speed;
            s->vx *= k;
            s->vy *= k;
        }

        s->px += s->vx * dt;
        s->py += s->vy * dt;

        // Rim containment: a soft bounce, not a wall. Independent of the
        // bowl's own restoring pull (see this file's header comment on why
        // this is a backstop for transient overshoot, not the primary
        // containment).
        float dx = s->px - BALL_CX, dy = s->py - BALL_CY;
        float r = sqrtf(dx * dx + dy * dy);
        if (r > BALL_TRAVEL_MAX && r > 0.0001f) {
            float nx = dx / r, ny = dy / r;
            s->px = BALL_CX + nx * BALL_TRAVEL_MAX;
            s->py = BALL_CY + ny * BALL_TRAVEL_MAX;
            float vn = s->vx * nx + s->vy * ny; // outward radial component
            if (vn > 0.0f) {
                s->vx -= (1.0f + BALL_RESTITUTION) * vn * nx;
                s->vy -= (1.0f + BALL_RESTITUTION) * vn * ny;
            }
        }

        remaining -= stepMs;
    }

    // Settle snap - see this file's header comment and the constant's own
    // comment above for why this is needed for "a still puck costs
    // nothing" to actually hold for a spring, not just for a dial.
    float ex, ey;
    equilibrium_for(gx, gy, &ex, &ey);
    float toEqX = (BALL_CX + ex) - s->px, toEqY = (BALL_CY + ey) - s->py;
    float distToEq = sqrtf(toEqX * toEqX + toEqY * toEqY);
    float speedNow = sqrtf(s->vx * s->vx + s->vy * s->vy);
    if (distToEq < BALL_SETTLE_POS_EPS && speedNow < BALL_SETTLE_VEL_EPS) {
        s->px = BALL_CX + ex;
        s->py = BALL_CY + ey;
        s->vx = 0.0f;
        s->vy = 0.0f;
    }
}

/* ---------------------------------------------------------------------
 * What should be on screen right now, from the phase and its clock.
 * ------------------------------------------------------------------- */
static void compute_visual(const tiltball_state_t *s, uint32_t now,
                            float *bx, float *by, float *br, float *rr) {
    switch (s->phase) {
    case BALL_ROLLING:
        *bx = s->px;
        *by = s->py;
        *br = BALL_R;
        *rr = 0.0f;
        break;
    case BALL_CAPTURED: {
        uint32_t elapsed = now - s->phaseStartMs;
        float t = (float)elapsed / (float)CAPTURE_ANIM_MS;
        if (t > 1.0f) t = 1.0f;
        float e = t * t * t; // ease-in: slow to start, plunges at the end -
                              // reads as falling rather than sliding flat
        *bx = s->ballFromX + (HOLE_X - s->ballFromX) * e;
        *by = s->ballFromY + (HOLE_Y - s->ballFromY) * e;
        *br = BALL_R * (1.0f - t);
        *rr = ripple_radius_for(elapsed);
        break;
    }
    case BALL_HIDDEN:
        *bx = HOLE_X;
        *by = HOLE_Y;
        *br = 0.0f;
        *rr = ripple_radius_for(CAPTURE_ANIM_MS + (now - s->phaseStartMs));
        break;
    case BALL_RESPAWNING: {
        float t = (float)(now - s->phaseStartMs) / (float)RESPAWN_ANIM_MS;
        if (t > 1.0f) t = 1.0f;
        *bx = BALL_CX;
        *by = BALL_CY;
        *br = BALL_R * t;
        *rr = 0.0f;
        break;
    }
    }
}

/* ---------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------- */
static void tiltball_enter(void) {
    s_state = APP_STATE(tiltball_state_t);
    tiltball_state_t *s = s_state;

    // enter() takes no frame (app.h) - same rule level.c follows, and for
    // the same reason: no app here peeks at a sensor before its first
    // tick(). The ball starts at rest, dead centre.
    s->phase = BALL_ROLLING;
    s->phaseStartMs = 0;
    s->px = BALL_CX;
    s->py = BALL_CY;
    s->vx = 0.0f;
    s->vy = 0.0f;
    s->drawnBallX = BALL_CX;
    s->drawnBallY = BALL_CY;
    s->drawnBallR = BALL_R;
    s->drawnRippleR = 0.0f;

    // The whole dish, once. enter() does not push - the runtime pushes the
    // panel after enter() returns (app.h).
    rect_t all;
    all.x0 = (int)floorf(BALL_CX - DISH_RIM_R - DISH_RIM_HALF - 2.0f);
    all.y0 = (int)floorf(BALL_CY - DISH_RIM_R - DISH_RIM_HALF - 2.0f);
    all.x1 = (int)ceilf(BALL_CX + DISH_RIM_R + DISH_RIM_HALF + 2.0f);
    all.y1 = (int)ceilf(BALL_CY + DISH_RIM_R + DISH_RIM_HALF + 2.0f);
    paint_rect(s, all);
}

static void tiltball_tick(const app_frame_t *f) {
    tiltball_state_t *s = s_state;
    uint32_t now = f->nowMs;

    switch (s->phase) {
    case BALL_ROLLING:
        step_physics(s, f);
        {
            float ddx = s->px - HOLE_X, ddy = s->py - HOLE_Y;
            if (ddx * ddx + ddy * ddy < CAPTURE_TRIGGER_R * CAPTURE_TRIGGER_R) {
                s->ballFromX = s->px;
                s->ballFromY = s->py;
                s->phase = BALL_CAPTURED;
                s->phaseStartMs = now;
                // Fired exactly once, on this transition - the moment the
                // ball arrives, per this file's header comment on why that
                // instant gets more care than the physics.
                sound_play(SOUND_ID_BALL_CAPTURE);
            }
        }
        break;
    case BALL_CAPTURED:
        if (now - s->phaseStartMs >= CAPTURE_ANIM_MS) {
            s->phase = BALL_HIDDEN;
            s->phaseStartMs = now;
        }
        break;
    case BALL_HIDDEN:
        if (now - s->phaseStartMs >= HIDDEN_PAUSE_MS) {
            s->phase = BALL_RESPAWNING;
            s->phaseStartMs = now;
        }
        break;
    case BALL_RESPAWNING:
        if (now - s->phaseStartMs >= RESPAWN_ANIM_MS) {
            s->phase = BALL_ROLLING;
            s->phaseStartMs = now;
            s->px = BALL_CX;
            s->py = BALL_CY;
            s->vx = 0.0f;
            s->vy = 0.0f;
        }
        break;
    }

    float wantBallX, wantBallY, wantBallR, wantRippleR;
    compute_visual(s, now, &wantBallX, &wantBallY, &wantBallR, &wantRippleR);

    float nbx = quantise(wantBallX), nby = quantise(wantBallY);
    float nbr = quantise(wantBallR);
    if (nbr < 0.0f) nbr = 0.0f;
    float nrr = quantise(wantRippleR);
    if (nrr < 0.0f) nrr = 0.0f;

    bool ballChanged = (nbx != s->drawnBallX) || (nby != s->drawnBallY) || (nbr != s->drawnBallR);
    bool rippleChanged = (nrr != s->drawnRippleR);
    if (!ballChanged && !rippleChanged) return; // the same picture: no paint, no push

    rect_t oldBall = disc_rect(s->drawnBallX, s->drawnBallY, s->drawnBallR);
    rect_t newBall = disc_rect(nbx, nby, nbr);

    float rippleBoundR = (s->drawnRippleR > nrr) ? s->drawnRippleR : nrr;
    bool haveRippleRect = rippleChanged && rippleBoundR > 0.0f;
    rect_t rippleRect;
    if (haveRippleRect) rippleRect = disc_rect(HOLE_X, HOLE_Y, rippleBoundR + RIPPLE_HALF);

    // Commit the model FIRST: paint_rect renders s->drawn*, so everything
    // repainted below is the new picture, including the ground the old
    // ball was standing on - same ordering level.c uses and for the same
    // reason.
    s->drawnBallX = nbx;
    s->drawnBallY = nby;
    s->drawnBallR = nbr;
    s->drawnRippleR = nrr;

    rect_t rects[3];
    int n = 0;
    if (ballChanged) {
        rects[n++] = oldBall;
        rects[n++] = newBall;
    }
    if (haveRippleRect) rects[n++] = rippleRect;

    // Merge any pair whose union costs no more pixels than the two of them
    // separately - one push always beats two at equal area. Same technique
    // as level.c's own merge loop.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                rect_t u = rect_union(rects[i], rects[j]);
                if (rect_area(u) <= rect_area(rects[i]) + rect_area(rects[j])) {
                    rects[i] = u;
                    for (int k = j; k < n - 1; k++) rects[k] = rects[k + 1];
                    n--;
                    j--;
                }
            }
        }
    }

    for (int i = 0; i < n; i++) paint_and_push(s, rects[i]);
}

const app_t g_tiltballApp = {
    .name = "tiltball",
    .enter = tiltball_enter,
    .tick = tiltball_tick,
    .leave = NULL,
    .landscape = true,
    // No shake. There is nothing to reset here that tilting itself does not
    // already put right, and shake stays opt-in precisely so it remains the
    // Etch A Sketch gesture rather than a universal verb (decision 0002
    // section 5).
    .wantsShake = false,
};
