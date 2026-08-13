/*
 * menu: how a child picks an app.
 *
 * Landscape, full-screen. Icons only, in a row along the TOP of the screen -
 * no borders, no tile rectangles, no app name printed underneath. The owner's
 * words for this design: "je pense qu'on fait juste des icones... ne mets que
 * les icones, en haut de l'ecran, sans texte, sans bordure." Pictures only:
 * reading English is not the entry fee for a child who cannot yet, and now
 * neither is reading a frame around a picture.
 *
 * Touch is the ONLY input this file reads. Touching an icon launches that
 * app immediately - "je veux que ce soit que du touch. toucher une app ouvre
 * une app." There used to also be a button-driven cursor (BOOT moved a
 * selection, PWR-short launched it) for a device held sideways with both
 * thumbs on the buttons and no finger free for the glass. That path, and the
 * "which tile is selected" state it needed, are both gone: with touch as the
 * only way to launch, there is nothing left to select in advance and nothing
 * left to draw a cursor around. One less piece of state to get wrong is a
 * simplification, not a loss - see menu_state_t's comment.
 *
 * The touch TARGET is not the small icon itself: each icon's touch region is
 * a full-height column spanning the whole screen top to bottom, only its
 * WIDTH tied to the icon above it - see this file's comment on
 * column_hit_test() for the exact sizes and why.
 *
 * Opening and closing the menu (the BOOT+PWR long-press chord) is entirely
 * the runtime's business; this file never reads the PMIC and never calls
 * app_switch_to() with its own index. It only ever switches TO a real
 * g_apps[] entry, on a touch.
 */
#include <math.h>
#include <stdio.h>

#include <stdbool.h>

#include "app.h"
#include "gfx.h"
#include "sensors.h"
#include "menu.h"
#include "shapes.h"

// Named directly, the same trick runtime.c uses to single out the
// sketchpad: picking an icon for a tile has to know which app it is looking
// at, and app_t carries nothing more specific than a display name (which is
// free-form text the app itself chose, not a stable identifier this file
// should be parsing).
extern const app_t g_chronoApp;
extern const app_t g_sketchApp;
extern const app_t g_timerApp;

/* ---------------------------------------------------------------------
 * Layout, landscape coordinates (LAND_W x LAND_H = 448 x 368). Icons sit in
 * one row near the top; g_appCount is an extern int, not a macro (sized from
 * the linked app table, see app.h), so the split below is computed at
 * runtime rather than checked with a compile-time _Static_assert.
 * ------------------------------------------------------------------- */
#define ICON_TOP_MARGIN 24 // gap from the screen's top edge to the icon's own box

#define ICON_W 96
#define ICON_H 96

// Column i's horizontal span, in landscape x. Contiguous and full-width
// (no gap, no margin either side): the LAST column absorbs whatever LAND_W
// does not divide evenly, so every x in [0, LAND_W) belongs to exactly one
// column and none is dead space a touch can fall into and hit nothing.
static void column_rect_land(int i, int *bx, int *bw) {
    int n = g_appCount;
    int w = LAND_W / n;
    *bx = i * w;
    *bw = (i == n - 1) ? (LAND_W - *bx) : w;
}

// The touch coordinates a tick() sees arrive in PANEL (portrait) space, not
// landscape - see app.h's comment on touchX/touchY and gfx.h's note that it
// rotates rectangles, not pixels, so a landscape app that hit-tests a touch
// has to invert the mapping itself. gfx.h only documents the forward
// direction (landscape -> panel: px = PANEL_W-1-ly, py = lx, see gfx_land_
// rect's header comment) because chrono and timer, the other two landscape
// apps, never need the inverse: they draw digits, not touch targets. Solved
// here by algebra on that same documented mapping, not by a new gfx.h
// helper (nothing else needs it, same reasoning as this file's icon code
// below).
static void panel_to_land(int px, int py, int *lx, int *ly) {
    *lx = py;
    *ly = PANEL_W - 1 - px;
}

// THE TOUCH TARGET. Not the icon's own ~96px box - a full-height column,
// LAND_H (368px, the panel's entire landscape height) tall and one column
// width (LAND_W/g_appCount, ~149px for today's 3 apps) wide. AGENTS.md's
// finger-size section measures a child's fingertip contact at ~75px on this
// panel; 149px is about two finger-widths across (the same width the old
// bordered tile already used, so no regression there), and 368px is the
// WHOLE screen top to bottom - there is no way to miss vertically at all,
// which is the actual gain over the old ~220px-tall tile: a child aiming
// only roughly at an icon, anywhere in its column, still launches it. ly is
// computed by panel_to_land() above but genuinely unused here (see its
// caller): only which column lx falls in decides the hit.
static int column_hit_test(int lx) {
    int n = g_appCount;
    int w = LAND_W / n;
    int idx = lx / w;
    if (idx >= n) idx = n - 1; // clamp into the last column, which absorbed
                                // LAND_W's remainder in column_rect_land()
    if (idx < 0) idx = 0;
    return idx;
}

