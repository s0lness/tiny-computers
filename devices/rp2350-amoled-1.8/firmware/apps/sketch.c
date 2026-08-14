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
 * UNDO: whatever the hold itself draws before it is recognised as a long
 * press (the initial dot stroke_begin always draws, plus any sub-radius
 * wobble) has to vanish once the palette opens - see this file's header
 * comment on decision 0002 4b and the owner's own requirement that opening
 * the palette must never mark the page. Decision: a saved pixel patch, not
 * a repaint from history, because there IS no history to repaint from -
 * decision 0002 section 2 is explicit that ink is MIN-composited
 * destructively and "re-rasterising history is impossible by design" on
 * this app. HOLD_UNDO_PATCH_PX is sized to comfortably cover anything the
 * hold could have drawn while still a candidate: HOLD_STILL_RADIUS_PX (12)
 * plus the pen's own maximum radius (8, pressure_to_radius's ceiling) plus
 * a few pixels of slack for antialiasing bleed, doubled for both sides.
 * ------------------------------------------------------------------- */
#define HOLD_STILL_RADIUS_PX 12.0f
#define LONG_PRESS_MS        550.0f
#define HOLD_UNDO_PATCH_PX   48

/* ---------------------------------------------------------------------
 * EXPERIMENTAL: the palette panel itself.
 *
 * FEW COLOURS, ONE ROW. Four: the device's own black ink (the default,
 * always present) plus red, blue and yellow - the classic small set a
 * child recognises before she can read a word, and "few" per the owner's
 * own requirement. A single row, not a grid, because a row's bounding box
 * is short and wide (SQUARE tall, not much more) rather than tall and
 * wide, and this feature has to slide vertically clear of the touch point
 * - see palette placement further down. A short block always fits in
 * whichever half of the 448px-tall panel has more room; a squarer block
 * would not.
 *
 * SIZE: a fingertip on this panel is roughly 75px across (the owner's own
 * figure). PALETTE_SQUARE_PX=82 is comfortably past that, and the same
 * number set any smaller starts to threaten the 64KB arena budget this
 * feature draws its undo/restore patch out of (see patch_save/patch_
 * restore further down) - the memory cost of "comfortably bigger than a
 * fingertip" IS the patch's own W*H*2 bytes, and it is the dominant term
 * in this app's whole arena footprint. At 82px this feature's saved patch
 * is ~57KB against a 64KB budget: measured, not assumed - see this file's
 * own build/test notes for the arena-fits check.
 *
 * PLACEMENT: PALETTE_TOUCH_GAP_PX is how far the block's near edge sits
 * from the touch point, on whichever side (above or below) has more room -
 * see open_palette(). Needs to clear the fingertip's own footprint (roughly
 * half of ~75px = ~38px in either direction from the touch centre) with a
 * little margin, hence 50, not exactly 38.
 * ------------------------------------------------------------------- */
#define PALETTE_SQUARE_PX    82
#define PALETTE_GAP_PX        6
#define PALETTE_COUNT          4
#define PALETTE_W  (PALETTE_COUNT * PALETTE_SQUARE_PX + (PALETTE_COUNT - 1) * PALETTE_GAP_PX)
#define PALETTE_H  PALETTE_SQUARE_PX
#define PALETTE_TOUCH_GAP_PX  50

// How long a lift may look like a dropout before the palette believes the
// finger genuinely left the glass and resolves the pick (or the cancel).
// Nothing is being drawn during this wait - a wrong guess here costs at
// worst a slightly late close, never a stray mark - so this does not need
// LIFT_DEBOUNCE_MS's own careful tuning; PENDING_GRACE_MS_DEFAULT's value
// (80ms) is reused as a reasonable, already-justified default for "how
// long this controller can drop out without meaning anything."
#define PALETTE_LIFT_GRACE_MS 80.0f

