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
// THIS FILE NOW IMPLEMENTS THE OWNER'S OWN "OR MAYBE... A FULL CIRCLE"
// ALTERNATIVE INSTEAD, after review: a continuous annulus, track in light
// grey, filled arc in black, Apple-activity-ring style. The crumbling dot
// was a patch over quantisation that never fully went away: even
// interpolated, it was still one distinguished element standing in for a
// continuous quantity, redrawn in discrete steps. An arc has nothing to
// quantise; it moves because the angle IS the remaining time, computed
// fresh every frame. It also answers the "hard to see big dots vs small
// dots" complaint more directly than grey-as-primary-cue did: a thick black
// arc against a light grey track is the strongest contrast this white-
// paper-black-ink panel can produce, and it reads as ONE shape rather than
// as a comparison across 65 marks. What the dots gave that a bare circle
// would not (see the old rejection of "just a full circle": "a full circle
// can only ever show ONE number... does not show the step size") went, for
// one iteration, to 12 small tick marks at the 5-minute positions outside
// the ring; the owner then asked for those gone too, on the same reasoning
// Apple's own activity rings follow: the exact value is already written
// large in the middle of the ring, so a graduation nobody needs to count
// adds no information and only breaks the ring's contour. So the ring ends
// up as exactly two things: the grey track (see TRACK_GRAY) and the black
// arc on top of it (see "Rounded caps" below) - nothing else.
//
// A dot is still a RECTANGLE shape, not a pixel shape: gfx rotates
// rectangles, not pixels (gfx.h). The ring is drawn as a stack of 1px-tall
// horizontal bars, each one a call to gfx_fill_rect_land, same technique
// the old dots used and the same one firmware/apps/shapes.c now offers
// (shapes_fill_half_width_table): this file uses that table builder for its
// outer/inner half-width tables rather than re-deriving the sqrtf loop, but
// NOT shapes_draw_annulus_row, because that draws one row in ONE colour and
// almost every row here is split between the black arc and the grey track;
// the angular clipping that split needs (see "Angle, and where it gets
// fiddly" below) has no other caller yet, so it stays in this file rather
// than being pushed into shapes.c speculatively.
//
// Digits are digits.c's, shared with chrono rather than redrawn here (see
// digits.h): same numerals, same two hard-won corrections, one copy.
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"
#include "shapes.h"

/* ---------------------------------------------------------------------
 * Tick geometry and the seconds-per-tick mapping.
 *
 * TICK_COUNT = 65: 10 ticks at 30s each covers 0..5 minutes (the brief's
 * "30 seconds per step below 5 minutes"), then 55 ticks at 60s each covers
 * 5..60 minutes ("a minute above"). 10*30 + 55*60 = 300 + 3300 = 3600s,
 * exactly one hour, which is the cap. Both the cap and the 5-minute knee are
 * the owner's words in the brief, not derived from anything; only the tick
 * count and the seconds arithmetic below follow from them.
 *
 * This no longer sizes anything about the RING (that used to be 65 dots
 * spaced around it; the ring is now a continuous arc, sized only by
 * RING_OUTER_R/RING_INNER_R below). What TICK_COUNT still does is size the
 * SNAP: dragging still lands on one of 65 discrete values, per the brief's
 * "snap to sensible steps... so a small imprecise finger still lands
 * somewhere round" - the arc's angle is continuous, the VALUE it represents
 * is not.
 * ------------------------------------------------------------------- */
#define FINE_TICKS      10
#define FINE_STEP_S     30
#define COARSE_STEP_S   60
#define TICK_COUNT      (FINE_TICKS + 55)   // 65
#define TIMER_MAX_SECONDS (FINE_TICKS * FINE_STEP_S + (TICK_COUNT - FINE_TICKS) * COARSE_STEP_S) // 3600

// Ring centre, in LANDSCAPE coordinates (448 wide x 368 tall). Centred on
// the canvas, same as the dot version.
#define RING_CX      224   // LAND_W / 2
#define RING_CY      184   // LAND_H / 2

