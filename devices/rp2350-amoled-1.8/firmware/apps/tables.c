/*
 * tables: multiplication-tables practice, the 6/7/8 tables mixed, for a
 * niece who reads digits fluently and is drilling them.
 *
 * WHICH HOUSE CONVENTIONS THIS APP KEEPS, AND WHICH IT DROPS - read this
 * before treating this file as a precedent for any other app.
 *
 * Every other app on this device is built for a two-year-old who cannot
 * read: no text, no digits, pictures only (see AGENTS.md's header and
 * decision 0013's "she cannot read a page number"). This app is for a
 * different child, one who is LEARNING her times tables, so digits are not
 * a barrier here, they are the entire point. Kept:
 *
 *   - decision 0001, the 8-pixel push rule (gfx enforces it regardless);
 *   - decision 0009, nothing drawn with a ruler where a curve or a round
 *     cap can carry it instead - every icon in this file (the checkmark,
 *     the backspace chevron, the multiply mark, the three counter glyphs,
 *     the numpad digits themselves) is built from shapes.h's float
 *     primitives (capsule/disc/annulus), the same brush every other app's
 *     ink uses, never a filled rectangle standing in for a shape;
 *   - the press-drag-release idiom (menu.c, four.c, morpion.c) and its
 *     arm/release-grace filtering against the FT3168's measured jitter -
 *     see "THE GESTURE" below for what is reused verbatim and what is
 *     re-derived;
 *   - PANEL_BEZEL_MARGIN_PX: every element in this layout is inset from
 *     all four physical panel edges, same as everywhere else.
 *
 * Dropped, deliberately, and only here:
 *
 *   - "no text, pictures only". The question, the answer she types and the
 *     three counters are digits on purpose - she reads them, and a picture
 *     of "6" is worse than the numeral 6 for a child drilling numerals.
 *     Two counter labels (attempted/correct/time) are told apart by SHAPE
 *     (a ring, a filled disc, a clock face) rather than by a word, which is
 *     the one place this file still reaches for the wordless-icon
 *     convention instead of text - see COUNTER ICONS below for why.
 *   - decision 0002 section 4b's "no chrome inside an app" is not violated
 *     (nothing here is navigation chrome), but this app is the first to
 *     put a live floating overlay - the loupe - on screen, which no other
 *     app needed before it had a small-target input to correct.
 *
 * ==========================================================================
 * THE INPUT DESIGN, and why it changed mid-build.
 *
 * The obvious first design was a numpad sized the way menu.c's grid is: a
 * child's fingertip contact measures ~75-100px on this panel (AGENTS.md),
 * so twelve keys (0-9, backspace, submit) at a size a thumb can hit
 * reliably would have consumed nearly the whole 448x368 glass, leaving
 * little room for the question or the counters, and would still have
 * inherited the FT3168's measured 80-250px centroid jitter on every
 * boundary between two adjacent keys - the worst case named in AGENTS.md's
 * own numpad warning.
 *
 * The owner proposed the actual fix, in his own words: "un pavé numérique
 * a la clavier iPhone ou elle peut rester enfoncee sur son doigt pour
 * selectionner et ca fait une loupe" - an iOS-style numpad where holding a
 * finger down opens a magnifier showing which key is under it, she slides
 * to correct without lifting, and it commits on release. This dissolves
 * the arithmetic problem rather than solving it: a key no longer has to be
 * bigger than a fingertip, because she is not required to land on one
 * precisely. She lands roughly, reads the loupe, slides if it is wrong,
 * lifts when it is right. That is what freed the numpad keys down to
 * 99x64 (see THE LOUPE and THE NUMPAD below) - small enough to leave real
 * room for the question and the three counters, legible rather than
 * finger-sized, because legibility is what a shrunk target can now be
 * judged on instead of hit-rate.
 *
 * It is also this device's own idiom rather than a thirteenth new gesture:
 * press-drag-release, with the candidate lit under the thumb before
 * anything commits, is exactly what the palette, Connect Four's column aim
 * and the menu already do. The loupe is that same gesture with a
 * magnifier drawn on top, because a numpad is the one place on this device
 * where "which target is my thumb over" cannot be answered by lighting up
 * a whole column and a whole row the way morpion's cross does (twelve
 * targets in a tight grid, not one cell on a 3x3 board) - the child needs
 * to see the actual character, not just a highlighted region.
 *
 * TWO DETAILS THAT DECIDE WHETHER IT FEELS RIGHT, both from the owner's
 * own framing of the iOS keyboard:
 *
 *   - the loupe must sit above the finger, offset, never under it, or it
 *     shows her nothing. iOS solves this by floating the bubble above the
 *     touch point, which breaks down at the top of a keyboard where there
 *     is no room left above. THIS FILE'S ANSWER: the loupe's height is NOT
 *     dynamic at all. It sits in a fixed-height band reserved above the
 *     numpad (LOUPE_ZONE_H, see THE LOUPE below) and only its horizontal
 *     position tracks the finger. That is a stronger fix than "give the
 *     top row extra headroom": it removes the top-row special case
 *     entirely, because the loupe is never vertically near ANY row it
 *     could run out of room above, and it collapses the redraw problem
 *     from an arbitrary floating rectangle (which could overlap numpad
 *     keys anywhere on the pad, the same erase/redraw-neighbours cost
 *     menu.c's halo comment describes) down to one fixed-height horizontal
 *     band that never overlaps a key at all, because nothing but the
 *     loupe is ever drawn there. See THE LOUPE's own comment for the
 *     redraw cost this buys.
 *   - releasing outside the pad must cancel, not commit - the way she
 *     corrects a wrong press without a delete key, and the same rule the
 *     menu already enforces by name for its own cancel band ("nothing
 *     hidden", decision 0013). See THE GESTURE below for how the numpad's
 *     own cancel region is defined.
 *
 * WHAT COMMITS IS WHAT THE LOUPE SHOWED. Same invariant menu.c states for
 * its own halo ("what launches is always what was lit"), carried over
 * unchanged: the release verdict reads the identical `hoverCell` the loupe
 * was just drawn from, so the device can never disagree with her about
 * which key she was looking at when she let go.
 * ==========================================================================
 */
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"
#include "shapes.h"