// The saved-patch buffer both patch_save() call sites below share (the
// tiny hold-undo capture, then later the much larger palette backdrop) -
// see patch_save's own comment for why one buffer safely serves both.
#define PALETTE_PATCH_CAP (PALETTE_W * PALETTE_H)

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

    // The palette panel, while showing.
    bool     paletteOpen;
    int      paletteX0, paletteY0;     // panel-space top-left of the row
    bool     paletteHasTouch;          // the continuing touch is down right now
    int      paletteTouchX, paletteTouchY; // its last-seen raw position
    uint32_t paletteTouchSeenMs;           // wall-clock of that last sample

    // The one saved-pixel patch this feature needs, reused for two
    // different, never-simultaneous jobs - see patch_save()'s own comment.
    int      patchX0, patchY0, patchW, patchH; // patchW==0: nothing currently saved
    uint16_t patchBuf[PALETTE_PATCH_CAP];

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
    draw_capsule(fb, st->sx, st->sy, st->radius, st->sx, st->sy, st->radius, dMinX, dMinY, dMaxX, dMaxY, st->inkColor);
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
 * wipe_erase for the design (timing, stillness radius, why a saved patch
 * rather than a repaint from history, palette sizing and placement).
 * ------------------------------------------------------------------- */

// Copies a panel-space rectangle out of the framebuffer into st->patchBuf,
// clipped to the panel. ONE buffer, sized for the larger of this feature's
// two jobs (the palette backdrop, PALETTE_W*PALETTE_H - see PALETTE_PATCH_
// CAP), reused for the smaller one (HOLD_UNDO_PATCH_PX square) too: the two
// never overlap in time. The hold-undo patch is captured, and either
// restored (long press fires) or simply abandoned (the touch moved or
// lifted first - a real stroke's ink stays exactly as drawn, nothing to
// undo), before the palette's own patch_save ever runs; there is never a
// point where both are needed live at once, so a second buffer would only
// ever sit idle.
static void patch_save(sketch_state_t *st, uint16_t *fb, int x0, int y0, int w, int h) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > PANEL_W) w = PANEL_W - x0;
    if (y0 + h > PANEL_H) h = PANEL_H - y0;
    if (w <= 0 || h <= 0) { st->patchW = 0; st->patchH = 0; return; }

    st->patchX0 = x0; st->patchY0 = y0; st->patchW = w; st->patchH = h;
    for (int j = 0; j < h; j++) {
        const uint16_t *src = fb + (size_t)(y0 + j) * PANEL_W + x0;
        uint16_t *dst = st->patchBuf + (size_t)j * w;
        for (int i = 0; i < w; i++) dst[i] = src[i];
    }
}

// Writes the saved patch back and folds its rectangle into the tick's own
// dirty-rect accumulator, so the caller's own gfx_push (sketch_tick's, at
// the end of its drain loop) covers the restore along with everything
// else that tick touched - the same accumulator draw_capsule already
// widens on every call, nothing new here. Marks the patch consumed
// (patchW=0) as a defensive guard against restoring it twice.
static void patch_restore(sketch_state_t *st, uint16_t *fb,
                           int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    int x0 = st->patchX0, y0 = st->patchY0, w = st->patchW, h = st->patchH;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *dst = fb + (size_t)(y0 + j) * PANEL_W + x0;
        const uint16_t *src = st->patchBuf + (size_t)j * w;
        for (int i = 0; i < w; i++) dst[i] = src[i];
    }
    if (x0 < *dMinX) *dMinX = x0;
    if (y0 < *dMinY) *dMinY = y0;
    if (x0 + w - 1 > *dMaxX) *dMaxX = x0 + w - 1;
    if (y0 + h - 1 > *dMaxY) *dMaxY = y0 + h - 1;
    st->patchW = 0;
}

// The palette's four colours, swapped for the panel's byte order the same
// way gfx.h's own PX_BLACK/PX_WHITE and arena_overflow_trap's alarm colour
// are - via px_swap(), a real function call, which is why these are not a
// static const array: a static initializer needs a compile-time constant
// expression and px_swap() is not guaranteed to fold into one, so this is
// computed fresh (cheap: four branches) wherever a colour is needed instead
// of risking hand-computed swapped hex constants going stale or wrong.
static uint16_t palette_color(int index) {
    switch (index) {
        case 0:  return PX_BLACK;         // the device's own ink, always first
        case 1:  return px_swap(0xF800);  // red
        case 2:  return px_swap(0x001F);  // blue
        default: return px_swap(0xFFE0);  // yellow
    }
}

