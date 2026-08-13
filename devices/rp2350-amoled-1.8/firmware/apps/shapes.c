// shapes: see shapes.h for why this exists. Implementation is deliberately
// tiny: one table filler, one row drawer.
#include <math.h>

#include "gfx.h"
#include "shapes.h"

void shapes_fill_half_width_table(int16_t *out, int rows, float radius) {
    float centre = rows / 2.0f;
    for (int row = 0; row < rows; row++) {
        float dy = ((float)row + 0.5f) - centre;
        float underRoot = radius * radius - dy * dy;
        out[row] = underRoot > 0.0f ? (int16_t)lroundf(sqrtf(underRoot)) : 0;
    }
}

void shapes_draw_annulus_row(int cx, int y, int16_t hwOuter, int16_t hwInner,
                              uint16_t color) {
    if (hwOuter <= 0) return;
    if (hwInner <= 0) {
        // Above or below the hole: nothing to leave white, the ring is
        // solid all the way across at this row.
        gfx_fill_rect_land(cx - hwOuter, y, 2 * hwOuter, 1, color);
        return;
    }
    // Through the hole: the left arc, then the right arc, leaving the
    // middle white. Two separate rects, not one wide one with a white
    // rect painted back over the middle: this file only ever adds ink,
    // never erases it, so a caller compositing several shapes (as menu.c's
    // stopwatch icon does, ring then wedge) never has one shape's white
    // punch a hole in another's ink drawn earlier in the same row.
    gfx_fill_rect_land(cx - hwOuter, y, hwOuter - hwInner, 1, color);
    gfx_fill_rect_land(cx + hwInner, y, hwOuter - hwInner, 1, color);
}

void shapes_fill_thick_segment_land(int x0, int y0, int x1, int y1,
                                     int thickness, uint16_t color) {
    if (thickness < 1) return;
    int half = thickness / 2;
    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        // Degenerate segment (a point): one stamp, not a divide by zero
        // below.
        gfx_fill_rect_land(x0 - half, y0 - half, thickness, thickness, color);
        return;
    }
    // Step count from the segment's own length against thickness/2 (see
    // shapes.h's header comment on why that spacing is the one that keeps
    // consecutive stamps overlapping): +1 rounds up so the last step never
    // falls short of (x1,y1), and the loop below runs steps+1 times (0..
    // steps inclusive) so t=1.0 is always actually reached, not just
    // approached.
    int steps = (int)(len / ((float)thickness / 2.0f)) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        int cx = x0 + (int)lroundf(dx * t);
        int cy = y0 + (int)lroundf(dy * t);
        gfx_fill_rect_land(cx - half, cy - half, thickness, thickness, color);
    }
}

void shapes_fill_tapered_segment_land(int x0, int y0, int r0,
                                       int x1, int y1, int r1,
                                       uint16_t color) {
    if (r0 < 1) r0 = 1;
    if (r1 < 1) r1 = 1;
    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    int rMax = r0 > r1 ? r0 : r1;
    if (len < 1.0f) {
        gfx_fill_rect_land(x0 - rMax, y0 - rMax, 2 * rMax, 2 * rMax, color);
        return;
    }
    // Same spacing rule as shapes_fill_thick_segment_land, against the
    // WIDER of the two ends: the narrow end only needs closer-together
    // stamps too, and drawing it at the wide end's finer spacing costs a
    // few extra stamps, not a visible difference, whereas the reverse (the
    // wide end's spacing sized off the narrow end) is exactly the gap this
    // rule exists to prevent.
    int steps = (int)(len / ((float)rMax / 2.0f)) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        int cx = x0 + (int)lroundf(dx * t);
        int cy = y0 + (int)lroundf(dy * t);
        int r = r0 + (int)lroundf((float)(r1 - r0) * t);
        if (r < 1) r = 1;
        gfx_fill_rect_land(cx - r, cy - r, 2 * r, 2 * r, color);
    }
}

// How finely a quadratic curve is flattened into straight chords before
// each chord is itself stamped (by shapes_fill_tapered_segment_land) into
// overlapping squares. This is a SEPARATE concern from stamp spacing: stamp
// spacing is what keeps one chord gap-free, chord count is what keeps the
// chords themselves from reading as facets instead of a curve. Values are
// firmware/apps/sketch.c's CURVE_SEG_PX/CURVE_MAX_STEPS (2.5px, 40 steps),
// loosened slightly: sketch.c flattens a live, arbitrarily long pen stroke
// on a full-panel canvas, this flattens one icon-sized curve a few dozen
// pixels long, so fewer, slightly coarser chords still disappear into a
// smooth curve at 96px and cost less.
#define QUAD_SEG_PX    3.0f
#define QUAD_MAX_STEPS 24

void shapes_fill_tapered_quad_land(int x0, int y0, int r0,
                                    int cx, int cy,
                                    int x1, int y1, int r1,
                                    uint16_t color) {
    // Arc-length estimate from the control polygon (x0,y0)->(cx,cy)->
    // (x1,y1): an upper bound, cheap and good enough to pick a chord count,
    // the same technique and the same reasoning as sketch.c's
    // draw_quad_midpoint (a true arc length needs the curve itself, which
    // is circular).
    float arm1x = (float)(cx - x0), arm1y = (float)(cy - y0);
    float arm2x = (float)(x1 - cx), arm2y = (float)(y1 - cy);
    float arm1 = sqrtf(arm1x * arm1x + arm1y * arm1y);
    float arm2 = sqrtf(arm2x * arm2x + arm2y * arm2y);
    int steps = (int)((arm1 + arm2) / QUAD_SEG_PX + 0.5f);
    if (steps < 2) steps = 2;
    if (steps > QUAD_MAX_STEPS) steps = QUAD_MAX_STEPS;

    float prevX = (float)x0, prevY = (float)y0;
    int prevR = r0;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float omt = 1.0f - t;
        // Standard quadratic Bezier position; radius interpolated linearly
        // in t alongside it, the same way shapes_fill_tapered_segment_land
        // interpolates r0->r1 linearly across a single straight span.
        float bx = omt * omt * (float)x0 + 2.0f * omt * t * (float)cx + t * t * (float)x1;
        float by = omt * omt * (float)y0 + 2.0f * omt * t * (float)cy + t * t * (float)y1;
        int br = r0 + (int)lroundf((float)(r1 - r0) * t);
        if (br < 1) br = 1;
        shapes_fill_tapered_segment_land((int)lroundf(prevX), (int)lroundf(prevY), prevR,
                                          (int)lroundf(bx), (int)lroundf(by), br, color);
        prevX = bx; prevY = by; prevR = br;
    }
}
