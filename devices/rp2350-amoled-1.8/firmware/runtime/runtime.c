/*
 * runtime: the board's entry point. Split from what used to be this whole
 * file per docs/decisions/0003-emulator-runs-the-real-apps.md: the arena,
 * the app table, app switching and the per-frame dispatch are now
 * runtime_core.c, portable and hardware-blind. What is left here is
 * everything that IS hardware: startup ordering, the watchdog, the
 * devlink wiring, the once-a-second profiler, and this file's
 * implementation of the three small hooks runtime_core.h declares
 * (rt_log, rt_time_us, rt_halt) - see runtime_core.h's header comment for
 * why those three exist at all.
 *
 * What used to live in firmware/main.c and is deliberately NOT here any
 * more (this predates the runtime_core split, carried over from this
 * file's original header comment):
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
#include "runtime_core.h"

// Cross-directory headers, included by relative path on purpose rather than
// by bare filename: which directories end up on the compiler's include path
// is CMakeLists.txt's decision, and a relative path resolves correctly
// regardless of how that list is written.
#include "../devlink.h"        // the agent-facing screenshot/touch-injection
                                // link; see "devlink wiring" below for the
                                // hooks this file feeds it.

/* ---- the three hooks runtime_core.h declares ------------------------------
 *
 * See runtime_core.h's header comment for why these exist: three pico-sdk
 * calls (printf's stdio lock, time_us_32(), watchdog_update()+sleep_ms())
 * that runtime_core.c cannot make itself.
 */

void rt_log(const char *msg) {
    printf("%s\r\n", msg);
}

uint32_t rt_time_us(void) {
    return time_us_32();
}

// Matches the pre-split arena_overflow_trap()'s loop exactly: keep feeding
// the watchdog here on purpose. Left unfed, the panel would go red for a few
// seconds and then the watchdog would reboot the board, which reruns the
// exact same overflow on the exact same app and looks, from across the
// room, indistinguishable from a battery-dead device that keeps
// power-cycling. The whole point of this trap is to be a screen someone can
// walk up to and read. runtime_core.c has already painted the panel red and
// logged why before calling this; all that is left is to hold it there.
void rt_halt(void) {
    for (;;) {
        watchdog_update();
        sleep_ms(500);
    }
}

/* ---- devlink wiring --------------------------------------------------
 *
 * devlink (../devlink.h) is the agent-facing screenshot and touch-injection
 * link. This is the only file that knows devlink exists: every hook below is
 * a thin adapter from devlink's generic function-pointer shape onto the
 * runtime's real API (sensors_inject_touch/erase, app_switch_to,
 * app_current, g_apps - all from app.h/sensors.h, or APP_INDEX_MENU from
 * runtime_core.h), so devlink.c itself never has to know what an "app" or a
 * touch queue is, and this file never has to know devlink's wire protocol.
 */

// DOWN and MOVE both mean "a finger is present at (x, y)" to
// sensors_inject_touch(): there is no separate "start of stroke" bit at the
// sample level, apps derive pressed/moved themselves from the fingers/x/y
// sequence, exactly like they do for real touch. Clamping happens here, not
// in devlink.c (tools/README-devlink.md documents that devlink itself does
// not clamp): this mirrors what runtime_core.c's touch drain does for
// hardware samples, so a careless script cannot inject a coordinate no real
// finger could ever produce.
static uint16_t devlink_clamp(int v, int maxInclusive) {
    if (v < 0) return 0;
    if (v > maxInclusive) return (uint16_t)maxInclusive;
    return (uint16_t)v;
}

static void devlink_hook_down(int x, int y) {
    sensors_inject_touch(1, devlink_clamp(x, PANEL_W - 1), devlink_clamp(y, PANEL_H - 1));
}

static void devlink_hook_move(int x, int y) {
    sensors_inject_touch(1, devlink_clamp(x, PANEL_W - 1), devlink_clamp(y, PANEL_H - 1));
}

static void devlink_hook_up(void) {
    sensors_inject_touch(0, 0, 0);
}

