#include <math.h>
#include <stdbool.h>
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "QMI8658.h"
#include "appswitch.h"
#include "bootbtn.h"
#include "menu.h"

#define PANEL_W AMOLED_1IN8_WIDTH
#define PANEL_H AMOLED_1IN8_HEIGHT

// Touch diagnostics. RETIRED since the multicore split below: it read
// FT3168 registers directly from what is now core0, and core0 must never
// touch i2c1 once core1 has started (see the big comment above
// core1_entry()). Left compiled out rather than deleted, in case it is ever
// worth porting into core1's own loop (no printf there, so it would need to
// publish counters instead of printing raw register values).
#define DEBUG_TOUCH 0

// Loop profiler: splits frame time across the suspects and prints once a
// second. Printing more often than that would perturb what it is trying to
// measure. Since touch sampling moved to core1 (see core1_entry()), this
// also tracks the touch queue between the cores and what core1 is doing on
// i2c1, so a regression on either core is visible from one printout.
#define PROFILE 1

#if PROFILE
static uint32_t pf_touch_us, pf_raster_us, pf_push_us;
static uint32_t pf_loops, pf_samples, pf_pushes, pf_pushPx, pf_reports;
// Added for the multicore split: how many queue entries core0 actually
// drained this second (as opposed to pf_samples, which only counts the ones
// that carried a finger down), the deepest the queue got between drains,
// and how many short capsules the curve interpolation emitted.
static uint32_t pf_drained, pf_qDepthPeak, pf_subsegs;
static uint32_t pf_lastMs;
static int pf_lastX = -1, pf_lastY = -1;
#define PF_NOW() time_us_32()
#define PF_ADD(acc, t0) do { (acc) += time_us_32() - (t0); } while (0)
#else
#define PF_NOW() 0
#define PF_ADD(acc, t0) do { (void)(t0); } while (0)
#endif

// Pen shape tuning (tldraw-style variable width draw tool).
// Smoothing is a latency knob as much as a smoothness one: at 0.55 the drawn
// point trails the finger by roughly two reports by construction, which was
// the largest contributor to felt lag once the pipeline itself measured clean
// (raster 9us, push 27us). 0.35 tightens it at the cost of some jitter.
#define STREAMLINE   0.35f
#define DEDUPE_PX    0.7f
#define SPEED_MAX    14.0f
#define PRESSURE_LERP 0.275f
#define PEN_SIZE     5.0f
#define PEN_THINNING 0.5f
#define START_TAPER_LEN 10.0f

// Quadratic-through-midpoints curve fill, following the technique
// aliceisjustplaying/tinydraw documented for this same board: a fast stroke
// only gets a raw report every 15-60px (measured: 50-61px on ours), so a
// straight capsule between consecutive reports reads as facets rather than a
// curve. Subdividing so each short capsule lands around this many pixels is
// fine enough that the facet disappears; CURVE_MAX_STEPS bounds the cost on
// the rare very long span (a bridged dropout, or the 150px MAX_JUMP_PX
// ceiling) so one big gap cannot spike raster time unboundedly.
#define CURVE_SEG_PX     2.5f
#define CURVE_MAX_STEPS  40

// The controller drops contact mid-stroke when the finger moves fast, which
// arrives as a brief run of zero-finger reports. Taken at face value that ends
// the stroke (drawing its end taper) and starts a new one a few pixels on
// (drawing a starting dot), and those taper-and-dot pairs are the "smudging"
// on fast strokes. So a lift is only believed after this long with no contact;
// anything shorter is treated as a dropout and the stroke continues across it.
#define LIFT_DEBOUNCE_MS 80

// Glitch rejection as a speed limit rather than a fixed distance, because the
// allowed jump has to grow with the gap: bridging an 80ms dropout legitimately
// covers more ground than one 17ms report.
//
// MAX_JUMP_PX is the hard ceiling and it matters more than the speed. Without
// it the speed limit alone permits a 480px jump across an 80ms dropout, which
// is most of the panel: lifting and touching down somewhere else then draws a
// straight line clean across the screen joining the two. Beyond this ceiling
// the gap is not treated as one stroke at all, it ends the stroke and starts
// a new one, which is what a lift and re-touch actually is.
// Measured from real strokes on this panel: a fast diagonal steps 50 to 61
// pixels between consecutive reports, so anything below about 4 px/ms rejects
// ordinary drawing. 6 px/ms leaves headroom; MAX_JUMP_PX is what actually
// stops a lift-and-retouch being joined by a line across the screen.
#define MAX_SPEED_PX_PER_MS 6.0f
#define MIN_JUMP_ALLOW_PX   40.0f
#define MAX_JUMP_PX         150.0f

// Ink is never laid down on the strength of a single report. A stray reading
// (a lone contact where there is no finger, or a position that jumped further
// than a finger could travel) is believed only once a second report agrees
// with it to within this distance. A real touch always produces a run of
// agreeing reports, so the only thing this refuses is a one-off, which is
// exactly what a stray dot is. The cost is one report of latency (~17ms) at
// the start of a stroke, and none at all during it.
#define CONFIRM_PX 25.0f

// Touch recovery. The FT3168 can be left in a state where it answers I2C with
// the right WhoAmI but never reports a finger again; observed after a picotool
// reboot, which resets the RP2350 without power-cycling the touch chip, so the
// state survives every reflash. If nothing has been reported for this long,
// pulse the reset line and re-arm the chip. ~110ms, invisible while idle.
// Now runs on core1 (touch_recover_core1()); the trigger is wall-clock time
// since the last observed finger, which is unaffected by whether core1 was
// gating its I2C reads on Touch_INT_PIN at the time (see core1_entry()).
#define TOUCH_STALL_MS 5000

// How long a PWR short press stays "recent" for the long-double-press
// gesture that opens the menu: press, release, press-and-hold within this
// window. See main()'s g_keyEvent handler for how it is evaluated, and
// menu.h for why the gesture is shaped this way (a stopwatch-style
// start-then-stop is two SHORT presses and can never arm-and-hold into it).
#define MENU_DOUBLE_WINDOW_MS 500

// Shake-to-erase tuning. Polled from core1 now (imu_poll_core1()), since the
// IMU shares i2c1 with touch and the PMIC; see the ownership comment above
// core1_entry().
#define IMU_POLL_MS    20
#define JOLT_DEV_MG    900.0f
#define JOLT_WINDOW_MS 700
#define JOLT_MIN_COUNT 4
#define ERASE_COOLDOWN_MS 1200
#define JOLT_MAX 16

/* ---------------------------------------------------------------------
 * Anti-aliased capsule rasterizer.
 *
 * The panel is monochrome (white paper, black ink), so every pixel is a
 * shade of grey, which RGB565 already stores as three correlated channels.
 * That lets us use the 6-bit green channel alone as an 8-bit coverage/ink
 * value instead of keeping a separate alpha buffer: read it back widened
 * to 8 bits, and rebuild R/G/B from it symmetrically on write.
 * ------------------------------------------------------------------- */
