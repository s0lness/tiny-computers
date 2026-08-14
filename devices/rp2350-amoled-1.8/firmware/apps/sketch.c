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
 *
 * Pressure below is derived from stroke speed (SPEED_MAX, PRESSURE_LERP),
 * not from the touch controller. Measured 2026-08-13 (see sensors.h): the
 * FT3168 reports zero for both its weight and area registers, always, so it
 * cannot tell a light touch from a hard press. Speed-derived pressure is not
 * a stand-in for a better source; it is the only pressure signal this
 * hardware can produce.
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
//
// RAISED 80ms -> 220ms, 2026-08-14. The CONFIRM_MS fix below fixed stroke
// START (401 candidates -> 12/12 confirmed, strays=0, measured on hardware)
// but not stroke SURVIVAL: the same hardware session showed dropouts=354
// against strokeStarted=strokeEnded=12 - strokes that started fine were still
// getting cut into fragments mid-draw, which from the owner's side of the
// glass reads exactly like "traits qui s'arrêtent subitement" (his own
// description, 2026-08-14, drawing on real hardware), not like the
// stray-parasite problem CONFIRM_MS already closed ("j'ai pas de parasite
// mais j'ai des traits qui s'arrêtent subitement").
//
// 80ms only tolerates 4-5 consecutive missed reports at this controller's
// rate. TouchSim's dropout model (repro-touch-dropout-stroke-start.ts's
// scenario C), driven at the ~34 dropout-episodes/sec this codebase's other
// comments already measured on hardware, puts a run that long at roughly a 6
// percent chance PER EPISODE; at 34 episodes a second that is close to a
// certainty within the first second of any real stroke, matching what
// hardware showed. 220ms needs a run about three times as long to trip,
// which the same model puts at roughly a 0.03 percent chance per episode -
// the difference between "every stroke fragments" and "most few-second
// strokes survive intact".
//
// NOT raised further than this. The owner's own ask was "augmente UN PEU"
// (increase it a LITTLE), not "never let a stroke end", and every extra
// millisecond here is also extra time during which a genuine lift followed
// by a new touch NEARBY gets silently bridged into one stroke with a
// straight line joining the two, instead of ending cleanly. A lift-and-
// retouch far enough away always still splits regardless of this value (see
// MAX_JUMP_PX below - that half of the trade is not at risk here), but a
// nearby retouch within this window genuinely IS bridged, on purpose: that is
// the cost being paid for fewer broken lines. See
// repro-touch-dropout-stroke-start.ts's scenario D for the check that a
// prompt, real lift still ends cleanly at this value with no connecting line
// drawn, and SKETCH_LIVE_TUNE further down for turning this into something
// the owner can feel out live on the device instead of guessing at one
// number from a description.
//
// GOVERNS THE MID-STROKE CASE ONLY. A not-yet-confirmed candidate's own
// dropout grace was split off into PENDING_GRACE_MS (further down this
// file) the same day this was raised: testing this exact change against
// repro-touch-dropout-stroke-start.ts's scenario B showed that giving an
// unconfirmed candidate the SAME larger window measurably raised how often
// a lone stray got mistaken for a real touch - a risk with no MAX_JUMP_PX-
// style backstop, unlike bridging an already-drawing stroke. See
// PENDING_GRACE_MS_DEFAULT's comment for the full reasoning.
#define LIFT_DEBOUNCE_MS_DEFAULT 220.0f

// Glitch rejection as a speed limit rather than a fixed distance, because the
// allowed jump has to grow with the gap: bridging a dropout legitimately
// covers more ground than one 17ms report, and LIFT_DEBOUNCE_MS above (220ms,
// not the 80ms it used to be) is the longest gap this file will still call
// one stroke.
//
// MAX_JUMP_PX is the hard ceiling and it matters more than the speed. Without
// it the speed limit alone would permit a jump covering most of the panel
// across a full LIFT_DEBOUNCE_MS gap: lifting and touching down somewhere
// else would then draw a straight line clean across the screen joining the
// two. Beyond this ceiling the gap is not treated as one stroke at all, it
// ends the stroke and starts a new one, which is what a lift and re-touch
// actually is - and this ceiling is what keeps that true no matter how long
// LIFT_DEBOUNCE_MS is set to, live or otherwise (see SKETCH_LIVE_TUNE below).
// Measured from real strokes on this panel: a fast diagonal steps 50 to 61
// pixels between consecutive reports, so anything below about 4 px/ms rejects
// ordinary drawing. 6 px/ms leaves headroom; MAX_JUMP_PX is what actually
// stops a lift-and-retouch being joined by a line across the screen.
#define MAX_SPEED_PX_PER_MS_DEFAULT 6.0f
#define MIN_JUMP_ALLOW_PX_DEFAULT   40.0f
#define MAX_JUMP_PX_DEFAULT         150.0f

