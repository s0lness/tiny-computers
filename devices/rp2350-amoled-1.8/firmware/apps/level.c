// level: a bubble level. Tip the puck and a dot slides downhill; hold it
// flat and the dot comes home to a ring that closes around it.
//
// It is the fifth app in g_apps[] (runtime_core.c) and the fourth-listed
// app in decision 0002's own table ("bubble level: exercises sensor
// signals at frame rate, input: none, it just responds"). Nothing here
// reads touch or a button: the only way to change what is on screen is to
// move the object.
//
// =========================================================================
// ORIENTATION: READ LIKE ANY OTHER SIGNAL, NOT OWNED HERE
// =========================================================================
//
// This app reads gravity through app_frame_t.tilt (app.h), exactly the way
// it reads touchDown or nowMs, and never touches the IMU: that is
// firmware/runtime/tilt.h's whole argument (docs/decisions/0012), and it
// applies to this app like any other. `f->tilt.valid` says whether there is
// a reading to draw from at all; `f->tilt.gx/gy/gz` is already filtered and
// already rotated into this app's own landscape drawing space (tilt.h
// again), so nothing here filters, maps device axes, or undoes a rotation.
//
// THIS WAS NOT ALWAYS TRUE. This app was originally written before the
// shared orientation signal existed, behind its own provisional seam and
// its own filter - see docs/decisions/0012's "What was built" table and
// commit c00db2f. That filter measured a real trade (see
// firmware/runtime/tilt.h's "FILTERING" section for the numbers) and has
// since moved upstream into tilt.c verbatim, because a filter chosen for
// one app is a filter every future orientation-aware app on this device
// would otherwise have to re-derive or diverge from. This file no longer
// carries any of that: this comment is the only trace of it left here.
//
// =========================================================================
// WHAT "LEVEL" MEANS HERE, AND WHY THAT ANGLE
// =========================================================================
//
// LEVEL_ENTER_DEG is 3.0 degrees from flat, in any direction.
//
// A builder's spirit vial resolves about 0.5 degrees, and it can, because
// it is lying on a beam and nobody is holding it. This is a puck in the
// hands of a two-year-old. The residual apparent tilt after tilt.h's own
// filtering is around half a degree of tremor, and her actual wrist control
// is a couple of degrees at best, so a half-degree band would essentially
// never latch and the toy would read as broken - not strict, broken. Three
// degrees is a shelf out by 5 cm over a metre, which is a genuinely level
// shelf, and it is reachable by a small hand in about a second.
//
// It leaves by 3.6 degrees, not 3.0 (LEVEL_EXIT_DEG), and it has to be held
// for LEVEL_DWELL_MS = 250 ms before it says yes. The hysteresis is what
// stops the verdict flickering while a hand hovers exactly on the boundary;
// the dwell is what stops a dot sweeping straight through the middle from
// flashing "yes" on its way past. Leaving is immediate, with no dwell, so
// the cue can be late but can never be a lie.
//
// =========================================================================
// SAYING IT WITHOUT WORDS
// =========================================================================
//
// She cannot read, so there is no text, no number and no degree sign
// anywhere in this file. The whole answer is one shape closing:
//
//   not level   a black dot somewhere on the dial, and a thin GREY ring
//               waiting for it in the middle - a target, plainly not
//               achieved, in the same secondary grey the timer's track uses.
//   level       that ring turns BLACK and twice as thick, and the dot
//               swells slightly. Two grey-and-scattered things become one
//               black-and-settled thing: a dot at rest inside a closed
//               ring.
//   meaningless the ring disappears entirely (see the third axis, below).
//
// The ring's inner edge is not a decorative radius: it is exactly where the
// dot's own edge sits at LEVEL_ENTER_DEG, so "inside the circle" and "level"
// are the same statement, the way the two lines on a real vial are. That is
// derived in LEVEL_TARGET_INNER rather than typed in.
//
// Everything on screen is a circle or an annulus, drawn from a signed
// distance and anti-aliased by coverage, so decision 0009 costs this app
// nothing: there is not a straight line or a corner anywhere in it, and no
// call to shapes_fill_between_curves_aa_land, which structurally could not
// have carried one.
//
// =========================================================================
// ONE AXIS OR TWO, AND WHAT THE THIRD ONE DOES
// =========================================================================
//
// TWO, at once, and the dot's freedom is the disc rather than a line. A
// tube level shows one axis because a tube is what a bubble fits in; this
// object is a round puck that will be laid flat on a table or a book, where
// tipping happens in both directions at once and a one-axis answer would be
// confidently wrong half the time. A circular level is also the honest
// picture of what the sensor measures: two in-plane components of one
// vector.
//
// THE DOT ROLLS DOWNHILL, like a marble in a dish, which is the OPPOSITE of
// a real spirit level, where the bubble floats up toward the raised side.
// That is a deliberate choice against tradition and it is one #define wide
// (LEVEL_BUBBLE_FLOATS_UP). A bubble's uphill float only reads as correct
// if you already know how a vial works; a child tipping a puck expects the
// thing inside to go the way she tipped it, and inverted control on a toy
// reads as the toy being wrong. Both conventions converge on the same
// gesture - hold it flat, thing comes to the middle - so nothing is lost
// except the physics lesson.
//
// THE THIRD AXIS. Which face is up comes from gz, and it decides whether
// "level" means anything at all:
//
//   face up, tilt 0-15 deg    the working range; the dot spans the dial.
//   tilt 15-55 deg            the dot is pinned against the rim, pointing
//                             downhill. Still true, still useful, just
//                             saturated - it says "way off, that way".
//   tilt >= 55 deg            held on edge or turned over. There is no
//                             "flat" to be near any more, so the target
//                             ring is REMOVED: nothing on screen claims a
//                             verdict that no longer has a meaning, and the
//                             dot parked at the rim is all that is left.
//                             It comes back the moment the puck does.
//
// Face DOWN is not a special case and does not need to be: the screen is
// against the table and nobody can see it, and on the way there the tilt
// passes 55 degrees and the ring has already gone.
//
// =========================================================================
// WHAT ONE FRAME COSTS, AND WHY THE DRAWING IS BUILT THIS WAY
// =========================================================================
//
// A moving object over a static background is the shape that leaves
// residue, and this project has paid for that four times. The usual way to
// get it wrong is to whiten a box the size of the old object and redraw the
// new one, which is correct only as long as nobody ever gets the box's
// extent wrong, and eventually somebody does.
//
// So nothing here erases anything. paint_rect() takes a rectangle and
// recomputes EVERY pixel in it from the model - rim, ring, dot, in that
// order, composited darkest-wins - so the old dot cannot survive a repaint
// of the ground it was standing on, by construction rather than by
// bookkeeping. The same function draws the whole dial once at enter() and a
// small window every frame after, which is why an incrementally-updated
// screen and a freshly-entered one are bit-identical (assert exactly that:
// emulator/wasm/tests/repro-level-bubble-residue.ts).
//
// The dial's rim never moves, so the per-frame window is the union of where
// the dot was and where it is (plus the ring's own box on the rare frame
// the verdict changes). The drawn position is quantised to half a pixel and
// a frame that would repaint the same picture is skipped entirely, so a
// perfectly still puck costs zero pixels and zero pushes.
//
// Budget, per frame, MEASURED over 332 frames of orbiting, slamming and
// flipping the verdict (repro-level-bubble-residue.ts prints these):
//
//   average frame that draws anything   5 417 px   3.3% of the panel
//   worst frame                        20 160 px  12.2% of the panel
//   still device                             0 px  no paint, no push
//
// The worst frame is not the dot moving fast - it is the ONE frame the
// verdict changes on, which repaints the target ring's own 140x144 box.
// Everything else is the dot's own two boxes, which merge into one near
// 62x62 whenever it is moving at hand speed. Painting and pushing are the
// same rectangle here (see align_for_push), so that pixel count is both
// costs at once: at an estimated 40-60 cycles per pixel (two squared-
// distance rejections, one or two sqrtf, the composite) the worst frame is
// roughly 8 ms of drawing on a 150 MHz M33, and the push of a 20k-pixel
// window is about 1.5 ms against the 12 ms a full-panel push costs. Under a
// 16 ms frame, with headroom, and the frame that spends it happens at most
// a few times a second because the verdict cannot flap faster than its own
// hysteresis and dwell allow.
//
// The emulator cannot see any of that time (decision 0003), so the standing
// test asserts the PIXEL count rather than a duration: that is the number a
// headless run can actually measure, and it is the number that got the
// sketchpad's palette animation into the watchdog.
//
// =========================================================================
// KNOWN, NOT SOLVED: BURN-IN
// =========================================================================
//
// A level left flat on a table is a static image on an AMOLED, which
// AGENTS.md flags as real. Every app here has that property when it is left
// alone (the menu, a stopped stopwatch, an untouched sketch), and the
// device-wide answer is decision 0002 section 10's still-open power
// management, not a per-app dodge. Not addressed here; recorded so the next
// person does not think it was missed.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"

