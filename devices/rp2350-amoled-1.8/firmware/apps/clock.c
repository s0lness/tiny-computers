/*
 * clock: what time it is, in two layouts, and an honest empty face when it
 * does not know.
 *
 * THE OWNER'S ASK, verbatim: "Horloge (si y a moyen de garder l'heure en
 * memoire, et si possible un display horizontal qui marche le chronometre et
 * un vertical qui fait HH / MM de facon egale, les heures sur une ligne et
 * les minutes sur l'autre sans ':' ou quoi que ce soit)."
 *
 * So there are two faces, and which one is showing is decided by how the
 * puck is being held, not by a setting:
 *
 *   held LONG-WAYS   one line, read like the stopwatch: HH : MM.
 *   held UPRIGHT     hours on one line, minutes on the line below, the same
 *                    size as each other, and NO colon, no bar, no dot, no
 *                    separator of any kind.
 *
 * HOW THE TWO LINES BALANCE WITHOUT A SEPARATOR, since that was the precise
 * part of the ask and the easy fix (put something between them) is the one
 * thing forbidden:
 *
 *   - the two lines are the SAME two cells wide, so they are exactly the
 *     same length. Two equal-length rows read as a pair the way two equal
 *     words do; nothing has to sit between them to say they belong together.
 *     This is also why the hours keep a leading zero: "09" over "05" is a
 *     pair, "9" over "05" is a number that lost a digit.
 *   - the gap BETWEEN the lines is 56px and the gap between the two digits
 *     WITHIN a line is 24px, a ratio of about 2.3. That is the whole
 *     separator: each line closes up into one word, and the two words sit
 *     apart. Typography has done it this way forever and it costs no ink.
 *   - the block is centred on both axes of the visible canvas, so the paper
 *     above the hours and below the minutes is equal, and the eye has no
 *     reason to read one line as a heading over the other.
 *
 * WHAT IT SHOWS WHEN IT DOES NOT KNOW. The RTC on this board keeps time
 * across a power-off but not across a flat battery, and it says which
 * happened through the OS flag in its Seconds register (sensors.h's wall
 * clock section, and docs/decisions/0011). When that flag is set, the honest
 * answer is not a wrong time and not an error message, because there is no
 * text on this device and a four-year-old could not read it anyway. It is an
 * EMPTY FACE: the same numerals, all seven segments of each, drawn thin and
 * very pale, breathing slowly in and out over five seconds. It has the shape
 * of a clock and no number in it, which is exactly the situation, and it is
 * the one screen in this app that is impossible to confuse with a working
 * one - a real digit is thick and black, this is a hairline ghost. It is
 * also the shape of an unlit seven-segment display, which is what this
 * device physically resembles.
 *
 * This project has already lost a day to a stopwatch that looked identical
 * whether it was working or dead (docs/decisions/0004). That is the mistake
 * this screen exists to not repeat.
 *
 * SECONDS ARE NOT SHOWN, on purpose. A face that ticks every second is a
 * different object from one that tells you the hour: it asks to be watched,
 * it costs a redraw a second forever on a panel with a real burn-in
 * concern, and it buys nothing for a child who cannot read a clock yet and
 * an adult who wants to know whether it is nearly lunchtime. The clock is
 * still exact to the second underneath (the seconds are what the minute
 * rolls over from); they are simply not on the screen. The only thing that
 * ever moves on a working face is a minute digit, once a minute, one cell.
 *
 * WHAT IS NOT HERE, and is not an oversight: no date, no alarm, no seconds,
 * no battery, no app name, nothing belonging to the runtime (decision 0002
 * section 4b). A clock is a clock.
 */
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"