// A position that jumped further than a finger could travel is believed to
// be the finger's real new spot, rather than noise, only once a second
// report agrees with it to within this distance - see the jump/glitch/split
// handling below (the `confirmed` check under `st->fingerDown`). This no
// longer has anything to do with starting a stroke; see CONFIRM_MS below for
// that rule, which used to be a stricter form of this same idea and was
// split off once measurement showed why it needed different tuning.
#define CONFIRM_PX 25.0f

// Stroke-start confirmation: a stroke starts only once contact has
// persisted, not on the strength of one report. "Persisted" is satisfied by
// EITHER of two independent signals, because they answer two different
// things a single report cannot tell apart:
//   - a second report lands at a different position: the finger visibly
//     moved, so this is obviously real. Confirms immediately, no latency
//     added beyond the one report it took to see it (the original rule,
//     unchanged).
//   - CONFIRM_MS elapses since the first report while contact keeps being
//     seen at least once every LIFT_DEBOUNCE_MS: the finger did not move,
//     but it also never genuinely went away, which is what a stray never
//     manages. This is the new half.
//
// Why the new half exists, measured on hardware 2026-08-14
// (TOUCH_POLL_SELFTEST, a continuous real drawing session): 401 candidate
// stroke starts (pendingStart) produced 14 confirmed strokes
// (strokeStarted) - a 3.5 percent success rate - while dropouts=798 in the
// same window. The old rule required the SECOND report to arrive before the
// FT3168 dropped contact even once; the instant a candidate saw a single
// zero-finger read, sketch_tick's haveTouch==false branch threw it away as
// a stray, with none of the grace an already-started stroke gets from its
// own dropout-bridging window for the exact same phenomenon. Given how
// often this controller drops out mid-touch (not just mid-stroke), that
// killed nearly every real touch before it had a chance to move. A
// one-report blip that genuinely never comes back is still rejected as a
// stray, just after PENDING_GRACE_MS of grace instead of instantly - free,
// since nothing is drawn until a stroke actually starts.
//
// THE TRADE: this believes more candidates than before. A stray that
// happens to read nonzero on and off for the whole grace window can now be
// confirmed where the old rule could not; TOUCH_POLL_SELFTEST's `strays`
// counter (read alongside `pendingStart` and `strokeStarted`) is what proves
// whether that actually happens in practice, not reasoning about it in
// advance.
//
// A SKETCH_LIVE_TUNE default now (see below), not a hardcoded constant; the
// value and the reasoning above are unchanged by that.
#define CONFIRM_MS_DEFAULT 40.0f

// The pendingStart dropout-grace window: how long a not-yet-confirmed
// candidate is allowed to see zero contact before it is given up on as a
// stray (see the haveTouch==false / !fingerDown branch further down this
// file). Originally this WAS LIFT_DEBOUNCE_MS - the comment above still
// describes that original reasoning - until 2026-08-14's fix for stroke
// FRAGMENTATION (see LIFT_DEBOUNCE_MS_DEFAULT's own comment) raised that
// constant from 80ms to 220ms for the mid-stroke bridging case and, tested
// against this file's own dropout-heavy repro (scenario B,
// repro-touch-dropout-stroke-start.ts), measurably raised the rate of a
// lone stray getting confirmed too: an unconfirmed candidate got the SAME
// extra grace an already-drawing stroke did, even though the two carry very
// different risk. An already-drawing stroke that gets bridged wrongly is
// bounded by MAX_JUMP_PX (a stray jump far enough away still splits,
// regardless of the grace window); an unconfirmed candidate that gets
// wrongly believed just draws a stray mark straight onto the canvas, with
// no such backstop. So this stays split off at the ORIGINAL 80ms value,
// which is what scenario B was already passing at before either fix
// touched it, rather than inheriting LIFT_DEBOUNCE_MS's new, larger number.
#define PENDING_GRACE_MS_DEFAULT 80.0f