/* =========================================================================
 * LAYOUT. Landscape 448x368. Everything below is inset from all four
 * physical panel edges by PANEL_BEZEL_MARGIN_PX (10px), because in
 * landscape space every one of the four edges (x=0, x=LAND_W, y=0,
 * y=LAND_H) is a real panel edge the case hides part of - not just two of
 * them, the way a portrait app's own top/bottom might read. The usable
 * canvas is therefore 428x348, and the numpad's own outer edges (its left
 * column at OX, its bottom row at OY+USABLE_H) sit exactly on that
 * boundary rather than on the raw panel edge.
 * ========================================================================= */
#define OX PANEL_BEZEL_MARGIN_PX
#define OY PANEL_BEZEL_MARGIN_PX
#define USABLE_W (LAND_W - 2 * PANEL_BEZEL_MARGIN_PX) // 428
#define USABLE_H (LAND_H - 2 * PANEL_BEZEL_MARGIN_PX) // 348

/* ---- THE NUMPAD ---------------------------------------------------------
 *
 * 3 columns x 4 rows, phone-dial order (1 2 3 / 4 5 6 / 7 8 9 / back 0
 * check), 12 cells: with the loupe doing the precision work (see this
 * file's header), a cell only has to be roughly aimable and legibly
 * labelled, not fingertip-sized. 99 x 64 is comfortably bigger than an
 * iOS key's own physical size on a phone and small enough to leave real
 * room for the question and the counters - see the header's "input
 * design" section for why that tradeoff is available at all here and
 * was not on the menu's own grid (decision 0013), which has no loupe and
 * therefore still has to pay in target size for every miss it forgives.
 */
#define NUMPAD_COLS 3
#define NUMPAD_ROWS 4
#define CELL_W 99
#define CELL_H 64
#define NUMPAD_W (CELL_W * NUMPAD_COLS)          // 297
#define LOUPE_ZONE_H 92
#define NUMPAD_H (CELL_H * NUMPAD_ROWS)          // 256; 92+256 = 348 = USABLE_H exactly
#define NUMPAD_X0 OX                              // 10
#define NUMPAD_Y0 (OY + LOUPE_ZONE_H)             // 102

#define CELL_BACK  9
#define CELL_ZERO  10
#define CELL_CHECK 11
// Cells 0..8 are digits 1..9 (cell i -> digit i+1); CELL_ZERO is digit 0.

static bool cell_is_digit(int cell) { return (cell >= 0 && cell <= 8) || cell == CELL_ZERO; }
static int  cell_digit_value(int cell) { return cell == CELL_ZERO ? 0 : cell + 1; }

static void cell_rect(int cell, int *bx, int *by, int *bw, int *bh) {
    int row = cell / NUMPAD_COLS, col = cell % NUMPAD_COLS;
    *bx = NUMPAD_X0 + col * CELL_W;
    *by = NUMPAD_Y0 + row * CELL_H;
    *bw = CELL_W;
    *bh = CELL_H;
}

// -1 for "not over any cell" (the loupe zone above, the info column to the
// right, or outside the panel entirely) - the numpad's own cancel region.
// A release verdict that reads -1 here commits nothing, the same rule
// menu.c's cancel band enforces for the app grid.
static int numpad_hit(int lx, int ly) {
    if (lx < NUMPAD_X0 || lx >= NUMPAD_X0 + NUMPAD_W) return -1;
    if (ly < NUMPAD_Y0 || ly >= NUMPAD_Y0 + NUMPAD_H) return -1;
    int col = (lx - NUMPAD_X0) / CELL_W;
    int row = (ly - NUMPAD_Y0) / CELL_H;
    if (col >= NUMPAD_COLS) col = NUMPAD_COLS - 1;
    if (row >= NUMPAD_ROWS) row = NUMPAD_ROWS - 1;
    return row * NUMPAD_COLS + col;
}

/* ---- THE LOUPE ------------------------------------------------------------
 *
 * A fixed-HEIGHT band above the numpad, LOUPE_ZONE_H tall, otherwise blank.
 * Only the bubble's horizontal centre tracks the finger; its vertical
 * centre (LOUPE_CY) never moves. See this file's header for why that is a
 * stronger fix than dynamic vertical placement, not a simplification of
 * one: it removes the "runs out of room above the top row" case for every
 * row at once, and it means the zone this bubble can ever touch is fixed
 * and blank, so redrawing it is always a plain white fill - never a
 * re-render of numpad cells underneath, because nothing else is ever drawn
 * there. Erase-then-redraw costs at most LOUPE_BOX_W x LOUPE_BOX_H twice
 * (old position, new position) per tick while dragging - see the cost
 * accounting in this app's own report.
 */
#define LOUPE_R    38.0f
#define LOUPE_PAD  6
#define LOUPE_BOX_W ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 100
#define LOUPE_BOX_H ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 100
#define LOUPE_CY   (OY + LOUPE_ZONE_H / 2)                   // 56
#define LOUPE_CX_MIN (NUMPAD_X0 + (int)LOUPE_R + LOUPE_PAD)          // 54
#define LOUPE_CX_MAX (NUMPAD_X0 + NUMPAD_W - (int)LOUPE_R - LOUPE_PAD) // 253

