// digits: seven-segment numerals, drawn as filled rectangles in landscape.
//
// See digits.h's header comment for why this file exists and for the two
// corrections it documents. The code below is lifted from
// apps/chrono/main.c's draw_digit() and draw_colon(), which is where both
// fixes were first worked out on real hardware: this is a move, not a
// rewrite, so the segment tables, the special cases and the comments that
// explain them all survive unchanged. The only thing that changed is how a
// rectangle actually reaches the panel: the old fill_rect_land(fb, ...) is
// now gfx_fill_rect_land(...), since the framebuffer is gfx's now, not this
// app's own malloc'd buffer.
#include "digits.h"

#include <stdbool.h>

#include "gfx.h"
// The float brush the soft numerals at the bottom of this file are painted
// with - see digits.h on why they exist and decision 0009 on why a numeral
// made of rectangles is a defect on this device.
#include "shapes.h"

/* ---------------------------------------------------------------------
 * Seven-segment digits, drawn as filled rectangles. No font, no
 * anti-aliasing (axis-aligned rectangles don't need it). Segment bits,
 * standard layout: a=top, b=top-right, c=bottom-right, d=bottom,
 * e=bottom-left, f=top-left, g=middle.
 * ------------------------------------------------------------------- */
static const uint8_t SEVEN_SEG[10] = {
    0x3F, // 0: a b c d e f
    0x06, // 1: b c
    0x5B, // 2: a b d e g
    0x4F, // 3: a b c d g
    0x66, // 4: b c f g
    0x6D, // 5: a c d f g
    0x7D, // 6: a c d e f g
    0x07, // 7: a b c
    0x7F, // 8: all
    0x6F, // 9: a b c d f g
};

// Draws one digit into a w x h cell at (x0,y0), segment thickness t, all in
// LANDSCAPE coordinates. Corner gaps of t x t between adjacent segments are
// deliberate (the classic seven-segment look) rather than a mistake in the
// geometry.
void digits_draw(int lx, int ly, int w, int h, int t, int value, uint16_t colorPx) {
    int x0 = lx, y0 = ly;
    uint8_t segs = SEVEN_SEG[value];
    int midY = y0 + h / 2 - t / 2;

    // Vertical segments run to the cell edge whenever the horizontal segment
    // that would have capped them is absent, so every digit occupies the full
    // cell height.
    //
    // Without this, a classic seven-segment "4" is visibly shorter than a "0":
    // 4 has neither the top nor the bottom bar, so its ink starts one segment
    // thickness down and ends one up, losing 2*t of height (36px here) against
    // its neighbours. Real LED displays have the same artifact and nobody
    // minds, because the segments are physically fixed; drawn at this size on
    // a screen it just reads as a smaller digit.
    int topY = (segs & 0x01) ? (y0 + t) : y0;          // below the top bar, or the very top
    int topH = midY - topY;
    int botY = midY + t;
    int botH = ((segs & 0x08) ? (y0 + h - t) : (y0 + h)) - botY;

    // When the middle bar is absent, an upper and lower vertical on the same
    // side are one continuous stroke, not two pieces with a gap. Otherwise a
    // "1" reads as two short dashes: it is b and c with no g between them, so
    // the middle segment's worth of space stays blank right through the digit.
    bool hasG = (segs & 0x40) != 0;
    int fullY = topY;
    int fullH = (botY + botH) - topY;

    if (segs & 0x01) gfx_fill_rect_land(x0,     y0,     w, t, colorPx); // a
    if (hasG)        gfx_fill_rect_land(x0,     midY,   w, t, colorPx); // g
    if (segs & 0x08) gfx_fill_rect_land(x0,     y0+h-t, w, t, colorPx); // d

    // Left side: f (upper) and e (lower).
    if (!hasG && (segs & 0x20) && (segs & 0x10)) {
        gfx_fill_rect_land(x0, fullY, t, fullH, colorPx);
    } else {
        if (segs & 0x20) gfx_fill_rect_land(x0, topY, t, topH, colorPx); // f
        if (segs & 0x10) gfx_fill_rect_land(x0, botY, t, botH, colorPx); // e
    }

    // Right side: b (upper) and c (lower).
    if (!hasG && (segs & 0x02) && (segs & 0x04)) {
        gfx_fill_rect_land(x0 + w - t, fullY, t, fullH, colorPx);
    } else {
        if (segs & 0x02) gfx_fill_rect_land(x0+w-t, topY, t, topH, colorPx); // b
        if (segs & 0x04) gfx_fill_rect_land(x0+w-t, botY, t, botH, colorPx); // c
    }
}

