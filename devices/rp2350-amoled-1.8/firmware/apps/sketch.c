/*
 * sketch: the drawing app.
 *
 * Carved out of the pre-runtime firmware/main.c, which used to be the whole
 * firmware. This file is the pen pipeline and nothing else: the framebuffer,
 * the panel push, the i2c1 touch/IMU/PMIC ownership, the menu gesture and the
 * profiler all moved to the runtime (see runtime/gfx.h, runtime/sensors.h,
 * runtime/app.h). What is left here is hardware-proven against real strokes
 * on this exact panel and finger, and none of its tuning changed in the move.
 *
 * The sketchpad is the one app that reads the raw touch sample stream
 * (sensors_touch_next()) instead of the runtime's resolved touchDown/
 * touchX/touchY: its stroke reconstruction (dropout bridging, glitch
 * rejection, stroke-start confirmation, split-on-far-jump) needs every
 * individual report, not just "is a finger down right now".
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "DEV_Config.h"

#include "app.h"
#include "gfx.h"
#include "sensors.h"

/* ---------------------------------------------------------------------
 * Pen shape tuning (tldraw-style variable width draw tool).
 * Smoothing is a latency knob as much as a smoothness one: at 0.55 the drawn
 * point trails the finger by roughly two reports by construction, which was
 * the largest contributor to felt lag once the pipeline itself measured clean
 * (raster 9us, push 27us). 0.35 tightens it at the cost of some jitter.
 * ------------------------------------------------------------------- */
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

// Touch-stall recovery timeout. Unlike the rest of this block, nothing in
// this file actually reads it: the FT3168 stall watchdog it used to gate
// (touch_recover_core1(), on the old single-core hot loop) now lives
// entirely in sensors.c, with its own copy of this same value and its full
// history comment. Kept here, unused, because it was part of the pen-tuning
// block in the pre-split main.c and the porting brief listed it alongside
// the rest of that block; flagged in the port report rather than dropped
// silently.
#define TOUCH_STALL_MS 5000

/* ---------------------------------------------------------------------
 * Anti-aliased capsule rasterizer. Pixel format helpers (px_to_gray,
 * gray_to_px) now live in gfx.h; see its header comment for why the green
 * channel doubles as an 8-bit ink value on this monochrome-in-practice panel.
 * ------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------
 * App state. Lives in the arena (see app.h), not in file-scope statics: an
 * app switch resets the arena, and this struct is everything the sketchpad
 * needs to pick up mid-gesture, so leaving it behind is exactly right.
 * ------------------------------------------------------------------- */
typedef struct {
    // Stroke pen state. Formerly file-scope statics g_sx/g_sy/g_pressure/
    // g_arcLen/g_radius/g_dirX/g_dirY in main.c.
    float sx, sy;      // current smoothed point
    float pressure;
    float arcLen;      // accumulated arc length, for the start taper
    float radius;      // radius at (sx, sy)
    float dirX, dirY;  // last travel direction, for the end taper

    // History for draw_quad_midpoint: the smoothed point from *two* samples
    // ago (the one before sx/sy's predecessor). haveH0 is false until a
    // stroke has at least two stroke_sample() calls behind it, which is also
    // true right after a bridged dropout (see stroke_sample): a curve drawn
    // across a gap would bow toward a control point that predates the gap,
    // so bridges fall back to a straight segment and the curve resumes on
    // the sample after. Formerly g_h0x/g_h0y/g_h0r/g_haveH0.
    float h0x, h0y, h0r;
    bool haveH0;

    // Stroke-machine state. Formerly main()'s while-loop locals.
    bool fingerDown;
    int lastRawX, lastRawY;
    uint32_t lastSampleMs;
    bool bridging;
    bool pendingStart;
    int pendX, pendY;
    bool haveCand;
    int candX, candY;
    int lastReportX, lastReportY;

    // Diagnostic counters. Formerly main()'s while-loop locals, drained into
    // the per-second profiler print, which moved to the runtime along with
    // everything else profiling-related. Nothing currently reads these; kept
    // as struct fields (rather than dropped) because they are exactly the
    // "the counters" the porting brief named, and they cost 16 bytes.
    uint32_t glitches, dropouts, strays, splits;
} sketch_state_t;

static sketch_state_t *st;

