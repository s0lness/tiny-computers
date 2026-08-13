/*
 * menu: how a child picks an app.
 *
 * Landscape, full-screen, one tile per entry in runtime.c's g_apps[]. Each
 * tile is a blocky icon (rectangles only, via gfx_fill_rect_land - nothing
 * here ever plots a single pixel or a curve) with the app's name underneath
 * in a small hand-authored font. Pictures first, words second: reading
 * English is not the entry fee for a child who cannot yet.
 *
 * Tapping a tile is the primary input and it launches immediately: this is
 * what resolves what "confirm" means, which a button-only menu never
 * answers on its own (see docs/decisions/0002 section 4). BOOT / PWR-short
 * are the backup path for a device held sideways with both thumbs on the
 * buttons and no finger free for the glass: BOOT moves the selection right,
 * wrapping, PWR-short launches whatever is selected. The selected tile
 * always has a visibly thicker border than the others, so which of BOOT's
 * two meanings ("move" vs "launch, once PWR is pressed") is live is never a
 * guess - see the header comment on "no modal state" in app.h and 0002.
 *
 * Opening and closing the menu (the BOOT+PWR long-press chord) is entirely
 * the runtime's business; this file never reads the PMIC and never calls
 * app_switch_to() with its own index. It only ever switches TO a real
 * g_apps[] entry, on a tap or on PWR-short.
 */
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
 * A minimal 5x7 block font: a private copy of firmware/menu.c's, itself
 * a private copy per that file's own header comment (see there for why
 * push_dirty-style code lives independently everywhere it is used rather
 * than being shared - the same reasoning applies to a font nothing else
 * in this codebase currently needs). Uppercase A-Z, digits 0-9, space and
 * hyphen; anything else, including lowercase before the uppercasing below
 * runs, draws as a blank cell rather than faulting. App names are each
 * app's own free-form string (this file does not own or validate them), so
 * degrading gracefully here matters more than it did for the old menu's
 * fixed, known-in-advance APP_NAMES table.
 * ------------------------------------------------------------------- */
#define FONT_W 5
#define FONT_H 7

static const uint8_t FONT5X7[38][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' ' (index 0)
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, // '0' (1)
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // '1'
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, // '2'
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, // '3'
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, // '4'
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, // '5'
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // '6'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // '7'
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // '8'
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, // '9' (10)
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // 'A' (11)
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, // 'B'
    { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F }, // 'C'
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, // 'D'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, // 'E'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // 'F'
    { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0E }, // 'G'
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // 'H'
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, // 'I'
    { 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E }, // 'J'
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, // 'K'
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, // 'L'
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, // 'M'
    { 0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11 }, // 'N'
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // 'O'
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, // 'P'
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, // 'Q'
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, // 'R'
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, // 'S'
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, // 'T'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // 'U'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, // 'V'
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, // 'W'
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, // 'X'
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, // 'Y'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, // 'Z' (36)
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, // '-' (37)
};

static int font_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); // app names are not
                                                          // guaranteed upper
                                                          // case; this font
                                                          // only draws one.
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c == '-') return 37;
    return 0; // space, and anything unsupported: a blank cell, not a fault.
}

#define FONT_SCALE 3
#define CHAR_W     (FONT_W * FONT_SCALE) // 15
#define CHAR_H     (FONT_H * FONT_SCALE) // 21
#define CHAR_GAP   (1 * FONT_SCALE)      // 3

static int menu_text_width(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    if (n == 0) return 0;
    return n * CHAR_W + (n - 1) * CHAR_GAP;
}

static void menu_draw_text(int lx, int ly, const char *s, uint16_t colorPx) {
    int x = lx;
    for (const char *p = s; *p; p++) {
        const uint8_t *rows = FONT5X7[font_index(*p)];
        for (int r = 0; r < FONT_H; r++) {
            uint8_t bits = rows[r];
            for (int c = 0; c < FONT_W; c++) {
                if (bits & (1u << (FONT_W - 1 - c))) {
                    gfx_fill_rect_land(x + c * FONT_SCALE, ly + r * FONT_SCALE,
                                        FONT_SCALE, FONT_SCALE, colorPx);
                }
            }
        }
        x += CHAR_W + CHAR_GAP;
    }
}

