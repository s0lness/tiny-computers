// See menu.h for the API and why input arrives through a callback instead of
// menu.c reading the PMIC itself.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/time.h"
#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "appswitch.h"
#include "bootbtn.h"
#include "menu.h"

#define PANEL_W AMOLED_1IN8_WIDTH
#define PANEL_H AMOLED_1IN8_HEIGHT
#define LAND_W  PANEL_H
#define LAND_H  PANEL_W

/* ---------------------------------------------------------------------
 * Pixel helpers and push_dirty/fill_rect, copied verbatim (same algorithm)
 * from apps/chrono/main.c, which itself copied them from firmware/main.c.
 * This is menu.c's own third copy rather than a shared function, on purpose:
 * see rot.h's header comment for why push_dirty in particular is not
 * centralised. docs/decisions/0001-push-min-width.md is the record of the
 * hardware defect this works around; do not "improve" the rounding here.
 * ------------------------------------------------------------------- */
static inline uint16_t px_swap(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t gray_to_px(uint8_t g) {
    uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    return px_swap(v);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t colorPx) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (size_t)(y + j) * PANEL_W + x;
        for (int i = 0; i < w; i++) row[i] = colorPx;
    }
}

#define PUSH_GRAN_W 8
#define PUSH_MIN_W  8

static void push_dirty(uint16_t *fb, int minX, int minY, int maxX, int maxY) {
    if (minX > maxX || minY > maxY) return;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;

    int x0 = minX & ~1;
    int w = maxX + 1 - x0;
    w = (w + PUSH_GRAN_W - 1) & ~(PUSH_GRAN_W - 1);
    if (w < PUSH_MIN_W) w = PUSH_MIN_W;
    int x1 = x0 + w;
    if (x1 > PANEL_W) {
        x0 = PANEL_W - w;
        if (x0 < 0) { x0 = 0; w = PANEL_W; }
        x0 &= ~1;
        x1 = x0 + w;
        if (x1 > PANEL_W) { x0 = PANEL_W - w; x1 = PANEL_W; }
    }

    int y0 = minY & ~1;
    int y1 = maxY + 1;
    if (y1 & 1) y1++;
    if (y1 > PANEL_H) y1 = PANEL_H;
    if (y1 <= y0) y1 = y0 + 2;

    AMOLED_1IN8_DisplayWindows(x0, y0, x1, y1, fb);
}

#include "rot.h"

/* ---------------------------------------------------------------------
 * A minimal 5x7 block font, hand-authored for this menu: uppercase A-Z,
 * digits 0-9, space and hyphen. That is every character APP_NAMES actually
 * uses today, plus enough headroom for short future slugs; it is not full
 * ASCII. An unsupported character is drawn as a blank cell rather than
 * faulting, so a future manifest-provided name with e.g. punctuation this
 * font lacks degrades to a gap, not a crash - see the APP_NAMES comment
 * below for the TODO on where names should eventually come from.
 *
 * Not verified on real hardware tonight (no board available - see the task
 * this file was written for). Each glyph was authored and cross-checked by
 * hand against the row-by-row bit patterns in the comments below; visually
 * inspect on first flash before trusting it further.
 *
 * Encoding: 7 rows top-to-bottom, each row's low 5 bits are columns
 * left-to-right (bit 4 = leftmost column, bit 0 = rightmost).
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
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c == '-') return 37;
    return 0; // space, and anything unsupported: draw a blank cell
}

/* ---------------------------------------------------------------------
 * App names. A static table for now, indexed by slot.
 *
 * TODO(manifest): the store writes a 4KB manifest partition per
 * store/README.md, meant to eventually carry real per-slot metadata
 * including a name. This table should read from there once that format
 * exists. Not done tonight: menu.c is the first thing that would ever
 * consume it, so there is no format to read yet, and there is no hardware
 * available to validate a partition-reading code path against - getting
 * that wrong reads as a hang with nothing on screen, which is exactly the
 * failure mode this repo's bring-up log warns cost real time to diagnose
 * blind. A static table ships something true tonight without inventing an
 * on-flash format under those conditions.
 * ------------------------------------------------------------------- */
static const char *const APP_NAMES[APPSWITCH_SLOT_COUNT] = {
    "DRAW",
    "CHRONO",
};

/* ---------------------------------------------------------------------
 * Layout, in landscape coordinates (448 wide x 368 tall, same canvas
 * chrono's digits use). A row of APPSWITCH_SLOT_COUNT boxes, centred both
 * ways, current slot's box starts highlighted (filled black, white text);
 * everything else is an outlined box with black text on white.
 * ------------------------------------------------------------------- */
#define FONT_SCALE   3
#define CHAR_W       (FONT_W * FONT_SCALE)        // 15
#define CHAR_H       (FONT_H * FONT_SCALE)        // 21
#define CHAR_GAP     (1 * FONT_SCALE)              // 3

#define MENU_BOX_W    150
#define MENU_BOX_H    72
#define MENU_GAP      16
#define MENU_PAD      12
#define MENU_BORDER_T 3

