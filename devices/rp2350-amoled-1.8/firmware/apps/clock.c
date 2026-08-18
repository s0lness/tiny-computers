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
 * THE PULSE. The owner asked the long-ways separator to pulse, so the
 * working face reads as alive rather than static ("tu peux faire pulser le
 * ':' qui sépare les chiffres ?"). It pulses ONLY in the long-ways face -
 * the upright face has no separator at all, and this file's own header
 * already argues at length for why nothing should sit between its two
 * lines; putting a pulsing mark there would undo that argument, not extend
 * it. That choice was put to the owner and he has not overruled it.
 *
 * WHY THIS IS ALLOWED WHEN SECONDS ARE NOT is a cost argument, not a taste
 * one, and it is the same argument as the paragraph above: a face that
 * ticks every second costs a redraw a second, forever, of the WHOLE face.
 * The dots pulse by toggling between two ink levels every
 * DOTS_PULSE_HALF_MS (500ms - lit half a second, faint half a second, so
 * ONE blink per second, the rate every clock a child has seen uses), and
 * every toggle redraws exactly the cell dots_cell() already owned before
 * this feature existed (32 x 208 landscape px, the separator's own bounding
 * box, sized for the known/unknown transition it always had to redraw for).
 * That is about 6,656 px pushed per toggle, roughly 13,300 px/sec at two
 * toggles a second - under a twelfth of one full-panel repaint (368*448 =
 * 164,864 px) per second, and confined to a cell already being pushed. The
 * first version ran at four toggles a second and the owner asked for it
 * slower; halving the rate halved the cost with it. A pulsing pair
 * of dots is a few dozen pixels moving four times a second inside a window
 * that was already there; a face that ticks every second is the whole face,
 * every second, forever. That difference is why one is acceptable here and
 * the other still is not.
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
#include "shapes.h"

/* =========================================================================
 * THE ORIENTATION SIGNAL, AND THE ONE FUNCTION THAT READS IT
 *
 * Which way up the puck is being held is a property of the DEVICE, not of
 * this app: it comes from the IMU, on i2c1, which core1 owns exclusively,
 * published through app_frame_t.tilt (firmware/runtime/tilt.h, decision
 * 0012). This app does not read the IMU itself and never has; everything
 * below asks clock_face() and nothing else, so this is still the one place
 * the signal enters the app.
 *
 * g_clockApp.landscape is true (see the app_t at the bottom of this file),
 * so the runtime hands this app gravity already rotated into ITS OWN
 * landscape drawing space (runtime_core.c's tilt_for_app) - the same space
 * chrono, timer and four already draw in, and that rotation is
 * `land.up = (panel.up + 3) % 4`.
 *
 * THE OWNER'S RULE, verbatim: "on the clock: when in top mode and bottom
 * mode i want the clock to be in portrait mode (with the hours facing the
 * top) and when in right or left use the paysage mode." That is a rule
 * about the PANEL's own physical edges (the same TOP/RIGHT/BOTTOM/LEFT the
 * emulator's TURN control names - puckpose.ts's edgeNameForTurn()), not
 * about land.up directly: `land.up = (panel.up + 3) % 4` rotates one step
 * backwards through TOP->RIGHT->BOTTOM->LEFT->TOP, so panel TOP/BOTTOM (the
 * owner's "portrait" pair) arrive here as land.up LEFT/RIGHT, and panel
 * RIGHT/LEFT (his "paysage" pair) arrive as land.up TOP/BOTTOM. So, IN
 * TERMS OF f->tilt.up, the value this function actually reads:
 *
 *     land.up == LEFT or RIGHT  ->  upright (portrait)
 *     land.up == TOP or BOTTOM  ->  long-ways (landscape)
 *
 * which is the OPPOSITE of what an earlier version of this function checked
 * (TOP/BOTTOM -> upright). THIS WAS MEASURED AGAINST THE EMULATOR, not
 * reasoned out from the rotation formula alone - the same caution this
 * comment already carried once before, for exactly the same reason: an
 * earlier draft of THIS OWN PARAGRAPH argued a "genuine quarter turn" lands
 * on {TOP, BOTTOM} and called that "upright", which is what the old code
 * did and is exactly backwards from what a hand actually holding the puck
 * long-ways produces - confirmed 2026-08-18 by driving
 * puckpose.ts's gravityFromPose() (the same conversion the emulator's own
 * TURN buttons now use, since commit 78f7c99 made them drive real firmware
 * orientation instead of only the picture) through this compiled emu.wasm
 * for all four TURN positions and reading back the firmware's own "clock:
 * layout ..." log line. Do not re-derive this pairing from the rotation
 * formula on paper again; it has already been gotten backwards twice that
 * way. If it is ever in doubt, drive the emulator and read the log.
 *
 * WHICH EDGE OF EACH PAIR IS "NORMAL" AND WHICH IS UPSIDE DOWN, PRECISELY.
 * Two edges both select the same FAMILY (portrait or landscape) but are 180
 * degrees apart from each other, so only one of them can be the face as
 * originally laid out - the other needs the whole picture turned to still
 * read right side up. See clock_face_t and PICTURE, ROTATED below for how;
 * this section is only which edge gets which treatment.
 *
 * THE OWNER'S DEFINITION, verbatim, and the one to build every edge's answer
 * from rather than reasoning about "which way is up" in the abstract: "in
 * 'left' up is towards the boot and pwr button". Both buttons sit on the
 * panel's own RIGHT edge (AGENTS.md's board table, PWR lower, BOOT upper) -
 * a single, fixed, physical edge of the case - so his sentence is really
 * "when the puck is turned so its own RIGHT edge points up, that pose is
 * called LEFT" (edgeNameForTurn()'s label for a pose is which edge is
 * pointing DOWN, resting in the hand or on the table - not, as an earlier
 * version of this comment assumed, which edge is up). That earlier,
 * WRONG assumption is exactly what put the flip on the wrong edge: BOTTOM
 * and TOP are opposite panel edges 180 degrees apart, where getting the
 * turn direction backwards cannot matter (180 has no handedness); LEFT and
 * RIGHT-for-long-ways is the SAME 180-degree pair by the panel's own
 * geometry, but this app anchors long-ways to land.up rather than to panel
 * edges directly, and the panel-to-land rotation (`(panel.up + 3) % 4`,
 * above) is a 90-degree step, where a direction mistake silently swaps
 * which of the two answers is "normal". Confirmed against
 * emulator/src/device.ts's actual CSS (`applyRotation()`: `rotate(totalDeg)`,
 * standard CSS clockwise-positive), not re-reasoned on paper a third time:
 * the buttons sit on the panel's native right edge, and TURN=LEFT is
 * `rotate(-90deg)`, 90 degrees COUNTERCLOCKWISE - which carries that right
 * edge to the TOP of the picture, matching the owner's screenshot exactly.
 *
 * So, edge by edge, what up.tilt means physically and what this app does
 * with it:
 *
 *   - land.up == LEFT (panel TOP up - the owner's own first example, and
 *     this device's flat, unmeasured default, see below): portrait, face as
 *     laid out, unflipped.
 *   - land.up == RIGHT (panel BOTTOM up, 180 from the above - no handedness
 *     to get wrong here): portrait, the same face turned 180.
 *   - land.up == BOTTOM (panel LEFT up - buttons toward the top edge, the
 *     owner's own definition, and the same "held sideways with the buttons
 *     along the top edge" convention app.h documents for chrono/timer/four/
 *     morpion): long-ways, face as laid out, unflipped.
 *   - land.up == TOP (panel RIGHT up - buttons toward the BOTTOM edge, 180
 *     from the above): long-ways, the same face turned 180.
 *
 * Notice the asymmetry on purpose: portrait's unflipped edge is LEFT,
 * long-ways's unflipped edge is BOTTOM - two different land.up values, not
 * a pattern like "whichever is first" that a future edit could simplify
 * into one formula without checking both against the emulator again.
 *
 * THE FLAT, UNMEASURED DEFAULT NOW READS UPRIGHT, AND THAT IS A REAL
 * BEHAVIOUR CHANGE FROM BEFORE, STATED RATHER THAN HIDDEN. tilt.c's `up`
 * defaults to TILT_UP_TOP (panel.up = TOP) before the IMU has produced a
 * measurement past TILT_UP_MIN_G, which is now land.up = LEFT - the
 * unflipped PORTRAIT case, per the table above. The previous version of
 * this file deliberately defaulted to long-ways, reasoning that a clock
 * picked up before the IMU has a reading should match every other app on
 * this device, which boots landscape. That reasoning does not survive the
 * owner's new rule intact: panel TOP is his own first, explicit example of
 * "portrait", and it is also the specific edge tilt.c reports before
 * anything has been measured, so making the default disagree with his own
 * rule for that one edge would be a special case invented to preserve an
 * old default he did not ask to keep. The boot face is now upright; a
 * child who picks the clock up before the IMU has produced a reading still
 * sees a stable, sensible face (not a guess that flips under her), it is
 * simply the other one now.
 *
 * COASTING NEEDS NO BRANCH HERE. tilt.h's filter holds gx/gy and the
 * derived `up` at their last belief while the trust gate has fully given up
 * (the device is being carried) - `up` is hysteretic for precisely this -
 * so simply reading f->tilt.up every tick already gives "hold the last
 * layout" for free during a coasting episode. A beat-late flip once the
 * carry ends and a fresh reading lands is tilt.h's own filter lag, the same
 * one every other orientation-aware app on this device already lives with;
 * this app trying to do better on its own would be exactly the
 * written-twice seam tilt.h exists to prevent (see its header comment).
 *
 * !VALID IS THE CASE COASTING IS NOT. Coasting still has a belief, held
 * only slightly stale; !valid means no belief exists at all - either this
 * is the very first tick, before the IMU has produced a single sample, or
 * the IMU has gone quiet for TILT_STALE_MS. There, up/gx/gy are not merely
 * stale, they are not derived from anything, so reading them would not be
 * "the last known orientation" in the sense this app means it, it would be
 * whatever those fields happen to still contain. So on an invalid tilt this
 * function does not touch f->tilt at all: it returns whatever was last
 * painted (s->upright, s->flip180). app_alloc() zeroes a fresh
 * clock_state_t (app.h), so the very first call - before any tilt has ever
 * arrived - reads both as false: upright, unflipped, the same face the flat
 * default above settles on a moment later once the IMU's first sample
 * lands. A child who picks the clock up before the IMU has produced a
 * reading sees the same face she would a moment later either way.
 *
 * PICTURE, ROTATED, NOT A SECOND RENDERER. The "upside down" half of each
 * pair is drawn by painting the WHOLE face normally - the exact same
 * digits_draw_soft()/digits_draw_dots_soft() calls, at their normal
 * coordinates, unaware anything is flipped - and then reversing every pixel
 * of the finished panel end for end (panel_rotate_180() below: pixel i
 * swaps with pixel (PANEL_W*PANEL_H-1-i), which is exactly a 180 degree
 * rotation about the panel's own centre - see that function's comment for
 * why one array reversal is the whole proof). This is deliberate and is
 * the ONLY way this file draws a rotated face: digits.h is explicit that a
 * second, rotated copy of the seven-segment renderer is not an acceptable
 * fix for anything ("the owner has already had to ask twice for the same
 * glyph fix... a second seven-segment renderer in an app file is how that
 * happens a third time"), and a generic, digit-blind pixel reversal cannot
 * drift from digits.c's own rendering the way a hand-rolled rotated glyph
 * could. The cost of this choice is real and is paid on the STEADY-STATE
 * minute rollover, not on entry: while flipped, a changed digit repaints
 * the WHOLE face (paint_all(), rotated) rather than the one cell that
 * changed, because a single cell's pixels cannot be reversed in place
 * without first knowing where its flipped counterpart on the panel is -
 * see clock_tick()'s own comment on this trade. It stays a once-a-minute
 * (or once-per-tap, while dialling) cost, never a per-tick one, which is
 * the same bound this file already holds everywhere else. The pulsing
 * dots need NONE of this: their cell sits exactly on the panel's own
 * rotational centre (proven in "THE PULSE" - digits.h and this file's own
 * geometry put dots_cell() at (80,208,208,32) in panel space, whose centre
 * is (184,224), the panel's own (PANEL_W/2, PANEL_H/2)) and two identical
 * round dots are unchanged by swapping which one is "the upper one", so
 * the pulse's existing small, cheap, unconditional per-cell update is
 * already correct in every orientation and needed no change at all.
 * ========================================================================= */

typedef struct clock_state_s clock_state_t;

// Which face to draw, and whether it needs turning 180 - see "THE
// ORIENTATION SIGNAL" above for the full mapping and why it is the only
// function that reads f->tilt.
typedef struct {
    bool upright;
    bool flip180;
} clock_face_t;
static clock_face_t clock_face(clock_state_t *s, const app_frame_t *f);

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

// A lit numeral is black, set mode included: both fields are live at once
// (see "THE GESTURE" below), so there is no longer a "resting" pair to grey
// out the way an old, one-field-at-a-time drag needed.
#define INK_LIT     0

// Sentinel for "this cell holds a ghost, not a digit".
#define CELL_GHOST (-1)

// The dots' pulse, on a working face - see "THE PULSE" above. Reuses
// tables.c's own BLINK_PERIOD_MS value and name (a plain two-state blink,
// not a smooth ramp, same convention that file's wrong-answer reveal
// already uses), file-scoped here like every #define in this file, so
// there is no cross-file coupling to a translation unit that never sees
// this one.
#define BLINK_PERIOD_MS   500u

// ONE BLINK PER SECOND, which is what every clock a child has ever seen does.
// The first version toggled every BLINK_PERIOD_MS/2, four times a second, and
// the owner asked for it slower: "the clock blink should blink every second".
// Lit for half a second, faint for half a second, so the pair completes one
// blink per second. Halves the pulse's cost at the same time - see THE PULSE
// in this file's header for the arithmetic.
#define DOTS_PULSE_HALF_MS 500u
#define DOTS_PULSE_FAINT  130

// ---- the setting gesture's chevrons: four small marks, one per direction,
// flanking each digit pair while set mode is open -------------------------
//
// THE OWNER'S MOCKUP (a hand-drawn "<21>" over "<06>", chevrons well clear
// of the digits, near the bezel) settled two things that were open
// questions before it arrived:
//
//   - upright: the chevrons point LEFT and RIGHT, flanking each LINE's own
//     sides - not above/below as an earlier draft of this file had them.
//     "<" left of a line, ">" right of it, for both the hours and the
//     minutes.
//   - long-ways: the up/down chevrons stay - "en mode paysage decale les
//     fleches / mets plus d'ecart avec les chiffres, t'as de la place
//     verticale" (the owner's own words) - just moved further from the
//     digits, since the long-ways layout has 80px of unused margin above
//     and below the digit row to spend on exactly that.
//
// A chevron that POINTS somewhere is a promise about what tapping it does,
// which the OLD "whichever quarter you tap, tap-and-it-happens" rule did
// not have to keep (see chevron_zone() below for the direction change this
// forced).
//
// CHEV_T/CHEV_HALF_W are shared shape constants (arm thickness, arm span);
// every position below is checked against PANEL_BEZEL_MARGIN_PX by
// arithmetic, not by eye - see the LAYOUT section's own rule about that
// margin - and confirmed by the gate.
#define CHEV_HALF_W 13
#define CHEV_T      6
#define CHEV_H      9   // long-ways: base-to-apex distance (vertical)

// Upright: each chevron sits CHEV_P_GAP clear of its line's own left/right
// digit edge, CHEV_P_H further out from there to its point. P_X0=60 and
// P_X1+P_DIGIT_W=308 leave 60px of margin on both sides of the two-column
// block (368 wide total) to spend this in - comfortably inside that with
// room to spare, matching the mockup's own clearance rather than crowding
// the bezel.
#define CHEV_P_GAP  6
#define CHEV_P_H    9
#define CHEV_P_HOURS_CY   (P_Y_HOURS + P_DIGIT_H / 2)
#define CHEV_P_MIN_CY     (P_Y_MINUTES + P_DIGIT_H / 2)
#define CHEV_P_LEFT_BASE  (P_X0 - CHEV_P_GAP)
#define CHEV_P_LEFT_APEX  (CHEV_P_LEFT_BASE - CHEV_P_H)
#define CHEV_P_RIGHT_BASE (P_X1 + P_DIGIT_W + CHEV_P_GAP)
#define CHEV_P_RIGHT_APEX (CHEV_P_RIGHT_BASE + CHEV_P_H)

// Long-ways: both pairs sit in the same single row (y=[L_Y0,
// L_Y0+L_DIGIT_H)), so the up/down offsets are shared; only the x centre
// differs between the hours' pair and the minutes' (computed in
// draw_all_chevrons() from L_DIGIT_X, not duplicated here as a second set
// of literals). CHEV_L_GAP is deliberately far bigger than CHEV_P_GAP: the
// owner asked specifically for long-ways to open up, and L_Y0 (80) leaves
// 80px of paper above and below the digit row to do it in - 36 spends
// under half of that and still leaves clear margin against
// PANEL_BEZEL_MARGIN_PX.
#define CHEV_L_GAP 36
#define CHEV_L_UP_BASE (L_Y0 - CHEV_L_GAP)
#define CHEV_L_UP_APEX (CHEV_L_UP_BASE - CHEV_H)
#define CHEV_L_DN_BASE (L_Y0 + L_DIGIT_H + CHEV_L_GAP)
#define CHEV_L_DN_APEX (CHEV_L_DN_BASE + CHEV_H)

// How long, idle, set mode waits before giving up and returning to the
// running clock (or the empty face) without committing - see "IF SHE
// WANDERS OFF MID-SET" below.
#define SET_MODE_TIMEOUT_MS 60000u

// The double-press window: two KEY_SHORT verdicts (sensors.h) this close
// together are one gesture, not two taps of something else - see "THE
// WINDOW" below for why this cannot be confused with either of this
// button's other two gestures.
#define DOUBLE_PRESS_WINDOW_MS 500u

// How long a chevron tap ignores a further touchPressed edge after it fires
// - see "THE TAP TARGET IS DEBOUNCED" below. Chosen from a MEASUREMENT, not
// a guess: repro-touch-dropout-clock-set.ts drove the real dropout-heavy
// profile (34 episodes/sec, sensors_touch_next()'s own per-report loss) at
// a chevron zone and found a single physical tap under it firing several
// touchPressed edges, over-stepping the field every time; 250ms clears that
// while staying well under a fifth of a second, comfortably faster than a
// deliberate second tap needs to be.
#define TAP_COOLDOWN_MS 250u

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
 * THE GESTURE: double-press PWR to open it, tap the pair you want to change,
 * double-press PWR again to commit. This replaces an earlier hold-BOOT-and-
 * slide gesture (see this file's git history) - the owner's own words for
 * the new one, verbatim: "double press une fois et des chevrons apparaissent
 * pour choisir l'heure, double press pour valider".
 *
 * WHY PWR AND NOT BOOT. PWR is wired to the AXP2101 PMIC, which already
 * decodes press, release, short-press and long-press verdicts off the raw
 * pin (sensors.h) - a double-press is just two of those short-press
 * verdicts close together in time, nothing new for this app to detect. BOOT
 * is not on any chip at all: it is read by borrowing the flash chip select
 * (bootbtn.c), sampled at 20Hz, and already spoken for by two other
 * gestures (the BOOT+PWR menu chord, and the hold-and-slide this one
 * replaces). A double-press gesture is a natural fit for a button the PMIC
 * already timestamps; it would be an awkward one for a button that has to
 * disturb flash timing just to be read.
 *
 * THE WINDOW, AND WHY IT CANNOT BE CONFUSED WITH EITHER OF THIS BUTTON'S
 * OTHER TWO GESTURES. A double-press is two KEY_SHORT verdicts within
 * DOUBLE_PRESS_WINDOW_MS (500ms) of each other - nothing about the raw
 * press/release edges, only the verdict the PMIC itself already computes on
 * release. That is what keeps it clear of the other two things this same
 * button does, and neither margin is close:
 *
 *   - the BOOT+PWR menu chord (runtime_core.c) needs PWR held long enough
 *     for the PMIC's OWN long-press verdict, KEY_LONG at 1.5s. A press under
 *     that threshold can only ever produce KEY_SHORT, never KEY_LONG, so a
 *     double-press - two presses each necessarily well short of 1.5s, or the
 *     second one would have been a long press instead of a short one - is
 *     not a partial or slow attempt at the chord, it is a verdict the
 *     chord's own detector never even looks at;
 *   - the PWR-held-alone power-off gesture (runtime_core.c,
 *     poweroff_gesture_tick()) restarts its own hold timer on every fresh
 *     KEY_PRESS and only starts dimming the panel after
 *     PWR_HOLD_DIM_START_MS (800ms) of ONE continuous hold, cutting power at
 *     PWR_HOLD_POWEROFF_MS (2000ms). Two quick taps inside a 500ms window
 *     are each a small fraction of 800ms, and releasing between them resets
 *     that timer anyway - a double-press cannot be mistaken for the start of
 *     a power-off, and opening or closing set mode never dims the screen.
 *
 * 500ms is a plain double-tap window, the same order of magnitude a mouse or
 * a touchscreen already asks a hand for, generous enough that a small
 * child's slower fingers do not need a stopwatch, and - as shown above - an
 * order of magnitude clear of both other thresholds this same button
 * carries.
 *
 * THE FIRST DOUBLE-PRESS opens set mode, and chevrons appear on BOTH the
 * hours and the minutes AT ONCE - not one pair with a further gesture to
 * move onto the other, because the owner asked for exactly two
 * double-presses total, open and commit, with nothing in between (an
 * earlier draft of this feature had a middle double-press to move a cursor
 * from the hours to the minutes; the owner cut it, in his own words: "on
 * refait double press pour valider les réglages" - two, not three). So "how
 * does she reach both fields with only one opening gesture" has to be
 * answered by WHERE she taps, not by a mode step, and this app already had
 * the rule: the hold-and-slide gesture this replaces picked its pair the
 * same way - "the axis that SEPARATES the two pairs chooses which pair; the
 * other axis carries the value" - and that sentence survives the redesign
 * unchanged in BOTH halves, not just the first: held upright the hours sit
 * above the minutes, so Y still separates them, and now X (left/right)
 * carries the direction instead of a slide's absolute value; held long-ways
 * the hours are the left pair and the minutes the right, so X still
 * separates them, and Y (up/down) carries the direction, same as it always
 * did here. Nothing to learn twice, in either the OLD gesture or between
 * this app's own two layouts.
 *
 * THE CHEVRONS, per the owner's own mockup (a hand-drawn "<21>" over "<06>"):
 * upright, a "<" sits left of each line and a ">" right of it - tap the left
 * side of a line to decrease it, the right side to increase; long-ways, the
 * "^"/"v" pair stays above and below each pair as it always did, moved
 * further from the digits than an earlier draft had them ("mets plus
 * d'ecart avec les chiffres, t'as de la place verticale", the owner's own
 * words) - tap above to increase, below to decrease. A drawn arrow is a
 * promise about what tapping it does; the OLD "whichever quarter, one
 * direction" rule was fine when nothing on screen implied a direction, and
 * stopped being fine the moment arrows appeared, so the direction follows
 * the arrow exactly, both ways.
 *
 * WHAT SAYS THE OTHER FIELD IS ALSO LIVE: the other field's own chevrons,
 * on screen at the same time, in the same ink, from the moment set mode
 * opens. There is no hidden mode to discover by trial and error the way a
 * numpad's missing check key once was (tables.c's own history records a
 * child who "attendait" at a key that was never coming); a glance at the
 * screen is the whole instruction, because nothing about it changes once
 * she starts tapping. Every tap only ever changes the field its zone
 * belongs to - there is no field latch to get wrong, because there is no
 * drag left to latch anything against; each tap is independent and takes
 * effect immediately.
 *
 * THE TAP TARGET is NOT the chevron ink. Each chevron mark is a few dozen
 * pixels, far smaller than the ~100px an adult fingertip or the ~75px a
 * child's covers (AGENTS.md's "a finger is about 100 pixels wide" section),
 * so the actual target is the whole quarter of the glass the axis rule above
 * assigns it: upright, the panel's own four quarters (top/bottom halves
 * split again into left/right) are hours-decrease, hours-increase,
 * minutes-decrease, minutes-increase; long-ways, each pair gets the full
 * half of the panel nearer it, split again into an upper and a lower half
 * for the two directions. Every zone is well over 100px in both dimensions,
 * and together they cover the ENTIRE glass - there is no dead area to miss,
 * which is the more forgiving of the two options an earlier draft of this
 * task posed (tap the chevrons themselves, or tap anywhere in their own
 * quarter of the glass) and the one every other touch target on this device
 * already prefers over a small, precise one (AGENTS.md's "a finger also
 * HIDES what it is choosing" section; four.c and morpion.c made the same
 * choice already, for the same reason).
 *
 * THE SECOND DOUBLE-PRESS commits, exactly the way releasing BOOT used to:
 * one sensors_set_clock() call, seconds zeroed so the owner can line it up
 * against a wall clock's minute and let go.
 *
 * IF SHE WANDERS OFF MID-SET: SET_MODE_TIMEOUT_MS (60s) of no tap and no
 * double-press returns silently to the running clock (or the empty face, if
 * it was still unknown when set mode opened), discarding whatever was
 * dialled in. Nothing reaches the RTC unless the second double-press
 * actually lands - a half-dialled value is never committed by a timeout,
 * only by a deliberate second press - and a set screen left open forever is
 * two real costs on this hardware: a static image on an AMOLED panel with a
 * documented burn-in concern (AGENTS.md), and a toy stuck in a mode a child
 * cannot get herself out of. 60 seconds is generous enough to tap a value in
 * one step at a time, worst case dialling minutes end to end, without
 * feeling like it is racing her.
 *
 * REACHABLE FROM, AND SETTABLE OUT OF, THE EMPTY FACE. The double-press does
 * not care what sensors_clock() currently says: an unset clock still
 * receives KEY_SHORT verdicts from the PMIC exactly like a set one, so the
 * gesture opens from the ghost face the same way it opens from a working
 * one - it has to, since a flat-battery clock is the one case this whole
 * feature exists for. Entering set mode from unknown seeds at 12:00, same
 * reasoning as before: a neutral middle of the range rather than midnight,
 * where half the useful range would be a long way of tapping away.
 * Committing clears the RTC's OS flag (sensors_set_clock()'s own contract),
 * so the face that appears afterwards is a real time, not the ghost.
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
 * gesture's whole feedback is the numbers changing under a finger. The
 * seconds are set to zero on commit, so setting it at the top of a minute is
 * exact; that is the only accuracy USB would have bought.
 * ========================================================================= */

