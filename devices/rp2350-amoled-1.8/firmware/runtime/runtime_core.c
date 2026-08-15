/*
 * runtime_core: implementation. See runtime_core.h for what this file is and
 * why it exists. This is a move from runtime.c, not a rewrite: the arena,
 * the app table, do_switch(), the deferred switch and the per-frame input
 * resolution below are that file's, unchanged in behaviour on the board.
 * The only real edits are the three spots where the original code called a
 * pico-sdk function this file is not allowed to include - each is called
 * out where it happens.
 *
 * Depends on ONLY app.h, gfx.h, sensors.h and freestanding C headers. No
 * pico-sdk, nothing under hardware/ or pico/. See runtime_core.h's header comment.
 */
#include "runtime_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"
#include "sensors.h"

/* ---- the app table -------------------------------------------------------
 *
 * One extern per app, exactly as app.h's header comment describes: adding
 * an app is one entry here plus one file. g_menuApp and menu_set_return_app
 * are declared directly here (the same extern-per-app pattern) rather than
 * by including apps/menu.h, which this file may not: menu.h only pulls in
 * app.h itself, but the include is still outside the {app.h, gfx.h,
 * sensors.h} allowance runtime_core.h documents, and there is nothing menu.h
 * would add beyond what a plain extern already gives.
 */
extern const app_t g_chronoApp;
extern const app_t g_sketchApp;
extern const app_t g_timerApp;
extern const app_t g_fourApp;
extern const app_t g_levelApp;
extern const app_t g_clockApp;
extern const app_t g_menuApp;
extern void menu_set_return_app(int index);

/* SCREENSHOT FIXTURE, compiled out by default. See apps/stubapps.c for the
 * whole argument; in one line: the menu's layout has to be judged at six and
 * twelve apps and this firmware declares five, so the emulator build can be
 * told to append do-nothing apps and the capture then comes out of the real
 * menu_enter() instead of a script re-implementing it. Declared with a bare
 * extern, the same way g_menuApp above is, because runtime_core.h's contract
 * is that this file includes nothing from apps/.
 *
 * MENU_STUB_APPS is the TOTAL app count wanted (5 real + the rest stubs), so
 * MENU_STUB_APPS=12 appends seven. Spelled out one #if per entry rather than
 * generated with a macro: a static initialiser list is exactly the place
 * where clever expansion stops being readable, and seven lines that say what
 * they do cost nothing. */
#if MENU_STUB_APPS
extern const app_t g_stubApps[];
#endif

// Appended, not inserted: index 0 is what boots (app.h) and every emulator
// test in emulator/wasm/tests/ addresses apps by their index in this array
// (APP_DRAW = 1 and so on), so a new app goes on the end. The bubble level
// (firmware/apps/level.c) and the clock (firmware/apps/clock.c) read
// app_frame_t.tilt like every other orientation-aware app, so there is no
// reason for either to sit outside this table behind a flag or a private
// index - see AGENTS.md's "The bubble level" section for the two blockers
// that used to justify APPS_INCLUDE_LEVEL, and runtime_core.h's git history
// for APP_INDEX_CLOCK, the clock's own now-removed equivalent (it existed
// only because appending to this table used to move every menu column;
// decision 0013's grid replaced that layout, so the reason is gone).
const app_t *const g_apps[] = {
    &g_chronoApp, &g_sketchApp, &g_timerApp, &g_fourApp, &g_levelApp, &g_clockApp,
#if MENU_STUB_APPS > 6
    &g_stubApps[0],
#endif
#if MENU_STUB_APPS > 7
    &g_stubApps[1],
#endif
#if MENU_STUB_APPS > 8
    &g_stubApps[2],
#endif
#if MENU_STUB_APPS > 9
    &g_stubApps[3],
#endif
#if MENU_STUB_APPS > 10
    &g_stubApps[4],
#endif
#if MENU_STUB_APPS > 11
    &g_stubApps[5],
#endif
};
const int g_appCount = sizeof(g_apps) / sizeof(g_apps[0]);

// Startup-only sentinel: "nothing has been entered yet", so do_switch()'s
// first-ever call has no outgoing app to call leave() on. Never observed by
// app_current() once rtcore_tick() has run at least once.
#define APP_INDEX_NONE (-2)

static const app_t *app_for_index(int index) {
    if (index == APP_INDEX_MENU) return &g_menuApp;
    return g_apps[index];
}

/* ---- tiny text formatting, because this file has no stdio.h --------------
 *
 * A freestanding wasm32 build links with -nostdlib and no libc, so there is
 * no snprintf to call (see emulator/wasm/build.ts's notes on which C headers
 * actually exist for that target). The three rt_log() call sites below are
 * the only formatted diagnostics in this file; these are just enough for
 * "name (N us)", "requested N bytes, M of K used" and the power-off
 * gesture's "PWR held N ms alone", not a general formatter.
 */
static char *fmt_append_str(char *out, const char *s) {
    while (*s) *out++ = *s++;
    return out;
}