/* ---------------------------------------------------------------------
 * Icons: filled rectangles only, per this file's header comment. Each is
 * drawn into an ICON_W x ICON_H box at a given landscape origin.
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * The chrono icon: a stopwatch, redrawn to match the owner's reference
 * glyph (a classic stopwatch pictogram) instead of the old stacked-bars-
 * with-gaps look, which read as a beehive rather than a clock. Four parts:
 *
 *   - a thick RING (an annulus, via shapes.h), not a filled disc and not a
 *     thin outline;
 *   - a filled quarter WEDGE in the top-right quadrant, apex at the
 *     centre, its outer edge following the ring's INNER edge (touching it,
 *     per the reference - not floating inside it with a gap);
 *   - a rectangular CROWN on top, joined to the ring by a short, narrower
 *     NECK;
 *   - a small angled TAB near one-thirty on the dial, clear of the ring.
 *
 * Every dimension is a fraction of ICON_W/ICON_H rather than a literal
 * pixel count, so the icon rescales if the tile geometry above ever
 * changes instead of silently assuming today's 96x96. All of it works out
 * to plain integers at ICON_W = ICON_H = 96 (the values noted after each
 * macro below), which is also why CHRONO_TAB_OFF below uses a 707/1000
 * integer approximation of 1/sqrt(2) rather than a runtime sinf/cosf call
 * for what is, at compile time, a single fixed 45-degree offset.
 * ------------------------------------------------------------------- */
#define CHRONO_R_OUT   (ICON_W * 3 / 8)                    // 36
#define CHRONO_R_IN    (CHRONO_R_OUT - CHRONO_R_OUT / 4)   // 27: 9px stroke
#define CHRONO_NECK_W  (CHRONO_R_OUT / 3)                  // 12
#define CHRONO_NECK_H  (ICON_H / 16)                       // 6
#define CHRONO_CROWN_W (CHRONO_R_OUT * 2 / 3)               // 24
#define CHRONO_CROWN_H (ICON_H / 8)                         // 12
#define CHRONO_TAB_R   (CHRONO_R_OUT / 6)                   // 6, half-diagonal
#define CHRONO_TAB_GAP (CHRONO_R_OUT / 9)                   // 4, clear of the ring
// Distance from the ring's centre to the tab's centre: past the outer
// radius by the gap, plus the tab's own half-diagonal so CHRONO_TAB_GAP is
// the clearance between the ring's outer edge and the tab's nearest point.
#define CHRONO_TAB_DIST (CHRONO_R_OUT + CHRONO_TAB_GAP + CHRONO_TAB_R) // 46
#define CHRONO_TAB_OFF  (CHRONO_TAB_DIST * 707 / 1000)                 // 32

// Top of the crown to the bottom of the ring, centred in ICON_H.
#define CHRONO_COMP_H (CHRONO_CROWN_H + CHRONO_NECK_H + 2 * CHRONO_R_OUT) // 90

static int16_t s_chronoHwOuter[2 * CHRONO_R_OUT];
static int16_t s_chronoHwInner[2 * CHRONO_R_OUT];
static bool s_chronoTablesReady = false;

static void ensure_chrono_tables(void) {
    if (s_chronoTablesReady) return;
    shapes_fill_half_width_table(s_chronoHwOuter, 2 * CHRONO_R_OUT, (float)CHRONO_R_OUT);
    // Same row grid as the outer table (both 2*CHRONO_R_OUT tall, both
    // centred on the ring's centre) but the smaller radius, so rows above
    // and below the inner circle come out 0 - exactly the "no hole here"
    // signal shapes_draw_annulus_row() needs, and the same table doubles
    // as the wedge's row widths below.
    shapes_fill_half_width_table(s_chronoHwInner, 2 * CHRONO_R_OUT, (float)CHRONO_R_IN);
    s_chronoTablesReady = true;
}

