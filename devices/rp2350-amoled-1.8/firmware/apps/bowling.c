/*
 * bowling: "Bowling (lancer la balle avec le doigt)" - the owner's own
 * words for the app this file is. Flick a ball down a lane at six pins with
 * one finger. No text, no numbers, no score, no instructions: the only user
 * is a two year old who cannot read, so every rule here has to be learnable
 * by touching the thing.
 *
 * ---------------------------------------------------------------------
 * 1. THE THROW IS THE WHOLE GAME, AND THE HARDWARE IS AGAINST IT
 * ---------------------------------------------------------------------
 *
 * "Lancer la balle avec le doigt" is a flick: the ball's speed and
 * direction have to come from HOW her finger moved, which is a velocity
 * measurement, and this touch controller is hostile to exactly that:
 *
 *   - the FT3168 repeats the same coordinates ~60 times per new position,
 *     so naive velocity from consecutive samples is zero almost always and
 *     then a spike;
 *   - it drops contact ~34 times a second (measured on this board, see
 *     sketch.c and four.c's own dropout comments), so a flick that ends in
 *     a dropped frame can read as a release at the wrong place, or as no
 *     release at all;
 *   - it splits a stationary contact - reports a position 80-250px away
 *     that HOLDS for a report or three - about 10 times per 40 seconds
 *     (emulator/src/constants.ts's TOUCHSIM_HARDWARE_MEASURED), which is a
 *     believable "second sample" a naive two-point velocity would trust.
 *
 * THE ANSWER, mirroring sketch.c and four.c rather than inventing a second
 * one for the same hardware (the brief's own instruction):
 *
 *   - a gesture only starts once contact has persisted (ARM_SAMPLES over
 *     ARM_MS at ARM_RATE_HZ, four.c's own arming rule, copied verbatim: a
 *     lone stray cannot arm a throw at any spacing);
 *   - a release is only believed after RELEASE_GRACE_MS of GENUINE silence
 *     (four.c's own value, 300ms, chosen for the same reason: a throw is
 *     irreversible once it fires, so it gets the same grace a piece drop
 *     does, not the shorter grace a merely cosmetic stroke-split gets);
 *   - velocity is NOT "last sample minus previous sample". It is the
 *     displacement over elapsed TIME across a short ring buffer of the
 *     last few ACCEPTED contact samples (VBUF_LEN, ~100-250ms depending on
 *     the dropout rate at the time), i.e. exactly "derive dt from
 *     timestamps, never from a sample count" - see vbuf_velocity() below.
 *
 * WHAT HAPPENS WHEN THE LAST SAMPLE IS BAD, stated plainly because the
 * brief asks for it explicitly. This file does NOT try to detect a jitter
 * spike and reject it - decision 0010's own law is that an instrument
 * (here, a filter) only ever sees what is upstream of where it reads, and
 * nothing this file can compute distinguishes "a genuinely explosive flick
 * in her last few samples" from "the controller lied in her last few
 * samples". Trying to tell them apart and getting it wrong would be a
 * second, hidden bug. Instead the OUTPUT is bounded: the computed speed is
 * clamped to [MIN_LAUNCH_SPEED, MAX_LAUNCH_SPEED] before it ever reaches
 * the ball (launch_velocity_from_vbuf() below). So the worst a corrupted
 * final sample can do is:
 *
 *   - produce a below-MIN reading -> treated as "she let go without really
 *     throwing" -> the ball snaps back to her hand, nothing happens;
 *   - produce an above-MAX reading -> clamped to MAX_LAUNCH_SPEED, i.e. the
 *     hardest throw this app can ever produce - a real, physically
 *     plausible outcome (a hard throw), never a ball teleporting off the
 *     panel or a speed with no physical analogue.
 *
 * A dropped LAST sample specifically (the brief's other named case) is
 * covered by RELEASE_GRACE_MS the same way it is in four.c: a release is
 * bridged through short silences, so the "last accepted sample" used for
 * both the release position and the velocity window is a genuine recent
 * touch report, just up to one report period (or a bridged gap) older than
 * the true physical lift - not corrupted, only slightly stale, which
 * shaves a little off the tail of the flick rather than breaking it.
 *
 * A backward flick (vx <= 0, away from the pins) is treated as no throw at
 * all, the same bucket as "too slow": nothing on this lane is behind the
 * ball, so a backward fling has no sensible outcome to show, and "nothing
 * happened" is always a safe, discoverable answer on this device.
 *
 * ---------------------------------------------------------------------
 * 2. THE GESTURE: PRESS-DRAG-RELEASE, THE ONE IDIOM THIS DEVICE ALREADY
 *    TEACHES (the sketchpad's palette, Connect Four's column)
 * ---------------------------------------------------------------------
 *
 * A press has to start somewhere near the ball to "pick it up" - but a
 * fingertip on this panel is ~75-100px (AGENTS.md) and the ball is drawn
 * much smaller than that, so the grab has to be forgiving in EVERY
 * direction, not just one axis the way Connect Four's column choice is.
 * GRAB_RADIUS_PX is deliberately generous, and is re-checked every tick a
 * candidate gesture is live (not just at its first sample): a drag that
 * starts a little off and slides across the ball still picks it up. Once
 * grabbed, the ball tracks the finger 1:1 - immediate, honest feedback,
 * discoverable purely by touching - and every accepted sample feeds the
 * velocity ring buffer. Release (per RELEASE_GRACE_MS above) computes the
 * throw and either launches the ball or snaps it back.
 *
 * While the ball is in flight or resetting, touch is inert except for one
 * thing: a touch during the post-throw pause skips straight to the next
 * throw, the same "a touch never means two different things, it just
 * means hurry up" rule four.c's celebration uses.
 *
 * ---------------------------------------------------------------------
 * 3. WHAT THE GAME LOOKS LIKE, AND WHY
 * ---------------------------------------------------------------------
 *
 * A flattened arcade view, not a literal top-down one: the lane runs along
 * the landscape's long (X) axis, the ball is a disc, and the six pins are
 * drawn as small side-view silhouettes even though the lane itself reads
 * as flat. This is the ordinary convention plenty of simple 2D games use
 * (a character drawn side-on while walking on a 2D plane) and it is what
 * lets a toppled pin read as "knocked over" at all: a true top-down pin
 * would just be a dot, with no way to show it lying down.
 *
 * SIX PINS, NOT TEN. "Ten pins or fewer" was the owner's own allowance.
 * Fewer, bigger pins read clearly at 1.8 inches and arm's length, each one
 * big enough to look convincingly pin-shaped and to topple in a way that
 * is legible rather than a texture of small marks - the same lesson
 * menu.c's own icon post-mortems already recorded about small shapes at
 * this size (see draw_icon_four's header comment: "twelve shapes in a 96px
 * box is a texture"). A 3-2-1 triangle, apex toward the thrower, is the
 * real game's own arrangement in miniature.
 *
 * THE LANE IS ONE BOWED CAPSULE, NOT TWO STRAIGHT RAILS. The brief calls
 * this out by name: "a lane is the obvious place to reach for two straight
 * converging lines: think harder than that." A single
 * shapes_fill_capsule_aa_land call, centreline running the lane's length,
 * gives a rounded-ended belt with no straight edge and no right angle
 * anywhere in it (decision 0009) - and it does not narrow toward the pins
 * either, since a taper is exactly the two-straight-converging-lines shape
 * being avoided; the flattened-arcade convention already carries the
 * "distance" cue, so the lane does not also need to fake perspective.
 *
 * NO SCORE, NO COUNTING, NO FRAMES. She cannot read numbers, so a count
 * would be decoration nobody can use, and it would ask the device to
 * remember something across throws for a game whose whole point is "throw
 * again immediately" - the opposite of decision 0002's "each app is an
 * object, forgets on switch" is already this device's convention (see
 * four.c section 7, which reasons the same way about a half-finished
 * board). EVERY throw resets the whole rack, strike or gutter alike, so
 * she always gets a fresh ten... six pins to aim at. A strike (all six
 * down) gets ONE extra beat of stillness plus a breathing ring drawn
 * around the fallen pins before the reset - "celebrated without a word" -
 * and a gutter ball gets nothing extra at all: the ABSENCE of a
 * celebration is the tell, the same vocabulary four.c's own draw-vs-win
 * states use.
 *
 * NO SOUND, ON PURPOSE, FOR NOW - the same call four.c made for its own
 * piece-landing knock and for the same two reasons. First, sound.h offers
 * exactly one voice today (the timer's alarm chime); borrowing it for a
 * strike would teach her the alarm means nothing in particular, and a
 * genuinely new voice touches firmware/runtime/sound.c, which is a
 * board-only file this change cannot flash or hear - editing it blind,
 * with no way to confirm it against the real ES8311 and speaker, is a
 * worse trade than shipping the (silent, still satisfying) visual
 * reward loop now. Second, decision 0010 already named this exact
 * trade-off as low priority relative to orientation/storage/game-loop
 * work ("it just should not be decided eight times"). A short knock for a
 * strike is a clean, scoped follow-up whenever sound.c can be tried on the
 * board.
 *
 * ---------------------------------------------------------------------
 * 4. DECISION 0001, THE SAME WAY FOUR.C ASSERTS IT: BY CONSTRUCTION
 * ---------------------------------------------------------------------
 *
 * Exactly four.c's own trick. gfx maps a landscape rectangle (lx, ly, w, h)
 * to the panel rectangle (PANEL_W-(ly+h), lx, h, w): width and height
 * swap, so the PUSHED ROW LENGTH IS THE LANDSCAPE HEIGHT. Every redraw in
 * this file is a full-height landscape strip (ly=0, h=LAND_H=368, already
 * a multiple of 8), so gfx_push_land never has anything to round and there
 * is no per-frame bounding-box arithmetic that can get the 8-pixel rule
 * wrong. Dirty tracking (mark_span below) only ever decides WHICH x-range
 * to repaint; being wrong about that can cost a stale strip, never a
 * corrupted one. render_span() is a pure function of (state, time): it
 * starts from white and redraws everything the strip intersects - lane,
 * pins, ball, the strike ring - every time, never patches, so nothing can
 * be left behind (decision 0001's "settles-the-same" shape) and nothing
 * outside the strip is ever touched.
 *
 * ---------------------------------------------------------------------
 * 5. WHAT THIS FILE CANNOT VERIFY WITHOUT THE BOARD IN HAND
 * ---------------------------------------------------------------------
 *
 * Per decision 0010's own closing discipline: say what cannot see this
 * before someone finds out by shipping it.
 *
 *   - FEEL. Every constant below (GRAB_RADIUS_PX, the velocity scale, the
 *     launch speed clamp, RELEASE_GRACE_MS) is tuned by reading the
 *     dropout arithmetic and by watching the emulator's clean mouse drag,
 *     never by a real fingertip's real weight and real drop-outs at once.
 *     The emulator's TouchSim is measurably KINDER than the real FT3168
 *     (sketch.c's own SKETCH_LIVE_TUNE comment: 63-83% in the emulator
 *     against 3.5% on hardware for an equivalent rule), so a throw that
 *     feels right here is a hypothesis, exactly as decision 0003 already
 *     says about every app's timing.
 *   - Whether GRAB_RADIUS_PX is actually big enough for a real two-year-old
 *     fingertip (~75px contact, AGENTS.md) rather than the adult one this
 *     was reasoned from.
 *   - Whether the launch speed clamp reads as "a satisfying range of
 *     throws" on the real panel's actual frame timing, or whether the
 *     board's real ~16.7ms-median/129ms-tail sample pacing (the brief's
 *     own measured figures) makes the velocity window feel laggier or
 *     twitchier than the emulator's steadier clock suggests.
 *   - The pixel cost figures in this file's own comments are read from
 *     `tools/gate/run.ts --measure`, which counts pixels and pushes, never
 *     microseconds - "is it responsive" is a board question, always
 *     (decision 0003).
 */
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "gfx.h"

