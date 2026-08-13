/*
 * runtime: startup, the app table, the arena, app switching, and the main
 * loop. This file is the thing docs/decisions/0002 actually describes: apps
 * used to each be their own main.c with their own copy of the startup
 * sequence, the push-window math and the PWR long-double-press gesture, and
 * switching between them meant a reboot. None of that is true any more.
 * Apps are callbacks; this file is what calls them.
 *
 * What used to live in firmware/main.c and is deliberately NOT here any
 * more:
 *   - the sketchpad's stroke reconstruction (now apps/sketch.c's problem);
 *   - appswitch.c's reboot-to-other-slot dance (switching is a function call
 *     now; the store/ partitions are a crash-recovery fallback, not how apps
 *     change - see docs/decisions/0002 section 6, and the watchdog note
 *     below for how that fallback and this runtime's own watchdog coexist);
 *   - a KEY_SHORT-triggered full-panel refresh (apps own their pixels now,
 *     they push what they change; a blanket refresh on every short press
 *     was a sketchpad-only convenience, not something every app wants).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"

#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"

#include "app.h"
#include "gfx.h"
#include "sensors.h"

// Cross-directory headers, included by relative path on purpose rather than
// by bare filename: which directories end up on the compiler's include path
// is CMakeLists.txt's decision (owned by another agent, mid-edit alongside
// this file), and a relative path resolves correctly regardless of how that
// list is written.
#include "../bootbtn.h"
#include "../apps/menu.h"

/* ---- the app table ------------------------------------------------------
 *
 * Defined in each app's own file, one extern per app, exactly as app.h's
 * header comment describes: adding an app is one entry here plus one file.
 * The menu is deliberately not in this list (see APP_INDEX_MENU below):
 * it is the shell that picks among these, not one of the things being
 * picked.
 */
extern const app_t g_chronoApp;
extern const app_t g_sketchApp;
extern const app_t g_timerApp;
extern const app_t g_menuApp;

const app_t *const g_apps[] = { &g_chronoApp, &g_sketchApp, &g_timerApp };
const int g_appCount = sizeof(g_apps) / sizeof(g_apps[0]);

// The menu's private slot. Not a valid g_apps[] index by construction (it is
// negative), so app_for_index() below can tell "the menu" apart from "app N"
// with one comparison, and nothing that enumerates g_apps[] (the menu's own
// tile layout included) can ever accidentally include the menu itself.
#define APP_INDEX_MENU (-1)
// Startup-only sentinel: "nothing has been entered yet", so do_switch()'s
// first-ever call has no outgoing app to call leave() on. Never observed by
// app_current() once main() reaches the loop.
#define APP_INDEX_NONE (-2)

static const app_t *app_for_index(int index) {
    if (index == APP_INDEX_MENU) return &g_menuApp;
    return g_apps[index];
}

/* ---- the arena -----------------------------------------------------------
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
// treated like one: loud, and impossible to miss, per this file's task brief
// rather than app.h's stale paraphrase of it.
static void arena_overflow_trap(size_t requested) {
    printf("FATAL: app arena overflow: requested %u bytes, %u of %u already used\r\n",
           (unsigned)requested, (unsigned)g_arenaUsed, (unsigned)APP_ARENA_BYTES);

    uint16_t alarm = px_swap(0xF800); // pure red, swapped for the panel's byte order (see gfx.h)
    for (;;) {
        gfx_fill_rect(0, 0, PANEL_W, PANEL_H, alarm);
        gfx_push_all();
        // Keep feeding the watchdog here on purpose. Left unfed, the panel
        // would go red for a few seconds and then the watchdog would reboot
        // the board, which reruns the exact same overflow on the exact same
        // app and looks, from across the room, indistinguishable from a
        // battery-dead device that keeps power-cycling. The whole point of
        // this trap is to be a screen someone can walk up to and read.
        watchdog_update();
        sleep_ms(500);
    }
}

void *app_alloc(size_t bytes) {
    size_t aligned = (g_arenaUsed + 7u) & ~(size_t)7u;
    if (bytes > APP_ARENA_BYTES || aligned > APP_ARENA_BYTES - bytes) {
        arena_overflow_trap(bytes);
    }
    void *p = &g_arena[aligned];
    g_arenaUsed = aligned + bytes;
    return p;
}

/* ---- switching -----------------------------------------------------------
 *
 * app_switch_to() only ever records a request. Both the runtime's own menu
 * gesture handling and an app's tick() (the menu tapping a tile) call it,
 * and neither is a safe place to tear down the currently-running app: the
 * caller might be that very app's own stack frame. The request is applied
 * once, at one place, after tick() has returned for this iteration.
 */