static uint16_t loupe_bubble_color(void) { return px_swap(0xDEFB); } // #DEDEDE, same grey as menu.c's halo

/* ---- RIGHT/WRONG COLOUR --------------------------------------------------
 *
 * The device is used monochrome everywhere else (gfx.h's own header: "the
 * panel is used as monochrome... the 6-bit green channel doubles as an
 * 8-bit ink/coverage value"), because shapes.h's anti-aliased primitives
 * all funnel through that trick (aa_composite_land converts every colour
 * to a grey level - see shapes.c). AGENTS.md is explicit that colour IS
 * available on this panel, so this app uses it in exactly the one place
 * that trick cannot reach anyway: a plain, hard-edged background wash
 * behind the answer, painted with gfx_fill_rect_land (which writes the
 * real RGB565 value, no grey conversion - the same "flat colour, no AA"
 * treatment timer.c's own progress-ring TRACK already uses). All the ink
 * on top of it - the digits themselves - stays black, so the wash is
 * background tinting, not a second inking system.
 *
 * WHY A WASH AT ALL, tied to "what happens after a wrong answer matters
 * more than the mark itself" (this file's own design brief). A right/wrong
 * verdict she has to read costs a beat of attention a colour does not: two
 * washes she can tell apart at a glance, seen the instant CHECK is
 * released, before she has read anything. Green is unambiguous the same
 * way a green light is; amber (not red) for wrong on purpose - this is a
 * child learning, not a form rejecting bad input, and amber reads as
 * "not yet" rather than as an alarm - consistent with WRONG_RETRY's whole
 * point (try again, no penalty for the attempt itself).
 */
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return px_swap(v);
}
static uint16_t tint_right(void) { return rgb565(214, 238, 214); } // pale green
static uint16_t tint_wrong(void) { return rgb565(250, 232, 196); } // pale amber

/* ---- THE INFO COLUMN -----------------------------------------------------
 *
 * To the right of the numpad: the question, the answer she is building,
 * and the three counters. All digits, per this file's header - she reads
 * fluently, and a picture of a number is a worse number than the numeral.
 */
#define INFO_X0 (NUMPAD_X0 + NUMPAD_W) // 307
#define INFO_W  (USABLE_W - NUMPAD_W)  // 131
#define INFO_CX (INFO_X0 + INFO_W / 2) // centre x for everything stacked in the column

#define Q1_Y 10
#define Q1_H 64
#define XMARK_Y 82
#define XMARK_H 24
#define Q2_Y 114
#define Q2_H 64
#define ANSWER_Y 186
#define ANSWER_H 64
#define COUNTERS_Y0 258
#define COUNTER_ROW_H 33 // 3 rows: 258, 291, 324 - last row's ink ends well short of OY+USABLE_H=358

/* =========================================================================
 * ICONS - shapes.h's float brush only, per decision 0009. Every one of
 * these is either round (no straight edge to worry about) or a short
 * straight capsule stroke, the same treatment digits_draw_soft's own
 * seven-segment rails already use (a capsule shaft is straight, its ends
 * are round caps - accepted throughout this codebase for exactly this
 * shape, see digits.h). Nothing here is a filled rectangle standing in for
 * an icon.
 * ========================================================================= */

// Backspace: a left-pointing chevron, "<" - two capsules meeting at the
// leftmost point. Universal enough to need no label.
static void draw_icon_back(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.32f;
    float tipX = (float)cx - reach, tipY = (float)cy;
    float topX = (float)cx + reach * 0.55f, topY = (float)cy - reach * 0.85f;
    float botX = (float)cx + reach * 0.55f, botY = (float)cy + reach * 0.85f;
    shapes_fill_capsule_aa_land(tipX, tipY, t, topX, topY, t, color);
    shapes_fill_capsule_aa_land(tipX, tipY, t, botX, botY, t, color);
}

// Submit: a checkmark - two capsules meeting at the low point.
static void draw_icon_check(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.26f;
    float p0x = (float)cx - reach * 0.75f, p0y = (float)cy;
    float p1x = (float)cx - reach * 0.15f, p1y = (float)cy + reach * 0.6f;
    float p2x = (float)cx + reach * 0.85f, p2y = (float)cy - reach * 0.7f;
    shapes_fill_capsule_aa_land(p0x, p0y, t, p1x, p1y, t, color);
    shapes_fill_capsule_aa_land(p1x, p1y, t, p2x, p2y, t, color);
}

// Multiply: an X of two crossing capsule strokes. Kept dead straight
// rather than bowed (decision 0009's own exception for a shape this small
// and this conventional - a hand-drawn multiply sign is two straight
// strokes even on paper; the digits right next to it are built from
// straight capsule rails too, see this block's own header note).
static void draw_icon_multiply(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.32f;
    shapes_fill_capsule_aa_land((float)cx - reach, (float)cy - reach, t,
                                 (float)cx + reach, (float)cy + reach, t, color);
    shapes_fill_capsule_aa_land((float)cx - reach, (float)cy + reach, t,
                                 (float)cx + reach, (float)cy - reach, t, color);
}

/* ---- COUNTER ICONS ---------------------------------------------------------
 *
 * The three counters are told apart by SHAPE, not by reading a word - the
 * one place this app still reaches for the wordless convention (see this
 * file's header). A ring, a filled disc and a clock face are unambiguous
 * even glanced at mid-practice, which is the actual requirement: "a child
 * mid-practice should not be able to misread 'correct' as 'attempted'".
 * Shape difference survives a glance; digit position next to a label
 * would not.
 */
