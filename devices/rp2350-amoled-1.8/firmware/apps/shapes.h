#ifndef SHAPES_H
#define SHAPES_H

#include <stdint.h>

/*
 * shapes: round silhouettes built out of axis-aligned rectangles.
 *
 * gfx rotates rectangles, not pixels (gfx.h), so a landscape app that wants
 * a circle draws it as a stack of horizontal 1px-tall bars, one
 * gfx_fill_rect_land() call per bar. timer.c was first to need this (its
 * ring of filled dots) and computed a half-width-per-row table once, up
 * front, rather than calling sqrtf per dot per frame. menu.c's stopwatch
 * icon needs the same technique twice more: a RING (an annulus - two bars
 * per row, collapsing to one for the rows above/below the inner circle)
 * and a quarter WEDGE. This file is that technique, factored out because
 * the annulus case is the fiddly part: getting the one-bar/two-bar switch
 * right is what makes a ring read as a ring instead of a filled disc at
 * its top and bottom.
 *
 * Used from menu.c only. timer.c does not use this: it is being edited by
 * another change at the same time this file was written, and touching it
 * here to share the disc case was judged not worth a merge conflict for a
 * few lines of duplication. Its own fill_half_width_table()/
 * draw_filled_dot() do the disc-only special case of
 * shapes_fill_half_width_table() below (radius == rows/2); it could adopt
 * this file later if someone wants the duplication gone.
 */

// Fills out[0..rows-1] with the half-width, at that row, of a circle of the
// given radius, on a `rows`-tall grid centred at rows/2.0 (row centre is
// (row+0.5) - rows/2.0, the same convention timer.c uses for its dots).
// radius may be smaller than rows/2: rows whose |dy| >= radius come out 0,
// which is exactly what an annulus's inner-radius table needs on the rows
// above and below the hole, and what a wedge's row table needs above its
// apex. Call once per shape at file scope, not per frame: this runs sqrtf,
// same reasoning as timer.c's comment on the same tradeoff.
void shapes_fill_half_width_table(int16_t *out, int rows, float radius);

// Draws one row of an annulus (a ring) at landscape y, centred horizontally
// on cx: two bars (the left and right arcs) when this row still clears the
// inner circle (hwInner > 0), collapsing to a single bar spanning the full
// outer width when it does not (the rows above/below the hole). Both half-
// widths come from shapes_fill_half_width_table(): hwOuter from the ring's
// outer radius, hwInner from its inner radius, both on the same `rows`
// grid so they line up row for row.
void shapes_draw_annulus_row(int cx, int y, int16_t hwOuter, int16_t hwInner,
                              uint16_t color);

// Draws a thick line segment from (x0,y0) to (x1,y1), landscape coordinates,
// as a march of overlapping filled squares of side `thickness`. This is how
// a landscape app draws a diagonal at all: gfx only fills rectangles (see
// gfx.h), there is no line primitive. The trap this avoids is the one the
// menu's old DRAW icon fell into - a diagonal built from squares spaced
// FARTHER apart than the squares themselves, which leaves the steps visible
// and reads as a staircase rather than a stroke. Samples are spaced at
// thickness/2, so consecutive squares always overlap by at least half their
// own size; that is enough overlap to merge into one continuous band at any
// angle this file's callers use, so `thickness` is the only knob a caller
// needs to get right, not a separate step size.
void shapes_fill_thick_segment_land(int x0, int y0, int x1, int y1,
                                     int thickness, uint16_t color);

// Draws a thick line segment from (x0,y0) to (x1,y1) with the stroke's
// half-width tapering LINEARLY from r0 to r1 along its length - the same
// march of overlapping stamps as shapes_fill_thick_segment_land, except the
// stamp is a square sized 2*r (not a fixed thickness) so the stroke can
// widen or narrow as it goes. This is the rectangle-stamp equivalent of
// firmware/apps/sketch.c's draw_capsule(), which does the same taper with
// anti-aliased circles for the live pen tool; a static icon on this
// monochrome panel does not need anti-aliasing, so a stamped square is
// enough. Spacing is r/2 using whichever of r0/r1 is LARGER at each end (a
// coarse spacing only leaves gaps where the stroke is at its widest), so a
// caller only ever has to pick radii, never a step count.
void shapes_fill_tapered_segment_land(int x0, int y0, int r0,
                                       int x1, int y1, int r1,
                                       uint16_t color);

// Draws a quadratic Bezier from (x0,y0) to (x1,y1) with control point
// (cx,cy), half-width tapering linearly from r0 to r1 - the rectangle-stamp
// equivalent of sketch.c's draw_quad_midpoint(). That function's own header
// comment explains the construction this is meant to be used the same way:
// given three consecutive waypoints A, B, C, call this with
// (x0,y0)=midpoint(A,B), (cx,cy)=B, (x1,y1)=midpoint(B,C). Consecutive calls
// built this way share endpoints exactly (this call's (x1,y1) is the next
// call's (x0,y0)), so a whole polyline of waypoints comes out as one
// continuous curve through their midpoints rather than a chain of straight
// facets meeting at hard corners - which is the same "sharp corners read as
// a constructed symbol, not a hand-drawn stroke" problem sketch.c's own
// comment there describes solving, just for a fixed set of waypoints
// instead of a live stream of touch samples.
void shapes_fill_tapered_quad_land(int x0, int y0, int r0,
                                    int cx, int cy,
                                    int x1, int y1, int r1,
                                    uint16_t color);

#endif // SHAPES_H