static void draw_icon_chrono(int ox, int oy, uint16_t color) {
    ensure_chrono_tables();

    int top = oy + (ICON_H - CHRONO_COMP_H) / 2;
    int ccx = ox + ICON_W / 2;
    int ringTop = top + CHRONO_CROWN_H + CHRONO_NECK_H;
    int ccy = ringTop + CHRONO_R_OUT;

    // Ring: one row of shapes.h's annulus drawer per table row.
    for (int row = 0; row < 2 * CHRONO_R_OUT; row++) {
        int y = ringTop + row;
        shapes_draw_annulus_row(ccx, y, s_chronoHwOuter[row], s_chronoHwInner[row], color);
    }

    // Wedge: top-right quadrant only, apex at the ring's centre. Rows
    // 0..CHRONO_R_OUT-1 are exactly the upper half of the same grid (dy <=
    // 0), and s_chronoHwInner already holds the inner circle's half-width
    // there, growing from 0 at the top to close to CHRONO_R_IN at the row
    // touching the centre - one bar per row, from the centre out to that
    // width, never past the ring's own inner edge.
    for (int row = 0; row < CHRONO_R_OUT; row++) {
        int hw = s_chronoHwInner[row];
        if (hw <= 0) continue;
        int y = ringTop + row;
        gfx_fill_rect_land(ccx, y, hw, 1, color);
    }

    // Crown and neck: plain rectangles, no curve involved.
    gfx_fill_rect_land(ccx - CHRONO_CROWN_W / 2, top, CHRONO_CROWN_W, CHRONO_CROWN_H, color);
    gfx_fill_rect_land(ccx - CHRONO_NECK_W / 2, top + CHRONO_CROWN_H, CHRONO_NECK_W, CHRONO_NECK_H, color);

    // Tab: a small diamond (a square rotated 45 degrees) near one-thirty,
    // offset clear of the ring - the "angled" reading the reference has,
    // built the same bars-per-row way as everything else here but with
    // Manhattan rather than Euclidean distance, since a diamond's edges are
    // straight lines and need no sqrt.
    int tcx = ccx + CHRONO_TAB_OFF;
    int tcy = ccy - CHRONO_TAB_OFF;
    for (int dy = -CHRONO_TAB_R; dy <= CHRONO_TAB_R; dy++) {
        int hw = CHRONO_TAB_R - (dy < 0 ? -dy : dy);
        if (hw <= 0) continue;
        gfx_fill_rect_land(tcx - hw, tcy + dy, 2 * hw, 1, color);
    }
}