// The panel wants RGB565 with the opposite byte order to how the CPU stores a
// uint16_t, and the framebuffer is DMA'd out raw, so pixels are kept swapped.
//
// This only became visible once anti-aliasing arrived. Pure black (0x0000) and
// pure white (0xFFFF) are palindromic in bytes and look correct either way,
// which is why every earlier app was fine. A mid grey is not: 0x8410 swapped
// reads as 0x1084, a dark blue, so stroke edges turned into colour speckle
// while their solid interiors stayed black.
static inline uint16_t px_swap(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint8_t px_to_gray(uint16_t px) {
    uint16_t v = px_swap(px);
    return (uint8_t)(((v >> 5) & 0x3F) << 2);
}

static inline uint16_t gray_to_px(uint8_t g) {
    uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    return px_swap(v);
}

// Draws a round-capped capsule from a (radius r0) to b (radius r1) as a
// signed-distance-to-segment field, converted to per-pixel coverage.
// Composition is MIN (darkest wins), not alpha blending: consecutive
// stroke segments overlap heavily along their shared edge, and blending
// would re-darken that overlap on every segment, turning a smooth line
// into a visibly banded, pixelated one. MIN just unions the ink shapes.
static void draw_capsule(uint16_t *fb, float ax, float ay, float r0,
                          float bx, float by, float r1,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    float maxR = (r0 > r1 ? r0 : r1) + 1.0f;
    int minX = (int)floorf((ax < bx ? ax : bx) - maxR);
    int maxX = (int)ceilf((ax > bx ? ax : bx) + maxR);
    int minY = (int)floorf((ay < by ? ay : by) - maxR);
    int maxY = (int)ceilf((ay > by ? ay : by) + maxR);
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;
    if (minX > maxX || minY > maxY) return;

    float abx = bx - ax, aby = by - ay;
    float abLenSq = abx * abx + aby * aby;

    for (int iy = minY; iy <= maxY; iy++) {
        float py = (float)iy + 0.5f;
        for (int ix = minX; ix <= maxX; ix++) {
            float px = (float)ix + 0.5f;
            float t = 0.0f;
            if (abLenSq > 0.0001f) {
                t = ((px - ax) * abx + (py - ay) * aby) / abLenSq;
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
            }
            float cx = ax + abx * t, cy = ay + aby * t;
            float dx = px - cx, dy = py - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float r = r0 + (r1 - r0) * t;
            float coverage = r + 0.5f - d;
            if (coverage <= 0.0f) continue;
            if (coverage > 1.0f) coverage = 1.0f;
            uint8_t ink = (uint8_t)((1.0f - coverage) * 255.0f + 0.5f);

            int idx = iy * PANEL_W + ix;
            uint8_t cur = px_to_gray(fb[idx]);
            if (ink < cur) fb[idx] = gray_to_px(ink);
        }
    }

    if (minX < *dMinX) *dMinX = minX;
    if (minY < *dMinY) *dMinY = minY;
    if (maxX > *dMaxX) *dMaxX = maxX;
    if (maxY > *dMaxY) *dMaxY = maxY;
}

// Fills the gap between two already-accepted stroke points with a curve
// instead of the straight line draw_capsule alone would draw. p0, p1, p2 are
// three consecutive smoothed stroke points (same radii and positions the pen
// model already computed; nothing about pressure or streamlining changes
// here); the curve drawn is the classic quadratic-through-midpoints
// construction, from midpoint(p0,p1) to midpoint(p1,p2) with p1 itself as the
// control point. Consecutive calls (sharing p1==next call's p0, and so on)
// meet exactly at those midpoints, so the whole polyline comes out as one
// continuous curve rather than a chain of straight facets - which is what a
// 50-61px raw jump on a fast stroke needs, per the profiler and per
// aliceisjustplaying/tinydraw's own measurement on this same board.
//
// This only ever inserts *positions* between the real samples; it does not
// touch MIN composition, radii, or the pen model; each sub-span is still one
// more draw_capsule call.
static void draw_quad_midpoint(uint16_t *fb,
                                float p0x, float p0y, float p0r,
                                float p1x, float p1y, float p1r,
                                float p2x, float p2y, float p2r,
                                int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    float ax = (p0x + p1x) * 0.5f, ay = (p0y + p1y) * 0.5f, ar = (p0r + p1r) * 0.5f;
    float dx = (p1x + p2x) * 0.5f, dy = (p1y + p2y) * 0.5f, dr = (p1r + p2r) * 0.5f;

    // Estimate the curve's length from its control polygon (A->p1->D), an
    // upper bound that is cheap and good enough to pick a subdivision count;
    // an exact arc length needs the curve itself, which is circular.
    float arm1 = sqrtf((p1x - ax) * (p1x - ax) + (p1y - ay) * (p1y - ay));
    float arm2 = sqrtf((dx - p1x) * (dx - p1x) + (dy - p1y) * (dy - p1y));
    int steps = (int)((arm1 + arm2) / CURVE_SEG_PX + 0.5f);
    if (steps < 1) steps = 1;
    if (steps > CURVE_MAX_STEPS) steps = CURVE_MAX_STEPS;
#if PROFILE
    pf_subsegs += (uint32_t)steps;
#endif

    float prevX = ax, prevY = ay, prevR = ar;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float omt = 1.0f - t;
        // Standard quadratic Bezier position; radius is interpolated
        // linearly in t alongside it, the same way draw_capsule already
        // interpolates r0->r1 linearly across a single straight span.
        float bx = omt * omt * ax + 2.0f * omt * t * p1x + t * t * dx;
        float by = omt * omt * ay + 2.0f * omt * t * p1y + t * t * dy;
        float br = ar + (dr - ar) * t;
        draw_capsule(fb, prevX, prevY, prevR, bx, by, br, dMinX, dMinY, dMaxX, dMaxY);
        prevX = bx; prevY = by; prevR = br;
    }
}

// Row length granularity, in pixels. Bisected on hardware: a window whose row
// length is not a multiple of 8 pixels (16 bytes) comes out corrupted, and
// that is the whole rule. An earlier fix also forced a 64 pixel minimum width,
// which worked but was pure overhead: the narrow columns in the bisect were
// clean as long as their row length was a multiple of 8.
#define PUSH_GRAN_W 8
#define PUSH_MIN_W  8

// Pushes a rect under an explicit alignment and minimum width, so the two can
// be varied independently. push_dirty() is this with the settled values.
static void push_rect2(uint16_t *fb, int minX, int minY, int maxX, int maxY,
                       int alignX, int gran, int minW) {
    if (minX > maxX || minY > maxY) return;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;

    int x0 = minX & ~(alignX - 1);
    int w = maxX + 1 - x0;
    w = (w + gran - 1) & ~(gran - 1);   // round width up to `gran`
    if (w < minW) w = minW;
    int x1 = x0 + w;
    if (x1 > PANEL_W) {
        x1 = PANEL_W;
        x0 = PANEL_W - w;
        if (x0 < 0) { x0 = 0; x1 = PANEL_W; }
        x0 &= ~(alignX - 1);
    }

    int y0 = minY & ~1;
    int y1 = maxY + 1;
    if (y1 & 1) y1++;
    if (y1 > PANEL_H) y1 = PANEL_H;
    if (y1 <= y0) y1 = y0 + 2;

    AMOLED_1IN8_DisplayWindows(x0, y0, x1, y1, fb);
}

static void push_rect(uint16_t *fb, int minX, int minY, int maxX, int maxY,
                      int align, int minW) {
    if (minX > maxX || minY > maxY) return;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;

    int mask = ~(align - 1);
    int x0 = minX & mask;
    int x1 = (maxX + align) & mask;
    int y0 = minY & ~1;
    int y1 = maxY + 1;
    if (y1 & 1) y1++;

    if (x1 - x0 < minW) {
        x1 = x0 + minW;
        if (x1 > PANEL_W) { x1 = PANEL_W; x0 = (PANEL_W - minW) & mask; }
        if (x0 < 0) x0 = 0;
    }
    if (x1 > PANEL_W) x1 = PANEL_W;
    if (y1 > PANEL_H) y1 = PANEL_H;
    if (x1 <= x0) x1 = x0 + align;
    if (y1 <= y0) y1 = y0 + 2;

    AMOLED_1IN8_DisplayWindows(x0, y0, x1, y1, fb);
}

// Draws four identical vertical strokes, each pushed under a different policy,
// to separate the two changes that were made together. Left to right:
//   1  align 2, no minimum width   (what upstream effectively did)
//   2  align 8, no minimum width   (alignment alone)
//   3  align 2, minimum width 64   (width alone)
//   4  align 8, minimum width 64   (what is shipping)
// Whichever columns come out shredded name the cause. Strokes are pushed in
// small increments, the way a real stroke arrives, because pushing the whole
// stroke as one rect would not reproduce the narrow-window case at all.
static void push_bisect_test(uint16_t *fb) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;
    AMOLED_1IN8_Display(fb);
    DEV_Delay_ms(80);

    // Round two. Round one showed only "align 2, no minimum" shredding, and
    // that column differs from the clean narrow one mainly in that its widths
    // land on 10, 12, 14 rather than multiples of 8. So the question now is
    // whether the rule is about where the window starts or how long each row
    // is. Column 2 isolates it: an unaligned start with a row length rounded
    // up to 8. If it is clean, row length is the rule and the start does not
    // matter, which is also the cheaper fix.
    const int xs[4]     = { 46, 138, 230, 322 };
    const int alignX[4] = { 2,  2,  8,  8 };
    const int gran[4]   = { 2,  8,  8,  8 };
    const int minW[4]   = { 0,  0,  0, 64 };

    for (int s = 0; s < 4; s++) {
        float x = (float)xs[s];
        for (int y = 70; y < 400; y += 6) {
            int dx0 = PANEL_W, dy0 = PANEL_H, dx1 = -1, dy1 = -1;
            draw_capsule(fb, x, (float)y, 3.0f, x, (float)(y + 6), 3.0f,
                         &dx0, &dy0, &dx1, &dy1);
            push_rect2(fb, dx0, dy0, dx1, dy1, alignX[s], gran[s], minW[s]);
            DEV_Delay_ms(8);
        }
    }
    printf("bisect2: L-to-R = raw(align2,gran2), gran8only, "
           "align8+gran8, align8+gran8+min64\r\n");
}

