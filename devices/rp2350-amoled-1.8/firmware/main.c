#include <math.h>
#include <stdbool.h>
#include "pico/time.h"
#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "QMI8658.h"
#include "appswitch.h"
#include "bootbtn.h"

#define PANEL_W AMOLED_1IN8_WIDTH
#define PANEL_H AMOLED_1IN8_HEIGHT

// Touch diagnostics. Costs 3 extra I2C reads every 250ms, so keep it off
// unless something is actually wrong; when on it prints the controller's
// WhoAmI, the finger count and the raw coordinates.
#define DEBUG_TOUCH 0

// Loop profiler: splits frame time across the three suspects (reading touch
// over I2C, rasterising the segment, pushing the dirty rect over QSPI) and
// prints once a second. Printing more often than that would perturb what it
// is trying to measure.
#define PROFILE 1

#if PROFILE
static uint32_t pf_touch_us, pf_raster_us, pf_push_us;
static uint32_t pf_loops, pf_samples, pf_pushes, pf_pushPx, pf_reports;
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
#define TOUCH_STALL_MS 5000

// Shake-to-erase tuning.
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

    draw_capsule(fb, prevX, prevY, prevR, g_sx, g_sy, r, dMinX, dMinY, dMaxX, dMaxY);

    g_radius = r;
    g_dirX = (nx - prevX) / dist;
    g_dirY = (ny - prevY) / dist;
}

static void stroke_end(uint16_t *fb, int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
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
 * QMI8658_read_xyz gives acceleration in mg; at rest |acc| ~= 1000 (1 g).
 * A single sample far from that is a "jolt" but is indistinguishable from
 * a bump or a firm tap, so we require several jolts inside a short
 * rolling window before treating it as an intentional shake, and then
 * enforce a cooldown so the same shake cannot be counted twice.
 * ------------------------------------------------------------------- */
static bool shake_poll_and_check(bool fingerDown) {
    static uint32_t lastPollMs = 0;
    static uint32_t joltTimes[JOLT_MAX];
    static int joltCount = 0;
    static uint32_t cooldownUntilMs = 0;

    uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    if (nowMs - lastPollMs < IMU_POLL_MS) return false;
    lastPollMs = nowMs;

    float acc[3], gyro[3];
    unsigned int tim = 0;
    QMI8658_read_xyz(acc, gyro, &tim);

    float mag = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    float dev = fabsf(mag - 1000.0f);
    if (dev > JOLT_DEV_MG) {
        int w = 0;
        for (int i = 0; i < joltCount; i++) {
            if (nowMs - joltTimes[i] <= JOLT_WINDOW_MS) joltTimes[w++] = joltTimes[i];
        }
        joltCount = w;
        if (joltCount < JOLT_MAX) joltTimes[joltCount++] = nowMs;
    }

    if (joltCount >= JOLT_MIN_COUNT && nowMs >= cooldownUntilMs && !fingerDown) {
        cooldownUntilMs = nowMs + ERASE_COOLDOWN_MS;
        joltCount = 0;
        return true;
    }
    return false;
}

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
 * Touch power state and stall recovery.
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
// GPIO2). Power-key events themselves stay on; they are a feature now.
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
    // events, which are noise for this purpose.
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x40, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x41, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x42, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x48, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x4A, 0xFF);

    printf("button hunt armed: watching %u pins + AXP2101 irq\r\n",
           (unsigned)sizeof(probePins));
}

// Set by buttons_poll, consumed by the main loop, which is where the
// framebuffer lives. Bits are REG 0x49's: 3 short press, 2 long press.
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