static char *fmt_append_u32(char *out, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) *out++ = tmp[--n];
    return out;
}

/* ---- the arena -------------------------------------------------------
 *
 * See app.h's header comment for why this exists at all. One static byte
 * array, bump-allocated, reset to empty on every switch.
 */
static uint8_t g_arena[APP_ARENA_BYTES] __attribute__((aligned(8)));
static size_t g_arenaUsed = 0;

// app.h's comment on app_alloc() says it "returns NULL" on overflow; that is
// the description of a well-behaved allocator, and this is deliberately not
// one. A NULL bump-allocator return handed to APP_STATE()'s caller (which
// dereferences it immediately, unchecked, in every app written against this
// header) is not an error path, it is a hard fault a few instructions later
// with no clue on screen what happened. An arena overflow can only happen by
// shipping an app whose state does not fit the 64KB budget: a build-time
// bug, never a runtime condition a real user's session can trigger. So it is
// treated like one: loud, and impossible to miss.
//
// The original (pre-split) version of this function painted the panel red
// and looped calling watchdog_update() + sleep_ms(500) itself. Both are
// pico-sdk calls (hardware/watchdog.h, pico/time.h) this file may not
// include, so painting the panel stays here (gfx.h is allowed) but holding
// the board there, fed, forever is handed to rt_halt() - see runtime_core.h
// for why, and why it must never return.
static void arena_overflow_trap(size_t requested) {
    char msg[96];
    char *p = msg;
    p = fmt_append_str(p, "FATAL: app arena overflow: requested ");
    p = fmt_append_u32(p, (uint32_t)requested);
    p = fmt_append_str(p, " bytes, ");
    p = fmt_append_u32(p, (uint32_t)g_arenaUsed);
    p = fmt_append_str(p, " of ");
    p = fmt_append_u32(p, (uint32_t)APP_ARENA_BYTES);
    p = fmt_append_str(p, " already used");
    *p = '\0';
    rt_log(msg);

    uint16_t alarm = px_swap(0xF800); // pure red, swapped for the panel's byte order (see gfx.h)
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, alarm);
    gfx_push_all();

    rt_halt(); // must not return
    for (;;) { } // unreachable if the host contract above is honoured; a
                 // safety net, not the trap itself.
}

void *app_alloc(size_t bytes) {
    size_t aligned = (g_arenaUsed + 7u) & ~(size_t)7u;
    if (bytes > APP_ARENA_BYTES || aligned > APP_ARENA_BYTES - bytes) {
        arena_overflow_trap(bytes);
    }
    void *p = &g_arena[aligned];
    g_arenaUsed = aligned + bytes;

    // Zero only the bytes just handed out, not the whole 64KB arena. app.h
    // promises app_alloc() returns zeroed memory, and chrono.c, timer.c and
    // sketch.c all lean on that explicitly in their enter() comments ("APP_
    // STATE zeroes", "lastDigit[i] is already 0"). do_switch() below only
    // ever rewound g_arenaUsed to 0, which resets what counts as allocated
    // without touching a single byte of the arena itself - so whatever the
    // PREVIOUS app left behind at these addresses was hand-delivered to the
    // NEXT app as its "fresh" state. That is exactly the bug reported
    // against the timer: leaving it and coming back resumed the last app's
    // paused countdown instead of showing a clean SETTING screen, and the
    // shape of the corruption depends on whichever app happened to run
    // before, since it is really just reading that app's old stack-shaped
    // bytes.
    //
    // Zeroed HERE, in app_alloc(), rather than by memset-ing the whole arena
    // in do_switch() on every switch: a switch is meant to complete inside
    // one frame (see app.h's cold/warm switch numbers), and every app
    // together allocates a tiny fraction of the 64KB budget, so clearing the
    // full arena every time would spend real time zeroing thousands of bytes
    // nothing is about to read. This loop only ever touches what was just
    // handed out.
    uint8_t *bytesOut = (uint8_t *)p;
    for (size_t i = 0; i < bytes; i++) bytesOut[i] = 0;

    return p;
}

