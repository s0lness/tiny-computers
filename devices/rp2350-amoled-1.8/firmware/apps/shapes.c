// shapes: see shapes.h for why this file has two generations of technique
// in it, and why that is deliberate rather than a half-finished migration.
#include <math.h>

#include "gfx.h"
#include "shapes.h"

/* -------------------------------------------------------------------------
 * Generation 1: non-anti-aliased, rectangle-bar. Kept for timer.c, which
 * uses both functions directly - see shapes.h's header comment.
 * ---------------------------------------------------------------------- */

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
        gfx_fill_rect_land(cx - hwOuter, y, 2 * hwOuter, 1, color);
        return;
    }
    gfx_fill_rect_land(cx - hwOuter, y, hwOuter - hwInner, 1, color);
    gfx_fill_rect_land(cx + hwInner, y, hwOuter - hwInner, 1, color);
}

/* -------------------------------------------------------------------------
 * Generation 2: anti-aliased, per-pixel coverage. See shapes.h's header
 * comment for the overall argument (sketch.c's draw_capsule technique,
 * MIN composition, why one "between curves" primitive covers a bulge, a
 * wedge and a diamond).
 * ---------------------------------------------------------------------- */

// THE landscape -> panel pixel mapping, applied one pixel at a time. Every
// AA primitive below funnels through this one function, so the mapping
// itself is written exactly once.
//
// gfx.h is explicit that it rotates RECTANGLES, not pixels
// (gfx_land_rect/gfx_fill_rect_land): a landscape app that only ever fills
// axis-aligned boxes never needs anything else, and until anti-aliasing
// arrived, every landscape app in this codebase was exactly that app. A
// per-pixel coverage value has no rectangle to hand gfx, so this is the
// deliberate exception - confined to this one static function precisely so
// it stays an exception rather than a second, competing way to do
// rotation. The mapping itself is not new: it is gfx.h's own documented
// forward rule, landscape (lx, ly) -> panel (PANEL_W-1-ly, lx) (see its
// "landscape helpers" comment), applied here per pixel instead of per
// rectangle corner.
//
// Composition is MIN, not blending - see shapes.h's header comment on why.
// coverage 0 means "this shape contributes no ink here" (leave the pixel
// alone); coverage 1 means "fully this shape's colour". `targetGray` is
// px_to_gray(colorPx), passed in rather than recomputed per pixel since
// every primitive below calls this from a tight loop.
static inline void aa_composite_land(int lx, int ly, float coverage, uint8_t targetGray) {
    if (lx < 0 || ly < 0 || lx >= LAND_W || ly >= LAND_H) return;
    if (coverage <= 0.0f) return;
    if (coverage > 1.0f) coverage = 1.0f;
    uint8_t ink = (uint8_t)((float)targetGray * coverage + 255.0f * (1.0f - coverage) + 0.5f);

    int px = PANEL_W - 1 - ly;
    int py = lx;
    int idx = py * PANEL_W + px;
    uint8_t cur = px_to_gray(gfx_fb[idx]);
    if (ink < cur) gfx_fb[idx] = gray_to_px(ink);
}

void shapes_fill_disc_aa_land(float cx, float cy, float r, uint16_t colorPx) {
    uint8_t targetGray = px_to_gray(colorPx);
    int minX = (int)floorf(cx - r - 1.0f);
    int maxX = (int)ceilf(cx + r + 1.0f);
    int minY = (int)floorf(cy - r - 1.0f);
    int maxY = (int)ceilf(cy + r + 1.0f);

    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float dx = px - cx, dy = py - cy;
            // Signed distance to the disc: negative inside, same
            // r+0.5-dist coverage convention as sketch.c's draw_capsule
            // (a disc IS a zero-length capsule).
            float d = sqrtf(dx * dx + dy * dy) - r;
            aa_composite_land(lx, ly, 0.5f - d, targetGray);
        }
    }
}