/* =========================================================================
 * THE ORIENTATION SIGNAL, AND THE ONE FUNCTION THAT READS IT
 *
 * Which way up the puck is being held is a property of the DEVICE, not of
 * this app: it comes from the IMU, on i2c1, which core1 owns exclusively,
 * and it is being published through sensors.h and app_frame_t by another
 * change that is in flight at the time this file was written. This app must
 * NOT read the IMU itself, and does not.
 *
 * That field does not exist in this worktree yet, so this file carries ONE
 * function, right here, as the single place the signal enters the app.
 * Everything below asks this function and nothing else, so wiring the real
 * signal in is a one-line edit inside these braces rather than a hunt
 * through the drawing code.
 *
 * THE STAND-IN, until then: a short press of PWR flips the layout. That is
 * a real signal this device genuinely delivers (KEY_SHORT, sensors.h - the
 * AXP2101 latches it, the runtime passes it through untouched), so nothing
 * here is being tested against an input the hardware cannot produce, which
 * is the rule emu_abi.h holds the emulator to and the same rule is worth
 * holding an app to. It is a stand-in and not a feature: when the shared
 * signal lands, the body of this function becomes
 *
 *     return f->heldUpright;      // or whatever the field ends up called
 *
 * and the KEY_SHORT branch and s->standInUpright go with it.
 * ========================================================================= */

typedef struct clock_state_s clock_state_t;
static bool clock_is_upright(clock_state_t *s, const app_frame_t *f);

/* =========================================================================
 * LAYOUT
 *
 * Two sets of numbers, one per way of holding the thing. Both are checked
 * against the two rules that are easy to break silently:
 *
 *   - PANEL_BEZEL_MARGIN_PX (gfx.h): the case hides a band along every edge,
 *     and the emulator cannot show it, so a shape that runs closer than this
 *     to the framebuffer's edge is invisible on the real device and looks
 *     perfect on screen. Every margin below is far outside it, and nothing
 *     in this app ever moves outside its own cell, so the resting extent IS
 *     the maximum extent here - which is the distinction that bit somebody
 *     recently and is worth stating rather than leaving to be re-derived.
 *   - decision 0001: a pushed window's row length must be a multiple of 8.
 *     The row length is the PANEL-space width of the window, which is the
 *     cell's width when the app is upright and the cell's HEIGHT when it is
 *     long-ways (gfx_land_rect swaps them). So the portrait cell width and
 *     the landscape cell height are both multiples of 8, and every cell
 *     origin is even, which means gfx_push has nothing to round and a push
 *     covers exactly the cell that was cleared and not a pixel more.
 * ========================================================================= */

// ---- held long-ways: 448 x 368, one line, laid out like the stopwatch ----
// [80][80] [32 dots] [80][80], 16px between every element, 16px margin each
// side: 4*80 + 32 + 4*16 = 416, plus two 16px margins = 448 exactly.
#define L_DIGIT_W 80
#define L_DIGIT_H 208   // multiple of 8: this is the pushed row length here
#define L_SEG_T   26
#define L_DOTS_W  32
#define L_Y0      80    // (368 - 208) / 2, vertically centred
#define L_DOTS_X  208

static const int L_DIGIT_X[4] = { 16, 112, 256, 352 };

// ---- held upright: 368 x 448, hours over minutes, no separator ----------
// Two 112px cells 24px apart = 248 wide, centred at x=60. Two 168px lines
// 56px apart = 392 tall, centred at y=28. See the header comment for why
// those two gaps are the numbers they are.
#define P_DIGIT_W 112   // multiple of 8: the pushed row length here
#define P_DIGIT_H 168
#define P_SEG_T   32
#define P_X0      60
#define P_X1      196
#define P_Y_HOURS   28
#define P_Y_MINUTES 252

static const int P_DIGIT_X[4] = { P_X0, P_X1, P_X0, P_X1 };
static const int P_DIGIT_Y[4] = { P_Y_HOURS, P_Y_HOURS, P_Y_MINUTES, P_Y_MINUTES };

// The empty face's stroke, as a fraction of a lit numeral's. Thin enough
// that a ghost can never be mistaken for a digit at a glance, and cheap:
// the anti-aliased rasteriser's cost scales with the stroke's own area, so
// the one thing this app animates is also the cheapest thing it draws.
#define GHOST_T(t) ((t) * 2 / 5)