static void draw_icon_attempted(int cx, int cy, float r, uint16_t color) {
    // An open ring: a tally not yet filled in.
    shapes_fill_annulus_aa_land((float)cx, (float)cy, r, r * 0.6f, color);
}
static void draw_icon_correct(int cx, int cy, float r, uint16_t color) {
    // A filled disc: landed, done.
    shapes_fill_disc_aa_land((float)cx, (float)cy, r, color);
}
static void draw_icon_time(int cx, int cy, float r, uint16_t color) {
    // A clock face: ring plus two hands. Not read as an actual time of
    // day, just the universal "duration" pictogram.
    shapes_fill_annulus_aa_land((float)cx, (float)cy, r, r * 0.82f, color);
    float t = r * 0.22f;
    shapes_fill_capsule_aa_land((float)cx, (float)cy, t, (float)cx, (float)cy - r * 0.55f, t, color);
    shapes_fill_capsule_aa_land((float)cx, (float)cy, t, (float)cx + r * 0.45f, (float)cy + r * 0.1f, t, color);
}

/* =========================================================================
 * NUMBERS. One helper draws 1 or 2 digits (0..99), left-to-right, centred
 * on cx - the shared renderer for the numpad's own digit keys, the
 * question's two factors (the second factor runs 1..10, so it alone needs
 * the two-digit case), the answer she is building, and the MM/SS time
 * counter (padTo2 forces a leading zero there).
 * ========================================================================= */
#define DIGIT_GAP 4

static void draw_number_lr(int cx, int cy, int digitW, int digitH, int t,
                            int value, bool padTo2, uint16_t color) {
    if (value < 0) value = 0;
    if (value > 99) value = 99;
    bool twoDigits = padTo2 || value >= 10;
    if (!twoDigits) {
        digits_draw_soft(DIGITS_LANDSCAPE, cx - digitW / 2, cy - digitH / 2, digitW, digitH, t, value, color);
        return;
    }
    int tens = value / 10, ones = value % 10;
    int totalW = digitW * 2 + DIGIT_GAP;
    int x0 = cx - totalW / 2;
    digits_draw_soft(DIGITS_LANDSCAPE, x0, cy - digitH / 2, digitW, digitH, t, tens, color);
    digits_draw_soft(DIGITS_LANDSCAPE, x0 + digitW + DIGIT_GAP, cy - digitH / 2, digitW, digitH, t, ones, color);
}

/* =========================================================================
 * FACTS: the 6, 7 and 8 tables, second factor 1..10 - 30 facts.
 *
 * WHICH QUESTIONS COME UP. Straight rotation is predictable and a child
 * pattern-matches the sequence rather than the arithmetic (AGENTS's own
 * framing of this exact question). The fix that is actually cheap: a
 * per-fact weight, bumped when she gets it wrong (gives up after two
 * tries - see THE QUESTION STATE below) and eased back down when she gets
 * it right, with the next question picked by weighted random draw over
 * every fact EXCEPT the one just asked (never immediately repeats). Wrong
 * facts come back around sooner without a fixed schedule, no separate
 * "review mode" screen, and nothing she has to configure.
 *
 * WHICH TABLE, AND WHY THERE IS NO PICKER SCREEN. All three tables are
 * always mixed; there is no per-table selection. Two reasons, not one:
 * interleaving different tables is the better way to practice in the
 * first place (it is what stops "the sixes" from being memorised as a
 * chant rather than as thirty independent facts - the same argument
 * against straight rotation, one level up), and a picker is exactly the
 * kind of screen this device's numpad-sized layout has no spare room for
 * without shrinking either the pad or the counters. If a wider range of
 * tables is ever wanted, TABLE_BASES below is the one array that grows;
 * nothing else in this file assumes there are three.
 */
#define TABLE_COUNT 3
static const int TABLE_BASES[TABLE_COUNT] = { 6, 7, 8 };
#define FACTOR_MAX 10
#define FACT_COUNT (TABLE_COUNT * FACTOR_MAX) // 30

static int fact_base(int f)    { return TABLE_BASES[f / FACTOR_MAX]; }
static int fact_factor(int f)  { return (f % FACTOR_MAX) + 1; }
static int fact_product(int f) { return fact_base(f) * fact_factor(f); }

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* =========================================================================
 * STATE
 * ========================================================================= */
typedef enum {
    PHASE_ASK = 0,        // waiting for her to build and submit an answer
    PHASE_RIGHT,          // brief positive pause, then a new question
    PHASE_WRONG_RETRY,     // brief pause, same question, answer cleared
    PHASE_WRONG_REVEAL,   // shows the right answer, then a new question
} phase_t;

// Feedback timings. Right is quick (keep momentum); retry is quicker still
// (get back to trying); reveal is the longest of the three on purpose - see
// THE QUESTION STATE below for why it is the one place this app slows down
// rather than moving on immediately.
#define RIGHT_MS        650
#define RETRY_MS        550
#define REVEAL_MS       1600