/* ---------------------------------------------------------------------
 * The ring: an annulus (track + arc), sized against the same three limits
 * the dot version was solved against, now for an outer and inner radius
 * instead of a dot-centre radius and a dot diameter:
 *
 *   1. edge clearance: RING_OUTER_R must stay under 184 (RING_CY, the
 *      distance to the top/bottom canvas edge, the tighter of the two:
 *      LAND_H is 368, LAND_W is 448).
 *   2. digit clearance: RING_INNER_R must clear the MM:SS digit block's
 *      half-diagonal, ~145.0px (see DIGIT_BLOCK_W below for that number's
 *      own derivation, unchanged from the dot version since the digits
 *      themselves did not move).
 *   3. no third "arc spacing" limit this time: that one existed only to
 *      keep 65 discrete dots from merging into a band. A continuous ring
 *      has no adjacent marks to merge, so it does not apply.
 *
 * RING_RADIUS = 165 is kept as the ring's midline, the same centre-line the
 * dot version used, so the ring sits in the same place on screen; the
 * thickness is new. RING_HALF_THICK = 8 (16px thick, "a thick black arc...
 * the strongest contrast this panel can produce" per the brief) was checked
 * against both limits:
 *   edge:   165 + 8 = 173, vs the 184 limit -> 11px margin.
 *   digit:  165 - 8 = 157, vs the 145.0 half-diagonal -> 12px margin.
 * Both margins are smaller than the dot version's (14px/15px) because this
 * ring is thicker than the old dot diameter (16px vs 10px), which is the
 * point: more ink, less spare room, still comfortably clear on both sides.
 * ------------------------------------------------------------------- */
#define RING_RADIUS      165
#define RING_HALF_THICK  8
#define RING_OUTER_R     (RING_RADIUS + RING_HALF_THICK) // 173
#define RING_INNER_R     (RING_RADIUS - RING_HALF_THICK) // 157
#define RING_ROWS        (2 * RING_OUTER_R)               // 346

// Track (the always-visible full ring under the arc) and tick marks: light
// grey, not black, so the black arc is the one thing the eye reads as "how
// much is left" and the track/marks read as calm background structure.
// gray_to_px (gfx.h) takes 0=black..255=white, so this grey's "ink" is
// (255-TRACK_GRAY)/255. 178 is about 30% ink: picked by building and
// looking at the emulator output (see this app's build/run instructions),
// carried over unchanged from the dot version's elapsed-dot grey, which was
// already checked against the owner's "clearly present but clearly
// secondary" ask.
#define TRACK_GRAY       178

// No tick marks/graduations outside the ring. An earlier version of this
// file had 12 small squares at the 5-minute positions (dots could not be
// drawn as literal radial lines - gfx only draws axis-aligned rectangles in
// landscape space - so they were small squares instead); the owner asked
// for them gone: the exact value is already written large in the middle of
// the ring, so a graduation adds no information it does not already give,
// and only breaks the ring's contour. Apple's activity rings, the reference
// for this design, have no ticks either, for the same reason. This also
// removes the last piece of the "counting the units" idea the original 65
// dots were built around (see this file's header comment): continuous
// motion plus an exact numeral now does that job instead of discrete marks.

#define TIMER_PI       3.14159265358979323846f
#define TIMER_HALF_PI  (TIMER_PI / 2.0f)
#define TIMER_RAD2DEG  (180.0f / TIMER_PI)
#define TIMER_DEG2RAD  (TIMER_PI / 180.0f)

/* ---------------------------------------------------------------------
 * Digit layout, in LANDSCAPE coordinates. Owner feedback: "use the same
 * font, same interlines and spacing as the chronometer". DIGIT_W, DIGIT_H,
 * SEG_T, SEP_W and the 12px gap below are chrono.c's own constants
 * (DIGIT_W/DIGIT_H/SEG_T/SEP_W and its X_* deltas, which are all +12),
 * copied rather than guessed at a smaller size the way the old 40/90/12
 * timer digits were. They are not shared via a header: chrono.c does not
 * expose them, and a shared layout header for two apps is a bigger call
 * than this task (see the owner's brief). If chrono.c's metrics ever
 * change, this block has to change with it by hand; that duplication is
 * accepted on purpose, not missed. Unchanged by the dots-to-ring rewrite:
 * the digits are pixel-identical to before.
 *
 * MM:SS is 4 digits, not chrono's 6, and one colon, not two, so the block
 * is narrower than chrono's; everything else (digit size, gap, colon
 * width) is identical, which is what makes it read as the same typeface.
 * ------------------------------------------------------------------- */
