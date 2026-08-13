/*
 * emu_abi: the contract between a firmware compiled to WebAssembly and the
 * emulator that runs it in a browser.
 *
 * THE ONE IDEA. The emulator runs the REAL firmware. Application code
 * compiles to wasm unmodified, and the browser supplies what the board would
 * have supplied: a surface to push pixels at, input devices, and a clock. Not
 * "the same algorithm" as the device. The same object code.
 *
 * The alternative, a careful reimplementation in TypeScript, was tried here
 * first and is what this replaces. It was correct on the day it was written
 * and stale by the next commit, which is the predictable behaviour of two
 * implementations of one thing: they agree exactly once, and drift from then
 * on with no test that can notice. The moment that became concrete was a real
 * bug on real hardware that the emulator was asked to reproduce, and could
 * not, because it was a bug in C and the emulator was not running any C. See
 * docs/decisions/0003-emulator-runs-the-real-apps.md.
 *
 * NOTHING BELOW NAMES THIS PARTICULAR DEVICE. A firmware declares its own
 * shape through emu_device(), and the emulator builds its chrome from that
 * declaration: panel size, buttons, sensors, and whatever else. That is not
 * generality for its own sake. An emulator that hardcodes a 368x448 panel and
 * two buttons called PWR and BOOT is a tool exactly one project can use, and
 * the cost of not doing this now is that it can never be done later.
 *
 * WHAT IS REAL, AND WHAT IS NOT
 *
 * Real, because it is the same object code: all application logic, layout and
 * redraw decisions; the framebuffer and its pixel format; whatever partial
 * refresh rules the firmware's own push path enforces.
 *
 * NOT real, and never to be trusted here:
 *   - Timing. The browser's clock drives emu_tick(). Nothing reproduces bus
 *     latency, panel push cost, or a second core. Any question about
 *     responsiveness is a question for the hardware. Always.
 *   - Input device defects. Real touch controllers drop contact mid stroke
 *     and emit strays, and firmware carries a lot of code that exists only
 *     because of that. A clean mouse drag exercises none of it. The emulator
 *     can synthesise those defects, and should, but off by default and
 *     clearly labelled when on.
 *   - The display as a physical object: no burn-in, no brightness, no
 *     tearing.
 */
#ifndef EMU_ABI_H
#define EMU_ABI_H

#include <stdint.h>

/*
 * Everything here is exported to JavaScript. Anything that is not a scalar is
 * returned as a byte offset into the module's linear memory, which is the
 * only thing a wasm export can hand back.
 */

/* ---- what this device is ------------------------------------------------
 *
 * Returns a NUL-terminated JSON string. The emulator reads it once at
 * startup and builds everything it shows from it. Unknown fields are ignored,
 * so a firmware may declare more than a given emulator version understands.
 *
 * {
 *   "name":  "RP2350-Touch-AMOLED-1.8",
 *   "panel": { "w": 368, "h": 448, "format": "rgb565be" },
 *   "buttons": [
 *     { "id": "boot", "label": "BOOT", "edge": "right", "at": 0.38 },
 *     { "id": "pwr",  "label": "PWR",  "edge": "right", "at": 0.62,
 *       "longPressMs": 1500 }
 *   ],
 *   "touch":   { "points": 1 },
 *   "sensors": [ { "id": "shake", "kind": "event" } ],
 *   "apps":    [ "chrono", "draw", "timer" ],
 *   "gestures": [
 *     { "id": "menu", "label": "menu",
 *       "how": "Hold BOOT, then also hold PWR. Keep both held until PWR "
 *              "registers a long press (about 1.5s): that opens the app "
 *              "menu. Do the same chord again to close it and return to "
 *              "what was running." }
 *   ]
 * }
 *
 * Notes on the fields that are easy to get wrong:
 *
 *   panel.format  "rgb565be" means the framebuffer holds RGB565 with the
 *                 bytes in the order the panel's DMA wants, which on a
 *                 little-endian CPU is the opposite of how a uint16_t is
 *                 stored. The emulator unswaps when blitting rather than the
 *                 firmware handing over a tidied copy, so that what the page
 *                 displays really is the device's memory.
 *
 *   buttons[].at  where the button sits along that edge, 0 at the top. The
 *                 emulator draws it there. This exists because button
 *                 position is a real source of confusion when a device is
 *                 held rotated, and a diagram beats a paragraph.
 *
 *   buttons[].longPressMs
 *                 if the hardware itself decides what a long press is (a PMIC
 *                 that reports "long press" rather than a raw level, say),
 *                 declare its threshold so the emulator reproduces the same
 *                 verdict instead of inventing its own.
 *
 *   apps          optional. Purely so the emulator can offer a jump-to-app
 *                 control. A firmware with no such concept omits it, and the
 *                 emulator shows no strip.
 *
 *   gestures      optional. A compound gesture recognised across more than
 *                 one input (a chord, a hold-then-something) belongs to no
 *                 single button or sensor, so there is nowhere else in this
 *                 JSON to hang a description of it on. "how" is prose
 *                 describing the physical gesture in device terms (which
 *                 buttons, held how); it is NOT expected to name a
 *                 particular emulator's keyboard shortcuts, since those are
 *                 assigned dynamically per session (see the emulator's
 *                 shortcuts.ts) and would go stale here. A firmware with no
 *                 gesture beyond its individual buttons/sensors omits this,
 *                 and the emulator says plainly that none is declared
 *                 rather than guessing at one.
 */