/* ---- the shared touch resolver's state, reset on every switch -----------
 *
 * wasDown/lastX/lastY used to be function-local statics inside
 * rtcore_tick()'s touch-resolve block below. That made them shared by every
 * app that ever runs, with nothing to reset them: an app switch clears the
 * arena and the framebuffer, but these three survived, unbounded, in a
 * stack frame's static storage that no switch touched at all.
 *
 * The observed failure: the menu launches an app by TAP, so the finger is
 * still down at the instant of the switch. wasDown is left true. If the app
 * that runs next is the sketchpad (which reads sensors_touch_next() raw and
 * never looks at wasDown at all), nothing here even runs for as long as it
 * is current, so wasDown just sits at true. The next time a NON-sketchpad
 * app becomes current - most concretely, the menu itself, reopened later -
 * its very first touchPressed is computed against that stale true and never
 * fires: down && !wasDown is false because wasDown was never false to begin
 * with. A tap that should have launched a tile is silently swallowed, and
 * because nothing on screen changes, this is discovered as "reentering the
 * menu just doesn't work any more" rather than as a missed tap. That
 * reproduced empirically (emulator/wasm/tests/repro-switch-input.ts, run
 * against this file before the fix below) and is what touch_resolver_reset()
 * exists to close: general principle, not a one-off patch - input state that
 * belongs to a run of an app must not outlive that run, exactly like the
 * arena.
 *
 * THE DECISION this also settles, because the fix has to pick an answer:
 * when an app is launched by a tap, does the newly entered app see that
 * still-down finger, or does it start blind to it until a fresh lift and
 * press? Reset seeds wasDown false, not true. A finger that is still down
 * when the new app's first tick runs will, on the very next drained sample,
 * read as down && !wasDown(false) = a genuine touchPressed for the NEW app -
 * the tap is treated as one continuous physical gesture that started the
 * old app's job (launching) and continues into the new app's, not two
 * gestures separated by an invisible, undiscoverable "lift and touch again"
 * requirement nothing on screen would ever hint at. That matches the whole
 * menu's own design (menu.c: "tapping a tile is the primary input and it
 * launches immediately", no modal confirmation step) and costs nothing extra
 * to implement: false is also the only value knowable for free at switch
 * time without adding new plumbing, since the alternative (seed to whatever
 * is physically true right now) needs a way to peek the sensor state
 * without consuming it, which raw-touch apps also depend on being untouched.
 * No shipped app currently reads touchPressed on the very tick it is
 * entered (chrono and the sketchpad do not read it at all; timer only reads
 * the level, touchDown, which was never affected by wasDown either way), so
 * this is a policy for the future, not a behaviour change measurable today -
 * but it is the one documented here, on purpose, so the next app author
 * does not have to re-derive it.
 */
static bool g_touchWasDown = false;
static int g_touchLastX = 0, g_touchLastY = 0;

static void touch_resolver_reset(void) {
    g_touchWasDown = false;
    g_touchLastX = 0;
    g_touchLastY = 0;
}

/* ---- switching -----------------------------------------------------------
 *
 * app_switch_to() only ever records a request. Both the menu gesture
 * handling below and an app's tick() (the menu tapping a tile) call it, and
 * neither is a safe place to tear down the currently-running app: the
 * caller might be that very app's own stack frame. The request is applied
 * once, at one place, after tick() has returned for this iteration.
 */
static int g_currentIndex = APP_INDEX_NONE;
static const app_t *g_currentApp = NULL;
static bool g_switchPending = false;
static int g_switchTarget = 0;

// The last switch, kept so the once-a-second profiler line (board-only, in
// runtime.c) can repeat it via rtcore_last_switch_name()/_us() below.
static const char *g_lastSwitchName = NULL;
static uint32_t g_lastSwitchUs = 0;

static void do_switch(int target) {
    uint32_t t0 = rt_time_us();

    const app_t *from = g_currentApp;
    const app_t *to = app_for_index(target);

    if (from != NULL && from->leave != NULL) from->leave();
    g_arenaUsed = 0; // only rewinds the bump pointer; app_alloc() is what
                      // actually zeroes a byte, and only the bytes it hands
                      // out - see its comment for why that split is
                      // deliberate rather than a memset here.
    touch_resolver_reset(); // see its comment above: per-run input state,
                             // reset alongside the arena it sits next to.
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
    if (to->enter != NULL) to->enter();
    gfx_push_all();

    uint32_t elapsedUs = rt_time_us() - t0;

    char msg[64];
    char *p = msg;
    p = fmt_append_str(p, "switch: ");
    p = fmt_append_str(p, to->name);
    p = fmt_append_str(p, " (");
    p = fmt_append_u32(p, elapsedUs);
    p = fmt_append_str(p, " us)");
    *p = '\0';
    rt_log(msg);
    // Also remembered, not just logged, because this line is written within
    // milliseconds of boot and USB CDC discards everything queued before a
    // host opens the port. That makes the one line establishing which app
    // booted, and what the switch cost, exactly the line nobody can ever
    // read. The once-a-second profiler repeats it instead.
    g_lastSwitchName = to->name;
    g_lastSwitchUs = elapsedUs;

    g_currentIndex = target;
    g_currentApp = to;
}

size_t rtcore_arena_used(void) {
    return g_arenaUsed; // see runtime_core.h: the gate's arena-headroom
                         // oracle, read at the app boundary, never by apps
}

void app_switch_to(int index) {
    g_switchPending = true;
    g_switchTarget = index;
}

int app_current(void) {
    return g_currentIndex;
}

const char *rtcore_last_switch_name(void) {
    return g_lastSwitchName;
}

uint32_t rtcore_last_switch_us(void) {
    return g_lastSwitchUs;
}

