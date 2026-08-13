// timer: a countdown, set by dragging a ring rather than typing a number.
// See docs/decisions/0002-runtime-architecture.md, "The timer, in detail",
// for the brief this implements; the reasoning below assumes that section
// has been read and only restates it where the code needs to justify a
// specific number.
//
// Setting: the egg-timer twist. A ring of discrete SQUARE ticks surrounds the
// landscape canvas; dragging a finger around it lights ticks up to the
// finger's angle, snapped to a round value. Squares, not rotated bars,
// because gfx rotates rectangles, not pixels (gfx.h): a square looks the
// same at any angle, so it needs no per-pixel rotation and cannot shred the
// way an approximated rotated rectangle could. Running: the same ring
// empties, one tick at a time. The alarm: a full-panel flash, self-limiting,
// dismissed by anything.
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
 * ------------------------------------------------------------------- */
#define FINE_TICKS      10
#define FINE_STEP_S     30
#define COARSE_STEP_S   60
#define TICK_COUNT      (FINE_TICKS + 55)   // 65

// Ring centre and radius, in LANDSCAPE coordinates (448 wide x 368 tall).
// Centred on the canvas; radius 150 leaves 34px clearance to the top/bottom
// edge (the tighter of the two, 368 tall) once the tick square's own half
// width is added, and keeps the ring well clear of the digits in the middle
// (see DIGIT_* below). Not measured on hardware, chosen to fit the geometry.
#define RING_CX      224   // LAND_W / 2
#define RING_CY      184   // LAND_H / 2
#define RING_RADIUS  150

// Tick square side and hollow-tick border thickness, in pixels. At radius
// 150, 65 ticks are 2*pi*150/65 =~ 14.5px apart centre-to-centre; a 10px
// square leaves ~4.5px of gap between neighbours, guessed to read as
// separate ticks rather than a solid ring from across a room, not measured.
#define TICK_SIZE    10
#define TICK_BORDER  2

#define TIMER_PI       3.14159265358979323846f
#define TIMER_HALF_PI  (TIMER_PI / 2.0f)
#define TICK_ANGLE_STEP (2.0f * TIMER_PI / (float)TICK_COUNT)

/* ---------------------------------------------------------------------
 * Digit layout, in LANDSCAPE coordinates. MM:SS, 4 digits + 1 colon,
 * comfortably inside the ring: half-diagonal of the digit block is about
 * 100px against a 150px ring radius, so there is no world in which a tick
 * square and a digit cell touch. DIGIT_H (90) and SEG_T (12) keep the same
 * proportion chrono uses (120:18, i.e. h/t =~ 6.7), just smaller, since this
 * app only ever shows 4 digits rather than 6 and has a ring to share the
 * canvas with.
 * ------------------------------------------------------------------- */
#define DIGIT_W   40
#define DIGIT_H   90
#define SEG_T     12
#define SEP_W     20
#define DIGIT_Y0  139   // RING_CY - DIGIT_H/2

