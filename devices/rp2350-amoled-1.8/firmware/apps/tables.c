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
 *     the backspace chevron, the multiply and equals marks, the two
 *     counter glyphs, the numpad digits themselves) is built from
 *     shapes.h's float primitives (capsule/disc/annulus), the same brush
 *     every other app's ink uses, never a filled rectangle standing in for
 *     a shape. The counters' own box and the rule under the question band
 *     (both added by the owner's exact mockup, see LAYOUT below) are
 *     straight for the same accepted reason multiply/equals already are:
 *     a ruled line and a rectangle are exactly what they look like even on
 *     paper, and both are still drawn with the float brush (round-capped
 *     capsules), not a hard-edged fill;
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
 *     two counters are digits on purpose - she reads them, and a picture
 *     of "6" is worse than the numeral 6 for a child drilling numerals.
 *     The two counters (wrong/right) are told apart by an EXPLICIT glyph -
 *     a cross, a checkmark - rather than by a word or an arbitrary shape
 *     she would have to learn to decode first: the owner's own exact
 *     mockup (2026-08-17) replaced an earlier ring/disc pairing that had
 *     no inherent connection to what it counted (a bare ring does not mean
 *     "attempted" to anyone who has not been told) with marks that already
 *     mean what they mean everywhere else - see COUNTER ICONS below.
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
 * 95x49 (see THE LOUPE and THE NUMPAD below) - small enough to leave real
 * room for a full-width question band above the pad and the three
 * counters beside it, legible rather than finger-sized, because
 * legibility is what a shrunk target can now be judged on instead of
 * hit-rate. (First shipped at 99x64, when the question and answer still
 * lived in a column beside the pad rather than a band above it; the
 * owner's redrawn layout - see LAYOUT below - claims a full-width row for
 * the question, which is what shrank the pad again. See this app's own
 * report for what that comes out to against a child's fingertip.)
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
 * canvas is therefore 428x348.
 *
 * THE OWNER'S LAYOUT. Went through two of his own drawings before this
 * file matched what he meant: a reMarkable sketch (2026-08-17) settled the
 * broad shape - a full-width question band across the top, the numpad
 * lower-left, the counters lower-right - and an exact mockup the same day
 * ("7 x 2 = |___", full mockup referenced in the commit that landed it)
 * settled the details a sketch cannot: a full-width RULE under the
 * question band, the counters living inside a DRAWN BOX rather than
 * strung down the column, and exactly TWO counters (wrong, right), not
 * three. Four bands, stacked and side by side: the question band across
 * the very top (the blank at the right end of that line, carrying a
 * blinking caret - see THE BLINKING CURSOR below), the rule under it, then
 * the numpad lower-left (~2/3 of the width) and the boxed counters
 * lower-right (~1/3), roughly the pad's own lower rows in height. This
 * replaces the first version's layout, which stacked the two factors, the
 * multiply mark and the answer as their own column beside the pad (posee,
 * not horizontal).
 * ========================================================================= */
#define OX PANEL_BEZEL_MARGIN_PX
#define OY PANEL_BEZEL_MARGIN_PX
#define USABLE_W (LAND_W - 2 * PANEL_BEZEL_MARGIN_PX) // 428
#define USABLE_H (LAND_H - 2 * PANEL_BEZEL_MARGIN_PX) // 348

/* ---- THE QUESTION BAND ---------------------------------------------------
 *
 * The full-width row across the top. QROW_H is chosen so that, together
 * with LOUPE_ZONE_H and the numpad's own four rows below it, the left
 * column's three stacked pieces still sum to exactly USABLE_H - see THE
 * NUMPAD's own comment for that arithmetic.
 */
#define QROW_Y0 OY
// 80, not the first pass's 64: the owner's exact mockup shows real
// whitespace between the equation and the full-width rule below it (see
// draw_question_rule()), and 64 left only 1-2px between the answer's own
// underline and that rule - the two read as one smudged double-line, not
// "text, then a divider". The extra 16px is recovered from the loupe
// (LOUPE_R 38->34, still safely bigger than its own content - see
// draw_loupe_at()'s comment) and the numpad (CELL_H 49->47) rather than
// grown for free; see THE NUMPAD's own comment for the full arithmetic.
#define QROW_H  80
#define QROW_CY (OY + QROW_H / 2) // 50

/* ---- THE NUMPAD ---------------------------------------------------------
 *
 * 3 columns x 4 rows, phone-dial order (1 2 3 / 4 5 6 / 7 8 9 / back 0
 * check), 12 cells, filling the lower-left ~2/3 of the width: with the
 * loupe doing the precision work (see this file's header), a cell only has
 * to be roughly aimable and legibly labelled, not fingertip-sized. See
 * this app's own report for CELL_W x CELL_H checked against a child
 * fingertip rather than assumed.
 *
 * QROW_H + LOUPE_ZONE_H + NUMPAD_H must sum to exactly USABLE_H (348),
 * because the question band and the loupe zone now compete with the
 * numpad for the SAME vertical budget the first version had all to itself
 * (the question band spans the numpad's own width too, unlike the earlier
 * column that sat beside it) - 80 + 80 + 188 = 348. CELL_H dropped one
 * more step, from 49 to 47, to help buy QROW_H the room it needed for the
 * rule to read as separated from the equation above it (see QROW_H's own
 * comment) - see this app's own report for what that comes out to in
 * pixels against a child's fingertip.
 */
#define NUMPAD_COLS 3
#define NUMPAD_ROWS 4
#define CELL_W 95
#define CELL_H 47
#define NUMPAD_W (CELL_W * NUMPAD_COLS)             // 285 - about 2/3 of USABLE_W (428)
#define NUMPAD_H (CELL_H * NUMPAD_ROWS)             // 188
#define LOUPE_ZONE_H 80                              // = LOUPE_BOX_H exactly, see THE LOUPE below
#define NUMPAD_X0 OX                                 // 10
#define NUMPAD_Y0 (OY + QROW_H + LOUPE_ZONE_H)       // 170

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
 * A fixed-HEIGHT band above the numpad, LOUPE_ZONE_H tall, otherwise blank
 * - between the question band and the pad now, not at the very top of the
 * app (the question band owns that top row instead). Only the bubble's
 * horizontal centre tracks the finger; its vertical centre (LOUPE_CY)
 * never moves. See this file's header for why that is a stronger fix than
 * dynamic vertical placement, not a simplification of one: it removes the
 * "runs out of room above the top row" case for every row at once, and it
 * means the zone this bubble can ever touch is fixed and blank, so
 * redrawing it is always a plain white fill - never a re-render of numpad
 * cells underneath or of the question band above, because nothing else is
 * ever drawn there. LOUPE_ZONE_H is exactly LOUPE_BOX_H - the tightest
 * this zone can be without clipping the bubble. LOUPE_R dropped from 38 to
 * 34 (see QROW_H's own comment for why) - still comfortably bigger than
 * its own content needs: draw_loupe_at() draws a 30x48 digit inside it,
 * whose half-diagonal is sqrt(15^2+24^2) = ~28.3px, so a radius-34 disc
 * still clears every corner by ~5.7px, about the same relative margin the
 * original 38-radius/34x54-digit pairing had (~6.1px) - shrunk together,
 * not just the frame around unchanged content. Erase-then-redraw costs at
 * most LOUPE_BOX_W x LOUPE_BOX_H twice (old position, new position) per
 * tick while dragging - see the cost accounting in this app's own report.
 */
#define LOUPE_R    34.0f
#define LOUPE_PAD  6
#define LOUPE_BOX_W ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 80
#define LOUPE_BOX_H ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 80
#define LOUPE_CY   (OY + QROW_H + LOUPE_ZONE_H / 2)          // 130
#define LOUPE_CX_MIN (NUMPAD_X0 + (int)LOUPE_R + LOUPE_PAD)          // 50
#define LOUPE_CX_MAX (NUMPAD_X0 + NUMPAD_W - (int)LOUPE_R - LOUPE_PAD) // 255

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

/* ---- THE QUESTION BAND'S OWN LAYOUT ---------------------------------------
 *
 * One horizontal line, in reading order: base, a multiply mark, factor, an
 * equals mark, then the blank she fills in. The owner's exact mockup
 * (2026-08-17, "7 x 2 = |___") reads as ONE tight phrase with no dead air
 * anywhere in it - which the first version of this band did not do: it
 * reserved a fixed two-digit-wide slot for the factor (it runs 1..10) and
 * always CENTRED whatever was actually typed inside that slot, so a
 * one-digit factor like "1" sat in the middle of a 76px box instead of
 * hugging the multiply mark, and read as "6 x    1 =" - a hole, not a
 * phrase. The fix: base/multiply/factor-START stay fixed (the factor
 * always BEGINS at the same x), but everything from the factor's own
 * width onward - where it ends, where "=" starts, where the blank starts
 * - is a function of whether THIS question's factor is one digit or two,
 * computed fresh from s->factIndex (question_factor_w() and the
 * question_*() positions below it). A single-digit factor now sits flush
 * against the multiply mark; a "10" still fits before "=" without
 * overlapping it. Every fixed position below is a running total (start,
 * then start+width+gap, ...), the same style NUMPAD_Y0 above already uses
 * for a vertical stack.
 */
#define Q_LPAD  24
#define Q_GAP   20
#define QDIGIT_W 36
#define QDIGIT_H 44
#define QDIGIT_T 10
#define QICON_BOX 32                    // multiply/equals icon's own reserved width
#define Q2W (QDIGIT_W * 2 + DIGIT_GAP)  // 76 - a 2-digit factor or answer

#define Q_X0        (OX + Q_LPAD)                    // 34
#define Q_BASE_CX   (Q_X0 + QDIGIT_W / 2)             // 52
#define Q_MUL_X0    (Q_X0 + QDIGIT_W + Q_GAP)         // 90
#define Q_MUL_CX    (Q_MUL_X0 + QICON_BOX / 2)        // 106
#define Q_FACTOR_X0 (Q_MUL_X0 + QICON_BOX + Q_GAP)    // 142 - the factor always STARTS here
// question_factor_w()/question_factor_cx()/question_eq_x0()/question_eq_cx()/
// question_slot_x0()/question_slot_cx() - everything from here on that
// depends on whether THIS question's factor is one digit or two - are
// defined just above redraw_question() below: they need tables_state_t and
// fact_factor(), both declared later in the file than this constants block.

#define Q_SLOT_W    Q2W                                // 76 - up to two digits, always reserved
#define Q_UNDERLINE_Y (QROW_CY + QDIGIT_H / 2 + 2)    // 74 - just below the digit baseline
#define Q_UNDERLINE_R 2.0f

// The answer box: what redraw_answer() erases and redraws wholesale on
// every keystroke - the digits (or their absence), the underline, and (by
// clearing s->cursorDrawn) whatever the blinking cursor had drawn there.
// Wider than the digit slot itself (Q_CURSOR_CLEARANCE) so the caret's
// rightmost resting place - one past a completed 2-digit answer - is
// inside this same box rather than needing its own separate erase rect.
// Its X0 moves with question_slot_x0() (the factor's width shifts it), its
// WIDTH does not.
//
// ANSWER_BOX_H stops right at the underline's own bottom edge (76) rather
// than filling the rest of QROW_H down to RULE_Y: the first version of
// this box was QROW_H tall, which meant every keystroke's own white/tint
// fill repainted straight through the rule's row and erased the MIDDLE of
// it (wherever the answer box happened to sit that question) without ever
// redrawing it - a full-width line that ran in from the left, stopped
// under the blank, and resumed on the right, which read as one broken
// line rather than "a blank, and a separator below it". Found by looking
// at the rendered PNG, not by inspection. Keeping the box's own bottom
// edge clear of RULE_Y (see that constant's own comment) is what makes
// the rule untouchable by anything redraw_answer() does, ever - not a
// tighter erase-and-redraw discipline, just never overlapping in the
// first place.
#define Q_CURSOR_CLEARANCE 16
#define ANSWER_BOX_W  (Q_SLOT_W + Q_CURSOR_CLEARANCE) // 92
#define ANSWER_BOX_Y0 QROW_Y0
#define ANSWER_BOX_H  ((int)(Q_UNDERLINE_Y + Q_UNDERLINE_R) - QROW_Y0) // 66 (ends at y=76)

// A full-width rule directly under the question band, separating it from
// everything below (the owner's exact mockup adds this) - drawn ONCE, in
// tables_enter(), because it never changes: nothing else in this band ever
// draws this far down. Given REAL clearance from the answer's own
// underline: RULE_Y - RULE_R sits 8px below ANSWER_BOX_H's own bottom edge
// (76), empty white between them, so the two read as two separate
// objects, not one smudge - the owner's own complaint about the first
// version of this line, which sat close enough under the underline to
// look like a broken continuation of it rather than its own element.
#define RULE_Y (OY + QROW_H - 4) // 86 (ink 84-88), 2px clear of the band's own bottom edge (90);
                                  // 8px of empty white above it, back to ANSWER_BOX_H's bottom edge (76)
#define RULE_R 2.0f

/* ---- THE COUNTERS PANEL ---------------------------------------------------
 *
 * The owner's exact mockup (2026-08-17) settles three things his earlier
 * sketch and brief left open, superseding them:
 *
 *   - TWO counters, not three. Both his drawings and this mockup show
 *     exactly a cross-count and a check-count; the time-spent counter this
 *     app carried before was this file's own addition (he mentioned wanting
 *     to know time spent, in speech, well before the mockup), not something
 *     either drawing ever asked to see on screen. Dropped from the display;
 *     s->enterMs/enterMsSet stay (a timestamp is nearly free to keep and
 *     the owner may still want it surfaced later), nothing computes a
 *     shown value from it any more.
 *   - The panel sits INSIDE A DRAWN BOX, lower-right, roughly the vertical
 *     span of the pad's lower rows - not strung down the full height next
 *     to the question the way the first attempt at this redesign put it.
 *     The box is what tells a child this column is a readout, not more
 *     keys - see draw_counter_box()'s own comment for why that reads
 *     better here than changing either glyph would have.
 *   - The CHECK counter keeps the numpad's own checkmark glyph unchanged
 *     (the box already disambiguates it from the CHECK key, so the glyph
 *     itself does not have to).
 */
#define INFO_X0 (NUMPAD_X0 + NUMPAD_W) // 295
#define INFO_W  (USABLE_W - NUMPAD_W)  // 143

// The box: bottom-flush with the numpad, spanning its lower 3 of 4 rows -
// "roughly the vertical span of the pad's lower rows" per the mockup, not
// the whole pad (that would run it up beside the question band again).
#define BOX_X0 (INFO_X0 + 8)                    // 303
#define BOX_W  (INFO_W - 16)                    // 127
#define BOX_Y0 (NUMPAD_Y0 + CELL_H)             // 211 - skip the pad's own top row
// NUMPAD_H - CELL_H would put the border's own bottom edge exactly on
// OY+USABLE_H (358), and its stroke radius (BOX_R) then bleeds 1-2px past
// that into the bezel margin - caught by the gate's "no ink inside
// PANEL_BEZEL_MARGIN_PX" rule and feature-tables.ts's own bezel check.
// Trimmed by BOX_R rounded up plus a pixel of margin.
#define BOX_H  (NUMPAD_H - CELL_H - 6)          // 141
#define BOX_R  1.5f                              // border stroke radius (~3px thick)

// Two rows inside the box, inset from the border so a row's own redraw
// (which fills its own rect white first) never touches the border itself.
#define BOX_INSET 5
#define COUNTER_ROW_X0 (BOX_X0 + BOX_INSET)      // 308
#define COUNTER_ROW_W  (BOX_W - 2 * BOX_INSET)   // 117
#define COUNTERS_Y0    (BOX_Y0 + BOX_INSET)      // 216
#define COUNTER_ROW_H  ((BOX_H - 2 * BOX_INSET) / 2) // 65 (2 rows: 216, 281)

// Icon and digit placement, shared by both rows so the glyphs line up in
// one column and the numbers in another. Digit box kept well above the
// width-to-thickness ratio that bit an earlier, smaller version of this
// row: too small relative to stroke thickness t, and digits_draw_soft's
// "0" renders as a solid blob instead of a ring (SOFT_INSET plus the
// stroke radius leaves less than a stroke-width of daylight between the
// two verticals) - not caught by eye until a rendered preview was looked
// at. W/T at or above 3 here keeps clear of it.
#define COUNTER_ICON_CX_OFF 20
#define COUNTER_ICON_R 15.0f
// The cross needs its OWN reach, not COUNTER_ICON_R: draw_icon_multiply and
// draw_icon_check both take a "reach" parameter, but it means a different
// fraction of each glyph's own bounding box, so the SAME reach does not
// produce the SAME apparent size. At reach=15, the check's own stroke
// bounding box (including its t=0.26*reach thickness) is about 32x27px;
// the cross's is a symmetric X whose bounding box (2*reach + 2*t, t =
// 0.32*reach) is about 40x40px at the same reach - visibly bigger, exactly
// the "the X and check aren't the same size" bug report. COUNTER_CROSS_R
// is solved from the cross's own bounding-box formula for the width the
// check already has at COUNTER_ICON_R (2*reach*1.32 = 31.8 => reach ~
// 12.0), not copied from the check's reach.
#define COUNTER_CROSS_R 12.0f
#define COUNTER_NUM_CX_OFF 73
#define COUNTER_DIGIT_W 30
#define COUNTER_DIGIT_H 38
#define COUNTER_DIGIT_T 9

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

// Equals: two straight horizontal strokes, stacked - as canonically
// straight on paper as the multiply mark above, and dead straight for the
// same reason (decision 0009's own exception for a glyph this small and
// this conventional).
static void draw_icon_equals(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.32f;
    float half = reach * 0.5f;
    shapes_fill_capsule_aa_land((float)cx - reach, (float)cy - half, t,
                                 (float)cx + reach, (float)cy - half, t, color);
    shapes_fill_capsule_aa_land((float)cx - reach, (float)cy + half, t,
                                 (float)cx + reach, (float)cy + half, t, color);
}

/* ---- COUNTER ICONS ---------------------------------------------------------
 *
 * The two counters are told apart by an EXPLICIT glyph, per the owner's
 * own mockup - a cross for wrong, a checkmark for right (see this file's
 * header for why that replaces an earlier ring/disc pairing). "Correct"
 * reuses draw_icon_check verbatim: the SAME mark the numpad's own CHECK
 * key draws - the mockup keeps that collision on purpose and resolves it
 * with the drawn box around the panel instead (draw_counter_box()), not by
 * giving the counter a different glyph. "Wrong" reuses draw_icon_multiply's
 * own X under its own name, for the reason decision 0009 already accepted
 * a dead-straight X at this size for the multiply sign: a cross is two
 * straight strokes even on paper.
 */
static void draw_icon_cross(int cx, int cy, float reach, uint16_t color) {
    draw_icon_multiply(cx, cy, reach, color);
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
 * WHICH QUESTIONS COME UP, and why this changed on 2026-08-17. The first
 * version picked the next question by a WEIGHTED random draw, a per-fact
 * weight bumped on a wrong answer and eased back down on a right one, so a
 * fact she had just missed would resurface sooner. Pedagogically motivated,
 * but the owner's own instruction after seeing it in practice was blunter
 * than that trade-off: "randomize the numbers" - questions have to be
 * genuinely unpredictable to a child who has seen the previous few, and a
 * weight that keeps climbing on a fact she is struggling with does the
 * opposite, clustering the SAME few questions in a burst that reads as
 * anything but random. pick_next_fact() now draws UNIFORMLY over every
 * fact except the one just asked: no fact is ever more likely than another,
 * and the one guaranteed exclusion is exactly what makes an immediate
 * back-to-back repeat impossible BY CONSTRUCTION (verified by
 * feature-tables.ts's own "never repeats immediately" check, driven for
 * many consecutive questions, not just asserted from reading the code).
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
//
// THE FIRST HOVER OF A GESTURE IS SHOWN WITH NO CONFIRM DELAY AT ALL. The
// table above already argues 72ms is a CHANGE-filtering window, chosen
// against jitter between two cells the loupe has already shown her at least
// one of. On the very first armed sample of a gesture hoverCell is still -1:
// nothing is on screen yet to filter against, and nothing commits on a
// hover in the first place (a release does, re-reading hoverCell then - see
// this file's header, "WHAT COMMITS IS WHAT THE LOUPE SHOWED"). So the
// tick's own armed branch below shows the first cell the instant it arms
// (~40-67ms in, per ARM_SAMPLES/ARM_MS/ARM_RATE_HZ) rather than waiting
// another COMMIT_CONFIRM_MS on top - the owner's own complaint after
// testing in the emulator ("i have to press for a fairly long time for a
// touch to register") was exactly this stacked ~112ms floor before anything
// lit up. Found and fixed 2026-08-17.
//
// RELEASE_GRACE_MS was copied verbatim from menu.c/four.c, per the note
// above, but MEASURED here too rather than left on that citation alone
// (tools/sweep-tables-grace.ts: rebuilds emu.wasm at each candidate with
// EMU_EXTRA_DEFINES=-DRELEASE_GRACE_MS=<n>, drives 600 held/dragged/tapped
// trials per value under TOUCHSIM_HARDWARE_MEASURED's 34 dropouts/sec -
// this project's own measured worst case, not an assumption; see that
// file's header for the "does the emulator even model dropouts" check this
// was verified against before trusting any of it). What it found was NOT
// what shrinking a copied 300ms number was expected to find:
//
//     grace(ms)  premature commits (of 200-400 trials, held-still / drag)
//            80        94%  /  83%
//           120      34.5%  /  18%
//           160        17%  /  9.5%
//           200       3.5%  /  0.5%
//           250         1%  /  1%
//           260      0.75%  /  0%
//           270         0%  /  0.25%
//           280      0.25%  /  0.25%
//           290         0%  /  0%
//           300         0%  /  0%    <- shipped value
//           320         0%  /  0%
//           350         0%  /  0%
//
// A REAL FLOOR, not a conservative guess with room to spare. Premature
// commits (a dropout run outlasting the grace window while a real finger is
// still down, read as a genuine lift - the exact failure this window
// exists to bridge) fall off a cliff between 80ms and 200ms, then sit in a
// noisy near-zero tail from 250-280ms (0-1%, consistent with a true rate
// too small for a few hundred trials to pin down exactly) before going
// cleanly and repeatably to zero at 290ms and up. 300ms is not "menu.c's
// number, probably fine here too" - it is inside the first band this
// sweep found with a clean zero AND margin either side of it (290 and 320
// both clean too), which is exactly the "smallest value with real margin"
// bar COMMIT_CONFIRM_MS's own table above was chosen against. Shrinking it
// would trade a real, measured safety margin for a delay that Task 1's own
// fix (immediate first hover) already removed from what the owner actually
// felt - RELEASE_GRACE_MS only ever gates the instant AFTER a genuine
// lift, not how fast a key lights up. Left at 300.
#define ARM_SAMPLES        4
#define ARM_MS            40
#define ARM_RATE_HZ       15u
#ifndef RELEASE_GRACE_MS
#define RELEASE_GRACE_MS 300
#endif
#ifndef COMMIT_CONFIRM_MS
#define COMMIT_CONFIRM_MS 72
#endif

typedef struct {
    // Facts.
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
    uint32_t wrongCount;
    uint32_t correctCount;
    uint32_t enterMs;   // kept (cheap) even though nothing displays it any
    bool     enterMsSet; // more - see THE COUNTERS PANEL's own comment

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

    // The answer blank's blinking cursor (see THE BLINKING CURSOR /
    // update_cursor() below). Bookkeeping only - what it draws is a pure
    // function of nowMs, s->answerLen and phase.
    bool     cursorDrawn;
    int      cursorX;
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
        draw_number_lr(cx, cy, 24, 32, 7, cell_digit_value(cell), false, PX_BLACK);
    } else if (cell == CELL_BACK) {
        draw_icon_back(cx, cy, 12.0f, PX_BLACK);
    } else { // CELL_CHECK
        draw_icon_check(cx, cy, 12.0f, PX_BLACK);
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
        draw_number_lr(cx, cy, 30, 48, 10, cell_digit_value(cell), false, PX_BLACK);
    } else if (cell == CELL_BACK) {
        draw_icon_back(cx, cy, 20.0f, PX_BLACK);
    } else if (cell == CELL_CHECK) {
        draw_icon_check(cx, cy, 20.0f, PX_BLACK);
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
 * DRAWING - the question band (full width, across the top)
 * ========================================================================= */

// Everything past the factor's own START depends on whether THIS
// question's factor (1..10) is one digit or two - see THE QUESTION BAND'S
// OWN LAYOUT above for why.
static int question_factor_w(tables_state_t *s) {
    return fact_factor(s->factIndex) >= 10 ? Q2W : QDIGIT_W;
}
static int question_factor_cx(tables_state_t *s) {
    return Q_FACTOR_X0 + question_factor_w(s) / 2;
}
static int question_eq_x0(tables_state_t *s) {
    return Q_FACTOR_X0 + question_factor_w(s) + Q_GAP;
}
static int question_eq_cx(tables_state_t *s) {
    return question_eq_x0(s) + QICON_BOX / 2;
}
static int question_slot_x0(tables_state_t *s) { // the blank starts here
    return question_eq_x0(s) + QICON_BOX + Q_GAP;
}
static int question_slot_cx(tables_state_t *s) {
    return question_slot_x0(s) + Q2W / 2;
}

// A full-width rule directly under the question band - the owner's exact
// mockup adds this, separating the band from everything below. Drawn from
// INSIDE redraw_question(), not once at entry: RULE_Y sits inside QROW_H
// (see its own comment), and redraw_question()'s own full-band erase runs
// on every NEW question, which would otherwise wipe the rule the very
// first time a question changed - found by looking at the "rest" preview
// PNG, not by inspection (the rule was simply gone). Endpoints inset by
// the stroke's own radius (+1 for AA coverage), not OX/OX+USABLE_W
// exactly: a capsule's round cap extends a full radius PAST the endpoint
// given, so drawing corner-to-corner would bleed a couple of pixels into
// PANEL_BEZEL_MARGIN_PX on both ends - caught by the gate's "no ink inside
// the bezel" rule and this app's own feature-tables.ts bezel check.
static void draw_question_rule(void) {
    int inset = (int)RULE_R + 1;
    shapes_fill_capsule_aa_land((float)(OX + inset), (float)RULE_Y, RULE_R,
                                 (float)(OX + USABLE_W - inset), (float)RULE_Y, RULE_R, PX_BLACK);
}

// The fixed part: base, multiply mark, factor, equals mark. Only redrawn
// when a NEW question starts (start_new_question()), never per keystroke -
// the blank at the right (redraw_answer(), below) owns its own separate
// rect for that.
//
// Erases the FULL band width (not just up to this question's own content),
// because question_slot_x0() moves depending on whether the factor is one
// digit or two (see this block's header comment above): the PREVIOUS
// question could have ended further right or left than this one, and a
// partial erase sized to only the new content would leave the old
// question's ink stranded outside it. A full-width erase is a bounded,
// once-per-question cost (never per keystroke, never per tick) so there is
// no reason to chase the tighter bound.
static void redraw_question(tables_state_t *s) {
    gfx_fill_rect_land(OX, QROW_Y0, USABLE_W, QROW_H, PX_WHITE);
    draw_number_lr(Q_BASE_CX, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_base(s->factIndex), false, PX_BLACK);
    draw_icon_multiply(Q_MUL_CX, QROW_CY, 12.0f, PX_BLACK);
    draw_number_lr(question_factor_cx(s), QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_factor(s->factIndex), false, PX_BLACK);
    draw_icon_equals(question_eq_cx(s), QROW_CY, 12.0f, PX_BLACK);
    draw_question_rule(); // RULE_Y sits inside QROW_H - see that function's own comment
    gfx_push_land(OX, QROW_Y0, USABLE_W, QROW_H);
}

// tint: PX_WHITE at rest, a pale wash while a wrong attempt is showing -
// see redraw_answer's own callers. Plain gfx_fill_rect_land, never the AA
// primitives: those convert every colour to grey (aa_composite_land),
// which is correct for ink but would silently discard an actual tint.
static void redraw_answer(tables_state_t *s, uint16_t tint, bool showCorrectValue) {
    int x0 = question_slot_x0(s), cx = question_slot_cx(s);
    // Tint only the visible slot (Q_SLOT_W, the same 76px reference every
    // digit centres on below) - the extra Q_CURSOR_CLEARANCE strip past it
    // is erase margin for the caret's own rightmost rest position, never
    // part of the "answer box" a child reads, so it always stays plain
    // white regardless of tint. Painting the whole ANSWER_BOX_W (92) with
    // the tint made the wash's own visual centre sit 8px right of where
    // every digit actually centres - measured on a resolved "6": wash
    // spanned [x0, x0+91] (centre x0+45.5), the digit inked [x0+21,x0+54]
    // (centre x0+37.5). That 8px bias is what "the highlight reveals it's
    // not centred" was seeing - the underlying centring (on Q_SLOT_W) was
    // always right, only the wash disagreed with it.
    gfx_fill_rect_land(x0, ANSWER_BOX_Y0, Q_SLOT_W, ANSWER_BOX_H, tint);
    gfx_fill_rect_land(x0 + Q_SLOT_W, ANSWER_BOX_Y0, ANSWER_BOX_W - Q_SLOT_W, ANSWER_BOX_H, PX_WHITE);
    if (showCorrectValue) {
        draw_number_lr(cx, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_product(s->factIndex), false, PX_BLACK);
    } else if (s->answerLen > 0) {
        int value = s->answerDigits[0];
        if (s->answerLen == 2) value = s->answerDigits[0] * 10 + s->answerDigits[1];
        draw_number_lr(cx, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, value, false, PX_BLACK);
    }
    // The blank itself: the owner's own underline, always drawn under the
    // slot whether it currently holds a digit, two digits, or nothing yet.
    shapes_fill_capsule_aa_land((float)x0, (float)Q_UNDERLINE_Y, Q_UNDERLINE_R,
                                 (float)(x0 + Q_SLOT_W), (float)Q_UNDERLINE_Y, Q_UNDERLINE_R, PX_BLACK);
    gfx_push_land(x0, ANSWER_BOX_Y0, ANSWER_BOX_W, ANSWER_BOX_H);
    // This just repainted the whole box, cursor included - update_cursor()
    // redraws it fresh next tick against whatever s->answerLen is now.
    s->cursorDrawn = false;
}

/* ---- THE BLINKING CURSOR ---------------------------------------------------
 *
 * The owner's drawing labels the blank "blink": it carries a caret while
 * she is entering an answer, the iOS-keyboard convention for "this is
 * where typing lands" applied to a single two-digit field. Purely a
 * function of the clock and s->answerLen - no stored "when did it last
 * toggle" base - so PHASE_ASK can start, stop and restart (a wrong retry
 * clears the answer and returns to PHASE_ASK) without ever needing to
 * resync a phase: the picture at any given tick is fully determined by
 * (nowMs, answerLen, active), the same "settles to what the state alone
 * would draw" bar the gate holds every animation on this device to.
 * s->cursorDrawn/s->cursorX are bookkeeping only, so a tick where nothing
 * changed does not re-push a rectangle unchanged from the tick before.
 */
#define BLINK_PERIOD_MS 500u
#define Q_CURSOR_H 34
#define Q_CURSOR_R 2.0f

// Caret x for 0, 1 or 2 digits typed - just after the last digit's own
// box, or at the slot's own left edge when empty (the owner's mockup:
// "= |___", the caret sitting at the LEFT of the underline before she has
// typed anything). Reads question_slot_x0(s), so the caret tracks the
// same per-question shift the factor's own width causes.
static int cursor_x_for_len(tables_state_t *s) {
    int x0 = question_slot_x0(s);
    if (s->answerLen <= 0) return x0 + 4;
    if (s->answerLen == 1) {
        // A single digit renders CENTRED in the whole slot (the same
        // question_slot_cx() redraw_answer() draws it at), not left-aligned
        // against x0 - the caret has to follow the digit's ACTUAL right
        // edge or it lands inside the digit's own ink. Measured (owner's
        // bug report, "the cursor doesn't move when I type some numbers"):
        // a centred "4" inks [x0+21, x0+54]; the old x0+QDIGIT_W+6 (x0+42)
        // sat squarely inside that span - the caret WAS moving, but into a
        // spot the digit's own stroke covered, so nothing looked different.
        int singleCx = x0 + Q2W / 2; // == question_slot_cx(s)
        return singleCx + QDIGIT_W / 2 + 6;
    }
    return x0 + Q_SLOT_W + 6;
}

// The gate's app-fuzzing pass caught this one: a capsule's round CAP
// extends a full RADIUS past the endpoint it is given, not "+1" - the
// original version of this rect under-covered the cursor's own ink by
// several pixels at the top and bottom, a real decision-0001-shaped bug
// (correct in the emulator's framebuffer, would have gone stale on the
// board). `rr` (radius, rounded up, plus one for AA falloff) is added on
// every side, matching what shapes_fill_capsule_aa_land actually paints.
static void cursor_rect(int cx, int *bx, int *by, int *bw, int *bh) {
    int rr = (int)Q_CURSOR_R + 1;
    int half = Q_CURSOR_H / 2;
    *bx = cx - rr;
    *by = QROW_CY - half - rr;
    *bw = 2 * rr;
    *bh = 2 * (half + rr);
}

static void update_cursor(tables_state_t *s, uint32_t nowMs, bool active) {
    bool want = active && (((nowMs / BLINK_PERIOD_MS) % 2u) == 0u);
    int cx = cursor_x_for_len(s);
    int bx, by, bw, bh;

    if (s->cursorDrawn && (!want || cx != s->cursorX)) {
        cursor_rect(s->cursorX, &bx, &by, &bw, &bh);
        gfx_fill_rect_land(bx, by, bw, bh, PX_WHITE);
        gfx_push_land(bx, by, bw, bh);
        s->cursorDrawn = false;
    }
    if (want && !s->cursorDrawn) {
        float half = (float)Q_CURSOR_H / 2.0f;
        shapes_fill_capsule_aa_land((float)cx, (float)QROW_CY - half, Q_CURSOR_R,
                                     (float)cx, (float)QROW_CY + half, Q_CURSOR_R, PX_BLACK);
        cursor_rect(cx, &bx, &by, &bw, &bh);
        gfx_push_land(bx, by, bw, bh);
        s->cursorDrawn = true;
        s->cursorX = cx;
    }
}

/* ---- the counters box -------------------------------------------------- */

// The border: four capsule strokes, one per edge, meeting at the corners -
// still shapes.h's float brush, per decision 0009 (a straight rule is the
// cheapest way to draw a straight rule, and the round caps where two edges
// meet soften what would otherwise be four hard right angles into a
// rounded-rect rather than a ruled box). Drawn ONCE in tables_enter(): the
// box itself never changes, only the two rows inside it do, and those stay
// inset from the border (BOX_INSET) so their own per-redraw white fill
// never touches it.
static void draw_counter_box(void) {
    shapes_fill_capsule_aa_land((float)BOX_X0, (float)BOX_Y0, BOX_R,
                                 (float)(BOX_X0 + BOX_W), (float)BOX_Y0, BOX_R, PX_BLACK);
    shapes_fill_capsule_aa_land((float)BOX_X0, (float)(BOX_Y0 + BOX_H), BOX_R,
                                 (float)(BOX_X0 + BOX_W), (float)(BOX_Y0 + BOX_H), BOX_R, PX_BLACK);
    shapes_fill_capsule_aa_land((float)BOX_X0, (float)BOX_Y0, BOX_R,
                                 (float)BOX_X0, (float)(BOX_Y0 + BOX_H), BOX_R, PX_BLACK);
    shapes_fill_capsule_aa_land((float)(BOX_X0 + BOX_W), (float)BOX_Y0, BOX_R,
                                 (float)(BOX_X0 + BOX_W), (float)(BOX_Y0 + BOX_H), BOX_R, PX_BLACK);
}

static void redraw_counter_row(int row, int iconKind, int value, bool padTo2) {
    int y = COUNTERS_Y0 + row * COUNTER_ROW_H;
    gfx_fill_rect_land(COUNTER_ROW_X0, y, COUNTER_ROW_W, COUNTER_ROW_H, PX_WHITE);
    int iconCx = COUNTER_ROW_X0 + COUNTER_ICON_CX_OFF, iconCy = y + COUNTER_ROW_H / 2;
    if (iconKind == 0) draw_icon_cross(iconCx, iconCy, COUNTER_CROSS_R, PX_BLACK);
    else draw_icon_check(iconCx, iconCy, COUNTER_ICON_R, PX_BLACK);
    draw_number_lr(COUNTER_ROW_X0 + COUNTER_NUM_CX_OFF, iconCy, COUNTER_DIGIT_W, COUNTER_DIGIT_H,
                    COUNTER_DIGIT_T, value, padTo2, PX_BLACK);
    gfx_push_land(COUNTER_ROW_X0, y, COUNTER_ROW_W, COUNTER_ROW_H);
}

static void redraw_wrong(tables_state_t *s) {
    uint32_t v = s->wrongCount > 99 ? 99 : s->wrongCount;
    redraw_counter_row(0, 0, (int)v, false);
}
static void redraw_correct(tables_state_t *s) {
    uint32_t v = s->correctCount > 99 ? 99 : s->correctCount;
    redraw_counter_row(1, 1, (int)v, false);
}

/* =========================================================================
 * QUESTION SELECTION AND RESOLUTION
 * ========================================================================= */
// Uniform over every fact (0..FACT_COUNT-1) on the very first question
// (factIndex is -1, nothing to exclude yet); uniform over the other
// FACT_COUNT-1 facts on every question after that, which is what makes an
// immediate repeat structurally impossible rather than merely unlikely.
static int pick_next_fact(tables_state_t *s) {
    if (s->factIndex < 0) return (int)(rng_next(&s->rng) % FACT_COUNT);
    int idx = (int)(rng_next(&s->rng) % (FACT_COUNT - 1));
    if (idx >= s->factIndex) idx++; // shift past the excluded slot
    return idx;
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
//
// THE WRONG COUNTER counts every wrong SUBMISSION now, not just the final
// give-up - a real logic bug, not a display one, and an asymmetric one:
// correctCount already incremented on every successful CHECK press,
// whichever attempt it landed on, but wrongCount only incremented on the
// SECOND wrong attempt, silently dropping the first. Worked example: wrong,
// then wrong again (gives up) used to read wrongCount=1 for two actual
// wrong submissions; wrong, then right on the retry used to read
// correctCount=1, wrongCount=0 for one wrong submission and one right one -
// two different counting units on the two counters is exactly what "the
// count of wrong doesn't add up" was seeing. Both submissions increment
// wrongCount now (redraw_wrong() called immediately on the retry too, not
// only on give-up), so wrongCount+correctCount always equals the number of
// CHECK presses that carried a digit, the same unit on both sides.
static void resolve_answer(tables_state_t *s, uint32_t nowMs, bool correct) {
    s->attemptsOnQuestion++;
    if (correct) {
        s->correctCount++;
        redraw_answer(s, tint_right(), false); // she already sees her own correct digits
        s->phase = PHASE_RIGHT;
        s->phaseDeadlineMs = nowMs + RIGHT_MS;
        redraw_correct(s);
        printf("tables: %d x %d = %d correct\r\n", fact_base(s->factIndex), fact_factor(s->factIndex), fact_product(s->factIndex));
        return;
    }
    s->wrongCount++;
    redraw_wrong(s);
    if (s->attemptsOnQuestion < 2) {
        redraw_answer(s, tint_wrong(), false); // her own wrong digits, on amber
        s->phase = PHASE_WRONG_RETRY;
        s->phaseDeadlineMs = nowMs + RETRY_MS;
        printf("tables: wrong, retry\r\n");
        return;
    }
    redraw_answer(s, tint_wrong(), true); // reveal the correct product, still amber
    s->phase = PHASE_WRONG_REVEAL;
    s->phaseDeadlineMs = nowMs + REVEAL_MS;
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
    s->factIndex = -1;
    s->hoverCell = -1;
    s->pendingCell = -1;

    draw_numpad_all();
    draw_counter_box();
    start_new_question(s); // also draws the rule (redraw_question() owns it - see draw_question_rule())
    redraw_wrong(s);
    redraw_correct(s);
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
    update_cursor(s, f->nowMs, s->phase == PHASE_ASK);

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
            if (s->hoverCell < 0) {
                // The FIRST cell of this gesture. COMMIT_CONFIRM_MS exists to
                // filter jitter against an already-shown cell (see THE
                // GESTURE above); there is nothing shown yet to filter
                // against, hoverCell is still -1, and nothing commits on a
                // hover (only a release does, re-reading hoverCell at that
                // point - "WHAT COMMITS IS WHAT THE LOUPE SHOWED", this
                // file's header). So showing the first cell the instant the
                // gesture arms is free: it cannot let a commit happen on a
                // cell the loupe never displayed, because the loupe is about
                // to display exactly this one.
                set_hover(s, cell);
                s->pendingCell = cell;
                s->pendingSinceMs = f->nowMs;
            } else if (cell == s->pendingCell) {
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
