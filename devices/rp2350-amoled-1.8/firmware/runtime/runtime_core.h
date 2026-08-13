/*
 * runtime_core: the portable half of runtime.c, split out per
 * docs/decisions/0003-emulator-runs-the-real-apps.md so it can compile for
 * both the board and wasm32-freestanding. Holds the arena, the app table,
 * app switching, and the per-frame input resolution and dispatch: everything
 * decision 0003 calls "a portable core: the arena, the app table, the
 * deferred switch, and the frame dispatch. No pico-sdk, no hardware."
 *
 * runtime_core.c depends on ONLY app.h, gfx.h, sensors.h and freestanding C
 * headers - never pico-sdk, never anything under hardware/ or pico/. That is the
 * whole point of the split: if the portable core only ever reaches outside
 * itself through those two runtime seams, then supplying gfx.h and
 * sensors.h from a browser is enough to run the real firmware there, not a
 * reimplementation of it.
 *
 * The four functions below are the one place that rule bends, and only
 * because things runtime.c used to do inline - print a diagnostic line, time
 * a switch, hold the board on a fatal arena overflow, and (since the PWR-held
 * power-off gesture) set the panel's brightness - are either pico-sdk calls
 * (printf's stdio lock, time_us_32(), watchdog_update(), sleep_ms()) with no
 * equivalent in a freestanding wasm module, or board-specific hardware calls
 * (AMOLED_1IN8_SetBrightness(), declared in a header the wasm build's own
 * shim deliberately does not carry - see shim/AMOLED_1in8.h's header
 * comment). Each target implements these four; none is part of emu_abi.h's
 * host-import contract (env.js_log, the math functions) - they are compiled
 * INTO the wasm module by emu_shim.c, same as gfx_init()'s malloc, so they
 * never touch the list the JS side is written against.
 */
#ifndef RUNTIME_CORE_H
#define RUNTIME_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ---- host-provided (see this file's header comment for why) ------------ */

// Diagnostic text, no trailing newline. On the board this wraps printf
// (adding \r\n itself); in wasm it is forwarded to env.js_log. The core's
// only escape hatch to a logging facility - it never calls printf directly.
void rt_log(const char *msg);

// A free-running microsecond clock, for timing do_switch()'s cost the same
// way runtime.c always did (time_us_32(), a pico-sdk call this file cannot
// make). The board returns the real hardware counter. wasm has no such
// counter - see docs/decisions/0003's "What is not real" section, which
// already rules timing out of the emulator's job - so its implementation
// returns a value derived from the host clock emu_tick() was last called
// with; a switch's measured cost there reads as 0us, which is honest rather
// than a fabricated number, not a bug.
uint32_t rt_time_us(void);

// Called only when app_alloc() detects an app's state does not fit
// APP_ARENA_BYTES: a build-time bug (see app_alloc()'s comment in
// runtime_core.c), never a condition a shipped app's session can trigger.
// Must not return. The board's implementation matches runtime.c's
// pre-split arena_overflow_trap() exactly: paint the panel red and hold
// there, watchdog fed, forever - see its comment for why a reboot here
// would be worse than no trap at all. wasm has no watchdog and cannot block
// a call without hanging the tab, so it logs and traps the module instead;
// unreachable by any of today's shipped apps on either target, so this
// divergence is never actually observed.
void rt_halt(void);

// Sets the panel's brightness, 0-100 percent. Used by the PWR-held-alone
// power-off gesture (runtime_core.c) to fade the screen out while the
// button is held, rather than cutting abruptly - see that file's "the
// PWR-held-alone power-off gesture" section for the full reasoning. The
// board's implementation is a single QSPI write (AMOLED_1IN8_SetBrightness,
// same bus gfx_push() already drives every frame, NOT i2c1 - see runtime.c).
// wasm has no real panel to dim: browser chrome is not part of emu_abi.h's
// exported surface and extending it is outside what this hook is for, so
// the wasm implementation is a documented no-op, not a simulated fade - see
// emu_shim.c. What the emulator DOES still exercise is the DECISION logic
// that calls this (whether and when the ramp runs, whether the chord
// suppresses it, whether release restores it), via the rt_log() lines that
// logic writes at the moments that matter.
void rt_set_brightness(uint8_t percent);

/* ---- lifecycle: what the host drives ------------------------------------
 *
 * rtcore_init() enters app 0, through the same path a later switch takes
 * (do_switch()), so its timing is measured the same way. rtcore_tick() is
 * one frame: resolve input, hand the current app its turn, apply a deferred
 * switch if tick() requested one. nowMs is the ONLY time source in here -
 * see emu_abi.h's emu_tick() comment for why a firmware reading its own
 * clock breaks reproducibility.
 */
void rtcore_init(void);
void rtcore_tick(uint32_t nowMs);

/* ---- accessors for runtime.c's profiler ---------------------------------
 *
 * The once-a-second profiler line stays board-only (see runtime.c), but the
 * switch it repeats (see do_switch()'s comment on why printing it once is
 * not enough) is now recorded in this file, since do_switch() moved here
 * too. These are how the profiler still gets at it.
 */
const char *rtcore_last_switch_name(void);
uint32_t rtcore_last_switch_us(void);

/* ---- the menu's private slot ---------------------------------------------
 *
 * Not a valid g_apps[] index by construction (negative), so app_for_index()
 * can tell "the menu" apart from "app N" with one comparison. Exposed here
 * (rather than kept file-static in runtime_core.c, as it was in the
 * pre-split runtime.c) because runtime.c's devlink wiring - which stays
 * board-only - needs the same constant to report "MENU" for this index in
 * its app_name hook, and must not carry its own, possibly-drifting copy.
 */
#define APP_INDEX_MENU (-1)

#endif // RUNTIME_CORE_H