/* ---------------------------------------------------------------------
 * The DRAW icon: a fast freehand squiggle, per the owner's sketch
 * (icon-draw2.png) and their follow-up correction in words: it must travel
 * top-left to bottom-right across the box, and it must look drawn, not
 * constructed - the test the owner set is literal ("does it look like
 * something a person drew quickly, or like a symbol someone built").
 *
 * A first version drew this as a straight-segment polyline (still true of
 * shapes_fill_thick_segment_land, kept below for whatever future icon
 * wants a plain diagonal) and failed that test twice: once because its
 * corners sat within a few degrees of dead vertical/horizontal and read as
 * a lightning bolt, and again, after hand-tuning the angles, because
 * straight segments at a constant 12px thickness read as a deliberate "Z"
 * rather than ink - a chain of facets is still a chain of facets no matter
 * how their angles are chosen. What was missing was curvature and taper,
 * not better corners.
 *
 * Both come from firmware/apps/sketch.c, which solved this exact problem
 * for the live pen tool:
 *
 *   - CURVE: sketch.c's draw_quad_midpoint() replaces each straight facet
 *     between three consecutive points with a quadratic Bezier through
 *     their midpoints, so consecutive curve segments meet exactly at a
 *     shared midpoint and the whole path reads as one continuous curve.
 *     shapes_fill_tapered_quad_land() below is that same construction as a
 *     reusable shapes.h helper (rectangle stamps instead of anti-aliased
 *     circles, since this panel is monochrome and needs no AA); this
 *     function calls it once per interior waypoint (P1, P2, P3), each time
 *     with that waypoint as the control point and the midpoints on either
 *     side of it as the segment's own endpoints - see shapes.c's header
 *     comment on shapes_fill_tapered_quad_land for exactly how that chains.
 *     P0->first-midpoint and last-midpoint->P4 are drawn straight, the same
 *     lead-in/lead-out sketch.c's stroke_begin()/stroke_end() use before
 *     enough history exists for a curve.
 *
 *   - TAPER: sketch.c's pressure model varies the pen radius continuously
 *     (thin at a stroke's start and end, fullest through a fast middle).
 *     There is no touch speed to read here, so the five waypoints below
 *     each carry a fixed radius instead - thin (2px) at P0, ramping up to
 *     fullest (4px) at P2, back down to thin (1px, deliberately narrower
 *     than the start - an asymmetric taper reads more like a hand lifting
 *     the pen than a symmetric one) at P4 - and
 *     shapes_fill_tapered_quad_land/shapes_fill_tapered_segment_land
 *     interpolate between those linearly along the path, the same way
 *     draw_capsule interpolates r0->r1 across sketch.c's own spans.
 *
 * A second pass at these numbers (curve and taper both already in place)
 * still failed the owner's test: at a peak HALF-width of 8px the stroke
 * was 16px thick, a sixth of the icon's own width, and the wiggles sat
 * close enough together that consecutive passes touched and merged into
 * one mass - a squiggle only reads as a squiggle because of the white
 * BETWEEN its passes, and there was almost none. This device's real pen is
 * 5px wide (sketch.c's PEN_SIZE); an icon representing it at 16px was also
 * just wrong. Fixed with both levers at once: the peak half-width dropped
 * to 4px (this device's own pen, roughly, rather than something invented
 * for the icon), and the waypoints below were pushed out to use the whole
 * box - P1 and P3 now sit within a few px of the right edge, P0 and P2
 * within a few px of the left - which is what actually buys the white:
 * a thinner stroke alone does nothing if the passes still overlap. Checked
 * by scanning the rendered framebuffer, not by eye - a vertical line
 * through the icon's centre column, at 1x, crosses the stroke three times
 * on its way down and gives run lengths (top to bottom, W=white/B=black,
 * px) of W11 B5 W11 B8 W19 B7 W35: every white gap (11px, then 19px) comes
 * out wider than the ink on either side of it (5-8px), 1.4x to 3.8x over,
 * not just barely clearing the bar.
 *
 * The five waypoints are otherwise unchanged in spirit from the earlier
 * versions: unequal wiggle amplitudes and unequal segment lengths on
 * purpose (a regular zigzag reads as a waveform, not a hand), travelling
 * top-left to bottom-right. Fractions of ICON_W/ICON_H (0..100), not
 * literal pixel counts, so the glyph rescales if the tile geometry above
 * ever changes; the radii are left as plain pixel counts, same as every
 * other icon's stroke widths in this file, since ICON_W/ICON_H are 96 in
 * every build this device ships (see the chrono icon's header comment on
 * the same choice).
 * ------------------------------------------------------------------- */
static void draw_icon_sketch(int ox, int oy, uint16_t color) {
    static const int fx[5] = { 8, 90, 8, 92, 84 };
    static const int fy[5] = { 8, 20, 48, 72, 94 };
    static const int fr[5] = { 2, 3, 4, 3, 1 };
    int px[5], py[5], pr[5];
    for (int i = 0; i < 5; i++) {
        px[i] = ox + fx[i] * ICON_W / 100;
        py[i] = oy + fy[i] * ICON_H / 100;
        pr[i] = fr[i];
    }

    // Midpoints between consecutive waypoints: the curve's actual segment
    // endpoints (see the header comment above and shapes.c's comment on
    // shapes_fill_tapered_quad_land). Radius at a midpoint is the average
    // of its two waypoints' radii, matching sketch.c's own ar/dr in
    // draw_quad_midpoint.
    int mx01 = (px[0] + px[1]) / 2, my01 = (py[0] + py[1]) / 2, mr01 = (pr[0] + pr[1]) / 2;
    int mx12 = (px[1] + px[2]) / 2, my12 = (py[1] + py[2]) / 2, mr12 = (pr[1] + pr[2]) / 2;
    int mx23 = (px[2] + px[3]) / 2, my23 = (py[2] + py[3]) / 2, mr23 = (pr[2] + pr[3]) / 2;
    int mx34 = (px[3] + px[4]) / 2, my34 = (py[3] + py[4]) / 2, mr34 = (pr[3] + pr[4]) / 2;

    shapes_fill_tapered_segment_land(px[0], py[0], pr[0], mx01, my01, mr01, color);
    shapes_fill_tapered_quad_land(mx01, my01, mr01, px[1], py[1], mx12, my12, mr12, color);
    shapes_fill_tapered_quad_land(mx12, my12, mr12, px[2], py[2], mx23, my23, mr23, color);
    shapes_fill_tapered_quad_land(mx23, my23, mr23, px[3], py[3], mx34, my34, mr34, color);
    shapes_fill_tapered_segment_land(mx34, my34, mr34, px[4], py[4], pr[4], color);
}