// M_PI is not guaranteed under -std=c11, same reason menu.c and timer.c
// each carry their own.
#define LEVEL_PI 3.14159265358979323846f

/* ---------------------------------------------------------------------
 * Geometry, LANDSCAPE coordinates (448 wide x 368 tall). Held sideways
 * with the buttons along the top edge, like the stopwatch and the timer:
 * this app draws per-pixel coverage rather than rectangles, so it maps
 * landscape to panel itself (see paint_rect), exactly the way shapes.c's
 * aa_composite_land does and for the same reason.
 *
 * The dial is centred and as large as the case allows. Its outer edge sits
 * at LEVEL_RIM_R + LEVEL_RIM_HALF = 171.5 from the centre, against a
 * landscape half-height of 184, so 12.5 px of clearance - past
 * PANEL_BEZEL_MARGIN_PX (10, gfx.h), which is the band the plastic hides.
 * Nothing that has to be seen goes any further out than that.
 * ------------------------------------------------------------------- */
#define LEVEL_CX 224.0f
#define LEVEL_CY 184.0f

#define LEVEL_RIM_R     168.0f  // the rim ring's mid-radius
#define LEVEL_RIM_HALF    3.5f  // half its thickness
#define LEVEL_RIM_GAP     4.0f  // clear air between the dot and the rim at full deflection