/* ---- the BOOT+PWR long-press chord: opens and closes the menu -----------
 *
 * Both buttons held, PWR held long enough for the PMIC to report KEY_LONG.
 * Replaces a long-double-press (a short PWR press, then a second PWR press
 * held long, starting within 500ms of the first) - see this file's git
 * history for that version.
 *
 * Why a chord instead: a chord is a state, sampled at one instant, with no
 * timing window to lose. The double-press gesture depended on two things
 * that can each independently fail: the PMIC's press-edge bit surviving
 * intact until the runtime reads it, and a 500ms window between two
 * physical presses. sensors_key_take() is a read-and-clear, not a queue
 * (sensors.h), so when several PMIC bits land in one read - entirely
 * plausible for a small child mashing the button, or just a read that
 * happens to land on the far side of both edges - the double-press's own
 * arming logic (re-evaluated and re-armed against whatever timestamp the
 * previous read happened to leave behind) could see both press edges
 * bundled into a single read and never arm at all, so a real press-then-
 * long-hold attempt produced no menu at all. Confirmed empirically while
 * investigating the reproduction this replaces
 * (emulator/wasm/tests/repro-switch-input.ts's git history has the earlier,
 * double-press version and a dedicated check for this). A chord has no
 * window and no edge history to lose: sensors_boot_down() answers "is BOOT
 * down right now", nothing about the past matters.
 */
// Which app to return to when the menu closes. Kept here (not just inside
// menu.c) because this file is what decides to leave the menu, via this
// same gesture fired a second time while already inside it; menu.c's own
// copy (set through menu_set_return_app()) exists only so the menu can
// default its cursor to "the app you came from" when it opens.
static int g_menuReturnApp = 0;

/* ---- the PWR-held-alone power-off gesture --------------------------------
 *
 * The owner's requirement: holding PWR powers the device off. The hold was
 * five seconds until 2026-08-14 and is PWR_HOLD_POWEROFF_MS (3.5s) now -
 * see that constant below for why only the END of the gesture moved.
 * Neither number exists in hardware - the AXP2101's OFFLEVEL field
 * only offers 4/6/8/10s (sensors.h), and 10s of that is already spent as
 * the menu chord's safety backstop (pmic_raise_poweroff_threshold(),
 * sensors.c) - so this firmware measures the hold itself, in wall-clock
 * time, from the two edges the PMIC now gives it: KEY_PRESS and KEY_RELEASE
 * (register 0x49 bits 1 and 0). KEY_RELEASE was deleted from sensors.h
 * earlier and is back for exactly this reason - see its comment there for
 * the full story of why deleting it was correct then and reinstating it is
 * correct now.
 *
 * THE CHORD MUST NEVER POWER OFF, however long it is held - the owner's
 * requirement, verbatim, and it is checked every tick PWR is down, not just
 * at the 1.5s KEY_LONG verdict the menu-open branch above cares about:
 * sensors_boot_down() is safe to call this often (see the KEY_LONG branch's
 * comment above - bootbtn_poll_level() rate-limits its own sampling
 * internally, regardless of call frequency).
 *
 * The moment BOOT is seen down during a PWR hold, that whole hold is marked
 * TAINTED (g_pwrChordTainted) and stays that way until PWR is released -
 * deliberately not just "not currently dimming while BOOT happens to be
 * held right now". A per-instant check would let a child open the menu
 * (chord fires at 1.5s), keep gripping PWR out of habit while browsing,
 * happen to let go of BOOT alone, and have the device power off under her
 * mid-browse once wall-clock time since the ORIGINAL press reaches
 * PWR_HOLD_POWEROFF_MS -
 * technically still "BOOT not held" at that instant, but not what "the
 * chord must never power off, however long it is held" is asking for.
 * Tainting the whole hold closes that gap: once BOOT has touched this hold
 * at all, this hold cannot dim or power off, full stop, until PWR comes
 * back up and is pressed fresh.
 *
 * THE FADE. The owner approved a progressive fade over a plain wipe at the
 * end: the screen dims continuously while PWR is held alone, starting at
 * 1.5s (the same instant the PMIC's own long-press verdict would fire, so
 * there is only one threshold to learn, not two unrelated ones) and
 * reaching black exactly at PWR_HOLD_POWEROFF_MS, when power-off is
 * requested. Releasing PWR
 * at any point restores full brightness immediately. This adds NOTHING to
 * any app's screen - decision 0002 section 4b forbids exactly that - it is
 * the panel's own brightness changing, the same physical property real
 * power loss would produce anyway, just arriving gradually instead of as a
 * cut. A child who holds PWR, watches it dim, and lets go learns what the
 * button does by exploring it safely: nothing is lost, nothing is punished,
 * and letting go is what undoes it - the best property a control on a toy
 * can have. No countdown digits, no icon, nothing drawn: only brightness.
 *
 * Linear, not eased: a curve that stays imperceptible for the first second
 * or two and then plunges reads as a fault rather than as a warning, where
 * a constant rate is continuously informative from the first frame of the
 * ramp onward - the property "teaches by failing" above depends on.
 *
 * rt_set_brightness() (runtime_core.h) is the fourth host hook alongside
 * rt_log/rt_time_us/rt_halt: a real QSPI panel write on the board, a
 * documented no-op in wasm - see both implementations' comments.
 * set_brightness_if_changed() below is this file's own throttle on top of
 * that hook, so the ramp calls it only when the rounded percentage actually
 * moves, not once a frame.
 *
 * WHEN SHUTDOWN DOES NOTHING, AND WHY THAT ONCE LOOKED LIKE A DEAD DEVICE.
 * sensors_request_poweroff() writes AXP2101 register 0x10 bit 0
 * (sensors.c), sourced from a third-party reference driver rather than a
 * datasheet page that would actually render as text (see sensors.h's
 * "power off" section) - a real, disclosed uncertainty, not a hypothetical
 * one. The first version of this gesture assumed that write would either
 * work (power drops, nothing left to fix) or fail loudly (a timeout,
 * counted in pmicTimeouts). On real hardware it did neither: the write
 * completed without error and the rails stayed up. Firmware kept running,
 * devlink kept answering `APP 0 chrono`, and the panel sat at zero
 * brightness forever, because nothing looked at the clock again after
 * calling sensors_request_poweroff() once. From across the room, and to
 * the owner holding it, that is indistinguishable from a dead toy.
 *
 * The fix is not a better power-off write (still sourced the same way,
 * still not verified against a readable datasheet - see sensors.h). It is
 * the firmware-side assumption that a single write to a chip on the far
 * side of an i2c bus can silently do nothing, paired with a recovery that
 * does not need to know why. PWR_HOLD_HARD_CEILING_MS bounds how long
 * brightness may EVER stay below baseline, counted from the press that
 * started the current hold, regardless of what g_pwrDown/
 * g_pwrChordTainted/g_pwrPoweroffSent currently believe. If firmware is
 * still ticking once that ceiling passes, the shutdown did not take (or a
 * release edge was lost somewhere - see sensors.h's PWR key section on how
 * a bit can go missing), and either way the right response is the same:
 * force brightness back up, forget the hold, and say so in the log. This
 * check runs first in the function below, unconditionally, every tick -
 * "checked every tick regardless of what the rest of the state machine
 * believes" is deliberate, not an optimisation left for later.
 */