/* ---------------------------------------------------------------------
 * The TIMER icon: an hourglass with sand in the TOP chamber only, the
 * bottom drawn empty. The asymmetry is the whole point (time still to
 * come) and is kept unchanged from the previous version - see the sand-
 * drawing code below for why it stays solid, not hatched.
 *
 * What changed: the owner's words were "l'icone de sablier est ultra
 * moche. elle devrait etre plus ronde" against a reference photo of a real
 * hourglass (see this file's task brief) - two ROUND, near-spherical
 * bulbs meeting at a narrow waist, not the previous version's two hard-
 * edged triangles (a schematic symbol, not an object).
 *
 * MIRROR SYMMETRY ABOUT THE WAIST is what makes an hourglass legible at a
 * glance: the top chamber is widest at its cap and narrows going down to
 * the waist; the bottom chamber must be the exact opposite - narrowest at
 * the waist, widening going down to its own cap. s_timerBulbHw[] below is
 * filled ONCE for that shape (row 0 = cap/wide, last row = waist/narrow)
 * and the bottom chamber's drawing loop reads it BACKWARDS (index
 * TIMER_CHAMBER_H-1-row), so row 0 of the bottom chamber (right under the
 * waist) reads the table's LAST entry (narrow) and its last row (the
 * bottom cap) reads the table's FIRST entry (wide) - see the bottom loop
 * below. Verified by measuring the actual rendered framebuffer, not just
 * by eye: half-width at the bottom chamber's own rows is strictly
 * increasing from the waist down to the cap, never the other way - this
 * was got wrong once during development (mirrored correctly on paper, but
 * the first rendered version still read as a funnel - see the next
 * paragraph for why that was a curve-SHAPE bug, not a mirroring one) and
 * is worth re-checking this way, not by eye, if this shape is ever
 * retuned again.
 *
 * A FIRST version of this curve (a single circle, one radius sampled off
 * its own lower arc - see this file's git history) got the mirroring
 * above right but still read as a funnel, not a bulb, for a reason that
 * has nothing to do with which end is wide: a circle is locally FLAT right
 * at its own peak and increasingly STEEP away from it, and forcing the
 * peak to sit exactly at row 0 (the only way a single circle can reach a
 * chamber this tall - see below) put all of that flatness at the wide end
 * and all of the steepness at the narrow end. Flat-then-steep is exactly
 * the silhouette of a funnel or a martini glass, not a sphere, regardless
 * of which end is which - a ball's curvature is closer to symmetric on
 * both sides of its widest point. Confirmed both by eye against the
 * reference photo and numerically: a circle's own half-width formula is
 * hw(row) = sqrt(R^2 - dy^2), whose slope near dy=0 (the peak) is
 * approximately -dy/R - near zero when dy is small relative to R, which it
 * necessarily is when R has to be almost as large as TIMER_HALF_OUT itself
 * just to reach TIMER_HALF_NECK within the chamber's own height (see next
 * paragraph) - so "near the peak" ends up covering most of the visible cap.
 *
 * Why the peak cannot just be moved inside the window: reaching from a
 * half-width of TIMER_HALF_OUT down to TIMER_HALF_NECK on a circle of
 * radius R takes very close to R rows of descent (exactly sqrt(R^2 -
 * TIMER_HALF_NECK^2), which is within a few percent of R whenever
 * TIMER_HALF_NECK is small next to R, as it is here). At the dimensions
 * this was first found against (a 34-row chamber, TIMER_HALF_OUT=40) that
 * was roughly 39-40 rows on its own - already more than the chamber's own
 * height, with nothing left over to also spend rows on a rounded shoulder
 * above the peak. Tried numerically before settling on the fix below:
 * moving the peak 10 rows inside that window while still hitting both
 * endpoints exactly forced a radius wider than TIMER_HALF_OUT by under
 * half a pixel - an invisible bulge bought at a real cost in code
 * complexity, for nothing. Growing the chamber a little (roughly +20%) was
 * the other lever tried at the time, and was rejected too, for spending
 * its rows on a still sub-pixel bulge rather than on anything the icon
 * keeps - the chamber eventually DID grow, but for a different reason (see
 * "PROPORTIONS" below), and by enough (34 -> 44 rows, nearly +30%) that
 * the bulge this same algebra produces is no longer sub-pixel: worth
 * knowing if the exact numbers here ever get re-tuned again, since the
 * tradeoff that ruled this lever out originally no longer holds.
 *
 * The fix: an ELLIPSE, not a circle. A circle has exactly one radius,
 * which fixes its width AND how many rows it takes to reach any given
 * narrower width TOGETHER - that coupling is the actual problem above, not
 * the chamber height. An ellipse has two independent axes, so the bulge's
 * PEAK WIDTH and how far it takes to fall from the peak to each endpoint
 * can be solved separately. hw(row) = A*sqrt(1 - ((row-P)/B)^2), with the
 * peak position P chosen directly (TIMER_BULB_PEAK_ROW, about 3/10 of the
 * way down - close to where the reference photo's own widest point sits)
 * and (A, B) solved from the two endpoints this shape must still hit
 * exactly (hw(0) = TIMER_HALF_OUT, hw(TIMER_CHAMBER_H-1) =
 * TIMER_HALF_NECK) - see ensure_timer_bulb_table() for the two-line
 * derivation. The overshoot past TIMER_HALF_OUT this buys is real and
 * visible, not the sub-pixel one a circle was stuck with, and the curve is
 * close to symmetric in shape on both sides of that peak the way a ball's
 * is.
 *
 * PROPORTIONS: an hourglass is TALL and NARROW - being that shape is as
 * much a part of reading as "hourglass" as the waist is, and an early pass
 * at this icon (TIMER_MARGIN=12, TIMER_HALF_OUT=40, TIMER_CHAMBER_H=34)
 * measured out at roughly 90px wide by 70px tall on the rendered
 * framebuffer, wider than it was tall - two stacked bowls, not an
 * hourglass. Fixed by pulling both available levers at once rather than
 * either alone: TIMER_MARGIN dropped close to 0 so the whole ICON_H is
 * spent on the glass instead of a lot of it sitting in unused top/bottom
 * padding, and TIMER_HALF_OUT/TIMER_HALF_NECK both came down so the
 * chambers are slimmer. Measured on the current numbers below: ink height
 * (top cap to bottom cap) 92px, ink width (at the widest bulge) about
 * 66px - a 0.72 width/height ratio, against the reference photo's own
 * "distinctly taller than wide" and the brief's 0.7 target.
 * ------------------------------------------------------------------- */
