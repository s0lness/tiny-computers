// chrono: a stopwatch, now an app inside the single-binary runtime rather
// than its own flash slot. Ported from apps/chrono/main.c, which was proven
// on real hardware; the digit renderer, the layout constants and the
// elapsed-time bookkeeping below are that file's, carried over rather than
// re-derived. What is gone is everything the runtime now owns outright:
// panel init, the framebuffer, the push, the button chips and the menu
// gesture. See this app's git history (apps/chrono/main.c) for the fuller
// standalone version this was cut from.
//
// Display: "MM:SS:CC", minutes, seconds, centiseconds, colon throughout
// (always ":", never a locale-flavoured comma; the owner asked for this
// specifically, see digits.h).
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"

/* ---------------------------------------------------------------------
 * Layout, in LANDSCAPE coordinates (448 wide x 368 tall). 6 digits + 2
 * colons, sized to comfortably fill the 448px landscape width with an even
 * margin on both sides, vertically centred in the 368px landscape height.
 *
 * DIGIT_H (120) is the digit's landscape *height* (its long axis, since
 * digits stand upright), and it is the dimension that becomes the panel
 * push's row length after rotation (gfx_land_rect swaps w and h). It is
 * kept a multiple of 8 for exactly the reason gfx_push rounds row length up
 * to a multiple of 8: so that rounding never has anything to do, and every
 * digit push is exactly its drawn size with no padding.
 *
 *   layout, left to right (element widths in brackets, all landscape x):
 *   [48][48] [24:] [48][48] [24:] [48][48]
 *    MM tens/units   SS tens/units   CC tens/units
 *   margin 14px each side, 12px gaps between every element (all landscape x).
 * ------------------------------------------------------------------- */
#define DIGIT_W 48
#define DIGIT_H 120
#define SEG_T   18
#define SEP_W   24
#define Y0      124   // (368 - DIGIT_H) / 2, landscape y

#define X_MM_TENS  14
#define X_MM_UNITS 74
#define X_COLON1   134
#define X_SS_TENS  170
#define X_SS_UNITS 230
#define X_COLON2   290
#define X_CS_TENS  326
#define X_CS_UNITS 386

static const int DIGIT_X[6] = { X_MM_TENS, X_MM_UNITS, X_SS_TENS, X_SS_UNITS, X_CS_TENS, X_CS_UNITS };

/* ---------------------------------------------------------------------
 * State. One struct, allocated from the arena in enter() (see app.h's
 * arena section: file-scope statics are not acceptable once every app
 * shares one image). s_state is a pointer to that allocation, not the
 * state itself: it has to live somewhere for tick() to find it again, and a
 * few bytes for a pointer is not the budget the arena rule is guarding
 * against.
 * ------------------------------------------------------------------- */
typedef struct {
    bool     running;
    // Elapsed time in milliseconds rather than microseconds: app_frame_t
    // only ever hands out f->nowMs, and a centisecond-resolution display has
    // no use for finer precision than that, so the arithmetic below is the
    // original us-based version with every unit relabelled ms and the
    // us-per-centisecond divisor (10000) replaced with the ms-per-centisecond
    // one (10).
    uint32_t elapsedMs;   // accumulated across all completed run segments
    uint32_t runStartMs;  // f->nowMs at the start of the current segment
    int      lastDigit[6];
} chrono_state_t;

static chrono_state_t *s_state;

/* ---------------------------------------------------------------------
 * enter(): draws the full face, zeroed, into the white framebuffer the
 * runtime has just cleared. Does not push: the runtime pushes the whole
 * panel once after this returns.
 * ------------------------------------------------------------------- */
static void chrono_enter(void) {
    s_state = APP_STATE(chrono_state_t);

    // Separators never change once drawn, so they are drawn once here and
    // never touched (and never pushed) again.
    digits_draw_colon(X_COLON1, Y0, SEP_W, DIGIT_H, SEG_T, PX_BLACK);
    digits_draw_colon(X_COLON2, Y0, SEP_W, DIGIT_H, SEG_T, PX_BLACK);

    for (int i = 0; i < 6; i++) {
        digits_draw(DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, SEG_T, 0, PX_BLACK);
        // s_state->lastDigit[i] is already 0: APP_STATE zeroes the
        // allocation, and 0 is exactly the value just drawn.
    }

    printf("chrono: entered, stopped at 00:00:00\r\n");
}

/* ---------------------------------------------------------------------
 * tick(): updates and pushes only the digit cells that changed.
 * ------------------------------------------------------------------- */
static void chrono_tick(const app_frame_t *f) {
    chrono_state_t *s = s_state;

    if (f->bootClicked) {
        s->running = false;
        s->elapsedMs = 0;
        printf("chrono: BOOT click, reset to 00:00:00\r\n");
    }

    if (f->key & KEY_SHORT) {
        // Toggle immediately on the short-press event the runtime hands us,
        // and do the elapsed-time arithmetic right here, before the digit
        // redraw below runs in this SAME tick. That is the whole fix for
        // the responsiveness complaint ("the chronometer is too slow when
        // stopping"): the old standalone app was already careful to toggle
        // on the press edge rather than waiting for a release verdict, and
        // to do so before any drawing that tick, so the frozen time appears
        // at once rather than on the next timer update. Nothing here defers
        // to a later tick.
        if (s->running) {
            s->elapsedMs += f->nowMs - s->runStartMs;
            s->running = false;
        } else {
            s->runStartMs = f->nowMs;
            s->running = true;
        }
    }

    uint32_t shownMs = s->running ? s->elapsedMs + (f->nowMs - s->runStartMs) : s->elapsedMs;
    uint32_t totalCs = shownMs / 10; // 1 centisecond = 10ms
    // Minutes wrap at 100 so the layout stays fixed at 2 digits; a
    // stopwatch running past 99:59:99 is not a case this puck's use needs
    // to handle cleanly.
    uint32_t mm = (totalCs / 6000) % 100;
    uint32_t ss = (totalCs / 100) % 60;
    uint32_t cs = totalCs % 100;
    int digits[6] = {
        (int)(mm / 10), (int)(mm % 10),
        (int)(ss / 10), (int)(ss % 10),
        (int)(cs / 10), (int)(cs % 10),
    };

    // Only repaint digits that actually changed, in this same tick. Each
    // push here covers exactly one 48x120 landscape digit cell, and DIGIT_H
    // (120) is already a multiple of 8, so gfx_push_land never has to pad
    // the pushed window's row length (the panel-space width, which is this
    // cell's landscape height after rotation).
    for (int i = 0; i < 6; i++) {
        if (digits[i] == s->lastDigit[i]) continue;
        digits_clear(DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, PX_WHITE);
        digits_draw(DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, SEG_T, digits[i], PX_BLACK);
        gfx_push_land(DIGIT_X[i], Y0, DIGIT_W, DIGIT_H);
        s->lastDigit[i] = digits[i];
    }
}

// wantsShake is deliberately false. Shake is the Etch-A-Sketch gesture and
// belongs only where erasing IS the app's identity (see sensors.h); wiring
// it here as a second reset would destroy exactly the thing a stopwatch is
// for, a number a child carries across a room, at a whole side of the
// board's chest that already opts in for the sketchpad. Do not add it back
// thinking its absence was an oversight: it was asked for, on purpose.
const app_t g_chronoApp = {
    .name      = "chrono",
    .enter     = chrono_enter,
    .tick      = chrono_tick,
    .leave     = NULL,
    .landscape = true,
    .wantsShake = false,
};