#define FIELD_HOURS   0
#define FIELD_MINUTES 1

struct clock_state_s {
    // What is currently on the panel, so a tick only ever redraws a cell
    // whose content actually changed. Both halves matter: the same digit in
    // a different ink (the empty face breathing, or the dots pulsing) is a
    // change, and a different digit in the same ink obviously is.
    int     cellDigit[4];
    uint8_t cellInk[4];
    bool    painted;   // has a full face been drawn at least once
    bool    upright;   // ...and in which layout
    bool    flip180;   // ...and whether that layout was painted turned 180 -
                        // see "THE ORIENTATION SIGNAL" above; doubles as both
                        // the !valid fallback and the "did this change"
                        // comparison, the same way `upright` already does
    bool    paintedActive; // ...and whether that paint included the
                            // chevrons - see paint_all()'s `active` param

    // The setting gesture. `active` spans the whole open-to-commit window,
    // from the first double-press to the second - see "THE GESTURE" above.
    // Both fields are live throughout, so there is no per-field "which one
    // is currently being touched" left to track.
    bool active;
    int  setH, setM;
    uint32_t lastActivityMs; // last tap or double-press while active, for
                              // the abandonment timeout (SET_MODE_TIMEOUT_MS)
    uint32_t lastTapMs;      // when the last chevron tap was ACCEPTED - see
                              // TAP_COOLDOWN_MS and "THE TAP TARGET IS
                              // DEBOUNCED" below