#define DIGIT_W   48   // chrono.c's DIGIT_W
#define DIGIT_H   120  // chrono.c's DIGIT_H
#define SEG_T     18   // chrono.c's SEG_T
#define SEP_W     24   // chrono.c's SEP_W (colon cell width)
#define DIGIT_GAP 12   // chrono.c's inter-element gap; every X_* delta in chrono.c is +12

// MM:SS block: 2 digits, colon, 2 digits, 4 gaps between the 5 elements.
#define DIGIT_BLOCK_W (4 * DIGIT_W + SEP_W + 4 * DIGIT_GAP)  // 264

// Centred on the ring, both axes. DIGIT_Y0 comes out to 124, the same
// number chrono.c uses for its own Y0, because both are centring the same
// DIGIT_H in the same 368px landscape height; not a coincidence, a check
// that the two derivations agree.
#define DIGIT_Y0  (RING_CY - DIGIT_H / 2)          // 124
#define DIGIT_X0  (RING_CX - DIGIT_BLOCK_W / 2)     // 92

#define X_MM_TENS   (DIGIT_X0)
#define X_MM_UNITS  (X_MM_TENS  + DIGIT_W + DIGIT_GAP)
#define X_COLON     (X_MM_UNITS + DIGIT_W + DIGIT_GAP)
#define X_SS_TENS   (X_COLON    + SEP_W   + DIGIT_GAP)
#define X_SS_UNITS  (X_SS_TENS  + DIGIT_W + DIGIT_GAP)

/* ---------------------------------------------------------------------
 * The alarm. No sound yet: the ES8311 codec is core1's, reserved but unused
 * (decision 0002, section 7). The one-line hook for it is marked below.
 *
 * ALARM_MAX_MS = 30000: "stop by itself after about 30 seconds" from the
 * brief, taken literally, not measured.
 * ALARM_FLASH_MS = 250: half of a 500ms full white-black-white cycle, i.e.
 * 2Hz, "noticeable without being frightening" per the brief. A guess at
 * what reads as calm rather than a fast, seizure-risk strobe; not tested
 * against the real panel or a real child. If it reads as too fast or too
 * slow on hardware, this is the constant to change.
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
 *   2. The RING'S SHAPE does not distinguish SETTING from RUNNING/PAUSED,
 *      on purpose, same as the dot version before it: a freshly-dragged
 *      SETTING arc and a freshly-started RUNNING arc at the same value look
 *      identical, and that is fine because the digit colour already answers
 *      "which state is this" unambiguously. What the ring DOES show, in
 *      every non-alarm state alike, is one continuous fact: how much of the
 *      3600s cap the current value represents, as the fraction of the
 *      circle the black arc covers. RUNNING and PAUSED differ from each
 *      other only in whether that arc is currently moving; a single
 *      snapshot of the ring cannot tell them apart either, which is again
 *      why the digit colour exists.
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

    int setTicks;          // 0..TICK_COUNT, chosen by dragging in SETTING
    int remainingSeconds;  // the real countdown, RUNNING/PAUSED only
    uint32_t lastDecMs;    // f->nowMs anchor for the once-per-second decrement

    float lastFillDeg;      // arc angle currently painted, 0..360, whatever the state
    int lastDigitSeconds;   // seconds value currently painted in the MM:SS cells

    // True (fractional) remaining seconds, captured the instant PAUSED is
    // entered. PAUSED must not keep deriving this from f->nowMs the way
    // RUNNING does: nowMs keeps advancing while paused but lastDecMs does
    // not, so that formula would run away the longer the pause lasts. This
    // is what lets PAUSED show the frozen arc angle rather than either
    // resetting it or corrupting it.
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
 * Seconds <-> ticks. Piecewise: fine steps below 5 minutes, coarse above,
 * per TICK_COUNT's comment above. Still used to SNAP a drag to a round
 * value; no longer used to size anything about how the ring is drawn.
 * ------------------------------------------------------------------- */
static int seconds_for_ticks(int ticks) {
    if (ticks <= 0) return 0;
    if (ticks > TICK_COUNT) ticks = TICK_COUNT;
    if (ticks <= FINE_TICKS) return ticks * FINE_STEP_S;
    return FINE_TICKS * FINE_STEP_S + (ticks - FINE_TICKS) * COARSE_STEP_S;
}