/* ---------------------------------------------------------------------
 * Tile layout, landscape coordinates (LAND_W x LAND_H = 448 x 368). One row
 * of g_appCount tiles, evenly spread with a fixed margin and gap. Computed
 * at runtime rather than with a compile-time _Static_assert the way the old
 * menu.c checked APPSWITCH_SLOT_COUNT: g_appCount is an extern int here, not
 * a macro, since it is sized from the linked app table (see app.h), so
 * there is nothing for the preprocessor to check against.
 * ------------------------------------------------------------------- */
#define TILE_MARGIN       16
#define TILE_GAP          16
#define TILE_H            220
#define TILE_PAD          18
#define TILE_BORDER_THICK 4
#define TILE_BORDER_THIN  2

#define ICON_W 96
#define ICON_H 96

static void tile_rect_land(int i, int *bx, int *by, int *bw, int *bh) {
    int n = g_appCount;
    int usableW = LAND_W - 2 * TILE_MARGIN - (n - 1) * TILE_GAP;
    int tileW = usableW / n;
    *bw = tileW;
    *bh = TILE_H;
    *bx = TILE_MARGIN + i * (tileW + TILE_GAP);
    *by = (LAND_H - TILE_H) / 2;
}

// The touch coordinates a tick() sees arrive in PANEL (portrait) space, not
// landscape - see app.h's comment on touchX/touchY and gfx.h's note that it
// rotates rectangles, not pixels, so a landscape app that hit-tests a touch
// has to invert the mapping itself. gfx.h only documents the forward
// direction (landscape -> panel: px = PANEL_W-1-ly, py = lx, see gfx_land_
// rect's header comment) because chrono and timer, the other two landscape
// apps, never need the inverse: they draw digits, not touch targets. Solved
// here by algebra on that same documented mapping, not by a new gfx.h
// helper (nothing else needs it, same reasoning as this file's private font
// above).
static void panel_to_land(int px, int py, int *lx, int *ly) {
    *lx = py;
    *ly = PANEL_W - 1 - px;
}

static int tile_hit_test(int lx, int ly) {
    for (int i = 0; i < g_appCount; i++) {
        int bx, by, bw, bh;
        tile_rect_land(i, &bx, &by, &bw, &bh);
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) return i;
    }
    return -1;
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
 * bottom drawn empty - per the owner's sketch (icon-sablier.png), which
 * hatches the top chamber and leaves the bottom a bare outline. The
 * asymmetry is the whole point (time still to come), which the old
 * icon here - two identical stacks of bars - could not say; it read as
 * neutral stripes, not an hourglass.
 *
 * Each chamber is a straight taper from TIMER_HALF_OUT down to
 * TIMER_HALF_NECK, one row at a time - the same bars-not-curves technique
 * as the chrono icon's ring above, just a linear lerp instead of a circle,
 * so it does not need shapes.h's sqrtf table (a straight taper has a
 * closed form per row).
 *
 * The sand is solid black, not hatched: hatching was tried (a handful of
 * short diagonal shapes_fill_thick_segment_land() strokes across the top
 * chamber) and looked wrong at this size - at 96px the gaps between
 * strokes closed up under the panel's own anti-aliasing and it read as a
 * grey smear, not lines, which is worse than committing to solid. A few
 * grains just past the neck stayed legible at this size and are kept.
 * ------------------------------------------------------------------- */
