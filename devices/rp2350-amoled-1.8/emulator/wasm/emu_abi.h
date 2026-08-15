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
 *
 * THE RULE THIS FILE EXISTS TO ENFORCE, A.K.A. KEEPING THE EMULATOR HONEST
 *
 * The emulator must never deliver an input the hardware cannot produce.
 * Where the two disagree, the emulator changes to match the hardware, never
 * the reverse. THE ONE IDEA above ("the emulator runs the real firmware")
 * is worthless as a guarantee if the browser then hands that firmware
 * inputs no board could ever generate: an emulator that is MORE GENEROUS
 * than the device it stands in for is worse than no emulator at all,
 * because it lets an app get built and tested against something that will
 * never happen on the device, and the gap does not surface until someone
 * is holding real hardware.
 *
 * Worked example, now historical but kept because it shows the rule biting
 * in both directions. sensors.h declares KEY_PRESS, KEY_LONG and KEY_SHORT
 * for the PWR key, all genuinely delivered by the AXP2101. For a while it did
 * NOT declare KEY_RELEASE: pmic_poll_core1() (firmware/runtime/sensors.c)
 * masked that bit out of every PMIC read before an app could ever see it, on
 * purpose, because nothing read it. An earlier version of emu_shim.c got this
 * backwards: it OR'd a synthetic release bit into the emulator's key event on
 * every button-up, so an app built and tested only in the browser could come
 * to depend on a signal that silently did nothing on the device. The fix then
 * was not to make the board deliver it, it was to stop the emulator inventing
 * what the board did not have.
 *
 * The requirement later changed for an unrelated reason (the PWR-held-alone
 * power-off gesture needs to know when a hold ENDS, which only the release
 * edge can say), so the board's own mask widened to deliver KEY_RELEASE for
 * real, and emu_shim.c changed to match: this is the rule's other direction,
 * an emulator that is LESS capable than the hardware it stands in for is just
 * as dishonest as one that is more generous. See sensors.h's PWR key section
 * ("KEY_RELEASE WAS DELETED FROM THIS FILE EARLIER, AND IS BACK") for the
 * full story, and emu_shim.c's PWR key section for the emulator's side of it.
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
 *   "sensors": [ { "id": "shake", "kind": "event" },
 *                { "id": "gravity", "kind": "gravity", "label": "tilt",
 *                  "unit": "g" } ],
 *   "apps":    [ "chrono", "draw", "timer", "four", "level" ],
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
 *
 *   tunables      optional. Development-only knobs a firmware exposes for
 *                 live iteration - this device's sketchpad dropout-tolerance
 *                 constants, gated behind SKETCH_LIVE_TUNE
 *                 (firmware/apps/sketch.c), are the first user of this.
 *                 Each entry is `{ "id": "lift", "min": 20, "max": 1000,
 *                 "default": 220 }`. Read-only here (the JSON is a
 *                 declaration, not a value); the emulator reads and writes
 *                 the LIVE value through emu_tune_get(index)/
 *                 emu_tune_set(index, value) below, indexed into this same
 *                 array, the same "declare the shape in JSON, act on it
 *                 through an indexed call" pattern buttons[] and
 *                 emu_button() already use. A firmware that declares no
 *                 tunables omits this key entirely and the emulator shows no
 *                 tuning panel, same as an app-less firmware omitting
 *                 "apps".
 *
 *                 THIS PANEL IS FOR FAST ITERATION, NOT FOR FINDING THE
 *                 RIGHT VALUE. The emulator's input-defect model (TouchSim)
 *                 is measurably kinder than a real touch controller - see
 *                 sketch.c's SKETCH_LIVE_TUNE comment for the 63-83 percent
 *                 (emulator) against 3.5 percent (hardware) figure that
 *                 proved it - so a value tuned here is a hypothesis, not a
 *                 result, until it is also tried on the device (devlink's
 *                 TUNE command, tools/README-devlink.md). The emulator's own
 *                 UI is expected to say this next to the controls, not just
 *                 this header comment.
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