#define MENU_N APPSWITCH_SLOT_COUNT

#define MENU_STRIP_W ((MENU_N) * MENU_BOX_W + ((MENU_N) - 1) * MENU_GAP + 2 * MENU_PAD)
#define MENU_STRIP_H (MENU_BOX_H + 2 * MENU_PAD)
#define MENU_LX      ((LAND_W - MENU_STRIP_W) / 2)
#define MENU_LY      ((LAND_H - MENU_STRIP_H) / 2)

// If APPSWITCH_SLOT_COUNT ever grows without revisiting this layout, fail
// the build loudly rather than silently drawing a menu that runs off the
// panel or blows the SRAM budget below.
_Static_assert(MENU_STRIP_W <= LAND_W,
               "menu strip too wide for APPSWITCH_SLOT_COUNT; shrink MENU_BOX_W/MENU_GAP or add wrapping");
_Static_assert(MENU_STRIP_H <= LAND_H, "menu strip too tall for the landscape canvas");

/* ---------------------------------------------------------------------
 * The menu overlay is a bounded rectangle, not the full screen, and its
 * pixels are saved before drawing and restored on cancel. This is not
 * cosmetic: a full-framebuffer snapshot (330KB) does not fit alongside the
 * live framebuffer (also 330KB) in 520KB of SRAM (see AGENTS.md's framing
 * of the same budget for why anti-aliasing here has no second buffer
 * either), and more importantly the sketchpad's canvas has no other
 * representation to fall back to - see menu.h's comment on why this save/
 * restore is load bearing there. MENU_STRIP_W x MENU_STRIP_H x 2 bytes is
 * the actual cost; kept well under the full frame on purpose.
 * ------------------------------------------------------------------- */
_Static_assert((uint32_t)MENU_STRIP_W * MENU_STRIP_H * 2 < 100000u,
               "menu save buffer bigger than intended; shrink the box layout");

static uint16_t g_menuSave[MENU_STRIP_W * MENU_STRIP_H];

static void menu_save_rect(const uint16_t *fb, int px, int py, int pw, int ph) {
    for (int row = 0; row < ph; row++) {
        const uint16_t *src = fb + (size_t)(py + row) * PANEL_W + px;
        uint16_t *dst = g_menuSave + (size_t)row * pw;
        for (int col = 0; col < pw; col++) dst[col] = src[col];
    }
}

static void menu_restore_rect(uint16_t *fb, int px, int py, int pw, int ph) {
    for (int row = 0; row < ph; row++) {
        uint16_t *dst = fb + (size_t)(py + row) * PANEL_W + px;
        const uint16_t *src = g_menuSave + (size_t)row * pw;
        for (int col = 0; col < pw; col++) dst[col] = src[col];
    }
}

static int menu_text_width(const char *s) {
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    if (n == 0) return 0;
    return n * CHAR_W + (n - 1) * CHAR_GAP;
}

static void menu_draw_text(uint16_t *fb, int lx, int ly, const char *s, uint16_t colorPx) {
    int x = lx;
    for (const char *p = s; *p; p++) {
        const uint8_t *rows = FONT5X7[font_index(*p)];
        for (int r = 0; r < FONT_H; r++) {
            uint8_t bits = rows[r];
            for (int c = 0; c < FONT_W; c++) {
                if (bits & (1u << (FONT_W - 1 - c))) {
                    fill_rect_land(fb, x + c * FONT_SCALE, ly + r * FONT_SCALE,
                                   FONT_SCALE, FONT_SCALE, colorPx);
                }
            }
        }
        x += CHAR_W + CHAR_GAP;
    }
}

static void menu_draw_all(uint16_t *fb, int cursor, uint16_t white, uint16_t black) {
    fill_rect_land(fb, MENU_LX, MENU_LY, MENU_STRIP_W, MENU_STRIP_H, white);

    for (int i = 0; i < MENU_N; i++) {
        int bx = MENU_LX + MENU_PAD + i * (MENU_BOX_W + MENU_GAP);
        int by = MENU_LY + MENU_PAD;
        bool hi = (i == cursor);

        if (hi) {
            fill_rect_land(fb, bx, by, MENU_BOX_W, MENU_BOX_H, black);
        } else {
            fill_rect_land(fb, bx, by, MENU_BOX_W, MENU_BORDER_T, black);
            fill_rect_land(fb, bx, by + MENU_BOX_H - MENU_BORDER_T, MENU_BOX_W, MENU_BORDER_T, black);
            fill_rect_land(fb, bx, by, MENU_BORDER_T, MENU_BOX_H, black);
            fill_rect_land(fb, bx + MENU_BOX_W - MENU_BORDER_T, by, MENU_BORDER_T, MENU_BOX_H, black);
        }

        const char *name = APP_NAMES[i];
        int tw = menu_text_width(name);
        int tx = bx + (MENU_BOX_W - tw) / 2;
        int ty = by + (MENU_BOX_H - CHAR_H) / 2;
        menu_draw_text(fb, tx, ty, name, hi ? white : black);
    }
}

