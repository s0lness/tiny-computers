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
 * The chrono icon: THIRD PASS, 2026-08-14. The owner's verdict on round 2
 * (a plain annulus plus one crown flick, commit e8e1548 - the ring/wedge/
 * crown-rect/neck-rect/tab-diamond silhouette replaced rather than fixed):
 * "j'aimais bien celui qu'on avait avant" - he preferred the OLD icon,
 * still on the device at the time he judged this. The coordinator's read,
 * which holds up: the filled WEDGE is what said "a dial with something on
 * it"; a plain ring with a nub reads as a ring with a nub.
 *
 * So the old silhouette is back - ring, wedge, crown+neck, tab - but built
 * two ways round 1 never was:
 *
 *   - "des traits moins droits" (less straight lines): round 1's wedge had
 *     one dead-straight vertical edge, its crown+neck were two exact
 *     rectangles, and its tab was an exact diamond. All three are now
 *     HAND-WOBBLED constant tables instead - see tools/gen-chrono-icon.ts,
 *     which generates them from a seeded RNG smoothed through tldraw's own
 *     streamline pass (ingest(), tools/tldraw-freehand/core.ts - the
 *     vendored pipeline), the same technique gen-strokes.ts already uses
 *     to humanise Hershey letterforms. Baked as fixed constants rather
 *     than recomputed on device, the same call draw_icon_sketch's own
 *     fx/fy/fr waypoints already made: a wobble chosen once at design
 *     time, not a device-cycle expense.
 *   - "les pixels qui sortent" (the rule that also cost the hourglass its
 *     two floating grains): round 1's tab was held deliberately
 *     CHRONO_TAB_GAP px "clear of the ring" - it floated, touching
 *     nothing, on purpose. Rebuilt below as a short connected stroke
 *     chain whose BASE DISC is centred on the ring's own painted band
 *     (CHRONO_R_MID) - the exact join technique round 2's crown flick
 *     already proved (the base overlaps the ring's own solid ink before
 *     MIN composition ever has to decide anything, so there is no seam to
 *     trap), now used for both the crown and the tab.
 *
 * The ring itself is unchanged from round 2: shapes_fill_annulus_aa_land,
 * a true circle. It has no straight edge for "moins droits" to apply to
 * and it was never the part that floated.
 * ------------------------------------------------------------------- */
#define CHRONO_R_OUT       (ICON_W * 17 / 48)                    // 34
#define CHRONO_STROKE      10                                    // ring band width - "pretty thick"
#define CHRONO_R_IN        (CHRONO_R_OUT - CHRONO_STROKE)        // 24
#define CHRONO_R_MID       ((CHRONO_R_OUT + CHRONO_R_IN) / 2)    // 29, the ring's own centreline
#define CHRONO_CROWN_LEN   14                                    // crown path's own extent, see s_chronoCrownDy

// Crown tip to ring bottom, centred in ICON_H.
#define CHRONO_COMP_H (CHRONO_R_MID + CHRONO_CROWN_LEN + CHRONO_R_OUT) // 77

// Wedge: top-right quarter of the INNER disc (radius CHRONO_R_IN), apex at
// the ring's own centre - needs the inner circle's own per-row half-widths
// for its curved edge, same table shapes.h's own header comment says
// timer.c also depends on (shapes_fill_half_width_table itself is generic
// and stays regardless of what this file does with it).
static int16_t s_chronoHwInner[2 * CHRONO_R_OUT];
static bool s_chronoTablesReady = false;

static void ensure_chrono_tables(void) {
    if (s_chronoTablesReady) return;
    shapes_fill_half_width_table(s_chronoHwInner, 2 * CHRONO_R_OUT, (float)CHRONO_R_IN);
    s_chronoTablesReady = true;
}

// The wedge's one straight edge, hand-wobbled - tools/gen-chrono-icon.ts,
// seed 20260814. One entry per row of the wedge (CHRONO_R_OUT - rowStart
// rows; works out to 24 at today's CHRONO_R_OUT/CHRONO_R_IN), added to ccx
// in place of the old dead-straight `leftX[i] = ccx`. Indexed defensively
// in draw_icon_chrono() below in case CHRONO_R_OUT/CHRONO_R_IN are ever
// retuned without regenerating this table.
// REPLACED 2026-08-14 after the owner saw it rendered: "the part in the
// middle of the chronometer is fucking horrible". He was right, and the
// cause is worth writing down because it will recur.
//
// The generated wobble changed by one pixel from one row to the next, with
// six pixels of amplitude over twenty-four rows, and
// shapes_fill_between_curves_aa_land takes integer column arrays, so it has
// no sub-pixel edge to anti-alias against. A per-row jitter under those
// conditions cannot look like a hand: it can only look like a saw, which is
// exactly what it looked like. Randomness is not the same thing as
// handwriting, and at this scale it reads as a tear.
//
// STRAIGHT, and this is now a decision rather than the absence of one.
//
// The gentle arc that replaced the jitter was better and still wrong: the
// owner, looking at it magnified, said the middle "a l'air de s'effriter".
// It did. Six one-pixel steps is fewer than twenty, but on a curve this
// tight every one of them is a visible notch.
//
// The reason is structural and worth stating so nobody tries a third
// amplitude: shapes_fill_between_curves_aa_land takes INTEGER column arrays,
// so this edge has no sub-pixel position to anti-alias against. A perfectly
// vertical edge is the only one that produces no steps at all, because it
// never has to change column. Any deviation, of any size, quantises into a
// staircase.
//
// So the hand-drawn quality lives where the primitives can carry it, in the
// crown and tab (float capsules, genuinely anti-aliased) and in the sketch
// and coil icons. Not here. If this edge is ever to bow, it needs a fill
// that accepts float edges, not a smaller number.
static const int8_t s_chronoWedgeWobble[24] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Crown path, hand-wobbled - tools/gen-chrono-icon.ts, same seed. Offsets
// from the base point (ccx, ccy - CHRONO_R_MID), +x right, +y down (this
// file's own landscape convention); the generator prints "+y = up" and
// this is its negation, already applied.
//
// SHORTENED after the first render (checked against the actual rendered
// PNG, not by eye on the numbers alone): at the original 20px reach with
// a taper down to a 2px point, this and the tab below read as two thin
// spikes off the top of the ring - "horns", not "a crown and a tab". See
// tools/gen-chrono-icon.ts's own "SHORTENED AND CALMED" comment for the
// shorter, gentler regeneration; draw_icon_chrono() below also stopped
// tapering either stroke all the way to a point, for the same reason.
#define CHRONO_CROWN_STEPS 6
static const float s_chronoCrownDx[CHRONO_CROWN_STEPS] = { 0.0f, -0.2f, -0.5f, -0.7f, -0.7f, -0.8f };
static const float s_chronoCrownDy[CHRONO_CROWN_STEPS] = { 0.0f, -2.7f, -5.4f, -8.1f, -10.9f, -13.6f };