static void buttons_poll(void) {
    uint32_t nowMs = to_ms_since_boot(get_absolute_time());

    // Report a pin only once its new level has held for 30ms. GPIO16 turned
    // out to toggle continuously at roughly the panel refresh rate, which is a
    // real periodic signal (the display tearing-effect line, most likely) and
    // not a button; without debouncing it buries everything else. No human
    // press is shorter than 30ms, so nothing real is lost.
#if DEBUG_PIN_SWEEP
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

    // A periodic snapshot of everything, so a press can be found by diffing
    // two lines even if it never produces a clean edge.
    static uint32_t snapMs = 0;
    if (nowMs - snapMs >= 2000) {
        snapMs = nowMs;
        printf("snap pins");
        for (size_t i = 0; i < sizeof(probePins); i++) {
            printf(" %u=%u", (unsigned)probePins[i], (unsigned)probePrev[i]);
        }
        printf(" | axp");
        for (uint8_t r = 0x00; r <= 0x03; r++) {
            printf(" %02x=%02x", r, DEV_I2C_Read_Byte(AXP2101_ADDR, r));
        }
        printf(" | sysout=%d\r\n", gpio_get(SYS_OUT));
    }
#endif

    // The PMIC is on the same I2C bus as touch, so poll it gently.
    static uint32_t lastMs = 0;
    if (nowMs - lastMs < 40) return;
    lastMs = nowMs;

    uint8_t s0 = DEV_I2C_Read_Byte(AXP2101_ADDR, 0x48);
    uint8_t s1 = DEV_I2C_Read_Byte(AXP2101_ADDR, 0x49);
    uint8_t s2 = DEV_I2C_Read_Byte(AXP2101_ADDR, 0x4A);
    if (s0 || s1 || s2) {
        // Bit names are from the AXP2101 datasheet, REG 49H (IRQ Status 1):
        // bit0 POWERON positive edge, bit1 negative edge, bit2 long press,
        // bit3 short press. The observed 0x02-then-0x09 pattern matches, so
        // press and release are both accounted for.
        //
        // REG 27H sets the two thresholds that matter here: the long-press IRQ
        // fires at 1.5s by default, and the PMIC cuts power at 6s. That gap is
        // what makes a hold usable as a gesture, with 4.5s of margin.
        printf("KEY raw=%02x %02x %02x t=%lu%s%s%s%s\r\n",
               s0, s1, s2, (unsigned long)nowMs,
               (s1 & 0x02) ? " down" : "",
               (s1 & 0x01) ? " up" : "",
               (s1 & 0x08) ? " SHORT" : "",
               (s1 & 0x04) ? " LONG" : "");
        if (s1 & 0x0C) g_keyEvent = s1 & 0x0C;
        // Write-1-to-clear, so the next event is distinguishable from this one.
        DEV_I2C_Write_Byte(AXP2101_ADDR, 0x48, s0);
        DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, s1);
        DEV_I2C_Write_Byte(AXP2101_ADDR, 0x4A, s2);
    }
}

// Scans the I2C bus by address. This exists because the vendor helpers discard
// every return code: DEV_I2C_Read_Byte reads into an uninitialised local and
// returns it regardless of whether the device acknowledged, so a chip that is
// absent or powered down reads as plausible-looking data rather than as an
// error. Anything printed here is a real ACK from a real device.
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