#define LEVEL_BUBBLE_R       30.0f // the dot. 60 px across, against a child
                                    // fingertip measured at ~75 px (AGENTS.md):
                                    // a substantial object, not a speck.
#define LEVEL_BUBBLE_R_LEVEL 32.0f // it settles slightly bigger when it is home

// How far the dot's centre can travel: right up to the rim, minus its own
// radius and a gap so it never collides with the rim's ink.
#define LEVEL_TRAVEL_MAX (LEVEL_RIM_R - LEVEL_RIM_HALF - LEVEL_BUBBLE_R - LEVEL_RIM_GAP)

// Tilt at which the dot reaches the rim. 15 degrees is a deliberate,
// visible tip that a child can make and undo; much more and the dial feels
// dead in the middle, much less and it slams to the rail constantly.
#define LEVEL_FULL_SCALE_DEG 15.0f

// The verdict. See this file's "WHAT LEVEL MEANS HERE" section.
#define LEVEL_ENTER_DEG   3.0f
#define LEVEL_EXIT_DEG    3.6f
#define LEVEL_DWELL_MS     250u
#define LEVEL_LOST_DEG   55.0f

// The target ring's INNER edge is where the dot's own edge sits at exactly
// LEVEL_ENTER_DEG, so "the dot is inside the circle" and "the device is
// level" are the same sentence. Derived, never typed.
#define LEVEL_TRAVEL_AT_BAND (LEVEL_TRAVEL_MAX * (LEVEL_ENTER_DEG / LEVEL_FULL_SCALE_DEG))
#define LEVEL_TARGET_INNER   (LEVEL_TRAVEL_AT_BAND + LEVEL_BUBBLE_R)
// Thickness only ever grows OUTWARD from that inner edge, so the edge that
// carries the meaning never moves when the verdict changes. 4 px of grey
// hairline against 10 px of black is a difference nobody has to be told
// about; the rim itself is 7, so the closed ring is deliberately the
// heaviest thing on the dial.
#define LEVEL_TARGET_T_IDLE   4.0f  // thickness when it is only a target
#define LEVEL_TARGET_T_LEVEL 10.0f  // ...and when it has closed