/* Continuous vector-valued sensors, by index into the same sensors array. A
 * gravity vector, a magnetic field, an angular rate: anything the firmware
 * receives as "this is its value now" rather than as an event. Sticky, like
 * a real one: the value stands until it is set again, and the module keeps
 * sampling it at whatever cadence its own firmware samples the part.
 *
 * The unit and the axes belong to the sensor's declaration, not to this
 * call. This device declares one, `{"id":"gravity","kind":"gravity"}`,
 * whose axes are the panel's own (+x right, +y down the screen, +z into the
 * glass) and whose unit is g, so lying flat face up is (0, 0, 1) - see
 * firmware/runtime/tilt.h, which is compiled into this module and does the
 * filtering, so the emulator's orientation is the board's orientation and
 * not a second implementation of it.
 *
 * "kind": "gravity" rather than a bare "vector3" because it tells a host
 * something it cannot otherwise know: this vector is an ORIENTATION, so the
 * natural control for it is two angles (tip and roll) rather than three
 * number boxes, and the natural default is "lying flat on a table". A host
 * that does not recognise the kind ignores the sensor, exactly as it
 * already ignores any non-"event" sensor.
 *
 * KEEPING THE EMULATOR HONEST, applied to this call. The host may send any
 * vector, including ones no still hand produces, because the real part
 * genuinely reports those: a shaken device reads several g and a dropped
 * one reads zero. What the emulator CANNOT reproduce, and what therefore
 * must not be judged here, is the opposite direction: a browser slider
 * holds perfectly still and is exactly unit length, where a real
 * accelerometer in a child's hand is never still. So an orientation-driven
 * app that looks smooth in this page can still jitter on the board, and
 * tilt.h's filter constant can only be judged with a real hand on real
 * hardware. Same category as the timing and input-defect caveats above.
 *
 * KEEPING THE EMULATOR HONEST, a second way: a float coming from JavaScript
 * can trivially be a value no accelerometer could produce. The
 * implementation clamps and quantises to what the real part actually
 * delivers - for this device's QMI8658 that is +-8 g of range and 0.244 mg
 * per count - so a test cannot pass on a reading the hardware is physically
 * incapable of reporting. See emu_shim.c.
 */
void emu_sensor_vector(int index, float x, float y, float z);

/* ---- optional: what the app was last handed -----------------------------
 *
 * The orientation the CURRENT app saw on the last emu_tick(), in that app's
 * own drawing space (runtime_core.c rotates it for landscape apps). This is
 * a test oracle, not an input, and it exists because everything that can
 * actually go wrong with an orientation signal happens between the sensor
 * and the app struct: a swapped axis, an inverted sign, a landscape
 * rotation applied the wrong way, a filter that never converges. A test
 * that asserted on what it had just sent to emu_sensor_vector would be
 * reading upstream of every one of those (docs/decisions/0010: an
 * instrument validates only what is upstream of where it reads).
 *
 * field: 0 gx, 1 gy, 2 gz, 3 tiltDeg, 4 up (0 top, 1 right, 2 bottom,
 * 3 left), 5 valid (0 or 1), 6 coasting (0 or 1 - tilt.h's magnitude trust
 * gate has fully given up on the current raw sample and gx/gy/gz are
 * holding their last belief). Returns 0 for an unknown field.
 *
 * What it still cannot see, said here rather than discovered later: whether
 * the axis mapping matches the physical case. No software oracle knows
 * which way is up, on either target. Only tilt.h's on-board ritual does.
 */
float emu_tilt(int field);

/* ---- optional: apps -----------------------------------------------------
 *
 * Only meaningful if emu_device() declared an "apps" array. A firmware
 * without a concept of apps leaves these unimplemented and the emulator will
 * not call them.
 */
int  emu_app_current(void);
void emu_app_switch(int index);

/* ---- optional: live tunables ---------------------------------------------
 *
 * Only meaningful if emu_device() declared a "tunables" array (see above). A
 * firmware with nothing tunable leaves these unimplemented and the emulator
 * will not call them, same as emu_app_current/emu_app_switch when there is
 * no "apps" array.
 *
 * index is the tunable's position in the declared "tunables" array, the same
 * indexing convention emu_button()/emu_button_verdict() already use against
 * "buttons". emu_tune_get returns the CURRENT value (not necessarily the
 * declared default - a previous emu_tune_set call may have changed it).
 * emu_tune_set clamps to the declared [min, max] itself (the same clamp the
 * real device's TUNE SET applies), so the host does not have to duplicate
 * that range check; call emu_tune_get afterward if the applied value matters
 * to the caller.
 *
 * emu_tune_reset restores tunable `index` to its declared default (the
 * "default" field in the "tunables" JSON) - the emulator-side twin of the
 * real device's devlink TUNE RESET (tools/README-devlink.md). Call
 * emu_tune_get afterward for the restored value; this returns nothing
 * because, unlike the device's line-oriented reply, the host already has
 * the declared default in the "tunables" array it read at startup and does
 * not need it echoed back.
 */