/* ---------------------------------------------------------------------
 * BOOT long-press threshold for "cancel", inside the menu only. This is
 * unrelated to PWR's 1.5s hardware long-press threshold (PMIC register
 * 0x27): BOOT has no hardware press-duration timer at all, only the raw
 * level bootbtn.c can sample, so this is timed entirely in software here.
 * Chosen shorter than PWR's, because there is no confirm-on-release step to
 * absorb extra latency the way PWR's short-press verdict (bit 0x08) does:
 * cancel fires the instant the hold crosses this threshold, still held,
 * exactly like PWR's hardware long press feels - waiting for release would
 * read as laggy for a gesture whose whole point is "get me out now".
 * ------------------------------------------------------------------- */
#define MENU_BOOT_LONGPRESS_MS 500
#define MENU_LOOP_DELAY_MS     5

static void menu_cleanup_and_push(uint16_t *fb, int px, int py, int pw, int ph) {
    menu_restore_rect(fb, px, py, pw, ph);
    push_dirty(fb, px, py, px + pw - 1, py + ph - 1);
}

int menu_run(uint16_t *fb, menu_pwr_poll_fn poll_pwr) {
    uint16_t white = gray_to_px(255);
    uint16_t black = gray_to_px(0);

    // Where the highlight starts. APPSWITCH_SLOT_UNKNOWN only happens for an
    // unpartitioned dev build (plain `picotool load`, no store); default to
    // slot 0 there rather than refusing to open the menu at all. In that
    // case "launching slot 0" and "cancel" both read as slot 0, which is a
    // harmless ambiguity that only exists outside the real store layout.
    appswitch_slot_t curEnum = appswitch_current_slot();
    int curSlot = (curEnum == APPSWITCH_SLOT_UNKNOWN) ? 0 : (int)curEnum;
    int cursor = curSlot;

    int px, py, pw, ph;
    land_to_panel_rect(MENU_LX, MENU_LY, MENU_STRIP_W, MENU_STRIP_H, &px, &py, &pw, &ph);

    menu_save_rect(fb, px, py, pw, ph);
    menu_draw_all(fb, cursor, white, black);
    push_dirty_land(fb, MENU_LX, MENU_LY, MENU_STRIP_W, MENU_STRIP_H);
    printf("menu: open, current=%d\r\n", curSlot);

    bool bootWasDown = false;
    uint32_t bootDownSinceMs = 0;

    for (;;) {
        uint8_t ev = poll_pwr();

        if (ev & 0x08) {
            // Confirmed short press (fires on release), not the raw press
            // edge (0x02): every long press also starts as a 0x02 edge, and
            // acting on that here would nudge the cursor one slot before the
            // eventual long press launches whatever is then highlighted -
            // launching the wrong app. Waiting for the release-confirmed bit
            // is what avoids that; see menu.h and the callers' g_keyEvent
            // masks for where 0x08 is threaded through.
            cursor = (cursor + 1) % MENU_N;
            printf("menu: next -> %d\r\n", cursor);
            menu_draw_all(fb, cursor, white, black);
            push_dirty_land(fb, MENU_LX, MENU_LY, MENU_STRIP_W, MENU_STRIP_H);
        } else if (ev & 0x04) {
            if (cursor == curSlot) {
                printf("menu: cancel (same slot)\r\n");
                menu_cleanup_and_push(fb, px, py, pw, ph);
                return -1;
            }
            printf("menu: launch slot %d\r\n", cursor);
            appswitch_go_to((uint8_t)cursor);
            // Only reached if the reboot request could not be issued.
            menu_cleanup_and_push(fb, px, py, pw, ph);
            return cursor;
        }

        uint32_t nowMs = to_ms_since_boot(get_absolute_time());
        bool lvl;
        if (bootbtn_poll_level(&lvl)) {
            if (lvl && !bootWasDown) {
                bootDownSinceMs = nowMs;
            }
            if (lvl && (nowMs - bootDownSinceMs >= MENU_BOOT_LONGPRESS_MS)) {
                printf("menu: cancel (BOOT long press)\r\n");
                // BOOT is still down at this instant (that is what just
                // triggered cancel); its eventual release must not also
                // read as a click to whatever the caller does with BOOT
                // clicks (chrono resets to 00:00:00 on one). Same mechanism
                // firmware/main.c already uses for the PWR+BOOT chord.
                bootbtn_consume_next_click();
                menu_cleanup_and_push(fb, px, py, pw, ph);
                return -1;
            }
            if (!lvl && bootWasDown && (nowMs - bootDownSinceMs < MENU_BOOT_LONGPRESS_MS)) {
                cursor = (cursor - 1 + MENU_N) % MENU_N;
                printf("menu: previous -> %d\r\n", cursor);
                menu_draw_all(fb, cursor, white, black);
                push_dirty_land(fb, MENU_LX, MENU_LY, MENU_STRIP_W, MENU_STRIP_H);
            }
            bootWasDown = lvl;
        }

        sleep_ms(MENU_LOOP_DELAY_MS);
    }
}