// The secondary grey, lifted from timer.c's TRACK_GRAY rather than picked
// again: gray_to_px (gfx.h) takes 0=black..255=white, so 178 is about 30
// percent ink. It was chosen there by building it and looking, and this app
// wants the identical "present but clearly secondary" weight.
#define LEVEL_TRACK_GRAY 178

// Which way the dot goes. 0 = downhill, like a marble in a dish (this
// device's choice, see the header). 1 = uphill, like a real spirit level's
// bubble. One edit, nothing else in the file cares.
#define LEVEL_BUBBLE_FLOATS_UP 0

// Redraw granularity. The drawn centre is snapped to this, so a frame that
// would repaint the same picture is skipped and two runs that converge on
// the same tilt converge on the same pixels.
#define LEVEL_POS_QUANT 0.5f

/* ---------------------------------------------------------------------
 * State: one struct from the arena (app.h), never file-scope.
 * ------------------------------------------------------------------- */
typedef enum {
    RING_HIDDEN = 0, // no verdict is meaningful (held on edge, or no signal yet)
    RING_IDLE,       // a grey target, waiting
    RING_LEVEL,      // closed, black
} ring_state_t;

typedef struct {
    // The verdict and its dwell.
    bool     level;
    bool     inBand;
    uint32_t inBandSinceMs;

    // What is actually ON SCREEN right now. paint_rect() renders exactly
    // this, which is what makes an incremental frame identical to a
    // from-scratch one.
    bool         drawnValid;
    float        drawnX, drawnY, drawnR;
    ring_state_t drawnRing;
} level_state_t;

static level_state_t *s_state;

/* ---------------------------------------------------------------------
 * Drawing. One function paints; nothing erases.
 * ------------------------------------------------------------------- */

typedef struct { int x0, y0, x1, y1; } rect_t; // inclusive, landscape

// Coverage (0..1) of one shape, over white, as this device's 8-bit ink
// level. Same convention as sketch.c's draw_capsule and shapes.c's
// aa_composite_land: a signed distance turned into coverage by 0.5 - d,
// composited by MIN (darkest wins) rather than blended, so overlapping
// shapes union instead of compounding into a hard edge.
static inline uint8_t shade(float coverage, float targetGray) {
    if (coverage <= 0.0f) return 255;
    if (coverage > 1.0f) coverage = 1.0f;
    return (uint8_t)(targetGray * coverage + 255.0f * (1.0f - coverage) + 0.5f);
}

static inline float ring_half(ring_state_t r) {
    return (r == RING_LEVEL) ? (LEVEL_TARGET_T_LEVEL * 0.5f) : (LEVEL_TARGET_T_IDLE * 0.5f);
}

static inline float ring_mid(ring_state_t r) {
    return LEVEL_TARGET_INNER + ring_half(r);
}

