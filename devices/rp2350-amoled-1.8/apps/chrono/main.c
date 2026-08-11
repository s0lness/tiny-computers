// chrono: a stopwatch. Second app for this board, meant to live in flash
// slot B alongside the sketchpad in slot A (see ../../store/README.md for
// the two-slot layout and ../../AGENTS.md / ../../README.md for the
// hardware truths this file leans on).
//
// Deliberately independent of firmware/main.c and firmware/lib/: this file
// only #includes the shared Waveshare drivers, it never edits them, so the
// sketchpad's hand-made patches (the DisplayWindows off-by-one and the
// push_dirty 8-pixel rule) stay untouched and this app still benefits from
// them.
//
// Display: "MM:SS:CC" — minutes, seconds, centiseconds, colon throughout
// (always ":", never a locale-flavoured comma). The panel itself is native
// portrait (368 wide x 448 tall); this app draws in landscape (448 x 368)
// through a coordinate rotation applied only in the drawing helpers, so the
// framebuffer, push_dirty and AMOLED_1IN8_Display all keep working in the
// panel's real, native orientation. See land_to_panel_rect() below.
//
// Buttons: PWR short press starts/stops, PWR long press switches app, BOOT
// click resets to 00:00:00. PWR is read through the AXP2101 PMIC and its
// GPIO2 interrupt line (see buttons_poll()); BOOT is read through bootbtn.c,
// which borrows the flash chip select briefly (see bootbtn.h) and is only
// ever polled, never read on every loop iteration via bootbtn_pressed().
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "pico/time.h"
#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "appswitch.h"
#include "bootbtn.h"
#include "qspi_pio.h"

#define PANEL_W AMOLED_1IN8_WIDTH
#define PANEL_H AMOLED_1IN8_HEIGHT

// The logical canvas this app draws into. Landscape: wider than it is tall,
// which is the panel's native dimensions with width and height swapped.
#define LAND_W PANEL_H
#define LAND_H PANEL_W

// Loop profiler: counts dirty pushes and pixels pushed per second, printed
// only in a second where something was actually drawn (an idle, stopped
// stopwatch pushes nothing at all). Also gates the per-toggle button
// latency print. See main()'s comment on what this actually measures.
#define PROFILE 1

/* ---------------------------------------------------------------------
 * Pixel helpers, copied from firmware/main.c rather than re-derived. The
 * panel wants RGB565 with the opposite byte order to a CPU uint16_t, so the
 * framebuffer is kept byte-swapped throughout. Pure black (0x0000) and pure
 * white (0xFFFF) are palindromic in bytes, so the swap is a no-op for this
 * app's two-colour image, but the helpers are used anyway rather than
 * hard-coding 0x0000/0xFFFF, so the framebuffer format stays in one place.
 * ------------------------------------------------------------------- */