/* ---------------------------------------------------------------------
 * The ring: track + arc, drawn as a stack of horizontal bars.
 *
 * s_hwOuter/s_hwInner are built once, lazily, via shapes.h's table builder
 * (see this file's header comment on why shapes_draw_annulus_row itself is
 * not reused). Same convention as menu.c's chrono icon: row 0 is the ring's
 * topmost pixel row, row RING_ROWS-1 its bottommost, both tables share the
 * same RING_ROWS-tall grid so hwOuter[row] and hwInner[row] describe the
 * same physical row.
 * ------------------------------------------------------------------- */
static int16_t s_hwOuter[RING_ROWS];
static int16_t s_hwInner[RING_ROWS];
static bool s_ringTablesReady = false;

static void ensure_ring_tables(void) {
    if (s_ringTablesReady) return;
    shapes_fill_half_width_table(s_hwOuter, RING_ROWS, (float)RING_OUTER_R);
    shapes_fill_half_width_table(s_hwInner, RING_ROWS, (float)RING_INNER_R);
    s_ringTablesReady = true;
}

/* ---------------------------------------------------------------------
 * Angle, and where it gets fiddly.
 *
 * Convention, kept from the dot version: 0 degrees is 12 o'clock, increasing
 * CLOCKWISE (matching the screen, not math convention - see the derivation
 * this replaces for why: landscape y increases downward, which flips the
 * usual counterclockwise-positive sense). The arc always starts at 0 and
 * covers [0, fillDeg), so unlike an arbitrary start+sweep there is no
 * generic wraparound to handle: the only question per pixel is whether its
 * own clockwise-from-12 angle is less than fillDeg.
 *
 * phi_deg_at() computes that angle via atan2f(dx, -dy): a "compass bearing"
 * form (0 = north/up, clockwise positive) whose branch cut sits at dx=0,
 * dy>0 - straight down, 6 o'clock - rather than at the more common negative-
 * x-axis cut a plain atan2f(dy,dx)+90 would have (which lands at 9 o'clock,
 * inside the ring's usable area). Every caller below excludes the dx=0
 * column from its two generic bars and handles that one column directly
 * with a hardcoded angle (0 above centre, 180 below), so the branch cut is
 * never evaluated through this function at all - see paint_ring_row.
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
// paint_ring_row for why dx=0 is never in here) black up to fillDeg and grey
// track beyond it. phi(dx) is monotonic across any such bar (atan2f(dx,-dy)
// is monotonic in dx whenever dx never crosses 0, which is exactly the
// condition every caller guarantees), so there is at most one colour
// transition in the whole bar and a binary search finds it in a handful of
// atan2f calls instead of testing every pixel.
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

// Paints one full landscape row of the ring at fillDeg. Splits into up to
// two bars (left, right) plus the single dx=0 column on rows with no hole
// (the caps at the very top/bottom of the ring, where hwInner is 0): that
// column is handled with a direct, hardcoded angle (0 straight up, 180
// straight down) rather than through phi_deg_at(), because dx=0/dy>0 is
// exactly that function's branch cut (see the header comment above it).
static void paint_ring_row(int y, float fillDeg) {
    int rowIdx = y - (RING_CY - RING_OUTER_R);
    if (rowIdx < 0 || rowIdx >= RING_ROWS) return;
    int hwOuter = s_hwOuter[rowIdx];
    int hwInner = s_hwInner[rowIdx];
    if (hwOuter <= 0) return;
    float dyCenter = ((float)rowIdx + 0.5f) - (float)RING_OUTER_R; // == (y+0.5) - RING_CY

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

/* ---------------------------------------------------------------------
 * Rounded caps. Owner feedback, with an Apple Watch activity-ring
 * screenshot as the reference: the arc's ends must be ROUNDED, not the
 * sharp radial cuts paint_ring_row/paint_ring_bar produce on their own. A
 * square-cut end reads as a pie slice with a wedge removed; a rounded end
 * reads as a solid band laid on a track, which matters more in monochrome
 * than it would in colour, since the cap shape is one of the few cues
 * available at all.
 *
 * A rounded cap is a filled disc, radius RING_HALF_THICK (so its diameter
 * equals the ring's stroke thickness), centred on the CENTRELINE radius
 * (RING_RADIUS, midway between inner and outer) at the cap's angle - not on
 * the inner or outer edge, which would make it bulge off the track on one
 * side. Two caps: one fixed at 0 degrees (the arc always starts at 12
 * o'clock) and one at the current fillDeg (the moving tip). Built from
 * shapes.h's table builder plus shapes_draw_annulus_row with hwInner=0,
 * which is exactly a filled-disc row (see shapes_draw_annulus_row's own
 * comment: hwInner<=0 draws one full-width bar), rather than a third way to
 * rasterise a circle in this file.
 * ------------------------------------------------------------------- */