// Repaints every pixel of an inclusive landscape rectangle from the model
// in `s->drawn*`. This is the whole anti-residue argument: a pixel inside
// the rectangle is recomputed, never patched, so nothing can survive there
// that the model does not put there.
//
// The landscape -> panel mapping is gfx.h's own documented forward rule
// (lx, ly) -> (PANEL_W-1-ly, lx), applied per pixel because a coverage
// value has no rectangle to hand gfx - the same deliberate exception
// shapes.c's aa_composite_land makes, for the same reason. Iterating ly on
// the inner loop walks panel memory backwards one pixel at a time, which is
// contiguous.
static void paint_rect(const level_state_t *s, rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > LAND_W - 1) r.x1 = LAND_W - 1;
    if (r.y1 > LAND_H - 1) r.y1 = LAND_H - 1;
    if (r.x0 > r.x1 || r.y0 > r.y1) return;

    const float rimOut = LEVEL_RIM_R + LEVEL_RIM_HALF + 1.0f;
    const float rimIn  = LEVEL_RIM_R - LEVEL_RIM_HALF - 1.0f;
    const float rimOut2 = rimOut * rimOut, rimIn2 = rimIn * rimIn;

    const bool  hasRing = (s->drawnRing != RING_HIDDEN);
    const float ringMid = ring_mid(s->drawnRing);
    const float ringHalf = ring_half(s->drawnRing);
    const float ringGray = (s->drawnRing == RING_LEVEL) ? 0.0f : (float)LEVEL_TRACK_GRAY;
    const float ringOut = ringMid + ringHalf + 1.0f, ringInn = ringMid - ringHalf - 1.0f;
    const float ringOut2 = ringOut * ringOut, ringInn2 = ringInn * ringInn;

    const float bR = s->drawnR;
    const float bOut = bR + 1.0f, bOut2 = bOut * bOut;

    for (int lx = r.x0; lx <= r.x1; lx++) {
        float fx = (float)lx + 0.5f;
        float dxc = fx - LEVEL_CX;
        float dxb = fx - s->drawnX;
        int idx = lx * PANEL_W + (PANEL_W - 1 - r.y0);
        for (int ly = r.y0; ly <= r.y1; ly++, idx--) {
            float fy = (float)ly + 0.5f;
            float dyc = fy - LEVEL_CY;
            float rc2 = dxc * dxc + dyc * dyc;

            uint8_t ink = 255;

            // The rim, and the target ring: both are annuli about the same
            // centre, so one squared distance rejects most pixels for both
            // without ever calling sqrtf.
            if (rc2 <= rimOut2 && rc2 >= rimIn2) {
                float d = fabsf(sqrtf(rc2) - LEVEL_RIM_R) - LEVEL_RIM_HALF;
                uint8_t v = shade(0.5f - d, 0.0f);
                if (v < ink) ink = v;
            }
            if (hasRing && rc2 <= ringOut2 && rc2 >= ringInn2) {
                float d = fabsf(sqrtf(rc2) - ringMid) - ringHalf;
                uint8_t v = shade(0.5f - d, ringGray);
                if (v < ink) ink = v;
            }

            // The dot.
            float dyb = fy - s->drawnY;
            float rb2 = dxb * dxb + dyb * dyb;
            if (rb2 <= bOut2) {
                float d = sqrtf(rb2) - bR;
                uint8_t v = shade(0.5f - d, 0.0f);
                if (v < ink) ink = v;
            }

            gfx_fb[idx] = gray_to_px(ink);
        }
    }
}