/* =====================================================================
 * Geometry, landscape space (LAND_W x LAND_H = 448 x 368), measured
 * against the VISIBLE canvas (gfx.h's PANEL_BEZEL_MARGIN_PX), exactly the
 * way four.c's own header insists on: nothing a child needs to see may
 * live under the case.
 * ================================================================== */
#define SAFE_X0 PANEL_BEZEL_MARGIN_PX
#define SAFE_X1 (LAND_W - PANEL_BEZEL_MARGIN_PX)
#define SAFE_Y0 PANEL_BEZEL_MARGIN_PX
#define SAFE_Y1 (LAND_H - PANEL_BEZEL_MARGIN_PX)

// The lane's centreline. Dead centre of the visible height, so aiming up
// or down from it is symmetric.
#define LANE_CY ((float)(SAFE_Y0 + SAFE_Y1) / 2.0f)

// The pale lane surface: one wide capsule, rounded ends, no taper (section
// 3 above). Wider than PLAY_HALF_W below so a ball bouncing off the play
// bound still visibly sits ON the lane rather than at its painted edge.
#define LANE_HALF_W   108.0f
#define LANE_X0       ((float)SAFE_X0 + 18.0f)
#define LANE_X1       ((float)SAFE_X1 - 14.0f)

// Where the ball actually bounces: comfortably inside the painted lane.
#define PLAY_HALF_W   86.0f

// The ball.
#define BALL_R 17.0f
#define BALL_START_X ((float)SAFE_X0 + 58.0f)
#define BALL_START_Y LANE_CY

// How close a press has to land to the ball's RESTING spot (or pass near
// it while dragging - see gesture_tick) to pick it up. Deliberately much
// larger than the ball itself: AGENTS.md measures a child's fingertip
// contact at ~75px on this panel, and a target has to be forgiving in
// every direction here, not just one axis the way four.c's column choice
// can afford to be (its target is a whole column, this app's is a small
// disc).
#define GRAB_RADIUS_PX 95.0f
#define GRAB_RADIUS_PX2 (GRAB_RADIUS_PX * GRAB_RADIUS_PX)

// Six pins, 3-2-1, apex toward the thrower (-X), back row toward the far
// wall (+X). Anchored off SAFE_X1 rather than off the apex, so the whole
// cluster and its fall/reset headroom stay a fixed, checked distance from
// the edge regardless of row/gap tuning.
#define PIN_COUNT     6
#define PIN_ROW_GAP   36.0f
#define PIN_COL_GAP   34.0f
#define PIN_BACK_X    ((float)SAFE_X1 - 62.0f)      // deepest row
#define PIN_APEX_X    (PIN_BACK_X - 2.0f * PIN_ROW_GAP)