static inline uint16_t px_swap(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t gray_to_px(uint8_t g) {
    uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    return px_swap(v);
}

/* ---------------------------------------------------------------------
 * push_dirty, copied verbatim (same algorithm, trimmed commentary) from
 * firmware/main.c. Do not "improve" this: docs/decisions/0001-push-min-width.md
 * is the record of a real hardware defect this works around. The rule,
 * bisected on this exact panel: every pushed window's row length must be a
 * multiple of 8 pixels (16 bytes), or AMOLED_1IN8_DisplayWindows corrupts
 * the transfer (a narrow window shreds into a ladder of horizontal ticks).
 * Where the window starts does not matter, only the rounded row length.
 *
 * This operates in PANEL coordinates, same as always. Rotation is handled
 * entirely upstream of this function, in land_to_panel_rect() below; once a
 * rectangle is in panel space it is pushed exactly like the sketchpad does.
 * ------------------------------------------------------------------- */
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
        // Slide the window left rather than clipping its width: shortening
        // the row is exactly what corrupts it. 368 is itself a multiple of
        // 8, so a left-slid window stays aligned.
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

/* ---------------------------------------------------------------------
 * Landscape-to-panel rotation. The framebuffer stays in the panel's native
 * portrait layout (368 wide x 448 tall) exactly as before; only the layout
 * and the digit drawing think in landscape (448 wide x 368 tall). This is a
 * fixed 90 degree rotation, not a reconfiguration of the panel or a second
 * buffer.
 *
 * Mapping: landscape (lx, ly), lx in [0, LAND_W), ly in [0, LAND_H), maps to
 * panel (px, py) = (ly, PANEL_H - 1 - lx).
 *
 * Checked against the four corners of a landscape rectangle (lx, ly, w, h):
 *   (lx,       ly      ) -> (ly,       PANEL_H-1-lx        )
 *   (lx+w-1,   ly      ) -> (ly,       PANEL_H-1-(lx+w-1)  )
 *   (lx,       ly+h-1  ) -> (ly+h-1,   PANEL_H-1-lx        )
 *   (lx+w-1,   ly+h-1  ) -> (ly+h-1,   PANEL_H-1-(lx+w-1)  )
 * px spans [ly, ly+h-1], a run of length h. py spans
 * [PANEL_H-1-(lx+w-1), PANEL_H-1-lx], a run of length w. So the panel
 * rectangle is (px=ly, py=PANEL_H-1-(lx+w-1), width=h, height=w): width and
 * height swap, which is exactly what a 90 degree rotation should do, and
 * px/py both land inside [0, PANEL_W) / [0, PANEL_H) whenever lx/ly/w/h were
 * inside the landscape canvas, since LAND_W == PANEL_H and LAND_H == PANEL_W.
 * ------------------------------------------------------------------- */
_Static_assert(LAND_W == PANEL_H && LAND_H == PANEL_W,
               "landscape canvas must be the panel's dimensions swapped");

// Rotated the opposite way to the first attempt, which came out upside down on
// the real device: the wanted orientation is buttons along the top edge.
// Mapping: landscape (lx, ly) -> panel (px, py) = (PANEL_W - 1 - ly, lx).
//
// Corners of a landscape rectangle (lx, ly, w, h) under that map:
//   (lx,     ly    ) -> (PANEL_W-1-ly,       lx      )
//   (lx+w-1, ly    ) -> (PANEL_W-1-ly,       lx+w-1  )
//   (lx,     ly+h-1) -> (PANEL_W-1-(ly+h-1), lx      )
//   (lx+w-1, ly+h-1) -> (PANEL_W-1-(ly+h-1), lx+w-1  )
// px spans [PANEL_W-(ly+h), PANEL_W-1-ly], a run of length h.
// py spans [lx, lx+w-1], a run of length w.
// So the panel rectangle is (px = PANEL_W-(ly+h), py = lx, width = h,
// height = w): width and height still swap, as any 90 degree rotation must,
// and this is the previous mapping turned through 180 degrees.
static void land_to_panel_rect(int lx, int ly, int w, int h,
                                int *px, int *py, int *pw, int *ph) {
    *px = PANEL_W - (ly + h);
    *py = lx;
    *pw = h;
    *ph = w;
}

/* ---------------------------------------------------------------------
 * Seven-segment digits, drawn as filled rectangles. No font, no
 * anti-aliasing (axis-aligned rectangles don't need it). Segment bits,
 * standard layout: a=top, b=top-right, c=bottom-right, d=bottom,
 * e=bottom-left, f=top-left, g=middle.
 * ------------------------------------------------------------------- */
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

// Same as fill_rect, but (x, y, w, h) are landscape coordinates: converted
// to a panel rectangle via land_to_panel_rect() before filling. Every digit
// segment is drawn through this, never through fill_rect directly, so the
// whole layout only has to be reasoned about in landscape space.
static void fill_rect_land(uint16_t *fb, int lx, int ly, int w, int h, uint16_t colorPx) {
    int px, py, pw, ph;
    land_to_panel_rect(lx, ly, w, h, &px, &py, &pw, &ph);
    fill_rect(fb, px, py, pw, ph, colorPx);
}

// Converts a landscape cell rectangle to panel space and pushes it, same
// rounding as push_dirty above (it calls push_dirty, it does not re-implement
// it), so the 8-pixel row-length rule still applies, now to the rotated
// (panel-space) width.
static void push_dirty_land(uint16_t *fb, int lx, int ly, int w, int h) {
    int px, py, pw, ph;
    land_to_panel_rect(lx, ly, w, h, &px, &py, &pw, &ph);
    push_dirty(fb, px, py, px + pw - 1, py + ph - 1);
}

static const uint8_t SEVEN_SEG[10] = {
    0x3F, // 0: a b c d e f
    0x06, // 1: b c
    0x5B, // 2: a b d e g
    0x4F, // 3: a b c d g
    0x66, // 4: b c f g
    0x6D, // 5: a c d f g
    0x7D, // 6: a c d e f g
    0x07, // 7: a b c
    0x7F, // 8: all
    0x6F, // 9: a b c d f g
};

// Draws one digit into a w x h cell at (x0,y0), segment thickness t, all in
// LANDSCAPE coordinates. Corner gaps of t x t between adjacent segments are
// deliberate (the classic seven-segment look) rather than a mistake in the
// geometry.
static void draw_digit(uint16_t *fb, int x0, int y0, int w, int h, int t,
                        int digit, uint16_t colorPx) {
    uint8_t segs = SEVEN_SEG[digit];
    int midY = y0 + h / 2 - t / 2;

    // Vertical segments run to the cell edge whenever the horizontal segment
    // that would have capped them is absent, so every digit occupies the full
    // cell height.
    //
    // Without this, a classic seven-segment "4" is visibly shorter than a "0":
    // 4 has neither the top nor the bottom bar, so its ink starts one segment
    // thickness down and ends one up, losing 2*t of height (36px here) against
    // its neighbours. Real LED displays have the same artifact and nobody
    // minds, because the segments are physically fixed; drawn at this size on
    // a screen it just reads as a smaller digit.
    int topY = (segs & 0x01) ? (y0 + t) : y0;          // below the top bar, or the very top
    int topH = midY - topY;
    int botY = midY + t;
    int botH = ((segs & 0x08) ? (y0 + h - t) : (y0 + h)) - botY;

    // When the middle bar is absent, an upper and lower vertical on the same
    // side are one continuous stroke, not two pieces with a gap. Otherwise a
    // "1" reads as two short dashes: it is b and c with no g between them, so
    // the middle segment's worth of space stays blank right through the digit.
    bool hasG = (segs & 0x40) != 0;
    int fullY = topY;
    int fullH = (botY + botH) - topY;

    if (segs & 0x01) fill_rect_land(fb, x0,     y0,     w, t, colorPx); // a
    if (hasG)        fill_rect_land(fb, x0,     midY,   w, t, colorPx); // g
    if (segs & 0x08) fill_rect_land(fb, x0,     y0+h-t, w, t, colorPx); // d

    // Left side: f (upper) and e (lower).
    if (!hasG && (segs & 0x20) && (segs & 0x10)) {
        fill_rect_land(fb, x0, fullY, t, fullH, colorPx);
    } else {
        if (segs & 0x20) fill_rect_land(fb, x0, topY, t, topH, colorPx); // f
        if (segs & 0x10) fill_rect_land(fb, x0, botY, t, botH, colorPx); // e
    }

    // Right side: b (upper) and c (lower).
    if (!hasG && (segs & 0x02) && (segs & 0x04)) {
        fill_rect_land(fb, x0 + w - t, fullY, t, fullH, colorPx);
    } else {
        if (segs & 0x02) fill_rect_land(fb, x0+w-t, topY, t, topH, colorPx); // b
        if (segs & 0x04) fill_rect_land(fb, x0+w-t, botY, t, botH, colorPx); // c
    }
}

// Two stacked dots, centred in a w x h cell, in LANDSCAPE coordinates. This
// is the only separator now: the owner was explicit, always ":", never a
// comma, so every gap between digit groups uses this and draw_comma is gone.
static void draw_colon(uint16_t *fb, int x0, int y0, int w, int h, int t, uint16_t colorPx) {
    int dotX = x0 + (w - t) / 2;
    fill_rect_land(fb, dotX, y0 + h/3 - t/2,       t, t, colorPx);
    fill_rect_land(fb, dotX, y0 + (2*h)/3 - t/2,   t, t, colorPx);
}

/* ---------------------------------------------------------------------
 * Layout, in LANDSCAPE coordinates (448 wide x 368 tall). 6 digits + 2
 * colons, sized to comfortably fill the 448px landscape width with an even
 * margin on both sides, vertically centred in the 368px landscape height.
 *
 * DIGIT_H (120) is the digit's landscape *height* — its long axis, since
 * digits stand upright — and it is the dimension that becomes the panel
 * push's row length after rotation (land_to_panel_rect swaps w and h). It is
 * kept a multiple of 8 for exactly the reason push_dirty rounds row length
 * up to a multiple of 8: so that rounding never has anything to do, and
 * every digit push is exactly its drawn size with no padding.
 *
 *   layout, left to right (element widths in brackets, all landscape x):
 *   [48][48] [24:] [48][48] [24:] [48][48]
 *    MM tens/units   SS tens/units   CC tens/units
 *   margin 14px each side, 12px gaps between every element (all landscape x).
 * ------------------------------------------------------------------- */
#define DIGIT_W 48
#define DIGIT_H 120
#define SEG_T   18
#define SEP_W   24
#define Y0      124   // (368 - DIGIT_H) / 2, landscape y

#define X_MM_TENS  14
#define X_MM_UNITS 74
#define X_COLON1   134
#define X_SS_TENS  170
#define X_SS_UNITS 230
#define X_COLON2   290
#define X_CS_TENS  326
#define X_CS_UNITS 386

static const int DIGIT_X[6] = { X_MM_TENS, X_MM_UNITS, X_SS_TENS, X_SS_UNITS, X_CS_TENS, X_CS_UNITS };

/* ---------------------------------------------------------------------
 * PWR button via the AXP2101 PMIC. PWR is not on a GPIO: it goes to the
 * PMIC's PWRON pin, which pulls GPIO2 low and latches a bit in PMIC
 * register 0x49 (bit0 release, bit1 press, bit2 long press, bit3 short
 * press). The two edge interrupts are off by default and must be enabled
 * in register 0x41 first. The PMIC times the long press itself (1.5s
 * default, measured ~1480ms), so no software timer is involved. Clearing a
 * latched bit is write-1-to-clear. See ../../firmware/main.c's
 * buttons_init()/buttons_poll() for the fuller version this is trimmed
 * from (that one also hunts other pins and drives a debug flash marker,
 * neither of which this app needs: the button is already found).
 *
 * Responsiveness: GPIO2 (AXP_IRQ) is polled every loop iteration — a bare
 * gpio_get(), effectively free — and the shared I2C bus is only touched on
 * the iteration where it reads low, i.e. where the PMIC actually has
 * something latched. That replaces a fixed 40ms blind poll (up to 40ms of
 * dead latency on every press) with detection inside a single loop
 * iteration, at less I2C traffic than the timer-based poll, not more.
 * ------------------------------------------------------------------- */
#define AXP2101_ADDR 0x34
#define AXP_IRQ_PIN  2

static void buttons_init(void) {
    gpio_init(AXP_IRQ_PIN);
    gpio_set_dir(AXP_IRQ_PIN, GPIO_IN);
    gpio_pull_up(AXP_IRQ_PIN);

    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x41, 0xFF); // arm both PWR edge interrupts
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, 0xFF); // clear anything already latched
    printf("chrono: buttons armed via AXP2101, GPIO2 irq-gated\r\n");
}