// Pre-rounds a rectangle to exactly what gfx_push would have rounded it to
// anyway, so that the window PAINTED and the window PUSHED are the same
// rectangle rather than the second being a superset of the first.
//
// gfx_push (decision 0001) rounds a panel window's ROW LENGTH up to a
// multiple of 8 and its start coordinates down to even. A landscape
// rectangle's HEIGHT becomes that row length, because gfx_land_rect swaps
// width and height, and its landscape y start decides whether the panel x
// start is even. So: landscape height a multiple of 8, landscape y even,
// landscape x even and landscape width even, and gfx_push then has nothing
// left to adjust.
//
// Worth doing rather than leaving to the push for two reasons. It stops
// every frame quietly pushing 8 more pixels of row than it drew, and it
// makes "no pixel changed outside a pushed rectangle" hold exactly instead
// of holding because the push happened to be generous - a margin that
// exists by accident is a margin nobody notices losing.
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
            back &= ~1; // keep the start even; the growth above is a
                         // multiple of 8, so this stays a whole 8 either way
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

static void paint_and_push(const level_state_t *s, rect_t r) {
    r = align_for_push(r);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;
    paint_rect(s, r);
    gfx_push_land(r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
}

static rect_t bubble_rect(float cx, float cy, float rad) {
    rect_t r;
    r.x0 = (int)floorf(cx - rad - 1.5f);
    r.y0 = (int)floorf(cy - rad - 1.5f);
    r.x1 = (int)ceilf(cx + rad + 1.5f);
    r.y1 = (int)ceilf(cy + rad + 1.5f);
    return r;
}

static rect_t ring_rect(void) {
    float outer = LEVEL_TARGET_INNER + LEVEL_TARGET_T_LEVEL + 2.0f;
    rect_t r;
    r.x0 = (int)floorf(LEVEL_CX - outer);
    r.y0 = (int)floorf(LEVEL_CY - outer);
    r.x1 = (int)ceilf(LEVEL_CX + outer);
    r.y1 = (int)ceilf(LEVEL_CY + outer);
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

/* ---------------------------------------------------------------------
 * The frame.
 * ------------------------------------------------------------------- */

// Where the dot belongs, in landscape coordinates, for a given gravity
// vector ALREADY IN THIS APP'S OWN LANDSCAPE DRAWING SPACE (app.h's
// app_tilt_t: +x right, +y down the screen as drawn, +z into the glass -
// the same space this file draws in, and the same rotation gfx_land_rect()
// already applies to this app's rectangles). runtime_core.c's
// tilt_for_app() has already done the panel-to-landscape rotation by the
// time this app ever sees a vector, so there is no rotation left to do
// here. Returns the tilt from flat in degrees through *outTiltDeg.
static void bubble_position(float gx, float gy, float gz,
                            float *outX, float *outY, float *outTiltDeg) {
    float inPlane = sqrtf(gx * gx + gy * gy);
    // atan2(in-plane, z): 0 lying flat face up, 90 on edge, 180 face down.
    float tiltDeg = atan2f(inPlane, gz) * (180.0f / LEVEL_PI);
    *outTiltDeg = tiltDeg;

    if (inPlane < 0.001f) {
        *outX = LEVEL_CX;
        *outY = LEVEL_CY;
        return;
    }

    // gx,gy point at the RAISED side (an accelerometer at rest reads +1g
    // along whichever axis points at the sky), so downhill is the negation.
    float ux = gx / inPlane;
    float uy = gy / inPlane;
#if !LEVEL_BUBBLE_FLOATS_UP
    ux = -ux; uy = -uy;
#endif

    float frac = tiltDeg / LEVEL_FULL_SCALE_DEG;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    float travel = LEVEL_TRAVEL_MAX * frac;

    *outX = LEVEL_CX + ux * travel;
    *outY = LEVEL_CY + uy * travel;
}

static float quantise(float v) {
    return floorf(v / LEVEL_POS_QUANT + 0.5f) * LEVEL_POS_QUANT;
}

static void level_enter(void) {
    s_state = APP_STATE(level_state_t);
    level_state_t *s = s_state;

    // enter() takes no frame (app.h), so this cannot read f->tilt - same as
    // every other app here, none of which peeks at a sensor before its
    // first tick(). The dial is drawn flat and centred, with no verdict:
    // level_tick() corrects it, in this app's usual incremental way, on the
    // very next frame. "No verdict on the first frame, ever" was already
    // this app's rule before there was a real signal to have an opinion
    // about; it still is.
    s->level = false;
    s->inBand = false;
    s->drawnX = LEVEL_CX;
    s->drawnY = LEVEL_CY;
    s->drawnR = LEVEL_BUBBLE_R;
    s->drawnRing = RING_HIDDEN;
    s->drawnValid = true;

    // The whole dial, once. enter() does not push - the runtime pushes the
    // panel after it returns (app.h).
    rect_t all;
    all.x0 = (int)floorf(LEVEL_CX - LEVEL_RIM_R - LEVEL_RIM_HALF - 2.0f);
    all.y0 = (int)floorf(LEVEL_CY - LEVEL_RIM_R - LEVEL_RIM_HALF - 2.0f);
    all.x1 = (int)ceilf(LEVEL_CX + LEVEL_RIM_R + LEVEL_RIM_HALF + 2.0f);
    all.y1 = (int)ceilf(LEVEL_CY + LEVEL_RIM_R + LEVEL_RIM_HALF + 2.0f);
    paint_rect(s, all);
}

static void level_tick(const app_frame_t *f) {
    level_state_t *s = s_state;

    bool have = f->tilt.valid;
    float bx = LEVEL_CX, by = LEVEL_CY, tiltDeg = 0.0f;
    if (have) bubble_position(f->tilt.gx, f->tilt.gy, f->tilt.gz, &bx, &by, &tiltDeg);

    // The verdict: hysteresis on the angle, a dwell before it says yes,
    // and no dwell at all before it takes it back.
    bool inBand = have && (s->level ? (tiltDeg <= LEVEL_EXIT_DEG)
                                    : (tiltDeg <= LEVEL_ENTER_DEG));
    if (inBand) {
        if (!s->inBand) {
            s->inBand = true;
            s->inBandSinceMs = f->nowMs;
        }
        if (f->nowMs - s->inBandSinceMs >= LEVEL_DWELL_MS) s->level = true;
    } else {
        s->inBand = false;
        s->level = false;
    }

    ring_state_t ring;
    if (!have || tiltDeg >= LEVEL_LOST_DEG) ring = RING_HIDDEN;
    else if (s->level) ring = RING_LEVEL;
    else ring = RING_IDLE;

    float nx = quantise(bx), ny = quantise(by);
    float nr = s->level ? LEVEL_BUBBLE_R_LEVEL : LEVEL_BUBBLE_R;

    if (s->drawnValid && nx == s->drawnX && ny == s->drawnY &&
        nr == s->drawnR && ring == s->drawnRing) {
        return; // the same picture: no paint, no push, no cost at all
    }

    rect_t oldR = bubble_rect(s->drawnX, s->drawnY, s->drawnR);
    bool ringChanged = (ring != s->drawnRing);

    // Commit the model FIRST: paint_rect renders s->drawn*, so everything
    // repainted below is the new picture, including the ground the old dot
    // was standing on.
    s->drawnX = nx;
    s->drawnY = ny;
    s->drawnR = nr;
    s->drawnRing = ring;

    rect_t newR = bubble_rect(nx, ny, nr);

    rect_t rects[3];
    int n = 0;
    rects[n++] = oldR;
    rects[n++] = newR;
    if (ringChanged) rects[n++] = ring_rect();

    // Merge any pair whose union costs no more pixels than the two of them
    // separately - one push always beats two at equal area. Three
    // rectangles at most, so a fixed pair of passes is enough.
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

const app_t g_levelApp = {
    .name = "level",
    .enter = level_enter,
    .tick = level_tick,
    .leave = NULL,
    .landscape = true,
    // No shake. This app has no destructive action to undo and nothing to
    // reset: shake is opt-in precisely so it stays the Etch A Sketch
    // gesture rather than a universal verb (decision 0002 section 5).
    .wantsShake = false,
};