// Tab path, hand-wobbled - tools/gen-chrono-icon.ts, same seed, same
// shortening as the crown above. Local frame: `along` is the outward
// radius direction at one-thirty (45 degrees clockwise from 12), `perp`
// is that direction rotated 90 degrees; s_chronoTabAlong/s_chronoTabPerp
// are the local (x, y) coordinates draw_icon_chrono() below projects into
// world space - `along` carries the reach (the large values), `perp` the
// small perpendicular wobble (a mislabelling of exactly this pair was
// caught and fixed in the same pass that shortened the reach: the first
// render's tab travelled mostly TANGENT to the ring instead of outward,
// because the generator's own printed column order - wobble first, reach
// second - had been transcribed into the wrong array).
#define CHRONO_TAB_STEPS 4
static const float s_chronoTabAlong[CHRONO_TAB_STEPS] = { 0.0f, 2.8f, 5.6f, 8.4f };
static const float s_chronoTabPerp[CHRONO_TAB_STEPS]  = { 0.0f, 0.2f, 0.4f, 0.5f };

static void draw_icon_chrono(int ox, int oy, uint16_t color) {
    ensure_chrono_tables();

    int top = oy + (ICON_H - CHRONO_COMP_H) / 2;
    int ccx = ox + ICON_W / 2;
    int ccy = top + CHRONO_R_MID + CHRONO_CROWN_LEN;
    float strokeHalf = (float)CHRONO_STROKE / 2.0f; // 5 - both the ring's own half-width and the
                                                       // radius every stroke below starts at, so
                                                       // every base disc sits inside the ring's ink

    // Dial: a true circular annulus, both edges anti-aliased, analytic -
    // no per-row table needed at all.
    shapes_fill_annulus_aa_land((float)ccx, (float)ccy, (float)CHRONO_R_OUT, (float)CHRONO_R_IN, color);

    // Wedge: top-right quarter of the inner disc, apex at the ring's own
    // centre - back from round 1, its curved edge from s_chronoHwInner
    // (padded outward by one pixel past the ring's own inner edge, the
    // same seam trap round 1 needed - two independent AA computations of
    // the "same" circle round to different sub-pixel edges otherwise, and
    // the ring's own AA fringe peeks through as scattered grey dots along
    // the seam), its straight edge now s_chronoWedgeWobble instead of a
    // constant ccx.
    {
        // PAINTED, not filled. Owner, drawing the shape he wanted straight
        // onto a magnified render: the L-shaped corner is "trop droite par
        // rapport au reste du dessin", and his line shows the two edges
        // bowing and the inner corner rounding off.
        //
        // The row fill could not do that at any amplitude, for the reason
        // recorded above s_chronoWedgeWobble: integer columns, no sub-pixel
        // edge, every deviation quantises into a staircase. So the wedge is
        // now swept with the float capsule primitive instead, the same brush
        // sketch.c draws with. Capsules from the apex out to the rim, one per
        // small angular step, merged by MIN composition into one mass.
        //
        // Three things fall out of that and all three are the point. Every
        // edge is genuinely anti-aliased, because the capsule is. The inner
        // corner is round by construction, since it is the apex cap. And the
        // radial edges can bow, because nothing here is quantised to a
        // column any more.
        //
        // Cost is about ninety capsule calls, paid once when the menu opens,
        // not per frame.
        const float wedgeR = (float)CHRONO_R_IN + 1.0f; // +1: the same seam overlap the row fill needed
        const float apexR = 4.0f;   // rounds the inner corner
        const float rimR = 1.6f;    // keeps the swept edge solid between steps
        const int wedgeSteps = 90;
        for (int i = 0; i <= wedgeSteps; i++) {
            float t = (float)i / (float)wedgeSteps;      // 0 at 12 o'clock, 1 at 3 o'clock
            float a = t * 90.0f * 3.14159265f / 180.0f;
            float x1 = (float)ccx + wedgeR * sinf(a);
            float y1 = (float)ccy - wedgeR * cosf(a);
            shapes_fill_capsule_aa_land((float)ccx, (float)ccy, apexR, x1, y1, rimR, color);
        }

        // The two radial edges, bowed outward. Kept as a separate pass on
        // purpose: an earlier attempt bowed the sweep itself by rotating each
        // capsule, which needed the rotation to change sign at the midpoint
        // and therefore left an eight degree wedge with no capsule in it at
        // all. Filling and outlining are two jobs, and one primitive doing
        // both is how that gap got in.
        //
        // A quadratic with its control point pushed perpendicular to the edge
        // is the bow. MIN composition merges these into the swept mass, so
        // the result is one piece of ink with slightly curved sides rather
        // than a shape with a stroke drawn around it.
        {
            const float bowPx = 3.0f;  // THE KNOB for how much the sides curve
            float rim = wedgeR;
            // Edge A: apex straight up to 12 o'clock, bowing left.
            shapes_fill_tapered_quad_aa_land((float)ccx, (float)ccy, apexR,
                                             (float)ccx - bowPx, (float)ccy - rim * 0.5f,
                                             (float)ccx, (float)ccy - rim, rimR, color);
            // Edge B: apex straight right to 3 o'clock, bowing down.
            shapes_fill_tapered_quad_aa_land((float)ccx, (float)ccy, apexR,
                                             (float)ccx + rim * 0.5f, (float)ccy + bowPx,
                                             (float)ccx + rim, (float)ccy, rimR, color);
        }
    }

    // Crown: a hand-wobbled stroke chain, base centred ON the ring's own
    // painted band so the join is unconditional (see this block's header
    // comment) - CHRONO_CROWN_STEPS-1 segments instead of round 2's one
    // straight flick. Tapers strokeHalf -> 3.5px, NOT to a point: a taper
    // all the way down is what made the first render's crown read as a
    // thin spike rather than a knob (see s_chronoCrownDx's own comment).
    {
        float baseX = (float)ccx, baseY = (float)ccy - (float)CHRONO_R_MID;
        float prevX = baseX, prevY = baseY, prevR = strokeHalf;
        for (int i = 1; i < CHRONO_CROWN_STEPS; i++) {
            float x = baseX + s_chronoCrownDx[i];
            float y = baseY + s_chronoCrownDy[i];
            float r = strokeHalf + (3.5f - strokeHalf) * (float)i / (float)(CHRONO_CROWN_STEPS - 1);
            shapes_fill_capsule_aa_land(prevX, prevY, prevR, x, y, r, color);
            prevX = x; prevY = y; prevR = r;
        }
    }

    // Tab: round 1's diamond, held deliberately "clear of the ring" - the
    // exact floating mark the owner named this pass ("les pixels qui
    // sortent"). Rebuilt as a short connected, hand-wobbled stroke chain:
    // base ON the ring's own band at one-thirty, same join technique as
    // the crown above, tip out near where the old tab sat. Tapers
    // strokeHalf -> 3px, same "not to a point" reasoning as the crown.
    {
        float alongX = 0.70710678f, alongY = -0.70710678f; // one-thirty, clockwise from 12
        float perpX = 0.70710678f, perpY = 0.70710678f;    // `along` rotated 90 degrees
        float baseX = (float)ccx + alongX * (float)CHRONO_R_MID;
        float baseY = (float)ccy + alongY * (float)CHRONO_R_MID;
        float prevX = baseX, prevY = baseY, prevR = strokeHalf;
        for (int i = 1; i < CHRONO_TAB_STEPS; i++) {
            float x = baseX + alongX * s_chronoTabAlong[i] + perpX * s_chronoTabPerp[i];
            float y = baseY + alongY * s_chronoTabAlong[i] + perpY * s_chronoTabPerp[i];
            float r = strokeHalf + (3.0f - strokeHalf) * (float)i / (float)(CHRONO_TAB_STEPS - 1);
            shapes_fill_capsule_aa_land(prevX, prevY, prevR, x, y, r, color);
            prevX = x; prevY = y; prevR = r;
        }
    }
}

