// timer: a countdown, set by dragging a ring rather than typing a number.
// See docs/decisions/0002-runtime-architecture.md, "The timer, in detail",
// for the brief this implements; the reasoning below assumes that section
// has been read and only restates it where the code needs to justify a
// specific number.
//
// Setting: the egg-timer twist. A ring of FILLED CIRCLES surrounds the
// landscape canvas; dragging a finger around it lights ticks up to the
// finger's angle, snapped to a round value. Running: the same ring shows
// less remaining. The alarm: a full-panel flash, self-limiting, dismissed
// by anything.
//
// Owner feedback, verbatim, on the previous look (square ticks, hollow
// outline for "not yet chosen"): "i'd rather you didn't draw boxes and just
// a full circle, or maybe many small circles with full color so that it
// looks more full (rather than just the outline with no colour filled
// inside). also use the same font, same interlines and spacing as the
// chronometer". This file implements the "many small circles, always
// filled" reading rather than one full circle: see the comment on
// DOT_DIAM_LARGE/DOT_DIAM_SMALL below for why.
//
// A dot is still a RECTANGLE shape, not a pixel shape: gfx rotates
// rectangles, not pixels (gfx.h). A filled circle here is a stack of
// 1px-tall horizontal bars, each one a call to gfx_fill_rect_land, so it
// gets correct rotation for free exactly like the old squares did. What
// changed is that every bar's width now follows a circle's profile instead
// of being constant, not that this file draws outside gfx's rectangle
// contract.
//
// Digits are digits.c's, shared with chrono rather than redrawn here (see
// digits.h): same numerals, same two hard-won corrections, one copy.
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"

/* ---------------------------------------------------------------------
 * Tick geometry and the seconds-per-tick mapping.
 *
 * TICK_COUNT = 65: 10 ticks at 30s each covers 0..5 minutes (the brief's
 * "30 seconds per step below 5 minutes"), then 55 ticks at 60s each covers
 * 5..60 minutes ("a minute above"). 10*30 + 55*60 = 300 + 3300 = 3600s,
 * exactly one hour, which is the cap: long enough for anything a child asks
 * this puck for (a nap, a chore, "five more minutes") without needing more
 * than one lap of the ring, short enough that one accidental full drag
 * cannot set an absurd wait. Both the cap and the 5-minute knee are the
 * owner's words in the brief, not derived from anything; only the tick
 * count and the seconds arithmetic below follow from them.
 *
 * 65 ticks was checked against the new radius below (see RING_RADIUS) and
 * still reads as separate dots rather than a merged ring; if that ever
 * changes (a bigger radius, a smaller canvas), re-check the arc-spacing
 * arithmetic in the "dots: diameter and radius" comment below before
 * assuming 65 still fits.
 * ------------------------------------------------------------------- */
#define FINE_TICKS      10
#define FINE_STEP_S     30
#define COARSE_STEP_S   60
#define TICK_COUNT      (FINE_TICKS + 55)   // 65

// Ring centre, in LANDSCAPE coordinates (448 wide x 368 tall). Centred on
// the canvas, same as before.
#define RING_CX      224   // LAND_W / 2
#define RING_CY      184   // LAND_H / 2