// 800, down from 1500 on 2026-08-15: the owner asked for the panel to start
// going dark sooner ("je veux que ca commence a s'assombrir plus rapidement").
//
// I had told him 1500 was a floor because it is where the AXP2101's long-press
// verdict lands. That was wrong, and worth correcting here rather than quietly:
// the hold is counted from KEY_PRESS, the RAW press edge, not from the verdict,
// so nothing forces the dim to wait for the PMIC to agree.
//
// What it does cost: a deliberate-but-slow press, a child leaning on PWR to
// start the stopwatch, now pales the panel briefly before she lets go. It
// restores itself on release. 800 is chosen to sit clear of an ordinary tap
// (a few hundred ms) while still reading as immediate.
//
// The BOOT+PWR chord is unaffected: the moment BOOT is seen the hold is marked
// tainted and brightness returns to baseline, so a real chord never dims.
//
// Welcome side effect: with PWR_HOLD_POWEROFF_MS at 2000 the fade was 500ms,
// which read as a blink. It is 1200ms again.
#define PWR_HOLD_DIM_START_MS       800u
// 3500, down from 5000 on 2026-08-14: the owner asked for the whole gesture
// to take 1.5s less. The dim START deliberately does not move, because 1500
// is not a taste number: it is where the PMIC's own long-press verdict lands,
// so the fade begins exactly when the hardware agrees a long press is
// happening. What shortens is the ramp between them, from 3.5s to 2s.
// 2000, down from 3500 on 2026-08-15, the second 1.5s the owner asked for.
//
// The dim START cannot follow it down: 1500 is where the AXP2101's own
// long-press verdict lands, and dimming earlier would make the panel react
// to ordinary short presses. So the fade is now 500ms, and that is the real
// cost of this change rather than the shorter hold: the fade is the only
// warning before the rails cut, and at 500ms it reads closer to a blink than
// to a ramp. Told to the owner before it was made.
//
// Also worth knowing: 2000 is only 500ms past the verdict the BOOT+PWR chord
// uses, so a chord attempt whose BOOT went unseen (BOOT is polled at 20Hz)
// now becomes a power-off in two seconds rather than five. g_pwrChordTainted
// still cancels it whenever BOOT IS seen.
#define PWR_HOLD_POWEROFF_MS        2000u
// How long to wait, after the panel reaches black and a shutdown command
// has been sent, before concluding it did not take. The AXP2101 cutting its
// own rails should be near-instant if it happens at all; 1.5s is generous
// margin, not a tuned value, chosen for the same reason
// PWR_HOLD_DIM_START_MS lines up with the PMIC's own long-press verdict -
// a round number a reader does not have to look up twice.
#define PWR_HOLD_RECOVER_MS         1500u
// The hard ceiling the invariant enforces: how long brightness may ever
// stay below baseline, full stop, counted from the press that started the
// current hold. Deliberately a single derived constant rather than a
// second independent number, so PWR_HOLD_POWEROFF_MS and
// PWR_HOLD_RECOVER_MS cannot drift apart from what this actually bounds.
#define PWR_HOLD_HARD_CEILING_MS    (PWR_HOLD_POWEROFF_MS + PWR_HOLD_RECOVER_MS)
#define PWR_BASELINE_BRIGHTNESS_PCT 100 // what runtime.c's boot already sets
                                        // (AMOLED_1IN8_SetBrightness(180)
                                        // clamps to 100 internally - see its
                                        // comment in runtime.c). If a future
                                        // idle-dim feature (decision 0002
                                        // section 10, still open) starts
                                        // changing brightness for its own
                                        // reasons, this fixed baseline needs
                                        // to become "whatever it was when
                                        // this hold started" instead - noted
                                        // here so that feature's author
                                        // trips over this rather than
                                        // shipping two dimmers that fight.