// The empty face breathes between these two greys (0 = black, 255 = paper),
// on a five second cycle, in six steps. Slow and shallow on purpose: this is
// a clock that is asleep waiting to be told the time, not an alarm. Six
// steps rather than a smooth ramp keeps it to one redraw of four cells about
// every 800ms.
#define GHOST_INK_PALE  228
#define GHOST_INK_DEEP  176
#define GHOST_STEPS     6
#define GHOST_PERIOD_MS 5000u

// A lit numeral is black; the pair you are NOT currently dragging goes grey
// while the time is being set, so the screen says which of the two your
// finger is moving without a word or an arrow on it.
#define INK_LIT     0
#define INK_RESTING 150

// Sentinel for "this cell holds a ghost, not a digit".
#define CELL_GHOST (-1)

/* =========================================================================
 * SETTING THE TIME, which is the whole product
 *
 * An RTC that has never been set is a counter. There is no keyboard, no
 * text, and the person who most often holds this device cannot read - so
 * the question is not "what dialog" but WHO sets it, HOW OFTEN, and WITH
 * WHAT GESTURE.
 *
 * WHO AND HOW OFTEN: the owner, roughly once per battery. The clock survives
 * every reboot and every deliberate power-off (sensors.h), so the only
 * events that lose it are a flat battery and the very first boot of a fresh
 * board. That is a handful of times a year, by an adult, usually with the
 * device in one hand.
 *
 * THE GESTURE: hold BOOT, and slide a finger over the pair you want to
 * change. One rule covers both layouts, and it is the same sentence in each:
 *
 *     the axis that SEPARATES the two pairs chooses which pair you are
 *     setting; the other axis carries the value.
 *
 * Held upright the hours are the top line and the minutes the bottom one, so
 * touching high sets hours, touching low sets minutes, and sliding left to
 * right runs the value from 0 up. Held long-ways the hours are the left pair
 * and the minutes the right, so touching left sets hours, touching right
 * sets minutes, and sliding top to bottom runs the value. Nothing to learn
 * twice.
 *
 * The value is ABSOLUTE, not accumulated: where your finger is along the
 * axis IS the number, edge to edge. So any value is one slide away, there is
 * no drift to correct, nothing to hold down and wait for, and letting go
 * without releasing BOOT lets you go straight to the other pair. Releasing
 * BOOT commits, and that single event is the only i2c write this app ever
 * causes.
 *
 * WHY BOOT HAS TO BE HELD: without it, the child who carries this device
 * around would reset the clock with her thumb, daily. BOOT is a real button
 * on the case that has to be held with the other hand while sliding, which
 * is a two-handed, deliberate act; it is also already the modifier idiom
 * this device uses (BOOT plus PWR opens the menu), so it is not a new
 * concept. A wrong setting costs ten seconds to fix, so the guard does not
 * need to be stronger than that.
 *
 * WHY NOT OVER USB, which tools/dev.ts could do in twenty lines. It was
 * considered and it is the weaker answer, for three reasons that all point
 * the same way. It needs a computer and a cable to fix a toy that lives on a
 * shelf - and the moment the clock is most likely to be wrong is exactly
 * when the battery went flat somewhere away from the desk. It puts a second
 * source of truth in the loop: the host knows UTC, the puck needs local wall
 * time, so the tool would have to carry a timezone and a DST rule, and a
 * clock that is confidently one hour out is precisely the failure this app's
 * empty face exists to avoid. And it is invisible: the owner cannot tell
 * whether it worked without looking at the device anyway, whereas the
 * gesture's whole feedback is the number changing under his finger. The
 * seconds are set to zero on commit, so setting it at the top of a minute is
 * exact; that is the only accuracy USB would have bought.
 * ========================================================================= */

#define FIELD_HOURS   0
#define FIELD_MINUTES 1

struct clock_state_s {
    // What is currently on the panel, so a tick only ever redraws a cell
    // whose content actually changed. Both halves matter: the same digit in
    // a different ink (the empty face breathing, or a pair greying out while
    // the other is set) is a change, and a different digit in the same ink
    // obviously is.
    int     cellDigit[4];
    uint8_t cellInk[4];
    bool    painted;   // has a full face been drawn at least once
    bool    upright;   // ...and in which layout