    // The double-press detector, shared by "open" and "commit" (see
    // pwr_double_press()): the first KEY_SHORT verdict of a pair is parked
    // here until either a second one lands within DOUBLE_PRESS_WINDOW_MS
    // (a double-press) or the window closes on its own (a single tap that
    // does nothing - this app has no use for a lone short press).
    bool     havePendingShort;
    uint32_t pendingShortMs;

    // Committed, and waiting for the value to come back out the other side.
    // THIS IS A HARDWARE-ONLY GLITCH GUARD and the emulator cannot show what
    // it prevents: on the board, sensors_set_clock() is a core0 REQUEST that
    // core1 performs (sensors.h), and performing it is three bounded i2c
    // transactions, so the published clock still reads "not known" for a
    // millisecond or two after the commit double-press lands. Without this,
    // committing would flash the empty face for a frame before the time
    // appeared - exactly at the moment the owner is looking to see whether
    // it worked. In wasm the shim's set is synchronous, so this branch is
    // never taken there and no test can catch its absence.
    bool pendingSet;
    int  pendingH, pendingM;

    // The separator dots are part of the face, not furniture: they carry
    // whatever ink the numerals do, so the empty face is empty all the way
    // through instead of being four ghosts with one bold black mark between
    // them. Remembered separately because they are their own cell.
    uint8_t dotsInk;

