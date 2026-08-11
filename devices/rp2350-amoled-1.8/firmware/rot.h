/*
 * rot: landscape-to-panel rotation, shared by every app that draws in
 * landscape (chrono, and now menu.c) while the physical framebuffer stays in
 * the panel's native portrait layout (368 wide x 448 tall).
 *
 * This is a straight lift of what used to be apps/chrono/main.c's own
 * land_to_panel_rect() / fill_rect_land() / push_dirty_land(), unchanged, so
 * chrono and the menu share one implementation instead of two copies quietly
 * drifting apart. The sketchpad (firmware/main.c) draws portrait and does
 * not include this header at all.
 *
 * Contract for anything that includes this header: by the point of
 * `#include "rot.h"`, the including .c file must already have defined, at
 * file scope, in panel (non-rotated) coordinates:
 *
 *   #define PANEL_W AMOLED_1IN8_WIDTH
 *   #define PANEL_H AMOLED_1IN8_HEIGHT
 *   #define LAND_W  PANEL_H
 *   #define LAND_H  PANEL_W
 *   static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t colorPx);
 *   static void push_dirty(uint16_t *fb, int minX, int minY, int maxX, int maxY);
 *
 * push_dirty is not defined here on purpose. It is the row-aligned panel
 * push documented in docs/decisions/0001-push-min-width.md: every pushed
 * window's row length must be a multiple of 8 pixels or
 * AMOLED_1IN8_DisplayWindows corrupts the transfer. That rule already has
 * one home in firmware/main.c and one in apps/chrono/main.c (menu.c carries
 * a third, being a separate translation unit compiled into both apps); this
 * header intentionally does not become a fourth, separate implementation to
 * keep in sync. It only rotates a rectangle and hands it to whichever
 * push_dirty/fill_rect the including file already has.
 *
 * Mapping, unchanged from the original: landscape (lx, ly), lx in
 * [0, LAND_W), ly in [0, LAND_H), maps to panel (px, py) =
 * (PANEL_W - 1 - ly, lx). Rotated so the wanted orientation - buttons along
 * the top edge - comes out right; see apps/chrono/main.c's git history for
 * the (rejected) 90-degrees-the-other-way mapping that came out upside down
 * on real hardware.
 *
 * Corners of a landscape rectangle (lx, ly, w, h) under that map:
 *   (lx,     ly    ) -> (PANEL_W-1-ly,       lx      )
 *   (lx+w-1, ly    ) -> (PANEL_W-1-ly,       lx+w-1  )
 *   (lx,     ly+h-1) -> (PANEL_W-1-(ly+h-1), lx      )
 *   (lx+w-1, ly+h-1) -> (PANEL_W-1-(ly+h-1), lx+w-1  )
 * px spans [PANEL_W-(ly+h), PANEL_W-1-ly], a run of length h.
 * py spans [lx, lx+w-1], a run of length w.
 * So the panel rectangle is (px = PANEL_W-(ly+h), py = lx, width = h,
 * height = w): width and height swap, as any 90 degree rotation must.
 */
#ifndef ROT_H
#define ROT_H

#include <stdint.h>

_Static_assert(LAND_W == PANEL_H && LAND_H == PANEL_W,
               "landscape canvas must be the panel's dimensions swapped");

static void land_to_panel_rect(int lx, int ly, int w, int h,
                                int *px, int *py, int *pw, int *ph) {
    *px = PANEL_W - (ly + h);
    *py = lx;
    *pw = h;
    *ph = w;
}

// Same as fill_rect, but (x, y, w, h) are landscape coordinates: converted
// to a panel rectangle via land_to_panel_rect() before filling.
static void fill_rect_land(uint16_t *fb, int lx, int ly, int w, int h, uint16_t colorPx) {
    int px, py, pw, ph;
    land_to_panel_rect(lx, ly, w, h, &px, &py, &pw, &ph);
    fill_rect(fb, px, py, pw, ph, colorPx);
}

// Converts a landscape rectangle to panel space and pushes it through the
// including file's own push_dirty, so the 8-pixel row-length rule still
// applies, now to the rotated (panel-space) width.
static void push_dirty_land(uint16_t *fb, int lx, int ly, int w, int h) {
    int px, py, pw, ph;
    land_to_panel_rect(lx, ly, w, h, &px, &py, &pw, &ph);
    push_dirty(fb, px, py, px + pw - 1, py + ph - 1);
}

#endif // ROT_H