/* ---------------------------------------------------------------------
 * Live tuning (SKETCH_LIVE_TUNE). Development-only, gated exactly like
 * TOUCH_POLL_SELFTEST (sensors.h) and PMIC_WRITE_SELFTEST (sensors.c):
 * default 0, a CMake flag to turn it on (firmware/CMakeLists.txt), and a
 * shipped build simply does not have this surface. With the gate off, every
 * macro above resolves straight to its _DEFAULT constant below - there is no
 * runtime variable, no devlink command reachable, and therefore no way to
 * leave a knob in a bad position: the constant IS the value, same as before
 * this section existed.
 *
 * WHY THIS EXISTS. Every threshold in this file so far was chosen by
 * flashing a candidate, drawing on real glass, and reading a diagnostic
 * counter back over serial - a minute-plus round trip that breaks
 * concentration and means only two or three values ever actually get tried.
 * The owner asked to turn these knobs live instead, while drawing, with no
 * rebuild and no reflash: `bun tools/dev.ts tune lift 180` over devlink
 * (firmware/devlink.c's TUNE command), applied on the very next tick. A
 * second, faster-but-less-honest copy of the same knobs is exposed to the
 * emulator (emu_shim.c's emu_tune_get/emu_tune_set) for quick iteration -
 * "less honest" because TouchSim's dropout model is measurably kinder than
 * the real FT3168 (the pre-CONFIRM_MS stroke-start rule scored 63-83 percent
 * in the emulator against 3.5 percent on real hardware, see
 * repro-touch-dropout-stroke-start.ts), so a value that feels right in the
 * browser is a hypothesis, not a result; only the device-side knob, against
 * a real finger, can promote it to one.
 *
 * WHAT IS TUNABLE, and why these six and not the whole file: exactly the
 * constants that govern the behaviour the owner is judging by feel while
 * drawing - dropout tolerance and the jump-vs-glitch-vs-split boundary - not
 * the pen's cosmetic shape (STREAMLINE, PEN_SIZE and friends, top of this
 * file), which nobody has asked to feel out live. PENDING_GRACE_MS joined
 * the other five the same day LIFT_DEBOUNCE_MS was raised for stroke
 * survival (see that constant's own comment): the two used to be one
 * constant, split once testing showed an unconfirmed candidate and an
 * already-drawing stroke need different tuning here, not just different
 * names.
 *
 * FREEZING is the end state. When a value is settled, `bun tools/dev.ts tune
 * freeze` prints every current value as a `#define ..._DEFAULT` line, ready
 * to paste straight over the six above; the knobs and SKETCH_LIVE_TUNE
 * itself then get deleted. This file is not the frozen copy of anything
 * until that happens - the numbers above are still the starting point, not
 * the last word.
 * ------------------------------------------------------------------- */
#ifndef SKETCH_LIVE_TUNE
#define SKETCH_LIVE_TUNE 0
#endif

#if SKETCH_LIVE_TUNE
static float g_tuneLiftDebounceMs  = LIFT_DEBOUNCE_MS_DEFAULT;
static float g_tuneConfirmMs       = CONFIRM_MS_DEFAULT;
static float g_tunePendingGraceMs  = PENDING_GRACE_MS_DEFAULT;
static float g_tuneMinJumpAllowPx  = MIN_JUMP_ALLOW_PX_DEFAULT;
static float g_tuneMaxJumpPx       = MAX_JUMP_PX_DEFAULT;
static float g_tuneMaxSpeedPxPerMs = MAX_SPEED_PX_PER_MS_DEFAULT;

#define LIFT_DEBOUNCE_MS     g_tuneLiftDebounceMs
#define CONFIRM_MS           g_tuneConfirmMs
#define PENDING_GRACE_MS     g_tunePendingGraceMs
#define MIN_JUMP_ALLOW_PX    g_tuneMinJumpAllowPx
#define MAX_JUMP_PX          g_tuneMaxJumpPx
#define MAX_SPEED_PX_PER_MS  g_tuneMaxSpeedPxPerMs