/* ---------------------------------------------------------------------
 * The DRAW icon: a fast freehand squiggle, per the owner's sketch
 * (icon-draw2.png) and their follow-up correction in words: it must travel
 * top-left to bottom-right across the box, and it must look drawn, not
 * constructed - the test the owner set is literal ("does it look like
 * something a person drew quickly, or like a symbol someone built").
 *
 * A first version drew this as a straight-segment polyline (built from what
 * was then shapes_fill_thick_segment_land, a plain fixed-thickness stamped
 * diagonal - deleted along with the rest of shapes.h's first generation
 * once every caller in this file moved to anti-aliased primitives; see
 * shapes.h's header comment) and failed that test twice: once because its
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
 *     shapes_fill_tapered_quad_aa_land() below is that same construction as
 *     a reusable shapes.h helper, anti-aliased circles now rather than
 *     rectangle stamps (see shapes.h's header comment: this whole file
 *     used to draw everything as rectangles, which is exactly what read as
 *     a staircase rather than ink); this function calls it once per
 *     interior waypoint (P1, P2, P3), each time with that waypoint as the
 *     control point and the midpoints on either side of it as the
 *     segment's own endpoints - see shapes.c's header comment on
 *     shapes_fill_tapered_quad_aa_land for exactly how that chains.
 *     P0->first-midpoint and last-midpoint->P4 are drawn straight (with
 *     shapes_fill_capsule_aa_land), the same lead-in/lead-out
 *     sketch.c's stroke_begin()/stroke_end() use before enough history
 *     exists for a curve.
 *
 *   - TAPER: sketch.c's pressure model varies the pen radius continuously
 *     (thin at a stroke's start and end, fullest through a fast middle).
 *     There is no touch speed to read here, so the five waypoints below
 *     each carry a fixed radius instead - thin (2px) at P0, ramping up to
 *     fullest (4px) at P2, back down to thin (1px, deliberately narrower
 *     than the start - an asymmetric taper reads more like a hand lifting
 *     the pen than a symmetric one) at P4 - and
 *     shapes_fill_tapered_quad_aa_land/shapes_fill_capsule_aa_land
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

    shapes_fill_capsule_aa_land((float)px[0], (float)py[0], (float)pr[0],
                                 (float)mx01, (float)my01, (float)mr01, color);
    shapes_fill_tapered_quad_aa_land((float)mx01, (float)my01, (float)mr01,
                                      (float)px[1], (float)py[1],
                                      (float)mx12, (float)my12, (float)mr12, color);
    shapes_fill_tapered_quad_aa_land((float)mx12, (float)my12, (float)mr12,
                                      (float)px[2], (float)py[2],
                                      (float)mx23, (float)my23, (float)mr23, color);
    shapes_fill_tapered_quad_aa_land((float)mx23, (float)my23, (float)mr23,
                                      (float)px[3], (float)py[3],
                                      (float)mx34, (float)my34, (float)mr34, color);
    shapes_fill_capsule_aa_land((float)mx34, (float)my34, (float)mr34,
                                 (float)px[4], (float)py[4], (float)pr[4], color);
}

/* ---------------------------------------------------------------------
 * The TIMER icon: an hourglass. Sand in the TOP bulb only, the bottom
 * empty - that asymmetry is the whole point (time still to come).
 *
 * WEIGHT DISTRIBUTION - read this before touching fill vs outline here
 * again. An earlier anti-aliased pass filled the WHOLE top chamber solid
 * and left the bottom as an outline, on a literal reading of the owner's
 * "que des aplats de couleur... comme si tout etait fait avec de l'encre"
 * (only flat areas of colour, as if everything were made with ink) as "no
 * outlines, only fills". Rendered, it read as an egg cup or a goblet: a
 * heavy dome sitting on a wire dome, not an hourglass, because a filled
 * bulb reads as the OBJECT rather than as glass with something inside it.
 * The correction: an anti-aliased stroke is ALSO an aplat - a solid band
 * of ink with a clean, soft edge, same as a filled region, just narrower.
 * What the owner was rejecting was the jagged PIXEL STAIRCASE the old
 * rectangle-bar renderer produced on every curve and diagonal (shapes.h's
 * whole second generation exists to fix exactly that), not the concept of
 * an outline. Do not re-fill the bulbs on the strength of that quote alone
 * - the fix for "petits pixels nuls" was anti-aliasing, already done; this
 * paragraph is the record of that being re-litigated once already.
 *
 * So: BOTH bulbs are glass, drawn identically (same stroke weight, mirror
 * images about the waist - see MIRROR SYMMETRY below, unchanged). The SAND
 * is a separate, solid shape sitting INSIDE the top bulb's glass, in its
 * lower portion only (see "THE SAND" below) - thin glass holding a heavy,
 * solid mass is what actually reads as an hourglass: you can see the sand
 * sitting in the glass, rather than the sand and the glass being the same
 * material.
 *
 * MIRROR SYMMETRY ABOUT THE WAIST - two ROUND, near-spherical bulbs, not
 * hard-edged triangles (a schematic symbol, not an object); unchanged from
 * the original owner correction ("l'icone de sablier est ultra moche. elle
 * devrait etre plus ronde") against a reference photo of a real hourglass.
 * This is what makes an hourglass legible at a glance: the top chamber is
 * widest at its cap and narrows going down to
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
 *
 * REDRAWN 2026-08-14, ink-strokes pass. The owner's rule this pass named
 * this icon specifically: "the hourglass must have no loose grain of
 * sand." The two individual falling-grain discs that used to sit just
 * below the neck are deleted (see that call site's own comment) - they
 * floated, touching nothing, which is exactly what the rule forbids; the
 * heap resting on the bottom cap already carries "sand has landed"
 * without a mark that has to float to say it. TIMER_OUTLINE also went
 * from 4px to 6px, for the "pretty thick, confident, Ink and Switch"
 * weight the other two icons now carry - kept short of THEIR stroke
 * width on purpose, since this icon's own argument above is that thin
 * glass around a solid sand mass is what reads as an hourglass rather
 * than an egg cup; matching the other icons' full boldness here would
 * refight that argument.
 *
 * The owner has never liked this object ("l'icone de sablier est ultra
 * moche") and this pass asked, on top of the pixel-level fix, whether an
 * hourglass is even the right choice any more - the timer app itself
 * stopped showing one when it became a coil dial (docs/decisions/
 * 0002-runtime-architecture.md, "the coil"). draw_icon_timer_coil()
 * below is the proposed alternative: same "one continuous piece of ink"
 * rule, a different object. Both are wired to compile; TIMER_ICON_USE_COIL
 * (see draw_icon_for) picks which one actually paints. See this icon's
 * task report for a rendered side-by-side and which the agent preferred.
 * ------------------------------------------------------------------- */
#define TIMER_MARGIN    (ICON_H / 48)                             // 2
#define TIMER_HALF_OUT  (ICON_W * 7 / 24)                          // 28
#define TIMER_HALF_NECK 5
#define TIMER_NECK_H    (ICON_H / 24)                              // 4
#define TIMER_CHAMBER_H ((ICON_H - 2 * TIMER_MARGIN - TIMER_NECK_H) / 2) // 44
#define TIMER_OUTLINE   (ICON_W / 16)                              // 6, bumped from 4 2026-08-14 for
                                                                     // the "pretty thick, confident"
                                                                     // ink-strokes pass - see this
                                                                     // block's header comment.

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

