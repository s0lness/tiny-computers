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
#include "sound.h"
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

// See runtime_core.h for why this hook exists. AMOLED_1IN8_SetBrightness()
// (firmware/lib/AMOLED/AMOLED_1in8.c) is a single QSPI register write
// (command 0x51, one data byte), the same bus gfx_push() already drives
// every frame - NOT i2c1, so calling it from core0 does not touch the
// ownership rule sensors.h enforces (touch/IMU/PMIC are the i2c1 parts;
// the panel is a separate bus entirely). Cost is the same order as one
// gfx_push() call, a handful of microseconds of PIO/DMA time, not
// milliseconds; runtime_core.c's power-off gesture only calls this when the
// requested percentage actually changed (see set_brightness_if_changed()
// there), so across the whole 3.5s dim ramp this fires at most a few dozen
// times, not once a frame.
//
// AMOLED_1IN8_SetBrightness() already takes a 0-100 percent and clamps it
// internally before scaling to the panel's 0-255 range, despite main()
// below calling it with a literal 180 at boot - that 180 clamps down to 100
// inside the function itself, so today's boot brightness is already this
// panel's maximum, not some intermediate value (see runtime_core.c's
// PWR_BASELINE_BRIGHTNESS_PCT, which relies on this). No second clamp is
// needed here.
void rt_set_brightness(uint8_t percent) {
    AMOLED_1IN8_SetBrightness(percent);
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

// Maps devlink's hardware-blind key names onto sensors.h's actual PMIC bit
// values. This is the one place that knowledge lives: devlink.h's
// devlink_key_t deliberately does not know these values (see its comment),
// so that devlink.c never has to include sensors.h.
static void devlink_hook_inject_key(devlink_key_t which) {
    switch (which) {
        case DEVLINK_KEY_PRESS:   sensors_inject_key(KEY_PRESS); break;
        case DEVLINK_KEY_LONG:    sensors_inject_key(KEY_LONG); break;
        case DEVLINK_KEY_SHORT:   sensors_inject_key(KEY_SHORT); break;
        case DEVLINK_KEY_RELEASE: sensors_inject_key(KEY_RELEASE); break;
    }
}

static void devlink_hook_inject_boot(bool down) {
    sensors_inject_boot(down);
}

static void devlink_hook_inject_boot_click(void) {
    sensors_inject_boot_click();
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

// How long core1 may go without advancing its loop counter before core0
// treats it as dead and acts - see the core1-liveness guard at the
// watchdog_update() call site for the full argument. Comfortably under
// RUNTIME_WATCHDOG_MS: core1's loop runs above 1M/s in normal operation,
// so any stall past a handful of milliseconds is already anomalous, and
// this leaves margin before the watchdog's own independent timer expires.
#define CORE1_STALL_MS 1500

// RECOVERY_HOLD_MS / RECOVERY_MAX_ATTEMPTS and the s_recoveryAttempts /
// s_recoverySuccessMs state they governed are DELETED here, not just
// unused: the in-place recovery path they belonged to
// (sensors_restart_core1(), called RECOVERY_MAX_ATTEMPTS times before
// falling back to a reboot) is not being called from this file any more -
// see the guard's own comment at the watchdog_update() call site for why.
// Leaving dead constants and dead state sitting next to the code that no
// longer reads them is exactly the kind of thing that survives a rewrite
// by accident and confuses the next person chasing this; deleting them is
// cheaper than a comment explaining they do nothing. sensors_restart_
// core1() itself stays in sensors.c, callable, pending the investigation
// the coordinator asked for.

// PERMANENT: takes the panel over the instant core1 is confirmed dead - see
// the guard's own comment at the watchdog_update() call site for why this
// does not violate decision 0002 section 4b. A solid, unmistakable colour
// no app on this device uses (every app here is white paper with black or
// coloured ink on it, never a full-bleed fill), pushed once. Deliberately
// no text: this device has no font renderer (digits.c draws seven-bar
// numerals, not general text), and a colour that cannot be mistaken for
// anything an app would ever draw is enough to answer the one question
// that matters - "is this device broken or just idle" - without needing to
// be read.
static void runtime_show_dead_screen(void) {
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, px_swap(0xF800)); // RGB565 red
    gfx_push_all();
}

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

// PERMANENT: state for the core1-liveness watchdog guard - see its comment
// at the watchdog_update() call site. Declared outside #if PROFILE
// deliberately: this is a safety mechanism, not a diagnostic, and must not
// depend on that flag.
static uint32_t s_core1LastLoops = 0;
static uint32_t s_core1LastChangeMs = 0;

#if PROFILE
static uint32_t g_profLoops = 0;
static uint32_t g_profLastMs = 0;
static sensors_stats_t g_profLastStats;
// TEMPORARY diagnostic: see sensors.h's sensors_debug_core1_loops() comment.
// Remove alongside it once closed.
static uint32_t g_profLastCore1Loops = 0;
// TEMPORARY diagnostic: i2c1 hardware status captured right after
// sound_init(), before sensors_start() - see main()'s comment there for why
// this is captured into globals instead of printf'd on the spot. Printed
// once, the first time the PROFILE block runs.
static uint32_t g_i2c1PostSoundEnable, g_i2c1PostSoundEnableStatus, g_i2c1PostSoundStatus,
                g_i2c1PostSoundRawIntrStat, g_i2c1PostSoundTxAbrtSource,
                g_i2c1PostSoundTxflr, g_i2c1PostSoundRxflr;
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
    // sound_init() belongs here, not inside sensors_init(): it brings up the
    // ES8311 over i2c1 (the codec's control interface) plus the I2S PIO/DMA
    // path, and per sensors.h's ownership rule this is still safe only
    // because core1 has not launched yet. Kept as its own function/file
    // rather than folded into sensors.c/sensors.h so the sound service never
    // has to touch the file that owns the i2c1 ownership rule itself - see
    // sound.h's header comment for the full argument.
    // TEMPORARILY DISABLED, 2026-08-13, and this is a product decision rather
    // than a diagnosis. With sound_init() active, core1 freezes at
    // PHASE_PMIC_CLR within a few seconds of every boot, which kills touch,
    // the IMU and BOTH BUTTONS. The owner has been picking up an unusable
    // device all day because of it.
    //
    // A toy that responds to nothing is worse than a toy that does not chime,
    // so the chime goes away until the race below is actually understood. The
    // code stays, the call does not. Re-enable it the moment the cause is
    // known: the owner explicitly likes this chime and it is not the thing at
    // fault, only the thing that reveals the fault.
    // sound_init();
    // BISECT RESULT (coordinator's hypothesis): with sound_init() skipped
    // and core1_install_fault_handlers() also disabled (the fast ~3.5s
    // repro configuration), core1 ran clean for 25s straight - no freeze,
    // raw loops 18.3M -> 51.1M, phase cycling normally the whole time, where
    // the same config with sound_init() active froze at PHASE_PMIC_CLR
    // within ~3.5s, twice. That correlates the freeze with sound_init()
    // running.
    //
    // NOT YET a clean causal proof, and this comment previously overclaimed
    // one: sound_init() itself costs real boot time (es8311_bring_up()'s own
    // sleep_ms(20) plus roughly twenty i2c1 register writes, ~20+ms total),
    // and this same investigation has separately shown the race is
    // sensitive to far smaller perturbations than that - reading four i2c1
    // hardware registers with no sleep and no printf (see below) was ALSO
    // enough to stop the freeze reproducing, in a run where sound_init() was
    // fully active. So "removing sound_init() removes ~20ms of boot delay"
    // and "removing sound_init() removes whatever i2c1 corruption it
    // causes" are BOTH still consistent with everything observed so far.
    // The test that would separate them - replace sound_init() with a
    // sleep_ms() of matched duration and no i2c1 traffic, see if the freeze
    // stays away regardless - has not been run yet.
    // TEMPORARY diagnostic: capture i2c1's own hardware status right after
    // sound_init() returns and before sensors_start() hands the bus to
    // core1 - the coordinator's cheapest next check. Still single-threaded
    // core0 here, so this is a plain register read, not a transaction, and
    // does not touch the ownership rule.
    //
    // Captured into globals and printed LATER (see PROFILE block below),
    // deliberately NOT printf'd here: a printf this early, before any host
    // has connected to drain stdio_usb, can itself cost up to ~2ms PER
    // CHARACTER (the SDK's own per-write timeout - see f360ccd's commit
    // message on why that number is 2ms and not the old 500ms), so a ~150
    // character line here could silently add up to hundreds of
    // milliseconds of exactly the boot delay this whole investigation is
    // about - discovered by watching this exact printf make the freeze stop
    // reproducing. Reading four registers into four words costs nothing
    // comparable.
    g_i2c1PostSoundEnable = i2c_get_hw(I2C_PORT)->enable;
    g_i2c1PostSoundEnableStatus = i2c_get_hw(I2C_PORT)->enable_status;
    g_i2c1PostSoundStatus = i2c_get_hw(I2C_PORT)->status;
    g_i2c1PostSoundRawIntrStat = i2c_get_hw(I2C_PORT)->raw_intr_stat;
    g_i2c1PostSoundTxAbrtSource = i2c_get_hw(I2C_PORT)->tx_abrt_source;
    g_i2c1PostSoundTxflr = i2c_get_hw(I2C_PORT)->txflr;
    g_i2c1PostSoundRxflr = i2c_get_hw(I2C_PORT)->rxflr;

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
        .inject_key = devlink_hook_inject_key,
        .inject_boot = devlink_hook_inject_boot,
        .inject_boot_click = devlink_hook_inject_boot_click,
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

        // sound: refills whichever I2S DMA buffer just finished playing (or
        // does nothing, most calls - see sound_poll()'s own comment). Never
        // touches i2c1 (sound.h's header comment), so this has nothing to do
        // with the ownership rule sensors_start() below is about; it just
        // needs to run every loop the same way devlink_poll() does.
        sound_poll();

        // devlink: drains whatever an agent already sent (a screenshot
        // request, an injected touch or erase, an app query or switch) and
        // replies. Never blocks (see devlink.h), so it cannot turn this loop
        // into something the watchdog below has to save it from.
        devlink_poll();

        // ONE recovery mechanism, deliberately alone. History, because it
        // matters here: this block has been a forced full-chip reboot with
        // a red-screen takeover (the red screen was a genuine win but its
        // OWN trigger, tied to the same recovery path, produced a worse
        // failure than the bug it revealed - a device stuck red is worse
        // than one that silently misses a press); then a plain revert with
        // no intervention at all (safe, but the first wedge is terminal
        // again); this is the coordinator's explicit combination of the
        // two: recovery stays, the red screen and the forced reboot do
        // not. One mechanism at a time, so what recovery alone does is
        // actually observable rather than folded into two other effects.
        //
        // sensors_restart_core1() (sensors.c): a hardware reset of core1
        // plus an i2c1 bus recovery (release SDA, clock SCL by hand, issue
        // a STOP, reinit the peripheral, clear the PMIC's own IRQ status
        // registers) plus a fresh multicore_launch_core1(). Runs every
        // time core1 is confirmed dead (CORE1_STALL_MS with no loop-count
        // change), with no attempt limit and no fallback to a reboot: a
        // wedge costs one invisible restart rather than a lost device, and
        // - independently of whether recovery ever runs at all -
        // pmic_poll_core1() now publishes a real press to g_keyEvent
        // BEFORE attempting the clear write that can wedge, so the press
        // itself survives even a wedge, not just core1's ability to keep
        // running afterward.
        {
            uint32_t core1LoopsNow = sensors_debug_core1_loops();
            if (core1LoopsNow != s_core1LastLoops) {
                s_core1LastLoops = core1LoopsNow;
                s_core1LastChangeMs = nowMs;
            }

            uint32_t deadMs = nowMs - s_core1LastChangeMs;
            if (deadMs < CORE1_STALL_MS) {
                watchdog_update();
            } else {
                sensors_restart_core1();
                // Give the freshly-launched core1 its own full CORE1_STALL_MS
                // before judging it again, rather than immediately
                // re-triggering because the counter has not moved yet.
                s_core1LastLoops = sensors_debug_core1_loops();
                s_core1LastChangeMs = nowMs;
                watchdog_update();
            }
        }

#if PROFILE
        g_profLoops++;
        if (nowMs - g_profLastMs >= 1000) {
            g_profLastMs = nowMs;
            sensors_stats_t cur;
            sensors_stats(&cur);
            const char *switchName = rtcore_last_switch_name();
            // core1 liveness: PERMANENT, part of the prof line itself, not a
            // separate DBG line - a standing guard, not a temporary
            // diagnostic (coordinator's instruction: "a dead core1 must
            // never again be silently invisible", and a prior version of
            // this printed it on its own DBG line, which is exactly the
            // kind of thing that gets dropped or missed). core0 renders
            // fine and looks perfectly healthy with core1 dead underneath
            // it - that is the entire danger this counter exists to catch,
            // so it lives in the one line nobody can miss. "DEAD" is
            // printed literally, not just a 0, so a human skimming the log
            // does not have to do arithmetic to notice.
            uint32_t core1Loops = sensors_debug_core1_loops();
            uint32_t core1LoopsPerSec = core1Loops - g_profLastCore1Loops;
            printf("prof app=%s switch=%luus | loops=%lu/s | core1=%lu/s%s core1restarts=%lu | touch reads=%lu timeouts=%lu drops=%lu recoveries=%lu "
                   "| imu timeouts=%lu | pmic timeouts=%lu poweroff cmds=%lu reg10h=%02lx->%02lx | shot drops=%lu\r\n",
                   switchName ? switchName : "?",
                   (unsigned long)rtcore_last_switch_us(),
                   (unsigned long)g_profLoops,
                   (unsigned long)core1LoopsPerSec,
                   core1LoopsPerSec == 0 ? " DEAD" : "",
                   // PERMANENT: how many times sensors_restart_core1() has
                   // run this boot (sensors.c) - the guard's own recovery
                   // count, not sensors_stats()'s touchRecoveries (below,
                   // a pre-existing, different counter for the touch
                   // controller's own stall recovery). A bus that wedges
                   // constantly should be visible here, not silently and
                   // endlessly papered over by a guard that just keeps
                   // fixing it.
                   (unsigned long)sensors_debug_i2c1_recoveries(),
                   (unsigned long)(cur.touchReads - g_profLastStats.touchReads),
                   (unsigned long)(cur.touchTimeouts - g_profLastStats.touchTimeouts),
                   (unsigned long)(cur.touchQueueDrops - g_profLastStats.touchQueueDrops),
                   (unsigned long)(cur.touchRecoveries - g_profLastStats.touchRecoveries),
                   (unsigned long)(cur.imuTimeouts - g_profLastStats.imuTimeouts),
                   (unsigned long)(cur.pmicTimeouts - g_profLastStats.pmicTimeouts),
                   (unsigned long)(cur.poweroffCmds - g_profLastStats.poweroffCmds),
                   // reg10h: NOT a delta, unlike everything else on this line - a
                   // register readback is a snapshot, not a count, and printing a
                   // difference of two register values would be meaningless. Reads
                   // ffffffff->ffffffff until the first shutdown attempt this boot
                   // (see sensors.h's sensors_stats_t comment on the two sentinels);
                   // a real attempt prints actual register byte values instead.
                   (unsigned long)cur.poweroffRegBefore,
                   (unsigned long)cur.poweroffRegAfter,
                   // shot drops: cumulative, not a delta - see devlink.h's
                   // devlink_dropped_shots() comment. Bumped only when a SHOT
                   // reply's body was cut short because the host stopped
                   // draining the port mid-transfer (devlink.c's
                   // DEVLINK_SHOT_BUDGET_US); this is the fix for the freeze
                   // that used to reboot the board in exactly that situation
                   // (see devlink.c's top-of-block comment), and this counter
                   // is what proves a truncation happened rather than nothing
                   // running at all.
                   (unsigned long)devlink_dropped_shots());
            // TEMPORARY diagnostic line: the DETAIL behind the permanent
            // core1=.../s in the prof line above (raw count, phase, fault
            // capture) - that summary is what stays forever; this line is
            // what a live investigation still needs and can be dropped once
            // the investigation closes. Reuses core1Loops/core1LoopsPerSec
            // computed above rather than reading the counter twice.
            printf("DBG core1 loops=%lu/s (raw=%lu) phase=%lu fault kind=%lu pc=%08lx lr=%08lx cfsr=%08lx faultphase=%lu "
                   "halted=%d stackheadroom=%lu writewait=%lu writewaitspins=%lu\r\n",
                   (unsigned long)core1LoopsPerSec,
                   (unsigned long)core1Loops,
                   (unsigned long)sensors_debug_core1_phase(),
                   (unsigned long)sensors_debug_core1_fault_kind(),
                   (unsigned long)sensors_debug_core1_fault_pc(),
                   (unsigned long)sensors_debug_core1_fault_lr(),
                   (unsigned long)sensors_debug_core1_fault_cfsr(),
                   (unsigned long)sensors_debug_core1_fault_phase(),
                   // SYSCFG_PROC_CONFIG.PROC1_HALTED and core1's stack
                   // high-water mark - see sensors.h's comment. Both are
                   // answerable regardless of whether core1 can execute.
                   // Coordinator's LOCKUP hypothesis was tested and ruled
                   // out with these (halted=0 during a live freeze); the
                   // stack reading itself came back suspicious (2048/2048,
                   // i.e. zero bytes ever touched, which is not credible
                   // for a core making function calls) and should not be
                   // trusted until that is explained - keeping it printed
                   // so any fix to it is visible in the same place.
                   (int)sensors_debug_core1_halted(),
                   (unsigned long)sensors_debug_core1_stack_headroom(),
                   // Which of i2c1_write_bytes_bounded()'s two wait loops
                   // core1 is (or was, at death) inside, and how many passes
                   // through THAT loop's current entry - see that function's
                   // comment. A frozen (non-advancing across two readings a
                   // second apart) spin count despite the loop's own
                   // time_reached() check would mean the CPU is not looping
                   // in software here at all, but stalled on the single
                   // register load the while-condition evaluates.
                   (unsigned long)sensors_debug_core1_write_wait_kind(),
                   (unsigned long)sensors_debug_core1_write_wait_spins());
            // TEMPORARY diagnostic: write-path transaction progress and the
            // PMIC write self-test counters (sensors.h). wr=started/returned
            // answers "did the write function itself ever return"; addr and
            // pushed say which slave and how many bytes of the current or
            // last write made it into the FIFO. selftest=writes/fails stays
            // 0/0 unless the build was made with PMIC_WRITE_SELFTEST=1.
            {
                uint32_t wrS, wrR, wrA, wrB, stW, stF;
                sensors_debug_i2c1_write_progress(&wrS, &wrR, &wrA, &wrB);
                sensors_debug_pmic_selftest(&stW, &stF);
                printf("DBG i2c1 wr started=%lu returned=%lu addr=%02lx pushed=%lu selftest=%lu/%lu\r\n",
                       (unsigned long)wrS, (unsigned long)wrR, (unsigned long)wrA,
                       (unsigned long)wrB, (unsigned long)stW, (unsigned long)stF);
            }
            // TEMPORARY diagnostic: the i2c1 status captured once, right
            // after sound_init() (main()'s comment). Printed every second
            // (not just once) specifically so a host that connects late -
            // as every capture during this investigation has - still sees
            // it, instead of it being a one-shot line lost before anyone was
            // listening.
            printf("DBG i2c1 post-sound_init: enable=%lu enable_status=%08lx status=%08lx "
                   "raw_intr_stat=%08lx tx_abrt_source=%08lx txflr=%lu rxflr=%lu\r\n",
                   (unsigned long)g_i2c1PostSoundEnable, (unsigned long)g_i2c1PostSoundEnableStatus,
                   (unsigned long)g_i2c1PostSoundStatus, (unsigned long)g_i2c1PostSoundRawIntrStat,
                   (unsigned long)g_i2c1PostSoundTxAbrtSource, (unsigned long)g_i2c1PostSoundTxflr,
                   (unsigned long)g_i2c1PostSoundRxflr);
            // TEMPORARY diagnostic: i2c1's LIVE status, only once core1 is
            // already observed dead this tick (core1LoopsPerSec == 0) - see
            // sensors_debug_i2c1_live()'s ONE CAVEAT comment for why this
            // gate is not optional (tx_abrt_source is clear-on-read and a
            // live core1 depends on seeing it). This is the reading that
            // matters: the bus state AT the hang, not at boot.
            if (core1LoopsPerSec == 0) {
                uint32_t en, enSt, st, ris, tas, txf, rxf;
                sensors_debug_i2c1_live(&en, &enSt, &st, &ris, &tas, &txf, &rxf);
                // Coordinator's decode of an earlier reading (status=0x27 =
                // ACTIVITY+TFNF+TFE+MST_ACTIVITY, raw_intr_stat=0x510 =
                // TX_EMPTY+ACTIVITY+START_DET with STOP_DET absent,
                // tx_abrt_source=0): a START was issued, the TX FIFO ran
                // dry, and no STOP ever followed - the master is still
                // waiting for the bus to let it finish, which the
                // peripheral itself does not consider an error. SDA/SCL
                // below is the direct check that decode calls for: read as
                // plain GPIO pad levels, bypassing the peripheral, the
                // classic "a slave is holding the bus low" signature.
                bool sda, scl;
                sensors_debug_i2c1_pins(&sda, &scl);
                printf("DBG i2c1 LIVE (core1 dead this tick): enable=%lu enable_status=%08lx status=%08lx "
                       "raw_intr_stat=%08lx tx_abrt_source=%08lx txflr=%lu rxflr=%lu sda=%d scl=%d\r\n",
                       (unsigned long)en, (unsigned long)enSt, (unsigned long)st,
                       (unsigned long)ris, (unsigned long)tas, (unsigned long)txf, (unsigned long)rxf,
                       (int)sda, (int)scl);
            }
            g_profLastCore1Loops = core1Loops;
            g_profLastStats = cur;
            g_profLoops = 0;
        }
#endif
    }
}