/* ---------------------------------------------------------------------
 * The dots: diameter and radius.
 *
 * The distinction between "chosen"/"remaining" and "unchosen"/"elapsed" is
 * now SIZE, not fill: every one of the 65 positions is always a SOLID BLACK
 * circle, small or large, never hollow and never blank. That is the direct
 * fix for the owner's complaint ("just the outline with no colour filled
 * inside") and it also fixes an inconsistency the old ring had: SETTING
 * used to draw a hollow outline for the not-yet-chosen ticks so the dial
 * looked whole, while RUNNING/PAUSED erased consumed ticks to nothing so
 * the dial visibly emptied. Those were two different rules for the same
 * ring. Now there is one rule, in every state: unchosen/elapsed is a small
 * dot, chosen/remaining is a large dot, and the dial is always visibly
 * whole, which is exactly the "many small circles... so it looks more
 * full" the owner asked for.
 *
 * The owner also offered "just a full circle" as a simpler alternative.
 * This file keeps the many-dots version instead: a full circle can only
 * ever show ONE number (how far around it is filled), which is the same
 * information a shrinking arc already gave in the old design and the same
 * complaint stands against a solid pie wedge (it does not show the step
 * size). 65 individually countable dots are a ring made of the actual
 * units a drag snaps to; a child watching the ring fill or empty by ONE
 * DOT AT A TIME can see, without reading the digits, that time here moves
 * in fixed steps rather than continuously. That reading is worth the extra
 * geometry below.
 *
 * DOT_DIAM_LARGE / DOT_DIAM_SMALL and RING_RADIUS were solved together, not
 * picked independently, against three hard limits:
 *
 *   1. edge clearance: RING_RADIUS + DOT_RADIUS_LARGE must stay under 184
 *      (RING_CY, the distance to the top/bottom canvas edge, the tighter of
 *      the two: LAND_H is 368, LAND_W is 448).
 *   2. digit clearance: RING_RADIUS - DOT_RADIUS_LARGE must clear the MM:SS
 *      digit block's half-diagonal (see DIGIT_BLOCK_W below), with margin.
 *   3. arc spacing: at TICK_COUNT ticks, neighbouring LARGE dots (the worst
 *      case: a run of "remaining" ticks is contiguous) must not touch, or
 *      the ring reads as one solid band instead of countable dots.
 *
 * Digit block: 4 digits at DIGIT_W=48 plus one SEP_W=24 colon plus four
 * DIGIT_GAP=12 gaps = 264px wide, 120px tall (DIGIT_H). Half-extents
 * 132 x 60; half-diagonal = sqrt(132^2 + 60^2) = sqrt(21024) = ~145.0px.
 * That 145.0, not the digit box's straight half-height (60) or half-width
 * (132) alone, is the number that matters: the ring is round and the box
 * is not, so the closest a full circle of radius R can get to EVERY point
 * of the box, at any angle, without ever dipping inside it, is R > 145.0
 * (the box's farthest corner from the centre). Anything less only "mostly"
 * clears it, which is exactly the collision the brief warned about.
 *
 * RING_RADIUS = 165, DOT_DIAM_LARGE = 10 (radius 5), DOT_DIAM_SMALL = 6:
 *   edge:   165 + 5 = 170, vs the 184 limit -> 14px margin.
 *   digit:  165 - 5 = 160, vs the 145.0 half-diagonal -> 15px margin.
 *   arc:    circumference = 2*pi*165 = 1036.7px; /65 ticks = 15.95px
 *           centre-to-centre. Two adjacent LARGE dots (10px each) leave
 *           15.95 - 10 = 5.95px of white between them, about 37% of the
 *           spacing: a real gap, not a rounding accident (position rounding
 *           from lroundf is at most +-1px per dot, an order of magnitude
 *           smaller than the gap).
 * All three numbers are derived above, not guessed; RING_RADIUS is the one
 * free choice that was searched (150, the old radius, fails #3 for any
 * DOT_DIAM_LARGE worth calling "large"; 165 is the smallest radius tried
 * that clears all three with double-digit margins).
 * ------------------------------------------------------------------- */
#define RING_RADIUS       165
#define DOT_DIAM_LARGE    10
#define DOT_DIAM_SMALL    6
#define DOT_RADIUS_LARGE  (DOT_DIAM_LARGE / 2)
#define DOT_RADIUS_SMALL  (DOT_DIAM_SMALL / 2)