// THE GESTURE. Arm and release-grace are copied verbatim from menu.c/
// four.c - the same reasoning, the same measured controller, no reason to
// re-derive numbers that already have a citation (AGENTS.md names both
// explicitly as the ones to reuse). What IS re-derived is the commit
// confirm window: menu.c's 150ms exists to protect against a WRONG LAUNCH
// nobody is watching for in the instant it happens - an irreversible act
// with no undo. Here, every cell change is visible in the loupe AS it
// happens (nothing is filtered from what she sees), the release is what
// commits and nothing else, and a wrong commit is one backspace away from
// fixed. Lag is real cost here (it is the whole reason the loupe has to
// feel connected to her finger) where a 150ms-style excursion is not,
// because nothing launches on an excursion - only a release does, and she
// is watching the bubble when she decides to lift.
//
// MEASURED, not guessed - tools/sweep-tables-commit.ts, rebuilding
// emu.wasm at each candidate (EMU_EXTRA_DEFINES=-DCOMMIT_CONFIRM_MS=<n>)
// and driving 600 press-hold-release trials per value under the same
// calibrated jitter profile menu.c's own table used (34 dropouts/sec,
// 80-250px jitter for up to 3 reports), landing on each of the 9 digit
// cells in turn, measuring how often the digit that actually commits is
// NOT the one the thumb was on:
//
//     confirm window   wrong commits (of 600)   cancelled (of 600)
//            0ms              3 (0.50%)                 14
//           72ms              1 (0.17%)                 10
//          150ms              0 (0.00%)                 10   <- menu.c's own number
//
// Two things this table says together. First, even 0ms already beats
// menu.c's own worst case by a wide margin (0.5% wrong commits here vs
// menu's 14 excursions per 100 gestures) - expected, since nothing here
// commits until a genuine, RELEASE_GRACE-confirmed lift, where menu's own
// worst case was measuring a HALO that could move mid-drag with no lift
// involved at all. Second, the marginal gain from 72ms to 150ms (0.17% to
// 0.00%) is real but small, while the cost is not symmetric: this same
// window gates what the loupe SHOWS as well as what commits (set_hover()
// is called from both), so at 150ms every digit's first appearance in the
// loupe is delayed a full 150ms from the moment she lands a finger. 72ms
// is chosen with margin over the observed knee (wrong commits are already
// at 0 by 68-72ms in a smaller sweep) rather than at menu's own number,
// because the two failure costs are not the same: menu's 150ms protects an
// irreversible launch nobody is watching for; a wrong digit here is one
// backspace away from fixed and she is looking at the loupe the whole
// time. 72ms is also close to 3 jitter reports' own worst-case duration at
// the controller's 60Hz report rate (3 * 16.7ms = 50ms) plus margin for a
// dropout extending that episode's effective lifetime - the same
// arithmetic menu.c's own comment names as a lower bound before it
// overshoots it for its own, different reasons.
#define ARM_SAMPLES        4
#define ARM_MS            40
#define ARM_RATE_HZ       15u
#define RELEASE_GRACE_MS 300
#ifndef COMMIT_CONFIRM_MS
#define COMMIT_CONFIRM_MS 72
#endif

typedef struct {
    // Facts and weighting.
    uint8_t  weight[FACT_COUNT];
    int      factIndex;    // -1 until the first question is picked
    uint32_t rng;
    bool     rngSeeded;

    // The question in progress.
    int      answerDigits[2];
    int      answerLen;
    int      attemptsOnQuestion;

    phase_t  phase;
    uint32_t phaseDeadlineMs;

    // Counters.
    uint32_t attemptedCount;
    uint32_t correctCount;
    uint32_t enterMs;
    bool     enterMsSet;
    int      lastShownTimeSec; // -1 until first drawn, else floored to 5s

    // Touch gesture (see THE GESTURE above).
    bool     contactSeen;
    bool     armed;
    uint32_t gestureStartMs;
    uint32_t lastContactMs;
    int      contactCount;
    int      hoverCell;       // -1 = none/cancel
    int      pendingCell;
    uint32_t pendingSinceMs;

    // The loupe's last drawn box, so a tick that hides or moves it knows
    // exactly what to erase.
    bool     loupeVisible;
    int      loupeBx, loupeBy, loupeBw, loupeBh;
} tables_state_t;

static tables_state_t *s_tables;

/* =========================================================================
 * DRAWING - the numpad
 * ========================================================================= */
static void render_cell(int cell, bool hovered) {
    int bx, by, bw, bh;
    cell_rect(cell, &bx, &by, &bw, &bh);
    gfx_fill_rect_land(bx, by, bw, bh, PX_WHITE);
    if (hovered) {
        float rw = (float)bw / 2.0f - 4.0f;
        float rh = (float)bh / 2.0f - 4.0f;
        float r = rw < rh ? rw : rh;
        shapes_fill_disc_aa_land((float)(bx + bw / 2), (float)(by + bh / 2), r, loupe_bubble_color());
    }
    int cx = bx + bw / 2, cy = by + bh / 2;
    if (cell_is_digit(cell)) {
        draw_number_lr(cx, cy, 26, 42, 8, cell_digit_value(cell), false, PX_BLACK);
    } else if (cell == CELL_BACK) {
        draw_icon_back(cx, cy, 16.0f, PX_BLACK);
    } else { // CELL_CHECK
        draw_icon_check(cx, cy, 16.0f, PX_BLACK);
    }
}

static void push_cell(int cell) {
    int bx, by, bw, bh;
    cell_rect(cell, &bx, &by, &bw, &bh);
    gfx_push_land(bx, by, bw, bh);
}

static void draw_numpad_all(void) {
    for (int c = 0; c < 12; c++) render_cell(c, false);
}

/* ---- the loupe: content, then erase/redraw for one tick ------------------ */
static void draw_loupe_at(int bx, int by, int bw, int bh, int cell) {
    gfx_fill_rect_land(bx, by, bw, bh, PX_WHITE);
    int cx = bx + bw / 2, cy = by + bh / 2;
    float r = LOUPE_R;
    shapes_fill_disc_aa_land((float)cx, (float)cy, r, loupe_bubble_color());
    if (cell_is_digit(cell)) {
        draw_number_lr(cx, cy, 34, 54, 11, cell_digit_value(cell), false, PX_BLACK);
    } else if (cell == CELL_BACK) {
        draw_icon_back(cx, cy, 22.0f, PX_BLACK);
    } else if (cell == CELL_CHECK) {
        draw_icon_check(cx, cy, 22.0f, PX_BLACK);
    }
}