// RENDERING: anti-aliased (shapes.h's second generation). BOTH bulbs are
// GLASS - a stroke of the same weight, TIMER_OUTLINE wide, marched along
// s_timerBulbHw's curve with shapes_fill_capsule_aa_land (chained so a
// capsule's own round cap keeps consecutive stamps merged into one
// continuous band, the same argument shapes_fill_capsule_aa_land's own
// header comment makes - right past the waist this curve's own half-width
// can jump more per row than TIMER_OUTLINE is wide, and a bar drawn only
// at each row's own x, with nothing bridging to the next row's, breaks the
// outline into visible dashes exactly there, confirmed on an old, non-AA
// version of this icon - see git history). The two bulbs are mirror images
// of the same table (row 0 = cap/wide, last row = waist/narrow - see
// MIRROR SYMMETRY above): the top chamber walks it forwards from the cap,
// the bottom walks it backwards from the waist, same table either way.
//
// THE OVERFLOW FIX applies to BOTH bulbs now, not just the bottom: a
// stroke's rounded end sticks out past its own last sample point by the
// stroke's own half-width, the same way any capsule's round cap always
// does. Marching a curve all the way to its own cap row would let that
// overhang poke straight through TIMER_MARGIN (measured on an earlier,
// bottom-only version of this fix: top margin exactly TIMER_MARGIN=2px
// while the top chamber was still a flat fill with no overhang to cause
// this, bottom margin only 1px where it already was a stroke - an
// asymmetry with nothing conceptual behind it, just this overhang). Both
// bulbs now march their curve FROM the waist (safely mid-icon, never near
// a margin) TOWARD their own cap, stopping outlineR rows short so the
// round end always lands ON the flat cap rectangle rather than past it;
// the flat cap itself (unchanged position/size either way) covers the
// reserved gap. Checked on the rendered framebuffer, not by eye: with this
// fix, both margins come out TIMER_MARGIN, symmetric.
static void draw_icon_timer(int ox, int oy, uint16_t color) {
    ensure_timer_bulb_table();

    int cx = ox + ICON_W / 2;
    int top = oy + TIMER_MARGIN;
    int outlineR = TIMER_OUTLINE / 2; // 2, TIMER_OUTLINE is even; both the
                                       // stroke's half-width AND the row
                                       // budget reserved at each bulb's own
                                       // cap end - see THE OVERFLOW FIX.
    int curveRows = TIMER_CHAMBER_H - outlineR;

    // ---- TOP bulb: the glass. Flat cap first (axis-aligned, the sealed
    // rim - a hard edge on purpose, nothing to anti-alias), then the
    // curved sides marched from the waist up toward the cap.
    gfx_fill_rect_land(cx - TIMER_HALF_OUT, top, 2 * TIMER_HALF_OUT, TIMER_OUTLINE, color);
    {
        float prevHw = (float)s_timerBulbHw[TIMER_CHAMBER_H - 1]; // the waist
        float prevY = (float)(top + TIMER_CHAMBER_H - 1);
        for (int step = 1; step < curveRows; step++) {
            int tableRow = TIMER_CHAMBER_H - 1 - step; // walking the table
                                                         // backwards, waist
                                                         // toward cap
            float hw = (float)s_timerBulbHw[tableRow];
            float y = (float)(top + tableRow);
            shapes_fill_capsule_aa_land(cx - prevHw, prevY, (float)outlineR,
                                         cx - hw, y, (float)outlineR, color);
            shapes_fill_capsule_aa_land(cx + prevHw, prevY, (float)outlineR,
                                         cx + hw, y, (float)outlineR, color);
            prevHw = hw;
            prevY = y;
        }
    }

    /* -----------------------------------------------------------------
     * THE SAND. A separate, solid shape sitting INSIDE the top bulb's
     * glass - not the bulb's own fill, and not filled to the brim: it
     * occupies roughly the lower half to two thirds of the chamber
     * (measured from the waist up), per the reference. sandDepthRows is
     * that fraction of TIMER_CHAMBER_H; rowSandTop is the table row (in
     * the top chamber's own row-0-is-cap convention) where the sand
     * begins.
     *
     * The sand's own top surface is not flat: sand settles into a shallow
     * CONE as it drains through the neck, higher against the glass walls
     * than in the middle, so the surface DIPS toward the centre - a
     * crater, not a table top. In cross-section that means, for the rows
     * right at the top of the sand, there are TWO separate spans of sand
     * (a sliver against the left wall, a sliver against the right wall)
     * with a gap between them, rather than one span across the full
     * width; the gap narrows going down (dipRows of it) until the two
     * slivers meet and the rest of the sand, down to the waist, is solid
     * full width. shapes_fill_between_curves_aa_land only ever fills ONE
     * span per row, so the two-sliver zone is drawn as two separate calls
     * (left blob, right blob) rather than one - everything below the dip
     * is the ordinary one-call mirrored fill the whole bulb used to use.
     * ----------------------------------------------------------------- */
    int sandDepthRows = TIMER_CHAMBER_H * 3 / 5; // ~60%: "half to two thirds"
    int rowSandTop = TIMER_CHAMBER_H - sandDepthRows;
    int sandInset = outlineR; // sand sits just inside the glass stroke's
                               // own inner edge - touching it, not past it
    int dipRows = 5;          // shallow on purpose: a handful of rows out
                               // of sandDepthRows (~26), or this reads as
                               // a bowl, not a dusting of settling sand
    if (dipRows > sandDepthRows) dipRows = sandDepthRows;

    int16_t sLeft[TIMER_CHAMBER_H], sRight[TIMER_CHAMBER_H];

    if (dipRows > 1) {
        int sandHwAtTop = s_timerBulbHw[rowSandTop] - sandInset;
        if (sandHwAtTop < 0) sandHwAtTop = 0;
        // How much sand shows at the very peak of the dip, against each
        // wall: small enough to read as a sliver, wide enough (with its
        // own AA fringe) not to be the "petit pixel nul" this whole file
        // exists to avoid.
        int sliverPx = 2;
        int maxCutout = sandHwAtTop - sliverPx;
        if (maxCutout < 0) maxCutout = 0;

        // Left sliver, growing inward (its own RIGHT edge is the dip's
        // moving boundary) as the row index increases.
        for (int i = 0; i < dipRows; i++) {
            int hw = s_timerBulbHw[rowSandTop + i] - sandInset;
            if (hw < 0) hw = 0;
            int cutout = maxCutout * (dipRows - 1 - i) / (dipRows - 1); // maxCutout -> 0
            if (cutout > hw) cutout = hw;
            sLeft[i] = (int16_t)(cx - hw);
            sRight[i] = (int16_t)(cx - cutout);
        }
        shapes_fill_between_curves_aa_land(top + rowSandTop, dipRows, sLeft, sRight, color);

        // Right sliver, the mirror.
        for (int i = 0; i < dipRows; i++) {
            int hw = s_timerBulbHw[rowSandTop + i] - sandInset;
            if (hw < 0) hw = 0;
            int cutout = maxCutout * (dipRows - 1 - i) / (dipRows - 1);
            if (cutout > hw) cutout = hw;
            sLeft[i] = (int16_t)(cx + cutout);
            sRight[i] = (int16_t)(cx + hw);
        }
        shapes_fill_between_curves_aa_land(top + rowSandTop, dipRows, sLeft, sRight, color);
    } else {
        dipRows = 0; // degenerate case (a tiny chamber): no room for a
                     // dip at all, fall straight through to the solid fill
                     // below.
    }

    // Below the dip: solid, full width (inset from the glass), down to
    // the waist - what connects the sand to the neck and to the falling
    // grains, and what makes it read as sand SITTING in the glass rather
    // than floating.
    {
        int fullRows = TIMER_CHAMBER_H - (rowSandTop + dipRows);
        for (int i = 0; i < fullRows; i++) {
            int row = rowSandTop + dipRows + i;
            int hw = s_timerBulbHw[row] - sandInset;
            if (hw < 0) hw = 0;
            sLeft[i] = (int16_t)(cx - hw);
            sRight[i] = (int16_t)(cx + hw);
        }
        shapes_fill_between_curves_aa_land(top + rowSandTop + dipRows, fullRows, sLeft, sRight, color);
    }

    // Neck: a short solid connector, already axis-aligned. It closes the
    // bottom of the sand and the top of the empty chamber in the same
    // rectangle, which is what gives the glass a pinch point instead of
    // two chambers meeting at a single pixel.
    int neckY = top + TIMER_CHAMBER_H;
    gfx_fill_rect_land(cx - TIMER_HALF_NECK, neckY, 2 * TIMER_HALF_NECK, TIMER_NECK_H, color);

    // ---- BOTTOM bulb: the glass, mirrored - empty, nothing has fallen
    // yet. Its own row 0 (right below the neck) is the NARROW end, so it
    // reads s_timerBulbHw in reverse (index TIMER_CHAMBER_H-1-row), same
    // table the top bulb used, just walked the other way; same march,
    // same overflow reservation, same flat cap treatment as the top bulb
    // above (see this block's header comment for both).
    int bottomTop = neckY + TIMER_NECK_H;
    {
        float prevHw = (float)s_timerBulbHw[TIMER_CHAMBER_H - 1]; // this chamber's own row 0 (waist)
        float prevY = (float)bottomTop;
        for (int row = 1; row < curveRows; row++) {
            float hw = (float)s_timerBulbHw[TIMER_CHAMBER_H - 1 - row];
            float y = (float)(bottomTop + row);
            shapes_fill_capsule_aa_land((float)cx - prevHw, prevY, (float)outlineR,
                                         (float)cx - hw, y, (float)outlineR, color);
            shapes_fill_capsule_aa_land((float)cx + prevHw, prevY, (float)outlineR,
                                         (float)cx + hw, y, (float)outlineR, color);
            prevHw = hw;
            prevY = y;
        }
    }
    gfx_fill_rect_land(cx - TIMER_HALF_OUT, bottomTop + TIMER_CHAMBER_H - TIMER_OUTLINE,
                        2 * TIMER_HALF_OUT, TIMER_OUTLINE, color);

    // REMOVED 2026-08-14: two individual grain discs used to float here,
    // a few pixels below the neck and touching nothing - exactly the
    // "loose grain of sand" the owner named directly this pass ("no
    // spare pixel... no stray pixel", the hourglass called out by name).
    // They read as connected only by proximity, not by any actual shared
    // ink with the sand mass above or the heap below - the one shape in
    // this icon shapes.h's MIN-composition argument does not save,
    // because a lone disc has nothing to composite AGAINST. The heap
    // just below (see next) already carries "grains have landed" without
    // a floating mark to do it.

    // A small heap where the fallen grains would be landing, tried rather
    // than assumed either way (see this file's task history): four rows,
    // narrowest at its own peak and widest at its base, sitting just above
    // the bottom cap. Kept because it still reads as a heap at 1x, not a
    // smear - see the task report for the framebuffer check.
    {
        static const int16_t heapHw[4] = {1, 2, 3, 4};
        int heapRows = 4;
        int heapTop = bottomTop + TIMER_CHAMBER_H - TIMER_OUTLINE - heapRows;
        int16_t hl[4], hr[4];
        for (int i = 0; i < heapRows; i++) {
            hl[i] = (int16_t)(cx - heapHw[i]);
            hr[i] = (int16_t)(cx + heapHw[i]);
        }
        shapes_fill_between_curves_aa_land(heapTop, heapRows, hl, hr, color);
    }
}