#define TIMER_PI       3.14159265358979323846f
#define TIMER_HALF_PI  (TIMER_PI / 2.0f)
#define TICK_ANGLE_STEP (2.0f * TIMER_PI / (float)TICK_COUNT)

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
 * accepted on purpose, not missed.
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
 *   1. The ring shape, which now reads the SAME WAY in every non-alarm
 *      state: small dot = unchosen (SETTING) or already elapsed
 *      (RUNNING/PAUSED); large dot = chosen (SETTING) or still remaining
 *      (RUNNING/PAUSED). There is no longer a hollow-vs-filled cue, and
 *      deliberately no SETTING-only ring rule either: the old code drew a
 *      fully-hollow-outlined dial in SETTING and an emptying, blank-past-
 *      the-mark dial in RUNNING/PAUSED, two different rules for what is
 *      conceptually the same "how much is set/left" reading. Collapsing
 *      them to one rule is what makes the ring "stay whole" the way the
 *      owner asked, and it means the ring ALONE no longer tells SETTING
 *      apart from RUNNING/PAUSED (a freshly started RUNNING timer and a
 *      maxed-out SETTING ring both show all 65 dots large).
 *   2. The digits' colour, which therefore now carries the FULL weight of
 *      telling SETTING, RUNNING and PAUSED apart, not just the tie-break it
 *      used to be:
 *        SETTING  light grey ("not committed yet")
 *        RUNNING  solid black ("live, counting down")
 *        PAUSED   darker grey ("frozen")
 *      ALARM does not draw digits at all; see below.
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

    int lastLit;           // ring ticks currently painted, whatever the state
    int lastDigitSeconds;  // seconds value currently painted in the MM:SS cells

    uint32_t alarmStartMs;
    bool alarmInverted;    // current flash phase; a push happens only on a flip
} timer_state_t;

// s_state is a pointer into the arena, not the state itself: see chrono.c's
// identical comment on the same pattern, and app.h's arena section for why a
// file-scope struct is the thing that is not acceptable, not a 4-byte
// pointer that has nowhere else to live between enter() and tick().
static timer_state_t *s_state;

/* ---------------------------------------------------------------------
 * Seconds <-> ticks, both directions. Piecewise: fine steps below 5
 * minutes, coarse above, per TICK_COUNT's comment above.
 * ------------------------------------------------------------------- */
static int seconds_for_ticks(int ticks) {
    if (ticks <= 0) return 0;
    if (ticks > TICK_COUNT) ticks = TICK_COUNT;
    if (ticks <= FINE_TICKS) return ticks * FINE_STEP_S;
    return FINE_TICKS * FINE_STEP_S + (ticks - FINE_TICKS) * COARSE_STEP_S;
}

// Ceiling division in both bands, so the ring only loses a tick once that
// tick's whole span of real time has actually elapsed, not the instant it
// starts. Used only to decide how many ticks to paint; the digits show
// remainingSeconds directly and are never quantised.
static int ticks_for_seconds(int seconds) {
    if (seconds <= 0) return 0;
    if (seconds <= FINE_TICKS * FINE_STEP_S) {
        return (seconds + FINE_STEP_S - 1) / FINE_STEP_S;
    }
    int rem = seconds - FINE_TICKS * FINE_STEP_S;
    return FINE_TICKS + (rem + COARSE_STEP_S - 1) / COARSE_STEP_S;
}

/* ---------------------------------------------------------------------
 * Dot rasterisation: a filled circle as a stack of horizontal bars.
 *
 * Each diameter's per-row half-width is computed ONCE, lazily, the first
 * time the ring is drawn, and cached here rather than calling sqrtf per dot
 * per frame: a full ring redraw (enter(), and every state transition) draws
 * all 65 positions, and this runs on a 150MHz part where a couple hundred
 * sqrtf calls a frame is a real cost, not a rounding error. There are only
 * two diameters, so the table is tiny (10 + 6 int8_t) and lives here at
 * file scope; it holds no per-app state and so is exempt from the arena
 * rule the same way chrono.c's DIGIT_X[] const array is.
 * ------------------------------------------------------------------- */