// Recomputes the loupe for this tick from the live (raw) touch x and the
// confirmed hoverCell, erasing whatever it drew last tick first. Called
// only while a gesture is armed; see tables_tick().
static void loupe_update(tables_state_t *s, int rawLandX) {
    bool show = s->hoverCell >= 0;
    int nbx = 0, nby = 0, nbw = LOUPE_BOX_W, nbh = LOUPE_BOX_H;
    if (show) {
        int cx = rawLandX;
        if (cx < LOUPE_CX_MIN) cx = LOUPE_CX_MIN;
        if (cx > LOUPE_CX_MAX) cx = LOUPE_CX_MAX;
        nbx = cx - LOUPE_BOX_W / 2;
        nby = LOUPE_CY - LOUPE_BOX_H / 2;
    }

    if (s->loupeVisible) {
        gfx_fill_rect_land(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh, PX_WHITE);
        gfx_push_land(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh);
    }
    if (show) {
        draw_loupe_at(nbx, nby, nbw, nbh, s->hoverCell);
        gfx_push_land(nbx, nby, nbw, nbh);
    }
    s->loupeVisible = show;
    s->loupeBx = nbx; s->loupeBy = nby; s->loupeBw = nbw; s->loupeBh = nbh;
}

static void loupe_hide(tables_state_t *s) {
    if (!s->loupeVisible) return;
    gfx_fill_rect_land(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh, PX_WHITE);
    gfx_push_land(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh);
    s->loupeVisible = false;
}

static void set_hover(tables_state_t *s, int cell) {
    if (cell == s->hoverCell) return;
    int old = s->hoverCell;
    s->hoverCell = cell;
    if (old >= 0)  { render_cell(old, false); push_cell(old); }
    if (cell >= 0) { render_cell(cell, true); push_cell(cell); }
}

/* =========================================================================
 * DRAWING - the info column
 * ========================================================================= */
static void redraw_question(tables_state_t *s) {
    gfx_fill_rect_land(INFO_X0, Q1_Y, INFO_W, Q1_H, PX_WHITE);
    gfx_fill_rect_land(INFO_X0, XMARK_Y, INFO_W, XMARK_H, PX_WHITE);
    gfx_fill_rect_land(INFO_X0, Q2_Y, INFO_W, Q2_H, PX_WHITE);
    draw_number_lr(INFO_CX, Q1_Y + Q1_H / 2, 40, 58, 12, fact_base(s->factIndex), false, PX_BLACK);
    draw_icon_multiply(INFO_CX, XMARK_Y + XMARK_H / 2, 11.0f, PX_BLACK);
    draw_number_lr(INFO_CX, Q2_Y + Q2_H / 2, 40, 58, 12, fact_factor(s->factIndex), false, PX_BLACK);
    gfx_push_land(INFO_X0, Q1_Y, INFO_W, (Q2_Y + Q2_H) - Q1_Y);
}

// tint: PX_WHITE at rest, a pale wash while a wrong attempt is showing -
// see redraw_answer's own callers. Plain gfx_fill_rect_land, never the AA
// primitives: those convert every colour to grey (aa_composite_land),
// which is correct for ink but would silently discard an actual tint.
static void redraw_answer(tables_state_t *s, uint16_t tint, bool showCorrectValue) {
    gfx_fill_rect_land(INFO_X0, ANSWER_Y, INFO_W, ANSWER_H, tint);
    int cy = ANSWER_Y + ANSWER_H / 2;
    if (showCorrectValue) {
        draw_number_lr(INFO_CX, cy, 40, 58, 12, fact_product(s->factIndex), false, PX_BLACK);
    } else if (s->answerLen > 0) {
        int value = s->answerDigits[0];
        if (s->answerLen == 2) value = s->answerDigits[0] * 10 + s->answerDigits[1];
        draw_number_lr(INFO_CX, cy, 40, 58, 12, value, false, PX_BLACK);
    } else {
        // Empty: a short placeholder underline, not a shape that could be
        // mistaken for a real digit.
        shapes_fill_capsule_aa_land((float)(INFO_CX - 18), (float)(cy + 20), 3.0f,
                                     (float)(INFO_CX + 18), (float)(cy + 20), 3.0f, PX_BLACK);
    }
    gfx_push_land(INFO_X0, ANSWER_Y, INFO_W, ANSWER_H);
}

/* ---- the three counters ---------------------------------------------------
 *
 * THE THREE COUNTERS' PLACEMENT: their own column, below the question and
 * the answer, never overlapping the numpad - the numpad's own 297px width
 * is never touched by any of this. TIME is deliberately coarse: it floors
 * to the nearest 5 seconds and only redraws when that floored value
 * changes, never a per-second tick. That is the answer to "should time be
 * visible while she works": visible, because the owner wants to know how
 * long a session ran, but never animating, because a number that visibly
 * counts up is a stopwatch she can feel racing against, and this is a
 * learning tool, not a timed drill. Coarsening to 5s steps is what turns
 * "elapsed time, technically" into "a quiet number that occasionally
 * changes" rather than a ticking clock in her peripheral vision.
 */
static void redraw_counter_row(int row, uint16_t iconKind, int value, bool padTo2) {
    int y = COUNTERS_Y0 + row * COUNTER_ROW_H;
    gfx_fill_rect_land(INFO_X0, y, INFO_W, COUNTER_ROW_H, PX_WHITE);
    int iconCx = INFO_X0 + 16, iconCy = y + COUNTER_ROW_H / 2;
    float r = 11.0f;
    if (iconKind == 0) draw_icon_attempted(iconCx, iconCy, r, PX_BLACK);
    else if (iconKind == 1) draw_icon_correct(iconCx, iconCy, r, PX_BLACK);
    else draw_icon_time(iconCx, iconCy, r, PX_BLACK);
    // 22x26 rather than the first pass's 16x24: at the smaller size "0"
    // (a ring - the digit every counter starts at) rendered as a solid
    // blob rather than a hole, because SOFT_INSET plus the stroke radius
    // left less than a stroke-width of daylight between the two verticals.
    // "1" (a single vertical) hid the bug entirely, which is why it only
    // showed up once a counter actually reached a "0" digit again - not
    // caught by eye until the rendered preview was looked at, per this
    // task's own instruction to look at the PNGs rather than trust the
    // arithmetic.
    int numCx = INFO_X0 + 16 + 38;
    draw_number_lr(numCx, iconCy, 22, 26, 7, value, padTo2, PX_BLACK);
    gfx_push_land(INFO_X0, y, INFO_W, COUNTER_ROW_H);
}