// Pushes the accumulated dirty rect. AMOLED_1IN8_DisplayWindows takes an
// exclusive end and the panel wants even alignment, so round start down
// and exclusive end up to even, then clamp to the panel.
static void push_dirty(uint16_t *fb, int minX, int minY, int maxX, int maxY) {
    if (minX > maxX || minY > maxY) return;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;

    // Windows are aligned to 8 pixels and never narrower than PUSH_MIN_W.
    //
    // Why: a vertical stroke makes a tall, narrow dirty rect, and the driver
    // pushes one row per DMA, so a narrow window means hundreds of transfers
    // of a few dozen bytes each. Those came out shredded into horizontal ticks
    // while the horizontal parts of the very same stroke were clean, and
    // full-screen refreshes (the widest case) were always perfect. The defect
    // tracked window width, not anything in the touch or stroke code.
    //
    // Padding costs a little more data per push and buys back correctness. It
    // is deliberately two changes at once, alignment and minimum width; which
    // of the two actually matters still has to be bisected.
    // x0 is only aligned to 2, not to 8. The bisect showed an unaligned start
    // with a rounded row length is clean, so aligning the start is not part of
    // the fix and would only widen the window for nothing.
    int x0 = minX & ~1;
    int w = maxX + 1 - x0;
    w = (w + PUSH_GRAN_W - 1) & ~(PUSH_GRAN_W - 1);
    if (w < PUSH_MIN_W) w = PUSH_MIN_W;
    int x1 = x0 + w;
    if (x1 > PANEL_W) {
        // Slide the window left rather than clipping its width, since
        // shortening the row is exactly what corrupts it. The panel is 368
        // wide, a multiple of 8, so a left-slid window stays aligned.
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
 * Stroke state: one finger, one stroke at a time.
 * ------------------------------------------------------------------- */
static float g_sx, g_sy;      // current smoothed point
static float g_pressure;
static float g_arcLen;        // accumulated arc length, for the start taper
static float g_radius;        // radius at (g_sx, g_sy)
static float g_dirX, g_dirY;  // last travel direction, for the end taper

// History for draw_quad_midpoint: the smoothed point from *two* samples ago
// (the one before g_sx/g_sy's predecessor). g_haveH0 is false until a stroke
// has at least two stroke_sample() calls behind it, which is also true right
// after a bridged dropout (see stroke_sample): a curve drawn across a gap
// would bow toward a control point that predates the gap, so bridges fall
// back to a straight segment and the curve resumes on the sample after.
static float g_h0x, g_h0y, g_h0r;
static bool g_haveH0;

static float ease_out_sine(float t) {
    return sinf(t * (float)M_PI / 2.0f);
}

// Simulated pressure, tldraw's draw-tool model: speed maps to a target
// pressure (fast = light, slow = heavy), and pressure is rate-limited
// toward that target rather than following it instantly, so width change
// stays smooth even when finger speed is noisy sample to sample.
static float pressure_to_radius(float pressure) {
    float r = PEN_SIZE * ease_out_sine(0.5f - PEN_THINNING * (0.5f - pressure));
    if (r < 1.0f) r = 1.0f;
    if (r > 8.0f) r = 8.0f;
    return r;
}

static void stroke_begin(uint16_t *fb, int x, int y,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    g_sx = (float)x;
    g_sy = (float)y;
    g_pressure = 0.5f;
    g_arcLen = 0.0f;
    g_dirX = 0.0f;
    g_dirY = 0.0f;
    g_radius = pressure_to_radius(g_pressure) * 0.35f; // start-taper factor at arc==0
    g_haveH0 = false; // not enough history yet for a curve
    draw_capsule(fb, g_sx, g_sy, g_radius, g_sx, g_sy, g_radius, dMinX, dMinY, dMaxX, dMaxY);
}

// `bridge` marks the first sample after the controller lost and regained
// contact. Such a sample is handled differently in two ways: it snaps straight
// to the reported position instead of being smoothed toward it (smoothing
// across a large gap would leave the ink trailing well behind the finger and
// put a kink in the line), and it leaves pressure alone, so the width carries
// continuously across the gap instead of thinning as if the finger had
// suddenly accelerated. The result is one straight segment filling the gap.
static void stroke_sample(uint16_t *fb, int x, int y, bool bridge,
                           int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    float prevX = g_sx, prevY = g_sy, prevR = g_radius;

    float k = bridge ? 1.0f : (1.0f - STREAMLINE);
    float nx = g_sx + ((float)x - g_sx) * k;
    float ny = g_sy + ((float)y - g_sy) * k;
    float dist = sqrtf((nx - prevX) * (nx - prevX) + (ny - prevY) * (ny - prevY));
    if (dist < DEDUPE_PX) return; // finger resting: drop the jitter, keep old state

    g_sx = nx;
    g_sy = ny;

    if (!bridge) {
        float target = 1.0f - fminf(1.0f, dist / SPEED_MAX);
        g_pressure += (target - g_pressure) * PRESSURE_LERP;
    }
    float r = pressure_to_radius(g_pressure);

    g_arcLen += dist;
    if (g_arcLen < START_TAPER_LEN) {
        r *= (0.35f + 0.65f * (g_arcLen / START_TAPER_LEN));
    }

    if (bridge) {
        // A curve here would bow toward whatever was drawn before the
        // dropout, which is stale by definition; fill the reconnection
        // straight, same as before curve-fitting existed.
        draw_capsule(fb, prevX, prevY, prevR, g_sx, g_sy, r, dMinX, dMinY, dMaxX, dMaxY);
        g_haveH0 = false; // don't curve the *next* segment across this gap either
    } else if (g_haveH0) {
        draw_quad_midpoint(fb, g_h0x, g_h0y, g_h0r, prevX, prevY, prevR, g_sx, g_sy, r,
                            dMinX, dMinY, dMaxX, dMaxY);
    } else {
        // First real sample of the stroke (or the one right after a bridge):
        // not enough history for a curve yet, so draw the plain straight
        // span, same as the pre-curve code always did.
        draw_capsule(fb, prevX, prevY, prevR, g_sx, g_sy, r, dMinX, dMinY, dMaxX, dMaxY);
    }
    g_h0x = prevX; g_h0y = prevY; g_h0r = prevR;
    g_haveH0 = true;

    g_radius = r;
    g_dirX = (nx - prevX) / dist;
    g_dirY = (ny - prevY) / dist;
}

static void stroke_end(uint16_t *fb, int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (g_haveH0) {
        // By construction, the last curve segment drawn in stroke_sample
        // stopped at midpoint(g_h0, current) rather than at the current
        // point itself - that's what lets consecutive curve segments meet
        // smoothly. Draw the remaining half straight, or the stroke visibly
        // falls short of where the finger actually lifted.
        float mx = (g_h0x + g_sx) * 0.5f, my = (g_h0y + g_sy) * 0.5f;
        float mr = (g_h0r + g_radius) * 0.5f;
        draw_capsule(fb, mx, my, mr, g_sx, g_sy, g_radius, dMinX, dMinY, dMaxX, dMaxY);
    }

    static const float scales[3] = {0.7f, 0.45f, 0.25f};
    float curX = g_sx, curY = g_sy, curR = g_radius;
    for (int i = 0; i < 3; i++) {
        float nx = curX + g_dirX * 1.2f;
        float ny = curY + g_dirY * 1.2f;
        float nr = g_radius * scales[i];
        draw_capsule(fb, curX, curY, curR, nx, ny, nr, dMinX, dMinY, dMaxX, dMaxY);
        curX = nx; curY = ny; curR = nr;
    }
}

/* ---------------------------------------------------------------------
 * Shake-to-erase.
 *
 * QMI8658 gives acceleration in mg; at rest |acc| ~= 1000 (1 g). A single
 * sample far from that is a "jolt" but is indistinguishable from a bump or a
 * firm tap, so we require several jolts inside a short rolling window before
 * treating it as an intentional shake, and then enforce a cooldown so the
 * same shake cannot be counted twice.
 *
 * This used to poll the IMU itself from the main loop. It now runs on core1
 * (imu_poll_core1(), below core1_entry()), since the IMU shares i2c1 with
 * touch and the PMIC; only wipe_erase() (framebuffer + QSPI, no I2C) stays
 * here on core0.
 * ------------------------------------------------------------------- */
static void wipe_erase(uint16_t *fb) {
    const int bands = 16;
    const int bandH = PANEL_H / bands;
    for (int b = 0; b < bands; b++) {
        int y0 = b * bandH;
        int y1 = (b == bands - 1) ? PANEL_H : y0 + bandH;
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < PANEL_W; x++)
                fb[y * PANEL_W + x] = 0xFFFF;
        AMOLED_1IN8_DisplayWindows(0, y0, PANEL_W, y1, fb);
        DEV_Delay_ms(15);
    }
}

// Shown once at boot and cleared on first touch. Two questions it answers:
// whether the panel renders our greys as smooth neutral grey at all (the ramp),
// and how the anti-aliasing actually looks at each stroke weight (the three
// diagonals, drawn with the same rasteriser the pen uses). If the ramp bands or
// tints, the problem is the pixel format. If the ramp is clean but the
// diagonals look chunky, the problem is geometry, not colour.
static void draw_test_pattern(uint16_t *fb) {
    int dx0 = PANEL_W, dy0 = PANEL_H, dx1 = -1, dy1 = -1;

    for (int y = 40; y < 110; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            uint8_t g = (uint8_t)((x * 255) / (PANEL_W - 1));
            fb[y * PANEL_W + x] = gray_to_px(g);
        }
    }

    // Same call the pen makes, at the three radii the pressure model spans.
    draw_capsule(fb, 40.0f, 170.0f, 1.0f, 320.0f, 215.0f, 1.0f, &dx0, &dy0, &dx1, &dy1);
    draw_capsule(fb, 40.0f, 250.0f, 3.0f, 320.0f, 295.0f, 3.0f, &dx0, &dy0, &dx1, &dy1);
    draw_capsule(fb, 40.0f, 330.0f, 6.0f, 320.0f, 375.0f, 6.0f, &dx0, &dy0, &dx1, &dy1);

    // A tapered one, to show the width ramp the pen produces.
    draw_capsule(fb, 40.0f, 410.0f, 1.0f, 320.0f, 430.0f, 7.0f, &dx0, &dy0, &dx1, &dy1);
}