    // The setting gesture. `active` spans the whole BOOT hold, not just the
    // frames a finger is down, so lifting to move to the other pair does not
    // end it.
    bool active;
    bool touching;     // a finger is down within this hold
    int  field;
    int  setH, setM;

    // Committed, and waiting for the value to come back out the other side.
    // THIS IS A HARDWARE-ONLY GLITCH GUARD and the emulator cannot show what
    // it prevents: on the board, sensors_set_clock() is a core0 REQUEST that
    // core1 performs (sensors.h), and performing it is three bounded i2c
    // transactions, so the published clock still reads "not known" for a
    // millisecond or two after BOOT comes up. Without this, releasing BOOT
    // would flash the empty face for a frame before the time appeared -
    // exactly at the moment the owner is looking to see whether it worked.
    // In wasm the shim's set is synchronous, so this branch is never taken
    // there and no test can catch its absence.
    bool pendingSet;
    int  pendingH, pendingM;

    // The separator dots are part of the face, not furniture: they carry
    // whatever ink the numerals do, so the empty face is empty all the way
    // through instead of being four ghosts with one bold black mark between
    // them. Remembered separately because they are their own cell.
    uint8_t dotsInk;

    // Only for the stand-in orientation signal - see clock_is_upright().
    bool standInUpright;

    // Log throttling: the firmware log is how the tests and the owner read
    // this app's mind, and a line per frame would drown it.
    int lastLoggedH, lastLoggedM;
    bool loggedUnknown;
};

static clock_state_t *s_state;

static bool clock_is_upright(clock_state_t *s, const app_frame_t *f) {
    // THE ONE PLACE THE ORIENTATION SIGNAL ENTERS THIS APP. See the section
    // at the top of this file: when sensors.h/app_frame_t publish it, this
    // whole body becomes `return f->heldUpright;`.
    if (f->key & KEY_SHORT) s->standInUpright = !s->standInUpright;
    return s->standInUpright;
}

/* ---- cells --------------------------------------------------------------
 *
 * One description of a digit cell, in whichever space the current layout
 * works in, so every draw, clear and push below is written once instead of
 * once per orientation.
 */
typedef struct {
    digits_space_t space;
    int x, y, w, h, t;
} cell_t;

static void cell_for(bool upright, int i, cell_t *out) {
    if (upright) {
        out->space = DIGITS_PORTRAIT;
        out->x = P_DIGIT_X[i];
        out->y = P_DIGIT_Y[i];
        out->w = P_DIGIT_W;
        out->h = P_DIGIT_H;
        out->t = P_SEG_T;
    } else {
        out->space = DIGITS_LANDSCAPE;
        out->x = L_DIGIT_X[i];
        out->y = L_Y0;
        out->w = L_DIGIT_W;
        out->h = L_DIGIT_H;
        out->t = L_SEG_T;
    }
}

// The separator, as a cell like any other so that it clears and pushes
// through the same two functions the numerals do. Long-ways only: the
// upright face has no separator at all, which was the owner's whole point.
static void dots_cell(cell_t *out) {
    out->space = DIGITS_LANDSCAPE;
    out->x = L_DOTS_X;
    out->y = L_Y0;
    out->w = L_DOTS_W;
    out->h = L_DIGIT_H;
    out->t = L_SEG_T;
}

static void cell_clear(const cell_t *c) {
    if (c->space == DIGITS_PORTRAIT) gfx_fill_rect(c->x, c->y, c->w, c->h, PX_WHITE);
    else gfx_fill_rect_land(c->x, c->y, c->w, c->h, PX_WHITE);
}

static void cell_push(const cell_t *c) {
    // The cleared rectangle and the pushed window are the same rectangle,
    // deliberately and every time. Clearing outside what is pushed leaves
    // the panel holding pixels the framebuffer no longer has, which is the
    // reverse of the rule this codebase already had and the one that was
    // found the hard way.
    if (c->space == DIGITS_PORTRAIT) gfx_push(c->x, c->y, c->x + c->w - 1, c->y + c->h - 1);
    else gfx_push_land(c->x, c->y, c->w, c->h);
}