// The old link's ERASE hook called the sketchpad's wipe_erase() directly.
// There is no cross-app equivalent of that any more, and inventing a global
// "erase" verb would contradict sensors.h's own reasoning for why shake is
// opt-in per app: a stopwatch's number and a sketchpad's drawing are both
// things a child carries across a room to show someone, and neither should
// be one stray command away from being wiped by an app that never asked for
// that power. So ERASE is wired to a synthetic shake instead of a drawing
// operation: it goes through the exact channel a real shake does
// (sensors_erase_seq(), via sensors_inject_erase()), which only ever reaches
// an app that already opted in with wantsShake. On an app that did not opt
// in (today: every app except the sketchpad), ERASE is a no-op, same as
// physically shaking the device would be.
static void devlink_hook_erase(void) {
    sensors_inject_erase();
}

static int devlink_hook_app_current(void) {
    return app_current();
}

// g_menuApp is declared here the same way runtime_core.c declares it (a
// direct extern, not by including apps/menu.h): this file only needs the
// name field devlink_hook_app_name() below reads, and app.h already gives
// the type.
extern const app_t g_menuApp;

static const char *devlink_hook_app_name(int index) {
    if (index == APP_INDEX_MENU) return g_menuApp.name;
    if (index < 0 || index >= g_appCount) return NULL;
    return g_apps[index]->name;
}

// Out-of-range (and menu-index) requests are refused here rather than
// forwarded to app_switch_to(), which indexes g_apps[] with no bounds check
// of its own: a bad index from a script should get an "ERR range" reply at
// worst, never an out-of-bounds read a few frames later when the deferred
// switch actually applies. The menu is excluded on purpose too: it is the
// shell a child uses to pick an app, not one of the apps devlink is meant to
// drive to.
static bool devlink_hook_app_switch(int index) {
    if (index < 0 || index >= g_appCount) return false;
    app_switch_to(index);
    return true;
}

// A generous, few-second watchdog timeout: the 182ms cold boot this
// architecture exists to buy back (see docs/decisions/0002 section 1) makes
// a watchdog reboot nearly invisible, so there is no reason to cut this
// close.
#define RUNTIME_WATCHDOG_MS 4000

/* ---- profiler --------------------------------------------------------
 *
 * Loop count and sensors_stats() deltas, once a second, modelled on
 * firmware/main.c's #if PROFILE block. Printing every loop would make the
 * printf itself the thing being measured; once a second is cheap enough to
 * leave on by default and still says nothing about steady-state cost.
 * Board-only (see this file's header comment): it reads
 * rtcore_last_switch_name()/_us() rather than keeping its own copy, since
 * do_switch() moved to runtime_core.c along with those two values.
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
        // arena trap in runtime_core.c, so this is the one failure in this
        // file that cannot be made visible on the panel; it can only be
        // visible on the serial console. Hang rather than stumble on with a
        // NULL gfx_fb.
        for (;;) { }
    }

    // devlink (screenshots + touch/erase injection + app navigation, for an
    // agent driving the board without a human) only touches the framebuffer
    // and the wrapper functions above, none of which go near i2c1, so it is
    // safe to wire in here, before sensors_start() hands that bus to core1.
    devlink_hooks_t devlinkHooks = {
        .fb = gfx_fb,
        .w = PANEL_W,
        .h = PANEL_H,
        .inject_down = devlink_hook_down,
        .inject_move = devlink_hook_move,
        .inject_up = devlink_hook_up,
        .erase = devlink_hook_erase,
        .app_current = devlink_hook_app_current,
        .app_name = devlink_hook_app_name,
        .app_switch = devlink_hook_app_switch,
    };
    devlink_init(&devlinkHooks);

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

    rtcore_init(); // enter app 0 (chrono)

#if PROFILE
    g_profLastMs = to_ms_since_boot(get_absolute_time());
    sensors_stats(&g_profLastStats);
#endif

    for (;;) {
        uint32_t nowMs = to_ms_since_boot(get_absolute_time());

        rtcore_tick(nowMs);

        // devlink: drains whatever an agent already sent (a screenshot
        // request, an injected touch or erase, an app query or switch) and
        // replies. Never blocks (see devlink.h), so it cannot turn this loop
        // into something the watchdog below has to save it from.
        devlink_poll();

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
            const char *switchName = rtcore_last_switch_name();
            printf("prof app=%s switch=%luus | loops=%lu/s | touch reads=%lu timeouts=%lu drops=%lu recoveries=%lu "
                   "| imu timeouts=%lu | pmic timeouts=%lu\r\n",
                   switchName ? switchName : "?",
                   (unsigned long)rtcore_last_switch_us(),
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