#define TIMER_MARGIN    (ICON_H / 48)                             // 2
#define TIMER_HALF_OUT  (ICON_W * 7 / 24)                          // 28
#define TIMER_HALF_NECK 5
#define TIMER_NECK_H    (ICON_H / 24)                              // 4
#define TIMER_CHAMBER_H ((ICON_H - 2 * TIMER_MARGIN - TIMER_NECK_H) / 2) // 44
#define TIMER_OUTLINE   (ICON_W / 24)                              // 4

// Where the bulb's TRUE widest point sits, measured down from the cap -
// see this block's header comment for why this needs to be a free choice
// (an ellipse, not a circle) rather than something solved for. 3/10 of the
// way down is close to where the reference photo's own bulge sits; 13 rows
// at today's (taller) chamber.
#define TIMER_BULB_PEAK_ROW (TIMER_CHAMBER_H * 3 / 10)

static int16_t s_timerBulbHw[TIMER_CHAMBER_H]; // row 0 = cap (wide), last = waist (narrow)
static bool s_timerBulbReady = false;

static void ensure_timer_bulb_table(void) {
    if (s_timerBulbReady) return;

    // hw(row) = A*sqrt(1 - ((row-P)/B)^2), P = TIMER_BULB_PEAK_ROW fixed.
    // Squaring the two endpoint conditions (hw(0) = TIMER_HALF_OUT,
    // hw(m) = TIMER_HALF_NECK, m = TIMER_CHAMBER_H-1) and subtracting
    // cancels the A^2/B^2 cross term down to one unknown, v = A^2/B^2; A
    // and B follow from there. See this block's header comment for why
    // this replaces a single circle (which cannot decouple the bulge's
    // width from how many rows it takes to reach it, and so cannot place
    // its own peak inside the window without an invisible, sub-pixel
    // bulge).
    float outF = (float)TIMER_HALF_OUT, neckF = (float)TIMER_HALF_NECK;
    float p = (float)TIMER_BULB_PEAK_ROW;
    float m = (float)(TIMER_CHAMBER_H - 1);

    float v = (outF * outF - neckF * neckF) / ((m - p) * (m - p) - p * p);
    float aSq = outF * outF + v * p * p;
    float a = sqrtf(aSq);
    float b = sqrtf(aSq / v);

    // ICON_W/2 - 1: the icon's own box edge, a hard safety clamp so a
    // future change to TIMER_BULB_PEAK_ROW or the chamber's proportions
    // cannot silently draw the bulge's peak past the icon's own bounding
    // box instead of just making it visibly too fat (an obvious bug, not a
    // corrupted one).
    int maxHw = ICON_W / 2 - 1;

    for (int row = 0; row < TIMER_CHAMBER_H; row++) {
        float dy = (float)row - p;
        float t = dy / b;
        float underRoot = 1.0f - t * t;
        float hwF = underRoot > 0.0f ? a * sqrtf(underRoot) : 0.0f;
        int16_t hw = (int16_t)(hwF + 0.5f);
        if (hw < TIMER_HALF_NECK) hw = TIMER_HALF_NECK; // guards the waist
                                                          // end against
                                                          // float rounding
                                                          // only - the
                                                          // algebra targets
                                                          // this exactly.
        if (hw > maxHw) hw = maxHw;
        s_timerBulbHw[row] = hw;
    }
    s_timerBulbReady = true;
}

