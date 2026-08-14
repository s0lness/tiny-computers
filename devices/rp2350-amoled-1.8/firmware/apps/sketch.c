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
 * EXPERIMENTAL: a colour palette, opened by holding the pen still.
 *
 * The owner's ask, verbatim: "long touch in the same place should open up
 * colored squares, upon touching one it will draw in this colour" - tested
 * in the emulator only, judged before it goes anywhere near real glass or
 * this file's normally hardware-proven tuning. Everything below this
 * comment and the matching block further down (near wipe_erase) is that
 * feature; nothing above this line changed to make room for it.
 *
 * TIMING: has to coexist with CONFIRM_MS (40ms) and LIFT_DEBOUNCE_MS
 * (220ms) above, both of which already govern what "a stroke started"
 * means. A long press must fire well after a stroke would already have
 * been confirmed and drawing (otherwise it cannot tell a deliberate hold
 * apart from a child who paused for a beat before starting a line), and
 * well before someone would give up looking for it. LONG_PRESS_MS=550
 * lands in the same neighbourhood as the "long press" convention on every
 * touch platform a parent or this owner has ever used (historically
 * 500-600ms), which matters because there is no way to teach a child what
 * this gesture is other than the gesture itself: it has to fall inside
 * the duration people already discover by fidgeting. It is timed from
 * PENDING_START's own arming (pendStartMs - when the finger first touched
 * the glass), not from CONFIRM_MS's later stroke-start, so "long touch"
 * measures what a finger on the glass would actually call a long touch,
 * not an internal bookkeeping instant 40ms later.
 *
 * STILLNESS: HOLD_STILL_RADIUS_PX bounds how far the touch may wander and
 * still count as "the same place". Set well above ordinary resting jitter
 * (DEDUPE_PX=0.7px) so a finger that is merely trying to hold still is not
 * punished for it, and well below the 50-61px this file's own measurements
 * say a real stroke moves between consecutive raw reports - so a real
 * stroke breaks candidacy on its very first or second sample, while a
 * genuine hold survives comfortably inside it.
 *
 * UNDO, REWORKED. The hold itself draws (the initial dot stroke_begin
 * always draws, plus any sub-radius wobble) and that has to vanish once
 * the palette opens - decision 0002 4b and the owner's own requirement
 * that opening the palette must never mark the page still stand. What
 * changed is HOW it vanishes. This used to save a small pixel patch and
 * restore it (HOLD_UNDO_PATCH_PX, now gone); it now rolls back the
 * in-progress stroke from the stroke history instead (see the "Stroke
 * history" comment section further down, above sketch_state_t, and
 * open_palette()'s own comment) and repaints. One mechanism now undoes
 * both the tiny hold wobble and the much larger job below, instead of two.
 * ------------------------------------------------------------------- */
#define HOLD_STILL_RADIUS_PX 12.0f
#define LONG_PRESS_MS        550.0f

// How long a raw reading has to stay outside HOLD_STILL_RADIUS_PX, with no
// return, before it is believed as real movement rather than this panel's
// own noise - see holdOutsideSinceMs's own struct comment for the full
// reasoning and why per-sample distance (even filtered through the
// drawing pipeline's own accept test) cannot be trusted on its own here.
// Calibrated the same way LIFT_DEBOUNCE_MS was: measured, not guessed,
// against a touchsim profile tuned to this controller's own hardware
// reading (splits=10 in 40s of stationary contact - see
// emulator/wasm/tests/repro-touch-dropout-palette-open.ts). 150ms was
// proven at 97-100% open-reliability across repeated 30- and 50-trial
// runs, AND at 0/80 false opens across four 20-trial runs of a slow,
// real, continuous drag under the same full dropout+jitter severity
// (that file's own scenario C) - both directions of the trade this
// mechanism makes, measured, not assumed safe from one side alone.
//
// A FALSE LEAD, kept here rather than erased, because the mistake is
// worth not repeating: an early calibration run measured that same drag
// scenario false-opening 10-20% of the time and this comment used to
// report it as a genuine, inherent cost of tolerating this panel's own
// noise. It was not real - it was a stale timestamp in the TEST
// harness's own settle tick (nowMs=10 while the rest of the scenario
// counted from nowMs=1000), which made holdStartMs read as "550ms
// already elapsed" on the very first sample of certain trials,
// independent of anything sketch.c does. Fixed in the test file, not
// here; see repro-touch-dropout-palette-open.ts's own settle-tick
// comment. Left as a reminder that a discriminator change and its own
// test harness both need to be trusted before a measured number is
// reported as a property of the code under test.
#define HOLD_MOVE_GRACE_MS 150.0f

/* ---------------------------------------------------------------------
 * EXPERIMENTAL: the palette panel itself, REWORKED to a 3x3 grid that may
 * cover the whole panel.
 *
 * WHY A GRID, AND WHY FULL-SCREEN. The owner's second ask on this feature:
 * "peut-etre que long press montre 9 rectangles de couleur et relacher en
 * selectionne un" - nine, not four, and released rather than tapped again:
 * one continuous gesture, press-slide-release. A 3x3 grid is the obvious
 * reading of "9 rectangles" and it is also what makes the gesture work at
 * all: whatever cell is under the finger has to be shown as the one about
 * to be picked (a child drags onto it and sees it light up before letting
 * go, rather than aiming once and hoping), which only reads well if the
 * grid is roomy. The owner's own follow-up settled how roomy: "c'est ok si
 * les cases recouvrent tout l'ecran, on est dans un menu en selectionnant
 * une couleur, c'est pas tres grave" - big, unmissable, easy for a child to
 * hit while sliding, because this is a real full-screen menu, not a small
 * panel dodging the fingertip. So PALETTE_COLS/ROWS partition the ENTIRE
 * panel (palette_cell_bounds() below), not a block placed relative to the
 * touch point - there is no more placement math, no PALETTE_TOUCH_GAP_PX,
 * because there is no longer a "clear of the finger" problem to place
 * around.
 *
 * SHAPE. "9 rectangles" meets decision 0009 head-on - that decision's own
 * consequences section names "a palette's squares" as an example of where
 * the hard-edges rule bites. The owner's later word for the shape was
 * explicit: "i'd love for them to pop out like little balloons. they
 * should be rounded rectangles" - not squares, not circles: a generous
 * corner radius (PALETTE_CORNER_PX below, large enough to read as soft at
 * arm's length, not a square with filed corners), drawn by
 * draw_rounded_rect() further down with the same signed-distance-to-shape
 * -> coverage -> MIN-composite technique draw_capsule already uses for the
 * pen, generalised from "distance to a segment" to "distance to a rounded
 * box" (the standard SDF for one). No shapes.h primitive is a rounded
 * rectangle on its own (capsule is round-ENDED but straight-sided only
 * when both radii match and the shape is a stadium, disc has no straight
 * sides at all, between-curves cannot round anything per 0009), so this is
 * a small new float primitive local to this file rather than a misuse of
 * a landscape-mapped one (shapes.h's _land primitives rotate landscape
 * coordinates onto this portrait panel - sketch.c draws natively in panel
 * space, like its own draw_capsule already does, and staying in that space
 * avoids a rotation round-trip for no benefit).
 *
 * WHY A HIGHLIGHT AND NOT JUST OCCLUSION-DOESN'T-MATTER. The prompt that
 * started this feature already worked out that a colour sitting under the
 * finger is fine now (she is already moving when she chooses, so she sees
 * it before releasing) - but "sees it" only holds if the candidate cell is
 * visually distinct while she is over it. draw_rounded_rect can only make
 * a pixel darker (MIN composite, same reasoning as the pen's ink - see
 * draw_capsule's own header comment), so the candidate cannot be lightened
 * to stand out; it grows instead (PALETTE_CANDIDATE_GROW_PX), puffing
 * slightly into its own gap - a size change reads as clearly as a colour
 * change and costs no second ink tone. See palette_render_frame().
 *
 * ANIMATION. "Pop out like little balloons" - a scale-up from small to
 * full size with a slight overshoot before it settles (ease_out_back()),
 * not a flat ease-to-stop: the overshoot is most of what makes it read as
 * a pop rather than a grow. PALETTE_POP_MS=240 is long enough to actually
 * see happen, short enough that a child who already knows which colour she
 * wants (this menu opens hundreds of times) is not kept waiting.
 * PALETTE_STAGGER_MS gives each cell a small delay by its Chebyshev
 * distance from the centre cell (palette_stagger_rank()) - 0/1/2 rings, so
 * the nine arrive as a small ripple (centre, then the four edges, then the
 * four corners) rather than one flat block, for the cost of one extra
 * subtraction and multiply per cell. Hit-testing (palette_cell_contains(),
 * used by both the live candidate and the final pick) reads the FIXED
 * target grid, never the animation's current scale - a cell still growing
 * is already fully selectable, so a fast child's slide is never fighting
 * the animation for correctness, only for how it looks while it happens.
 *
 * SIZE. PALETTE_CELL_GAP_PX is the visible gap that keeps adjacent
 * balloons reading as separate shapes rather than one solid field, even
 * though together they span the whole panel (the raw grid cells from
 * palette_cell_bounds() tile PANEL_W x PANEL_H exactly with no remainder;
 * each balloon is then inset by half the gap on every side). That gap is
 * also this feature's only "over nothing" zone now - see palette_cell_
 * contains() and PALETTE_LIFT_GRACE_MS's own comment below for what lands
 * there.
 * ------------------------------------------------------------------- */
#define PALETTE_COLS              3
#define PALETTE_ROWS              3
#define PALETTE_COUNT             (PALETTE_COLS * PALETTE_ROWS)
#define PALETTE_CELL_GAP_PX       8
#define PALETTE_CORNER_PX        26.0f
#define PALETTE_CANDIDATE_GROW_PX 3.0f
#define PALETTE_POP_MS          240.0f
#define PALETTE_STAGGER_MS       20.0f

// Extra margin past every cell's own mathematically exact settle time
// (the last-starting cell's own delay + PALETTE_POP_MS) before palette_
// drain_sample() stops treating the palette as "still animating" and
// therefore stops calling palette_render_frame() on every sample. Without
// this, the very last rendered frame can land a hair before t=1.0 for the
// slowest cell (a stepped 15ms touch-sample clock does not necessarily
// land exactly on the animation's own boundary), leaving it a fraction of
// a percent undersized forever after - not visible as residue (palette_
// render_frame's own full-panel whiten, see its header comment, means
// nothing is ever left BEHIND), but still worth closing rather than
// depending on both clocks lining up by luck.
#define PALETTE_ANIM_SETTLE_MARGIN_MS 30.0f

// How long a lift may look like a dropout before the palette believes the
// finger genuinely left the glass and resolves the pick (or the cancel).
// Nothing is being drawn during this wait - a wrong guess here costs at
// worst a slightly late close, never a stray mark - so this does not need
// LIFT_DEBOUNCE_MS's own careful tuning; PENDING_GRACE_MS_DEFAULT's value
// (80ms) is reused as a reasonable, already-justified default for "how
// long this controller can drop out without meaning anything."
#define PALETTE_LIFT_GRACE_MS 80.0f

/* ---------------------------------------------------------------------
 * Anti-aliased capsule rasterizer. Pixel format helpers (px_to_gray,
 * gray_to_px) now live in gfx.h; see its header comment for why the green
 * channel doubles as an 8-bit ink value on this monochrome-in-practice panel.
 * ------------------------------------------------------------------- */

// EXPERIMENTAL, for the colour palette feature (see this file's header
// section above wipe_erase). Every existing call below still passes
// PX_BLACK, which makes this branch in draw_capsule's own inner loop a
// no-op that reproduces gray_to_px(ink) exactly - see that call site's own
// comment. This function is what runs instead once a colour is actually
// selected: it interpolates each RGB565 channel from the target colour
// (at ink==0, full coverage) toward white (at ink==255, no coverage),
// the same "coverage fades to background" idea gray_to_px already encodes
// for black, generalised off the green-channel-as-luminance trick that
// only works for a strictly monochrome pixel.
static uint16_t tint_to_px(uint8_t ink, uint16_t colorPxSwapped) {
    uint16_t c = px_swap(colorPxSwapped); // back to logical (unswapped) RGB565
    uint16_t r = (c >> 11) & 0x1F;
    uint16_t g = (c >> 5) & 0x3F;
    uint16_t b = c & 0x1F;
    uint16_t rr = (uint16_t)(r + ((31u - r) * (uint32_t)ink) / 255u);
    uint16_t gg = (uint16_t)(g + ((63u - g) * (uint32_t)ink) / 255u);
    uint16_t bb = (uint16_t)(b + ((31u - b) * (uint32_t)ink) / 255u);
    uint16_t v = (uint16_t)((rr << 11) | (gg << 5) | bb);
    return px_swap(v);
}

// Draws a round-capped capsule from a (radius r0) to b (radius r1) as a
// signed-distance-to-segment field, converted to per-pixel coverage.
// Composition is MIN (darkest wins), not alpha blending: consecutive
// stroke segments overlap heavily along their shared edge, and blending
// would re-darken that overlap on every segment, turning a smooth line
// into a visibly banded, pixelated one. MIN just unions the ink shapes.
//
// inkColor: EXPERIMENTAL (see this file's header section on the colour
// palette). PX_BLACK reproduces this function's original, hardware-proven
// behaviour exactly, unconditionally - see the branch inside the loop
// below. Every call site not touched by that feature still passes
// PX_BLACK.
static void draw_capsule(uint16_t *fb, float ax, float ay, float r0,
                          float bx, float by, float r1,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY,
                          uint16_t inkColor) {
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
            if (ink < cur) fb[idx] = (inkColor == PX_BLACK) ? gray_to_px(ink) : tint_to_px(ink, inkColor);
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
                                int *dMinX, int *dMinY, int *dMaxX, int *dMaxY,
                                uint16_t inkColor) {
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
        draw_capsule(fb, prevX, prevY, prevR, bx, by, br, dMinX, dMinY, dMaxX, dMaxY, inkColor);
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
 * Stroke history, REPLACING the palette's old saved-pixel patch.
 *
 * WHY THIS EXISTS. The palette's first version saved a copy of the pixels
 * under it (patch_save/patch_restore, now gone) and paid for that in the
 * arena: W*H*2 bytes. That worked at 4 squares in one row (~57KB of a
 * 64KB arena, see this file's own earlier build notes) and was already
 * being asked whether 9 would fit. Then the owner relaxed the palette's own
 * footprint - "c'est ok si les cases recouvrent tout l'ecran" - which does
 * not just make the arithmetic tighter, it makes the saved-patch approach
 * impossible outright: a full-panel copy is PANEL_W * PANEL_H * 2 bytes =
 * 368 * 448 * 2 = 329,728 bytes, ~322KB, against a 64KB arena and a main
 * framebuffer that already holds that same 322KB out of the RP2350's
 * 520KB of SRAM. There is no copy of the screen to be had, at any cell
 * count, ever, once the palette is allowed to cover it.
 *
 * THE ALTERNATIVE: keep what was drawn, not a copy of its pixels. Every
 * stroke this app draws is recorded here as it happens (stroke_pool_
 * append_point(), called from stroke_begin/stroke_sample - see their own
 * comments), and closing the palette (or rolling back an aborted hold, see
 * open_palette()) repaints the WHOLE canvas by replaying every stored
 * stroke, in original order, onto a fresh white panel
 * (sketch_repaint_from_history()). Ink is still MIN-composited
 * destructively on the live framebuffer during ordinary drawing (decision
 * 0002 section 2 is unchanged: nothing about how a stroke lands changed),
 * but the framebuffer is no longer the only record of what was drawn.
 *
 * A STORED POINT is exactly the (x, y, radius) draw_capsule/draw_quad_
 * midpoint were already called with, at full float precision, plus one
 * byte recording whether this point followed a bridged dropout - i.e. the
 * SAME inputs stroke_sample computes today, kept instead of only being
 * used once and discarded. Replaying them through the same drawing
 * functions, in the same order, with the same floats, is what makes the
 * repaint EXACT rather than approximate: draw_capsule and draw_quad_
 * midpoint are pure functions of their inputs and the framebuffer's
 * current state, so the same call sequence against the same starting
 * white panel produces the same bytes, unconditionally. What is NOT
 * stored is anything about touch timing, smoothing state, or pressure -
 * replay never re-runs the stroke state machine, only the geometry it
 * already decided on. See replay_stroke() for the exact reconstruction
 * (it mirrors stroke_begin/stroke_sample/stroke_end's own drawing calls
 * line for line, reading from stored points instead of live touch).
 *
 * BOUNDED, ON PURPOSE. STROKE_POOL_POINTS and STROKE_POOL_STROKES cap the
 * memory this can ever cost (see stroke_point_t/stroke_meta_t's own
 * comments for the exact per-slot cost and sketch_state_t's own comment
 * for the arena arithmetic this yields). A child scribbling for ten
 * minutes WILL fill either bound eventually. stroke_pool_evict_oldest()
 * is what happens then: the single oldest stroke is dropped and the pools
 * compacted, same policy either bound triggers. Silently corrupting the
 * drawing was rejected outright; a strict cap with a clear, boring
 * eviction rule (oldest first, always) was preferred over anything
 * cleverer, because "which stroke to drop" is not a decision this app
 * should be improvising per session. The one thing eviction cannot do is
 * evict the CURRENT stroke (there is nothing older to fall back to while
 * it is still the only one) - see stroke_pool_append_point()'s own
 * comment for what a single stroke longer than the whole pool does
 * instead (drawing continues live; recording for that one stroke simply
 * stops, so a later repaint would reproduce it truncated - documented,
 * not hidden, and reported alongside this file's build notes).
 *
 * UNDO FALLS OUT OF THIS, AND IS NOT BUILT. Dropping the most recent
 * stroke (strokeCount--; pointCount = strokes[strokeCount].startIdx) and
 * repainting is exactly what an undo button would do - the data structure
 * already supports it trivially. Nobody has asked for one, so none of
 * this adds a gesture, a button, or any code path that removes anything
 * other than the whole in-progress hold (open_palette's own rollback).
 * ------------------------------------------------------------------- */

// One accepted sample from a stroke: full-precision position and radius
// (never quantised - see this file's header comment above on why exact
// floats are what makes the repaint byte-identical, not just similar),
// plus whether it followed a bridged dropout (stroke_sample's own
// `bridge` parameter, threaded straight through so replay can tell a
// reconnection-straight-line segment from an ordinary curved one, exactly
// as the live path does). sizeof is 16 (three floats then one byte,
// padded to float's 4-byte alignment) - measured via this file's own
// static assertion further down, not assumed.
typedef struct {
    float x, y, r;
    uint8_t bridge;
} stroke_point_t;

// One stroke's slice of the shared point pool: where it starts, how many
// points it has, and the one colour it was drawn in (already px_swap()'d,
// same convention st->inkColor itself uses - see palette_color()'s own
// comment on why that swap happens at the call site rather than being
// baked into a table). uint16_t on all three: STROKE_POOL_POINTS is far
// below 65536, so a point index and a per-stroke count both fit with
// headroom to spare, and this halves the metadata cost a plain int would
// have cost. sizeof is 6, no padding (all three members share 2-byte
// alignment).
typedef struct {
    uint16_t startIdx;
    uint16_t count;
    uint16_t color;
} stroke_meta_t;

// Pool sizing. Chosen, then verified (not assumed) by this file's own
// static assertion on sizeof(sketch_state_t) further down and by
// sketch_enter()'s own diagnostic print of that same size - see this
// file's build notes for the actual measured numbers these two constants
// settled on.
#define STROKE_POOL_POINTS  3200
#define STROKE_POOL_STROKES  256

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
    uint32_t lastSampleMs;   // last ACCEPTED MOVE - the speed/jump-allowance clock only. See lastContactMs.
    // Last haveTouch sample seen at all while fingerDown, moved or not -
    // the genuine-lift clock. Found missing 2026-08-14: a real lift used
    // to be judged by `nowMs - lastSampleMs >= LIFT_DEBOUNCE_MS`, but
    // lastSampleMs only advances on newReport (an accepted MOVE), so a
    // finger held perfectly still - which is the palette's entire
    // gesture, HOLD_STILL_RADIUS_PX by construction - never refreshed it
    // past the first sample. On real hardware (34 dropout episodes/sec)
    // any dropout landing more than LIFT_DEBOUNCE_MS after the last
    // movement was believed as a genuine lift, even with contact reported
    // continuously right up to that one dropped sample - ending the
    // stroke, clearing holdCandidate, and forcing the whole confirm+hold
    // sequence to start over. Reproduced headless with TouchSim's
    // realistic-controller profile before this was diagnosed (see
    // emulator/wasm/tests/repro-touch-dropout-stroke-start.ts's own sibling
    // test for the hold gesture): a stationary 2-second hold produced 5-6
    // "stroke start"/"stroke end" pairs instead of one, and the palette
    // opened on well under half of trials despite LONG_PRESS_MS=550 being
    // comfortably inside the hold duration - never a lack of contact, only
    // a lack of MOVEMENT being mistaken for one. Split off exactly the way
    // pendStartMs/pendLastTouchMs already are for the not-yet-confirmed
    // case (see pendLastTouchMs's own comment) - this is the same
    // "persistence vs movement" distinction, just missing on the mid-
    // stroke side until now.
    uint32_t lastContactMs;
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

    // EXPERIMENTAL: the colour palette (see this file's header section
    // above wipe_erase for the full design). PX_BLACK is 0, so a freshly
    // entered app (arena zeroed, app.h) starts inking black - the device's
    // own default - with no explicit init needed.
    uint16_t inkColor;

    // Hold-candidacy: armed the instant a stroke is confirmed, cleared the
    // moment the touch moves past HOLD_STILL_RADIUS_PX or genuinely lifts.
    bool     holdCandidate;
    int      holdAnchorX, holdAnchorY; // the confirmed stroke's own start point
    uint32_t holdStartMs;              // pendStartMs at confirmation - see LONG_PRESS_MS's comment

    // holdOutsideSinceMs is what actually decides "moved, cancel the
    // candidacy" now - not a per-sample distance test. Found 2026-08-14,
    // second round: fixing the false-lift bug (lastContactMs) made the
    // palette open in the emulator, but the owner still could not open it
    // on the real board. A TOUCH_POLL_SELFTEST session with his finger held
    // deliberately still measured splits=10 in 40 seconds - splits, not
    // glitches, meaning the reported position was jumping hundreds of
    // pixels AND a second reading was landing near the wrong spot often
    // enough to get "confirmed" into a stroke_end/stroke_begin pair.
    //
    // The first attempt at a fix reused the drawing pipeline's own jump/
    // glitch/split classification (only trust a sample the pipeline judged
    // "smoothly accepted"), on the theory that a jitter excursion already
    // fails that test by construction. It does not, reliably: that
    // classification's own allowed-jump radius GROWS with time since the
    // last accepted sample (`MAX_SPEED_PX_PER_MS * dtMs`, up to
    // MAX_JUMP_PX), because it exists to let a stroke that reappears after
    // a real dropout jump back to wherever the finger actually travelled
    // to while it was gone. A held-still finger produces almost no
    // accepted samples at all (the true position never changes, so
    // newReport itself barely fires), which means dtMs keeps growing right
    // up until the moment a jitter sample finally arrives - at which point
    // the allowance has often grown large enough to accept a 100-250px
    // jitter excursion outright as "plausible travel". Caught by this
    // file's own repro test before it reached hardware a second time (see
    // emulator/wasm/tests/repro-touch-dropout-palette-open.ts): a single
    // accepted jitter sample was measured landing 130px from the anchor.
    //
    // What actually distinguishes a held finger from a real stroke is not
    // any single sample, confirmed or not - it is that a stroke travels
    // away and keeps going, while a held finger's reported position
    // wanders and comes BACK. So this is measured the same way
    // LIFT_DEBOUNCE_MS already measures "is this genuinely gone, or just a
    // blip": every haveTouch sample's RAW (x, y) - not filtered, not
    // accepted-only - is checked against the anchor. Inside
    // HOLD_STILL_RADIUS_PX, holdOutsideSinceMs resets to 0: the finger is
    // still where it was, whatever this one sample says. Outside it,
    // holdOutsideSinceMs starts timing (if it was not already) and
    // candidacy is only actually cancelled once the excursion has
    // PERSISTED, sample after sample with no return, for HOLD_MOVE_GRACE_MS
    // - long enough that a 1-3-report jitter episode (this controller's own
    // measured shape) reliably returns to the true position before the
    // grace period closes, short enough that a real stroke - which does not
    // return - is still recognised as one well within LONG_PRESS_MS's own
    // 550ms budget. See HOLD_MOVE_GRACE_MS's own comment for the calibrated
    // value and what it was measured against.
    //
    // This also survives a split for free, and for the same underlying
    // reason it survives noise: nothing about stroke_end()/stroke_begin()
    // touches holdOutsideSinceMs, and a split only ever fires on a sample
    // this same raw-distance check would already have flagged as "outside,
    // timing" rather than "confirmed moved" - the two mechanisms are
    // reading the same evidence, just with different patience.
    //
    // THE COST, stated rather than assumed: a real stroke's own movement
    // is no longer noticed instantly, but only after HOLD_MOVE_GRACE_MS of
    // sustained travel - a real but small delay, well inside the 550ms
    // budget. HOLD_STILL_RADIUS_PX (12px) is unchanged, and a real stroke
    // slower than that in 550ms already read as a hold before any of this -
    // not a new trade, the same one the original design made. Measured
    // directly, not assumed safe: a slow, real, continuous drag under the
    // full dropout+jitter severity false-opened the palette 0 times across
    // 80 trials (repro-touch-dropout-palette-open.ts's own scenario C) -
    // see HOLD_MOVE_GRACE_MS's own comment for why an EARLIER run of that
    // same measurement said otherwise, and was wrong about the code, not
    // about the number it printed.
    uint32_t holdOutsideSinceMs; // 0 = currently believed still; nonzero = timing an excursion

    // The palette panel, while showing. No more paletteX0/Y0 - the grid is
    // a fixed partition of the whole panel now (palette_cell_bounds()),
    // not a block placed relative to the touch point.
    bool     paletteOpen;
    bool     paletteHasTouch;          // the continuing touch is down right now
    int      paletteTouchX, paletteTouchY; // its last-seen raw position
    uint32_t paletteTouchSeenMs;           // wall-clock of that last sample
    uint32_t paletteAnimStartMs;           // when open_palette() fired - the pop-in's t=0
    int      paletteLastCandidate;         // -1: none; -2: nothing rendered yet (open_palette's own sentinel)

    // The stroke history (see this file's header section above this
    // struct for the full design). strokeCount/pointCount are the pools'
    // own bump-allocator cursors; stroke_pool_evict_oldest() is what
    // rewinds them when either pool fills.
    int           strokeCount, pointCount;
    stroke_meta_t strokes[STROKE_POOL_STROKES];
    stroke_point_t points[STROKE_POOL_POINTS];

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

// Build-time proof this still fits, rather than a comment claiming it
// does. A negative array size is a compile error on every target this
// file builds for (the pico-sdk board build and the wasm32-freestanding
// emulator build alike), so STROKE_POOL_POINTS/STROKES above cannot
// silently drift past the arena the way a comment-only budget could -
// see app.h's own note that an app failing to fit the arena is a
// build-time bug, not a runtime one. A plain array-size trick is used
// instead of _Static_assert: this file already avoids assuming more of
// the C standard than it needs (see sketch_tune_name_eq's own comment on
// why it hand-rolls strcmp), and this works under any C standard both
// targets compile as.
typedef char sketch_arena_budget_check[(sizeof(sketch_state_t) <= APP_ARENA_BYTES) ? 1 : -1];

static sketch_state_t *st;

/* ---------------------------------------------------------------------
 * Stroke pool management: eviction, starting a stroke, appending a point.
 * See sketch_state_t's own header comment (above the struct) for the
 * overall design; this is just the bookkeeping.
 * ------------------------------------------------------------------- */

// Drops the single oldest stroke and compacts both pools. Never evicts the
// last remaining stroke (strokeCount<=1): that stroke is also the current
// one (the only way strokeCount can be 1 while drawing is in progress),
// and there is nothing older to fall back to - see stroke_pool_append_
// point()'s own comment for what happens instead if that one stroke alone
// outgrows the whole point pool.
static void stroke_pool_evict_oldest(sketch_state_t *st) {
    if (st->strokeCount <= 1) return;
    int evicted = st->strokes[0].count;
    for (int i = 1; i < st->strokeCount; i++) st->strokes[i - 1] = st->strokes[i];
    st->strokeCount--;
    int remaining = st->pointCount - evicted;
    for (int i = 0; i < remaining; i++) st->points[i] = st->points[i + evicted];
    st->pointCount = remaining;
    for (int i = 0; i < st->strokeCount; i++) st->strokes[i].startIdx -= (uint16_t)evicted;
}

// Starts recording a new stroke: evicts oldest strokes until there is a
// free slot, then opens one with 0 points at the current write cursor.
// Called from stroke_begin(), so every confirmed stroke - including a
// long-press candidate that may still be rolled back, see open_palette()'s
// own comment - is recorded from its very first point.
static void stroke_pool_begin_stroke(sketch_state_t *st, uint16_t colorPxSwapped) {
    while (st->strokeCount >= STROKE_POOL_STROKES) stroke_pool_evict_oldest(st);
    st->strokes[st->strokeCount].startIdx = (uint16_t)st->pointCount;
    st->strokes[st->strokeCount].count = 0;
    st->strokes[st->strokeCount].color = colorPxSwapped;
    st->strokeCount++;
}

// Appends one accepted sample to the current (most recently begun) stroke.
// Evicts oldest strokes to make room if the point pool is full; if even
// that cannot free a slot (the current stroke, alone, has already grown to
// fill the entire pool - a single stroke longer than STROKE_POOL_POINTS,
// which is a lot of drawing without ever lifting), recording for THIS
// stroke simply stops here: live drawing to the framebuffer is completely
// unaffected (this pool is only ever read at replay time), but a later
// repaint would reproduce this one stroke truncated at whatever point
// recording stopped, rather than corrupting anything else. Returns false
// in that one case, purely so a caller could log it; nothing currently
// does, since it costs nothing to leave silent-but-safe here and loud
// there if it is ever seen in practice.
static bool stroke_pool_append_point(sketch_state_t *st, float x, float y, float r, bool bridge) {
    if (st->strokeCount == 0) return false;
    while (st->pointCount >= STROKE_POOL_POINTS && st->strokeCount > 1) stroke_pool_evict_oldest(st);
    if (st->pointCount >= STROKE_POOL_POINTS) return false;
    stroke_point_t *p = &st->points[st->pointCount];
    p->x = x; p->y = y; p->r = r; p->bridge = bridge ? 1 : 0;
    st->pointCount++;
    st->strokes[st->strokeCount - 1].count++;
    return true;
}

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
    draw_capsule(fb, st->sx, st->sy, st->radius, st->sx, st->sy, st->radius, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);

    // Record this stroke's very first point (see this file's header
    // comment on stroke_point_t/stroke_meta_t above sketch_state_t) - a
    // long-press that turns out to open the palette rolls this whole
    // stroke back, see open_palette()'s own comment.
    stroke_pool_begin_stroke(st, st->inkColor);
    stroke_pool_append_point(st, st->sx, st->sy, st->radius, false);
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
        draw_capsule(fb, prevX, prevY, prevR, st->sx, st->sy, r, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
        st->haveH0 = false; // don't curve the *next* segment across this gap either
    } else if (st->haveH0) {
        draw_quad_midpoint(fb, st->h0x, st->h0y, st->h0r, prevX, prevY, prevR, st->sx, st->sy, r,
                            dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
    } else {
        // First real sample of the stroke (or the one right after a bridge):
        // not enough history for a curve yet, so draw the plain straight
        // span, same as the pre-curve code always did.
        draw_capsule(fb, prevX, prevY, prevR, st->sx, st->sy, r, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
    }
    st->h0x = prevX; st->h0y = prevY; st->h0r = prevR;
    st->haveH0 = true;

    // Record this accepted sample - see this file's header comment on
    // stroke_point_t above sketch_state_t. Exactly the (x, y, r, bridge)
    // just drawn with, at full precision, so a later replay reproduces
    // this same draw_capsule/draw_quad_midpoint call bit for bit.
    stroke_pool_append_point(st, st->sx, st->sy, r, bridge);

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
        draw_capsule(fb, mx, my, mr, st->sx, st->sy, st->radius, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
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
        draw_capsule(fb, curX, curY, curR, nx, ny, nr, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
        curX = nx; curY = ny; curR = nr;
    }
}

/* ---------------------------------------------------------------------
 * Stroke replay - reconstructing a stroke from stored points instead of
 * live touch. See sketch_state_t's own header comment (above the struct)
 * for why this is what makes the palette's close byte-identical to the
 * saved-patch approach it replaced.
 * ------------------------------------------------------------------- */

// Redraws exactly one stored stroke, mirroring stroke_begin/stroke_sample/
// stroke_end's own drawing calls line for line - the SAME functions
// (draw_capsule, draw_quad_midpoint), the SAME order, the SAME float
// values (read back from the pool, never recomputed), so MIN compositing
// against a white panel lands on the same bytes it did live. What is
// deliberately NOT replayed is any of the stroke state machine above this
// comment (smoothing, pressure, dropout/jump handling): those decisions
// are already baked into the (x, y, r, bridge) values stored per point,
// and re-deriving them here would risk a second, subtly different path
// to the same picture rather than the same path twice.
static void replay_stroke(uint16_t *fb, const stroke_point_t *pts, int count, uint16_t colorPxSwapped,
                           int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (count <= 0) return;

    float prevX = pts[0].x, prevY = pts[0].y, prevR = pts[0].r;
    // stroke_begin()'s own opening dot: a zero-length capsule.
    draw_capsule(fb, prevX, prevY, prevR, prevX, prevY, prevR, dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);

    float h0x = 0.0f, h0y = 0.0f, h0r = 0.0f;
    bool haveH0 = false;
    float dirX = 0.0f, dirY = 0.0f;

    for (int i = 1; i < count; i++) {
        float nx = pts[i].x, ny = pts[i].y, r = pts[i].r;
        bool bridge = pts[i].bridge != 0;

        if (bridge) {
            draw_capsule(fb, prevX, prevY, prevR, nx, ny, r, dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);
            haveH0 = false;
        } else if (haveH0) {
            draw_quad_midpoint(fb, h0x, h0y, h0r, prevX, prevY, prevR, nx, ny, r,
                                dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);
        } else {
            draw_capsule(fb, prevX, prevY, prevR, nx, ny, r, dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);
        }
        h0x = prevX; h0y = prevY; h0r = prevR;
        haveH0 = true;

        float dist = sqrtf((nx - prevX) * (nx - prevX) + (ny - prevY) * (ny - prevY));
        if (dist > 0.0f) { dirX = (nx - prevX) / dist; dirY = (ny - prevY) / dist; }
        prevX = nx; prevY = ny; prevR = r;
    }

    // stroke_end()'s own closing half-segment and taper.
    if (haveH0) {
        float mx = (h0x + prevX) * 0.5f, my = (h0y + prevY) * 0.5f, mr = (h0r + prevR) * 0.5f;
        draw_capsule(fb, mx, my, mr, prevX, prevY, prevR, dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);
    }
    static const float scales[3] = {0.7f, 0.45f, 0.25f};
    // baseR fixed at the stroke's own final radius, matching stroke_end()'s
    // own `st->radius * scales[i]` exactly: each taper segment shrinks from
    // the SAME original radius, not from the previous segment's already-
    // shrunk one (that would compound the three scales instead of applying
    // them independently, and is a bug this replay had until it was caught
    // by this file's own byte-identical test failing on a real stroke's
    // tail - see the test's own header comment on why every tick is
    // checked, not just settled ones).
    float baseR = prevR;
    float curX = prevX, curY = prevY, curR = prevR;
    for (int i = 0; i < 3; i++) {
        float nx = curX + dirX * 1.2f, ny = curY + dirY * 1.2f, nr = baseR * scales[i];
        draw_capsule(fb, curX, curY, curR, nx, ny, nr, dMinX, dMinY, dMaxX, dMaxY, colorPxSwapped);
        curX = nx; curY = ny; curR = nr;
    }
}

// Whitens the whole panel and replays every stored stroke, oldest first -
// the only way back to the drawing now that the arena cannot hold a saved
// copy of it (see sketch_state_t's own header comment). Called once when
// the palette closes (a pick or a cancel, both need the canvas back - see
// palette_drain_sample()) - not per animation frame, which only touches
// the palette's own overlay while it is open (see palette_render_frame()).
static void sketch_repaint_from_history(sketch_state_t *st, uint16_t *fb) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;
    int dMinX = PANEL_W, dMinY = PANEL_H, dMaxX = -1, dMaxY = -1; // unused by the caller; replay needs the pointers
    for (int s = 0; s < st->strokeCount; s++) {
        stroke_meta_t *sm = &st->strokes[s];
        replay_stroke(fb, &st->points[sm->startIdx], sm->count, sm->color, &dMinX, &dMinY, &dMaxX, &dMaxY);
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
 * EXPERIMENTAL: the colour palette. See this file's header section above
 * the palette #defines (near HOLD_STILL_RADIUS_PX) for the full design:
 * the 3x3 full-panel grid, why rounded rectangles, the pop-in animation,
 * and why the palette's own undo/close now replay stroke history instead
 * of restoring a saved patch.
 * ------------------------------------------------------------------- */

// The nine colours, swapped for the panel's byte order the same way
// gfx.h's own PX_BLACK/PX_WHITE and arena_overflow_trap's alarm colour are
// - via px_swap(), a real function call, which is why these are not a
// static const array (a static initializer needs a compile-time constant
// expression and px_swap() is not guaranteed to fold into one). Index is
// row-major (index = row*PALETTE_COLS + col): black sits at index 4, the
// grid's own centre, as a recognisable "home" position - not functionally
// special any more (the grid no longer follows the touch point, so ANY
// cell can be the one under a long press), just a nice constant one for a
// child building a mental map of the menu.
static uint16_t palette_color(int index) {
    switch (index) {
        case 0:  return px_swap(0xF800); // red
        case 1:  return px_swap(0xFC60); // orange
        case 2:  return px_swap(0xFFE0); // yellow
        case 3:  return px_swap(0x07E0); // green
        case 4:  return PX_BLACK;        // centre: the device's own ink
        case 5:  return px_swap(0x001F); // blue
        case 6:  return px_swap(0x981F); // purple
        case 7:  return px_swap(0xFB56); // pink
        default: return px_swap(0x8A22); // brown (index 8)
    }
}

// The FIXED target rectangle (centre and half-extents) for grid cell
// (col, row), before any pop-in scale or candidate growth is applied. The
// raw cell (x0..x1, y0..y1) tiles the whole panel exactly (integer
// division distributed by the standard "i*N/D" partition, so the widths
// sum to PANEL_W and the heights to PANEL_H with no remainder pixel left
// over); the balloon actually drawn is inset by half the gap on every
// side, which is also this feature's only "over nothing" zone - see
// palette_cell_contains().
static void palette_cell_bounds(int col, int row, int *cx, int *cy, int *halfW, int *halfH) {
    int x0 = (PANEL_W * col) / PALETTE_COLS;
    int x1 = (PANEL_W * (col + 1)) / PALETTE_COLS;
    int y0 = (PANEL_H * row) / PALETTE_ROWS;
    int y1 = (PANEL_H * (row + 1)) / PALETTE_ROWS;
    *cx = (x0 + x1) / 2;
    *cy = (y0 + y1) / 2;
    *halfW = (x1 - x0 - PALETTE_CELL_GAP_PX) / 2;
    *halfH = (y1 - y0 - PALETTE_CELL_GAP_PX) / 2;
}

// Signed distance from (px,py) to a rounded rectangle centred at (cx,cy)
// with half-extents (halfW,halfH) and corner radius cornerR - negative
// inside, positive outside, zero at the visible edge. The standard SDF for
// a rounded box (Inigo Quilez's construction: shrink to the sharp inner
// box by cornerR, take the distance to THAT, then subtract cornerR back
// out), used both to anti-alias draw_rounded_rect()'s edge (same coverage
// = 0.5 - d idea draw_capsule already uses, generalised from "distance to
// a segment" to this) and, unscaled, to hit-test palette_cell_contains():
// the hit region is exactly the visible shape, corners included, so a
// touch near a rounded-off corner reads as "over nothing" the same way it
// LOOKS like nothing is there.
static float rounded_rect_sdf(float px, float py, float cx, float cy,
                               float halfW, float halfH, float cornerR) {
    float qx = fabsf(px - cx) - halfW + cornerR;
    float qy = fabsf(py - cy) - halfH + cornerR;
    float ax = qx > 0.0f ? qx : 0.0f;
    float ay = qy > 0.0f ? qy : 0.0f;
    float outside = sqrtf(ax * ax + ay * ay);
    float mxy = qx > qy ? qx : qy;
    float inside = mxy < 0.0f ? mxy : 0.0f;
    return inside + outside - cornerR;
}

// Fills a rounded rectangle by per-pixel coverage from rounded_rect_sdf(),
// MIN-composited exactly like draw_capsule (same reasoning: this device's
// ink is "darkest wins", never alpha blending - see draw_capsule's own
// header comment). This is decision 0009's float brush applied to a shape
// none of shapes.h's primitives cover on their own (see this file's
// palette header comment for why a local primitive, in panel space,
// rather than shapes.h's landscape-mapped ones).
static void draw_rounded_rect(uint16_t *fb, float cx, float cy, float halfW, float halfH, float cornerR,
                               uint16_t colorPx, int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (halfW <= 0.0f || halfH <= 0.0f) return;
    if (cornerR > halfW) cornerR = halfW;
    if (cornerR > halfH) cornerR = halfH;
    if (cornerR < 0.0f) cornerR = 0.0f;

    int minX = (int)floorf(cx - halfW - 1.0f), maxX = (int)ceilf(cx + halfW + 1.0f);
    int minY = (int)floorf(cy - halfH - 1.0f), maxY = (int)ceilf(cy + halfH + 1.0f);
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;
    if (minX > maxX || minY > maxY) return;

    for (int iy = minY; iy <= maxY; iy++) {
        float py = (float)iy + 0.5f;
        for (int ix = minX; ix <= maxX; ix++) {
            float px = (float)ix + 0.5f;
            float d = rounded_rect_sdf(px, py, cx, cy, halfW, halfH, cornerR);
            float coverage = 0.5f - d;
            if (coverage <= 0.0f) continue;
            if (coverage > 1.0f) coverage = 1.0f;
            uint8_t ink = (uint8_t)((1.0f - coverage) * 255.0f + 0.5f);

            int idx = iy * PANEL_W + ix;
            uint8_t cur = px_to_gray(fb[idx]);
            if (ink < cur) fb[idx] = (colorPx == PX_BLACK) ? gray_to_px(ink) : tint_to_px(ink, colorPx);
        }
    }

    if (minX < *dMinX) *dMinX = minX;
    if (minY < *dMinY) *dMinY = minY;
    if (maxX > *dMaxX) *dMaxX = maxX;
    if (maxY > *dMaxY) *dMaxY = maxY;
}

// Whether (px,py) is over cell `index`'s visible shape, at its BASE size -
// deliberately ignoring both the pop-in animation's current scale and the
// candidate's own grow: hit-testing always reads the fixed target grid
// (see this file's palette header comment on why a still-growing cell is
// already fully selectable), and using the grown size here would make an
// already-chosen candidate's hit region keep changing under it.
static bool palette_cell_contains(int index, int px, int py) {
    int col = index % PALETTE_COLS, row = index / PALETTE_COLS;
    int cx, cy, halfW, halfH;
    palette_cell_bounds(col, row, &cx, &cy, &halfW, &halfH);
    float d = rounded_rect_sdf((float)px + 0.5f, (float)py + 0.5f,
                                (float)cx, (float)cy, (float)halfW, (float)halfH, PALETTE_CORNER_PX);
    return d <= 0.0f;
}

// Chebyshev distance (in grid cells) from the centre cell (1,1) - the
// pop-in's stagger ring: 0 for the centre, 1 for the four edge-adjacent
// cells, 2 for the four corners. See this file's palette header comment
// for why a ripple rather than a flat simultaneous pop.
static int palette_stagger_rank(int col, int row) {
    int dc = col - 1; if (dc < 0) dc = -dc;
    int dr = row - 1; if (dr < 0) dr = -dr;
    return dc > dr ? dc : dr;
}

// Overshoot-then-settle easing ("back out"): grows past 1.0 before
// returning to exactly 1.0 at t=1, which is what makes a scale-up read as
// a pop rather than a plain grow (see this file's palette header comment).
// The standard closed-form cubic ("easeOutBack"), computed with plain
// multiplication rather than powf - this file already avoids reaching for
// extra libm surface when a couple of multiplies do the same job (compare
// ease_out_sine earlier in this file, which only needs sinf).
static float ease_out_back(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// Redraws all nine cells for the current instant: each cell's own pop-in
// progress (by its stagger rank) and, once a cell has finished popping,
// whether it is the current drag candidate (grown by PALETTE_CANDIDATE_
// GROW_PX - see this file's palette header comment on why growing, not
// lightening, is the highlight).
//
// THE WHOLE PANEL IS WHITENED FIRST, UNCONDITIONALLY, EVERY CALL - not a
// per-cell box. It used to be a per-cell box sized for the candidate grow
// alone (halfW + PALETTE_CANDIDATE_GROW_PX), which missed the OTHER thing
// that pushes a cell past its own base size: ease_out_back's own overshoot,
// which peaks at ~1.10x scale (a "little balloon" popping past full size
// before settling back to it - see this file's palette header comment on
// why the overshoot is the point). A cell 10% past its base halfW is wider
// than halfW + PALETTE_CANDIDATE_GROW_PX (3px) by several pixels at this
// grid's cell sizes, so that box was too small: the overshoot painted past
// its own edge, the NEXT frame's smaller box never reached back out that
// far to reclaim it, and the leftover ring sat in the gutter between cells
// forever, un-repainted once the animation settled and stopped redrawing
// (the owner's own report: "y a des espèces de traits qui traînent",
// visible between both rows and columns). Bounding the exact swept extent
// of nine independently staggered, independently overshooting shapes,
// precisely enough to whiten only that and no more, is exactly the kind
// of margin arithmetic decision 0001's own history (a push rectangle one
// column too narrow, cap pixels outside their band, a marker's trail) says
// not to trust. Whitening the whole panel is what a full-panel push
// already made free: it is not a bigger area than a correct tight bound
// would have been by more than a small constant factor, and it cannot be
// wrong regardless of what PALETTE_POP_MS, PALETTE_CANDIDATE_GROW_PX, or
// the easing curve are ever tuned to later.
static void palette_render_frame(sketch_state_t *st, uint16_t *fb, uint32_t nowMs, int candidateIdx,
                                  int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;

    for (int index = 0; index < PALETTE_COUNT; index++) {
        int col = index % PALETTE_COLS, row = index / PALETTE_COLS;
        int cx, cy, halfW, halfH;
        palette_cell_bounds(col, row, &cx, &cy, &halfW, &halfH);

        float elapsed = (float)(nowMs - st->paletteAnimStartMs);
        float delay = (float)palette_stagger_rank(col, row) * PALETTE_STAGGER_MS;
        float t = (elapsed - delay) / PALETTE_POP_MS;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float scale = ease_out_back(t);

        if (scale <= 0.0f) continue; // this cell's own stagger delay has not elapsed yet: stays blank

        float curHalfW = (float)halfW * scale, curHalfH = (float)halfH * scale;
        float curCorner = PALETTE_CORNER_PX * scale;
        if (t >= 1.0f && index == candidateIdx) {
            curHalfW += PALETTE_CANDIDATE_GROW_PX;
            curHalfH += PALETTE_CANDIDATE_GROW_PX;
        }
        draw_rounded_rect(fb, (float)cx, (float)cy, curHalfW, curHalfH, curCorner, palette_color(index),
                           dMinX, dMinY, dMaxX, dMaxY);
    }

    // Always push the whole panel while the palette is doing anything -
    // deliberately, not an oversight. An animation is many frames, each
    // one has to obey decision 0001's 8px rule and the "no pixel changes
    // outside the pushed rectangle" invariant, and getting a tight
    // per-frame union of nine independently animating, independently
    // growing rounded rects right, every single frame, is exactly the
    // kind of off-by-one this project has already been bitten by
    // separately in each of those three ways. The full panel is trivially
    // correct on both counts (368 is a multiple of 8; nothing this
    // feature ever draws while open is outside it) and, since the grid
    // already spans the whole panel by design, is not meaningfully more
    // data than a tight union would have been anyway.
    if (0 < *dMinX) *dMinX = 0;
    if (0 < *dMinY) *dMinY = 0;
    if (*dMaxX < PANEL_W - 1) *dMaxX = PANEL_W - 1;
    if (*dMaxY < PANEL_H - 1) *dMaxY = PANEL_H - 1;
}

// Fires once the hold-candidacy check in sketch_tick's drain loop decides
// LONG_PRESS_MS has genuinely elapsed within HOLD_STILL_RADIUS_PX. touchX/
// touchY is the confirmed stroke's own current position (the hold's
// anchor, or very close to it by construction - candidacy would have been
// cancelled otherwise).
static void open_palette(sketch_state_t *st, uint16_t *fb, int touchX, int touchY, uint32_t nowMs,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    // The hold itself was recorded into stroke history like any other
    // stroke (stroke_begin/stroke_sample append incrementally as they
    // draw - see sketch_state_t's own header comment). It turns out to
    // have been a long press, not a mark, so it is rolled back here: it
    // is always the CURRENT (most recent) stroke, because holdCandidate
    // only ever arms right after stroke_begin() started this very stroke.
    // This is the ENTIRE undo mechanism now - no saved patch, no separate
    // buffer, just forgetting the stroke that never really happened.
    if (st->strokeCount > 0) {
        st->pointCount = st->strokes[st->strokeCount - 1].startIdx;
        st->strokeCount--;
    }

    // The "stroke" this hold belonged to never happened, as far as the
    // canvas or the stroke state machine are concerned. haveCand/
    // pendingStart are already false here (holdCandidate only ever arms
    // once fingerDown is true), cleared anyway so nothing downstream has to
    // reason about why.
    st->fingerDown = false;
    st->pendingStart = false;
    st->haveCand = false;
    st->holdCandidate = false;
    st->haveH0 = false;

    // EXPERIMENTAL, per the owner: "c'est ok si les cases recouvrent tout
    // l'ecran, on est dans un menu ... c'est pas tres grave" - a real
    // full-screen menu, so hiding the drawing completely while it is open
    // is expected, not a bug to work around. Whited out directly (not
    // through wipe_erase()): that function's own banded, delayed push is
    // shake-to-erase's own deliberate animation, not what opening a menu
    // should look like.
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = 0xFFFF;

    st->paletteOpen = true;
    st->paletteHasTouch = true; // the finger that triggered this is still on the glass right now
    st->paletteTouchX = touchX;
    st->paletteTouchY = touchY;
    st->paletteTouchSeenMs = nowMs;
    st->paletteAnimStartMs = nowMs;

    // What happens if she never moves at all: whichever cell her long
    // press already landed on (or none, if she happened to hold exactly
    // in the gap between cells) is the candidate from frame one, so
    // releasing without moving picks that cell, exactly as if she had
    // dragged onto it.
    int initialCandidate = -1;
    for (int i = 0; i < PALETTE_COUNT; i++) {
        if (palette_cell_contains(i, touchX, touchY)) { initialCandidate = i; break; }
    }
    palette_render_frame(st, fb, nowMs, initialCandidate, dMinX, dMinY, dMaxX, dMaxY);
    st->paletteLastCandidate = initialCandidate;

    printf("palette: open at (%d,%d) candidate=%d\r\n", touchX, touchY, initialCandidate);
}

// Every drained sample while st->paletteOpen is true goes here instead of
// the ordinary stroke state machine - the touch that opened the palette is
// still down, and its only remaining job is to say which cell, if any, it
// lifts over, re-rendering the grid whenever the candidate changes (or the
// pop-in animation is still settling) so the highlight always matches
// what is actually under the finger.
static void palette_drain_sample(sketch_state_t *st, uint16_t *fb, bool haveTouch, int x, int y, uint32_t nowMs,
                                  int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (haveTouch) {
        st->paletteHasTouch = true;
        st->paletteTouchX = x;
        st->paletteTouchY = y;
        st->paletteTouchSeenMs = nowMs;

        int candidate = -1;
        for (int i = 0; i < PALETTE_COUNT; i++) {
            if (palette_cell_contains(i, x, y)) { candidate = i; break; }
        }

        uint32_t elapsed = nowMs - st->paletteAnimStartMs;
        bool animating = elapsed < (uint32_t)(PALETTE_POP_MS + 2.0f * PALETTE_STAGGER_MS + PALETTE_ANIM_SETTLE_MARGIN_MS);
        if (animating || candidate != st->paletteLastCandidate) {
            palette_render_frame(st, fb, nowMs, candidate, dMinX, dMinY, dMaxX, dMaxY);
            st->paletteLastCandidate = candidate;
        }
        return;
    }

    if (!st->paletteHasTouch) return; // no contact seen yet since the palette opened
    if (nowMs - st->paletteTouchSeenMs < PALETTE_LIFT_GRACE_MS) return; // could still be a dropout

    // Genuine lift: which cell (if any) is the last-seen position over?
    int chosen = -1;
    for (int i = 0; i < PALETTE_COUNT; i++) {
        if (palette_cell_contains(i, st->paletteTouchX, st->paletteTouchY)) { chosen = i; break; }
    }
    if (chosen >= 0) st->inkColor = palette_color(chosen);

    // Bring the real drawing back. There is no saved copy of the panel
    // any more (see sketch_state_t's own header comment on why one no
    // longer fits, or exists, once the palette is allowed to cover the
    // screen) - the only way back is replaying every stored stroke onto a
    // fresh white canvas, whether a colour was chosen or this is a cancel:
    // both close the palette the same way, just with inkColor already
    // changed above or not.
    sketch_repaint_from_history(st, fb);
    if (0 < *dMinX) *dMinX = 0;
    if (0 < *dMinY) *dMinY = 0;
    if (*dMaxX < PANEL_W - 1) *dMaxX = PANEL_W - 1;
    if (*dMaxY < PANEL_H - 1) *dMaxY = PANEL_H - 1;

    st->paletteOpen = false;
    st->paletteHasTouch = false;
    // Let a fresh touch afterwards start a genuinely clean stroke/hold
    // candidacy, exactly as if this whole gesture had never happened.
    st->fingerDown = false;
    st->pendingStart = false;
    st->haveCand = false;
    st->holdCandidate = false;
    st->lastReportX = -1; st->lastReportY = -1;

    printf("palette: %s\r\n", chosen >= 0 ? "picked" : "cancelled");
}

/* ---------------------------------------------------------------------
 * app_t callbacks.
 * ------------------------------------------------------------------- */

static void sketch_enter(void) {
    // APP_STATE zeroes: the arena was just reset, and app_alloc hands back
    // zeroed memory (see app.h). Nothing else to do: the framebuffer is
    // already white (the runtime's job), and the runtime pushes it, not us.
    st = APP_STATE(sketch_state_t);

    // The arena budget for this struct is checked at compile time
    // (sketch_arena_budget_check, above sketch_state_t's own definition),
    // but that only proves it fits, not by how much - this is the measured
    // number itself, the same way this feature's earlier saved-patch
    // design was reported as "~57KB of 64KB", not assumed.
    printf("sketch: state=%u bytes (arena %u)\r\n",
           (unsigned)sizeof(sketch_state_t), (unsigned)APP_ARENA_BYTES);
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

        // EXPERIMENTAL: while the colour palette is showing, every sample
        // belongs to it instead of the ordinary stroke machine below - see
        // palette_drain_sample's own comment.
        if (st->paletteOpen) {
            palette_drain_sample(st, gfx_fb, haveTouch, x, y, smp.tMs, &dMinX, &dMinY, &dMaxX, &dMaxY);
            continue;
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
                        st->lastContactMs = nowMs; // see lastContactMs's own comment

                        // EXPERIMENTAL: arm hold-candidacy for the colour
                        // palette right here, before anything is drawn - see
                        // this file's header section on the palette
                        // #defines (near HOLD_STILL_RADIUS_PX). No undo
                        // patch to capture any more: stroke_begin, called
                        // next, records this stroke into history like any
                        // other, and open_palette() rolls that same stroke
                        // back if this candidacy turns into a long press.
                        st->holdCandidate = true;
                        st->holdAnchorX = st->pendX;
                        st->holdAnchorY = st->pendY;
                        st->holdStartMs = st->pendStartMs;
                        st->holdOutsideSinceMs = 0; // freshly armed: believed still until proven otherwise

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

            // Contact heartbeat, not a move: refreshed on EVERY haveTouch
            // sample while a stroke is already down, whether or not this
            // particular sample moved - see lastContactMs's own struct
            // comment for the bug this fixes (a stationary hold used to be
            // judged lifted after LIFT_DEBOUNCE_MS of no MOVEMENT, not of
            // no CONTACT). lastSampleMs itself is untouched here - it keeps
            // its original, narrower job of dating the last accepted move
            // for the speed/jump-allowance calc above, which genuinely
            // needs "time since we last knew where the finger was going",
            // not "time since we last heard from it".
            if (st->fingerDown) st->lastContactMs = nowMs;

            // EXPERIMENTAL: hold-candidacy for the colour palette. Runs on
            // EVERY haveTouch sample, not only newReport ones - same reason
            // CONFIRM_MS's own persisted check above does: this controller
            // keeps repeating the same coordinate far more often than it
            // reports a new one, and a wall-clock hold has to keep being
            // measured through that repetition, not just at the rare moment
            // the position actually changes.
            if (st->fingerDown && st->holdCandidate) {
                // Raw (x, y), deliberately not filtered through the
                // drawing pipeline's own accept test - see
                // holdOutsideSinceMs's own struct comment for why that
                // filter cannot be trusted here (its allowance grows with
                // time since the last accepted sample, which is exactly
                // backwards for a finger that has been sitting still). The
                // noise tolerance lives entirely in HOLD_MOVE_GRACE_MS
                // below: a single far reading only starts a clock, it does
                // not by itself cancel anything.
                float ddx = (float)(x - st->holdAnchorX);
                float ddy = (float)(y - st->holdAnchorY);
                if (ddx * ddx + ddy * ddy <= HOLD_STILL_RADIUS_PX * HOLD_STILL_RADIUS_PX) {
                    // Back inside the radius: whatever excursion was being
                    // timed is over, and it did not last. This is what
                    // makes a brief jitter episode free - "wandered and
                    // came back", per holdOutsideSinceMs's own comment.
                    st->holdOutsideSinceMs = 0;
                } else {
                    if (st->holdOutsideSinceMs == 0) st->holdOutsideSinceMs = nowMs;
                    if (nowMs - st->holdOutsideSinceMs >= HOLD_MOVE_GRACE_MS) {
                        // Outside the radius for HOLD_MOVE_GRACE_MS straight,
                        // with no return: this is becoming a real stroke,
                        // not a long press. Whatever it has drawn so far
                        // stands - it is a legitimate stroke start, not
                        // something to undo.
                        st->holdCandidate = false;
                    }
                }
                if (st->holdCandidate && nowMs - st->holdStartMs >= LONG_PRESS_MS) {
                    // holdAnchorX/Y, not raw (x, y) or whatever this exact
                    // instant's reading happens to be: the anchor is the
                    // best estimate of where she actually is, precisely
                    // because a genuine hold - by construction, having
                    // survived every excursion above - never really left it.
                    open_palette(st, gfx_fb, st->holdAnchorX, st->holdAnchorY, nowMs, &dMinX, &dMinY, &dMaxX, &dMaxY);
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
            //
            // Measured against lastContactMs, NOT lastSampleMs: the latter
            // only dates the last MOVE, and a stroke that has been sitting
            // perfectly still (a held stroke's own last accepted position,
            // or the palette's hold gesture) would otherwise look exactly
            // like a lift the moment any single dropout landed more than
            // LIFT_DEBOUNCE_MS after the last movement - see lastContactMs's
            // own struct comment for how this was found.
            uint32_t nowMs = smp.tMs;
            if (nowMs - st->lastContactMs >= LIFT_DEBOUNCE_MS) {
                st->fingerDown = false;
                st->bridging = false;
                // Forget the last coordinates, so touching down again on the
                // exact same pixel still counts as a new report.
                st->lastReportX = -1; st->lastReportY = -1;
                // EXPERIMENTAL: a genuine lift ends this stroke normally -
                // whatever it drew stands, there is no long press to open.
                st->holdCandidate = false;
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
    // EXPERIMENTAL: || st->paletteOpen so a finger still resting on the
    // glass while choosing a colour keeps suppressing shake, same as an
    // ordinary in-progress stroke does - see palette_drain_sample's own
    // comment on why st->fingerDown alone reads false during that window.
    sensors_set_finger_down(st->fingerDown || (st->paletteOpen && st->paletteHasTouch));

    // Shake-to-erase: the runtime only delivers f->shaken (true for exactly
    // the tick an accepted shake lands on) because wantsShake is set below.
    //
    // EXPERIMENTAL: && !st->paletteOpen. A shake while the palette is
    // showing does nothing, rather than let the two full-panel repaints
    // (wipe_erase's own, and the palette's close-time replay from history)
    // race each other over what the canvas should look like once both are
    // done.
    if (f->shaken && !st->paletteOpen) {
        wipe_erase(gfx_fb);
        st->pressure = 0.5f;
        st->arcLen = 0.0f;
        st->radius = 0.0f;
        st->dirX = 0.0f;
        st->dirY = 0.0f;
        st->haveH0 = false;
        // EXPERIMENTAL: any in-progress hold-candidacy is now stale (the
        // erase just repainted that whole area white) - forget it rather
        // than risk it later rolling back a stroke that no longer matches
        // what is on screen.
        st->holdCandidate = false;
        // The erase also has to clear the stroke history, not just the
        // framebuffer: a later palette close repaints from history (see
        // sketch_repaint_from_history()), and an un-cleared history would
        // silently resurrect everything shake-to-erase just wiped.
        st->strokeCount = 0;
        st->pointCount = 0;
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
