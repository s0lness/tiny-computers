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

#endif // DIGITS_H