/* ---------------------------------------------------------------------
 * Touch power state.
 *
 * FT3168_Init writes 0x01 to REG_POWER_MODE (0xA5), which is MONITOR in the
 * driver's own Device_Mode enum, not ACTIVE. Monitor is the low-power scan
 * state: the controller samples at a reduced rate and is meant to promote
 * itself to active when it sees a finger. If that promotion does not happen,
 * the chip stays alive on the bus, keeps answering the WhoAmI, and reports
 * zero fingers forever, which is exactly the failure seen here. We put it in
 * ACTIVE explicitly after init and after every recovery.
 * ------------------------------------------------------------------- */
// FT3168 register 0x88 is the active-mode scan period in milliseconds. The
// datasheet gives 60fps as the default and says the part supports up to 100Hz,
// which is worth taking: the report rate is the hard floor on input latency,
// and it also shortens every dropout gap the stroke code has to bridge.
// Written after the mode is set, and the effect is checked by the profiler's
// reports-per-second rather than assumed.
#define FT3168_REG_PERIOD_ACTIVE 0x88
#define FT3168_PERIOD_MS         10

// Runs once, on core0, before multicore_launch_core1() - i.e. before any
// contention over i2c1 can exist. Never call this after core1 has started;
// touch_set_active_to() (below core1_entry()) is the timeout-guarded
// equivalent for use once core1 owns the bus.
static void touch_set_active(void) {
    // Register 0x00 is DEVICE_MODE. 0 is normal working mode; the chip will not
    // report points in factory/test mode, and nothing in the vendor init ever
    // sets it, so it is whatever the last state left behind.
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, 0x00, 0x00);
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, REG_POWER_MODE, FT3168_POWER_ACTIVE);
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE, FT3168_PERIOD_MS);
}

/* ---------------------------------------------------------------------
 * Button hunt.
 *
 * Neither side button produces anything on SYS_OUT (GPIO18), across two
 * separate sessions of deliberate presses, and the board does not reboot
 * either, so the vendor's "PWR KEY on GPIO18" reading does not hold on this
 * hardware. Rather than guess again, watch every pin we do not already use
 * and ask the PMIC directly.
 *
 * Pins 4-15, 17 and 18 are the panel, touch, I2C and IMU. Pins 20-24 are the
 * audio I2S lines in the vendor's ES8311 demo. Everything else is fair game;
 * they are pulled up so a button to ground reads as a clean falling edge
 * rather than as floating noise.
 *
 * The AXP2101 latches power-key events in its interrupt status registers
 * whether or not anything is wired to the MCU, which is the most likely place
 * a press actually shows up on a board where the PMIC owns the button.
 * ------------------------------------------------------------------- */
#define AXP2101_ADDR 0x34

// The pin sweep and the periodic snapshot were how the button was found. They
// are off now that it is known (PWR is the lower of the two side buttons,
// wired to the PMIC's PWRON pin, seen here only as an AXP2101 interrupt on
// GPIO2). Power-key events themselves stay on; they are a feature now, and
// live in pmic_poll_core1() below, since register 0x49 is on i2c1.
#define DEBUG_PIN_SWEEP 0

static const uint8_t probePins[] = { 0, 1, 2, 3, 16, 19, 25, 26, 27, 28, 29 };
static uint8_t probePrev[sizeof(probePins)];
static uint8_t probeCand[sizeof(probePins)];
static uint32_t probeSinceMs[sizeof(probePins)];

static void buttons_init(void) {
    for (size_t i = 0; i < sizeof(probePins); i++) {
        gpio_init(probePins[i]);
        gpio_set_dir(probePins[i], GPIO_IN);
        gpio_pull_up(probePins[i]);
    }
    sleep_ms(2);
    for (size_t i = 0; i < sizeof(probePins); i++) {
        probePrev[i] = (uint8_t)gpio_get(probePins[i]);
        probeCand[i] = probePrev[i];
    }

    // Only the middle interrupt register is enabled. That is where the power
    // key events land; enabling all three also produced battery and charger
    // events, which are noise for this purpose. This is one-time setup and
    // runs on core0 before multicore_launch_core1(), so touching i2c1 here is
    // still safe.
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x40, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x41, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x42, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x48, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x4A, 0xFF);

    printf("button hunt armed: watching %u pins + AXP2101 irq\r\n",
           (unsigned)sizeof(probePins));
}

// Set by pmic_poll_core1() (core1), consumed by the main loop (core0), which
// is where the framebuffer lives. Bits are REG 0x49's: 3 short press, 2 long
// press. A plain volatile byte is enough synchronisation for this: it is
// only ever read-and-cleared once per outer loop on core0, a stale or
// slightly-late read costs at most one loop iteration of a button gesture
// that is measured in hundreds of milliseconds, and there is no queue of
// events to lose since the PMIC itself already debounces press/release in
// hardware.
static volatile uint8_t g_keyEvent;

// Flashes a filled square in the top-left corner without disturbing the
// drawing: the covered pixels are saved, overwritten, pushed, then put back.
// This exists so the button can be identified by pressing it and watching the
// screen, rather than by correlating a serial log against a stopwatch.
#define FLASH_MAX 128
static uint16_t flashSave[FLASH_MAX * FLASH_MAX];

static void flash_marker(uint16_t *fb, int size, int holdMs) {
    if (size > FLASH_MAX) size = FLASH_MAX;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            flashSave[y * size + x] = fb[y * PANEL_W + x];
            fb[y * PANEL_W + x] = 0x0000;
        }
    }
    AMOLED_1IN8_DisplayWindows(0, 0, size, size, fb);
    DEV_Delay_ms(holdMs);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fb[y * PANEL_W + x] = flashSave[y * size + x];
        }
    }
    AMOLED_1IN8_DisplayWindows(0, 0, size, size, fb);
}

// GPIO-only half of the old buttons_poll(): the PMIC I2C poll that used to
// live here moved to pmic_poll_core1(), since register reads against
// AXP2101_ADDR are i2c1 traffic and core0 must not touch i2c1 once core1 has
// started (see core1_entry()). What is left compiles to nothing unless
// DEBUG_PIN_SWEEP is flipped on, same as before; the periodic AXP register
// snapshot that used to accompany the pin scan is gone; the pin edges alone
// are what actually found the button (see README.md's bring-up log).
static void buttons_poll_gpio(void) {
#if DEBUG_PIN_SWEEP
    uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    // Report a pin only once its new level has held for 30ms. GPIO16 turned
    // out to toggle continuously at roughly the panel refresh rate, which is a
    // real periodic signal (the display tearing-effect line, most likely) and
    // not a button; without debouncing it buries everything else. No human
    // press is shorter than 30ms, so nothing real is lost.
    for (size_t i = 0; i < sizeof(probePins); i++) {
        uint8_t v = (uint8_t)gpio_get(probePins[i]);
        if (v != probeCand[i]) {
            probeCand[i] = v;
            probeSinceMs[i] = nowMs;
        } else if (v != probePrev[i] && nowMs - probeSinceMs[i] >= 30) {
            probePrev[i] = v;
            printf("GPIO%u -> %u t=%lu\r\n", (unsigned)probePins[i],
                   (unsigned)v, (unsigned long)nowMs);
        }
    }
#endif
}

// Scans the I2C bus by address. RETIRED from the hot path along with
// touch_selftest() below: both hit i2c1 directly and core0 must not once
// core1 owns the bus. Kept for reference; would need to run before
// multicore_launch_core1() (like buttons_init()) to be safe again.
static void i2c_scan(void) {
    printf("i2c scan:");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t b;
        int r = i2c_read_blocking(I2C_PORT, a, &b, 1, false);
        if (r >= 0) {
            const char *who = (a == 0x38) ? "FT3168" :
                              (a == 0x6B) ? "QMI8658" :
                              (a == 0x34) ? "AXP2101" :
                              (a == 0x51) ? "PCF85063" :
                              (a == 0x18) ? "ES8311" : "?";
            printf(" 0x%02x(%s)", a, who);
            found++;
        }
    }
    printf(" | %d device(s)\r\n", found);
}

// Proves the two things the register prints cannot distinguish on their own:
// that a write actually reaches the chip (rather than being NACKed and lost),
// and what the interrupt line is doing while we poll.
// Reads one register with a STOP between the pointer write and the data read,
// instead of the repeated START the vendor helper uses. Some FocalTech parts
// do not accept a repeated start and answer the *previous* pointer instead,
// which shows up as every read being one transaction stale.
static uint8_t ft_read_stop(uint8_t reg) {
    uint8_t v = 0;
    i2c_write_blocking(I2C_PORT, FT3168_I2C_ADDR, &reg, 1, false);
    i2c_read_blocking(I2C_PORT, FT3168_I2C_ADDR, &v, 1, false);
    return v;
}

static uint8_t ft_read_restart(uint8_t reg) {
    uint8_t v = 0;
    i2c_write_blocking(I2C_PORT, FT3168_I2C_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, FT3168_I2C_ADDR, &v, 1, false);
    return v;
}

static void ft_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    i2c_write_blocking(I2C_PORT, FT3168_I2C_ADDR, b, 2, false);
}