#define PIN_HEIGHT    44.0f   // standing, base to tip
// A bowling pin's silhouette is base, WAIST, then a head that bulges back
// out - the head is what makes it a pin rather than a cone. PIN_R_TOP was
// 3.0, i.e. narrower than the waist above it, so the shape tapered to a
// point all the way up and read as a spike or an upside-down teardrop.
// draw_pin's own comment already said "then a small round head"; the code
// did not draw one. Every test passed either way, because none of them can
// read a picture.
#define PIN_R_BASE    8.5f
#define PIN_R_NECK    4.2f   // the waist, and it must be the NARROWEST point
#define PIN_R_TOP     6.2f   // the head, ~1.5x the waist, round-capped

// Domino radius: a standing pin within this of one that just toppled goes
// down too, one hop only (section on "domino chain" - simple and bounded
// rather than a real physics solve, and plenty for six pins this close
// together to look like a convincing strike).
#define DOMINO_R   (PIN_ROW_GAP + 8.0f)
#define DOMINO_R2  (DOMINO_R * DOMINO_R)

_Static_assert(PIN_APEX_X - GRAB_RADIUS_PX > BALL_START_X + GRAB_RADIUS_PX,
               "the pin cluster and the ball's grab zone overlap");
_Static_assert(LANE_CY - LANE_HALF_W >= (float)SAFE_Y0 &&
               LANE_CY + LANE_HALF_W <= (float)SAFE_Y1,
               "the lane surface reaches past the visible area / bezel");
_Static_assert(LANE_CY - PLAY_HALF_W - PIN_COL_GAP >= (float)SAFE_Y0 &&
               LANE_CY + PLAY_HALF_W + PIN_COL_GAP <= (float)SAFE_Y1,
               "the outer pins or the play bound reach past the visible area");

// How far past the strip actually touched a redraw is padded, so a shape's
// own anti-aliased fringe (or a chained capsule's join) can never be
// clipped mid-shape at a push boundary.
#define SPAN_PAD 22.0f

/* =====================================================================
 * Touch/gesture timing - copied from four.c's own arithmetic rather than
 * re-derived, per the brief's instruction to reuse the thinking rather
 * than invent a second answer to the same hardware. Both numbers are
 * measured against the SAME profile (34 dropout episodes/sec,
 * TOUCHSIM_HARDWARE_MEASURED / repro-touch-dropout-*.ts), and a throw is
 * exactly as irreversible as a Connect Four drop once it fires, so it
 * gets the same grace, not sketch.c's shorter cosmetic one.
 * ================================================================== */
#define ARM_SAMPLES 4
#define ARM_MS 40
#define ARM_RATE_HZ 15u
#define RELEASE_GRACE_MS 300u

// The velocity ring buffer: the last few ACCEPTED contact samples (dropped
// ticks simply do not push one, so the buffer's time span stretches
// automatically under heavier dropout rather than lying about a fixed
// number of milliseconds). 6 samples is ~100ms of clean 60Hz contact or
// ~150-250ms once the measured 34/sec dropout rate is thinning them - a
// short, recent window, long enough to average out one repeated-coordinate
// tick, short enough that a flick's tail dominates it.
#define VBUF_LEN 6
#define MIN_VEL_DT_MS 12.0f  // guards a divide when two samples land in the same ms

// The safety bound this file's header comment argues for at length: no
// matter how corrupted the last few samples are, the ball can only ever
// come out slower than this (treated as no throw) or faster than this
// (clamped) - never anything in between that looks broken.
#define MIN_LAUNCH_SPEED 0.16f   // px/ms; below this, "she let go without throwing"
#define MAX_LAUNCH_SPEED 1.55f   // px/ms; the hardest throw this app can ever produce
// The raw finger-speed-to-ball-speed gain. <1 because a flick's peak
// finger speed over a short window is faster than a satisfying ball speed
// on a ~370px lane - see MAX_LAUNCH_SPEED above, which is the real ceiling
// regardless of this gain.
#define LAUNCH_GAIN 0.62f

/* =====================================================================
 * Ball flight.
 * ================================================================== */
// Friction: linear deceleration of the ball's SPEED (not per-axis), so
// direction never drifts from drag alone.
//
// DERIVED FROM THE LANE'S OWN LENGTH, not picked and then eyeballed. A
// straight throw's stopping distance under constant deceleration is
// v^2 / (2*FRICTION), and the number that matters is how that compares to
// PIN_APEX_X - BALL_START_X (~236px, the distance to the HEADPIN, the
// nearest thing that can be hit at all): a friction high enough that only
// near-MAX_LAUNCH_SPEED throws ever reach the rack would make most
// ordinary flicks fall silently short, which reads as "broken" rather than
// as a soft throw. Solved instead for a MODERATE throw
// (~0.5 px/ms of raw finger speed * LAUNCH_GAIN, i.e. a ball speed around
// 0.35) to reach the headpin with a little momentum to spare:
// FRICTION = 0.35^2 / (2 * 236) ~= 0.00026.
//
//   speed   stopping distance   what happens
//   ------  ------------------  --------------------------------------
//   MIN_LAUNCH_SPEED (0.16)     ~49px       falls well short of the pins -
//                                           a weak nudge, visibly a weak
//                                           throw, never absurd
//   a moderate flick (~0.35)    ~236px      just reaches the headpin
//   MAX_LAUNCH_SPEED (1.55)     ~4600px     never stops on its own inside
//                                           the lane at all - a hard throw
//                                           always ends by reaching the far
//                                           wall (see FLIGHT below), which
//                                           is the realistic outcome for a
//                                           ball thrown that hard
#define FRICTION_PX_PER_MS2 0.00026f
#define STOP_SPEED 0.05f            // below this, the ball is considered stopped
#define FLIGHT_SAFETY_MS 3000u      // backstop: nothing here should ever take this long
#define BOUNCE_DAMP 0.45f           // off the lane's side bounds, four.c's own BOUNCE_KEEP idea

/* =====================================================================
 * Pacing.
 * ================================================================== */
#define FALL_MS 260.0f              // a pin's own topple, base to lying flat
#define CHAIN_DELAY_MS 90u          // stagger before a domino-neighbour starts falling
#define RESET_STAGGER_MS 55u        // pins pop back up left-to-right-ish, staggered
#define RESET_MS 260.0f
#define PIN_RESET_TOTAL_MS ((uint32_t)((PIN_COUNT - 1) * RESET_STAGGER_MS + (uint32_t)RESET_MS))
#define BALL_RETURN_GAP_MS 140u     // beat between pins settling and the ball sliding back
#define BALL_RETURN_MS 260.0f
#define PAUSE_MS 650u               // ordinary pause before reset
#define PAUSE_STRIKE_MS 1350u       // longer, so the breathing ring is actually seen
#define RENDER_MIN_MS 16u           // ~60fps repaint throttle, four.c's own number

// The strike ring: a plain black annulus around the fallen cluster,
// breathing gently (radius only - shapes.h's AA primitives are opaque,
// MIN-composited, with no alpha, so "fading" is not on offer without a
// second, source-over blend path four.c had to write for its own coloured
// halo; a breathing radius is the honest thing this app can afford
// cheaply and it reads perfectly well as "something happened here").
#define STRIKE_RING_R0 (PIN_ROW_GAP * 1.7f)
#define STRIKE_RING_R1 (PIN_ROW_GAP * 2.15f)
#define STRIKE_RING_STROKE 4.0f
#define STRIKE_BREATHE_MS 480.0f