static bool g_pwrDown = false;
static bool g_pwrChordTainted = false;
static bool g_pwrPoweroffSent = false;
static uint32_t g_pwrPressStartMs = 0;
static int g_lastBrightnessPct = -1; // -1: never set, forces the first real
                                      // call through even if it would
                                      // compute exactly the baseline

static void set_brightness_if_changed(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == g_lastBrightnessPct) return; // the cheapest possible no-op:
        // see rt_set_brightness()'s comment in runtime.c for why a dropped
        // repeat call is free and an extra one is not.
    g_lastBrightnessPct = pct;
    rt_set_brightness((uint8_t)pct);
}

static void poweroff_gesture_tick(uint32_t nowMs, uint8_t key) {
    if (key & KEY_PRESS) {
        g_pwrDown = true;
        g_pwrPressStartMs = nowMs;
        g_pwrChordTainted = false;
        g_pwrPoweroffSent = false;
    }
    if (key & KEY_RELEASE) {
        g_pwrDown = false;
    }

    // THE INVARIANT. See "WHEN SHUTDOWN DOES NOTHING" above: this is what
    // brought the owner's board back from a panel stuck at zero brightness
    // with the firmware otherwise alive and answering devlink. Runs before
    // anything else below, and the ACTION does not consult
    // g_pwrChordTainted or g_pwrPoweroffSent - a hold this old resetting
    // itself and forcing brightness to baseline is always correct, never a
    // case that needs special-casing away, including a chord held (tainted,
    // never dimmed) well past this same ceiling: nothing there was ever
    // wrong, but there is no reason to keep tracking a hold this stale
    // either, and the reset is free.
    //
    // The LOG line is narrower on purpose: only claim "shutdown did not
    // take" when a shutdown was actually requested (g_pwrPoweroffSent).
    // A long-held, BOOT-tainted chord also crosses this ceiling on a slow
    // child, and did not attempt anything - saying otherwise would be a
    // false alarm in the one log a real incident like the owner's is
    // diagnosed from.
    if (g_pwrDown && (nowMs - g_pwrPressStartMs) >= PWR_HOLD_HARD_CEILING_MS) {
        if (g_pwrPoweroffSent) {
            rt_log("poweroff: still running past the recovery window - shutdown did not take, restoring brightness");
        }
        g_pwrDown = false;
        g_pwrChordTainted = false;
        set_brightness_if_changed(PWR_BASELINE_BRIGHTNESS_PCT);
        return;
    }

    if (!g_pwrDown) {
        set_brightness_if_changed(PWR_BASELINE_BRIGHTNESS_PCT);
        return;
    }

    if (sensors_boot_down()) {
        g_pwrChordTainted = true;
    }
    if (g_pwrChordTainted) {
        set_brightness_if_changed(PWR_BASELINE_BRIGHTNESS_PCT);
        return;
    }

    uint32_t heldMs = nowMs - g_pwrPressStartMs;
    if (heldMs < PWR_HOLD_DIM_START_MS) {
        set_brightness_if_changed(PWR_BASELINE_BRIGHTNESS_PCT);
    } else if (heldMs < PWR_HOLD_POWEROFF_MS) {
        uint32_t rampMs = heldMs - PWR_HOLD_DIM_START_MS;
        uint32_t rampSpan = PWR_HOLD_POWEROFF_MS - PWR_HOLD_DIM_START_MS;
        int pct = PWR_BASELINE_BRIGHTNESS_PCT -
                  (int)((uint64_t)PWR_BASELINE_BRIGHTNESS_PCT * rampMs / rampSpan);
        set_brightness_if_changed(pct);
    } else {
        set_brightness_if_changed(0);
        if (!g_pwrPoweroffSent) {
            g_pwrPoweroffSent = true;
            // The threshold is FORMATTED IN, not spelled out in the string.
            // This line read "PWR held 5s alone" for as long as it took
            // someone to notice, after PWR_HOLD_POWEROFF_MS moved to 3500 -
            // and this is the one line an incident like the owner's
            // (sensors_request_poweroff() silently doing nothing, see
            // "WHEN SHUTDOWN DOES NOTHING" above) actually gets read from.
            // A diagnostic that misreports the threshold it just crossed is
            // worse than one that never mentioned it.
            char msg[112];
            char *p = msg;
            p = fmt_append_str(p, "poweroff: PWR held ");
            p = fmt_append_u32(p, PWR_HOLD_POWEROFF_MS);
            p = fmt_append_str(p, "ms alone (BOOT never held during this hold), requesting power-off");
            *p = '\0';
            rt_log(msg);
            sensors_request_poweroff();
        }
    }
}