static void touch_recover(void) {
    FT3168_Reset();
    touch_set_active();
    sleep_ms(10);
    printf("touch recover (id=0x%02x)\r\n", FT3168_ReadID());
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
    // default and the reports-per-second figure below is the proof.
    printf("sketchpad ready (touch period reg=%u ms)\r\n",
           DEV_I2C_Read_Byte(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE));

    bool fingerDown = false;
    bool patternShown = false;
    int lastRawX = 0, lastRawY = 0;
    int glitchRun = 0;
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

    // Touch diagnostics: the boot-time WhoAmI print from FT3168_Init is lost
    // because USB CDC drops output until a host opens the port, so re-report
    // it here on a timer along with the raw register reads.
    uint32_t dbgLastMs = 0;
    uint32_t dbgLoops = 0;
    uint32_t lastFingerMs = to_ms_since_boot(get_absolute_time());

    while (true) {
        int dMinX = PANEL_W, dMinY = PANEL_H, dMaxX = -1, dMaxY = -1;

        uint32_t pf_t = PF_NOW();

        // Touch read, exactly the vendor sequence. This is the only variant
        // observed to work reliably on this hardware.
        //
        // Two "obvious" optimisations were tried and both broke touch:
        //   - one 5-byte burst from 0x02, on the assumption the controller
        //     auto-increments from the finger-count register into the
        //     coordinate registers;
        //   - dropping the redundant finger-count read that FT3168_Get_Point
        //     performs internally, and reading 0x03/0x05 directly instead.
        // The second failing is the informative one: it suggests the read of
        // the count register is not redundant at all, and is what latches a
        // fresh coordinate report. Cost is ~380us per finger-present loop,
        // which the profiling showed is ~98% of the frame. Worth revisiting
        // only with a datasheet in hand, not by guessing.
        uint8_t fingers = (uint8_t)FT3168_ReadState(FT3168_FINGER_NUMBER);
        int rawX = 0, rawY = 0;
        if (fingers != 0) {
            // One burst for both axes. The vendor path reads X (0x03,0x04) and
            // Y (0x05,0x06) as two separate I2C transactions, so a report that
            // lands between them yields a point built from the new X and the
            // old Y: a coordinate that the finger never visited, off the line
            // at right angles. At speed that draws visible spurs and smudges.
            // Auto-increment across these four registers is already relied on
            // by the vendor's own 2-byte reads, so this is the same access
            // widened, not a new assumption.
            //
            // The finger-count read above is deliberately left untouched.
            // Removing it broke touch entirely once before, which suggests it
            // is what latches a fresh report rather than being redundant.
            uint8_t b[4];
            DEV_I2C_Read_nByte(FT3168_I2C_ADDR, REG_X1_H, b, 4);
            rawX = ((int)(b[0] & 0x0F) << 8) | b[1];
            rawY = ((int)(b[2] & 0x0F) << 8) | b[3];
            FT3168.x_point = (uint16_t)rawX;
            FT3168.y_point = (uint16_t)rawY;
        }

        // The interrupt line is the controller's own opinion about whether a
        // finger is present, formed before any register read. Logging its edges
        // separates "the panel senses nothing" from "the panel senses it and we
        // read it wrong", which no amount of register polling can distinguish.
        {
            static int prevInt = 1;
            static bool selfTested = false;
            int lvl = gpio_get(Touch_INT_PIN);
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (lvl != prevInt) {
                prevInt = lvl;
                printf("INT %s t=%lu fingers=%u\r\n", lvl ? "high" : "LOW",
                       (unsigned long)nowMs, fingers);
            }
            // Once, late enough that a host is certainly attached and reading.
            if (!selfTested && nowMs > 6000) {
                selfTested = true;
                touch_selftest();
            }
        }

        // PWR button, reported by the AXP2101 on SYS_OUT rather than wired to
        // the MCU directly. The vendor demo treats it as active high and does
        // nothing but reboot on it. Logging both edges and the hold duration
        // answers three things at once: which of the two side buttons is PWR
        // (BOOT produces nothing at all at runtime), whether the line stays
        // asserted while held or merely pulses, and therefore whether a
        // long-press app switch is implementable at all.
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

        buttons_poll();

        // BOOT is deliberately NOT polled here. Reading it takes the flash
        // chip select away for a few microseconds with interrupts off, and
        // doing that fifty times a second underneath an app that is
        // continuously executing from flash, running DMA and talking I2C is a
        // large exposure for no benefit: the only thing this app needs to know
        // is whether BOOT is down at the instant PWR's long press arrives,
        // which is one read per event. A periodic poll was in here briefly and
        // the app hung with a white screen and no input, which is exactly what
        // a corrupted instruction fetch looks like.

        // Visible acknowledgement of a power-key press. A small square for a
        // short press, a large one for a long press, so which physical button
        // is PWR (and whether the PMIC's 1.5s long-press threshold is being
        // reached) can both be read straight off the screen.
        if (g_keyEvent) {
            uint8_t ev = g_keyEvent;
            g_keyEvent = 0;
            if (ev & 0x04) {
                // Switching requires BOTH buttons: PWR held past the PMIC's
                // 1.5s long-press threshold, with BOOT down at that instant.
                // That is one BOOT read per long press and none in the hot
                // loop, which is the difference that makes it safe.
                //
                // An earlier note here claimed reading BOOT was not worth the
                // risk because it had hung the board twice. That was a wrong
                // reading of the evidence: the hangs came from a flash probe
                // inside appswitch, and from flashes that silently never
                // landed while the app was already hung, so every "fix" under
                // test was actually the same old binary. BOOT reads reliably.
                if (bootbtn_pressed()) {
                    appswitch_go_other();
                    // Only reached when there is no other app to switch to.
                    flash_marker(fb, 128, 250);
                }
            } else {
                // Short press repaints the whole screen from the framebuffer.
                // This is a test, and a decisive one: the log reports zero
                // splits, glitches and strays, so the firmware believes it
                // drew continuous strokes, yet some strokes appear as dashes.
                // Either the framebuffer really has gaps, in which case a full
                // repaint changes nothing, or the partial-refresh path is
                // losing pixels on the way to the panel, in which case the
                // missing ink reappears. One press tells us which.
                AMOLED_1IN8_Display(fb);
                printf("full refresh\r\n");
            }
        }

        // Stall watchdog. Only the finger count can arm it, so a working
        // controller never triggers it while in use, and an idle one pays one
        // reset per TOUCH_STALL_MS with nothing on screen to disturb.
        {
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (fingers != 0) {
                lastFingerMs = nowMs;
            } else if (nowMs - lastFingerMs >= TOUCH_STALL_MS) {
                lastFingerMs = nowMs;
                touch_recover();
            }
        }

#if DEBUG_TOUCH
        dbgLoops++;
        {
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
            if (nowMs - dbgLastMs >= 250) {
                dbgLastMs = nowMs;
                uint16_t id = FT3168_ReadID();
                uint16_t rx = FT3168_ReadState(FT3168_COORDINATE_X);
                uint16_t ry = FT3168_ReadState(FT3168_COORDINATE_Y);
                uint8_t pwr = DEV_I2C_Read_Byte(FT3168_I2C_ADDR, REG_POWER_MODE);
                uint8_t mode = DEV_I2C_Read_Byte(FT3168_I2C_ADDR, 0x00);
                uint8_t ev = DEV_I2C_Read_Byte(FT3168_I2C_ADDR, REG_X1_H);
                printf("dbg t=%lu loops=%lu id=0x%02x mode=0x%02x pwr=0x%02x int=%d "
                       "fingers=%u ev=0x%02x raw=(%u,%u) struct=(%u,%u)\r\n",
                       (unsigned long)nowMs, (unsigned long)dbgLoops,
                       id, mode, pwr, gpio_get(Touch_INT_PIN),
                       fingers, ev, rx, ry, FT3168.x_point, FT3168.y_point);
                dbgLoops = 0;
            }
        }
#endif

        int x = 0, y = 0;
        bool haveTouch = false;
        if (fingers != 0) {
            x = rawX; y = rawY;
            if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
            if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;
            haveTouch = true;
#if PROFILE
            // Count only genuinely new readings. The gap between this and the
            // poll rate is how much of the I2C traffic is re-reading the same
            // point, and its ceiling is the panel's real report rate, which is
            // the floor on input latency no matter how fast we poll.
            if (x != pf_lastX || y != pf_lastY) { pf_reports++; pf_lastX = x; pf_lastY = y; }
#endif
        }
        PF_ADD(pf_touch_us, pf_t);

        uint32_t pf_r = PF_NOW();
        if (haveTouch && patternShown) {
            // First touch wipes the test pattern; do not draw with it.
            patternShown = false;
            for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;
            AMOLED_1IN8_Display(fb);
            printf("pattern cleared\r\n");
        } else if (haveTouch) {
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());

            // Only act on genuinely new coordinates. The loop runs about 5000
            // times a second while the controller reports at 60, so most
            // iterations re-read a report that was already handled. Filtering
            // here rather than inside stroke_sample matters twice over: the
            // jump allowance is derived from the interval between accepted
            // samples, and counting repeats collapsed that interval to ~3ms,
            // which made every fast stroke look like an impossible jump and
            // split it into dots; and the stroke-start check counts reports,
            // which repeats turned into two loop iterations 0.2ms apart, so it
            // was not really checking anything.
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
            uint32_t nowMs = to_ms_since_boot(get_absolute_time());
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
                // Counted once per episode, not once per loop iteration, which
                // at ~8700 loops/sec would otherwise be meaningless.
                if (!bridging) dropouts++;
                bridging = true;
            }
        }
        PF_ADD(pf_raster_us, pf_r);

        if (shake_poll_and_check(fingerDown)) {
            wipe_erase(fb);
            g_pressure = 0.5f;
            g_arcLen = 0.0f;
            g_radius = 0.0f;
            g_dirX = 0.0f;
            g_dirY = 0.0f;
            printf("erase (shake)\r\n");
            dMinX = PANEL_W; dMinY = PANEL_H; dMaxX = -1; dMaxY = -1;
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
                // Stay quiet unless something was actually drawn this second.
                // Idle lines say nothing and made it impossible to tell a
                // capture that missed the drawing from one that measured it.
                if (pf_samples > 0)
                printf("prof loops=%lu samples=%lu reports=%lu glitches=%lu dropouts=%lu strays=%lu splits=%lu | touch %luus/loop "
                       "| raster %luus/sample | push %luus/push avg %lupx | total %luus/loop\r\n",
                       pf_loops, pf_samples, pf_reports,
                       (unsigned long)glitches, (unsigned long)dropouts,
                       (unsigned long)strays, (unsigned long)splits,
                       pf_touch_us / d, pf_raster_us / s,
                       pf_push_us / p, pf_pushPx / p,
                       (pf_touch_us + pf_raster_us + pf_push_us) / d);
                pf_touch_us = pf_raster_us = pf_push_us = 0;
                pf_loops = pf_samples = pf_pushes = pf_pushPx = pf_reports = 0;
                glitches = 0;
                dropouts = 0;
                strays = 0;
                splits = 0;
                pf_lastMs = nowMs;
            }
        }
#endif
    }
}