/* =====================================================================
 * State.
 * ================================================================== */
typedef struct {
    float x, y;
    uint32_t t;
} vsample_t;

typedef struct {
    float baseX, baseY;    // fixed pivot; never moves
    bool  down;            // knocked over this throw (still true while rising back up)
    uint32_t fellAtMs;      // when it (or its domino trigger) went down; may be in the
                            // future for a staggered chain reaction - see fall_u()
    float dirX, dirY;       // unit fall direction
} pin_t;

enum {
    PH_READY,   // ball at rest, waiting to be grabbed
    PH_HELD,    // a finger has it, tracking touch
    PH_FLYING,  // released, physics-driven
    PH_PAUSE,   // flight resolved; showing the result
    PH_RESET,   // pins popping back up, then the ball sliding home
};

typedef struct {
    uint8_t phase;
    uint32_t phaseStartMs;

    // The ball.
    float ballX, ballY;
    float ballVX, ballVY;   // valid only while PH_FLYING
    float ballFromX, ballFromY; // where it stood when PH_RESET began (its
                                 // flight's resting spot) - the fixed start
                                 // point for the return-slide interpolation,
                                 // captured once so the ball eases smoothly
                                 // toward BALL_START rather than chasing a
                                 // moving target each tick.

    // The gesture (mirrors four.c's gesture_tick shape exactly).
    bool  contactSeen;
    bool  armed;
    uint32_t gestureStartMs;
    uint32_t lastContactMs;
    int   contactCount;

    vsample_t vbuf[VBUF_LEN];
    int vbufCount;

    bool wasStrike;

    pin_t pins[PIN_COUNT];

    // Dirty tracking: one unioned x-range per tick, always pushed as a
    // full-height strip - section 4 of this file's header comment.
    bool  dirty;
    float dirtyX0, dirtyX1;
    bool  dirtyAll;
    uint32_t lastRenderMs;
} bowling_state_t;

static bowling_state_t *s_state;

/* =====================================================================
 * Small helpers.
 * ================================================================== */