// One declaration, walked by devlink's TUNE command (runtime.c's wiring) and
// by the emulator's wasm export (emu_shim.c's emu_tune_get/emu_tune_set) -
// the same "the firmware declares its own shape, nothing else hardcodes a
// list" pattern emu_device() already uses for the panel, the buttons and the
// sensors (emulator/wasm/emu_abi.h). Adding a sixth tunable later is one row
// here; neither of those two callers needs to change.
typedef struct {
    const char *protoName;  // devlink/emulator-facing identifier: short,
                             // lowercase, no spaces - typed at a prompt and
                             // sent over the wire, so it stays terse.
    const char *defineName; // the #define this becomes when frozen back into
                             // source, e.g. "LIFT_DEBOUNCE_MS" (FREEZE's own
                             // output adds the _DEFAULT suffix).
    float *value;
    float min, max, def;
} sketch_tunable_t;

static sketch_tunable_t g_sketchTunables[] = {
    { "lift",      "LIFT_DEBOUNCE_MS",    &g_tuneLiftDebounceMs,   20.0f, 1000.0f, LIFT_DEBOUNCE_MS_DEFAULT },
    { "confirm",   "CONFIRM_MS",          &g_tuneConfirmMs,         0.0f,  500.0f, CONFIRM_MS_DEFAULT },
    { "pendgrace", "PENDING_GRACE_MS",    &g_tunePendingGraceMs,   20.0f, 1000.0f, PENDING_GRACE_MS_DEFAULT },
    { "minjump",   "MIN_JUMP_ALLOW_PX",   &g_tuneMinJumpAllowPx,    0.0f,  300.0f, MIN_JUMP_ALLOW_PX_DEFAULT },
    // 368 = PANEL_W: a ceiling past the panel's own width allows nothing a
    // real jump could not already reach.
    { "maxjump",   "MAX_JUMP_PX",         &g_tuneMaxJumpPx,        10.0f,  368.0f, MAX_JUMP_PX_DEFAULT },
    { "maxspeed",  "MAX_SPEED_PX_PER_MS", &g_tuneMaxSpeedPxPerMs,   0.5f,   30.0f, MAX_SPEED_PX_PER_MS_DEFAULT },
};
#define SKETCH_TUNABLE_COUNT ((int)(sizeof(g_sketchTunables) / sizeof(g_sketchTunables[0])))

// A local strcmp-equivalent rather than <string.h>'s: this file is compiled
// for two very different targets (the pico-sdk board build, and the
// wasm32-freestanding emulator build via emulator/wasm/build.ts), and the
// freestanding target's shim/ directory stands in for stdlib.h/math.h/
// stdio.h (see that file's header comment) but not string.h - nothing in
// this codebase needed it before this section. Five short, fixed,
// hand-written names do not justify adding a fourth shim header for one
// function.
static bool sketch_tune_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static sketch_tunable_t *sketch_tune_find(const char *name) {
    for (int i = 0; i < SKETCH_TUNABLE_COUNT; i++) {
        if (sketch_tune_name_eq(g_sketchTunables[i].protoName, name)) return &g_sketchTunables[i];
    }
    return NULL;
}

int sketch_tune_count(void) { return SKETCH_TUNABLE_COUNT; }

bool sketch_tune_describe(int index, const char **name, float *min, float *max, float *def) {
    if (index < 0 || index >= SKETCH_TUNABLE_COUNT) return false;
    *name = g_sketchTunables[index].protoName;
    *min = g_sketchTunables[index].min;
    *max = g_sketchTunables[index].max;
    *def = g_sketchTunables[index].def;
    return true;
}

const char *sketch_tune_define_name(int index) {
    if (index < 0 || index >= SKETCH_TUNABLE_COUNT) return NULL;
    return g_sketchTunables[index].defineName;
}

bool sketch_tune_get(const char *name, float *out) {
    sketch_tunable_t *t = sketch_tune_find(name);
    if (!t) return false;
    *out = *t->value;
    return true;
}