static void draw_icon_timer(int ox, int oy, uint16_t color) {
    ensure_timer_bulb_table();

    int cx = ox + ICON_W / 2;
    int top = oy + TIMER_MARGIN;

    // Top chamber: solid fill, the sand. Row 0 is the cap
    // (s_timerBulbHw[0] == TIMER_HALF_OUT), bulging out WIDER still by
    // TIMER_BULB_PEAK_ROW before curving back in and down to the waist
    // (s_timerBulbHw[last] == TIMER_HALF_NECK) - see this block's header
    // comment for why the peak sits past the cap's own width instead of
    // exactly at it.
    for (int row = 0; row < TIMER_CHAMBER_H; row++) {
        int hw = s_timerBulbHw[row];
        gfx_fill_rect_land(cx - hw, top + row, 2 * hw, 1, color);
    }

    // Neck: a short solid connector. It closes the bottom of the sand and
    // the top of the empty chamber in the same rectangle, which is what
    // gives the glass a pinch point instead of two chambers meeting at a
    // single pixel.
    int neckY = top + TIMER_CHAMBER_H;
    gfx_fill_rect_land(cx - TIMER_HALF_NECK, neckY, 2 * TIMER_HALF_NECK, TIMER_NECK_H, color);

    // Bottom chamber: outline only - nothing has fallen yet. Mirrored: its
    // own row 0 (right below the neck) is the NARROW end, so it reads
    // s_timerBulbHw in reverse (index TIMER_CHAMBER_H-1-row), same table
    // the top chamber used, just walked the other way.
    //
    // Each edge is a march of shapes_fill_thick_segment_land() calls
    // connecting one row's point to the next, not an independent bar per
    // row: right past the waist this curve's own half-width can jump more
    // per row than TIMER_OUTLINE is wide (the bulge rises fast right after
    // the pinch - see this block's header comment on why), and a bar drawn
    // only at each row's own x, with nothing bridging the horizontal gap to
    // the next row's, breaks the outline into visible dashes exactly there.
    // Confirmed empirically: rendering the old per-row-bar version showed a
    // real gap on both edges a few rows past the neck, not just a
    // theoretical risk. A marched thick segment is the same fix
    // shapes_fill_thick_segment_land's own header comment already
    // describes solving for a diagonal at any angle - see there for why its
    // stamps are spaced to always overlap.
    int bottomTop = neckY + TIMER_NECK_H;
    int prevHw = s_timerBulbHw[TIMER_CHAMBER_H - 1]; // this chamber's own row 0 (waist)
    for (int row = 1; row < TIMER_CHAMBER_H; row++) {
        int hw = s_timerBulbHw[TIMER_CHAMBER_H - 1 - row];
        int y0 = bottomTop + row - 1, y1 = bottomTop + row;
        shapes_fill_thick_segment_land(cx - prevHw, y0, cx - hw, y1, TIMER_OUTLINE, color);
        shapes_fill_thick_segment_land(cx + prevHw, y0, cx + hw, y1, TIMER_OUTLINE, color);
        prevHw = hw;
    }
    gfx_fill_rect_land(cx - TIMER_HALF_OUT, bottomTop + TIMER_CHAMBER_H - TIMER_OUTLINE,
                        2 * TIMER_HALF_OUT, TIMER_OUTLINE, color);

    // A couple of grains just past the neck, already fallen into the empty
    // chamber - small enough to read as grains rather than a blob at this
    // size (see this block's header comment: hatching the sand itself was
    // tried and rejected the same way, for the same reason).
    gfx_fill_rect_land(cx - 2, bottomTop + 6, 3, 3, color);
    gfx_fill_rect_land(cx - 1, bottomTop + 15, 2, 2, color);
}