static void redraw_attempted(tables_state_t *s) {
    uint32_t v = s->attemptedCount > 99 ? 99 : s->attemptedCount;
    redraw_counter_row(0, 0, (int)v, false);
}
static void redraw_correct(tables_state_t *s) {
    uint32_t v = s->correctCount > 99 ? 99 : s->correctCount;
    redraw_counter_row(1, 1, (int)v, false);
}
// Draws MM (padded) : SS (padded) as two number groups side by side in the
// same row - a colon is not carried here (digits.h's dot-pair is
// LANDSCAPE-sized for a much bigger clock face); a thin gap reads clearly
// enough at this size, matching the same "no comma, always two groups"
// spirit without pulling in a separator this row has no room for anyway.
static void redraw_time(tables_state_t *s, uint32_t nowMs) {
    uint32_t elapsedSec = s->enterMsSet ? (nowMs - s->enterMs) / 1000u : 0u;
    int shown = (int)((elapsedSec / 5u) * 5u);
    if (shown == s->lastShownTimeSec) return;
    s->lastShownTimeSec = shown;
    int mm = shown / 60, ss = shown % 60;
    int y = COUNTERS_Y0 + 2 * COUNTER_ROW_H;
    gfx_fill_rect_land(INFO_X0, y, INFO_W, COUNTER_ROW_H, PX_WHITE);
    int iconCx = INFO_X0 + 16, iconCy = y + COUNTER_ROW_H / 2;
    draw_icon_time(iconCx, iconCy, 11.0f, PX_BLACK);
    // Same 0-as-a-blob fix as redraw_counter_row: 16x26 rather than 12x22.
    draw_number_lr(INFO_X0 + 16 + 40, iconCy, 16, 26, 5, mm, true, PX_BLACK);
    draw_number_lr(INFO_X0 + 16 + 80, iconCy, 16, 26, 5, ss, true, PX_BLACK);
    gfx_push_land(INFO_X0, y, INFO_W, COUNTER_ROW_H);
}

/* =========================================================================
 * QUESTION SELECTION AND RESOLUTION
 * ========================================================================= */
static int pick_next_fact(tables_state_t *s) {
    uint32_t sum = 0;
    for (int i = 0; i < FACT_COUNT; i++) {
        if (i == s->factIndex) continue;
        sum += s->weight[i];
    }
    if (sum == 0) sum = 1;
    uint32_t r = rng_next(&s->rng) % sum;
    uint32_t acc = 0;
    for (int i = 0; i < FACT_COUNT; i++) {
        if (i == s->factIndex) continue;
        acc += s->weight[i];
        if (r < acc) return i;
    }
    return (s->factIndex + 1) % FACT_COUNT; // unreachable in practice
}

static void start_new_question(tables_state_t *s) {
    s->factIndex = pick_next_fact(s);
    s->answerLen = 0;
    s->attemptsOnQuestion = 0;
    s->phase = PHASE_ASK;
    redraw_question(s);
    redraw_answer(s, PX_WHITE, false);
}

// The SAME question, after a first wrong attempt: clears the answer she
// typed and goes back to PHASE_ASK, but does NOT touch factIndex or
// attemptsOnQuestion (already 1, so a second wrong answer reaches the
// reveal rather than a third retry) or redraw the question itself (it has
// not changed). Kept as its own function rather than a branch inside
// start_new_question(): that function's whole job is picking a NEW fact,
// and folding "stay on this one" into it as a special case is exactly how
// this bug happened the first time here - a retry silently called
// start_new_question(), which picked a fresh fact AND reset
// attemptsOnQuestion to 0, so two wrong answers in a row could never reach
// the reveal at all (found by feature-tables.ts's own "a second wrong
// attempt reveals the answer" check, not by inspection).
static void resume_same_question(tables_state_t *s) {
    s->answerLen = 0;
    s->phase = PHASE_ASK;
    redraw_answer(s, PX_WHITE, false);
}