/* ---- orientation: rotated into the app's own space, once, here -----------
 *
 * tilt.h publishes gravity in PANEL space. Half this device's apps draw in
 * LANDSCAPE space (app_t.landscape), through gfx's rotation, and handing
 * those a panel-space vector would mean every one of them rotating it back
 * by hand - which is the written-twice seam tilt.h exists to prevent,
 * reintroduced one link further along, and in the worst possible place: a
 * spirit level whose bubble moves at ninety degrees to the tilt passes
 * every automated check this project can build, because no software oracle
 * knows which way is up.
 *
 * So the runtime rotates it, exactly like gfx_land_rect() already rotates
 * rectangles for the same apps, and an app reads gravity in the same
 * coordinates it draws in.
 *
 * The mapping, derived from gfx.h's own: landscape (lx, ly) -> panel
 * (PANEL_W - 1 - ly, lx). Differentiating that (a direction has no origin,
 * so the constant drops out) gives panel direction (dpx, dpy) from
 * landscape (dlx, dly) as (-dly, dlx), and the inverse - which is what is
 * needed here, panel to landscape - as:
 *
 *     dlx =  dpy
 *     dly = -dpx
 *
 * Sanity check against the physical device: held sideways with the buttons
 * along the top edge (gfx.h's landscape posture), gravity reads (-1, 0) in
 * panel axes, which comes out (0, +1) in landscape axes: straight down the
 * screen the app drew. Correct.
 *
 * The up-edge code rotates the same way, as a relabelling: the panel's
 * right edge IS the landscape top edge, so land = (panel + 3) % 4.
 * gz is unchanged by any rotation about the panel normal, by definition.
 */
static app_tilt_t tilt_for_app(const tilt_reading_t *r, bool landscape) {
    app_tilt_t t;
    t.tiltDeg = r->tiltDeg;
    t.valid = r->valid;
    t.coasting = r->coasting; // not affected by a rotation about the panel normal
    t.gz = r->gz;
    if (landscape) {
        t.gx = r->gy;
        t.gy = -r->gx;
        t.up = (uint8_t)((r->up + 3u) % 4u);
    } else {
        t.gx = r->gx;
        t.gy = r->gy;
        t.up = r->up;
    }
    return t;
}

// The last thing handed to an app, kept for the one-off instruments that
// have to read at the app boundary rather than at the sensor: devlink's
// TILT command (the axis ritual in tilt.h) and the emulator's headless
// orientation test. Nothing in the runtime's own logic reads this.
static app_tilt_t g_lastAppTilt;

void rtcore_last_tilt(app_tilt_t *out) {
    *out = g_lastAppTilt;
}

/* ---- lifecycle: what the host drives -------------------------------------- */

// Whether this is the very first tick, so dtMs reads 0 for it instead of
// being computed against an uninitialised "previous" timestamp. The
// pre-split main() set lastNowMs once, right before entering the loop, so
// the very first dtMs was actually a few microseconds, not exactly 0; that
// difference is unobservable to any app (dtMs is clamped and used only for
// continuous-time bookkeeping, never compared to exactly 0) and is what
// rtcore_init() taking no clock reading of its own costs.
static bool g_started = false;
static uint32_t g_lastNowMs = 0;

void rtcore_init(void) {
    do_switch(0); // enter app 0 (chrono), through the same path a later
                  // switch takes, so its timing gets the same logged line.
    g_started = false;
}