static void stroke_begin(sketch_state_t *st, uint16_t *fb, int x, int y,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    st->sx = (float)x;
    st->sy = (float)y;
    st->pressure = 0.5f;
    st->arcLen = 0.0f;
    st->dirX = 0.0f;
    st->dirY = 0.0f;
    st->radius = pressure_to_radius(st->pressure) * 0.35f; // start-taper factor at arc==0
    st->haveH0 = false; // not enough history yet for a curve
    draw_capsule(fb, st->sx, st->sy, st->radius, st->sx, st->sy, st->radius, dMinX, dMinY, dMaxX, dMaxY);
}

// `bridge` marks the first sample after the controller lost and regained
// contact. Such a sample is handled differently in two ways: it snaps straight
// to the reported position instead of being smoothed toward it (smoothing
// across a large gap would leave the ink trailing well behind the finger and
// put a kink in the line), and it leaves pressure alone, so the width carries
// continuously across the gap instead of thinning as if the finger had
// suddenly accelerated. The result is one straight segment filling the gap.
static void stroke_sample(sketch_state_t *st, uint16_t *fb, int x, int y, bool bridge,
                           int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    float prevX = st->sx, prevY = st->sy, prevR = st->radius;

    float k = bridge ? 1.0f : (1.0f - STREAMLINE);
    float nx = st->sx + ((float)x - st->sx) * k;
    float ny = st->sy + ((float)y - st->sy) * k;
    float dist = sqrtf((nx - prevX) * (nx - prevX) + (ny - prevY) * (ny - prevY));
    if (dist < DEDUPE_PX) return; // finger resting: drop the jitter, keep old state

    st->sx = nx;
    st->sy = ny;

    if (!bridge) {
        float target = 1.0f - fminf(1.0f, dist / SPEED_MAX);
        st->pressure += (target - st->pressure) * PRESSURE_LERP;
    }
    float r = pressure_to_radius(st->pressure);

    st->arcLen += dist;
    if (st->arcLen < START_TAPER_LEN) {
        r *= (0.35f + 0.65f * (st->arcLen / START_TAPER_LEN));
    }

    if (bridge) {
        // A curve here would bow toward whatever was drawn before the
        // dropout, which is stale by definition; fill the reconnection
        // straight, same as before curve-fitting existed.
        draw_capsule(fb, prevX, prevY, prevR, st->sx, st->sy, r, dMinX, dMinY, dMaxX, dMaxY);
        st->haveH0 = false; // don't curve the *next* segment across this gap either
    } else if (st->haveH0) {
        draw_quad_midpoint(fb, st->h0x, st->h0y, st->h0r, prevX, prevY, prevR, st->sx, st->sy, r,
                            dMinX, dMinY, dMaxX, dMaxY);
    } else {
        // First real sample of the stroke (or the one right after a bridge):
        // not enough history for a curve yet, so draw the plain straight
        // span, same as the pre-curve code always did.
        draw_capsule(fb, prevX, prevY, prevR, st->sx, st->sy, r, dMinX, dMinY, dMaxX, dMaxY);
    }
    st->h0x = prevX; st->h0y = prevY; st->h0r = prevR;
    st->haveH0 = true;

    st->radius = r;
    st->dirX = (nx - prevX) / dist;
    st->dirY = (ny - prevY) / dist;
}

