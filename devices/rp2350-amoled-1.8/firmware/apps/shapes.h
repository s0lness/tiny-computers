#ifndef SHAPES_H
#define SHAPES_H

#include <stdint.h>

/*
 * shapes: round silhouettes, landscape space.
 *
 * TWO GENERATIONS IN ONE FILE, ON PURPOSE - not a half-finished migration.
 *
 * shapes_fill_half_width_table() and shapes_draw_annulus_row() are the
 * original, non-anti-aliased technique: gfx rotates rectangles, not pixels
 * (gfx.h), so a landscape app that wants a circle draws it as a stack of
 * horizontal 1px-tall bars, one gfx_fill_rect_land() call per bar, and a
 * table of half-widths (one sqrtf per row, computed once) is what a ring or
 * a disc needs to do that. These two stay exactly as they were and are NOT
 * going away: firmware/apps/timer.c uses both directly for its own
 * progress ring (a flat grey TRACK colour behind a black arc - see its
 * "shapes.h's table builder" comment), and timer.c is out of this change's
 * scope (another change is editing it at the same time this file was
 * written). Removing or reshaping either function would break a file nobody
 * reviewing this change can see move.
 *
 * Everything below shapes_fill_tapered_quad_aa_land() is new: menu.c's
 * three icons used to be built the same rectangle-bar way (plus a march of
 * stamped squares for a diagonal - see this file's git history for
 * shapes_fill_thick_segment_land/shapes_fill_tapered_segment_land/
 * shapes_fill_tapered_quad_land, now deleted), and every hard edge that
 * technique cannot represent - a ring's curve, a wedge's arc, a diamond's
 * 45-degree side, a squiggle's diagonal - came out as a staircase of single
 * pixels. That is what the owner is naming as a standing rule for this
 * device, not a one-icon bug: "je veux pas de petits pixels nuls nulle
 * part, mais que des aplats de couleur, comme si tout etait fait avec de
 * l'encre" (no nasty little pixels anywhere, only flat areas of colour, as
 * if everything were made with ink). sketch.c already solved exactly this
 * for the live pen (draw_capsule: a signed distance to the shape, turned
 * into an 8-bit grey coverage value, composited by MIN - darkest wins, see
 * its own header comment for why not alpha blending); this is that same
 * technique, factored into shapes menu.c's icons actually need: a filled
 * disc, an annulus, a filled shape bounded by two curves (a bulge, or a
 * wedge, or a diamond - see shapes_fill_between_curves_aa_land's own
 * comment for why one primitive covers all three), and a tapered stroke
 * along a path (a single capsule, or several chained through a quadratic
 * curve, exactly sketch.c's draw_capsule/draw_quad_midpoint pair).
 *
 * DECISION: replace menu.c's use of the old technique, keep the old
 * technique itself. The three functions this deleted
 * (shapes_fill_thick_segment_land, shapes_fill_tapered_segment_land,
 * shapes_fill_tapered_quad_land) had exactly one caller each, all in
 * menu.c, confirmed by grep across the whole tree before deleting them;
 * once menu.c stopped calling them they were dead code, not a second code
 * path worth carrying. shapes_fill_half_width_table/shapes_draw_annulus_row
 * are the opposite case - timer.c depends on them right now - so those stay
 * untouched rather than "replaced", alongside the new AA primitives below.
 *
 * gfx.h's landscape helpers (gfx_land_rect, gfx_fill_rect_land) rotate
 * RECTANGLES only - see its own header comment - so none of them can back a
 * per-pixel coverage value. Every AA function below maps its own pixels,
 * landscape to panel, through one shared static helper in shapes.c
 * (aa_composite_land); see that function's comment for the mapping and why
 * it is the one deliberate exception to "gfx owns rotation".
 */

// Fills out[0..rows-1] with the half-width, at that row, of a circle of the
// given radius, on a `rows`-tall grid centred at rows/2.0 (row centre is
// (row+0.5) - rows/2.0). radius may be smaller than rows/2: rows whose
// |dy| >= radius come out 0, which is what an annulus's inner-radius table
// needs on the rows above and below the hole. Call once per shape at file
// scope, not per frame: this runs sqrtf, same reasoning as timer.c's
// comment on the same tradeoff. Used directly by timer.c (its own ring).
// menu.c's chrono icon no longer needs it: the wedge that once used it for
// its curved edge's row widths was removed 2026-08-14, when that icon was
// redrawn as plain ink (a ring and a crown, no pie-slice) - see
// draw_icon_chrono's own header comment.
void shapes_fill_half_width_table(int16_t *out, int rows, float radius);

// Draws one row of a NON-anti-aliased annulus (a ring) at landscape y,
// centred horizontally on cx: two bars (the left and right arcs) when this
// row still clears the inner circle (hwInner > 0), collapsing to a single
// bar spanning the full outer width when it does not. Used by timer.c only
// now (its progress ring, which is drawn in a flat grey TRACK colour behind
// the black arc, not the black-ink-on-white case anti-aliasing targets -
// see timer.c's own header comment on why that stays a flat fill). menu.c's
// stopwatch icon uses shapes_fill_annulus_aa_land below instead.
void shapes_draw_annulus_row(int cx, int y, int16_t hwOuter, int16_t hwInner,
                              uint16_t color);

