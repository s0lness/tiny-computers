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
extern const app_t g_menuApp;
extern void menu_set_return_app(int index);

const app_t *const g_apps[] = { &g_chronoApp, &g_sketchApp, &g_timerApp };
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
 * actually exist for that target). The two rt_log() call sites below are
 * the only formatted diagnostics in this file; these are just enough for
 * "name (N us)" and "requested N bytes, M of K used", not a general
 * formatter.
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
        // (bootbtn.h) and must not be sampled often - one read per
        // long-press verdict, never per tick, which is exactly what this
        // gate gives it: this branch only runs on the tick a KEY_LONG bit
        // is actually present.
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

    frame.key = key; // KEY_SHORT and KEY_PRESS always pass through
                      // unchanged; the release edge (0x01) is never even
                      // delivered here - see sensors.h's PWR key section.
                      // Only a KEY_LONG that opened or closed the menu is
                      // missing by the time an app sees this.

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

    g_currentApp->tick(&frame);

    if (g_switchPending) {
        g_switchPending = false;
        do_switch(g_switchTarget);
    }
}