// x*x*(3-2x): cheap, monotonic, no overshoot in either direction - used for
// both the fall (0->1) and the reset-rise (1->0, by feeding 1-u).
static float smoothstep(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

static void pin_layout_init(pin_t pins[PIN_COUNT]) {
    // row 0: the single apex pin, nearest the thrower.
    pins[0].baseX = PIN_APEX_X;         pins[0].baseY = LANE_CY;
    // row 1: two pins.
    pins[1].baseX = PIN_APEX_X + PIN_ROW_GAP; pins[1].baseY = LANE_CY - PIN_COL_GAP / 2.0f;
    pins[2].baseX = PIN_APEX_X + PIN_ROW_GAP; pins[2].baseY = LANE_CY + PIN_COL_GAP / 2.0f;
    // row 2: three pins, the back wall.
    pins[3].baseX = PIN_BACK_X;         pins[3].baseY = LANE_CY - PIN_COL_GAP;
    pins[4].baseX = PIN_BACK_X;         pins[4].baseY = LANE_CY;
    pins[5].baseX = PIN_BACK_X;         pins[5].baseY = LANE_CY + PIN_COL_GAP;
    for (int i = 0; i < PIN_COUNT; i++) {
        pins[i].down = false;
        pins[i].fellAtMs = 0;
        pins[i].dirX = 1.0f;
        pins[i].dirY = 0.0f;
    }
}

static void mark_span(bowling_state_t *s, float x0, float x1) {
    x0 -= SPAN_PAD;
    x1 += SPAN_PAD;
    if (!s->dirty) {
        s->dirty = true;
        s->dirtyX0 = x0;
        s->dirtyX1 = x1;
    } else {
        if (x0 < s->dirtyX0) s->dirtyX0 = x0;
        if (x1 > s->dirtyX1) s->dirtyX1 = x1;
    }
}

static void mark_ball(bowling_state_t *s) {
    mark_span(s, s->ballX - BALL_R, s->ballX + BALL_R);
}

static void mark_pin(bowling_state_t *s, int i) {
    // Wide enough to cover the pin lying flat in any direction.
    mark_span(s, s->pins[i].baseX - PIN_HEIGHT, s->pins[i].baseX + PIN_HEIGHT);
}

static void mark_all(bowling_state_t *s) { s->dirtyAll = true; }

/* =====================================================================
 * The velocity ring buffer.
 * ================================================================== */
static void vbuf_reset(bowling_state_t *s) { s->vbufCount = 0; }

static void vbuf_push(bowling_state_t *s, float x, float y, uint32_t t) {
    if (s->vbufCount < VBUF_LEN) {
        s->vbuf[s->vbufCount++] = (vsample_t){ x, y, t };
        return;
    }
    for (int i = 1; i < VBUF_LEN; i++) s->vbuf[i - 1] = s->vbuf[i];
    s->vbuf[VBUF_LEN - 1] = (vsample_t){ x, y, t };
}

// Raw finger velocity (px/ms) over the buffered window - see this file's
// header comment section 1 for why this is a displacement-over-elapsed-
// TIME window rather than a two-sample derivative.
static bool vbuf_velocity(const bowling_state_t *s, float *vx, float *vy) {
    if (s->vbufCount < 2) return false;
    const vsample_t *a = &s->vbuf[0];
    const vsample_t *b = &s->vbuf[s->vbufCount - 1];
    float dt = (float)(b->t - a->t);
    if (dt < MIN_VEL_DT_MS) return false;
    *vx = (b->x - a->x) / dt;
    *vy = (b->y - a->y) / dt;
    return true;
}

// The throw, end to end: raw finger velocity -> a launch velocity that can
// never be absurd. Returns false for "no throw" (too slow, or backward).
static bool launch_velocity_from_vbuf(const bowling_state_t *s, float *outVX, float *outVY) {
    float fvx, fvy;
    if (!vbuf_velocity(s, &fvx, &fvy)) return false;
    float vx = fvx * LAUNCH_GAIN, vy = fvy * LAUNCH_GAIN;
    if (vx <= 0.0f) return false; // not thrown toward the pins - section 1
    float speed = sqrtf(vx * vx + vy * vy);
    if (speed < MIN_LAUNCH_SPEED) return false;
    if (speed > MAX_LAUNCH_SPEED) {
        float k = MAX_LAUNCH_SPEED / speed;
        vx *= k; vy *= k;
    }
    *outVX = vx;
    *outVY = vy;
    return true;
}

/* =====================================================================
 * Pins: falling, chaining, resetting.
 * ================================================================== */
static float pin_fall_u(const bowling_state_t *s, int i, uint32_t nowMs) {
    const pin_t *p = &s->pins[i];
    if (!p->down) return 0.0f;
    if (s->phase == PH_RESET) {
        uint32_t windowStart = s->phaseStartMs + (uint32_t)i * RESET_STAGGER_MS;
        if (nowMs <= windowStart) return 1.0f; // hasn't started rising yet
        float u = (float)(nowMs - windowStart) / RESET_MS;
        return 1.0f - smoothstep(u);
    }
    // Falling (or already fallen and just sitting there through the pause).
    if (nowMs <= p->fellAtMs) return 0.0f; // a staggered domino neighbour, not due yet
    float u = (float)(nowMs - p->fellAtMs) / FALL_MS;
    return smoothstep(u);
}

// True while pin i's own topple animation still has visible work left to
// do: already down, and either its fellAtMs is still in the future (a
// staggered domino neighbour that has not started yet) or less than
// FALL_MS has elapsed since it started. Used to keep RE-MARKING a pin's
// span every tick its silhouette is still changing - topple_pin() below
// only marks it ONCE, at the instant of collision (or of being chained),
// and a chained pin's fellAtMs can be up to CHAIN_DELAY_MS in the future,
// so without this a chained pin's fall would never actually be seen
// animating: the one mark from topple_pin() would cover its span for a
// single push and nothing would repaint it again until some UNRELATED
// dirty event happened to touch that span later. Found by this file's own
// feature-bowling.ts, which caught the residue this produced: a chained
// pin's mid-fall frame reached the panel and then sat there un-erased
// while later frames (rendered for a different reason) redrew everything
// AROUND it without including its own now-stale span.
static bool pin_falling_in_progress(const bowling_state_t *s, int i, uint32_t nowMs) {
    const pin_t *p = &s->pins[i];
    if (!p->down) return false;
    return nowMs < p->fellAtMs + (uint32_t)FALL_MS;
}

// Knocks pin i down at time fellAtMs, in direction (dirX,dirY), and chains
// to any standing neighbour within DOMINO_R (one hop - section 3). Marks
// whatever spans need repainting.
static void topple_pin(bowling_state_t *s, int i, uint32_t fellAtMs, float dirX, float dirY) {
    if (s->pins[i].down) return;
    s->pins[i].down = true;
    s->pins[i].fellAtMs = fellAtMs;
    s->pins[i].dirX = dirX;
    s->pins[i].dirY = dirY;
    mark_pin(s, i);
    printf("bowling: pin %d down at t=%lu\r\n", i, (unsigned long)fellAtMs);

    for (int j = 0; j < PIN_COUNT; j++) {
        if (j == i || s->pins[j].down) continue;
        float dx = s->pins[j].baseX - s->pins[i].baseX;
        float dy = s->pins[j].baseY - s->pins[i].baseY;
        if (dx * dx + dy * dy > DOMINO_R2) continue;
        s->pins[j].down = true;
        s->pins[j].fellAtMs = fellAtMs + CHAIN_DELAY_MS;
        s->pins[j].dirX = dirX;
        s->pins[j].dirY = dirY;
        mark_pin(s, j);
        printf("bowling: pin %d chained down at t=%lu\r\n", j, (unsigned long)(fellAtMs + CHAIN_DELAY_MS));
    }
}

static bool all_pins_down(const bowling_state_t *s) {
    for (int i = 0; i < PIN_COUNT; i++) if (!s->pins[i].down) return false;
    return true;
}

/* =====================================================================
 * Drawing.
 *
 * NOT shapes.h's AA primitives, on purpose, and this is the same call
 * four.c's own header comment makes and argues for at length: "[shapes.h]
 * cannot clip to an x span, which is the whole basis of this file's push
 * discipline." shapes_fill_capsule_aa_land/_disc_aa_land/_annulus_aa_land
 * take no clip parameter at all - they paint wherever their geometry says,
 * however narrow the strip that is about to be pushed is. This file's
 * FIRST version called them directly and passed every clean-input check;
 * feature-bowling.ts's own residue invariant (the one every test file in
 * this directory carries, mirroring the gate's `pushed-or-invisible` rule)
 * caught what that missed: a pin mid-topple, redrawn as part of a LATER,
 * narrower strip triggered by the ball alone, painted its own tip several
 * pixels past that strip's right edge - correct in the framebuffer,
 * because the shape itself doesn't know or care what got pushed, and
 * exactly decision 0001's "correct in the emulator, stale on the panel"
 * shape of bug on the real device.
 *
 * So every primitive below is a local, CLIPPED reimplementation of the
 * same signed-distance-to-coverage technique shapes.c already uses
 * (sketch.c's draw_capsule is the original source both copy), composited
 * by MIN exactly as shapes.h's own family is - ink on paper, not
 * source-over, so an overlapping pin and lane tint union rather than
 * re-darken. render_span() below is what four.c's own comment describes:
 * "no primitive is allowed to draw outside the clip it is handed."
 * ================================================================== */
// A pale warm-neutral grey (logical RGB565): darker than white, much
// lighter than black ink, MIN-composites cleanly under a pin or the ball.
// px_swap() at the use site, not baked into the constant - px_to_gray()
// (gfx.h) un-swaps whatever it is handed to extract the logical channels,
// so a colour a caller passes in must already be in the SAME swapped,
// framebuffer-storage format gfx_fb itself holds (px_to_gray reads
// gfx_fb[idx] directly with no conversion). four.c's own col_piece() etc.
// hit the same requirement and solve it the same way: a raw hex literal is
// the LOGICAL colour, wrapped in px_swap() at every use, since px_swap()
// is a real function call and cannot appear inside a #define used as a
// constant expression. Missing this wrap was found by the gate's own
// `bezel` rule: the lane's intended pale grey (~76% logical brightness,
// gray value ~194) rendered instead as gray value 24 - reading as solid
// ink wherever the capsule's soft, deliberately off-canvas round end caps
// (decision 0009: no straight lane edges) happened to graze the bezel
// band, which a genuinely pale tint there is allowed to do and a
// near-black one is not (rules/rp2350-amoled-1.8.ts's inkGrayMax=128).
#define COL_LANE 0xC618

// Composites one landscape pixel by MIN, exactly shapes.c's own
// aa_composite_land, with an added x-clip: a coverage value outside
// [clipX0, clipX1) contributes nothing, however far into "inside the
// shape" territory it would otherwise be.
static inline void ink_clip(int lx, int ly, float coverage, uint8_t targetGray, int clipX0, int clipX1) {
    if (lx < clipX0 || lx >= clipX1 || ly < 0 || ly >= LAND_H) return;
    if (coverage <= 0.0f) return;
    if (coverage > 1.0f) coverage = 1.0f;
    uint8_t ink = (uint8_t)((float)targetGray * coverage + 255.0f * (1.0f - coverage) + 0.5f);
    int px = PANEL_W - 1 - ly;
    int py = lx;
    int idx = py * PANEL_W + px;
    uint8_t cur = px_to_gray(gfx_fb[idx]);
    if (ink < cur) gfx_fb[idx] = gray_to_px(ink);
}

static void fill_disc_clip(float cx, float cy, float r, uint16_t colorPx, int clipX0, int clipX1) {
    uint8_t targetGray = px_to_gray(colorPx);
    int minX = (int)floorf(cx - r - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(cx + r + 1.0f); if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(cy - r - 1.0f);
    int maxY = (int)ceilf(cy + r + 1.0f);
    if (minX > maxX) return;
    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float dx = px - cx, dy = py - cy;
            float d = sqrtf(dx * dx + dy * dy) - r;
            ink_clip(lx, ly, 0.5f - d, targetGray, clipX0, clipX1);
        }
    }
}