static void touch_selftest(void) {
    i2c_scan();

    // Write a distinctive value, then read it back three ways. If only the
    // STOP-separated read returns it, the vendor helper's repeated start is
    // the bug and every register read in the driver is suspect.
    ft_write(REG_POWER_MODE, FT3168_POWER_STANDBY);
    sleep_ms(5);
    uint8_t viaRestart = ft_read_restart(REG_POWER_MODE);
    uint8_t viaStop    = ft_read_stop(REG_POWER_MODE);
    uint8_t viaStop2   = ft_read_stop(REG_POWER_MODE);
    printf("selftest wrote 0x02 -> restart=0x%02x stop=0x%02x stop-again=0x%02x\r\n",
           viaRestart, viaStop, viaStop2);
    ft_write(REG_POWER_MODE, FT3168_POWER_ACTIVE);

    // Alternating two registers exposes lag directly: with a correct read each
    // column is stable, with a one-transaction lag they swap.
    uint8_t a1 = ft_read_restart(FT3168_RD_DEVICE_ID);
    uint8_t b1 = ft_read_restart(REG_FINGER_NUM);
    uint8_t a2 = ft_read_restart(FT3168_RD_DEVICE_ID);
    uint8_t b2 = ft_read_restart(REG_FINGER_NUM);
    printf("selftest alternate restart id/num: %02x %02x %02x %02x\r\n", a1, b1, a2, b2);
    uint8_t c1 = ft_read_stop(FT3168_RD_DEVICE_ID);
    uint8_t d1 = ft_read_stop(REG_FINGER_NUM);
    uint8_t c2 = ft_read_stop(FT3168_RD_DEVICE_ID);
    uint8_t d2 = ft_read_stop(REG_FINGER_NUM);
    printf("selftest alternate stop    id/num: %02x %02x %02x %02x\r\n", c1, d1, c2, d2);

    printf("selftest regs 00..07 stop:");
    for (int r = 0; r < 8; r++) printf(" %02x", ft_read_stop((uint8_t)r));
    printf(" | int=%d\r\n", gpio_get(Touch_INT_PIN));
}

/* =========================================================================
 * Core 1: the sole owner of i2c1.
 *
 * i2c1 is shared by the touch controller, the IMU and the PMIC (AGENTS.md).
 * From the moment multicore_launch_core1() runs (see main(), at the bottom
 * of this file) core1 is the ONLY code in this firmware allowed to touch
 * i2c1: no DEV_I2C_*, no i2c_*_blocking or i2c_*_timeout_us against
 * I2C_PORT, and no call into the FT3168 or QMI8658 drivers (they call
 * DEV_I2C_* internally) from core0. Everything core0 still does with a GPIO
 * - SYS_OUT, Touch_INT_PIN, the boot-select pad, the probe pins - is fine
 * from either core; GPIO input registers are just memory, reading them from
 * both cores at once is not a race. I2C transactions are not: two cores
 * issuing them concurrently could interleave on the wire.
 *
 * Every read or write in this section uses the SDK's *_timeout_us calls,
 * never the plain _blocking ones. The plain ones have no timeout at all, so
 * a wedged bus would hang core1 forever with nothing core0 could do about
 * it short of a physical power cycle - which is exactly the failure mode
 * this board has already produced more than once today for other reasons.
 * A timeout failure here is counted, not retried in a loop.
 * ========================================================================= */

// 2ms against a transaction that normally costs a few hundred microseconds
// (695us measured for the worst case, touch's finger-count-then-XY-burst
// pair) gives comfortable margin without turning a single real stall into a
// multi-second one.
#define I2C_TIMEOUT_US 2000

static inline bool i2c1_write_to(uint8_t addr, const uint8_t *data, size_t len) {
    return i2c_write_timeout_us(I2C_PORT, addr, data, len, false, I2C_TIMEOUT_US) == (int)len;
}

static inline bool i2c1_write_reg_to(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c1_write_to(addr, b, 2);
}

// Register-pointer write (repeated START, no STOP) followed by a bounded
// read, same transaction shape DEV_I2C_Read_nByte uses, just with a timeout
// on both halves.
static inline bool i2c1_read_reg_n_to(uint8_t addr, uint8_t reg, uint8_t *out, size_t len) {
    if (i2c_write_timeout_us(I2C_PORT, addr, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, addr, out, len, false, I2C_TIMEOUT_US) == (int)len;
}

/* ---- touch: read helpers, gated on Touch_INT_PIN ---------------------- */

static inline bool touch_read_fingers_to(uint8_t *out) {
    uint8_t v = 0;
    if (!i2c1_read_reg_n_to(FT3168_I2C_ADDR, REG_FINGER_NUM, &v, 1)) return false;
    *out = v;
    return true;
}

// One burst for both axes, same as the single-core version used to do: the
// vendor path reads X (0x03,0x04) and Y (0x05,0x06) as two separate
// transactions, so a report landing between them yields a point built from
// the new X and the old Y - a coordinate the finger never visited, off the
// line at right angles. Auto-increment across these four registers is
// already relied on by the vendor's own 2-byte reads, so this is the same
// access widened, not a new assumption.
static inline bool touch_read_xy_to(uint16_t *x, uint16_t *y) {
    uint8_t b[4];
    if (!i2c1_read_reg_n_to(FT3168_I2C_ADDR, REG_X1_H, b, 4)) return false;
    *x = ((uint16_t)(b[0] & 0x0F) << 8) | b[1];
    *y = ((uint16_t)(b[2] & 0x0F) << 8) | b[3];
    return true;
}

// Timeout-guarded equivalent of touch_set_active() (above), for use after
// multicore_launch_core1(). Never call the plain touch_set_active() past
// that point.
static bool touch_set_active_to(void) {
    bool ok = true;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, 0x00, 0x00) && ok;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, REG_POWER_MODE, (uint8_t)FT3168_POWER_ACTIVE) && ok;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE, FT3168_PERIOD_MS) && ok;
    return ok;
}

/* ---- the touch queue: core1 (producer) -> core0 (consumer) ------------
 *
 * A single-producer/single-reader ring, power-of-two sized so the index
 * wrap is a mask instead of a modulo. head is only ever written by core0,
 * tail only ever written by core1; each core only *reads* the other's
 * index. That split, plus a memory barrier around the point where the new
 * element becomes visible, is what makes this safe without a mutex or a
 * spinlock:
 *   - a spinlock held across an I2C transaction is exactly what the vendor
 *     demo's own touch/IMU locking bug looked like (see AGENTS.md's "Do not
 *     move either onto an interrupt" note) - so this deliberately never
 *     takes a lock around anything that can block;
 *   - the producer writes the sample into touchQ[tail] *then* publishes the
 *     new tail with __dmb() in between, so a consumer that observes the new
 *     tail is guaranteed to observe a fully-written sample, not a torn one;
 *   - the consumer's __dmb() before copying out the sample is the mirror of
 *     that: it stops the compiler or core from reading the sample data
 *     ahead of reading the tail index that guards it.
 * The queue can never appear to have more than one writer or reader because
 * only core1 calls touch_q_push() and only core0 calls touch_q_pop().
 */
#define TOUCH_Q_CAP 64
_Static_assert((TOUCH_Q_CAP & (TOUCH_Q_CAP - 1)) == 0, "TOUCH_Q_CAP must be a power of two");

typedef struct {
    uint32_t tMs;    // to_ms_since_boot() at the moment core1 took this reading
    uint8_t  fingers;
    uint16_t x, y;   // raw (unclamped) touch registers; core0 clamps on dequeue
} touch_sample_t;

static volatile touch_sample_t g_touchQ[TOUCH_Q_CAP];
static volatile uint32_t g_touchHead = 0; // core0-owned (consumer index)
static volatile uint32_t g_touchTail = 0; // core1-owned (producer index)

static inline bool touch_q_push(const touch_sample_t *s) {
    uint32_t tail = g_touchTail;
    uint32_t next = (tail + 1) & (TOUCH_Q_CAP - 1);
    if (next == g_touchHead) return false; // full: core0 is behind, drop and count it
    g_touchQ[tail] = *s;
    __dmb();
    g_touchTail = next;
    return true;
}

static inline bool touch_q_pop(touch_sample_t *out) {
    uint32_t head = g_touchHead;
    if (head == g_touchTail) return false; // empty
    __dmb();
    *out = g_touchQ[head];
    g_touchHead = (head + 1) & (TOUCH_Q_CAP - 1);
    return true;
}

// How often to push a synthetic "no finger" sample while Touch_INT_PIN is
// high, so core0's lift-debounce and stall-watchdog timing (both wall-clock
// based, see LIFT_DEBOUNCE_MS / TOUCH_STALL_MS) keep getting fresh
// timestamps without core1 spending any I2C time to produce them. Well
// under LIFT_DEBOUNCE_MS (80ms), so a real lift is still noticed promptly;
// the transition itself is also pushed immediately (see core1_entry), so
// this interval only governs the steady idle state, not the edge.
#define TOUCH_IDLE_HEARTBEAT_MS 5