void shapes_fill_annulus_aa_land(float cx, float cy, float rOuter, float rInner,
                                  uint16_t colorPx) {
    uint8_t targetGray = px_to_gray(colorPx);
    int minX = (int)floorf(cx - rOuter - 1.0f);
    int maxX = (int)ceilf(cx + rOuter + 1.0f);
    int minY = (int)floorf(cy - rOuter - 1.0f);
    int maxY = (int)ceilf(cy + rOuter + 1.0f);

    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float dx = px - cx, dy = py - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float dOuter = dist - rOuter;  // negative inside the outer disc
            float dInner = dist - rInner;  // negative inside the inner disc (the hole)
            // SDF subtraction (outer disc minus inner disc):
            // max(dOuter, -dInner) - whichever constraint ("inside the
            // outer edge", "outside the inner edge") is more violated
            // wins, exactly the same max-of-two-distances idea
            // shapes_fill_between_curves_aa_land uses below for an
            // intersection of two half-planes/curves.
            float d = dOuter > -dInner ? dOuter : -dInner;
            aa_composite_land(lx, ly, 0.5f - d, targetGray);
        }
    }
}

// How many rows either side of a pixel's own row to search for the curve's
// TRUE nearest point - see shapes_fill_between_curves_aa_land's header
// comment for why the pixel's own row alone is not enough. Table rows are
// 1 landscape pixel apart (the same density shapes_fill_half_width_table
// already uses), so a window this wide covers several pixels of curve
// travel in every direction; cheap, since it is only a handful of extra
// squared-distance comparisons per pixel, not a second sqrtf.
#define CURVE_SEARCH_ROWS 4

// Unsigned distance from (px, py) to the nearest SAMPLE POINT of a curve
// given as one x value per row (landscape row topY+r, r in [0, rows)),
// searching only rows near rowGuess (see CURVE_SEARCH_ROWS). This is an
// unsigned magnitude only - shapes_fill_between_curves_aa_land supplies its
// own sign from the pixel's own row, which is accurate exactly where
// accuracy matters (see its comment).
static float nearest_curve_dist(float px, float py, const int16_t *xTab,
                                 int rows, int topY, int rowGuess) {
    int lo = rowGuess - CURVE_SEARCH_ROWS;
    int hi = rowGuess + CURVE_SEARCH_ROWS;
    if (lo < 0) lo = 0;
    if (hi > rows - 1) hi = rows - 1;

    float bestSq = 1e18f;
    for (int r = lo; r <= hi; r++) {
        float cx = (float)xTab[r];
        float cy = (float)(topY + r) + 0.5f;
        float dx = px - cx, dy = py - cy;
        float dSq = dx * dx + dy * dy;
        if (dSq < bestSq) bestSq = dSq;
    }
    return sqrtf(bestSq);
}

void shapes_fill_between_curves_aa_land(int topY, int rows,
                                         const int16_t *leftX, const int16_t *rightX,
                                         uint16_t colorPx) {
    if (rows <= 0) return;
    uint8_t targetGray = px_to_gray(colorPx);

    int minX = leftX[0], maxX = rightX[0];
    for (int r = 1; r < rows; r++) {
        if (leftX[r] < minX) minX = leftX[r];
        if (rightX[r] > maxX) maxX = rightX[r];
    }
    minX -= 2;
    maxX += 2;

    // ly is deliberately NOT padded above topY or below topY+rows-1: the
    // top and bottom of the range are hard cutoffs, not feathered - see
    // this function's header comment in shapes.h.
    for (int ly = topY; ly < topY + rows; ly++) {
        int row = ly - topY;
        float py = (float)ly + 0.5f;
        float leftHere = (float)leftX[row];
        float rightHere = (float)rightX[row];

        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;

            // Magnitude from the windowed search (fixes the staircase a
            // near-horizontal stretch of curve would otherwise get - see
            // shapes.h's header comment on this function). Sign from THIS
            // row's own sample: reliable right where it has to be (the ~1px
            // band the coverage falloff actually spans, where the nearest
            // point on the curve is necessarily close to this same row),
            // and irrelevant everywhere else (a wrong sign far from the
            // edge only saturates coverage to the same 0 or 1 either way).
            float dLeft = nearest_curve_dist(px, py, leftX, rows, topY, row);
            float sLeft = (px < leftHere) ? dLeft : -dLeft; // positive = outside (left of the left edge)

            float dRight = nearest_curve_dist(px, py, rightX, rows, topY, row);
            float sRight = (px > rightHere) ? dRight : -dRight; // positive = outside (right of the right edge)

            // Intersection SDF ("right of left curve" AND "left of right
            // curve"): max of the two, same combinator the annulus above
            // uses for its own two-edge shape.
            float d = sLeft > sRight ? sLeft : sRight;
            aa_composite_land(lx, ly, 0.5f - d, targetGray);
        }
    }
}