int emu_device(void);

/* ---- lifecycle ---------------------------------------------------------- */

// Brings the firmware up, through the same path the board takes. Returns 1 on
// success, 0 on failure (and should have logged why through the host's log
// import). Call once.
int emu_init(void);

// Advances one frame. nowMs is the host's clock and is the ONLY time source
// inside the module, so a test harness can drive it deterministically rather
// than in real time. A firmware that reads its own clock has broken this and
// will not be reproducible.
void emu_tick(uint32_t nowMs);

/* ---- the panel ---------------------------------------------------------- */

// The framebuffer, in the format declared by emu_device().
int emu_fb(void);

/* ---- what the last tick pushed ------------------------------------------
 *
 * The firmware's push path records every window it sent to the panel, AFTER
 * whatever rounding or alignment that path applies. The emulator blits only
 * these, so it exercises the real partial-refresh path, and draws them as an
 * overlay.
 *
 * That overlay is the single most useful thing here. A partial-refresh bug is
 * a bug about window geometry, and on this project it cost days of bisection
 * precisely because the windows were invisible: the panel corrupts any window
 * whose row length is not a multiple of 8 pixels
 * (docs/decisions/0001-push-min-width.md). Making the windows visible turns
 * that class of bug from a bisection into a glance.
 *
 * Cleared at the start of every emu_tick().
 */
int emu_push_count(void);
int emu_push_x(int i);
int emu_push_y(int i);
int emu_push_w(int i);
int emu_push_h(int i);

/* ---- input --------------------------------------------------------------
 *
 * Coordinates are always in the panel's own, unrotated space. If a device is
 * used rotated, mapping the pointer back is the emulator's job, because the
 * firmware's own coordinate handling is under test and must not be helped.
 */
void emu_touch(int down, int x, int y);

// Buttons are identified by their index in emu_device()'s buttons array. The
// emulator reports level changes; anything derived (click, long press) is the
// firmware's business, exactly as on hardware, unless the device declared a
// longPressMs, in which case the emulator also calls emu_button_verdict().
void emu_button(int index, int down);
void emu_button_verdict(int index, int isLong);

// Sensor events declared with "kind": "event", by index into the sensors
// array. A shake, a tap, a step: anything the firmware receives as "it
// happened" rather than as a continuous value.
void emu_sensor_event(int index);

/* ---- optional: apps -----------------------------------------------------
 *
 * Only meaningful if emu_device() declared an "apps" array. A firmware
 * without a concept of apps leaves these unimplemented and the emulator will
 * not call them.
 */
int  emu_app_current(void);
void emu_app_switch(int index);

/* ---- what the module imports from the host ------------------------------
 *
 * Freestanding wasm has no libc, so the host provides these. Kept to the
 * smallest set that real firmware actually needs, since every import is
 * something a new host has to implement:
 *
 *   env.js_log(ptr, len)   UTF-8 diagnostic text. This is the firmware's
 *                          printf, and the emulator shows it in a console
 *                          pane. A device with a serial port has one; this is
 *                          the same thing.
 *
 *   env.sinf, cosf, atan2f, sqrtf, fabsf, floorf, fmodf, powf
 *                          math, mapped to the host's own. Deliberately not
 *                          reimplemented in the module: they would be a
 *                          second source of numerical difference between the
 *                          two targets, on top of the one that already exists
 *                          (the device's FPU is single precision, the host's
 *                          Math is double), and the host's are at least
 *                          correct.
 */

#endif // EMU_ABI_H