// PEDAGOGY: a wrong answer does not move on immediately and does not show
// the right answer immediately either. First wrong attempt -> the answer
// she typed is shown briefly against the amber wash (see tint_wrong()'s
// own comment for why amber and not red), then cleared, same question - a
// retrieval attempt before anything is given away, because being told the
// answer without trying again teaches "wait for the answer" rather than
// "recall the fact". A SECOND wrong attempt on the same question reveals
// the correct product, still on amber (REVEAL_MS, the longest pause of the
// three), and moves on - so she is never stuck retrying a fact she does
// not yet know, and always leaves having SEEN the right answer once before
// a new question replaces it on screen. Getting it right on the retry
// counts as correct: the point is recall, and a corrected recall is still
// recall.
static void resolve_answer(tables_state_t *s, uint32_t nowMs, bool correct) {
    s->attemptsOnQuestion++;
    if (correct) {
        s->weight[s->factIndex] = s->weight[s->factIndex] > 1 ? (uint8_t)(s->weight[s->factIndex] - 1) : 1;
        s->attemptedCount++;
        s->correctCount++;
        redraw_answer(s, tint_right(), false); // she already sees her own correct digits
        s->phase = PHASE_RIGHT;
        s->phaseDeadlineMs = nowMs + RIGHT_MS;
        redraw_attempted(s);
        redraw_correct(s);
        printf("tables: %d x %d = %d correct\r\n", fact_base(s->factIndex), fact_factor(s->factIndex), fact_product(s->factIndex));
        return;
    }
    if (s->attemptsOnQuestion < 2) {
        redraw_answer(s, tint_wrong(), false); // her own wrong digits, on amber
        s->phase = PHASE_WRONG_RETRY;
        s->phaseDeadlineMs = nowMs + RETRY_MS;
        printf("tables: wrong, retry\r\n");
        return;
    }
    uint32_t w = (uint32_t)s->weight[s->factIndex] + 4;
    s->weight[s->factIndex] = (uint8_t)(w > 255 ? 255 : w);
    s->attemptedCount++;
    redraw_answer(s, tint_wrong(), true); // reveal the correct product, still amber
    s->phase = PHASE_WRONG_REVEAL;
    s->phaseDeadlineMs = nowMs + REVEAL_MS;
    redraw_attempted(s);
    printf("tables: %d x %d = %d, gave up after 2 tries\r\n", fact_base(s->factIndex), fact_factor(s->factIndex), fact_product(s->factIndex));
}

/* =========================================================================
 * INPUT ACTIONS
 * ========================================================================= */
static void action_digit(tables_state_t *s, int digit) {
    if (s->answerLen >= 2) return; // no product in these tables exceeds two digits
    s->answerDigits[s->answerLen++] = digit;
    redraw_answer(s, PX_WHITE, false);
    printf("tables: digit %d\r\n", digit); // observable by the regression tests, same
                                            // convention as morpion.c's "mark cell=" line
}
static void action_backspace(tables_state_t *s) {
    if (s->answerLen > 0) s->answerLen--;
    redraw_answer(s, PX_WHITE, false);
    printf("tables: backspace\r\n");
}
static void action_submit(tables_state_t *s, uint32_t nowMs) {
    if (s->answerLen == 0) return;
    int value = s->answerDigits[0];
    if (s->answerLen == 2) value = s->answerDigits[0] * 10 + s->answerDigits[1];
    resolve_answer(s, nowMs, value == fact_product(s->factIndex));
}

static void panel_to_land(int px, int py, int *lx, int *ly) {
    *lx = py;
    *ly = PANEL_W - 1 - px;
}

/* =========================================================================
 * app_t callbacks
 * ========================================================================= */
static void tables_enter(void) {
    tables_state_t *s = s_tables = APP_STATE(tables_state_t);
    for (int i = 0; i < FACT_COUNT; i++) s->weight[i] = 1;
    s->factIndex = -1;
    s->hoverCell = -1;
    s->pendingCell = -1;
    s->lastShownTimeSec = -1;

    draw_numpad_all();
    start_new_question(s);
    redraw_attempted(s);
    redraw_correct(s);
    // Time is drawn once at 00:00 here; enterMs is not known yet (no frame
    // has run), so the first real tick's redraw_time() call establishes it
    // - see tables_tick()'s own seeding comment.
    {
        int y = COUNTERS_Y0 + 2 * COUNTER_ROW_H;
        draw_icon_time(INFO_X0 + 16, y + COUNTER_ROW_H / 2, 11.0f, PX_BLACK);
        int x0 = INFO_X0 + 16 + 30;
        draw_number_lr(x0 + 14, y + COUNTER_ROW_H / 2, 12, 22, 4, 0, true, PX_BLACK);
        draw_number_lr(x0 + 46, y + COUNTER_ROW_H / 2, 12, 22, 4, 0, true, PX_BLACK);
    }
    printf("tables: entered\r\n");
}

static void tables_tick(const app_frame_t *f) {
    tables_state_t *s = s_tables;

    if (!s->rngSeeded) {
        s->rng = f->nowMs ^ 0xB5297A4Du;
        if (s->rng == 0) s->rng = 1;
        s->rngSeeded = true;
        s->enterMs = f->nowMs;
        s->enterMsSet = true;
    }
    redraw_time(s, f->nowMs);

    if (s->phase != PHASE_ASK) {
        if (f->nowMs >= s->phaseDeadlineMs) {
            if (s->phase == PHASE_WRONG_RETRY) resume_same_question(s);
            else start_new_question(s); // PHASE_RIGHT or PHASE_WRONG_REVEAL: this question is done
        }
        return; // feedback states ignore touch entirely - see this file's header
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
        if (s->armed) {
            int lx, ly;
            panel_to_land(f->touchX, f->touchY, &lx, &ly);
            int cell = numpad_hit(lx, ly);
            if (cell == s->pendingCell) {
                if (cell != s->hoverCell && (f->nowMs - s->pendingSinceMs) >= COMMIT_CONFIRM_MS) {
                    set_hover(s, cell);
                }
            } else {
                s->pendingCell = cell;
                s->pendingSinceMs = f->nowMs;
            }
            loupe_update(s, lx);
        }
        return;
    }

    if (!s->contactSeen) return;
    if ((f->nowMs - s->lastContactMs) < RELEASE_GRACE_MS) return;

    bool wasArmed = s->armed;
    int cell = s->hoverCell;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    s->pendingCell = -1;
    set_hover(s, -1);
    loupe_hide(s);

    if (!wasArmed || cell < 0) return; // cancelled - see this file's header

    if (cell_is_digit(cell)) action_digit(s, cell_digit_value(cell));
    else if (cell == CELL_BACK) action_backspace(s);
    else action_submit(s, f->nowMs);
}

const app_t g_tablesApp = {
    .name       = "TABLES",
    .enter      = tables_enter,
    .tick       = tables_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