void rtcore_tick(uint32_t nowMs) {
    if (!g_started) {
        g_lastNowMs = nowMs;
        g_started = true;
    }

    // Clamped so a slow one-off hands the next tick a sane dt instead of a
    // multi-second jump that a timer app would visibly lurch on.
    uint32_t dtMs = nowMs - g_lastNowMs;
    if (dtMs > 250) dtMs = 250;
    g_lastNowMs = nowMs;

    app_frame_t frame = { 0 };
    frame.nowMs = nowMs;
    frame.dtMs = dtMs;

    // ---- touch: drain into the simple down/pressed/released/x/y shape,
    // except when the sketchpad is running.
    //
    // This is a wart: app_t carries no "I want raw touch" flag, and the
    // comparison below hardcodes one specific app's identity into this
    // file, which is exactly the kind of thing app.h's design is supposed
    // to keep out of here. It is done anyway because building the general
    // mechanism serves exactly one consumer today: the sketchpad is the
    // only app whose whole reason for existing is stroke reconstruction
    // from the raw sample stream (see sensors.h and app.h's comment on
    // touchDown/X/Y).
    //
    // Consequence of skipping the drain here: sensors_set_finger_down() is
    // only called from this branch too, so while the sketchpad is running
    // it is that app's own job (in its own tick(), draining
    // sensors_touch_next() itself) to keep that signal fresh.
    if (g_currentApp != &g_sketchApp) {
        // g_touchWasDown/g_touchLastX/g_touchLastY: file-scope now, reset by
        // touch_resolver_reset() on every switch - see that function's
        // comment (up by the arena) for why these cannot be per-app-run
        // statics that just happen to live in this function.
        touch_sample_t s;
        bool down = g_touchWasDown;
        int x = g_touchLastX, y = g_touchLastY;
        while (sensors_touch_next(&s)) {
            down = (s.fingers != 0);
            x = s.x;
            y = s.y;
        }
        if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
        if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;

        frame.touchPressed = down && !g_touchWasDown;
        frame.touchReleased = !down && g_touchWasDown;
        frame.touchDown = down;
        frame.touchX = x;
        frame.touchY = y;

        g_touchWasDown = down;
        g_touchLastX = x;
        g_touchLastY = y;

        sensors_set_finger_down(down);
    }

    // ---- power key: exactly one sensors_key_take() call per loop (see
    // sensors.h: read-and-clear, not a queue), then the menu gesture
    // consumes what belongs to it and the rest goes to the app.
    uint8_t key = sensors_key_take();

    if (key & KEY_LONG) {
        // sensors_boot_down() borrows the flash chip select on the board
        // (bootbtn.h), which sounds expensive but is not sampled any
        // faster for it: bootbtn_poll_level() rate-limits its own sampling
        // to once every 50ms internally, regardless of how often its
        // caller asks (see bootbtn.h's comment on bootbtn_poll_level() -
        // "safe to call every main-loop iteration"). This branch still only
        // NEEDS one read per long-press verdict, which is exactly what this
        // gate gives it, but the power-off gesture below calls the same
        // function every tick PWR is held for an unrelated reason (it needs
        // BOOT's live level, not just its value at one instant) and that is
        // not a new cost this callsite has to worry about sharing.
        bool bootHeld = sensors_boot_down();
        if (g_currentIndex == APP_INDEX_MENU) {
            // Already in the menu: the same chord fired again closes it,
            // back to whatever was running before it opened.
            if (bootHeld) {
                app_switch_to(g_menuReturnApp);
                sensors_boot_consume_click(); // releasing BOOT after this
                                               // must not also read as a
                                               // click to the app we land on
                key &= ~KEY_LONG;
            }
        } else if (bootHeld) {
            g_menuReturnApp = g_currentIndex;
            menu_set_return_app(g_menuReturnApp);
            app_switch_to(APP_INDEX_MENU);
            // menu_tick() does not read bootClicked, so this is
            // precautionary rather than load-bearing here - kept symmetric
            // with the close branch above, so BOOT's release is swallowed
            // by the chord that used it regardless of which side it opens
            // or closes.
            sensors_boot_consume_click();
            key &= ~KEY_LONG;
        }
    }

    // The power-off gesture below reads KEY_PRESS/KEY_RELEASE off `key`
    // too, alongside whatever the chord above already consumed. It does not
    // remove anything from `key`: an app that already tolerates
    // KEY_PRESS passing through unchanged tolerates KEY_RELEASE the same
    // way (see sensors.h's PWR key section on why the release edge is back,
    // and this file's own "the PWR-held-alone power-off gesture" section
    // below for why it is not masked out here the way KEY_LONG sometimes
    // is).
    poweroff_gesture_tick(nowMs, key);

    frame.key = key; // KEY_SHORT, KEY_PRESS and KEY_RELEASE always pass
                      // through unchanged. Only a KEY_LONG that opened or
                      // closed the menu is missing by the time an app sees
                      // this.

    frame.bootClicked = sensors_boot_clicked();

    {
        static uint32_t lastEraseSeq = 0;
        uint32_t seq = sensors_erase_seq();
        // Advance the tracked sequence every loop regardless of whether the
        // current app wants shake, so switching into a shake-aware app does
        // not immediately hand it a stale "shaken" for jolts that happened
        // while some other app was running.
        bool bumped = (seq != lastEraseSeq);
        lastEraseSeq = seq;
        frame.shaken = bumped && g_currentApp->wantsShake;
    }

    // Orientation. Read once per frame, rotated into whatever space the
    // current app draws in (see tilt_for_app above), and handed over with
    // no opt-in flag: it consumes nothing and destroys nothing, unlike
    // shake.
    {
        tilt_reading_t r;
        tilt_read(nowMs, &r);
        frame.tilt = tilt_for_app(&r, g_currentApp->landscape);
        g_lastAppTilt = frame.tilt;
    }

    g_currentApp->tick(&frame);

    if (g_switchPending) {
        g_switchPending = false;
        do_switch(g_switchTarget);
    }
}