// Fires once the hold-candidacy check in sketch_tick's drain loop decides
// LONG_PRESS_MS has genuinely elapsed within HOLD_STILL_RADIUS_PX. touchX/
// touchY is the confirmed stroke's own current position (the hold's
// anchor, or very close to it by construction - candidacy would have been
// cancelled otherwise).
static void open_palette(sketch_state_t *st, uint16_t *fb, int touchX, int touchY, uint32_t nowMs,
                          int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    // Undo whatever the hold itself drew (see HOLD_UNDO_PATCH_PX's own
    // comment) before anything else happens - the palette opening must
    // never leave a mark of its own.
    patch_restore(st, fb, dMinX, dMinY, dMaxX, dMaxY);

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

    // Placement: horizontally centred always (the row is nearly the panel's
    // full width, so a touch anywhere along X still sits well inside it -
    // see PALETTE_W's own comment on why a row, not a grid). Vertically,
    // PALETTE_TOUCH_GAP_PX clear of the touch point, on whichever side
    // (above/below) has more room; a tie goes below.
    int x0 = (PANEL_W - PALETTE_W) / 2;
    int spaceAbove = touchY;
    int spaceBelow = (PANEL_H - 1) - touchY;
    int y0;
    if (spaceBelow >= spaceAbove) {
        y0 = touchY + PALETTE_TOUCH_GAP_PX;
        if (y0 + PALETTE_H - 1 > PANEL_H - 1) y0 = PANEL_H - PALETTE_H;
    } else {
        y0 = touchY - PALETTE_TOUCH_GAP_PX - PALETTE_H;
        if (y0 < 0) y0 = 0;
    }

    patch_save(st, fb, x0, y0, PALETTE_W, PALETTE_H);

    // A white backdrop first, so the row reads as one clean panel rather
    // than showing whatever artwork used to be under it through the gaps
    // between squares; then the four flat colour fills - "aplats de
    // couleur", the same flat-fill idiom the rest of this device's UI
    // (chrono's digits, the timer's track) already uses for anything that
    // is not the pen's own anti-aliased ink.
    gfx_fill_rect(x0, y0, PALETTE_W, PALETTE_H, PX_WHITE);
    for (int col = 0; col < PALETTE_COUNT; col++) {
        int px = x0 + col * (PALETTE_SQUARE_PX + PALETTE_GAP_PX);
        gfx_fill_rect(px, y0, PALETTE_SQUARE_PX, PALETTE_SQUARE_PX, palette_color(col));
    }

    if (x0 < *dMinX) *dMinX = x0;
    if (y0 < *dMinY) *dMinY = y0;
    if (x0 + PALETTE_W - 1 > *dMaxX) *dMaxX = x0 + PALETTE_W - 1;
    if (y0 + PALETTE_H - 1 > *dMaxY) *dMaxY = y0 + PALETTE_H - 1;

    st->paletteOpen = true;
    st->paletteX0 = x0;
    st->paletteY0 = y0;
    st->paletteHasTouch = true; // the finger that triggered this is still on the glass right now
    st->paletteTouchX = touchX;
    st->paletteTouchY = touchY;
    st->paletteTouchSeenMs = nowMs;

    printf("palette: open at (%d,%d)\r\n", x0, y0);
}