    // Log throttling: the firmware log is how the tests and the owner read
    // this app's mind, and a line per frame would drown it.
    int lastLoggedH, lastLoggedM;
    bool loggedUnknown;
};

static clock_state_t *s_state;

// THE ONE PLACE THE ORIENTATION SIGNAL ENTERS THIS APP. See "THE
// ORIENTATION SIGNAL" at the top of this file for the full reasoning on the
// mapping, on which edge of each pair is flipped, on coasting, and on
// !valid; the short version, MEASURED against the real emulator (twice now
// - see "WHICH WAY IS UP, PRECISELY" below for what was wrong the first
// time) rather than reasoned out on paper:
//
//     land.up   family      flipped?
//     LEFT      portrait    no  (the flat, unmeasured default)
//     RIGHT     portrait    yes
//     BOTTOM    long-ways   no  (buttons toward the top edge)
//     TOP       long-ways   yes
//
// and an invalid reading holds whatever is already on screen. Note the
// asymmetry: portrait's UNFLIPPED edge is LEFT, long-ways's UNFLIPPED edge
// is BOTTOM - two DIFFERENT names, not "whichever edge is first
// alphabetically" or some other pattern that would make one formula cover
// both without checking two distinct values.
static clock_face_t clock_face(clock_state_t *s, const app_frame_t *f) {
    clock_face_t out;
    if (!f->tilt.valid) {
        out.upright = s->upright;
        out.flip180 = s->flip180;
        return out;
    }
    uint8_t up = f->tilt.up;
    out.upright = (up == TILT_UP_LEFT || up == TILT_UP_RIGHT);
    out.flip180 = (up == TILT_UP_RIGHT || up == TILT_UP_TOP);
    return out;
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

/* ---- the chevrons --------------------------------------------------------
 *
 * Four small "^"/"v" marks, drawn only while set mode is open - see "THE
 * GESTURE" above for what they mean and why every tap on the glass, not
 * just a tap on the ink itself, is the real target.
 */

// One capsule of a chevron's arm, in whichever space this face draws in -
// the same portrait/landscape isometry digits.c's own soft_capsule uses
// (panel (x,y) -> landscape (y, PANEL_W-x), see that function's comment for
// the derivation), duplicated here rather than exported from digits.h: a
// chevron is UI chrome, not a numeral, and digits.h's "these must not fork"
// argument is specifically about the seven-segment glyphs it renders.
static void chevron_capsule(digits_space_t space, float x0, float y0,
                            float x1, float y1, float r, uint16_t colorPx) {
    if (space == DIGITS_LANDSCAPE) {
        shapes_fill_capsule_aa_land(x0, y0, r, x1, y1, r, colorPx);
    } else {
        float edge = (float)PANEL_W;
        shapes_fill_capsule_aa_land(y0, edge - x0, r, y1, edge - x1, r, colorPx);
    }
}

// Two capsules meeting at (cx, apexY), base spanning cx-halfW..cx+halfW at
// baseY. apexY above baseY draws an up chevron (increase), apexY below
// baseY a down chevron (decrease) - the sign of apexY-baseY says which, so
// no caller needs to say it twice. Used for the long-ways face's up/down
// marks.
static void draw_chevron(digits_space_t space, int cx, int apexY, int baseY,
                         int halfW, int t, uint16_t colorPx) {
    float r = (float)t * 0.5f;
    chevron_capsule(space, (float)(cx - halfW), (float)baseY, (float)cx, (float)apexY, r, colorPx);
    chevron_capsule(space, (float)cx, (float)apexY, (float)(cx + halfW), (float)baseY, r, colorPx);
}

// The same shape, turned 90 degrees: two capsules meeting at (apexX, cy),
// base spanning cy-halfH..cy+halfH at baseX. apexX left of baseX draws a
// "<" (decrease, per the owner's mockup); apexX right of baseX draws a ">"
// (increase). Used for the upright face's left/right marks - see "THE
// OWNER'S MOCKUP" above for why upright points sideways while long-ways
// still points up/down.
static void draw_chevron_h(digits_space_t space, int cy, int apexX, int baseX,
                           int halfH, int t, uint16_t colorPx) {
    float r = (float)t * 0.5f;
    chevron_capsule(space, (float)baseX, (float)(cy - halfH), (float)apexX, (float)cy, r, colorPx);
    chevron_capsule(space, (float)apexX, (float)cy, (float)baseX, (float)(cy + halfH), r, colorPx);
}

// All four, for whichever layout is showing. Positions come from the
// CHEV_* macros above (LAYOUT-adjacent, checked against
// PANEL_BEZEL_MARGIN_PX by arithmetic there); only the landscape x centres
// are computed here, from L_DIGIT_X, rather than duplicated as a second set
// of literals.
static void draw_all_chevrons(bool upright) {
    uint16_t px = gray_to_px(INK_LIT);
    if (upright) {
        // "<" left of each line (decrease), ">" right of it (increase) -
        // the owner's own mockup. Both lines share the same left/right X
        // positions, since both columns line up between the hours and the
        // minutes; only the Y centre differs.
        draw_chevron_h(DIGITS_PORTRAIT, CHEV_P_HOURS_CY, CHEV_P_LEFT_APEX,  CHEV_P_LEFT_BASE,  CHEV_HALF_W, CHEV_T, px);
        draw_chevron_h(DIGITS_PORTRAIT, CHEV_P_HOURS_CY, CHEV_P_RIGHT_APEX, CHEV_P_RIGHT_BASE, CHEV_HALF_W, CHEV_T, px);
        draw_chevron_h(DIGITS_PORTRAIT, CHEV_P_MIN_CY,   CHEV_P_LEFT_APEX,  CHEV_P_LEFT_BASE,  CHEV_HALF_W, CHEV_T, px);
        draw_chevron_h(DIGITS_PORTRAIT, CHEV_P_MIN_CY,   CHEV_P_RIGHT_APEX, CHEV_P_RIGHT_BASE, CHEV_HALF_W, CHEV_T, px);
    } else {
        int cxHours   = (L_DIGIT_X[0] + L_DIGIT_X[1] + L_DIGIT_W) / 2;
        int cxMinutes = (L_DIGIT_X[2] + L_DIGIT_X[3] + L_DIGIT_W) / 2;
        draw_chevron(DIGITS_LANDSCAPE, cxHours,   CHEV_L_UP_APEX, CHEV_L_UP_BASE, CHEV_HALF_W, CHEV_T, px);
        draw_chevron(DIGITS_LANDSCAPE, cxHours,   CHEV_L_DN_APEX, CHEV_L_DN_BASE, CHEV_HALF_W, CHEV_T, px);
        draw_chevron(DIGITS_LANDSCAPE, cxMinutes, CHEV_L_UP_APEX, CHEV_L_UP_BASE, CHEV_HALF_W, CHEV_T, px);
        draw_chevron(DIGITS_LANDSCAPE, cxMinutes, CHEV_L_DN_APEX, CHEV_L_DN_BASE, CHEV_HALF_W, CHEV_T, px);
    }
}

// The dots' pulse - see "THE PULSE" above. A plain two-state blink on
// BLINK_PERIOD_MS, not a smooth ramp: cheap (one comparison, no table) and
// unmistakably distinct from the empty face's slow six-step breathe, which
// is the point - a pulsing clock and a breathing ghost must not read as the
// same animation wearing two colours.
static uint8_t dots_pulse_ink(uint32_t nowMs) {
    return ((nowMs / DOTS_PULSE_HALF_MS) % 2u) ? DOTS_PULSE_FAINT : INK_LIT;
}

// Turns the WHOLE panel 180 degrees in place - see "PICTURE, ROTATED, NOT
// A SECOND RENDERER" above for why this exists and why it is deliberately
// generic. gfx_fb is one contiguous row-major array of PANEL_W*PANEL_H
// pixels, so a point reflection about the panel's own centre - (x,y) ->
// (PANEL_W-1-x, PANEL_H-1-y) - is exactly index i -> (PANEL_W*PANEL_H-1-i):
// (PANEL_H-1-y)*PANEL_W + (PANEL_W-1-x) = PANEL_W*PANEL_H - 1 - (y*PANEL_W+x).
// That collapses a 2D rotation into a single linear array reversal, which
// is the entire function.
static void panel_rotate_180(void) {
    int n = PANEL_W * PANEL_H;
    for (int i = 0; i < n / 2; i++) {
        uint16_t tmp = gfx_fb[i];
        gfx_fb[i] = gfx_fb[n - 1 - i];
        gfx_fb[n - 1 - i] = tmp;
    }
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

    // Both fields are live at once while set mode is open (see "THE
    // GESTURE"), so there is no "resting" pair to grey out any more - the
    // chevrons are what says a field is editable, not the ink colour.
    ink[0] = ink[1] = ink[2] = ink[3] = INK_LIT;
    return true;
}

/* ---- painting ----------------------------------------------------------- */

// A whole face, from white. Used on enter(), whenever the puck is turned
// over or turned upside down, whenever set mode opens or closes, and (while
// flipped) whenever a digit changes - every one of those is a moment where
// more than a single cell's worth of the panel changes at once, or where
// PICTURE, ROTATED above needs the whole assembled face to reverse
// correctly. Always draws the face NORMALLY, unflipped, digits.c's own
// calls untouched; `flip180` is applied once, at the very end, as a single
// whole-panel pixel reversal - see panel_rotate_180() and this file's own
// "THE ORIENTATION SIGNAL" section for why that is the only place a flip
// ever happens.
static void paint_all(clock_state_t *s, bool upright, bool flip180, bool active,
                      const int digits[4], const uint8_t ink[4],
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

    if (active) draw_all_chevrons(upright);

    if (flip180) panel_rotate_180();

    s->upright = upright;
    s->flip180 = flip180;
    s->painted = true;
    s->paintedActive = active;

    // enter() must not push (app.h: the runtime pushes the whole panel once
    // after it returns); a turn-over or a set-mode transition must, and the
    // whole panel is exactly what changed in both cases.
    if (push) gfx_push_all();
}

/* ---- the setting gesture ------------------------------------------------ */

// Two KEY_SHORT verdicts within DOUBLE_PRESS_WINDOW_MS of each other is a
// double-press; one on its own, with the window closing before a second
// arrives, is a tap this app has no use for and simply forgets. Shared by
// both halves of the gesture (opening set mode and committing it) - see
// "THE GESTURE" above for why this cannot be confused with the menu chord
// or the PWR-held-alone power-off gesture.
static bool pwr_double_press(clock_state_t *s, uint32_t nowMs, uint8_t key) {
    if (!(key & KEY_SHORT)) return false;
    if (s->havePendingShort && (nowMs - s->pendingShortMs) <= DOUBLE_PRESS_WINDOW_MS) {
        s->havePendingShort = false;
        return true;
    }
    s->havePendingShort = true;
    s->pendingShortMs = nowMs;
    return false;
}

// Which field a tap at (touchX, touchY) adjusts, and which way - see "THE
// TAP TARGET" above, and "THE OWNER'S MOCKUP" by the CHEV_* constants for
// what the chevrons now promise: a left arrow decreases, a right arrow
// increases (upright); an up arrow increases, a down arrow decreases
// (long-ways). Both layouts follow the same rule this file's own header
// already states for the gesture this one replaced - "the axis that
// SEPARATES the two pairs chooses which pair you are setting; the other
// axis carries the value" - checked against that header rather than
// invented fresh: upright already used Y to separate hours from minutes
// and X to carry the value, long-ways already used the opposite, and both
// still do here, direction included. Every point on the glass lands in
// exactly one of the four zones; there is no miss case, deliberately,
// because a target a child can fail to hit is worse than one that is
// occasionally the coarser of two reasonable choices.
static void chevron_zone(bool upright, int touchX, int touchY, int *field, int *dir) {
    if (upright) {
        // Y separates the pair (top half hours, bottom half minutes, same
        // as the digits themselves are laid out); X carries the direction
        // (left half decreases, right half increases, matching "<" left of
        // a line and ">" right of it). Y is unscoped within its half and X
        // within its half - the full panel, quartered into four HALF-height,
        // HALF-width zones, each comfortably over 100px in both dimensions.
        *field = (touchY < PANEL_H / 2) ? FIELD_HOURS : FIELD_MINUTES;
        *dir = (touchX < PANEL_W / 2) ? -1 : +1;
    } else {
        // Panel coordinates map to landscape as (lx, ly) = (touchY,
        // PANEL_W-1-touchX) - gfx.h's own mapping, inverted, the same
        // conversion the old positional gesture used. lx separates the pair
        // (left half hours, right half minutes); ly carries the direction
        // (smaller ly - toward the digit row's own top edge, where the "^"
        // sits - increases; larger ly, where the "v" sits, decreases). lx
        // and ly are independent axes here, so each pair gets the FULL
        // height for its own up/down split rather than sharing one axis for
        // both jobs the way the upright layout has to.
        int lx = touchY;
        int ly = PANEL_W - 1 - touchX;
        *field = (lx < LAND_W / 2) ? FIELD_HOURS : FIELD_MINUTES;
        *dir = (ly < LAND_H / 2) ? +1 : -1;
    }
}

// `upright`/`flip180` are passed in rather than read back off s->upright/
// s->flip180, which is the layout currently PAINTED and is one frame stale
// on the tick the puck is turned over or turned upside down - a gesture in
// flight across that instant would otherwise pick its field with the wrong
// rule for exactly one frame.
static void setting_tick(clock_state_t *s, const app_frame_t *f, bool upright, bool flip180) {
    if (pwr_double_press(s, f->nowMs, f->key)) {
        if (!s->active) {
            s->active = true;
            // Seed from whatever is on screen. An unknown clock seeds at
            // 12:00, a neutral place to start tapping from rather than
            // midnight, where half the useful range is a long way of
            // tapping away.
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
            s->lastActivityMs = f->nowMs;
            // See TAP_COOLDOWN_MS below: subtracting it (unsigned wrap is
            // harmless here, it only ever makes the very next check MORE
            // permissive) means the first tap after opening is never itself
            // blocked by a cooldown left over from nothing.
            s->lastTapMs = f->nowMs - TAP_COOLDOWN_MS;
            printf("clock: set mode opened, seeded at %02d:%02d\r\n", s->setH, s->setM);
        } else {
            s->active = false;
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
        // The double-press that just opened or closed set mode is not also
        // a tap: nothing else this tick touches setH/setM or the timeout.
        return;
    }

    if (!s->active) return;

    // THE TAP TARGET IS DEBOUNCED. touchPressed is a raw single-tick edge
    // (runtime_core.c: down && !wasDown, resolved fresh from whatever the
    // touch queue drained THIS tick), with no arming delay of its own -
    // unlike dino.c's jump or four.c's drop, which both require several
    // consistent samples before accepting a tap as real. Measured by
    // repro-touch-dropout-clock-set.ts: under the controller's real dropout
    // rate (34 episodes/sec, sensors_touch_next()'s own per-report loss), a
    // single physical tap-and-release can make `down` flicker across
    // several ticks, and every flicker back to true is a FRESH touchPressed
    // edge - one physical tap fired many steps, every time, before this
    // guard existed. So a tap only counts if it lands at least
    // TAP_COOLDOWN_MS after the last one that did; see that constant's own
    // comment for where 250ms comes from.
    if (f->touchPressed && (f->nowMs - s->lastTapMs) >= TAP_COOLDOWN_MS) {
        // Touch coordinates always arrive in real PANEL space, unrotated by
        // anything this app draws (app.h) - the controller reports where a
        // finger actually is, not where the picture happens to be. So a
        // flipped face needs its touch point point-reflected the same way
        // the picture itself was, BEFORE chevron_zone's own axis logic runs,
        // or a tap in what the child sees as "the hours' up zone" would be
        // read against the unrotated layout and hit the wrong quarter.
        int tx = flip180 ? PANEL_W - 1 - f->touchX : f->touchX;
        int ty = flip180 ? PANEL_H - 1 - f->touchY : f->touchY;
        int field, dir;
        chevron_zone(upright, tx, ty, &field, &dir);
        if (field == FIELD_HOURS) s->setH = (s->setH + 24 + dir) % 24;
        else s->setM = (s->setM + 60 + dir) % 60;
        s->lastActivityMs = f->nowMs;
        s->lastTapMs = f->nowMs;
    }

    // Abandonment: see "IF SHE WANDERS OFF MID-SET" above. Discards
    // silently - nothing reaches the RTC unless the commit double-press
    // actually lands.
    if (f->nowMs - s->lastActivityMs > SET_MODE_TIMEOUT_MS) {
        s->active = false;
        printf("clock: set mode abandoned after %us idle, discarding\r\n",
               SET_MODE_TIMEOUT_MS / 1000u);
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

static void clock_enter(void) {
    s_state = APP_STATE(clock_state_t);
    s_state->lastLoggedH = -1;
    s_state->lastLoggedM = -1;

    app_frame_t first = { 0 };
    clock_face_t face = clock_face(s_state, &first);
    int digits[4];
    uint8_t ink[4];
    bool known = face_now(s_state, &first, digits, ink);
    paint_all(s_state, face.upright, face.flip180, false, digits, ink,
             known ? dots_pulse_ink(0) : ink[0], false);

    printf("clock: state=%d bytes (arena %d)\r\n", (int)sizeof(clock_state_t), APP_ARENA_BYTES);
}

static void clock_tick(const app_frame_t *f) {
    clock_state_t *s = s_state;

    clock_face_t face = clock_face(s, f);
    bool upright = face.upright, flip180 = face.flip180;
    setting_tick(s, f, upright, flip180);

    int digits[4];
    uint8_t ink[4];
    bool known = face_now(s, f, digits, ink);
    // The dots pulse (see "THE PULSE") when there is a time to show, and
    // breathe with the ghosts when there is not - either way they are
    // redrawn by the same "did this cell's ink change" test below.
    uint8_t dotsInk = known ? dots_pulse_ink(f->nowMs) : ink[0];

    bool modeChanged = (s->active != s->paintedActive);
    bool layoutChanged = (upright != s->upright) || (flip180 != s->flip180);

    if (modeChanged || layoutChanged || !s->painted) {
        if (modeChanged) {
            printf("clock: set mode %s\r\n", s->active ? "opened" : "closed");
        } else if (upright != s->upright) {
            printf("clock: layout %s\r\n", upright ? "upright" : "long-ways");
        } else {
            printf("clock: turned %s\r\n", flip180 ? "upside down" : "right side up");
        }
        paint_all(s, upright, flip180, s->active, digits, ink, dotsInk, true);
    } else {
        bool digitsChanged = false;
        for (int i = 0; i < 4; i++) {
            if (digits[i] != s->cellDigit[i] || ink[i] != s->cellInk[i]) { digitsChanged = true; break; }
        }
        if (flip180 && digitsChanged) {
            // See "PICTURE, ROTATED" above: a single cell's pixels cannot be
            // reversed in place without a second, digit-specific coordinate
            // transform this file deliberately does not build (that is the
            // "second seven-segment renderer" digits.h forbids, one step
            // removed). A full repaint costs more here, but it is still
            // bounded to the same rare events - once a minute, or once per
            // tap while dialling - not a per-tick cost.
            paint_all(s, upright, flip180, s->active, digits, ink, dotsInk, true);
        } else {
            // The steady state: at most one cell changes, once a minute, and
            // on most frames this loop does nothing at all.
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
        }
        // The dots' pulse never needs flip handling (see "PICTURE, ROTATED"
        // above: their cell sits exactly on the panel's own rotational
        // centre and two round dots are unchanged by swapping which is
        // "upper"), so this runs unconditionally, flipped or not, and does
        // nothing extra on a tick that just did a full repaint above (that
        // repaint already drew the dots at dotsInk and recorded it).
        if (!upright && dotsInk != s->dotsInk && !(flip180 && digitsChanged)) {
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

// landscape = true because the long-ways face is the one the owner described
// first ("un display horizontal qui marche le chronometre") and the one
// every landscape app on this device already assumes for its own gravity
// rotation (tilt_for_app, "THE ORIENTATION SIGNAL" above) - it is NOT a
// claim about which face is showing right now (that flipped to upright by
// default once "THE ORIENTATION SIGNAL"'s new mapping landed; see its own
// comment on why). This is a static flag and this app is not statically
// oriented, which the runtime only uses to decide which way to draw the menu
// overlay on top of it (app.h); when the shared orientation signal lands,
// whether that flag should follow it is the runtime's question rather than
// this file's.
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