static void fill_annulus_clip(float cx, float cy, float rOuter, float rInner,
                               uint16_t colorPx, int clipX0, int clipX1) {
    uint8_t targetGray = px_to_gray(colorPx);
    int minX = (int)floorf(cx - rOuter - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(cx + rOuter + 1.0f); if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(cy - rOuter - 1.0f);
    int maxY = (int)ceilf(cy + rOuter + 1.0f);
    if (minX > maxX) return;
    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float dx = px - cx, dy = py - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float dOuter = dist - rOuter, dInner = dist - rInner;
            float d = dOuter > -dInner ? dOuter : -dInner;
            ink_clip(lx, ly, 0.5f - d, targetGray, clipX0, clipX1);
        }
    }
}

static void fill_capsule_clip(float x0, float y0, float r0, float x1, float y1, float r1,
                               uint16_t colorPx, int clipX0, int clipX1) {
    uint8_t targetGray = px_to_gray(colorPx);
    float maxR = (r0 > r1 ? r0 : r1) + 1.0f;
    int minX = (int)floorf((x0 < x1 ? x0 : x1) - maxR); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf((x0 > x1 ? x0 : x1) + maxR); if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf((y0 < y1 ? y0 : y1) - maxR);
    int maxY = (int)ceilf((y0 > y1 ? y0 : y1) + maxR);
    if (minX > maxX) return;
    float abx = x1 - x0, aby = y1 - y0;
    float abLenSq = abx * abx + aby * aby;
    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float t = 0.0f;
            if (abLenSq > 0.0001f) {
                t = ((px - x0) * abx + (py - y0) * aby) / abLenSq;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            }
            float cx = x0 + abx * t, cy = y0 + aby * t;
            float dx = px - cx, dy = py - cy;
            float d = sqrtf(dx * dx + dy * dy) - (r0 + (r1 - r0) * t);
            ink_clip(lx, ly, 0.5f - d, targetGray, clipX0, clipX1);
        }
    }
}

static void draw_lane(int clipX0, int clipX1) {
    fill_capsule_clip(LANE_X0, LANE_CY, LANE_HALF_W, LANE_X1, LANE_CY, LANE_HALF_W,
                       px_swap((uint16_t)COL_LANE), clipX0, clipX1);
}

// One pin, base fixed at (bx,by), tip swept from straight up (u=0,
// standing) to lying along (dirX,dirY) (u=1, fallen). Two chained capsules
// give the classic pin bulge: a fat lower body tapering to a narrow neck,
// then a small round head - see shapes.h's own comment on why chaining two
// calls at a shared midpoint reads as one continuous shape rather than a
// hard joint.
static void draw_pin(float bx, float by, float dirX, float dirY, float u, uint16_t color, int clipX0, int clipX1) {
    // Standing direction is straight "up" the lane's own local axis, which
    // this flattened side-view draws as -Y (toward the far row) blended
    // with a little -X lean so a standing pin still reads as upright
    // rather than as a horizontal capsule pointing at the camera. Simpler
    // and just as legible: standing tip is directly "north" in landscape
    // space (0,-1); interpolating its unit vector toward (dirX,dirY) via u
    // sweeps it down into the fallen pose.
    float stx = 0.0f, sty = -1.0f;
    float tx = stx + (dirX - stx) * u;
    float ty = sty + (dirY - sty) * u;
    float len = sqrtf(tx * tx + ty * ty);
    if (len < 0.001f) { tx = stx; ty = sty; len = 1.0f; }
    tx /= len; ty /= len;

    // The pin shortens visually as it lies down (foreshortening this
    // side-view already implies), from PIN_HEIGHT standing to about 62% of
    // that lying flat - long enough to still read as a pin, short enough
    // to look like it is now lying rather than still standing.
    float h = PIN_HEIGHT * (1.0f - 0.38f * u);
    // The waist sits high (0.64, was 0.55): a pin's body is most of its
    // height and its neck-and-head are the short bit on top. At 0.55 the
    // head section was nearly half the pin, which reads as a cone with a
    // knob rather than as a pin.
    float neckT = 0.64f;
    float midX = bx + tx * h * neckT, midY = by + ty * h * neckT;
    float tipX = bx + tx * h,          tipY = by + ty * h;
    float rBase = PIN_R_BASE, rNeck = PIN_R_NECK * (1.0f - 0.25f * u), rTip = PIN_R_TOP;

    fill_capsule_clip(bx, by, rBase, midX, midY, rNeck, color, clipX0, clipX1);
    fill_capsule_clip(midX, midY, rNeck, tipX, tipY, rTip, color, clipX0, clipX1);
}

static void draw_ball(float x, float y, uint16_t color, int clipX0, int clipX1) {
    fill_disc_clip(x, y, BALL_R, color, clipX0, clipX1);
}

static void render_span(bowling_state_t *s, uint32_t nowMs, int lx0, int lx1) {
    if (lx0 < 0) lx0 = 0;
    if (lx1 > LAND_W) lx1 = LAND_W;
    if (lx1 <= lx0) return;

    gfx_fill_rect_land(lx0, 0, lx1 - lx0, LAND_H, PX_WHITE);
    draw_lane(lx0, lx1);

    for (int i = 0; i < PIN_COUNT; i++) {
        const pin_t *p = &s->pins[i];
        if (p->baseX + PIN_HEIGHT < (float)lx0 || p->baseX - PIN_HEIGHT > (float)lx1) continue;
        float u = pin_fall_u(s, i, nowMs);
        draw_pin(p->baseX, p->baseY, p->dirX, p->dirY, u, PX_BLACK, lx0, lx1);
    }

    if (s->phase == PH_PAUSE && s->wasStrike) {
        float cx = (PIN_APEX_X + PIN_BACK_X) / 2.0f;
        if (cx + STRIKE_RING_R1 >= (float)lx0 && cx - STRIKE_RING_R1 <= (float)lx1) {
            float breathe = 0.5f - 0.5f * cosf(2.0f * 3.14159265f *
                (float)(nowMs - s->phaseStartMs) / STRIKE_BREATHE_MS);
            float rOut = STRIKE_RING_R0 + (STRIKE_RING_R1 - STRIKE_RING_R0) * breathe;
            fill_annulus_clip(cx, LANE_CY, rOut, rOut - STRIKE_RING_STROKE, PX_BLACK, lx0, lx1);
        }
    }

    if (s->ballX + BALL_R >= (float)lx0 && s->ballX - BALL_R <= (float)lx1) {
        draw_ball(s->ballX, s->ballY, PX_BLACK, lx0, lx1);
    }
}

static void flush(bowling_state_t *s, uint32_t nowMs) {
    if (s->dirtyAll) {
        render_span(s, nowMs, 0, LAND_W);
        gfx_push_land(0, 0, LAND_W, LAND_H);
        s->dirtyAll = false;
        s->dirty = false;
        return;
    }
    if (!s->dirty) return;
    int lx0 = (int)floorf(s->dirtyX0);
    int lx1 = (int)ceilf(s->dirtyX1);
    render_span(s, nowMs, lx0, lx1);
    gfx_push_land(lx0 < 0 ? 0 : lx0, 0, (lx1 > LAND_W ? LAND_W : lx1) - (lx0 < 0 ? 0 : lx0), LAND_H);
    s->dirty = false;
}