/* ---------------------------------------------------------------------
 * PROPOSED ALTERNATIVE timer icon, 2026-08-14: a coil, not an hourglass.
 * The owner has never liked the hourglass and this pass asked whether it
 * is even the right object - a fair question now that the timer app
 * itself no longer shows one anywhere: it is a coil dial that winds like
 * a hose (docs/decisions/0002-runtime-architecture.md, "the coil"). This
 * icon draws that same idea small - a spiral winding inward, the same
 * "winding inward so the outer diameter never grows" language the real
 * dial's own header comment uses - rather than inventing a second,
 * unrelated pictogram for "waiting".
 *
 * ONE continuous ink stroke, by construction rather than by discipline: a
 * single chain of shapes_fill_capsule_aa_land calls marched along a
 * spiral parametric curve (COIL_POINTS samples, COIL_TURNS turns,
 * radius linear from COIL_R_OUTER down to COIL_R_INNER), each call's
 * start point the previous call's end point exactly - the same
 * shared-endpoint argument shapes_fill_tapered_quad_aa_land's own header
 * comment makes for why a chain built this way reads as one curve, not a
 * set of facets. Width follows a sine hump (COIL_W_MIN at both ends,
 * COIL_W_MAX at the middle turn) - the same asymmetric-taper spirit
 * draw_icon_sketch's own header comment argues for, thin where the pen
 * would be leaving or arriving, thick mid-stroke. There is no second
 * shape for this icon to connect to anything: the "no stray pixel" rule
 * is satisfied trivially, not by discipline, which is part of the case
 * for it over the hourglass.
 *
 * COIL_TWO_PI/COIL_PI are spelled out rather than pulled from math.h's
 * M_PI, which is not guaranteed available under -std=c11 on every
 * toolchain this file builds under (the board's arm-none-eabi-gcc and
 * zig's wasm32-freestanding target, see docs/decisions/0003) - the same
 * reasoning CHRONO_TAB_OFF used to use to avoid sinf/cosf, in reverse:
 * here the angle genuinely varies per point, so the runtime call is the
 * right tool, but the constant it needs should not depend on a macro
 * some toolchains define behind a feature-test flag and some do not.
 * ------------------------------------------------------------------- */
#define COIL_PI       3.14159265359f
#define COIL_TWO_PI   6.28318530718f
#define COIL_TURNS    2.25f
#define COIL_R_OUTER  40
#define COIL_R_INNER  6
#define COIL_POINTS   72
#define COIL_W_MAX    5.0f  // matches CHRONO_CROWN_BASE_R: same hand, same weight
#define COIL_W_MIN    1.5f

static void draw_icon_timer_coil(int ox, int oy, uint16_t color) {
    int ccx = ox + ICON_W / 2;
    int ccy = oy + ICON_H / 2;

    float prevX = 0.0f, prevY = 0.0f, prevW = 0.0f;
    for (int i = 0; i < COIL_POINTS; i++) {
        float t = (float)i / (float)(COIL_POINTS - 1);
        float theta = t * COIL_TURNS * COIL_TWO_PI;
        float r = (float)COIL_R_OUTER + ((float)COIL_R_INNER - (float)COIL_R_OUTER) * t;
        float x = (float)ccx + r * sinf(theta);
        float y = (float)ccy - r * cosf(theta);
        float w = COIL_W_MIN + (COIL_W_MAX - COIL_W_MIN) * sinf(COIL_PI * t);

        if (i > 0) {
            shapes_fill_capsule_aa_land(prevX, prevY, prevW, x, y, w, color);
        }
        prevX = x; prevY = y; prevW = w;
    }
}

