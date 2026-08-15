/*
 * digits: seven-segment numerals, drawn as filled rectangles in landscape.
 *
 * Shared by the stopwatch and the timer because they are the same numerals,
 * and two copies of a glyph renderer drift: the owner has already had to ask
 * twice for the same fix (once for "4 is too small", once for "1 should be
 * full, not cut in the middle"), and that is exactly the kind of correction
 * that gets applied to one copy and not the other.
 *
 * WHY RECTANGLES. gfx rotates rectangles, not pixels (see gfx.h), so a
 * landscape app that draws only filled rectangles gets its rotation for
 * free and correct. Seven-segment numerals are seven filled bars, which is
 * why this shape was chosen over a font: it is legible at 120px from across
 * a room, and it is honest about being a machine's numerals rather than
 * pretending to be typography.
 *
 * THE TWO CORRECTIONS, both from the owner looking at the real panel, and
 * both easy to undo by accident:
 *
 *   1. Every numeral must occupy the FULL cell height. A naive seven-segment
 *      renderer draws the verticals between the horizontal bars, so a digit
 *      missing its top bar (4) or its middle bar comes out visibly shorter
 *      than its neighbours and the row of numbers looks broken. Fix: a
 *      vertical extends to the cell edge when the horizontal that would cap
 *      it is absent.
 *   2. A vertical pair with no middle bar between them must JOIN THROUGH the
 *      middle rather than stop short of it, or 1 reads as two stacked ticks
 *      with a gap. This is the "1 should be full not cut in the middle" fix.
 *
 * Both rules are properties of the renderer, not of individual glyphs, so
 * they hold for any digit added later.
 */
#ifndef DIGITS_H
#define DIGITS_H

#include <stdint.h>

// Draws one decimal digit (0..9) in landscape coordinates, filling the cell
// (lx, ly, w, h) exactly: the glyph's ink spans the full height for every
// value, per rule 1 above. `t` is the bar thickness. Draws ink only; the
// caller is responsible for having cleared the cell first, because the
// callers redraw a digit only when its value changed and they know what the
// background was.
void digits_draw(int lx, int ly, int w, int h, int t, int value, uint16_t colorPx);

// The separator between two digit pairs, drawn in its own cell so the
// callers can lay out "MM:SS" as five cells of known width. Always ":", and
// never "," : the owner asked for this specifically and it is not a matter
// of locale here, a comma reads as a decimal point on a clock face.
void digits_draw_colon(int lx, int ly, int w, int h, int t, uint16_t colorPx);

// Clears a cell to the background, for the redraw-only-what-changed path.
void digits_clear(int lx, int ly, int w, int h, uint16_t bgPx);

/* ---- the same numerals, drawn with ink instead of with a ruler ----------
 *
 * digits_draw() above fills rectangles. That is a hard edge, a right angle
 * and a dead straight line at every one of its 28 corners, which is exactly
 * what docs/decisions/0009-nothing-is-drawn-with-a-ruler.md forbids: "a hard
 * edge, a right angle, or a dead-straight line is a defect on this device in
 * the same way an off-by-one is a defect elsewhere."
 *
 * digits_draw_soft() draws the SAME seven segments from the SAME segment
 * table as round-capped, anti-aliased capsules (shapes.h's
 * shapes_fill_capsule_aa_land, the float brush decision 0009 says to reach
 * for). It lives here rather than in the app that wanted it first, because
 * digits.h's whole reason for existing is that these numerals must not fork:
 * the owner has already had to ask twice for the same glyph fix, and a
 * second seven-segment renderer in an app file is how that happens a third
 * time. chrono and timer are free to adopt this whenever somebody wants to
 * look at them next to each other; nothing about their behaviour changes
 * today.
 *
 * IT ALSO MAKES BOTH OF THE CORRECTIONS ABOVE STRUCTURAL. Segments are
 * defined by three horizontal rails (top, middle, bottom) that the verticals
 * run BETWEEN, and every end is a round cap that overlaps its neighbour, so:
 *   1. every numeral spans the full cell height by construction, because the
 *      top and bottom rails are where they are whether or not a bar is lit
 *      there - there is no "extends to the edge when the cap is absent"
 *      special case left to get wrong;
 *   2. a vertical pair with no middle bar (the "1") is two capsules that
 *      meet exactly at the middle rail, so it is continuous with no join
 *      visible and no special case either.
 * Both are properties of the geometry now, not of code that remembers to
 * check for them.
 *
 * WHY THE SPACE ARGUMENT. Everything else in this file works in landscape
 * coordinates because every app that had numerals was a landscape app. A
 * clock is not: held upright it puts the hours on one line and the minutes
 * on the line below. A capsule is invariant under a 90 degree rotation - it
 * is two points and a radius - so a portrait glyph is the same call with its
 * endpoints mapped through gfx.h's own landscape/panel relation, and no
 * second rasteriser is needed. DIGITS_PORTRAIT takes (x, y) in PANEL
 * coordinates, DIGITS_LANDSCAPE in landscape ones; a cell is (w, h) with w
 * across the numeral and h along it in both.
 */
typedef enum {
    DIGITS_LANDSCAPE = 0, // (x, y) are landscape coordinates, as above
    DIGITS_PORTRAIT  = 1, // (x, y) are panel coordinates, unrotated
} digits_space_t;

// One decimal digit (0..9) filling the cell (x, y, w, h) exactly, stroke
// thickness `t`, ink only (the caller clears). Ink stays strictly inside the
// cell, anti-aliased fringe included, so the cell is a legal push window.
void digits_draw_soft(digits_space_t space, int x, int y, int w, int h,
                      int t, int value, uint16_t colorPx);

// The two-dot separator, as round dots rather than squares. Same cell
// convention as digits_draw_soft; `t` is the dot diameter.
void digits_draw_dots_soft(digits_space_t space, int x, int y, int w, int h,
                           int t, uint16_t colorPx);

#endif // DIGITS_H