static int16_t s_capHw[2 * RING_HALF_THICK];
static bool s_capTableReady = false;

static void ensure_cap_table(void) {
    if (s_capTableReady) return;
    shapes_fill_half_width_table(s_capHw, 2 * RING_HALF_THICK, (float)RING_HALF_THICK);
    s_capTableReady = true;
}

static void cap_center(float deg, int *cx, int *cy) {
    float mathAngle = (deg - 90.0f) * TIMER_DEG2RAD;
    *cx = RING_CX + (int)lroundf((float)RING_RADIUS * cosf(mathAngle));
    *cy = RING_CY + (int)lroundf((float)RING_RADIUS * sinf(mathAngle));
}

static void draw_cap(float deg) {
    ensure_cap_table();
    int cx, cy;
    cap_center(deg, &cx, &cy);
    for (int row = 0; row < 2 * RING_HALF_THICK; row++) {
        int y = cy - RING_HALF_THICK + row;
        shapes_draw_annulus_row(cx, y, s_capHw[row], 0, PX_BLACK);
    }
}

// Both caps, or none: with no arc (fillDeg == 0, only reachable via a full
// SETTING redraw at the untouched default - see ring_tick_for_touch's
// comment on why a drag can never bring setTicks back to exactly 0) there
// is nothing to round the end of. As fillDeg shrinks toward 0 the two caps
// (fixed at 0 degrees, moving at fillDeg) draw closer together: past
// roughly 5.6 degrees apart (2*RING_HALF_THICK's worth of arc length at
// RING_RADIUS) they start to overlap into one rounded blob rather than two
// separate discs joined by a bar. That is correct and is the point: a
// nearly-finished timer still shows a visible rounded nub near 12 o'clock
// instead of thinning into an invisible sliver, which is what a sharp-cut
// arc would have done. Checked by looking at the emulator output as the
// countdown's last minute (60/3600*360 = 6 degrees, already inside the
// overlap zone) played out; see this app's build/run instructions for how
// to reproduce that capture.
static void draw_arc_caps(float fillDeg) {
    if (fillDeg <= 0.0f) return;
    draw_cap(0.0f);
    draw_cap(fillDeg);
}

// The track (whatever the arc is not currently covering) is the complement
// of the arc within one continuous annulus, painted by the same
// paint_ring_row calls that paint the arc: it is never "less than a full
// ring" on its own, so unlike the arc it has no free-standing end that
// needs a cap of its own - its two apparent "ends" are exactly where the
// arc's two caps already sit, already rounded by draw_arc_caps.
static void paint_ring_full(float fillDeg) {
    int yTop = RING_CY - RING_OUTER_R;
    int yBot = RING_CY + RING_OUTER_R - 1;
    for (int y = yTop; y <= yBot; y++) paint_ring_row(y, fillDeg);
    draw_arc_caps(fillDeg);
}