float emu_tune_get(int index);
void  emu_tune_set(int index, float value);
void  emu_tune_reset(int index);

/* ---- sound ----------------------------------------------------------------
 *
 * GENUINE, not simulated, in the one sense that matters most here: the
 * samples the host plays are computed by firmware/runtime/sound_synth.c,
 * compiled into this module unmodified - the exact same object code that
 * generates the samples the board's own DMA hands to the ES8311 (see
 * firmware/runtime/sound.c). Nothing in emu_shim.c re-derives the chime in
 * JavaScript; the module hands the host raw PCM it already computed, the
 * same way emu_fb() hands over a framebuffer the module already rendered.
 * That is the same "same object code, not a reimplementation" argument
 * decision 0003 makes for the graphics and app logic, extended to sound.
 *
 * NOT real, and this belongs next to the timing and input-device caveats
 * above, not hidden below them: the device's speaker is a small, cheap part
 * that will distort and lose bass in ways whatever speaker plays this
 * browser tab will not. The emulator will always flatter the sound. It is
 * for judging the TUNE - the notes, the shape, the pacing, whether the idea
 * is right - not the TIMBRE - whether it is harsh or pleasant coming out of
 * this specific tiny speaker. That second question can only be answered on
 * the real board.
 *
 * SHAPE. sound_play()/sound_stop() (firmware/runtime/sound.h) are called by
 * firmware logic (the timer's alarm), never by the host directly - sound is
 * an output, not an input device, so there is no emu_sound_play() the host
 * calls. Instead, two counters the host diffs against what it last saw, the
 * same pattern app_current() and emu_push_count() already use:
 *
 *   emu_sound_play_seq()   bumped once per sound_play() call (once per
 *                          alarm start, not once per sample or per tick).
 *                          On a change, the host should fetch the buffer
 *                          below and start playing it.
 *   emu_sound_stop_seq()   bumped once per sound_stop() call. On a change,
 *                          the host should stop whatever is currently
 *                          playing, immediately - this is what makes
 *                          dismissal (any button, touch or shake, per
 *                          timer.c) silence the browser's audio right away
 *                          rather than letting an already-started buffer
 *                          finish, matching sound.h's "immediate" claim for
 *                          the real hardware.
 *
 * The buffer itself is fixed-length mono PCM, valid only immediately after
 * emu_sound_play_seq() has just changed (the next sound_play() call
 * overwrites it - there is one buffer, not a queue):
 *
 *   emu_sound_sample_rate()  Hz, constant.
 *   emu_sound_buffer()       byte offset into linear memory.
 *   emu_sound_frames()       sample count at that offset. int16, signed,
 *                            native (little-endian) byte order.
 *
 * The module decides how many seconds to generate up front (a fixed preview
 * length, several repeats of the chime's own phrase - see emu_shim.c),
 * independent of the real device's 30-second self-limit (timer.c's own
 * ALARM_MAX_MS, which this module does not know about: that is the TIMER's
 * self-limit, not the sound service's - see sound.h). A finite buffer the
 * host just plays once, rather than a streaming pull protocol, is enough
 * here: unlike the board (SRAM-constrained, see sound.c's header comment on
 * why IT streams sample-by-sample into a small ring instead), a browser's
 * wasm linear memory has no comparable budget problem for a few seconds of
 * mono int16 audio.
 */
int      emu_sound_sample_rate(void);
uint32_t emu_sound_play_seq(void);
uint32_t emu_sound_stop_seq(void);
int      emu_sound_buffer(void);
int      emu_sound_frames(void);

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
 *   env.sinf, cosf, atan2f, sqrtf, fabsf, floorf, fmodf, powf, expf
 *                          math, mapped to the host's own. Deliberately not
 *                          reimplemented in the module: they would be a
 *                          second source of numerical difference between the
 *                          two targets, on top of the one that already exists
 *                          (the device's FPU is single precision, the host's
 *                          Math is double), and the host's are at least
 *                          correct. expf joined this list for the alarm
 *                          chime's decay envelope (sound_synth.c); it was not
 *                          needed before sound existed.
 */

#endif // EMU_ABI_H