// Which timer icon actually paints. Flip to 1 once the owner has picked
// between draw_icon_timer() (the cleaned-up hourglass) and
// draw_icon_timer_coil() above, then delete whichever function this
// stops calling - both are kept compiled for now only so the emulator
// can render either one on request while the choice is open.
// 1 since 2026-08-14: the owner chose the coil over the hourglass. It is one
// continuous stroke, so "no stray pixel" holds by construction rather than by
// vigilance, and the timer has not shown an hourglass since it became a wound
// dial that morning. The hourglass stays compiled and one edit away.
#define TIMER_ICON_USE_COIL 1

/* =======================================================================
 * LUCIDE-DERIVED CANDIDATES, 2026-08-14. The owner's direction, which
 * replaces every icon rather than refining what is there: "essayez de
 * redessiner nos icones de zero en utilisant Lucide" - the icon language
 * behind the Cursor set he referenced, ISC licensed, vendored at
 * third_party/lucide/ (see that directory's own README.md for exactly
 * which files and why, and LICENSE for the licence text the ISC terms
 * require to travel with the source).
 *
 * CONVERSION, NOT TRACING. Lucide icons are exactly the vocabulary
 * shapes.h already speaks: <circle> is shapes_fill_annulus_aa_land, and
 * every <line>/<path> is a chain of round-capped, CONSTANT-width
 * segments - shapes_stroke_polyline_aa_land (new this pass, see shapes.h;
 * it is shapes_fill_tapered_quad_aa_land's own chaining argument without
 * the taper, since Lucide never tapers). tools/lucide-convert.ts parses
 * each vendored SVG's <line>/<circle>/<path> (M/L/H/V/A/Z, flattening
 * arcs to points) and tools/gen-lucide-menu-icons.ts prints the point
 * arrays below - generated, then hand-pasted, the same discipline every
 * other baked-constant table in this file already uses (draw_icon_sketch's
 * fx/fy/fr, the chrono icon's hand-wobble tables): a design decision made
 * once, not a build-time dependency on a third file.
 *
 * SCALE: Lucide's own viewBox is always 24x24 at a 2px stroke; this
 * device's icon box is 96x96, so 4x lands every coordinate exactly on
 * this file's own ICON_W/ICON_H, and the FAITHFUL conversion is therefore
 * 2px*4=8px (LUCIDE_STROKE_HALF=4.0f) - close to the chrono ring's old
 * 10px band, which is why it was expected to land naturally. This is
 * exactly the legibility question the coordinator raised before anything
 * was rendered: Lucide lives at 16px in an IDE with a thin line, this icon
 * lives at 96px read by a child who cannot read yet - a thin outline can
 * say "precision tool" where this device wants "object". LUCIDE_STROKE_HALF
 * is set to 5.0f (10px) below, heavier than the faithful conversion,
 * because the design-time approximate raster (tools/gen-lucide-menu-icons.ts,
 * NOT anti-aliased, NOT the real firmware) already looked thin at 4.0f
 * side by side with the ring/coil's own boldness - but that is a
 * quick-iteration tool's opinion, not the real panel's. See this pass's
 * own report for what the pixel-accurate wasm-emulator capture actually
 * showed at both weights, and whether 5.0f was the right call or needed
 * a further pass.
 *
 * NEITHER SET IS WIRED AS THE DEFAULT. draw_icon_for() below still
 * paints the current shipped icons; the *_ICON_LUCIDE defines are 0
 * (off) until the owner has looked at the preview and chosen - see this
 * pass's own report for the PNGs and which candidate is recommended
 * where more than one was rendered.
 * ----------------------------------------------------------------------
 * NAMING NOTE, worth having in mind reading the constant names below:
 * Lucide's own icon called "timer" is shaped like what THIS device calls
 * chrono (a stopwatch - a dial, a crown, a hand), and Lucide has no icon
 * for this device's timer app's own coil dial at all. The match below is
 * by SILHOUETTE, never by Lucide's own filename - s_lucideChronoTimer*
 * draws from timer.svg because chrono is the stopwatch, not because the
 * names happen to agree.
 * ======================================================================= */
#define LUCIDE_STROKE_HALF 5.0f

// ---- chrono candidate: timer.svg (Lucide), sole candidate. alarm-clock.svg
// and watch.svg were also fetched and considered - rejected because both
// carry a specific OTHER object's silhouette (an alarm clock's bell-ear
// legs, a wristwatch's strap lugs) that says something this device's
// stopwatch does not do (ring an alarm, strap to a wrist); clock.svg was
// rejected for the opposite reason, too generic (a bare clock face reads
// as "tells the time", not "counts elapsed time"). timer.svg's own dial +
// crown + hand is the closest thing Lucide has to a stopwatch pictogram.
static const float s_lucideChronoRingCx = 48.00f, s_lucideChronoRingCy = 56.00f, s_lucideChronoRingR = 32.00f;
static const float s_lucideChronoCrownX[2] = { 40.00f, 56.00f };
static const float s_lucideChronoCrownY[2] = { 8.00f, 8.00f };
static const float s_lucideChronoHandX[2] = { 48.00f, 60.00f };
static const float s_lucideChronoHandY[2] = { 56.00f, 44.00f };

static void draw_icon_lucide_chrono(int ox, int oy, uint16_t color) {
    float dx = (float)ox, dy = (float)oy;
    shapes_fill_annulus_aa_land(dx + s_lucideChronoRingCx, dy + s_lucideChronoRingCy,
                                 s_lucideChronoRingR + LUCIDE_STROKE_HALF, s_lucideChronoRingR - LUCIDE_STROKE_HALF, color);
    float cx[2] = { dx + s_lucideChronoCrownX[0], dx + s_lucideChronoCrownX[1] };
    float cy[2] = { dy + s_lucideChronoCrownY[0], dy + s_lucideChronoCrownY[1] };
    shapes_stroke_polyline_aa_land(cx, cy, 2, LUCIDE_STROKE_HALF, color);
    float hx[2] = { dx + s_lucideChronoHandX[0], dx + s_lucideChronoHandX[1] };
    float hy[2] = { dy + s_lucideChronoHandY[0], dy + s_lucideChronoHandY[1] };
    shapes_stroke_polyline_aa_land(hx, hy, 2, LUCIDE_STROKE_HALF, color);

    // TWO STEMS, ours and not Lucide's. Lucide's timer glyph leaves the top
    // bar floating clear of the circle, and the conversion was faithful to
    // that. The owner drew the two missing lines straight onto the render:
    // "assure toi que le haut du chronometre est bien relie au reste de
    // l'appareil comme je l'ai decide".
    //
    // He is holding an addition to his own standing rule, stated on the very
    // first icon round and never withdrawn since: one connected piece of ink,
    // nothing floating. A detached bar is the same defect as the hourglass's
    // loose grain of sand and the old chrono's floating tab, and it does not
    // stop being one because an upstream icon set drew it that way.
    //
    // Deliberately thinner than LUCIDE_STROKE_HALF: these are the crown's
    // stem, not another bar. They run from inside the bar's own ink down past
    // the ring's outer edge, so both joins overlap rather than abut, which is
    // what MIN composition needs to merge them into one mark.
    // ONE neck, not two stems. The first version used two, which left a
    // two-pixel window of paper between them: at 96px that reads as a hole
    // punched in the crown rather than as a neck, and the pair also piled ink
    // up where both met the ring, making that junction visibly heavier than
    // any other stroke in the set. The owner saw it as the icon spilling out
    // of itself ("ca deborde un tout petit peu sur le chronometre").
    //
    // A single centred neck has neither problem: no gap to misread, and one
    // junction instead of two. It also matches what a stopwatch actually
    // looks like, a wide button on a narrow stem.
    //
    // It runs from inside the bar's own ink down past the ring's outer edge,
    // so both joins overlap rather than abut, which is what MIN composition
    // needs to merge them into one mark.
    {
        // A round cap extends a full radius past the point it is given: at
        // y=27 with a 4px radius the ink reached y=31, while the ring's inner
        // edge is y=29, so two pixels hung inside the dial. Ending at 24 puts
        // the cap's far edge at 28, inside the band rather than through it.
        const float neckHalf = 4.0f;
        float sx[2] = { dx + 48.0f, dx + 48.0f };
        float sy[2] = { dy + 10.0f, dy + 24.0f };
        shapes_stroke_polyline_aa_land(sx, sy, 2, neckHalf, color);
    }
}