// Every drained sample while st->paletteOpen is true goes here instead of
// the ordinary stroke state machine - the touch that opened the palette is
// still down, and its only remaining job is to say which square, if any,
// it lifts over. Nothing is drawn to the canvas by this function; it only
// tracks a position and, on a genuine lift, resolves the pick and closes
// the panel.
static void palette_drain_sample(sketch_state_t *st, uint16_t *fb, bool haveTouch, int x, int y, uint32_t nowMs,
                                  int *dMinX, int *dMinY, int *dMaxX, int *dMaxY) {
    if (haveTouch) {
        st->paletteHasTouch = true;
        st->paletteTouchX = x;
        st->paletteTouchY = y;
        st->paletteTouchSeenMs = nowMs;
        return;
    }

    if (!st->paletteHasTouch) return; // no contact seen yet since the palette opened
    if (nowMs - st->paletteTouchSeenMs < PALETTE_LIFT_GRACE_MS) return; // could still be a dropout

    // Genuine lift: which square (if any) is the last-seen position over?
    int chosen = -1;
    for (int col = 0; col < PALETTE_COUNT; col++) {
        int px = st->paletteX0 + col * (PALETTE_SQUARE_PX + PALETTE_GAP_PX);
        int py = st->paletteY0;
        if (st->paletteTouchX >= px && st->paletteTouchX < px + PALETTE_SQUARE_PX &&
            st->paletteTouchY >= py && st->paletteTouchY < py + PALETTE_SQUARE_PX) {
            chosen = col;
            break;
        }
    }
    if (chosen >= 0) st->inkColor = palette_color(chosen);

    // Undo the panel itself, whether or not a square was chosen - lifting
    // outside every square is the cancel gesture, and picking one closes
    // the palette exactly the same way, just with inkColor already changed
    // above.
    patch_restore(st, fb, dMinX, dMinY, dMaxX, dMaxY);

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

                        // EXPERIMENTAL: arm hold-candidacy for the colour
                        // palette right here, before anything is drawn - see
                        // this file's header section above wipe_erase.
                        // Captures the small undo patch first (stroke_begin,
                        // called next, is the first thing that could mark
                        // it), anchored and timed from this confirmed start.
                        patch_save(st, gfx_fb, st->pendX - HOLD_UNDO_PATCH_PX / 2, st->pendY - HOLD_UNDO_PATCH_PX / 2,
                                   HOLD_UNDO_PATCH_PX, HOLD_UNDO_PATCH_PX);
                        st->holdCandidate = true;
                        st->holdAnchorX = st->pendX;
                        st->holdAnchorY = st->pendY;
                        st->holdStartMs = st->pendStartMs;

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

            // EXPERIMENTAL: hold-candidacy for the colour palette. Runs on
            // EVERY haveTouch sample, not only newReport ones - same reason
            // CONFIRM_MS's own persisted check above does: this controller
            // keeps repeating the same coordinate far more often than it
            // reports a new one, and a wall-clock hold has to keep being
            // measured through that repetition, not just at the rare moment
            // the position actually changes.
            if (st->fingerDown && st->holdCandidate) {
                float ddx = (float)(x - st->holdAnchorX);
                float ddy = (float)(y - st->holdAnchorY);
                if (ddx * ddx + ddy * ddy > HOLD_STILL_RADIUS_PX * HOLD_STILL_RADIUS_PX) {
                    // Moved: this is becoming a real stroke, not a long
                    // press. Whatever it has drawn so far stands - it is a
                    // legitimate stroke start, not something to undo.
                    st->holdCandidate = false;
                } else if (nowMs - st->holdStartMs >= LONG_PRESS_MS) {
                    open_palette(st, gfx_fb, x, y, nowMs, &dMinX, &dMinY, &dMaxX, &dMaxY);
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
    // EXPERIMENTAL: && !st->paletteOpen. wipe_erase() repaints the whole
    // panel unconditionally, which would blow away the palette this tick
    // just drew while patchBuf still holds ITS OWN pre-open backdrop save -
    // closing the palette afterwards would then restore stale, pre-erase
    // pixels on top of the freshly wiped page. Simplest safe answer for a
    // rare, transient overlap: a shake while the palette is showing does
    // nothing, rather than risk that residue.
    if (f->shaken && !st->paletteOpen) {
        wipe_erase(gfx_fb);
        st->pressure = 0.5f;
        st->arcLen = 0.0f;
        st->radius = 0.0f;
        st->dirX = 0.0f;
        st->dirY = 0.0f;
        st->haveH0 = false;
        // EXPERIMENTAL: any in-progress hold-candidacy's saved undo patch
        // is now stale (the erase just repainted that whole area white) -
        // forget the candidacy rather than risk reapplying pre-erase pixels
        // if it later fires.
        st->holdCandidate = false;
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