bool sketch_tune_set(const char *name, float value, float *outApplied) {
    sketch_tunable_t *t = sketch_tune_find(name);
    if (!t) return false;
    if (value < t->min) value = t->min;
    if (value > t->max) value = t->max;
    *t->value = value;
    if (outApplied) *outApplied = value;
    return true;
}
#else // !SKETCH_LIVE_TUNE
#define LIFT_DEBOUNCE_MS     LIFT_DEBOUNCE_MS_DEFAULT
#define CONFIRM_MS           CONFIRM_MS_DEFAULT
#define PENDING_GRACE_MS     PENDING_GRACE_MS_DEFAULT
#define MIN_JUMP_ALLOW_PX    MIN_JUMP_ALLOW_PX_DEFAULT
#define MAX_JUMP_PX          MAX_JUMP_PX_DEFAULT
#define MAX_SPEED_PX_PER_MS  MAX_SPEED_PX_PER_MS_DEFAULT

// Reads as empty/false in a normal build, the same "0 when the gate is off"
// contract sensors.h's diagnostic structs already use (e.g.
// sensors_debug_touch_poll_selftest()): a shipping build's devlink TUNE
// command and the emulator's tunables panel both see "nothing declared"
// rather than a stub that pretends to work.
int sketch_tune_count(void) { return 0; }
bool sketch_tune_describe(int index, const char **name, float *min, float *max, float *def) {
    (void)index; (void)name; (void)min; (void)max; (void)def;
    return false;
}
const char *sketch_tune_define_name(int index) { (void)index; return NULL; }
bool sketch_tune_get(const char *name, float *out) { (void)name; (void)out; return false; }
bool sketch_tune_set(const char *name, float value, float *outApplied) {
    (void)name; (void)value; (void)outApplied;
    return false;
}
#endif // SKETCH_LIVE_TUNE

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
    uint32_t pendStartMs;    // when this candidate first armed - CONFIRM_MS is measured from here
    uint32_t pendLastTouchMs; // most recent haveTouch sample seen while pending - the dropout-grace
                              // clock (mirrors lastSampleMs's role for an already-started stroke)
    bool haveCand;
    int candX, candY;
    int lastReportX, lastReportY;

    // Diagnostic counters. Formerly main()'s while-loop locals, drained into
    // the per-second profiler print, which moved to the runtime along with
    // everything else profiling-related. Nothing currently reads these; kept
    // as struct fields (rather than dropped) because they are exactly the
    // "the counters" the porting brief named, and they cost 16 bytes.
    uint32_t glitches, dropouts, strays, splits;

#if TOUCH_POLL_SELFTEST
    // TEMPORARY: the app-side stage of the touch pipeline diagnostic (see
    // sensors.h's sketch_touch_diag_t). Every sample this app's drain loop
    // sees passes through here in order, so a live incident shows exactly
    // where the count stops growing: drained (came out of sensors_touch_
    // next() at all) -> haveTouch (fingers != 0) -> newReport (coordinates
    // actually moved) -> pendingStart (armed the two-report start check) ->
    // strokeStarted (persistence completed, ink should now be landing).
    // Gated, unlike the four counters above, because these are new state
    // added for this investigation specifically and must not ship.
    uint32_t diagDrained, diagHaveTouch, diagNewReport, diagPendingStart;
    uint32_t diagStrokeStarted, diagStrokeEnded;
#endif
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
#if TOUCH_POLL_SELFTEST
        st->diagDrained++;