// Bounding landscape row range [*yLo, *yHi] that could contain any pixel
// whose angle lies in [fromDeg, toDeg] (fromDeg <= toDeg): the two edges of
// that angular wedge at both radii, PLUS any of the four axis angles
// (0/90/180/270, where the ring's dy is at its extreme for a given radius)
// that fall inside the wedge, since the wedge can bulge past a straight
// line between its two edges at those points. Used to bound the work an
// incremental update does to the rows that could actually have changed,
// rather than repainting all RING_ROWS every time (see update_ring_to).
static void ring_sweep_row_range(float fromDeg, float toDeg, int *yLo, int *yHi) {
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
        float dyOuter = (float)RING_OUTER_R * s;
        float dyInner = (float)RING_INNER_R * s;
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

// Moves the ring from its last-painted angle (s->lastFillDeg) to newFillDeg,
// touching only the rows the swept wedge could have changed and pushing
// just that bounding rect. Used for BOTH SETTING's drag (each snap can jump
// either direction, or a long way if the finger jumps to a new spot) and
// RUNNING's per-frame shrink (always a small step, since it is driven by
// real elapsed time): the row-range bound in ring_sweep_row_range handles
// both the same way, correctness does not depend on the jump being small,
// only the SIZE of the work does.
//
// Both caps are redrawn on every call, not only when the swept range
// happens to reach them: ring_sweep_row_range bounds where the ARC EDGE
// could have moved, but the fixed start cap (always at 0 degrees) never
// moves and so is never "inside" that swept range by the same logic, yet a
// sweep near a small fillDeg (the last minute or so of a countdown, see
// draw_arc_caps) sits right on top of it and would overwrite its rounded
// pixels with a sharp cut if the cap were not put back afterward. Redrawing
// both unconditionally costs two small discs (2*RING_HALF_THICK rows each)
// every call, which is cheap next to the row-range work already being done,
// and is simpler to get right than proving the sweep already covers them.
static void update_ring_to(timer_state_t *s, float newFillDeg) {
    if (newFillDeg < 0.0f) newFillDeg = 0.0f;
    if (newFillDeg > 360.0f) newFillDeg = 360.0f;
    float oldFillDeg = s->lastFillDeg;
    if (newFillDeg == oldFillDeg) return;

    float fromDeg = newFillDeg < oldFillDeg ? newFillDeg : oldFillDeg;
    float toDeg = newFillDeg < oldFillDeg ? oldFillDeg : newFillDeg;
    int yLo, yHi;
    ring_sweep_row_range(fromDeg, toDeg, &yLo, &yHi);
    for (int y = yLo; y <= yHi; y++) paint_ring_row(y, newFillDeg);
    draw_arc_caps(newFillDeg);

    // Grow the push rect to cover both caps too, in case a cap sits outside
    // the swept row range (typically the fixed start cap, while the moving
    // tip is elsewhere on the dial).
    if (newFillDeg > 0.0f) {
        int cx0, cy0, cx1, cy1;
        cap_center(0.0f, &cx0, &cy0);
        cap_center(newFillDeg, &cx1, &cy1);
        int capYLo = (cy0 < cy1 ? cy0 : cy1) - RING_HALF_THICK;
        int capYHi = (cy0 > cy1 ? cy0 : cy1) + RING_HALF_THICK - 1;
        if (capYLo < yLo) yLo = capYLo;
        if (capYHi > yHi) yHi = capYHi;
    }
    gfx_push_land(RING_CX - RING_OUTER_R, yLo, 2 * RING_OUTER_R, yHi - yLo + 1);
    s->lastFillDeg = newFillDeg;
}

// Touch angle to tick count. f->touchX/Y arrive in PANEL (portrait)
// coordinates (app.h), not landscape ones: gfx only rotates rectangles, not
// points (gfx.h's note on gfx_land_rect), so a landscape app reading a touch
// position has to invert that mapping itself. gfx_land_rect's corner math
// says landscape (lx, ly) -> panel (PANEL_W-1-ly, lx); inverted, panel
// (px, py) -> landscape (lx=py, ly=PANEL_W-1-px). Done once here rather than
// once per digit, since only the ring cares where the finger is.
static int ring_tick_for_touch(int touchPanelX, int touchPanelY) {
    int lx = touchPanelY;
    int ly = PANEL_W - 1 - touchPanelX;

    float dx = (float)(lx - RING_CX);
    float dy = (float)(ly - RING_CY);
    float norm = atan2f(dy, dx) + TIMER_HALF_PI; // 0 at 12 o'clock
    if (norm < 0.0f) norm += 2.0f * TIMER_PI;
    if (norm >= 2.0f * TIMER_PI) norm -= 2.0f * TIMER_PI;

    int slot = (int)(norm / (2.0f * TIMER_PI / (float)TICK_COUNT));
    if (slot < 0) slot = 0;
    if (slot >= TICK_COUNT) slot = TICK_COUNT - 1;
    // slot+1, not slot: a full revolution of TICK_COUNT equal slots has no
    // slot that means "zero", so a drag can set 1..TICK_COUNT ticks (30s to
    // 60min) but never back down to exactly zero. That is deliberate rather
    // than a gap in the mapping: true zero is only ever the untouched
    // default, which removes the ambiguity a dial with a true-zero position
    // would have between "zero" and "one full lap".
    return slot + 1;
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

// The ring's current fill angle, 0..360, as a function of state alone.
// Proportional to REMAINING SECONDS (not to tick index): the arc moves at a
// constant angular speed the whole way round this way, since seconds tick
// at a constant rate but ticks do not (30s below 5 minutes, 60s above).
// Driving the angle from tick index instead would make the arc visibly
// speed up or slow down at the 5-minute knee, which is exactly the kind of
// non-uniform motion a continuous arc exists to avoid.
static float current_fill_deg(const timer_state_t *s, uint32_t nowMs) {
    float remainSec;
    switch (s->state) {
        case TS_SETTING: remainSec = (float)seconds_for_ticks(s->setTicks); break;
        case TS_PAUSED:  remainSec = s->pausedTrueRemaining; break;
        case TS_RUNNING:
        default:         remainSec = running_true_remaining(s, nowMs); break;
    }
    float deg = (remainSec / (float)TIMER_MAX_SECONDS) * 360.0f;
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 360.0f) deg = 360.0f;
    return deg;
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

// Full repaint of the ring (track + arc, with rounded caps) and the digits,
// for enter() and for every state transition. Does not push: callers that
// run after enter() (i.e. every transition) follow this with
// gfx_push_all(), since a transition changes the digits' colour, which is
// cheaper as one push than as several smaller ones.
static void redraw_full(timer_state_t *s, uint32_t nowMs) {
    ensure_ring_tables();
    float fillDeg = current_fill_deg(s, nowMs);
    paint_ring_full(fillDeg);
    s->lastFillDeg = fillDeg;

    int seconds = digit_seconds_for(s);
    draw_all_digits(seconds, digit_color_for_state(s->state));
    s->lastDigitSeconds = seconds;
}

/* ---------------------------------------------------------------------
 * The alarm. Runs entirely inside this function; timer_tick() calls it
 * first, before anything else, and returns, so an input frame that was
 * actually "make it stop" is never also processed as a ring drag or a
 * button toggle.
 * ------------------------------------------------------------------- */
static void handle_alarm(timer_state_t *s, const app_frame_t *f) {
    // "Any input", read as literally every event app_frame_t can carry, per
    // the brief: a child reaching for a beeping object should not have to
    // remember which button, or that it has to be a button at all.
    bool anyInput = f->touchPressed || f->touchDown || f->touchReleased ||
                    f->bootClicked || (f->key != 0);
    bool timedOut = (f->nowMs - s->alarmStartMs) >= ALARM_MAX_MS;

    if (anyInput || timedOut) {
        // Back to SETTING at the value that was set, not to zero: the same
        // "again is one press" the brief asks of BOOT applies here too, the
        // ring should not have to be re-dragged just because it finished.
        //
        // Clear the whole panel first: the alarm's last flash frame may have
        // left it solid black (see the fill below, PX_BLACK on the inverted
        // phase), and redraw_full() only repaints the ring and the digit
        // cells, not every pixel in between. Skipping this leaves black
        // showing through the gaps between digit cells and everywhere
        // outside the ring - found by actually looking at a captured
        // dismiss frame, not by reasoning about the code. Pre-existing (the
        // alarm's flash-then-partial-redraw shape predates this file's
        // dots-to-ring rewrite); fixed here because it is squarely this
        // function's own bug. Same "whole rect needs no landscape rotation"
        // reasoning as the flash fill just below.
        gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
        s->state = TS_SETTING;
        redraw_full(s, f->nowMs);
        gfx_push_all();
        int sec = seconds_for_ticks(s->setTicks);
        printf("timer: alarm %s, back to %02d:%02d\r\n",
               anyInput ? "dismissed" : "timed out", sec / 60, sec % 60);
        return;
    }

    bool wantInverted = (((f->nowMs - s->alarmStartMs) / ALARM_FLASH_MS) % 2u) == 1u;
    if (wantInverted == s->alarmInverted) return;
    s->alarmInverted = wantInverted;

    // TODO(sound): one call here, e.g. sound_play(SOUND_ID_TIMER_ALARM),
    // once the ES8311 service from decision 0002 section 7 exists. Nothing
    // else in this function needs to change to add it.

    // A full-panel fill needs no landscape rotation: a rectangle covering
    // the whole screen is the same rectangle whichever way it is rotated, so
    // this is the only place in this file that draws in native panel space
    // rather than through gfx_*_land.
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, s->alarmInverted ? PX_BLACK : PX_WHITE);
    gfx_push_all();
}

/* ---------------------------------------------------------------------
 * enter(): draws the initial SETTING screen (empty ring, 00:00, light grey
 * digits) into the white framebuffer the runtime has just cleared. Does not
 * push: the runtime pushes the whole panel once after this returns.
 * ------------------------------------------------------------------- */
static void timer_enter(void) {
    s_state = APP_STATE(timer_state_t);
    // APP_STATE zeroes the allocation: state == TS_SETTING (0), setTicks ==
    // 0, everything else 0/false, which is exactly the "nothing set yet"
    // starting point.
    redraw_full(s_state, 0); // nowMs unused: SETTING ignores it (current_fill_deg's switch)
    printf("timer: entered, drag the ring to set a time\r\n");
}

/* ---------------------------------------------------------------------
 * tick(): one state transition or one incremental update per call, never
 * both, so every visible change traces to exactly one input.
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
            redraw_full(s, f->nowMs);
            gfx_push_all();
            int sec = seconds_for_ticks(s->setTicks);
            printf("timer: BOOT reset to %02d:%02d\r\n", sec / 60, sec % 60);
        }
        return;
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
            // Freeze the arc's exact fractional position BEFORE flipping
            // state: running_true_remaining() reads remainingSeconds and
            // lastDecMs, both still valid pre-flip, using f->nowMs one last
            // time while it still means "how far into this second are we".
            // Once state is TS_PAUSED, current_fill_deg() stops consulting
            // nowMs at all, so this is the only chance to capture it;
            // skipping this would either snap the arc back to a whole tick
            // on pause or let it drift while frozen.
            s->pausedTrueRemaining = running_true_remaining(s, f->nowMs);
            s->state = TS_PAUSED;
            redraw_full(s, f->nowMs); // arc freezes exactly where it was; digit colour changes
            gfx_push_all();
            printf("timer: pause at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        } else { // TS_PAUSED
            s->state = TS_RUNNING;
            // Reset the decrement anchor rather than reusing the old one: a
            // long pause must not dump a backlog of missed seconds into the
            // countdown the instant it resumes. This also means the arc's
            // sub-second position resets to 0 rather than resuming from
            // pausedTrueRemaining's exact fraction; the discrepancy is at
            // most one second out of a 3600s max, under 0.03% of the full
            // sweep, so it reads as continuous rather than as a jump.
            s->lastDecMs = f->nowMs;
            redraw_full(s, f->nowMs);
            gfx_push_all();
            printf("timer: resume at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        }
        return;
    }

    if (s->state == TS_SETTING) {
        if (!f->touchDown) return;
        int newTicks = ring_tick_for_touch(f->touchX, f->touchY);
        if (newTicks == s->setTicks) return;
        s->setTicks = newTicks;
        int newSeconds = seconds_for_ticks(newTicks);
        update_ring_to(s, (newSeconds / (float)TIMER_MAX_SECONDS) * 360.0f);
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
            printf("timer: ringing\r\n");
            return;
        }
        // Every RUNNING frame, not just on a whole-second `changed`: the arc
        // moves at sub-second granularity (that is the entire point of a
        // continuous arc over the old stepped dots), so gating it on
        // `changed` would reintroduce once-a-second stepping. update_ring_to
        // is itself a no-op when the angle has not moved enough to touch a
        // new pixel, so most calls here do nothing.
        update_ring_to(s, current_fill_deg(s, f->nowMs));
        if (changed) update_digits_if_changed(s, s->remainingSeconds);
        return;
    }

    // TS_PAUSED: nothing changes on its own; only BOOT and PWR, both handled
    // above, ever move it.
}

// wantsShake is false: shake is opt-in per app and belongs only where
// erasing is the app's identity (sensors.h, decision 0002 section 5). BOOT
// already resets this app to the set value; a second, accidental-shake path
// to the same reset would just be a second way to startle a child holding
// a countdown she is waiting on.
const app_t g_timerApp = {
    .name       = "timer",
    .enter      = timer_enter,
    .tick       = timer_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