// Two stacked dots, centred in a w x h cell, in LANDSCAPE coordinates. This
// is the only separator now: the owner was explicit, always ":", never a
// comma, so every gap between digit groups uses this and draw_comma is gone.
void digits_draw_colon(int lx, int ly, int w, int h, int t, uint16_t colorPx) {
    int dotX = lx + (w - t) / 2;
    gfx_fill_rect_land(dotX, ly + h/3 - t/2,       t, t, colorPx);
    gfx_fill_rect_land(dotX, ly + (2*h)/3 - t/2,   t, t, colorPx);
}

// Clears a cell to the background, for the redraw-only-what-changed path.
void digits_clear(int lx, int ly, int w, int h, uint16_t bgPx) {
    gfx_fill_rect_land(lx, ly, w, h, bgPx);
}

/* ---------------------------------------------------------------------
 * The soft numerals. See digits.h for why they exist, why they live here
 * next to the rectangle ones rather than in the app that asked for them,
 * and why the two corrections this file documents become structural rather
 * than special-cased.
 * ------------------------------------------------------------------- */

// One capsule, in whichever space the caller is working in.
//
// The portrait branch is gfx.h's own mapping read backwards. gfx.h maps
// landscape (lx, ly) to panel (PANEL_W-1-ly, lx); invert that and a panel
// point (px, py) is the landscape point (py, PANEL_W-1-px). A capsule is two
// points and a radius, and a rotation preserves distances, so mapping the
// two endpoints is the entire conversion - there is no second rasteriser
// here, and no per-pixel code in this file at all.
static void soft_capsule(digits_space_t space, float x0, float y0,
                         float x1, float y1, float r, uint16_t colorPx) {
    if (space == DIGITS_LANDSCAPE) {
        shapes_fill_capsule_aa_land(x0, y0, r, x1, y1, r, colorPx);
    } else {
        float edge = (float)(PANEL_W - 1);
        shapes_fill_capsule_aa_land(y0, edge - x0, r, y1, edge - x1, r, colorPx);
    }
}

static void soft_dot(digits_space_t space, float cx, float cy, float r, uint16_t colorPx) {
    if (space == DIGITS_LANDSCAPE) {
        shapes_fill_disc_aa_land(cx, cy, r, colorPx);
    } else {
        shapes_fill_disc_aa_land(cy, (float)(PANEL_W - 1) - cx, r, colorPx);
    }
}

// How far inside the cell edge a segment's AXIS sits, on top of its own
// radius. shapes.c's coverage reaches half a pixel past the mathematical
// edge of a shape, so three quarters of a pixel of slack keeps every lit
// pixel strictly inside the cell - which is what lets the caller push
// exactly the cell it cleared and nothing wider (decision 0001's rule about
// row length is about the WINDOW; this is about the ink staying inside it).
#define SOFT_INSET 0.75f

void digits_draw_soft(digits_space_t space, int x, int y, int w, int h,
                      int t, int value, uint16_t colorPx) {
    if (value < 0 || value > 9) return;
    uint8_t segs = SEVEN_SEG[value];
    float r = (float)t * 0.5f;

    // The two rails the numeral is built on. Everything below is one of
    // three horizontal runs between xL and xR, or one of four vertical runs
    // between two of yT/yM/yB.
    float xL = (float)x + r + SOFT_INSET;
    float xR = (float)(x + w) - r - SOFT_INSET;
    float yT = (float)y + r + SOFT_INSET;
    float yB = (float)(y + h) - r - SOFT_INSET;
    float yM = (float)y + (float)h * 0.5f;

    if (segs & 0x01) soft_capsule(space, xL, yT, xR, yT, r, colorPx); // a, top
    if (segs & 0x40) soft_capsule(space, xL, yM, xR, yM, r, colorPx); // g, middle
    if (segs & 0x08) soft_capsule(space, xL, yB, xR, yB, r, colorPx); // d, bottom
    if (segs & 0x20) soft_capsule(space, xL, yT, xL, yM, r, colorPx); // f, upper left
    if (segs & 0x10) soft_capsule(space, xL, yM, xL, yB, r, colorPx); // e, lower left
    if (segs & 0x02) soft_capsule(space, xR, yT, xR, yM, r, colorPx); // b, upper right
    if (segs & 0x04) soft_capsule(space, xR, yM, xR, yB, r, colorPx); // c, lower right
}

void digits_draw_dots_soft(digits_space_t space, int x, int y, int w, int h,
                           int t, uint16_t colorPx) {
    float r = (float)t * 0.5f;
    float cx = (float)x + (float)w * 0.5f;
    soft_dot(space, cx, (float)y + (float)h / 3.0f, r, colorPx);
    soft_dot(space, cx, (float)y + (float)(2 * h) / 3.0f, r, colorPx);
}