/* -------------------------------------------------------------------------
 * Anti-aliased primitives.
 *
 * Coverage-based, matching sketch.c's draw_capsule exactly: a signed
 * distance to the shape (negative inside, positive outside), turned into an
 * 8-bit grey value and composited by MIN against whatever is already in the
 * framebuffer - darkest wins, never alpha blending. This matters here for
 * the same reason sketch.c's header comment gives: menu.c's icons overlap
 * shapes on purpose (the chrono icon's crown starts centred inside the
 * ring's own painted band, the hourglass outline's two sides converge at
 * the waist), and blending would re-darken that overlap and band the
 * result; MIN just unions the ink.
 *
 * `colorPx` is carried through from the old API (menu.c always passes
 * PX_BLACK today) rather than hardcoding black, so a future caller drawing
 * something other than black ink is not blocked by this file.
 * ---------------------------------------------------------------------- */

// A filled disc, landscape centre (cx, cy), radius r. No current caller in
// menu.c: the hourglass's two individual "fallen grain" discs, the one
// place this device ever used a genuinely round dot rather than a curve or
// a stroke, were deleted 2026-08-14 - they floated, touching nothing,
// which is exactly the "loose grain of sand" the owner's ink-strokes rule
// forbids. Kept in shapes.h rather than deleted alongside them: a disc is
// a legitimate primitive on its own (a zero-length capsule, per
// shapes_fill_capsule_aa_land's own construction), just not one this
// device currently has a use for.
void shapes_fill_disc_aa_land(float cx, float cy, float r, uint16_t colorPx);

// A filled ring: an outer disc with an inner disc's worth of ink taken back
// out, both edges anti-aliased independently. This is the standard SDF
// "subtraction" combinator (shape = A minus B has signed distance
// max(distToA, -distToB) - see shapes.c for the two-line derivation), which
// is also why this needs no half-width table the way the old, non-AA ring
// did: the two radii are enough to place both edges analytically, no
// per-row sampling involved. Used by menu.c's chrono icon for the
// stopwatch's ring.
void shapes_fill_annulus_aa_land(float cx, float cy, float rOuter, float rInner,
                                  uint16_t colorPx);

// Fills the region spanning landscape rows [topY, topY+rows): row `topY+i`
// is filled from leftX[i] to rightX[i] (landscape x, one sample per row,
// both inclusive of the row's own true boundary - see shapes.c for how the
// edge itself is anti-aliased). The top and bottom of the range (row 0 and
// row rows-1) are NOT feathered vertically - they are hard, axis-aligned
// cutoffs, the same as the flat rectangle this primitive replaces would
// have been at those two rows - so a caller that wants a rounded cap there
// too has to ask for one separately (see menu.c's hourglass icon, which
// deliberately keeps its flat caps).
//
// One primitive backs every bulge-shaped menu.c fill: leftX = cx-hw,
// rightX = cx+hw, a mirrored pair from one half-width table - the
// hourglass's sand (both the dip's two slivers and the solid mass below
// it) and its resting heap. Until 2026-08-14 this also carried the
// stopwatch's quarter-pie wedge (leftX constant, rightX a circular arc)
// and its small diamond tab (hw = R-|dy|, a straight-sided tent); both
// were dropped when that icon was redrawn as plain ink - see
// draw_icon_chrono's header comment - which is why only the bulge case
// remains as a caller today. Nothing about the fill loop cared which
// shape it was drawing either way: it only ever reads x-per-row samples,
// circular, elliptical or straight-edged alike.
void shapes_fill_between_curves_aa_land(int topY, int rows,
                                         const int16_t *leftX, const int16_t *rightX,
                                         uint16_t colorPx);

// A single round-capped capsule from (x0,y0,r0) to (x1,y1,r1) - sketch.c's
// draw_capsule, landscape-mapped (see that function's own header comment
// for the signed-distance-to-segment construction and why composition is
// MIN, not blending). The building block for a tapered stroke along a
// path: call it once per straight span, or chain several for a polyline -
// menu.c's hourglass outline chains a march of these along its own curve
// table, the same way its old, non-AA version marched
// shapes_fill_thick_segment_land calls. menu.c's chrono icon uses a single
// call for its crown, deliberately started centred inside the ring's own
// annulus rather than touching its edge, so the two shapes are guaranteed
// to overlap; its proposed coil alternative chains many calls the same
// way the hourglass does, along a spiral instead of the bulb table.
void shapes_fill_capsule_aa_land(float x0, float y0, float r0,
                                  float x1, float y1, float r1,
                                  uint16_t colorPx);

// A tapered stroke along a quadratic Bezier from (x0,y0) to (x1,y1) with
// control point (cx,cy), half-width interpolated linearly r0->r1 - the
// anti-aliased equivalent of the deleted shapes_fill_tapered_quad_land,
// built the same way sketch.c's draw_quad_midpoint is: flatten the curve
// into a handful of chords (cheap and good enough - see sketch.c's own
// comment on why an exact arc length is not needed to pick a chord count)
// and stamp each chord with shapes_fill_capsule_aa_land. Given three
// consecutive waypoints A, B, C, call this with (x0,y0)=midpoint(A,B),
// (cx,cy)=B, (x1,y1)=midpoint(B,C): consecutive calls built this way share
// endpoints exactly, so a whole polyline of waypoints comes out as one
// continuous curve through their midpoints, not a chain of straight facets
// meeting at hard corners. Used by menu.c's squiggle icon.
void shapes_fill_tapered_quad_aa_land(float x0, float y0, float r0,
                                       float cx, float cy,
                                       float x1, float y1, float r1,
                                       uint16_t colorPx);

#endif // SHAPES_H