#define TIMER_MARGIN    (ICON_H / 8)                             // 12
#define TIMER_HALF_OUT  (ICON_W / 2 - ICON_W / 12)                // 40
#define TIMER_HALF_NECK (ICON_W / 16)                              // 6
#define TIMER_NECK_H    (ICON_H / 24)                              // 4
#define TIMER_CHAMBER_H ((ICON_H - 2 * TIMER_MARGIN - TIMER_NECK_H) / 2) // 34
#define TIMER_OUTLINE   (ICON_W / 24)                              // 4

static void draw_icon_timer(int ox, int oy, uint16_t color) {
    int cx = ox + ICON_W / 2;
    int top = oy + TIMER_MARGIN;

    // Top chamber: solid fill, the sand. Row 0 comes out at TIMER_HALF_OUT
    // (a flat top edge, no separate cap rectangle needed) and tapers down
    // to TIMER_HALF_NECK at the waist.
    for (int row = 0; row < TIMER_CHAMBER_H; row++) {
        int hw = TIMER_HALF_OUT -
                 (TIMER_HALF_OUT - TIMER_HALF_NECK) * row / (TIMER_CHAMBER_H - 1);
        gfx_fill_rect_land(cx - hw, top + row, 2 * hw, 1, color);
    }

    // Neck: a short solid connector. It closes the bottom of the sand and
    // the top of the empty chamber in the same rectangle, which is what
    // gives the glass a pinch point instead of two triangles meeting at a
    // single pixel.
    int neckY = top + TIMER_CHAMBER_H;
    gfx_fill_rect_land(cx - TIMER_HALF_NECK, neckY, 2 * TIMER_HALF_NECK, TIMER_NECK_H, color);

    // Bottom chamber: outline only - nothing has fallen yet. Two edge bars
    // per row (left side, right side), the same idea as
    // shapes_draw_annulus_row() above but a linear taper instead of a
    // circle, plus one bottom cap bar to close the wide end (the narrow
    // end is already closed by the neck).
    int bottomTop = neckY + TIMER_NECK_H;
    for (int row = 0; row < TIMER_CHAMBER_H; row++) {
        int hw = TIMER_HALF_NECK +
                 (TIMER_HALF_OUT - TIMER_HALF_NECK) * row / (TIMER_CHAMBER_H - 1);
        int y = bottomTop + row;
        int outT = TIMER_OUTLINE;
        if (outT > hw) outT = hw; // near the neck the shape is narrower
                                   // than the outline stroke; clamp so the
                                   // two edges meet instead of overshooting
                                   // past each other.
        gfx_fill_rect_land(cx - hw, y, outT, 1, color);
        gfx_fill_rect_land(cx + hw - outT, y, outT, 1, color);
    }
    gfx_fill_rect_land(cx - TIMER_HALF_OUT, bottomTop + TIMER_CHAMBER_H - TIMER_OUTLINE,
                        2 * TIMER_HALF_OUT, TIMER_OUTLINE, color);

    // A couple of grains just past the neck, already fallen into the empty
    // chamber - small enough to read as grains rather than a blob at this
    // size (see the header comment above on what did NOT survive the same
    // test: hatching the sand).
    gfx_fill_rect_land(cx - 2, bottomTop + 6, 3, 3, color);
    gfx_fill_rect_land(cx - 1, bottomTop + 15, 2, 2, color);
}

static void draw_icon_for(const app_t *app, int ox, int oy, uint16_t color) {
    if (app == &g_chronoApp) draw_icon_chrono(ox, oy, color);
    else if (app == &g_sketchApp) draw_icon_sketch(ox, oy, color);
    else if (app == &g_timerApp) draw_icon_timer(ox, oy, color);
    // An app added to g_apps[] without a matching icon here draws no icon,
    // just its name below - a silent gap, not a fault. The menu is a
    // navigation aid; it must never be the thing that stops a build.
}

/* ---------------------------------------------------------------------
 * Painting. paint_tile() only ever fills pixels; it never pushes, so
 * menu_enter_paint_all() (the initial full draw) does not push once per
 * tile for nothing - the runtime already pushes the whole panel once after
 * enter() returns (see app.h). repaint_tile_and_push() is what interactive
 * updates (a moved selection) use instead, so "repaint only what changed"
 * means exactly what it says: two tiles' worth of pixels and two small
 * pushes, not a full-panel one.
 * ------------------------------------------------------------------- */