/* =====================================================================
 * Phase transitions.
 * ================================================================== */
static void go_ready(bowling_state_t *s, uint32_t nowMs) {
    s->phase = PH_READY;
    s->phaseStartMs = nowMs;
    s->ballX = BALL_START_X;
    s->ballY = BALL_START_Y;
    s->ballVX = s->ballVY = 0.0f;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    vbuf_reset(s);
    for (int i = 0; i < PIN_COUNT; i++) {
        s->pins[i].down = false;
        s->pins[i].fellAtMs = 0;
    }
    mark_all(s);
}

static void resolve_flight(bowling_state_t *s, uint32_t nowMs) {
    s->wasStrike = all_pins_down(s);
    s->phase = PH_PAUSE;
    s->phaseStartMs = nowMs;
    mark_ball(s);
    printf("bowling: flight resolved strike=%d\r\n", (int)s->wasStrike);
}

static void start_reset(bowling_state_t *s, uint32_t nowMs) {
    s->phase = PH_RESET;
    s->phaseStartMs = nowMs;
    // The ball has been frozen at its flight's resting spot throughout
    // PH_PAUSE and stays frozen through the pins' own reset, so this is
    // exactly the point the return-slide eases away from.
    s->ballFromX = s->ballX;
    s->ballFromY = s->ballY;
    mark_all(s);
}

/* =====================================================================
 * The gesture. Structurally four.c's gesture_tick, adapted from "which
 * column" to "is this near the ball, and what is its recent velocity" -
 * see this file's header comment section 1 for why the shape is copied
 * rather than reinvented.
 * ================================================================== */
static void gesture_tick(bowling_state_t *s, const app_frame_t *f) {
    bool live = (s->phase == PH_READY || s->phase == PH_HELD);

    if (!live) {
        if (s->contactSeen || s->armed) {
            s->contactSeen = false;
            s->armed = false;
            s->contactCount = 0;
            vbuf_reset(s);
        }
        return;
    }

    if (f->touchDown) {
        if (!s->contactSeen) {
            s->contactSeen = true;
            s->gestureStartMs = f->nowMs;
            s->contactCount = 0;
        }
        s->lastContactMs = f->nowMs;
        s->contactCount++;

        uint32_t elapsed = f->nowMs - s->gestureStartMs;
        if (!s->armed && s->contactCount >= ARM_SAMPLES && elapsed >= ARM_MS &&
            (uint32_t)s->contactCount * 1000u >= ARM_RATE_HZ * elapsed) {
            s->armed = true;
        }

        // touchX/Y arrive in PANEL coordinates; the inverse of gfx.h's own
        // documented landscape->panel mapping, exactly four.c's own
        // conversion for the same reason (it is the only other landscape
        // app that hit-tests a raw touch point).
        float lx = (float)f->touchY;
        float ly = (float)(PANEL_W - 1 - f->touchX);

        if (s->phase == PH_READY) {
            if (s->armed) {
                float dx = lx - BALL_START_X, dy = ly - BALL_START_Y;
                if (dx * dx + dy * dy <= GRAB_RADIUS_PX2) {
                    // Grabbed. Seed the ball at the finger immediately -
                    // honest, discoverable feedback - and start the
                    // velocity window from here.
                    s->phase = PH_HELD;
                    s->phaseStartMs = f->nowMs;
                    mark_ball(s);
                    s->ballX = lx; s->ballY = ly;
                    mark_ball(s);
                    vbuf_reset(s);
                    vbuf_push(s, lx, ly, f->nowMs);
                }
            }
            return;
        }

        // PH_HELD: track the finger every accepted sample.
        mark_ball(s);
        s->ballX = lx; s->ballY = ly;
        mark_ball(s);
        vbuf_push(s, lx, ly, f->nowMs);
        return;
    }

    // No contact this tick - almost always a dropout, not a lift, per
    // four.c's own reasoning; only believe a release after real silence.
    if (!s->contactSeen) return;
    if ((f->nowMs - s->lastContactMs) < RELEASE_GRACE_MS) return;

    bool wasHeld = (s->phase == PH_HELD);
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;

    if (!wasHeld) { vbuf_reset(s); return; }

    float vx, vy;
    if (launch_velocity_from_vbuf(s, &vx, &vy)) {
        s->phase = PH_FLYING;
        s->phaseStartMs = f->nowMs;
        s->ballVX = vx;
        s->ballVY = vy;
        // %d on a truncated float silently drops the sign of anything
        // between -1 and 0 ((int)(-0.3f) is 0, not -1), so vy - which CAN
        // be negative, unlike vx which launch_velocity_from_vbuf() already
        // guarantees positive - gets an explicit sign character rather than
        // trusting the integer part to carry one.
        printf("bowling: launch vx=%d.%03d vy=%s%d.%03d\r\n",
               (int)vx, (int)(fabsf(vx - (int)vx) * 1000),
               vy < 0.0f ? "-" : "", (int)fabsf(vy), (int)(fabsf(vy - (int)vy) * 1000));
    } else {
        // Too slow, or thrown the wrong way: she let go without really
        // throwing. The ball snaps back rather than sitting stranded
        // wherever her finger happened to be - see section 1.
        go_ready(s, f->nowMs);
        return;
    }
    vbuf_reset(s);
}

/* =====================================================================
 * enter / tick.
 * ================================================================== */
static void bowling_enter(void) {
    s_state = APP_STATE(bowling_state_t);
    bowling_state_t *s = s_state;
    pin_layout_init(s->pins);
    go_ready(s, 0);
    s->dirtyAll = false;  // enter() must not push itself - the runtime does, once
    s->dirty = false;
    render_span(s, 0, 0, LAND_W);
    printf("bowling: state=%d bytes (arena %d)\r\n", (int)sizeof(bowling_state_t), (int)APP_ARENA_BYTES);
}