/* ---- cross-core published state, other than the touch queue -----------
 *
 * All of these are single, plain-old-data words: each is written by
 * exactly one core and only ever read by the other, so an aligned 32-bit
 * (or bool-sized) load/store is already atomic on Cortex-M33 and no barrier
 * is needed for correctness - the worst case is the reader seeing last
 * loop's value for one more iteration, which for a button gesture, a shake
 * flag or a diagnostic counter is nowhere near visible.
 */
static volatile bool g_fingerDownShared; // core0 -> core1: suppress shake while drawing
static volatile uint32_t g_eraseSeq;     // core1 -> core0: bumped once per accepted shake

// Diagnostics, core1-increment-only, core0-read-only (via the profiler).
static volatile uint32_t g_touchReads;
static volatile uint32_t g_touchTimeouts;
static volatile uint32_t g_touchQueueDrops;
static volatile uint32_t g_touchRecoveries;
static volatile uint32_t g_imuTimeouts;
static volatile uint32_t g_pmicTimeouts;

/* ---- touch stall recovery, core1 side ---------------------------------- */

static void touch_recover_core1(void) {
    FT3168_Reset();          // RST pin toggle + sleep_ms only, no I2C: safe as-is.
    if (!touch_set_active_to()) g_touchTimeouts++;
    g_touchRecoveries++;
}

/* ---- IMU: shake-to-erase, core1 side ------------------------------------
 *
 * QMI8658_init() (called once on core0, before multicore_launch_core1())
 * leaves the part at QMI8658AccRange_8g, which is what fixes the raw-to-mg
 * scale factor below: acc_lsb_div = 1<<12 = 4096 for that range (see
 * QMI8658.c's QMI8658_config_acc()). That divisor is a private static in
 * the vendor driver with no getter, so it is re-derived here rather than
 * read back; if QMI8658_init()'s accRange is ever changed, this constant
 * has to change with it.
 */
#define QMI8658_I2C_ADDR    0x6B  // confirmed on this board by i2c_scan(); QMI8658_init()
                                  // itself probes 0x6A then 0x6B and only 0x6B acks here.
#define QMI8658_ACC_LSB_DIV 4096.0f

static void imu_poll_core1(uint32_t nowMs) {
    static uint32_t lastMs = 0;
    static uint32_t joltTimes[JOLT_MAX];
    static int joltCount = 0;
    static uint32_t cooldownUntilMs = 0;

    if (nowMs - lastMs < IMU_POLL_MS) return;
    lastMs = nowMs;

    uint8_t buf[6];
    if (!i2c1_read_reg_n_to(QMI8658_I2C_ADDR, QMI8658Register_Ax_L, buf, 6)) {
        g_imuTimeouts++;
        return;
    }
    short rawX = (short)((uint16_t)(buf[1] << 8) | buf[0]);
    short rawY = (short)((uint16_t)(buf[3] << 8) | buf[2]);
    short rawZ = (short)((uint16_t)(buf[5] << 8) | buf[4]);
    float ax = ((float)rawX * 1000.0f) / QMI8658_ACC_LSB_DIV;
    float ay = ((float)rawY * 1000.0f) / QMI8658_ACC_LSB_DIV;
    float az = ((float)rawZ * 1000.0f) / QMI8658_ACC_LSB_DIV;

    float mag = sqrtf(ax * ax + ay * ay + az * az);
    float dev = fabsf(mag - 1000.0f);
    if (dev > JOLT_DEV_MG) {
        int w = 0;
        for (int i = 0; i < joltCount; i++) {
            if (nowMs - joltTimes[i] <= JOLT_WINDOW_MS) joltTimes[w++] = joltTimes[i];
        }
        joltCount = w;
        if (joltCount < JOLT_MAX) joltTimes[joltCount++] = nowMs;
    }

    if (joltCount >= JOLT_MIN_COUNT && nowMs >= cooldownUntilMs && !g_fingerDownShared) {
        cooldownUntilMs = nowMs + ERASE_COOLDOWN_MS;
        joltCount = 0;
        g_eraseSeq++;
    }
}

/* ---- PMIC: power key events, core1 side --------------------------------
 *
 * Same three status registers the old buttons_poll() read on core0, just
 * timeout-guarded and printf-free (core1 must not printf; see the section
 * banner above). The verbose "KEY raw=.. down/up/SHORT/LONG" trace that used
 * to print here is gone with it - g_keyEvent and the short/long-press
 * actions in main() are unaffected, only the raw diagnostic line is lost.
 * Deliberately NOT batched into one 3-byte burst read from 0x48, even though
 * the registers are consecutive: unlike the touch and IMU bursts above, that
 * pattern is not something the existing, hardware-proven code already does
 * for this chip, and there is no hardware here tonight to check it against.
 */
static void pmic_poll_core1(uint32_t nowMs) {
    static uint32_t lastMs = 0;
    if (nowMs - lastMs < 40) return;
    lastMs = nowMs;

    uint8_t s0 = 0, s1 = 0, s2 = 0;
    bool ok = i2c1_read_reg_n_to(AXP2101_ADDR, 0x48, &s0, 1) &&
              i2c1_read_reg_n_to(AXP2101_ADDR, 0x49, &s1, 1) &&
              i2c1_read_reg_n_to(AXP2101_ADDR, 0x4A, &s2, 1);
    if (!ok) {
        g_pmicTimeouts++;
        return;
    }
    if (s0 || s1 || s2) {
        // Bit names are from the AXP2101 datasheet, REG 49H (IRQ Status 1):
        // bit0 POWERON positive edge, bit1 negative edge, bit2 long press,
        // bit3 short press.
        //
        // Long press (0x04) and short-press verdict (0x08) drive this app's
        // own actions, same as always. The press edge (0x02) is newly
        // captured too: the menu's long-double-press-to-open gesture (see
        // main()'s event handler) needs to time this press against the
        // previous one, which only the edge - not the eventual verdict -
        // can tell it. main()'s own handler still only acts on 0x04 (chord
        // and menu-open) and 0x08 (full refresh), so this does not change
        // this app's own behaviour by itself.
        if (s1 & 0x0E) g_keyEvent = s1 & 0x0E;
        // Write-1-to-clear, so the next event is distinguishable from this one.
        i2c1_write_reg_to(AXP2101_ADDR, 0x48, s0);
        i2c1_write_reg_to(AXP2101_ADDR, 0x49, s1);
        i2c1_write_reg_to(AXP2101_ADDR, 0x4A, s2);
    }
}

/* ---- core1 entry point --------------------------------------------------
 *
 * Touch is gated on Touch_INT_PIN (active low, already configured as an
 * input with a pull-up in main() before this core starts): skip the I2C
 * read while the line is high, which is most of the time when no finger is
 * down, and that is where the 695us-per-loop cost measured on this board
 * (98% of frame time) actually went. aliceisjustplaying/tinydraw's own
 * writeup on this same board warns that reading *only* on the falling
 * edge - i.e. one read, then wait for the next edge - was jittery and had
 * to be reverted; this does not do that. The gate is on the *level*, so
 * every pass through this loop re-checks it: while the line is low the read
 * repeats continuously, same as the old single-core hot loop did, and it
 * only stops once the line is actually back high.
 */
static void core1_entry(void) {
    uint32_t lastFingerMs = to_ms_since_boot(get_absolute_time());
    uint32_t lastIdleHeartbeatMs = 0;
    bool wasTouchLow = false;

    while (true) {
        uint32_t nowMs = to_ms_since_boot(get_absolute_time());

        int intLow = (gpio_get(Touch_INT_PIN) == 0);
        if (intLow) {
            uint8_t fingers = 0;
            bool ok = touch_read_fingers_to(&fingers);
            g_touchReads++;
            uint16_t x = 0, y = 0;
            if (ok && fingers != 0) {
                ok = touch_read_xy_to(&x, &y);
                g_touchReads++;
            }
            if (!ok) {
                g_touchTimeouts++;
            } else {
                touch_sample_t s = { nowMs, fingers, x, y };
                if (!touch_q_push(&s)) g_touchQueueDrops++;
                if (fingers != 0) lastFingerMs = nowMs;
            }
            wasTouchLow = true;
        } else {
            // Push immediately on the low->high edge (a lift should be seen
            // right away), then keep a light heartbeat afterward so core0's
            // wall-clock timers (lift debounce, stall watchdog) keep getting
            // fresh "still no finger" timestamps without any I2C cost.
            bool edge = wasTouchLow;
            wasTouchLow = false;
            if (edge || nowMs - lastIdleHeartbeatMs >= TOUCH_IDLE_HEARTBEAT_MS) {
                lastIdleHeartbeatMs = nowMs;
                touch_sample_t s = { nowMs, 0, 0, 0 };
                if (!touch_q_push(&s)) g_touchQueueDrops++;
            }
        }

        if (nowMs - lastFingerMs >= TOUCH_STALL_MS) {
            lastFingerMs = nowMs;
            touch_recover_core1();
        }

        imu_poll_core1(nowMs);
        pmic_poll_core1(nowMs);
    }
}

