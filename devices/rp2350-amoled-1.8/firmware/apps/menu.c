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
 * Opening and closing the menu (the long-double-press gesture) is entirely
 * the runtime's business; this file never reads the PMIC and never calls
 * app_switch_to() with its own index. It only ever switches TO a real
 * g_apps[] entry, on a tap or on PWR-short.
 */
#include <stdio.h>

#include "app.h"
#include "gfx.h"
#include "sensors.h"
#include "menu.h"

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

// A circle, approximated by a stack of centred horizontal bars (the cheapest
// way to get a round silhouette out of axis-aligned rectangles), plus a
// small stem on top: a stopwatch's crown is what reads as "clock" instead of
// "ball".
static void draw_icon_chrono(int ox, int oy, uint16_t color) {
    static const int widths[8] = { 34, 56, 70, 78, 78, 70, 56, 34 };
    const int rowH = 10;
    const int totalH = 8 * rowH;
    int top = oy + (ICON_H - totalH) / 2;
    for (int r = 0; r < 8; r++) {
        int w = widths[r];
        int x = ox + (ICON_W - w) / 2;
        gfx_fill_rect_land(x, top + r * rowH, w, rowH - 2, color);
    }
    gfx_fill_rect_land(ox + ICON_W / 2 - 6, top - 12, 12, 12, color);
}

// A diagonal staircase of small filled squares: the cheapest thing that
// reads as "a pencil stroke" out of rectangles alone.
static void draw_icon_sketch(int ox, int oy, uint16_t color) {
    const int squares = 6;
    const int step = 14;
    const int sq = 12;
    int startX = ox + 6;
    int startY = oy + ICON_H - sq - 6;
    for (int i = 0; i < squares; i++) {
        gfx_fill_rect_land(startX + i * step, startY - i * step, sq, sq, color);
    }
}

// An hourglass: two triangles, apexes meeting in the middle, each
// approximated the same bars-not-curves way as the chrono icon above, just
// linear instead of round.
static void draw_icon_timer(int ox, int oy, uint16_t color) {
    static const int widths[8] = { 78, 64, 50, 34, 34, 50, 64, 78 };
    const int rowH = 10;
    const int totalH = 8 * rowH;
    int top = oy + (ICON_H - totalH) / 2;
    for (int r = 0; r < 8; r++) {
        int w = widths[r];
        int x = ox + (ICON_W - w) / 2;
        gfx_fill_rect_land(x, top + r * rowH, w, rowH - 2, color);
    }
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