static void bowling_tick(const app_frame_t *f) {
    bowling_state_t *s = s_state;
    uint32_t now = f->nowMs;

    // BOOT abandons whatever is happening and deals a fresh rack - the same
    // "one physical button, one obvious job" every other app on this
    // device gives it.
    if (f->bootClicked && s->phase != PH_READY) {
        go_ready(s, now);
    }

    // A touch during the post-throw pause or the reset just hurries it
    // along - decision 0002's "a touch never means two different things".
    if (f->touchDown && (s->phase == PH_PAUSE || s->phase == PH_RESET)) {
        go_ready(s, now);
    }

    gesture_tick(s, f);

    switch (s->phase) {
    case PH_READY:
        // Nothing animates here on its own; the ball is genuinely still
        // until touched. (No idle bob: PAUSE_STRIKE_MS's breathing ring and
        // the throw-to-throw cycle itself already keep this screen from
        // ever sitting static for long once she is playing at all, and the
        // very first frame after enter() is exactly as static as every
        // other app's own resting state - chrono at 0, the sketchpad's
        // blank page.)
        break;

    case PH_HELD:
        // Rendering already happened inside gesture_tick on every accepted
        // sample; nothing to integrate here.
        break;

    case PH_FLYING: {
        float dt = (float)f->dtMs;
        mark_ball(s);
        s->ballX += s->ballVX * dt;
        s->ballY += s->ballVY * dt;

        // Bounce off the play bounds - four.c's own BOUNCE_KEEP idea,
        // renamed BOUNCE_DAMP here.
        if (s->ballY < LANE_CY - PLAY_HALF_W) {
            s->ballY = LANE_CY - PLAY_HALF_W;
            s->ballVY = -s->ballVY * BOUNCE_DAMP;
        } else if (s->ballY > LANE_CY + PLAY_HALF_W) {
            s->ballY = LANE_CY + PLAY_HALF_W;
            s->ballVY = -s->ballVY * BOUNCE_DAMP;
        }

        // Friction: shrink the SPEED, never flip its sign (a ball does not
        // reverse by decelerating), and never touch direction, so a throw
        // that is basically straight stays basically straight.
        float speed = sqrtf(s->ballVX * s->ballVX + s->ballVY * s->ballVY);
        if (speed > 0.0f) {
            float newSpeed = speed - FRICTION_PX_PER_MS2 * dt;
            if (newSpeed < 0.0f) newSpeed = 0.0f;
            float k = newSpeed / speed;
            s->ballVX *= k; s->ballVY *= k;
            speed = newSpeed;
        }
        mark_ball(s);

        // Pin collisions: only standing pins the ball has just reached.
        for (int i = 0; i < PIN_COUNT; i++) {
            if (s->pins[i].down) continue;
            float dx = s->pins[i].baseX - s->ballX, dy = s->pins[i].baseY - s->ballY;
            float r = BALL_R + PIN_R_BASE + 3.0f;
            if (dx * dx + dy * dy <= r * r) {
                float vlen = sqrtf(s->ballVX * s->ballVX + s->ballVY * s->ballVY);
                float dirX = vlen > 0.001f ? s->ballVX / vlen : 1.0f;
                float dirY = vlen > 0.001f ? s->ballVY / vlen : 0.0f;
                topple_pin(s, i, now, dirX, dirY);
            }
        }

        // Keep re-marking any pin still mid-topple (its own fall, or a
        // chained neighbour whose stagger has not caught up to `now` yet) -
        // topple_pin() only marks once, at the instant a pin goes down, and
        // that is not enough to make the animation actually visible over
        // its own FALL_MS. See pin_falling_in_progress()'s own comment.
        for (int i = 0; i < PIN_COUNT; i++) {
            if (pin_falling_in_progress(s, i, now)) mark_pin(s, i);
        }

        bool reachedEnd = s->ballX > PIN_BACK_X + PIN_HEIGHT;
        bool stopped = speed < STOP_SPEED;
        bool timedOut = (now - s->phaseStartMs) > FLIGHT_SAFETY_MS;
        if (reachedEnd || stopped || timedOut) {
            // The reachedEnd check only fires AFTER this tick's own
            // integration has already moved the ball, so at MAX_LAUNCH_SPEED
            // (1.55px/ms) a single tick's dt can carry it several pixels
            // past PIN_BACK_X + PIN_HEIGHT before this line ever runs -
            // found by the gate's own `bezel` rule, whose "swipe diagonally"
            // stimulus happened to grab and fling the ball hard enough to
            // land it past SAFE_X1 altogether. Clamp the RESTING position
            // (what PH_PAUSE actually freezes and draws) rather than trying
            // to predict and pre-empt the overshoot; the couple of pixels
            // this costs the "ball travelled a little further" illusion are
            // invisible next to never drawing ink under the case.
            float maxRestX = (float)SAFE_X1 - BALL_R - 4.0f;
            if (s->ballX > maxRestX) {
                mark_ball(s);
                s->ballX = maxRestX;
                mark_ball(s);
            }
            resolve_flight(s, now);
        }
        break;
    }

    case PH_PAUSE: {
        uint32_t limit = s->wasStrike ? PAUSE_STRIKE_MS : PAUSE_MS;
        // A pin can still be finishing a chained fall when the flight
        // resolves (its stagger delay outlasting the ball's own end
        // condition), so this phase needs the same re-marking PH_FLYING
        // does, not just the strike ring.
        for (int i = 0; i < PIN_COUNT; i++) {
            if (pin_falling_in_progress(s, i, now)) mark_pin(s, i);
        }
        if (s->wasStrike && (now - s->lastRenderMs) >= RENDER_MIN_MS) {
            float cx = (PIN_APEX_X + PIN_BACK_X) / 2.0f;
            mark_span(s, cx - STRIKE_RING_R1, cx + STRIKE_RING_R1);
        }
        if ((now - s->phaseStartMs) >= limit) start_reset(s, now);
        break;
    }

    case PH_RESET: {
        bool anyRising = false;
        if ((now - s->lastRenderMs) >= RENDER_MIN_MS) {
            for (int i = 0; i < PIN_COUNT; i++) {
                if (!s->pins[i].down) continue;
                uint32_t windowEnd = s->phaseStartMs + (uint32_t)i * RESET_STAGGER_MS + (uint32_t)RESET_MS;
                if (now >= windowEnd) {
                    s->pins[i].down = false;
                    mark_pin(s, i);
                } else {
                    mark_pin(s, i);
                    anyRising = true;
                }
            }
        } else {
            for (int i = 0; i < PIN_COUNT; i++) if (s->pins[i].down) { anyRising = true; break; }
        }

        uint32_t ballStartAt = s->phaseStartMs + PIN_RESET_TOTAL_MS + BALL_RETURN_GAP_MS;
        if (!anyRising && now >= ballStartAt) {
            float u = (float)(now - ballStartAt) / BALL_RETURN_MS;
            if (u >= 1.0f) {
                go_ready(s, now);
                break;
            }
            if ((now - s->lastRenderMs) >= RENDER_MIN_MS) {
                mark_ball(s);
                float eu = smoothstep(u);
                s->ballX = s->ballFromX + (BALL_START_X - s->ballFromX) * eu;
                s->ballY = s->ballFromY + (BALL_START_Y - s->ballFromY) * eu;
                mark_ball(s);
            }
        }
        break;
    }
    }

    // Marking is deliberately UNCONDITIONAL, every tick, for anything whose
    // FOOTPRINT moves (the ball in PH_FLYING/PH_HELD): mark_span only ever
    // grows the pending region until a flush actually happens, so calling
    // it every tick is what makes the pending region correctly cover the
    // WHOLE path travelled since the last flush, not just wherever the
    // ball happened to be on the one tick a throttle allowed a look. Fixed-
    // footprint marks (a pin, the strike ring) are already throttled at
    // their own call sites above, since their span never depends on which
    // tick they were marked on. The flush itself is throttled UNIFORMLY
    // here, to ~60fps regardless of phase, which is what keeps the real
    // board's much-faster-than-60Hz main loop from pushing hundreds of
    // times a second for a ball in flight; a discrete event (grab, launch,
    // a pin toppling) waits at most one throttle interval (RENDER_MIN_MS,
    // under one frame) before its push goes out, which reads as immediate.
    if (s->dirtyAll || (s->dirty && (now - s->lastRenderMs) >= RENDER_MIN_MS)) {
        flush(s, now);
        s->lastRenderMs = now;
    }
}

// wantsShake is false: nothing here is destructive or reset-worthy by a
// jolt, and generalising shake past the sketchpad's own Etch-A-Sketch
// identity is exactly what decision 0002 section 5 forbids.
const app_t g_bowlingApp = {
    .name       = "bowling",
    .enter      = bowling_enter,
    .tick       = bowling_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