static void paint_tile(int i, bool selected) {
    int bx, by, bw, bh;
    tile_rect_land(i, &bx, &by, &bw, &bh);

    // Clear the tile's own area first. Not a cosmetic step: switching a
    // tile from selected to not (or back) changes the border's thickness,
    // and redrawing a thinner border over a thicker one leaves the old
    // outer edge behind unless the tile is wiped first.
    gfx_fill_rect_land(bx, by, bw, bh, PX_WHITE);

    int borderT = selected ? TILE_BORDER_THICK : TILE_BORDER_THIN;
    gfx_fill_rect_land(bx, by, bw, borderT, PX_BLACK);
    gfx_fill_rect_land(bx, by + bh - borderT, bw, borderT, PX_BLACK);
    gfx_fill_rect_land(bx, by, borderT, bh, PX_BLACK);
    gfx_fill_rect_land(bx + bw - borderT, by, borderT, bh, PX_BLACK);

    const app_t *app = g_apps[i];

    int iconX = bx + (bw - ICON_W) / 2;
    int iconY = by + TILE_PAD;
    draw_icon_for(app, iconX, iconY, PX_BLACK);

    int tw = menu_text_width(app->name);
    int tx = bx + (bw - tw) / 2;
    int ty = by + bh - TILE_PAD - CHAR_H;
    menu_draw_text(tx, ty, app->name, PX_BLACK);
}

static void repaint_tile_and_push(int i, bool selected) {
    paint_tile(i, selected);
    int bx, by, bw, bh;
    tile_rect_land(i, &bx, &by, &bw, &bh);
    gfx_push_land(bx, by, bw, bh);
}

static void menu_paint_all(int cursor) {
    for (int i = 0; i < g_appCount; i++) paint_tile(i, i == cursor);
}

/* ---------------------------------------------------------------------
 * State and the app_t callbacks.
 * ------------------------------------------------------------------- */
typedef struct {
    int cursor;
} menu_state_t;

static menu_state_t *s_menu;

// Set by the runtime (see menu.h) before it switches into the menu; NOT
// arena-allocated, since it has to survive the arena reset that happens
// between that call and menu_enter() running.
static int s_returnAppIndex = 0;

void menu_set_return_app(int index) {
    s_returnAppIndex = index;
}

static void menu_enter(void) {
    s_menu = APP_STATE(menu_state_t);

    int cursor = s_returnAppIndex;
    if (g_appCount <= 0) {
        // Should be unreachable: the app table is compiled in, not loaded.
        // Guarded anyway rather than dividing by g_appCount elsewhere below.
        s_menu->cursor = 0;
        return;
    }
    if (cursor < 0 || cursor >= g_appCount) cursor = 0;
    s_menu->cursor = cursor;

    menu_paint_all(s_menu->cursor);
}

static void menu_tick(const app_frame_t *f) {
    if (f->touchPressed) {
        int lx, ly;
        panel_to_land(f->touchX, f->touchY, &lx, &ly);
        int hit = tile_hit_test(lx, ly);
        if (hit >= 0) {
            // Tap launches immediately: see this file's header comment on
            // why tapping is the primary input and resolves "confirm" on
            // its own. The arena is about to be reset by the switch, so
            // there is nothing worth repainting here first.
            s_menu->cursor = hit;
            app_switch_to(hit);
            return;
        }
    }

    if (f->bootClicked) {
        int prev = s_menu->cursor;
        int next = (prev + 1) % g_appCount;
        s_menu->cursor = next;
        repaint_tile_and_push(prev, false);
        repaint_tile_and_push(next, true);
    }

    if (f->key & KEY_SHORT) {
        app_switch_to(s_menu->cursor);
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