#endif

        int x = 0, y = 0;
        bool haveTouch = (smp.fingers != 0);
        if (haveTouch) {
            x = smp.x; y = smp.y;
            if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
            if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;
#if TOUCH_POLL_SELFTEST
            st->diagHaveTouch++;
#endif
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
#if TOUCH_POLL_SELFTEST
            if (newReport) st->diagNewReport++;
#endif

            if (!st->fingerDown) {
                // See CONFIRM_MS's comment (top of file) for the full
                // reasoning and the measured 401->14 numbers behind this.
                // Unlike the mid-stroke branch below, this one runs on every
                // haveTouch sample while a candidate is pending, not just
                // newReport ones: the persisted check needs wall-clock time
                // to pass even while the controller keeps repeating the same
                // coordinate (haveTouch far outnumbers newReport on this
                // controller - measured 88977 against 1399 in one session -
                // so gating on newReport alone would rarely let a stationary
                // or dropout-interrupted touch confirm at all).
                if (!st->pendingStart) {
                    st->pendingStart = true;
                    st->pendX = x; st->pendY = y;
                    st->pendStartMs = nowMs;
                    st->pendLastTouchMs = nowMs;
#if TOUCH_POLL_SELFTEST
                    st->diagPendingStart++;
#endif
                } else {
                    st->pendLastTouchMs = nowMs;
                    bool persisted = (nowMs - st->pendStartMs) >= CONFIRM_MS;
                    if (newReport || persisted) {
                        st->pendingStart = false;
                        st->fingerDown = true;
                        st->haveCand = false;
                        st->lastSampleMs = nowMs;
                        // Begin at the first report and immediately extend to
                        // this one, so no travel is lost to the confirmation.
                        stroke_begin(st, gfx_fb, st->pendX, st->pendY, &dMinX, &dMinY, &dMaxX, &dMaxY);
                        st->lastRawX = x; st->lastRawY = y;
                        stroke_sample(st, gfx_fb, x, y, false, &dMinX, &dMinY, &dMaxX, &dMaxY);
#if TOUCH_POLL_SELFTEST
                        st->diagStrokeStarted++;
#endif
                        printf("stroke start (%d,%d) t=%lu (%s)\r\n",
                               st->pendX, st->pendY, (unsigned long)nowMs,
                               newReport ? "moved" : "persisted");
                    }
                    // else: still waiting - pendingStart stays armed, and the
                    // haveTouch==false branch below is what can still give up
                    // on it, after LIFT_DEBOUNCE_MS of no contact at all.
                }
            } else if (newReport) {
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
        } else if (!st->fingerDown) {
            if (st->pendingStart) {
                // Contact dropped while still confirming. The FT3168 does
                // this constantly, mid-stroke as much as mid-confirmation
                // (see dropouts, and CONFIRM_MS's comment for the measured
                // 798-dropout session that motivated this) - so a candidate
                // gets real grace instead of being thrown away the instant a
                // single zero-finger read arrives. Nothing was drawn yet
                // either way, which is what makes the wait free.
                //
                // PENDING_GRACE_MS, not LIFT_DEBOUNCE_MS: an unconfirmed
                // candidate and an already-drawing stroke used to share one
                // constant here, on the reasoning that they get "exactly the
                // same grace" for "the exact same phenomenon". They were
                // split 2026-08-14 (see PENDING_GRACE_MS_DEFAULT's own
                // comment) once LIFT_DEBOUNCE_MS grew for the mid-stroke
                // case and measurably raised how often a lone stray got
                // believed here too - a risk this branch has no MAX_JUMP_PX-
                // style backstop against, unlike the fingerDown branch below.
                uint32_t nowMs = smp.tMs;
                if (nowMs - st->pendLastTouchMs >= PENDING_GRACE_MS) {
                    st->pendingStart = false;
                    st->lastReportX = -1; st->lastReportY = -1;
                    st->strays++;
                }
                // else: within the grace window - keep pendingStart armed.
                // The next haveTouch sample (moved or merely persisted, per
                // the branch above) is what actually confirms it.
            }
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
#if TOUCH_POLL_SELFTEST
                st->diagStrokeEnded++;
#endif
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

// TEMPORARY diagnostic accessor - see sensors.h's sketch_touch_diag_t.
// `st` is arena-allocated (app.h) and NULL until sketch_enter() has run at
// least once this boot; reads as all-zero rather than dereferencing NULL in
// that case, same as before the sketchpad has ever been the current app.
void sketch_debug_touch_diag(sketch_touch_diag_t *out) {
#if TOUCH_POLL_SELFTEST
    if (st == NULL) {
        out->drained = out->haveTouch = out->newReport = out->pendingStart = 0;
        out->strokeStarted = out->strokeEnded = 0;
        out->glitches = out->dropouts = out->strays = out->splits = 0;
        return;
    }
    out->drained = st->diagDrained;
    out->haveTouch = st->diagHaveTouch;
    out->newReport = st->diagNewReport;
    out->pendingStart = st->diagPendingStart;
    out->strokeStarted = st->diagStrokeStarted;
    out->strokeEnded = st->diagStrokeEnded;
    out->glitches = st->glitches;
    out->dropouts = st->dropouts;
    out->strays = st->strays;
    out->splits = st->splits;
#else
    out->drained = out->haveTouch = out->newReport = out->pendingStart = 0;
    out->strokeStarted = out->strokeEnded = 0;
    out->glitches = out->dropouts = out->strays = out->splits = 0;
#endif
}