// Callback for menu_run() (see menu.h). Never touches i2c1: core1's
// pmic_poll_core1() keeps servicing the PMIC on its own the entire time
// core0 is blocked inside menu_run() (it does not know or care what core0 is
// doing), so this only ever needs to drain the cross-core g_keyEvent byte,
// exactly like this file's own handler in main() does. Touching i2c1 from
// here (core0) would violate the ownership rule in core1_entry()'s banner
// comment above - this function is the reason that rule is safe to keep:
// nothing in menu.c ever needs to.
static uint8_t sketchpad_menu_poll_pwr(void) {
    uint8_t ev = g_keyEvent;
    g_keyEvent = 0;
    return ev;
}

int main(void) {
    DEV_Module_Init();
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    AMOLED_1IN8_Init();
    AMOLED_1IN8_SetBrightness(180);
    QMI8658_init();
    FT3168_Init(FT3168_Point_Mode);
    // The vendor never configures the interrupt line; we only read it, but it
    // has to be an input with a pull-up or the level printed is meaningless.
    gpio_init(Touch_INT_PIN);
    gpio_set_dir(Touch_INT_PIN, GPIO_IN);
    gpio_pull_up(Touch_INT_PIN);
    touch_set_active();
    buttons_init();
    // Everything above touches i2c1 (QMI8658_init, FT3168_Init,
    // touch_set_active, buttons_init) and runs single-threaded on core0, so
    // there is no ownership question yet. That changes the moment core1
    // launches, below: from here on only core1 may touch i2c1 (see the
    // banner comment above core1_entry()).

    uint16_t *fb = (uint16_t *)malloc((size_t)PANEL_W * PANEL_H * 2);
    if (fb == NULL) {
        printf("framebuffer allocation failed\r\n");
        while (true) { }
    }
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;
    // The boot test pattern (grey ramp plus four capsules at the pen's radii)
    // did its job: it proved the panel renders neutral greys correctly and put
    // the corruption on the partial-refresh path rather than the pixel format.
    // draw_test_pattern() is kept for the next time the rasteriser is touched.
    AMOLED_1IN8_Display(fb);
    // Read the scan period back rather than assuming the write took. If this
    // does not read as FT3168_PERIOD_MS the controller is still at its 60Hz
    // default and the reports-per-second figure below is the proof. This is
    // still on core0, still before multicore_launch_core1(), so still safe.
    printf("sketchpad ready (touch period reg=%u ms)\r\n",
           DEV_I2C_Read_Byte(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE));

    multicore_launch_core1(core1_entry);

    bool fingerDown = false;
    bool patternShown = false;
    int lastRawX = 0, lastRawY = 0;
    uint32_t glitches = 0;
    uint32_t lastSampleMs = 0;
    uint32_t dropouts = 0;
    bool bridging = false;
    bool pendingStart = false;
    int pendX = 0, pendY = 0;
    bool haveCand = false;
    int candX = 0, candY = 0;
    int lastReportX = -1, lastReportY = -1;
    uint32_t strays = 0;
    uint32_t splits = 0;
    uint32_t lastEraseSeqSeen = 0;

    // Long-double-press-opens-the-menu state (see MENU_DOUBLE_WINDOW_MS and
    // sketchpad_menu_poll_pwr() above). Recomputed on every 0x02 press edge,
    // not set-and-forget, so it always answers "was the press right before
    // this one within the window" - see chrono's identical pattern in
    // apps/chrono/main.c for the fuller reasoning.
    uint32_t lastPwrPressMs = 0;
    bool menuDoubleArmed = false;

    // Snapshots for turning core1's monotonic counters into per-second
    // deltas in the profiler print, below.
    uint32_t lastTouchReads = 0, lastTouchTimeouts = 0, lastTouchDrops = 0;
    uint32_t lastTouchRecoveries = 0, lastImuTimeouts = 0, lastPmicTimeouts = 0;

    while (true) {
        int dMinX = PANEL_W, dMinY = PANEL_H, dMaxX = -1, dMaxY = -1;

        // PWR button, reported by the AXP2101 on SYS_OUT rather than wired to
        // the MCU directly. GPIO-only (no I2C), so still fine on core0.
        {
            static int prevPwr = 0;
            static uint32_t pwrDownMs = 0;
            static uint32_t pwrTickMs = 0;
            int lvl = gpio_get(SYS_OUT);
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (lvl != prevPwr) {
                prevPwr = lvl;
                if (lvl) {
                    pwrDownMs = nowMs;
                    pwrTickMs = nowMs;
                    printf("BTN pwr down t=%lu\r\n", (unsigned long)nowMs);
                } else {
                    printf("BTN pwr up t=%lu held=%lums\r\n",
                           (unsigned long)nowMs, (unsigned long)(nowMs - pwrDownMs));
                }
            } else if (lvl && nowMs - pwrTickMs >= 500) {
                pwrTickMs = nowMs;
                printf("BTN pwr still held %lums\r\n", (unsigned long)(nowMs - pwrDownMs));
            }
        }

        buttons_poll_gpio();

        // BOOT is deliberately NOT polled here. Reading it takes the flash
        // chip select away for a few microseconds with interrupts off, and
        // doing that fifty times a second underneath an app that is
        // continuously executing from flash, running DMA and talking I2C is a
        // large exposure for no benefit: the only thing this app needs to know
        // is whether BOOT is down at the instant PWR's long press arrives,
        // which is one read per event. A periodic poll was in here briefly and
        // the app hung with a white screen and no input, which is exactly what
        // a corrupted instruction fetch looks like.

        // Visible acknowledgement of a power-key press. g_keyEvent is now set
        // by pmic_poll_core1() on core1 instead of buttons_poll() here, but
        // the read-and-clear-once-per-loop contract is unchanged.
        if (g_keyEvent) {
            uint8_t ev = g_keyEvent;
            g_keyEvent = 0;
            if (ev & 0x04) {
                // Switching requires BOTH buttons: PWR held past the PMIC's
                // 1.5s long-press threshold, with BOOT down at that instant.
                // That is one BOOT read per long press and none in the hot
                // loop, which is the difference that makes it safe.
                if (bootbtn_pressed()) {
                    appswitch_go_other();
                    // Only reached when there is no other app to switch to.
                    flash_marker(fb, 128, 250);
                } else if (menuDoubleArmed) {
                    // Long double press (short press, then a second press
                    // held past the long-press threshold within
                    // MENU_DOUBLE_WINDOW_MS of the first): open the menu.
                    // See menu.h for why this shape was chosen and
                    // sketchpad_menu_poll_pwr() for how the menu gets PWR
                    // events without touching i2c1 from core0.
                    int chosen = menu_run(fb, sketchpad_menu_poll_pwr);
                    (void)chosen; // a real launch reboots and never returns
                                  // here; reaching this line means cancel,
                                  // or a launch request that could not be
                                  // issued.
                    // menu_run() has already restored the small strip it
                    // drew into (see menu.h): that IS this app's repaint,
                    // since a stroke has no representation other than the
                    // live framebuffer to redraw from. Nothing further to do.
                    printf("menu: closed (chosen=%d)\r\n", chosen);
                }
                menuDoubleArmed = false;
            } else if (ev & 0x08) {
                // Confirmed short press (verdict bit, latches on release).
                // Repaints the whole screen from the framebuffer.
                AMOLED_1IN8_Display(fb);
                printf("full refresh\r\n");
            }
            if (ev & 0x02) {
                // Press edge: only ever used to time the long-double-press
                // gesture above, never to trigger an action by itself.
                uint32_t nowMs2 = to_ms_since_boot(get_absolute_time());
                menuDoubleArmed = (nowMs2 - lastPwrPressMs <= MENU_DOUBLE_WINDOW_MS);
                lastPwrPressMs = nowMs2;
            }
        }

        // Drain everything core1 has queued since the last pass. Multiple
        // samples routinely arrive between two core0 iterations now that
        // core0 is not blocked on I2C at all; draining the whole backlog
        // before pushing means one push per outer loop still covers however
        // many samples landed, rather than growing the number of pushes.
        touch_sample_t smp;
        for (;;) {
            uint32_t pf_t = PF_NOW();
            bool got = touch_q_pop(&smp);
            PF_ADD(pf_touch_us, pf_t);
            if (!got) break;
#if PROFILE
            pf_drained++;
            {
                uint32_t depth = (g_touchTail - g_touchHead) & (TOUCH_Q_CAP - 1);
                if (depth > pf_qDepthPeak) pf_qDepthPeak = depth;
            }
#endif
            uint32_t pf_r = PF_NOW();

            int x = 0, y = 0;
            bool haveTouch = (smp.fingers != 0);
            if (haveTouch) {
                x = smp.x; y = smp.y;
                if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
                if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;
#if PROFILE
                // Count only genuinely new readings. core1 re-reads
                // continuously while Touch_INT_PIN is low (about 1.4kHz,
                // bounded by the I2C transaction cost) while the controller
                // itself only reports at ~60-68Hz, so most drained samples
                // repeat the last coordinate; this is the same dedup the
                // single-core version did, just now over drained samples
                // instead of raw loop iterations.
                if (x != pf_lastX || y != pf_lastY) { pf_reports++; pf_lastX = x; pf_lastY = y; }
#endif
            }

            if (haveTouch && patternShown) {
                // First touch wipes the test pattern; do not draw with it.
                patternShown = false;
                for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;
                AMOLED_1IN8_Display(fb);
                printf("pattern cleared\r\n");
            } else if (haveTouch) {
                uint32_t nowMs = smp.tMs;

                // Only act on genuinely new coordinates. See the pf_reports
                // comment above for why counting repeats would corrupt both
                // the jump allowance (derived from inter-sample interval)
                // and the stroke-start persistence check.
                bool newReport = (x != lastReportX) || (y != lastReportY);
                lastReportX = x;
                lastReportY = y;

                if (!newReport) {
                    // Nothing new from the controller: leave all stroke state be.
                } else if (!fingerDown) {
                    // A stroke starts only once contact has persisted for two
                    // reports, so a one-report blip leaves nothing behind.
                    // What makes a stray a stray is that it does not persist, not
                    // that it is far away. Requiring the two reports to agree on
                    // position instead broke fast strokes: consecutive reports of
                    // a quick flick are further apart than the agreement radius,
                    // so the stroke kept failing to start and left isolated dots
                    // where it briefly succeeded. Persistence alone is the test.
                    if (!pendingStart) {
                        pendingStart = true;
                        pendX = x; pendY = y;
                    } else {
                        pendingStart = false;
                        fingerDown = true;
                        haveCand = false;
                        lastSampleMs = nowMs;
                        // Begin at the first report and immediately extend to this
                        // one, so no travel is lost to the confirmation.
                        stroke_begin(fb, pendX, pendY, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        lastRawX = x; lastRawY = y;
                        stroke_sample(fb, x, y, false, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        printf("stroke start (%d,%d) t=%lu\r\n",
                               pendX, pendY, (unsigned long)nowMs);
                    }
                } else {
                    float jx = (float)(x - lastRawX), jy = (float)(y - lastRawY);
                    float dtMs = (float)(nowMs - lastSampleMs);
                    float allow = MAX_SPEED_PX_PER_MS * dtMs;
                    if (allow < MIN_JUMP_ALLOW_PX) allow = MIN_JUMP_ALLOW_PX;
                    if (allow > MAX_JUMP_PX) allow = MAX_JUMP_PX;

                    float jumpSq = jx * jx + jy * jy;
                    bool confirmed = haveCand &&
                        ((float)(x - candX) * (float)(x - candX) +
                         (float)(y - candY) * (float)(y - candY) <= CONFIRM_PX * CONFIRM_PX);

                    if (jumpSq <= allow * allow) {
                        haveCand = false;
                        lastRawX = x; lastRawY = y;
                        lastSampleMs = nowMs;
                        stroke_sample(fb, x, y, bridging, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        bridging = false;
                    } else if (confirmed) {
                        // The finger really is over there. Too far to be a dropout
                        // in one stroke, so close this stroke and open a new one
                        // instead of drawing a line across the gap.
                        haveCand = false;
                        stroke_end(fb, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        stroke_begin(fb, x, y, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        lastRawX = x; lastRawY = y;
                        lastSampleMs = nowMs;
                        bridging = false;
                        splits++;
                        printf("stroke split at (%d,%d) gap=%dpx dt=%dms\r\n",
                               x, y, (int)sqrtf(jumpSq), (int)dtMs);
                    } else {
                        candX = x; candY = y;
                        haveCand = true;
                        glitches++;
                    }
                }
#if PROFILE
                pf_samples++;
#endif
            } else if (pendingStart && !fingerDown) {
                // Contact appeared for a single report and vanished. Nothing was
                // drawn, which is the whole point of waiting for confirmation.
                pendingStart = false;
                lastReportX = -1; lastReportY = -1;
                strays++;
            } else if (fingerDown) {
                // No contact reported. This is either a real lift or the controller
                // briefly losing a fast-moving finger, and the two are
                // indistinguishable at this instant, so wait before believing it.
                uint32_t nowMs = smp.tMs;
                if (nowMs - lastSampleMs >= LIFT_DEBOUNCE_MS) {
                    fingerDown = false;
                    bridging = false;
                    // Forget the last coordinates, so touching down again on the
                    // exact same pixel still counts as a new report.
                    lastReportX = -1; lastReportY = -1;
                    stroke_end(fb, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    printf("stroke end t=%lu\r\n", (unsigned long)nowMs);
                } else {
                    // Still inside the grace window: keep the stroke open, and mark
                    // the next real sample as the one that has to bridge the gap.
                    // Counted once per episode, not once per drained sample, which
                    // at the queue's drain rate would otherwise be meaningless.
                    if (!bridging) dropouts++;
                    bridging = true;
                }
            }
            PF_ADD(pf_raster_us, pf_r);
        }

        // Published every pass so core1's shake check (imu_poll_core1) sees a
        // reasonably fresh value; see g_fingerDownShared's declaration for why
        // a plain volatile bool is enough here.
        g_fingerDownShared = fingerDown;

        // Shake-to-erase: core1 bumps g_eraseSeq once per accepted shake
        // (jolt count, cooldown and the finger-down suppression all live in
        // imu_poll_core1() now); core0 just watches for the sequence number
        // to move and does the actual erase, since that touches the
        // framebuffer and QSPI, not i2c1.
        {
            uint32_t seq = g_eraseSeq;
            if (seq != lastEraseSeqSeen) {
                lastEraseSeqSeen = seq;
                wipe_erase(fb);
                g_pressure = 0.5f;
                g_arcLen = 0.0f;
                g_radius = 0.0f;
                g_dirX = 0.0f;
                g_dirY = 0.0f;
                g_haveH0 = false;
                printf("erase (shake)\r\n");
                dMinX = PANEL_W; dMinY = PANEL_H; dMaxX = -1; dMaxY = -1;
            }
        }

        uint32_t pf_p = PF_NOW();
        push_dirty(fb, dMinX, dMinY, dMaxX, dMaxY);
        PF_ADD(pf_push_us, pf_p);

#if PROFILE
        pf_loops++;
        if (dMaxX >= dMinX && dMaxY >= dMinY) {
            pf_pushes++;
            pf_pushPx += (uint32_t)(dMaxX - dMinX + 1) * (uint32_t)(dMaxY - dMinY + 1);
        }
        {
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (nowMs - pf_lastMs >= 1000) {
                uint32_t d = pf_loops ? pf_loops : 1;
                uint32_t s = pf_samples ? pf_samples : 1;
                uint32_t p = pf_pushes ? pf_pushes : 1;

                uint32_t curTouchReads = g_touchReads;
                uint32_t curTouchTimeouts = g_touchTimeouts;
                uint32_t curTouchDrops = g_touchQueueDrops;
                uint32_t curTouchRecoveries = g_touchRecoveries;
                uint32_t curImuTimeouts = g_imuTimeouts;
                uint32_t curPmicTimeouts = g_pmicTimeouts;
                uint32_t qDepthNow = (g_touchTail - g_touchHead) & (TOUCH_Q_CAP - 1);

                // Stay quiet unless something was actually drawn this second.
                // Idle lines say nothing and made it impossible to tell a
                // capture that missed the drawing from one that measured it.
                if (pf_samples > 0) {
                    printf("prof loops=%lu samples=%lu reports=%lu glitches=%lu dropouts=%lu strays=%lu splits=%lu "
                           "| touch %luus/loop | raster %luus/sample | push %luus/push avg %lupx | total %luus/loop\r\n",
                           pf_loops, pf_samples, pf_reports,
                           (unsigned long)glitches, (unsigned long)dropouts,
                           (unsigned long)strays, (unsigned long)splits,
                           pf_touch_us / d, pf_raster_us / s,
                           pf_push_us / p, pf_pushPx / p,
                           (pf_touch_us + pf_raster_us + pf_push_us) / d);
                    printf("prof2 drained=%lu/s subsegs=%lu/s qdepth=%lu peak=%lu "
                           "| core1: reads=%lu/s timeouts=%lu/s drops=%lu/s recoveries=%lu "
                           "| imu timeouts=%lu/s pmic timeouts=%lu/s\r\n",
                           (unsigned long)pf_drained, (unsigned long)pf_subsegs,
                           (unsigned long)qDepthNow, (unsigned long)pf_qDepthPeak,
                           (unsigned long)(curTouchReads - lastTouchReads),
                           (unsigned long)(curTouchTimeouts - lastTouchTimeouts),
                           (unsigned long)(curTouchDrops - lastTouchDrops),
                           (unsigned long)(curTouchRecoveries - lastTouchRecoveries),
                           (unsigned long)(curImuTimeouts - lastImuTimeouts),
                           (unsigned long)(curPmicTimeouts - lastPmicTimeouts));
                }

                pf_touch_us = pf_raster_us = pf_push_us = 0;
                pf_loops = pf_samples = pf_pushes = pf_pushPx = pf_reports = 0;
                pf_drained = pf_subsegs = pf_qDepthPeak = 0;
                glitches = 0;
                dropouts = 0;
                strays = 0;
                splits = 0;
                lastTouchReads = curTouchReads;
                lastTouchTimeouts = curTouchTimeouts;
                lastTouchDrops = curTouchDrops;
                lastTouchRecoveries = curTouchRecoveries;
                lastImuTimeouts = curImuTimeouts;
                lastPmicTimeouts = curPmicTimeouts;
                pf_lastMs = nowMs;
            }
        }
#endif
    }
}