void shapes_fill_capsule_aa_land(float x0, float y0, float r0,
                                  float x1, float y1, float r1,
                                  uint16_t colorPx) {
    uint8_t targetGray = px_to_gray(colorPx);
    float maxR = (r0 > r1 ? r0 : r1) + 1.0f;
    int minX = (int)floorf((x0 < x1 ? x0 : x1) - maxR);
    int maxX = (int)ceilf((x0 > x1 ? x0 : x1) + maxR);
    int minY = (int)floorf((y0 < y1 ? y0 : y1) - maxR);
    int maxY = (int)ceilf((y0 > y1 ? y0 : y1) + maxR);

    float abx = x1 - x0, aby = y1 - y0;
    float abLenSq = abx * abx + aby * aby;

    // Signed-distance-to-segment field, converted to coverage - lifted
    // directly from sketch.c's draw_capsule (see its header comment for
    // the full reasoning); the only thing that changes here is that each
    // plotted pixel goes through aa_composite_land's landscape->panel
    // mapping instead of writing the (portrait) framebuffer directly, and
    // there is no dirty-rect tracking to update: menu.c paints its icons
    // once from menu_enter() and the runtime pushes the whole panel
    // afterwards (see menu.c's "Painting" header comment), so there is no
    // per-shape dirty rectangle worth accumulating here.
    for (int ly = minY; ly <= maxY; ly++) {
        float py = (float)ly + 0.5f;
        for (int lx = minX; lx <= maxX; lx++) {
            float px = (float)lx + 0.5f;
            float t = 0.0f;
            if (abLenSq > 0.0001f) {
                t = ((px - x0) * abx + (py - y0) * aby) / abLenSq;
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
            }
            float cx = x0 + abx * t, cy = y0 + aby * t;
            float dx = px - cx, dy = py - cy;
            float d = sqrtf(dx * dx + dy * dy) - (r0 + (r1 - r0) * t);
            aa_composite_land(lx, ly, 0.5f - d, targetGray);
        }
    }
}

// Same chord-count tuning as sketch.c's CURVE_SEG_PX/CURVE_MAX_STEPS (2.5px,
// 40 steps), loosened slightly the same way the deleted
// shapes_fill_tapered_quad_land was: this flattens one icon-sized curve a
// few dozen pixels long, not a live, arbitrarily long pen stroke, so
// fewer, slightly coarser chords still disappear into a smooth curve at
// icon scale and cost less.
#define TAPER_QUAD_SEG_PX    3.0f
#define TAPER_QUAD_MAX_STEPS 24

void shapes_fill_tapered_quad_aa_land(float x0, float y0, float r0,
                                       float cx, float cy,
                                       float x1, float y1, float r1,
                                       uint16_t colorPx) {
    float arm1x = cx - x0, arm1y = cy - y0;
    float arm2x = x1 - cx, arm2y = y1 - cy;
    float arm1 = sqrtf(arm1x * arm1x + arm1y * arm1y);
    float arm2 = sqrtf(arm2x * arm2x + arm2y * arm2y);
    int steps = (int)((arm1 + arm2) / TAPER_QUAD_SEG_PX + 0.5f);
    if (steps < 2) steps = 2;
    if (steps > TAPER_QUAD_MAX_STEPS) steps = TAPER_QUAD_MAX_STEPS;

    float prevX = x0, prevY = y0, prevR = r0;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float omt = 1.0f - t;
        float bx = omt * omt * x0 + 2.0f * omt * t * cx + t * t * x1;
        float by = omt * omt * y0 + 2.0f * omt * t * cy + t * t * y1;
        float br = r0 + (r1 - r0) * t;
        shapes_fill_capsule_aa_land(prevX, prevY, prevR, bx, by, br, colorPx);
        prevX = bx; prevY = by; prevR = br;
    }
}

void shapes_stroke_polyline_aa_land(const float *xs, const float *ys, int count,
                                     float radius, uint16_t colorPx) {
    for (int i = 1; i < count; i++) {
        shapes_fill_capsule_aa_land(xs[i - 1], ys[i - 1], radius, xs[i], ys[i], radius, colorPx);
    }
}