#define X_MM_TENS   134   // RING_CX - (4*DIGIT_W + SEP_W)/2
#define X_MM_UNITS  174
#define X_COLON     214
#define X_SS_TENS   234
#define X_SS_UNITS  274

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
 *   1. The ring shape.
 *      SETTING draws all 65 tick positions: filled up to the chosen value,
 *      HOLLOW (outlined, not blank) beyond it. The hollow outline is the
 *      "whole dial, still being set" cue and is never drawn in any other
 *      state.
 *      RUNNING and PAUSED draw ONLY the remaining ticks, filled; the
 *      consumed ones are blank, no outline. The ring visibly "empties" and
 *      never looks like the always-fully-outlined SETTING dial, even in the
 *      one case where the fill counts would otherwise coincide (a freshly
 *      started RUNNING timer looks, at that instant, like a maxed-out
 *      SETTING ring except for the missing outline).
 *   2. The digits' colour, which does not depend on the ring at all and so
 *      covers the case above on its own:
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
 * Ring drawing. Tick positions are fixed (angle = index * 2*pi/TICK_COUNT,
 * starting at 12 o'clock, increasing clockwise), so tick_rect() is the same
 * function whether painting the initial dial or reacting to a drag.
 * ------------------------------------------------------------------- */
static void tick_rect(int idx, int *x0, int *y0) {
    float angle = -TIMER_HALF_PI + ((float)idx + 0.5f) * TICK_ANGLE_STEP;
    int cx = RING_CX + (int)lroundf(RING_RADIUS * cosf(angle));
    int cy = RING_CY + (int)lroundf(RING_RADIUS * sinf(angle));
    *x0 = cx - TICK_SIZE / 2;
    *y0 = cy - TICK_SIZE / 2;
}

// lit: draw filled (black), always, in every state.
// !lit && settingMode: draw hollow (outlined), the SETTING-only cue.
// !lit && !settingMode: draw nothing, cleared to background: RUNNING/PAUSED
// ticks that have been consumed simply vanish, which is "the ring empties".
static void draw_ring_tick(int idx, bool lit, bool settingMode) {
    int x0, y0;
    tick_rect(idx, &x0, &y0);
    if (lit) {
        gfx_fill_rect_land(x0, y0, TICK_SIZE, TICK_SIZE, PX_BLACK);
        return;
    }
    gfx_fill_rect_land(x0, y0, TICK_SIZE, TICK_SIZE, PX_WHITE);
    if (!settingMode) return;
    int b = TICK_BORDER;
    gfx_fill_rect_land(x0,                 y0,                 TICK_SIZE, b,          PX_BLACK); // top
    gfx_fill_rect_land(x0,                 y0 + TICK_SIZE - b, TICK_SIZE, b,          PX_BLACK); // bottom
    gfx_fill_rect_land(x0,                 y0,                 b,         TICK_SIZE,  PX_BLACK); // left
    gfx_fill_rect_land(x0 + TICK_SIZE - b, y0,                 b,         TICK_SIZE,  PX_BLACK); // right
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

// Repaints and pushes only the tick squares between the old and new lit
// count, individually: they are scattered around a circle, not contiguous,
// so one bounding-box push would cover most of the ring for a one-tick
// change. Each push here is a single TICK_SIZE square; gfx_push_land pads it
// to a legal window on its own.
static void update_ring_diff(timer_state_t *s, int newLit, bool settingMode) {
    int oldLit = s->lastLit;
    int lo = oldLit < newLit ? oldLit : newLit;
    int hi = oldLit < newLit ? newLit : oldLit;
    for (int i = lo; i < hi; i++) {
        draw_ring_tick(i, i < newLit, settingMode);
        int x0, y0;
        tick_rect(i, &x0, &y0);
        gfx_push_land(x0, y0, TICK_SIZE, TICK_SIZE);
    }
    s->lastLit = newLit;
}

// Full repaint of the ring (all TICK_COUNT positions) and the digits, for
// enter() and for every state transition. Does not push: callers that run
// after enter() (i.e. every transition) follow this with gfx_push_all(),
// since a transition changes the ring's whole style (outlined vs not) and
// the digits' colour together, which is cheaper as one push than as up to
// 65 tiny diffed ones.
static void redraw_full(timer_state_t *s) {
    int lit, seconds;
    current_lit_and_seconds(s, &lit, &seconds);
    bool settingMode = (s->state == TS_SETTING);
    for (int i = 0; i < TICK_COUNT; i++) {
        draw_ring_tick(i, i < lit, settingMode);
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
 * enter(): draws the initial SETTING screen (ring fully hollow, 00:00, light
 * grey) into the white framebuffer the runtime has just cleared. Does not
 * push: the runtime pushes the whole panel once after this returns.
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
        update_ring_diff(s, newLit, /*settingMode=*/true);
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
        if (newLit != s->lastLit) update_ring_diff(s, newLit, /*settingMode=*/false);
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