// ---- sketch candidates: pencil.svg and pencil-line.svg. pen-tool.svg
// (a vector-editor bezier-pen glyph) and brush.svg/paintbrush.svg (both
// say "colour", which this monochrome ink display never has) were
// rejected as the wrong tool's icon, not a style variant of the right
// one. Two are rendered because they are genuinely close: pencil-line
// adds a short mark under the pencil's own tip, which argues for itself
// on THIS app specifically (it draws, so a mark it already made is a
// fair thing to show) but costs a little of the plain pencil's own
// quiet minimalism ("invisible" per the owner's own goal for this set).
static const float s_lucidePencilBodyX[69] = { 84.70f, 86.10f, 87.14f, 87.78f, 88.00f, 87.78f, 87.14f, 86.10f, 84.70f, 82.99f, 81.04f, 78.93f, 76.73f, 74.53f, 72.41f, 70.46f, 68.75f, 15.37f, 15.20f, 15.03f, 14.87f, 14.72f, 14.57f, 14.43f, 14.29f, 14.16f, 14.04f, 13.92f, 13.81f, 13.71f, 13.62f, 13.53f, 13.44f, 13.37f, 8.08f, 8.02f, 8.00f, 8.01f, 8.06f, 8.14f, 8.26f, 8.41f, 8.59f, 8.79f, 9.01f, 9.26f, 9.51f, 9.78f, 10.05f, 10.31f, 10.58f, 27.99f, 28.22f, 28.45f, 28.68f, 28.90f, 29.12f, 29.34f, 29.56f, 29.77f, 29.98f, 30.18f, 30.38f, 30.58f, 30.77f, 30.95f, 31.13f, 31.31f, 84.70f };
static const float s_lucidePencilBodyY[69] = { 27.25f, 25.54f, 23.59f, 21.48f, 19.28f, 17.08f, 14.96f, 13.01f, 11.30f, 9.90f, 8.86f, 8.22f, 8.00f, 8.21f, 8.86f, 9.90f, 11.30f, 64.70f, 64.87f, 65.05f, 65.24f, 65.43f, 65.62f, 65.82f, 66.03f, 66.23f, 66.44f, 66.66f, 66.88f, 67.10f, 67.33f, 67.55f, 67.78f, 68.02f, 85.42f, 85.69f, 85.95f, 86.22f, 86.49f, 86.74f, 86.99f, 87.21f, 87.41f, 87.59f, 87.74f, 87.85f, 87.94f, 87.98f, 88.00f, 87.97f, 87.91f, 82.63f, 82.56f, 82.48f, 82.39f, 82.29f, 82.19f, 82.08f, 81.96f, 81.84f, 81.71f, 81.58f, 81.44f, 81.29f, 81.14f, 80.98f, 80.81f, 80.64f, 27.25f };
static const float s_lucidePencilTickX[2] = { 60.00f, 76.00f };
static const float s_lucidePencilTickY[2] = { 20.00f, 36.00f };

static void draw_icon_lucide_sketch_pencil(int ox, int oy, uint16_t color) {
    float dx = (float)ox, dy = (float)oy;
    float bx[69], by[69];
    for (int i = 0; i < 69; i++) { bx[i] = dx + s_lucidePencilBodyX[i]; by[i] = dy + s_lucidePencilBodyY[i]; }
    shapes_stroke_polyline_aa_land(bx, by, 69, LUCIDE_STROKE_HALF, color);
    float tx[2] = { dx + s_lucidePencilTickX[0], dx + s_lucidePencilTickX[1] };
    float ty[2] = { dy + s_lucidePencilTickY[0], dy + s_lucidePencilTickY[1] };
    shapes_stroke_polyline_aa_land(tx, ty, 2, LUCIDE_STROKE_HALF, color);
}

static const float s_lucidePencilLineMarkX[2] = { 52.00f, 84.00f };
static const float s_lucidePencilLineMarkY[2] = { 84.00f, 84.00f };

static void draw_icon_lucide_sketch_pencilline(int ox, int oy, uint16_t color) {
    draw_icon_lucide_sketch_pencil(ox, oy, color);
    float dx = (float)ox, dy = (float)oy;
    float mx[2] = { dx + s_lucidePencilLineMarkX[0], dx + s_lucidePencilLineMarkX[1] };
    float my[2] = { dy + s_lucidePencilLineMarkY[0], dy + s_lucidePencilLineMarkY[1] };
    shapes_stroke_polyline_aa_land(mx, my, 2, LUCIDE_STROKE_HALF, color);
}