// Draws one cell's content into the framebuffer. Does not clear and does not
// push: the two callers below do that around it, in the two different ways
// they need (a full repaint clears the whole panel once and pushes it once;
// a per-cell update clears and pushes exactly that cell).
static void cell_render(const cell_t *c, int digit, uint8_t ink) {
    uint16_t px = gray_to_px(ink);
    if (digit == CELL_GHOST) {
        digits_draw_soft(c->space, c->x, c->y, c->w, c->h, GHOST_T(c->t), 8, px);
    } else {
        digits_draw_soft(c->space, c->x, c->y, c->w, c->h, c->t, digit, px);
    }
}

/* ---- what the face should show right now -------------------------------- */

// Fills digits[4] and ink[4] with what this frame ought to be showing.
// Returns false if the time is not known, in which case the cells are
// ghosts. Nothing here draws; deciding and drawing are kept apart so the
// redraw-only-what-changed loop has one thing to compare against.
static bool face_now(clock_state_t *s, const app_frame_t *f, int digits[4], uint8_t ink[4]) {
    int hh = 0, mm = 0;
    bool known;

    if (s->active) {
        // While the owner is setting it, the face shows what he is dialling
        // in, not what the chip still holds.
        hh = s->setH;
        mm = s->setM;
        known = true;
    } else {
        sensors_clock_t c;
        sensors_clock(&c);
        known = c.known;
        if (known) {
            // The RTC was read once, at boot; the RP2350's own timer has
            // been counting since. See sensors.h's wall clock section for
            // why nothing polls the chip.
            uint32_t sec = (c.secOfDay + (f->nowMs - c.sampledAtMs) / 1000u) % 86400u;
            hh = (int)(sec / 3600u);
            mm = (int)((sec / 60u) % 60u);
            s->pendingSet = false; // the value came back out the other side
        } else if (s->pendingSet) {
            // Just committed, and core1 has not published it yet - see
            // pendingSet. Keep showing what was dialled in rather than
            // flashing the empty face for a frame.
            hh = s->pendingH;
            mm = s->pendingM;
            known = true;
        }
    }

    if (!known) {
        // The empty face, breathing. A triangle wave rather than a sine: it
        // is six discrete levels either way, and a triangle needs no math
        // library on a path that runs on both targets.
        uint32_t phase = f->nowMs % GHOST_PERIOD_MS;
        uint32_t half = GHOST_PERIOD_MS / 2u;
        uint32_t tri = phase < half ? phase : GHOST_PERIOD_MS - phase;
        uint32_t step = tri * GHOST_STEPS / half;
        if (step > GHOST_STEPS - 1) step = GHOST_STEPS - 1;
        uint8_t g = (uint8_t)(GHOST_INK_PALE -
                              (int)step * (GHOST_INK_PALE - GHOST_INK_DEEP) / (GHOST_STEPS - 1));
        for (int i = 0; i < 4; i++) {
            digits[i] = CELL_GHOST;
            ink[i] = g;
        }
        return false;
    }

    digits[0] = hh / 10;
    digits[1] = hh % 10;
    digits[2] = mm / 10;
    digits[3] = mm % 10;

    uint8_t hoursInk = INK_LIT, minutesInk = INK_LIT;
    if (s->active && s->touching) {
        // Say which pair the finger is moving, with the only thing on screen
        // there is to say it with.
        if (s->field == FIELD_HOURS) minutesInk = INK_RESTING;
        else hoursInk = INK_RESTING;
    }
    ink[0] = ink[1] = hoursInk;
    ink[2] = ink[3] = minutesInk;
    return true;
}

/* ---- painting ----------------------------------------------------------- */