static int g_currentIndex = APP_INDEX_NONE;
static const app_t *g_currentApp = NULL;
static bool g_switchPending = false;
static int g_switchTarget = 0;

// The last switch, kept so the once-a-second profiler line can repeat it.
// See do_switch() for why printing it once is not enough.
static const char *g_lastSwitchName = NULL;
static uint32_t g_lastSwitchUs = 0;

static void do_switch(int target) {
    uint32_t t0 = time_us_32();

    const app_t *from = g_currentApp;
    const app_t *to = app_for_index(target);

    if (from != NULL && from->leave != NULL) from->leave();
    g_arenaUsed = 0;
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
    if (to->enter != NULL) to->enter();
    gfx_push_all();

    uint32_t elapsedUs = time_us_32() - t0;
    printf("switch: %s (%lu us)\r\n", to->name, (unsigned long)elapsedUs);
    // Also remembered, not just printed, because this line is written within
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

/* ---- the PWR long-double-press gesture: opens and closes the menu --------
 *
 * A press, a release, then a second press held past the PMIC's 1.5s
 * long-press threshold, with the second press starting within
 * MENU_DOUBLE_WINDOW_MS of the first. Lifted from firmware/main.c's
 * (~line 1300-1350) identical state machine, which every app that read the
 * PMIC directly used to duplicate; now there is exactly one copy, here,
 * because only the runtime touches sensors_key_take() at all.
 *
 * Why THIS shape and not, say, a single long press: a stopwatch start/stop
 * is two SHORT presses (0x08, the release-confirmed verdict bit, never the
 * raw press edge and never a press held past 1.5s), so nothing about
 * ordinary stopwatch use can arm-and-hold into this gesture by accident. A
 * single long press was the first draft and would collide with any app that
 * ever wants "hold PWR to do X" for its own purposes; requiring the priming
 * short press first is what makes the gesture unambiguous everywhere.
 *
 * menuDoubleArmed is recomputed on every KEY_PRESS edge, never set-and-
 * forget: it always answers "was the press immediately before this one
 * within the window", not "was there ever a press in the window at some
 * point in the past".
 */
#define MENU_DOUBLE_WINDOW_MS 500

// A generous, few-second watchdog timeout: the 182ms cold boot this
// architecture exists to buy back (see docs/decisions/0002 section 1) makes
// a watchdog reboot nearly invisible, so there is no reason to cut this
// close.
#define RUNTIME_WATCHDOG_MS 4000

static uint32_t g_lastPwrPressMs = 0;
static bool g_menuDoubleArmed = false;
// Which app to return to when the menu closes. Kept here (not just inside
// menu.c) because the runtime is what decides to leave the menu, via this
// same gesture fired a second time while already inside it; menu.c's own
// copy (set through menu_set_return_app()) exists only so the menu can
// default its cursor to "the app you came from" when it opens.
static int g_menuReturnApp = 0;

/* ---- profiler --------------------------------------------------------
 *
 * Loop count and sensors_stats() deltas, once a second, modelled on
 * firmware/main.c's #if PROFILE block. Printing every loop would make the
 * printf itself the thing being measured; once a second is cheap enough to
 * leave on by default and still says nothing about steady-state cost.
 */
#define PROFILE 1

#if PROFILE
static uint32_t g_profLoops = 0;
static uint32_t g_profLastMs = 0;
static sensors_stats_t g_profLastStats;
#endif

int main(void) {
    // Startup order is exact and load-bearing: everything through
    // AMOLED_1IN8_SetBrightness() and sensors_init() runs single-threaded on
    // core0 and is free to touch i2c1. sensors_start() launches core1, and
    // from that call onward core0 must never touch i2c1 again (see
    // sensors.h's ownership rule banner) - nothing below that line does.
    stdio_init_all();
    DEV_Module_Init();
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    AMOLED_1IN8_Init();
    AMOLED_1IN8_SetBrightness(180);

    sensors_init();

    if (!gfx_init()) {
        // gfx_init() already printed why (SRAM already spoken for). There is
        // no framebuffer to paint a loud red screen into here, unlike the
        // arena trap above, so this is the one failure in this file that
        // cannot be made visible on the panel; it can only be visible on the
        // serial console. Hang rather than stumble on with a NULL gfx_fb.
        for (;;) { }
    }

    sensors_start();
    // ---- core0 must never touch i2c1 past this point. ----

    // Whether this collides with appswitch.c / bootreq.h's own use of the
    // watchdog: it does not. bootreq.h documents (and this was checked
    // against the pico-sdk 2.x hardware_watchdog source, not assumed) that
    // watchdog_reboot()/watchdog_enable() only ever touch
    // WATCHDOG_SCRATCH4..7; bootreq.h's request word lives in SCRATCH0
    // specifically because it was picked to be disjoint from that range.
    // appswitch.c's own watchdog_reboot(0, 0, 0) is a one-shot "reboot now"
    // call, not a periodic timer, so there is no second timeout running
    // concurrently with this one to race against either. The two mechanisms
    // share the peripheral but not a single register or a single meaning.
    // pause_on_debug=true so a debugger session does not fight it.
    watchdog_enable(RUNTIME_WATCHDOG_MS, true);

    do_switch(0); // enter app 0 (chrono), through the same path a later
                  // switch takes, so its timing gets the same printed line.

    uint32_t lastNowMs = to_ms_since_boot(get_absolute_time());

#if PROFILE
    g_profLastMs = lastNowMs;
    sensors_stats(&g_profLastStats);
#endif

    for (;;) {
        uint32_t nowMs = to_ms_since_boot(get_absolute_time());

        // Clamped so a slow one-off (a printf that blocks on a host that
        // is not draining USB CDC, say) hands the next tick a sane dt
        // instead of a multi-second jump that a timer app would visibly
        // lurch on.
        uint32_t dtMs = nowMs - lastNowMs;
        if (dtMs > 250) dtMs = 250;
        lastNowMs = nowMs;

        app_frame_t frame = { 0 };
        frame.nowMs = nowMs;
        frame.dtMs = dtMs;

        // ---- touch: drain into the simple down/pressed/released/x/y shape,
        // except when the sketchpad is running.
        //
        // This is a wart: app_t carries no "I want raw touch" flag, and the
        // comparison below hardcodes one specific app's identity into the
        // runtime, which is exactly the kind of thing app.h's design is
        // supposed to keep out of here. It is done anyway because building
        // the general mechanism (a flag, a second frame shape, or handing
        // every app the raw queue and making all of them do their own
        // resolution) serves exactly one consumer today: the sketchpad is
        // the only app whose whole reason for existing is stroke
        // reconstruction from the raw sample stream (see sensors.h and
        // app.h's comment on touchDown/X/Y). A general mechanism nobody else
        // would ever use is not simpler than one named comparison; it is the
        // same special case wearing a configuration flag.
        //
        // Consequence of skipping the drain here: sensors_set_finger_down()
        // is only called from this branch too, so while the sketchpad is
        // running it is that app's own job (in its own tick(), draining
        // sensors_touch_next() itself) to keep that signal fresh - it is the
        // only one with an accurate up/down state during its own drain.
        // sensors.h's finger-down suppression exists specifically so a
        // resting hand while drawing cannot fire a shake, so this is not
        // optional for that app, just not this file's to do.
        if (g_currentApp != &g_sketchApp) {
            static bool wasDown = false;
            static int lastX = 0, lastY = 0;

            touch_sample_t s;
            bool down = wasDown;
            int x = lastX, y = lastY;
            while (sensors_touch_next(&s)) {
                down = (s.fingers != 0);
                x = s.x;
                y = s.y;
            }
            if (x < 0) x = 0; else if (x > PANEL_W - 1) x = PANEL_W - 1;
            if (y < 0) y = 0; else if (y > PANEL_H - 1) y = PANEL_H - 1;

            frame.touchPressed = down && !wasDown;
            frame.touchReleased = !down && wasDown;
            frame.touchDown = down;
            frame.touchX = x;
            frame.touchY = y;

            wasDown = down;
            lastX = x;
            lastY = y;

            sensors_set_finger_down(down);
        }

        // ---- power key: exactly one sensors_key_take() call per loop (see
        // sensors.h: read-and-clear, not a queue), then the menu gesture
        // consumes what belongs to it and the rest goes to the app.
        uint8_t key = sensors_key_take();

        if (key & KEY_PRESS) {
            g_menuDoubleArmed = (nowMs - g_lastPwrPressMs <= MENU_DOUBLE_WINDOW_MS);
            g_lastPwrPressMs = nowMs;
        }

        if (key & KEY_LONG) {
            if (g_currentIndex == APP_INDEX_MENU) {
                // Already in the menu: firing the same gesture again closes
                // it, back to whatever was running before it opened.
                if (g_menuDoubleArmed) {
                    app_switch_to(g_menuReturnApp);
                    key &= ~KEY_LONG;
                }
            } else if (g_menuDoubleArmed) {
                g_menuReturnApp = g_currentIndex;
                menu_set_return_app(g_menuReturnApp);
                app_switch_to(APP_INDEX_MENU);
                key &= ~KEY_LONG;
            }
            // Cleared unconditionally: a long press, armed or not, has used
            // up whatever the preceding short press was priming. A long
            // press that did not open or close the menu is passed through
            // below untouched (still set in `key`), for an app that wants
            // its own meaning for a hold.
            g_menuDoubleArmed = false;
        }

        frame.key = key; // KEY_SHORT and KEY_PRESS always pass through
                          // unchanged; KEY_RELEASE was never consumed here
                          // either. Only a KEY_LONG that opened or closed the
                          // menu is missing by the time an app sees this.

        frame.bootClicked = bootbtn_poll_clicked();

        {
            static uint32_t lastEraseSeq = 0;
            uint32_t seq = sensors_erase_seq();
            // Advance the tracked sequence every loop regardless of whether
            // the current app wants shake, so switching into a shake-aware
            // app does not immediately hand it a stale "shaken" for jolts
            // that happened while some other app was running.
            bool bumped = (seq != lastEraseSeq);
            lastEraseSeq = seq;
            frame.shaken = bumped && g_currentApp->wantsShake;
        }

        g_currentApp->tick(&frame);

        if (g_switchPending) {
            g_switchPending = false;
            do_switch(g_switchTarget);
        }

        // Fed once per iteration. A hung app (or a wedged sensors_touch_next
        // loop, though the ring is lock-free and single-producer/single-
        // consumer so that should not happen) stops feeding this and the
        // board reboots on its own; see the watchdog_enable() comment above
        // for why that does not race appswitch.c/bootreq.h's own use of the
        // watchdog peripheral.
        watchdog_update();

#if PROFILE
        g_profLoops++;
        if (nowMs - g_profLastMs >= 1000) {
            g_profLastMs = nowMs;
            sensors_stats_t cur;
            sensors_stats(&cur);
            printf("prof app=%s switch=%luus | loops=%lu/s | touch reads=%lu timeouts=%lu drops=%lu recoveries=%lu "
                   "| imu timeouts=%lu | pmic timeouts=%lu\r\n",
                   g_lastSwitchName ? g_lastSwitchName : "?",
                   (unsigned long)g_lastSwitchUs,
                   (unsigned long)g_profLoops,
                   (unsigned long)(cur.touchReads - g_profLastStats.touchReads),
                   (unsigned long)(cur.touchTimeouts - g_profLastStats.touchTimeouts),
                   (unsigned long)(cur.touchQueueDrops - g_profLastStats.touchQueueDrops),
                   (unsigned long)(cur.touchRecoveries - g_profLastStats.touchRecoveries),
                   (unsigned long)(cur.imuTimeouts - g_profLastStats.imuTimeouts),
                   (unsigned long)(cur.pmicTimeouts - g_profLastStats.pmicTimeouts));
            g_profLastStats = cur;
            g_profLoops = 0;
        }
#endif
    }
}