// ---- timer candidates: hourglass.svg and loader-circle.svg. NAMING THE
// TRADE the owner asked not to drop silently: Lucide has nothing shaped
// like a wound coil - the real dial is this device's own invention, not
// a stock pictogram, so nothing in Lucide's set can be faithful to it.
// hourglass.svg is the classic, universally-read "waiting" symbol (a
// child does not need to have used a coil timer to read it, unlike
// loader-circle.svg); loader-circle.svg is Lucide's own spinner glyph -
// an open ring, one continuous stroke - which is a WEAKER symbol on its
// own (spinners mean "loading", not "counting down") but is honestly the
// closer relative of the two to what the real dial actually IS (a
// partial ring that fills as it winds). Both rendered; recommend
// hourglass for legibility a child needs no context to read, but say so
// rather than pick quietly - the coil itself is not represented either
// way, and that loss is real, not papered over by either choice.
static const float s_lucideHourglassCapTopX[2] = { 20.00f, 76.00f };
static const float s_lucideHourglassCapTopY[2] = { 8.00f, 8.00f };
static const float s_lucideHourglassCapBotX[2] = { 20.00f, 76.00f };
static const float s_lucideHourglassCapBotY[2] = { 88.00f, 88.00f };
// Lucide's own hourglass is NOT a rounded two-bulb shape like this device's
// hand-drawn one - it is two flat "V"/"^" wedges, each entirely on its own
// side of the waist: the BOTTOM wedge runs corner-to-waist-to-corner along
// the bottom cap only (y in [48,88], never reaching the top), the TOP
// wedge the mirror. First transcription of these two arrays swapped which
// array's SECOND half belonged to which, splicing the bottom wedge's own
// first half onto the top wedge's second half - caught only by rendering
// it (a lightning-bolt "S" instead of a bowtie X) and re-verified against
// tools/gen-lucide-menu-icons.ts's own fresh output, not by re-reading the
// numbers harder. Worth the reminder: bounds-checking a baked table means
// checking it stays ON ITS OWN SIDE of a shared point like the waist, not
// just checking its first few entries look plausible.
static const float s_lucideHourglassBottomWedgeX[37] = { 68.00f, 68.00f, 67.99f, 67.96f, 67.91f, 67.85f, 67.76f, 67.66f, 67.53f, 67.39f, 67.23f, 67.05f, 66.86f, 66.65f, 66.42f, 66.18f, 65.93f, 65.66f, 48.00f, 30.34f, 30.07f, 29.82f, 29.58f, 29.35f, 29.14f, 28.95f, 28.77f, 28.61f, 28.47f, 28.34f, 28.24f, 28.15f, 28.09f, 28.04f, 28.01f, 28.00f, 28.00f };
static const float s_lucideHourglassBottomWedgeY[37] = { 88.00f, 71.31f, 70.92f, 70.53f, 70.14f, 69.75f, 69.37f, 68.99f, 68.62f, 68.25f, 67.89f, 67.54f, 67.20f, 66.87f, 66.55f, 66.24f, 65.94f, 65.66f, 48.00f, 65.66f, 65.94f, 66.24f, 66.55f, 66.87f, 67.20f, 67.54f, 67.89f, 68.25f, 68.62f, 68.99f, 69.37f, 69.75f, 70.14f, 70.53f, 70.92f, 71.31f, 88.00f };
static const float s_lucideHourglassTopWedgeX[37] = { 28.00f, 28.00f, 28.01f, 28.04f, 28.09f, 28.15f, 28.24f, 28.34f, 28.47f, 28.61f, 28.77f, 28.95f, 29.14f, 29.35f, 29.58f, 29.82f, 30.07f, 30.34f, 48.00f, 65.66f, 65.93f, 66.18f, 66.42f, 66.65f, 66.86f, 67.05f, 67.23f, 67.39f, 67.53f, 67.66f, 67.76f, 67.85f, 67.91f, 67.96f, 67.99f, 68.00f, 68.00f };
static const float s_lucideHourglassTopWedgeY[37] = { 8.00f, 24.69f, 25.08f, 25.47f, 25.86f, 26.25f, 26.63f, 27.01f, 27.38f, 27.75f, 28.11f, 28.46f, 28.80f, 29.13f, 29.45f, 29.76f, 30.06f, 30.34f, 48.00f, 30.34f, 30.06f, 29.76f, 29.45f, 29.13f, 28.80f, 28.46f, 28.11f, 27.75f, 27.38f, 27.01f, 26.63f, 26.25f, 25.86f, 25.47f, 25.08f, 24.69f, 8.00f };

static void draw_icon_lucide_timer_hourglass(int ox, int oy, uint16_t color) {
    float dx = (float)ox, dy = (float)oy;
    float cx[2] = { dx + s_lucideHourglassCapTopX[0], dx + s_lucideHourglassCapTopX[1] };
    float cy[2] = { dy + s_lucideHourglassCapTopY[0], dy + s_lucideHourglassCapTopY[1] };
    shapes_stroke_polyline_aa_land(cx, cy, 2, LUCIDE_STROKE_HALF, color);
    cx[0] = dx + s_lucideHourglassCapBotX[0]; cx[1] = dx + s_lucideHourglassCapBotX[1];
    cy[0] = dy + s_lucideHourglassCapBotY[0]; cy[1] = dy + s_lucideHourglassCapBotY[1];
    shapes_stroke_polyline_aa_land(cx, cy, 2, LUCIDE_STROKE_HALF, color);
    float bx[37], by[37], tx[37], ty[37];
    for (int i = 0; i < 37; i++) {
        bx[i] = dx + s_lucideHourglassBottomWedgeX[i]; by[i] = dy + s_lucideHourglassBottomWedgeY[i];
        tx[i] = dx + s_lucideHourglassTopWedgeX[i];    ty[i] = dy + s_lucideHourglassTopWedgeY[i];
    }
    shapes_stroke_polyline_aa_land(bx, by, 37, LUCIDE_STROKE_HALF, color);
    shapes_stroke_polyline_aa_land(tx, ty, 37, LUCIDE_STROKE_HALF, color);
}

static const float s_lucideLoaderX[17] = { 84.00f, 82.24f, 77.12f, 69.16f, 59.12f, 48.00f, 36.87f, 26.84f, 18.88f, 13.76f, 12.00f, 13.76f, 18.88f, 26.84f, 36.88f, 48.00f, 59.12f };
static const float s_lucideLoaderY[17] = { 48.00f, 59.12f, 69.16f, 77.12f, 82.24f, 84.00f, 82.24f, 77.12f, 69.16f, 59.12f, 48.00f, 36.87f, 26.84f, 18.87f, 13.76f, 12.00f, 13.76f };

static void draw_icon_lucide_timer_loadercircle(int ox, int oy, uint16_t color) {
    float dx = (float)ox, dy = (float)oy;
    float xs[17], ys[17];
    for (int i = 0; i < 17; i++) { xs[i] = dx + s_lucideLoaderX[i]; ys[i] = dy + s_lucideLoaderY[i]; }
    shapes_stroke_polyline_aa_land(xs, ys, 17, LUCIDE_STROKE_HALF, color);
}

// 0 = keep painting the current shipped icon for that app; 1 (2 for
// sketch/timer's second candidate) previews the named Lucide candidate
// instead. All 0 until the owner has looked - see this pass's own report.
#define CHRONO_ICON_LUCIDE 1
#define SKETCH_ICON_LUCIDE 1 // 0=off, 1=pencil, 2=pencil-line
#define TIMER_ICON_LUCIDE  1 // 0=off, 1=hourglass, 2=loader-circle

static void draw_icon_for(const app_t *app, int ox, int oy, uint16_t color) {
    if (app == &g_chronoApp) {
#if CHRONO_ICON_LUCIDE
        draw_icon_lucide_chrono(ox, oy, color);
#else
        draw_icon_chrono(ox, oy, color);
#endif
    } else if (app == &g_sketchApp) {
#if SKETCH_ICON_LUCIDE == 1
        draw_icon_lucide_sketch_pencil(ox, oy, color);
#elif SKETCH_ICON_LUCIDE == 2
        draw_icon_lucide_sketch_pencilline(ox, oy, color);
#else
        draw_icon_sketch(ox, oy, color);
#endif
    } else if (app == &g_timerApp) {
#if TIMER_ICON_LUCIDE == 1
        draw_icon_lucide_timer_hourglass(ox, oy, color);
#elif TIMER_ICON_LUCIDE == 2
        draw_icon_lucide_timer_loadercircle(ox, oy, color);
#elif TIMER_ICON_USE_COIL
        draw_icon_timer_coil(ox, oy, color);
#else
        draw_icon_timer(ox, oy, color);
#endif
    }
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