// A whole face, from white. Used on enter() and whenever the puck is turned
// over, which is the one moment every pixel on the panel changes at once.
static void paint_all(clock_state_t *s, bool upright, const int digits[4], const uint8_t ink[4],
                      uint8_t dotsInk, bool push) {
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);

    if (!upright) {
        // The one separator in this app, and only in the layout the owner
        // asked to read like the stopwatch. Round dots, not squares
        // (decision 0009), and in the same ink as the numerals beside them,
        // so that an empty face has nothing solid left on it to look at.
        cell_t d;
        dots_cell(&d);
        digits_draw_dots_soft(d.space, d.x, d.y, d.w, d.h, d.t, gray_to_px(dotsInk));
        s->dotsInk = dotsInk;
    }

    for (int i = 0; i < 4; i++) {
        cell_t c;
        cell_for(upright, i, &c);
        cell_render(&c, digits[i], ink[i]);
        s->cellDigit[i] = digits[i];
        s->cellInk[i] = ink[i];
    }
    s->upright = upright;
    s->painted = true;

    // enter() must not push (app.h: the runtime pushes the whole panel once
    // after it returns); a turn-over must, and the whole panel is exactly
    // what changed.
    if (push) gfx_push_all();
}

/* ---- the setting gesture ------------------------------------------------ */

// Where a value lands when the finger is at `v` along an axis running from
// `lo` to `hi`, for a field with `range` values. Absolute: the position IS
// the number. Clamped at both ends rather than wrapping, so pressing at the
// edge of the glass is a stable 0 or 23 instead of a coin flip.
static int value_from_axis(int v, int lo, int hi, int range) {
    if (v <= lo) return 0;
    if (v >= hi) return range - 1;
    return (v - lo) * range / (hi - lo);
}