static int8_t s_halfWidthLarge[DOT_DIAM_LARGE];
static int8_t s_halfWidthSmall[DOT_DIAM_SMALL];
static bool s_dotTablesReady = false;

static void fill_half_width_table(int8_t *table, int diam) {
    float r = diam / 2.0f;
    for (int row = 0; row < diam; row++) {
        // Distance of this row's vertical centre from the circle's centre.
        float dy = ((float)row + 0.5f) - r;
        float underRoot = r * r - dy * dy;
        float hw = underRoot > 0.0f ? sqrtf(underRoot) : 0.0f;
        table[row] = (int8_t)lroundf(hw);
    }
}

static void ensure_dot_tables(void) {
    if (s_dotTablesReady) return;
    fill_half_width_table(s_halfWidthLarge, DOT_DIAM_LARGE);
    fill_half_width_table(s_halfWidthSmall, DOT_DIAM_SMALL);
    s_dotTablesReady = true;
}

// Draws one filled circle of the given diameter, centred at (cx, cy), as a
// stack of 1px-tall horizontal bars: every bar is a gfx_fill_rect_land
// call, so this obeys gfx's "rectangles only" contract exactly like the old
// square ticks did.
static void draw_filled_dot(int cx, int cy, int diam, const int8_t *halfWidth, uint16_t color) {
    int r = diam / 2;
    for (int row = 0; row < diam; row++) {
        int hw = halfWidth[row];
        if (hw <= 0) continue;
        int y = cy - r + row;
        gfx_fill_rect_land(cx - hw, y, 2 * hw, 1, color);
    }
}

/* ---------------------------------------------------------------------
 * Ring drawing. Tick positions are fixed (angle = index * 2*pi/TICK_COUNT,
 * starting at 12 o'clock, increasing clockwise), so tick_center() is the
 * same function whether painting the initial dial or reacting to a drag.
 * ------------------------------------------------------------------- */
static void tick_center(int idx, int *cx, int *cy) {
    float angle = -TIMER_HALF_PI + ((float)idx + 0.5f) * TICK_ANGLE_STEP;
    *cx = RING_CX + (int)lroundf(RING_RADIUS * cosf(angle));
    *cy = RING_CY + (int)lroundf(RING_RADIUS * sinf(angle));
}