static void draw_icon_for(const app_t *app, int ox, int oy, uint16_t color) {
    if (app == &g_chronoApp) draw_icon_chrono(ox, oy, color);
    else if (app == &g_sketchApp) draw_icon_sketch(ox, oy, color);
    else if (app == &g_timerApp) draw_icon_timer(ox, oy, color);
    // An app added to g_apps[] without a matching icon here draws nothing -
    // a silent gap, not a fault. The menu is a navigation aid; it must
    // never be the thing that stops a build. There is no name to fall back
    // to any more either (see this file's header comment): an app with no
    // icon is simply an empty column, still fully touchable by position.
}

/* ---------------------------------------------------------------------
 * Painting. Icons are drawn exactly once, from menu_enter(): there is no
 * selection state any more (see menu_state_t below), so there is nothing
 * that ever changes about the screen after the initial draw - a touch
 * launches immediately and the arena is reset by the switch before another
 * frame could ever run, so there is never a "repaint just this icon"
 * case the way the old bordered-cursor design needed. menu_enter() does
 * not need to push either: the runtime pushes the whole panel once after
 * enter() returns (see app.h).
 * ------------------------------------------------------------------- */
static void paint_icon(int i) {
    int bx, bw;
    column_rect_land(i, &bx, &bw);
    int iconX = bx + (bw - ICON_W) / 2;
    int iconY = ICON_TOP_MARGIN;
    draw_icon_for(g_apps[i], iconX, iconY, PX_BLACK);
}

static void menu_paint_all(void) {
    for (int i = 0; i < g_appCount; i++) paint_icon(i);
}

/* ---------------------------------------------------------------------
 * State and the app_t callbacks.
 * ------------------------------------------------------------------- */

// Just one flag: whether the current touch (if any) has already launched
// something. There is deliberately no "which icon is selected" field any
// more - the old cursor's whole reason for existing was to make a
// button-driven "move, then confirm" gesture legible (see this file's
// header comment on the removed BOOT/PWR path), and a touch-only menu has
// no such two-step gesture to make legible: a touch IS the confirmation,
// the instant it lands. Removing the cursor is a simplification, not a
// loss - the child never chose "which one is highlighted" as a separate
// step even in the old design (tapping a tile always launched immediately
// too), so no capability goes away with the state that used to back it.
typedef struct {
    bool armed;
} menu_state_t;

static menu_state_t *s_menu;

// Called by the runtime (see menu.h) before it switches into the menu, so
// the runtime can tell menu_enter() which app to default a selection
// cursor to. That cursor is gone (see menu_state_t's comment above), so
// this file has nothing left to do with the value - but the function stays
// and keeps accepting it regardless: runtime_core.c calls this
// unconditionally on every chord that opens the menu (see its own "BOOT+PWR
// long-press chord" comment) and this file is not the one that gets to
// change that contract (see this file's header comment on scope).
void menu_set_return_app(int index) {
    (void)index;
}

static void menu_enter(void) {
    s_menu = APP_STATE(menu_state_t);
    menu_paint_all();
}

static void menu_tick(const app_frame_t *f) {
    // Touch only, per this file's header comment - no BOOT cursor, no
    // PWR-short launch. f->touchDown is a LEVEL (true for every tick a
    // finger is down), not an edge, so s_menu->armed is this file's own
    // latch: launch on the first tick a touch is seen, then stay quiet
    // until the finger lifts, so a touch that drags across several columns
    // while still down cannot launch a second app out from under the
    // first switch. Reading the level rather than app_frame_t's own
    // touchPressed edge is deliberate hardening, not a style choice: this
    // is now the ONLY way to launch anything (see header comment), so it
    // should not depend on catching one specific tick's edge if a future
    // touch-resolution change ever makes that edge less reliable than the
    // level it is derived from.
    if (f->touchDown) {
        if (!s_menu->armed) {
            s_menu->armed = true;
            int lx, ly;
            panel_to_land(f->touchX, f->touchY, &lx, &ly);
            (void)ly; // the touch target is a full-height column - see
                      // column_hit_test()'s comment - so only which column
                      // lx falls in decides the hit, never ly.
            int hit = column_hit_test(lx);
            // Launches immediately: see this file's header comment on why
            // a touch resolves "confirm" on its own, with no separate
            // step. The arena is about to be reset by the switch, so there
            // is nothing worth repainting here first.
            app_switch_to(hit);
            return;
        }
    } else {
        s_menu->armed = false;
    }
}

const app_t g_menuApp = {
    .name       = "MENU",
    .enter      = menu_enter,
    .tick       = menu_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