// `upright` is passed in rather than read back off s->upright, which is the
// layout currently PAINTED and is one frame stale on the tick the puck is
// turned over - a gesture in flight across that instant would otherwise pick
// its field with the wrong rule for exactly one frame.
static void setting_tick(clock_state_t *s, const app_frame_t *f, bool upright) {
    // BOOT's LEVEL, not its click: this is a modifier being held, the same
    // way the menu chord uses it (runtime_core.c). sensors_boot_down() is a
    // published signal like any other - the app is not touching a chip - and
    // it is rate-limited to 20Hz inside bootbtn.c regardless of how often it
    // is asked (see sensors.h's BOOT section).
    bool bootHeld = sensors_boot_down();

    if (!bootHeld) {
        if (s->active) {
            s->active = false;
            s->touching = false;
            // ONE write, on the event that made the value worth keeping.
            // Seconds are zero because the owner set this at a moment he
            // chose: he lines it up with a wall clock's minute and lets go.
            sensors_set_clock((uint32_t)s->setH * 3600u + (uint32_t)s->setM * 60u);
            s->pendingSet = true;
            s->pendingH = s->setH;
            s->pendingM = s->setM;
            printf("clock: set to %02d:%02d\r\n", s->setH, s->setM);
            s->lastLoggedH = -1; // make the next steady-state line print
        }
        return;
    }

    if (!f->touchDown) {
        // BOOT is held but nothing is on the glass: the hold stays open (so
        // a finger can be lifted and put down on the other pair) and nothing
        // changes.
        s->touching = false;
        return;
    }

    bool starting = !s->active;
    if (starting) {
        s->active = true;
        // Seed from whatever is on screen, so the pair that is NOT touched
        // keeps its value. An unknown clock seeds at 12:00, which is a
        // neutral place to start dialling from rather than midnight, where
        // half the useful range is a long slide away.
        sensors_clock_t c;
        sensors_clock(&c);
        if (c.known) {
            uint32_t sec = (c.secOfDay + (f->nowMs - c.sampledAtMs) / 1000u) % 86400u;
            s->setH = (int)(sec / 3600u);
            s->setM = (int)((sec / 60u) % 60u);
        } else {
            s->setH = 12;
            s->setM = 0;
        }
    }

    // The field is latched when the finger LANDS, not re-evaluated as it
    // moves: sliding a value all the way to one end must not hand the
    // gesture over to the other pair halfway through.
    bool landing = starting || !s->touching;
    s->touching = true;

    int value;
    if (upright) {
        // Upright: the pairs are stacked, so vertical position chooses and
        // horizontal position sets.
        if (landing) s->field = (f->touchY < PANEL_H / 2) ? FIELD_HOURS : FIELD_MINUTES;
        value = value_from_axis(f->touchX, PANEL_BEZEL_MARGIN_PX, PANEL_W - PANEL_BEZEL_MARGIN_PX,
                                s->field == FIELD_HOURS ? 24 : 60);
    } else {
        // Long-ways: the pairs are side by side, so it is the other way
        // round. Panel coordinates map to landscape as (lx, ly) =
        // (py, PANEL_W-1-px) - gfx.h's own mapping, inverted.
        int lx = f->touchY;
        int ly = PANEL_W - 1 - f->touchX;
        if (landing) s->field = (lx < LAND_W / 2) ? FIELD_HOURS : FIELD_MINUTES;
        value = value_from_axis(ly, PANEL_BEZEL_MARGIN_PX, LAND_H - PANEL_BEZEL_MARGIN_PX,
                                s->field == FIELD_HOURS ? 24 : 60);
    }

    if (s->field == FIELD_HOURS) s->setH = value;
    else s->setM = value;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void clock_enter(void) {
    s_state = APP_STATE(clock_state_t);
    s_state->lastLoggedH = -1;
    s_state->lastLoggedM = -1;

    app_frame_t first = { 0 };
    bool upright = clock_is_upright(s_state, &first);
    int digits[4];
    uint8_t ink[4];
    bool known = face_now(s_state, &first, digits, ink);
    paint_all(s_state, upright, digits, ink, known ? INK_LIT : ink[0], false);

    printf("clock: state=%d bytes (arena %d)\r\n", (int)sizeof(clock_state_t), APP_ARENA_BYTES);
}

static void clock_tick(const app_frame_t *f) {
    clock_state_t *s = s_state;

    bool upright = clock_is_upright(s, f);
    setting_tick(s, f, upright);

    int digits[4];
    uint8_t ink[4];
    bool known = face_now(s, f, digits, ink);
    // The dots are as dark as the numerals when there is a time to show, and
    // as pale as the ghosts when there is not - breathing with them, since
    // they are redrawn by the same "did this cell's ink change" test below.
    uint8_t dotsInk = known ? INK_LIT : ink[0];

    if (upright != s->upright || !s->painted) {
        printf("clock: layout %s\r\n", upright ? "upright" : "long-ways");
        paint_all(s, upright, digits, ink, dotsInk, true);
    } else {
        // The steady state: at most one cell changes, once a minute, and on
        // most frames this loop does nothing at all.
        for (int i = 0; i < 4; i++) {
            if (digits[i] == s->cellDigit[i] && ink[i] == s->cellInk[i]) continue;
            cell_t c;
            cell_for(upright, i, &c);
            cell_clear(&c);
            cell_render(&c, digits[i], ink[i]);
            cell_push(&c);
            s->cellDigit[i] = digits[i];
            s->cellInk[i] = ink[i];
        }
        if (!upright && dotsInk != s->dotsInk) {
            cell_t d;
            dots_cell(&d);
            cell_clear(&d);
            digits_draw_dots_soft(d.space, d.x, d.y, d.w, d.h, d.t, gray_to_px(dotsInk));
            cell_push(&d);
            s->dotsInk = dotsInk;
        }
    }

    if (!known) {
        if (!s->loggedUnknown) {
            s->loggedUnknown = true;
            printf("clock: the time is not known - showing the empty face\r\n");
        }
    } else {
        s->loggedUnknown = false;
        int hh = digits[0] * 10 + digits[1];
        int mm = digits[2] * 10 + digits[3];
        if (hh != s->lastLoggedH || mm != s->lastLoggedM) {
            s->lastLoggedH = hh;
            s->lastLoggedM = mm;
            printf("clock: %02d:%02d\r\n", hh, mm);
        }
    }
}

// landscape = true because that is the face this app comes up in and the one
// the owner described first. It is a static flag and this app is not
// statically oriented, which the runtime only uses to decide which way to
// draw the menu overlay on top of it (app.h); when the shared orientation
// signal lands, whether that flag should follow it is the runtime's question
// rather than this file's.
//
// wantsShake is false for the same reason the stopwatch's is (sensors.h):
// shaking a clock should not do anything, and erasing is not a verb a clock
// has.
const app_t g_clockApp = {
    .name       = "clock",
    .enter      = clock_enter,
    .tick       = clock_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