static void stroke_end(sketch_state_t *st, uint16_t *fb,
                        int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (st->haveH0) {
        // By construction, the last curve segment drawn in stroke_sample
        // stopped at midpoint(h0, current) rather than at the current
        // point itself - that's what lets consecutive curve segments meet
        // smoothly. Draw the remaining half straight, or the stroke visibly
        // falls short of where the finger actually lifted.
        float mx = (st->h0x + st->sx) * 0.5f, my = (st->h0y + st->sy) * 0.5f;
        float mr = (st->h0r + st->radius) * 0.5f;
        draw_capsule(fb, mx, my, mr, st->sx, st->sy, st->radius, dMinX, dMinY, dMaxX, dMaxY);
    }

    // A compile-time constant, not per-stroke state: it lives in .rodata
    // either way, so it stays a function-local static rather than moving
    // into sketch_state_t along with the mutable fields above.
    static const float scales[3] = {0.7f, 0.45f, 0.25f};
    float curX = st->sx, curY = st->sy, curR = st->radius;
    for (int i = 0; i < 3; i++) {
        float nx = curX + st->dirX * 1.2f;
        float ny = curY + st->dirY * 1.2f;
        float nr = st->radius * scales[i];
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
 * Detection itself (the jolt window, the cooldown, the IMU poll) runs on
 * core1 now (sensors.c's imu_poll_core1()), since the IMU shares i2c1 with
 * touch and the PMIC. This app only ever sees the verdict, via f->shaken
 * (wantsShake opts into it; see app.h and sensors.h). wipe_erase() is what
 * is left here on core0: framebuffer and panel work, no I2C.
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
        // Through gfx_push(), not AMOLED_1IN8_DisplayWindows directly: the
        // 8-pixel row-length rule (docs/decisions/0001-push-min-width.md) is
        // not optional, and gfx_push is the only place allowed to apply it.
        // Full width is already a multiple of 8 (PANEL_W is 368), so this
        // pushes exactly the same window it always did.
        gfx_push(0, y0, PANEL_W - 1, y1 - 1);
        DEV_Delay_ms(15);
    }
}

/* ---------------------------------------------------------------------
 * app_t callbacks.
 * ------------------------------------------------------------------- */

static void sketch_enter(void) {
    // APP_STATE zeroes: the arena was just reset, and app_alloc hands back
    // zeroed memory (see app.h). Nothing else to do: the framebuffer is
    // already white (the runtime's job), and the runtime pushes it, not us.
    st = APP_STATE(sketch_state_t);
}

static void sketch_tick(const app_frame_t *f) {
    int dMinX = PANEL_W, dMinY = PANEL_H, dMaxX = -1, dMaxY = -1;

    // Drain everything core1 has queued since the last pass. Multiple
    // samples routinely arrive between two ticks now that core0 is never
    // blocked on I2C at all; draining the whole backlog before pushing means
    // one push per tick still covers however many samples landed, rather
    // than growing the number of pushes.
    touch_sample_t smp;
    for (;;) {
        if (!sensors_touch_next(&smp)) break;

        int x = 0, y = 0;
        bool haveTouch = (smp.fingers != 0);
        if (haveTouch) {
            x = smp.x; y = smp.y;
            if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
            if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;
        }

        if (haveTouch) {
            uint32_t nowMs = smp.tMs;

            // Only act on genuinely new coordinates. core1 re-reads
            // continuously while Touch_INT_PIN is low (about 1.4kHz, bounded
            // by the I2C transaction cost) while the controller itself only
            // reports at ~60-68Hz, so most drained samples repeat the last
            // coordinate. Counting repeats would corrupt both the jump
            // allowance (derived from inter-sample interval) and the
            // stroke-start persistence check below.
            bool newReport = (x != st->lastReportX) || (y != st->lastReportY);
            st->lastReportX = x;
            st->lastReportY = y;

            if (!newReport) {
                // Nothing new from the controller: leave all stroke state be.
            } else if (!st->fingerDown) {
                // A stroke starts only once contact has persisted for two
                // reports, so a one-report blip leaves nothing behind.
                // What makes a stray a stray is that it does not persist, not
                // that it is far away. Requiring the two reports to agree on
                // position instead broke fast strokes: consecutive reports of
                // a quick flick are further apart than the agreement radius,
                // so the stroke kept failing to start and left isolated dots
                // where it briefly succeeded. Persistence alone is the test.
                if (!st->pendingStart) {
                    st->pendingStart = true;
                    st->pendX = x; st->pendY = y;
                } else {
                    st->pendingStart = false;
                    st->fingerDown = true;
                    st->haveCand = false;
                    st->lastSampleMs = nowMs;
                    // Begin at the first report and immediately extend to this
                    // one, so no travel is lost to the confirmation.
                    stroke_begin(st, gfx_fb, st->pendX, st->pendY, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    st->lastRawX = x; st->lastRawY = y;
                    stroke_sample(st, gfx_fb, x, y, false, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    printf("stroke start (%d,%d) t=%lu\r\n",
                           st->pendX, st->pendY, (unsigned long)nowMs);
                }
            } else {
                float jx = (float)(x - st->lastRawX), jy = (float)(y - st->lastRawY);
                float dtMs = (float)(nowMs - st->lastSampleMs);
                float allow = MAX_SPEED_PX_PER_MS * dtMs;
                if (allow < MIN_JUMP_ALLOW_PX) allow = MIN_JUMP_ALLOW_PX;
                if (allow > MAX_JUMP_PX) allow = MAX_JUMP_PX;

                float jumpSq = jx * jx + jy * jy;
                bool confirmed = st->haveCand &&
                    ((float)(x - st->candX) * (float)(x - st->candX) +
                     (float)(y - st->candY) * (float)(y - st->candY) <= CONFIRM_PX * CONFIRM_PX);

                if (jumpSq <= allow * allow) {
                    st->haveCand = false;
                    st->lastRawX = x; st->lastRawY = y;
                    st->lastSampleMs = nowMs;
                    stroke_sample(st, gfx_fb, x, y, st->bridging, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    st->bridging = false;
                } else if (confirmed) {
                    // The finger really is over there. Too far to be a dropout
                    // in one stroke, so close this stroke and open a new one
                    // instead of drawing a line across the gap.
                    st->haveCand = false;
                    stroke_end(st, gfx_fb, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    stroke_begin(st, gfx_fb, x, y, &dMinX, &dMinY, &dMaxX, &dMaxY);
                    st->lastRawX = x; st->lastRawY = y;
                    st->lastSampleMs = nowMs;
                    st->bridging = false;
                    st->splits++;
                    printf("stroke split at (%d,%d) gap=%dpx dt=%dms\r\n",
                           x, y, (int)sqrtf(jumpSq), (int)dtMs);
                } else {
                    st->candX = x; st->candY = y;
                    st->haveCand = true;
                    st->glitches++;
                }
            }
        } else if (st->pendingStart && !st->fingerDown) {
            // Contact appeared for a single report and vanished. Nothing was
            // drawn, which is the whole point of waiting for confirmation.
            st->pendingStart = false;
            st->lastReportX = -1; st->lastReportY = -1;
            st->strays++;
        } else if (st->fingerDown) {
            // No contact reported. This is either a real lift or the controller
            // briefly losing a fast-moving finger, and the two are
            // indistinguishable at this instant, so wait before believing it.
            uint32_t nowMs = smp.tMs;
            if (nowMs - st->lastSampleMs >= LIFT_DEBOUNCE_MS) {
                st->fingerDown = false;
                st->bridging = false;
                // Forget the last coordinates, so touching down again on the
                // exact same pixel still counts as a new report.
                st->lastReportX = -1; st->lastReportY = -1;
                stroke_end(st, gfx_fb, &dMinX, &dMinY, &dMaxX, &dMaxY);
                printf("stroke end t=%lu\r\n", (unsigned long)nowMs);
            } else {
                // Still inside the grace window: keep the stroke open, and mark
                // the next real sample as the one that has to bridge the gap.
                if (!st->bridging) st->dropouts++;
                st->bridging = true;
            }
        }
    }

    // The runtime calls sensors_set_finger_down() before tick() runs, but it
    // has nothing real to base that call on yet (it does not drain the touch
    // queue itself, see the file banner above): resolving fingerDown IS this
    // drain loop's job. Publish our own answer now that it is known, so
    // core1's shake suppression (see wipe_erase's header comment) sees this
    // tick's state and not a stale or synthetic one.
    sensors_set_finger_down(st->fingerDown);

    // Shake-to-erase: the runtime only delivers f->shaken (true for exactly
    // the tick an accepted shake lands on) because wantsShake is set below.
    if (f->shaken) {
        wipe_erase(gfx_fb);
        st->pressure = 0.5f;
        st->arcLen = 0.0f;
        st->radius = 0.0f;
        st->dirX = 0.0f;
        st->dirY = 0.0f;
        st->haveH0 = false;
        printf("erase (shake)\r\n");
        dMinX = PANEL_W; dMinY = PANEL_H; dMaxX = -1; dMaxY = -1;
    }

    gfx_push(dMinX, dMinY, dMaxX, dMaxY);
}

const app_t g_sketchApp = {
    .name = "draw",
    .enter = sketch_enter,
    .tick = sketch_tick,
    .landscape = false,     // the sketchpad draws portrait
    .wantsShake = true,     // shake-to-erase IS this app's identity
};