// Set by buttons_poll, consumed by the main loop. Bits are register 0x49's:
// 0x02 press, 0x04 long press. g_keyIrqUs is time_us_64() at the instant
// GPIO2 was observed low, i.e. the earliest moment this app could possibly
// have known about the event; the main loop uses it to report end-to-end
// button latency.
static volatile uint8_t g_keyEvent;
static volatile uint64_t g_keyIrqUs;

static void buttons_poll(void) {
    if (gpio_get(AXP_IRQ_PIN)) return; // idle high: nothing latched, no I2C needed

    uint64_t irqUs = time_us_64();
    uint8_t s1 = DEV_I2C_Read_Byte(AXP2101_ADDR, 0x49);
    if (s1) {
        DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, s1); // write-1-to-clear whatever we saw
    }
    // Only the press (0x02) and long-press (0x04) bits drive an action here.
    // The release bit (0x01) and the PMIC's short-press verdict (0x08, which
    // only latches once the finger lifts) are cleared above and otherwise
    // ignored, on purpose: see the main loop's event handler for why acting
    // on 0x08 would double the toggle.
    if (s1 & 0x06) {
        g_keyEvent = s1 & 0x06;
        g_keyIrqUs = irqUs;
    }
}

/* ---------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------- */
int main(void) {
    DEV_Module_Init();
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    AMOLED_1IN8_Init();
    AMOLED_1IN8_SetBrightness(180);

    // buttons_init() deliberately does NOT run here. It is the first thing in
    // this app that can block forever: it talks I2C, and the SDK's blocking
    // I2C calls have no timeout, so a bus left mid-transaction by the app we
    // just switched away from hangs startup with nothing on screen. Drawing
    // happens first, so a stuck bus costs the buttons and not the display.

    uint16_t black = gray_to_px(0);
    uint16_t white = gray_to_px(255);

    uint16_t *fb = (uint16_t *)malloc((size_t)PANEL_W * PANEL_H * 2);
    if (fb == NULL) {
        printf("chrono: framebuffer allocation failed\r\n");
        while (true) { }
    }
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = white;

    // Separators never change once drawn, so they are drawn once here and
    // never touched (and never pushed) again.
    draw_colon(fb, X_COLON1, Y0, SEP_W, DIGIT_H, SEG_T, black);
    draw_colon(fb, X_COLON2, Y0, SEP_W, DIGIT_H, SEG_T, black);

    int lastDigit[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++) {
        draw_digit(fb, DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, SEG_T, 0, black);
    }

    // One full-frame push at boot, like the sketchpad does. Everything
    // after this is a dirty rect over a single digit cell.
    AMOLED_1IN8_Display(fb);
    printf("chrono ready, stopped at 00:00:00\r\n");

    // Now the bus. The touch controller shares i2c1 with the PMIC and is left
    // configured by whichever app ran before this one; if it was mid-transfer
    // when we rebooted it can still be holding SDA down. Pulsing its reset
    // line releases it. This costs 90ms once and removes a whole class of
    // startup hang that is invisible from the outside.
    gpio_put(Touch_RST_PIN, 1);
    sleep_ms(20);
    gpio_put(Touch_RST_PIN, 0);
    sleep_ms(20);
    gpio_put(Touch_RST_PIN, 1);
    sleep_ms(50);

    buttons_init();

    bool running = false;
    uint64_t elapsedUs = 0;   // accumulated across all completed run segments
    uint64_t runStartUs = 0;  // time_us_64() at the start of the current segment

#if PROFILE
    uint32_t pf_pushes = 0, pf_pushPx = 0;
    uint32_t pf_lastMs = to_ms_since_boot(get_absolute_time());
#endif

    while (true) {
        buttons_poll();

        // BOOT is polled once per iteration via bootbtn_poll_clicked(), which
        // rate-limits its own sampling internally (see bootbtn.c) — never
        // bootbtn_pressed() here, which would borrow the flash chip select
        // on every single loop iteration instead of at most once per sample
        // window.
        if (bootbtn_poll_clicked()) {
            running = false;
            elapsedUs = 0;
            printf("chrono: BOOT click, reset to 00:00:00\r\n");
        }

        if (g_keyEvent) {
            uint8_t ev = g_keyEvent;
            g_keyEvent = 0;
            uint64_t irqUs = g_keyIrqUs;

            if (ev & 0x04) {
                // Long press switches app. This shares the 0x02 press bit
                // below with every long press (a long press begins as a
                // press), which is safe: appswitch_go_other() reboots and
                // never returns, so any toggle the press below performed on
                // the way here is discarded along with the rest of this
                // app's state. Do not add logic to distinguish a toggle-press
                // from a press-that-becomes-a-long-press: the PMIC itself
                // cannot tell them apart until the long-press threshold has
                // already passed, so neither can we.
                // Switching requires BOTH buttons: PWR held past the PMIC's
                // 1.5s long-press threshold WITH BOOT down at that moment.
                // One read of BOOT per long press, never in the hot loop.
                //
                // The chord was dropped earlier on the belief that reading
                // BOOT hung the board. That was wrong: the hangs came from a
                // flash probe in appswitch and from flashes that silently
                // never landed while the app was already hung, so the fixes
                // under test were never actually running. BOOT reads fine, and
                // this app has been polling it for reset without trouble.
                if (bootbtn_pressed()) {
                    bootbtn_consume_next_click();  // do not also reset on release
                    appswitch_go_other();
                }
            } else if (ev & 0x02) {
                // Toggle on the PRESS (bit 0x02), sampled the instant GPIO2
                // asserted in buttons_poll(), not on the PMIC's short-press
                // verdict (bit 0x08), which only latches once the finger
                // lifts. Toggling on release is exactly what made stop feel
                // sluggish. time_us_64() is read here, before any drawing or
                // printing, so the recorded stop time is not polluted by the
                // redraw that follows in this same iteration.
                uint64_t nowUs = time_us_64();
                if (running) {
                    elapsedUs += nowUs - runStartUs;
                    running = false;
                } else {
                    runStartUs = nowUs;
                    running = true;
                }

                if (running) {
                    printf("chrono: press, start\r\n");
                } else {
                    uint32_t cs = (uint32_t)(elapsedUs / 10000ULL);
                    printf("chrono: press, stop at %02u:%02u:%02u\r\n",
                           (unsigned)((cs / 6000) % 100), (unsigned)((cs / 100) % 60),
                           (unsigned)(cs % 100));
                }
#if PROFILE
                // End-to-end latency: GPIO2 going low (earliest possible
                // detection) to the toggle above being handled. This is the
                // number that says whether the press-edge/IRQ-gated rework
                // actually helped, measured rather than assumed.
                printf("chrono: toggle latency=%luus\r\n",
                       (unsigned long)(uint32_t)(nowUs - irqUs));
#endif
            }
        }

        uint64_t shownUs = running ? elapsedUs + (time_us_64() - runStartUs) : elapsedUs;
        uint64_t totalCs = shownUs / 10000ULL; // 1 centisecond = 10,000us
        // Minutes wrap at 100 so the layout stays fixed at 2 digits; a
        // stopwatch running past 99:59:99 is not a case this puck's use
        // needs to handle cleanly.
        uint32_t mm = (uint32_t)((totalCs / 6000ULL) % 100ULL);
        uint32_t ss = (uint32_t)((totalCs / 100ULL) % 60ULL);
        uint32_t cs = (uint32_t)(totalCs % 100ULL);
        int digits[6] = {
            (int)(mm / 10), (int)(mm % 10),
            (int)(ss / 10), (int)(ss % 10),
            (int)(cs / 10), (int)(cs % 10),
        };

        // Only repaint digits that actually changed, and do it in this same
        // iteration (no deferring to a later tick), so a reset or a stop
        // shows the frozen/zeroed time at once. Centiseconds-units
        // (digits[5]) changes on essentially every loop iteration while
        // running (every 10ms); minutes almost never does. Each push here
        // covers exactly one 48x120 landscape digit cell — 5,760px, up from
        // the portrait layout's 3,840px because the digit itself is bigger,
        // not because of the rotation — and DIGIT_H (120) is already a
        // multiple of 8, so push_dirty never has to pad the pushed window's
        // row length (the panel-space width, which is this cell's landscape
        // height after rotation).
        for (int i = 0; i < 6; i++) {
            if (digits[i] == lastDigit[i]) continue;
            fill_rect_land(fb, DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, white);
            draw_digit(fb, DIGIT_X[i], Y0, DIGIT_W, DIGIT_H, SEG_T, digits[i], black);
            push_dirty_land(fb, DIGIT_X[i], Y0, DIGIT_W, DIGIT_H);
            lastDigit[i] = digits[i];
#if PROFILE
            pf_pushes++;
            pf_pushPx += (uint32_t)DIGIT_W * (uint32_t)DIGIT_H;
#endif
        }

#if PROFILE
        {
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (nowMs - pf_lastMs >= 1000) {
                // Quiet when nothing was drawn (stopped, idle), same as the
                // sketchpad's profiler: an idle line says nothing useful.
                if (pf_pushes > 0) {
                    printf("prof pushes=%lu totalPx=%lu avgPx/push=%lu\r\n",
                           (unsigned long)pf_pushes, (unsigned long)pf_pushPx,
                           (unsigned long)(pf_pushPx / pf_pushes));
                }
                pf_pushes = 0;
                pf_pushPx = 0;
                pf_lastMs = nowMs;
            }
        }
#endif
    }
}