// Always paints a solid black dot: large if `big`, small otherwise. Clears
// the full DOT_DIAM_LARGE bounding box first regardless of which size is
// about to be drawn, because that box is the largest either size ever
// occupies at this centre, so it is the only rect that is guaranteed to
// erase whatever was there before (a large dot shrinking to a small one, or
// vice versa).
static void draw_ring_tick(int idx, bool big) {
    int cx, cy;
    tick_center(idx, &cx, &cy);
    int bx0 = cx - DOT_RADIUS_LARGE;
    int by0 = cy - DOT_RADIUS_LARGE;
    gfx_fill_rect_land(bx0, by0, DOT_DIAM_LARGE, DOT_DIAM_LARGE, PX_WHITE);
    if (big) {
        draw_filled_dot(cx, cy, DOT_DIAM_LARGE, s_halfWidthLarge, PX_BLACK);
    } else {
        draw_filled_dot(cx, cy, DOT_DIAM_SMALL, s_halfWidthSmall, PX_BLACK);
    }
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

    int slot = (int)(norm / TICK_ANGLE_STEP);
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

// What the ring and the digits should currently show, as a function of
// state alone. ALARM never calls this: it does not draw a ring or digits at
// all while it is flashing.
static void current_lit_and_seconds(const timer_state_t *s, int *lit, int *seconds) {
    if (s->state == TS_SETTING) {
        *lit = s->setTicks;
        *seconds = seconds_for_ticks(s->setTicks);
    } else {
        *seconds = s->remainingSeconds;
        *lit = ticks_for_seconds(s->remainingSeconds);
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

// Repaints and pushes only the dots between the old and new lit count,
// individually: they are scattered around a circle, not contiguous, so one
// bounding-box push would cover most of the ring for a one-tick change.
// Each push here is a single DOT_DIAM_LARGE square, the biggest either dot
// size ever occupies; gfx_push_land pads it to a legal window on its own.
static void update_ring_diff(timer_state_t *s, int newLit) {
    int oldLit = s->lastLit;
    int lo = oldLit < newLit ? oldLit : newLit;
    int hi = oldLit < newLit ? newLit : oldLit;
    for (int i = lo; i < hi; i++) {
        draw_ring_tick(i, i < newLit);
        int cx, cy;
        tick_center(i, &cx, &cy);
        gfx_push_land(cx - DOT_RADIUS_LARGE, cy - DOT_RADIUS_LARGE, DOT_DIAM_LARGE, DOT_DIAM_LARGE);
    }
    s->lastLit = newLit;
}

// Full repaint of the ring (all TICK_COUNT positions) and the digits, for
// enter() and for every state transition. Does not push: callers that run
// after enter() (i.e. every transition) follow this with gfx_push_all(),
// since a transition changes the digits' colour, which is cheaper as one
// push than as up to 65 tiny diffed ones.
static void redraw_full(timer_state_t *s) {
    ensure_dot_tables();
    int lit, seconds;
    current_lit_and_seconds(s, &lit, &seconds);
    for (int i = 0; i < TICK_COUNT; i++) {
        draw_ring_tick(i, i < lit);
    }
    s->lastLit = lit;
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
        s->state = TS_SETTING;
        redraw_full(s);
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
 * enter(): draws the initial SETTING screen (ring all small dots, 00:00,
 * light grey) into the white framebuffer the runtime has just cleared. Does
 * not push: the runtime pushes the whole panel once after this returns.
 * ------------------------------------------------------------------- */
static void timer_enter(void) {
    s_state = APP_STATE(timer_state_t);
    // APP_STATE zeroes the allocation: state == TS_SETTING (0), setTicks ==
    // 0, everything else 0/false, which is exactly the "nothing set yet"
    // starting point.
    redraw_full(s_state);
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
        // since it is already showing the set value (setTicks == lastLit
        // there always, so there is nothing to redraw). "Again" is one press
        // away, per the brief, exactly like the old stopwatch's
        // BOOT-resets-to-zero except the target is setTicks, not zero.
        if (s->state != TS_SETTING) {
            s->state = TS_SETTING;
            redraw_full(s);
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
            redraw_full(s);
            gfx_push_all();
            printf("timer: start, %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        } else if (s->state == TS_RUNNING) {
            s->state = TS_PAUSED;
            redraw_full(s); // digit colour changes on pause; ring does not
            gfx_push_all();
            printf("timer: pause at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        } else { // TS_PAUSED
            s->state = TS_RUNNING;
            // Reset the decrement anchor rather than reusing the old one: a
            // long pause must not dump a backlog of missed seconds into the
            // countdown the instant it resumes.
            s->lastDecMs = f->nowMs;
            redraw_full(s);
            gfx_push_all();
            printf("timer: resume at %02d:%02d\r\n", s->remainingSeconds / 60, s->remainingSeconds % 60);
        }
        return;
    }

    if (s->state == TS_SETTING) {
        if (!f->touchDown) return;
        int newLit = ring_tick_for_touch(f->touchX, f->touchY);
        if (newLit == s->setTicks) return;
        s->setTicks = newLit;
        update_ring_diff(s, newLit);
        update_digits_if_changed(s, seconds_for_ticks(newLit));
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
        if (!changed) return;
        int newLit = ticks_for_seconds(s->remainingSeconds);
        if (newLit != s->lastLit) update_ring_diff(s, newLit);
        update_digits_if_changed(s, s->remainingSeconds);
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
