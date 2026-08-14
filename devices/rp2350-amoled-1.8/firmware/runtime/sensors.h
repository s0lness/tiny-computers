/*
 * sensors: core1 owns every chip on i2c1 and publishes signals; core0
 * renders and only ever reads what is published here.
 *
 * This was a measurement, not a preference. The touch I2C read costs about
 * 695us and was roughly 98 percent of frame time, while rasterising a
 * stroke took 5 to 95us. Moving the bus off the render loop is the single
 * largest responsiveness win this firmware has.
 *
 * THE OWNERSHIP RULE, which is the whole reason this file exists:
 *
 *   Once sensors_start() has been called, core0 must NEVER touch i2c1.
 *   Not the touch controller, not the IMU, not the PMIC, not a debug read.
 *
 * The vendor demo guards the shared bus with a non-atomic
 * `while (lock); lock = 1;` on a non-volatile flag, which is not a lock at
 * all; concurrent access corrupts transactions in ways that surface as
 * unrelated failures somewhere else entirely, hours of debugging away from
 * the actual cause. The rule above is enforced by convention rather than by
 * the compiler, so it is written here in the place anyone adding a sensor
 * read will be looking.
 *
 * Corollary: nothing in the core1 path may printf. stdio takes a lock that
 * core0 also takes, which turns a diagnostic into a deadlock. Core1
 * publishes counters instead (see sensors_stats_t).
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/* ---- touch -------------------------------------------------------------
 *
 * Samples arrive through a single-producer/single-consumer ring: core1 is
 * the only writer, core0 the only reader, with a barrier at the point the
 * new element becomes visible. No lock is taken anywhere near an I2C
 * transaction, deliberately.
 *
 * sensors_touch_next() also merges in a second ring, fed by
 * sensors_inject_touch() below (the agent-facing devlink test link's
 * synthetic touches). That ring is core0-owned on both ends, so it is not a
 * second writer of the ring above; see sensors_inject_touch()'s comment and
 * sensors.c for how the merge works.
 */
typedef struct {
    uint32_t tMs;    // to_ms_since_boot() when core1 took this reading
    uint8_t  fingers;
    uint16_t x, y;   // raw, unclamped touch registers; the consumer clamps
} touch_sample_t;

// Pops one queued sample. Returns false when the queue is empty. Core0
// should drain the whole backlog each frame and push once, not push per
// sample: several samples routinely arrive between two render iterations
// now that core0 never blocks on the bus.
bool sensors_touch_next(touch_sample_t *out);

// Injects a synthetic touch sample, for the agent-facing test link. It
// enters the same queue real samples do, so an app cannot tell the
// difference and no app needs to know this exists.
//
// Callable only from core0 (devlink_poll() is the one caller, and it always
// runs on core0's main loop, same as sensors_touch_next()). It does NOT push
// into the real ring above: that ring's entire safety argument is "core1 is
// the sole writer" (see the banner above), and a second writer from core0
// would reintroduce the exact torn-sample race the barrier there exists to
// prevent. Instead it pushes into a second, core0-owned ring that
// sensors_touch_next() merges in by timestamp. See sensors.c for the merge
// and for why that ring needs no barrier at all.
void sensors_inject_touch(uint8_t fingers, uint16_t x, uint16_t y);

// Core0 -> core1. While a finger is down, shake detection is suppressed, so
// that resting a hand on the puck while drawing cannot erase the drawing.
void sensors_set_finger_down(bool down);

/* ---- power key ---------------------------------------------------------
 *
 * PWR is wired to the AXP2101's PWRON pin, not to a GPIO, so firmware only
 * ever sees it second hand: a press latches bits in PMIC register 0x49.
 * From the datasheet, confirmed on hardware:
 *
 *   0x01  release edge      0x02  press edge
 *   0x04  long press        0x08  short press (verdict, latches on release)
 *
 * Both edge interrupts are disabled by default and are enabled in register
 * 0x41 at init. The long-press interrupt fires at 1.5s (measured at 1480ms).
 * The PMIC's hard power-off (register 0x27, OFFLEVEL field) was silicon's
 * 6s default, giving only 4.5s of margin between the menu gesture's
 * long-press verdict and an unannounced power cut; sensors_init() now raises
 * it to 10s, the field's maximum, on every boot (see
 * pmic_raise_poweroff_threshold() in sensors.c), so the margin is 8.5s. That
 * 10s ceiling is now also this firmware's power-off BACKSTOP rather than its
 * only power-off mechanism - see KEY_RELEASE and sensors_request_poweroff()
 * below for the real one, and pmic_poll_core1()'s comment in sensors.c for
 * why the backstop still matters even though it does.
 *
 * KEY_RELEASE (0x01) WAS DELETED FROM THIS FILE EARLIER, AND IS BACK.
 * Register 0x41 is written 0xFF at init, so the PMIC always latches the
 * release edge into 0x49 on every press, like the other three bits; for a
 * long time nothing in this firmware asked for it, so pmic_poll_core1()
 * (sensors.c) masked it out before it ever reached g_keyEvent, and the
 * macro itself was deleted on the reasoning that a #define for a bit that
 * can never appear in app_frame_t.key is a worse trap than no macro at all
 * - it lets an app compile against a signal that silently does nothing on
 * hardware. That reasoning was correct at the time and nobody made a
 * mistake deleting it.
 *
 * The requirement changed: the owner's power-off gesture (hold PWR alone,
 * for PWR_HOLD_POWEROFF_MS - five seconds originally, 3.5 since
 * 2026-08-14) has to be timed in firmware, because neither number
 * exists as an OFFLEVEL value (4/6/8/10s only, see above), and timing a hold
 * needs to know when it ENDS, not just when it started or how long the PMIC
 * decided it had already been. KEY_PRESS alone cannot answer "is PWR still
 * down right now", and re-deriving that from KEY_LONG (a one-shot verdict
 * at 1.5s, not a level) does not cover the window between that verdict and
 * the power-off threshold at all. So the
 * mask widened again (pmic_poll_core1()'s `s1 & 0x0E` became `s1 & 0x0F`)
 * and the macro is reinstated to name the bit that mask now lets through.
 * See runtime_core.c's "the PWR-held-alone power-off gesture" section for
 * the consumer.
 */
#define KEY_PRESS   0x02
#define KEY_LONG    0x04
#define KEY_SHORT   0x08
#define KEY_RELEASE 0x01

// Reads and clears the pending key bits. Exactly one caller per loop
// iteration, or events are lost: this is a read-and-clear, not a queue.
// The runtime is that caller, and it hands apps what it does not consume
// itself (see app.h's app_key field).
uint8_t sensors_key_take(void);

// Injects PMIC key bits (any of KEY_PRESS/KEY_LONG/KEY_SHORT/KEY_RELEASE,
// OR'd together), for the agent-facing devlink test link. Merged into
// whatever sensors_key_take() next returns, with the same read-and-clear
// semantics a real PMIC event has: call once per synthetic event, not once
// per frame you want it to appear "held", or it is delivered more than
// once. devlink.c's own KEY command names all four, including RELEASE
// (see tools/README-devlink.md): without it, `KEY PRESS` alone started a
// hold that devlink had no way to end, which is exactly what made the
// PWR-held-alone power-off gesture (runtime_core.c) hard to reproduce safely
// by injection until this was added.
//
// Core0-owned end to end, like sensors_inject_erase() above, but for a
// different reason: g_keyEvent (sensors.c) is written by core1
// (pmic_poll_core1(), a plain assignment, not a read-modify-write) and
// read-and-cleared by core0 (sensors_key_take()). Writing an injected bit
// straight into that variable from core0 would make core0 a second writer of
// something core1 also writes, exactly the hazard the ownership rule at the
// top of this file exists to prevent, so injected bits instead go into their
// own core0-owned word and get OR'd in only inside sensors_key_take() (see
// sensors.c).
//
// THE HONESTY REQUIREMENT. This function starts downstream of the AXP2101
// entirely. On real hardware, a press latches bits in PMIC register 0x49;
// pmic_poll_core1() (sensors.c) is what actually issues the i2c1 read,
// masks the result down to `s1 & 0x0E`, and write-1-to-clears the chip's
// latch so the next event is distinguishable from this one. None of that
// runs here: sensors_inject_key() hands sensors_key_take() the exact bits a
// real read would have produced, as if the PMIC transaction had already
// happened and come out clean. That is not a small omission. The one real
// bug this codebase has already shipped in this exact area (several PMIC
// bits landing in one read and silently breaking the old double-press menu
// gesture, see runtime_core.c's chord comment) lived in the register read
// and the read-and-clear timing, precisely the part injection skips. A
// devlink chord that opens the menu proves the runtime and the menu app
// handle a KEY_LONG bit correctly. It proves nothing about whether the
// AXP2101 will ever actually hand the runtime that bit on real silicon. That
// can only be checked by holding the real button.
//
// Mirror image of emu_abi.h's rule for the emulator ("must never deliver an
// input the hardware cannot produce"). That rule polices a SIMULATED board
// pretending to be hardware; this is a test tool driving REAL firmware on
// REAL hardware through the same signals runtime_core.c already consumes,
// with no simulated chip standing in for anything. So this is not a
// violation of that rule, it is the other side of it: emu_abi.h exists to
// stop a stand-in board from lying about what real silicon can do, and
// sensors_inject_key() has no stand-in board to lie through. What it does
// have is a real, documented gap of its own (the paragraph above), which is
// why that gap is written down here rather than left for someone to
// discover by trusting a green chord test too far.
void sensors_inject_key(uint8_t bits);

/* ---- power off -----------------------------------------------------------
 *
 * Requests that the AXP2101 cut power to the board (register 0x10 bit 0,
 * "Soft PWROFF"; every rail drops except VRTC). Confirmed straight from the
 * datasheet text: a first attempt to WebFetch the PDF got structure noise
 * back and fell through to corroborating this against lewisxhe/XPowersLib
 * instead (an open AXP2101 driver whose other register addresses already
 * matched this file's own hardware-confirmed ones exactly - slave 0x34,
 * register 0x27, 0x48/0x49/0x4A). Downloading the PDF and running
 * `pdftotext -layout` on it locally DOES extract the register tables as
 * readable text (a few Chinese-font glyphs elsewhere in the document fail
 * to decode; the tables that matter are unaffected) - section 7.5.4.3
 * "Power Off" lists `Write "1" to REG10H[0]` as one of six power-off
 * sources, and register 0x10's own bitfield table names bit 0 "Soft
 * PWROFF". XPowersLib was right; this is now the primary source, not a
 * corroborated guess. See sensors.c for the exact quotes.
 *
 * Core0-owned request, core1-owned execution, the same pattern
 * g_fingerDownShared (sensors.c) already uses: the DECISION - has PWR been
 * held PWR_HOLD_POWEROFF_MS alone, per runtime_core.c's gesture logic - is made by a
 * portable, hardware-blind file that must never touch i2c1 even if it
 * wanted to, but the i2c write that actually cuts power must come from
 * core1 like every other transaction on this bus (see the ownership rule
 * banner at the top of this file). One flag, one writer (this function,
 * called from core0), one reader (core1_entry(), sensors.c); no barrier
 * needed, for the same reason g_fingerDownShared needs none: a plain bool
 * load/store is already atomic on this part, and a one-loop-iteration delay
 * before core1 notices is invisible for a gesture measured in seconds.
 *
 * Calling this does not guarantee power actually drops: it is a best-effort
 * i2c write like any other in sensors.c, not retried if it times out. The
 * OFFLEVEL backstop (pmic_raise_poweroff_threshold(), 10s) is what still
 * cuts power if this write is lost or if firmware is too wedged to have
 * reached this call in the first place - see that function's comment for
 * why the two are not redundant.
 */
void sensors_request_poweroff(void);

/* ---- shake -------------------------------------------------------------
 *
 * A monotonic counter bumped once per accepted shake, rather than a flag,
 * so a consumer that misses a frame still sees that it happened and a slow
 * consumer cannot be handed the same shake twice.
 *
 * Detection requires several jolts inside a window, not one threshold
 * crossing, so a social shake or a knock cannot fire it. Shake is opt-in
 * per app (app_t.wants_shake): it is the Etch A Sketch gesture and belongs
 * only where erasing is the app's identity. Promoting it to a universal
 * reset would destroy exactly the two things a child carries across a room
 * to show someone, the drawing and the number on the stopwatch.
 */
uint32_t sensors_erase_seq(void);

// Injects a synthetic shake, for the agent-facing test link's ERASE command.
// Bumps the same counter sensors_erase_seq() reports, so it goes through the
// identical wants_shake gate a real shake does and reaches an app only if
// that app opted in; there is deliberately no direct "erase this app now"
// call, for the same reason shake itself is opt-in above. Core0-owned end to
// end (see sensors.c), so, like sensors_inject_touch(), this is not a second
// writer of anything core1 also writes.
void sensors_inject_erase(void);

/* ---- diagnostics -------------------------------------------------------
 *
 * Core1 increments these and never prints them; core0 reads them and turns
 * them into per-second deltas in the profiler. A regression on either core
 * is then visible from one printout.
 */
typedef struct {
    uint32_t touchReads;
    uint32_t touchTimeouts;
    uint32_t touchQueueDrops;
    uint32_t touchRecoveries;
    uint32_t imuTimeouts;
    uint32_t pmicTimeouts;
    // Bumped once per sensors_request_poweroff() core1 actually notices and
    // acts on (see that function's comment). Core1 cannot printf to prove a
    // shutdown command was sent (the ownership-rule banner above explains
    // why), so this counter is the only way the once-a-second profiler line
    // can show it happened - the difference between "we watched the counter"
    // and "we held the button and the screen went dark" that
    // sensors_inject_key()'s HONESTY REQUIREMENT comment already insists on
    // elsewhere in this file.
    uint32_t poweroffCmds;
    // Register 0x10 read back immediately before and after the shutdown
    // write (sensors.c's pmic_poweroff_poll_core1()), so a live incident
    // can tell "the i2c write never reached the chip" from "the chip took
    // it and ignored it" without needing a logic analyser. 0xFFFFFFFF: no
    // shutdown attempted yet this boot. 0x000000FF specifically for
    // poweroffRegAfter: the readback transaction itself failed (distinct
    // from an actual 0xFF register value, which this register cannot hold -
    // bits 7:6 are hard reserved-0).
    uint32_t poweroffRegBefore;
    uint32_t poweroffRegAfter;
} sensors_stats_t;

void sensors_stats(sensors_stats_t *out);

// PERMANENT: raw per-iteration counter, read once a second by the profiler
// (runtime.c) to compute the core1=<n>/s liveness figure, and by the
// core1-liveness watchdog guard there. Originally added as a temporary
// diagnostic; decision 0004 made it permanent after it was deleted once
// during the investigation and the bug it exists to catch instantly became
// invisible again. Do not remove.
uint32_t sensors_debug_core1_loops(void);

// Core1 fault capture: fault_kind is the IPSR exception number that fired
// (0 = no fault seen this boot; 3 = HardFault, 4 = MemManage, 5 = BusFault,
// 6 = UsageFault, 7 = SecureFault - see hardware/exception.h's enum
// exception_number). fault_pc/fault_lr are the faulting instruction address
// and link register, read off the exception stack frame. fault_cfsr is
// SCB->CFSR, which sub-reason (bit layout: ARMv8-M architecture reference
// manual, section on CFSR). The handler these report on stays installed
// permanently, not because the investigation it was built for is still
// open (it is not - see docs/decisions/0004/0005), but because it is a
// genuine safety net and because the invariant checker
// (tools/invariants/rules/rp2350-amoled-1.8.ts) hardcodes core1_fault_
// handler as a reachability root: removing it fails the build, not just a
// diagnostic.
uint32_t sensors_debug_core1_fault_kind(void);
uint32_t sensors_debug_core1_fault_pc(void);
uint32_t sensors_debug_core1_fault_lr(void);
uint32_t sensors_debug_core1_fault_cfsr(void);

// The PMIC write self-test's attempt/failure counts - see sensors.c's
// PMIC_WRITE_SELFTEST block. Both 0 when the gate is off (which is how this
// firmware ships - see that block's comment).
void sensors_debug_pmic_selftest(uint32_t *writes, uint32_t *fails);

/* ---- TEMPORARY: touch poll self-test, for the "does the FT3168 see the
 * finger at all" question -----------------------------------------------
 *
 * Declared here, not in sensors.c, so runtime.c (core0's profiler) can gate
 * its own printf on the same macro sensors.c gates the polling on - one
 * source of truth for "is this diagnostic build or a normal one", the same
 * way both files already share every other public name in this header.
 * MUST be 0 in anything shipped.
 *
 * Every measurement so far (touch reads=0, recoveries climbing every 5s)
 * comes from a counter that only increments inside `if (intLow)` in
 * core1_entry()'s loop - so all it can prove is that Touch_INT_PIN never
 * reads low. It says nothing about whether the FT3168 itself ever notices a
 * finger, because nothing ever asks it while the INT line is high.
 *
 * With TOUCH_POLL_SELFTEST set, core1 polls the controller directly, a few
 * times a second, ignoring Touch_INT_PIN entirely, and separately reads back
 * a few of its mode/config registers about once a second. Same discipline as
 * PMIC_WRITE_SELFTEST (sensors.c): a compile-time gate, default 0, MUST be 0
 * in anything shipped, and every read goes through this file's own
 * timeout-bounded i2c1 calls (never DEV_I2C_*, never the SDK's blocking
 * ones) so it cannot become the thing that wedges core1 while investigating
 * why input does not work.
 *
 * `polls` is deliberately a separate counter from `fingers`: a `fingers==0`
 * reading only means something once `polls` proves the loop actually ran.
 * See docs/decisions/0004's "a counter that can only read zero is not a
 * measurement" for why that distinction is the whole point.
 */
#ifndef TOUCH_POLL_SELFTEST
#define TOUCH_POLL_SELFTEST 0
#endif

typedef struct {
    uint32_t polls;         // diagnostic poll attempts (i2c to the controller), regardless of outcome
    uint32_t ok;            // of those, how many completed without a timeout
    uint32_t fail;          // of those, how many timed out (finger-count or xy read)
    uint32_t fingers;       // last finger count read, snapshot
    uint32_t maxFingers;    // highest finger count observed this boot, in case a snapshot missed a blip
    uint32_t x, y;          // last reported coordinates, when fingers != 0
    uint32_t intLowCount;   // of `polls`, how many sampled Touch_INT_PIN low at that instant
    uint32_t intHighCount;  // of `polls`, how many sampled it high
    uint32_t intLastLevel;  // most recent raw sample, 0 or 1 (1 = high)
    uint32_t regPolls;      // register-readback attempts (~1/s), regardless of outcome
    uint32_t regFails;      // of those, how many timed out
    uint32_t deviceModeReg; // last read of 0x00 DEVICE_MODE (0 = normal working mode)
    uint32_t powerModeReg;  // last read of 0xA5 REG_POWER_MODE (0 = active, per FT3168_Device_Mode)
    uint32_t intModeReg;    // last read of 0xA4 - see sensors.c for why this address, specifically
    uint32_t thGroupReg;    // last read of 0x80 - see sensors.c's FT3168_REG_THGROUP comment:
                             // measured on hardware, this register does not accept a write on
                             // this part, so neither touch_set_active() nor touch_set_active_to()
                             // writes it any more. This IS the chip's own live value, not
                             // something this firmware asserted.
} sensors_touch_diag_t;

// Reads back the touch poll self-test's published state - see the struct
// above. Every field is 0 when the gate is off (mirrors sensors_debug_
// pmic_selftest()'s own "reads as 0/0 in a build with the gate off").
void sensors_debug_touch_poll_selftest(sensors_touch_diag_t *out);

/* ---- TEMPORARY: touch pipeline diagnostics ------------------------------
 *
 * Added after the FT3168-level self-test above turned out not to be the
 * whole story: injected touches (which enter downstream of core1 entirely,
 * in devlink's own core0-owned ring, so they cannot be explained by
 * anything at the controller or GPIO level) failed to draw too. That points
 * at a shared stage further downstream: the real ring, the injection ring,
 * their merge (sensors_touch_next()), or what an app does with what comes
 * out. Every field below is a count at one specific stage, so "the chain
 * breaks between the merge and the app" is readable from one profiler line
 * instead of inferred from what did NOT print. Same gate as the struct
 * above (TOUCH_POLL_SELFTEST); every field is 0 when it is off.
 */
typedef struct {
    uint32_t realPushOk;             // touch_q_push() (core1) accepted a sample
    uint32_t injectPushOk;           // sensors_inject_touch() (core0, devlink) accepted a sample
    uint32_t injectPushDropped;      // ...and how many it rejected (ring full)
    uint32_t mergeCalls;             // sensors_touch_next() call count
    uint32_t mergeYieldReal;         // ...returned true, taken from the real ring
    uint32_t mergeYieldInjected;     // ...returned true, taken from the injected ring
    uint32_t mergeEmpty;             // ...returned false, both rings empty
    uint32_t mergeYieldFingersNonzero; // of the yields (either ring), fingers != 0
} sensors_touch_pipeline_diag_t;

// Reads back the touch pipeline diagnostic's published state - see the
// struct above. realPushDropped is deliberately not duplicated here: it is
// already sensors_stats_t.touchQueueDrops (unconditional, not gated), so
// read that alongside this rather than a second copy of the same counter.
void sensors_debug_touch_pipeline(sensors_touch_pipeline_diag_t *out);

/* ---- TEMPORARY: sketchpad-side touch diagnostics ------------------------
 *
 * The sketchpad is the one app that reads sensors_touch_next() itself
 * (apps/sketch.c's own comment, and runtime_core.c's `g_currentApp !=
 * &g_sketchApp` branch: the shared touchDown/X/Y resolver never runs while
 * it is current), so the pipeline diagnostic above ends at the merge - what
 * happens to a sample after that is entirely inside sketch.c's own stroke
 * state machine. This struct is that last stage: what the app's drain loop
 * actually saw and did with it. Declared here rather than a new header,
 * alongside every other TOUCH_POLL_SELFTEST name, since it exists for the
 * same investigation and shares the same gate; implemented in sketch.c
 * (the only file with the state to report), not sensors.c.
 */
typedef struct {
    uint32_t drained;         // samples pulled from sensors_touch_next() this run
    uint32_t haveTouch;       // of those, fingers != 0
    uint32_t newReport;       // of the haveTouch ones, coordinates differed from the last report
    uint32_t pendingStart;    // haveTouch samples that armed a new stroke-start candidate
                               // (sketch.c's CONFIRM_MS; renamed from "two-report start check" once
                               // a dropout mid-confirmation stopped being an instant kill)
    uint32_t strokeStarted;   // fingerDown transitioned false->true (stroke_begin() ran)
    uint32_t strokeEnded;     // fingerDown transitioned true->false (stroke_end() ran, real lift)
    // Existing per-run counters (sketch_state_t's glitches/dropouts/strays/
    // splits), surfaced here rather than duplicated - see that struct's own
    // comment ("nothing currently reads these"). 0 if no app run has
    // happened yet this boot (arena not yet allocated).
    uint32_t glitches;
    uint32_t dropouts;
    uint32_t strays;
    uint32_t splits;
} sketch_touch_diag_t;

// Reads back the sketchpad's touch diagnostic state - see the struct above.
// Every field is 0 when TOUCH_POLL_SELFTEST is off, or before the sketchpad
// has been entered at least once this boot (its counters live in the
// per-run arena, like the rest of sketch_state_t - see app.h on why).
void sketch_debug_touch_diag(sketch_touch_diag_t *out);

/* ---- DEVELOPMENT: sketchpad live tuning (SKETCH_LIVE_TUNE) --------------
 *
 * Declared here rather than a new header, same reasoning as
 * sketch_debug_touch_diag() just above: this exists for the same class of
 * investigation (feeling out sketch.c's dropout-tolerance tuning against
 * real hardware, live) and is implemented in sketch.c, the only file with
 * the state to report or change. See sketch.c's own SKETCH_LIVE_TUNE
 * comment for the full design (what is tunable, why, and the freeze path).
 *
 * Unlike the TOUCH_POLL_SELFTEST diagnostics above, these are declared
 * unconditionally (not inside an #if in this header): sketch.c itself
 * always defines all five functions below, gated internally on
 * SKETCH_LIVE_TUNE - the gate-off build returns count()==0 and false from
 * every describe/get/set call, the same "0 when the gate is off" contract
 * sensors_debug_touch_poll_selftest() uses. That means callers (devlink's
 * TUNE command wiring in runtime.c, the emulator's emu_tune_get/set) never
 * need to know whether the gate is on; they just see "nothing declared"
 * when it is off.
 *
 * Persists across app switches: the tunables are plain file-scope
 * statics in sketch.c, not part of sketch_state_t's per-run arena (app.h),
 * specifically so a TUNE command works regardless of which app is currently
 * showing on screen - the owner can be looking at the menu or the timer and
 * still dial in the sketchpad's touch tuning for the next time it is drawn
 * on.
 */

// How many tunables are declared. 0 when SKETCH_LIVE_TUNE is off.
int sketch_tune_count(void);

// Describes tunable `index` (0..sketch_tune_count()-1): its devlink/emulator
// -facing name, its clamp range, and its shipped default. Returns false for
// an out-of-range index or when the gate is off.
bool sketch_tune_describe(int index, const char **name, float *min, float *max, float *def);

// The #define this tunable becomes when frozen back into source (e.g.
// "LIFT_DEBOUNCE_MS", without the _DEFAULT suffix the freeze output adds
// itself). NULL for an out-of-range index or when the gate is off.
const char *sketch_tune_define_name(int index);

// Current value of the tunable named `name` (sketch_tune_describe()'s
// `name` out-param, e.g. "lift"). Returns false, and leaves *out untouched,
// if no tunable has that name or the gate is off.
bool sketch_tune_get(const char *name, float *out);

// Sets the tunable named `name` to `value`, clamped to its declared
// [min, max]. *outApplied (if non-NULL) receives the value actually
// applied, i.e. after clamping, so a caller can tell a client what really
// took effect. Returns false, applying nothing, if no tunable has that name
// or the gate is off.
bool sketch_tune_set(const char *name, float value, float *outApplied);

/* ---- FT3168 has no pressure signal, measured -----------------------------
 *
 * Measured 2026-08-13: FT3168 registers 0x07 (FocalTech's per-touch "weight")
 * and 0x08 (area/misc) read 0 always, under a hard finger press included.
 * Evidence: over a 75s window with a real finger held on the glass, the
 * profiler showed touch reads=3032 and reads=558 in successive seconds (so
 * the controller was genuinely being read with a finger present) while the
 * running min/max moved off the never-sampled sentinel (min=255 max=0) to
 * min=0 max=0, meaning every sample seen was zero.
 *
 * Consequence: this panel cannot report press force or contact area. There
 * is nothing to calibrate here; the information does not exist on this part.
 * Do not re-add a burst read of 0x07/0x08 to answer this again.
 */

/* ---- BOOT button ---------------------------------------------------------
 *
 * Not a chip on i2c1 - BOOT is read by borrowing the flash chip select (see
 * bootbtn.h on the board for what that costs and why it is sampled slowly
 * and never in a hot loop) - but it widens this header rather than getting
 * a header of its own, because app.h's rule is absolute: apps read signals,
 * never chips, and the portable runtime core (runtime_core.c) is exactly as
 * hardware-blind as an app is. Before this, the core reached past sensors.h
 * into bootbtn.h directly, which is the one place it depended on anything
 * outside {app.h, gfx.h, sensors.h} - see docs/decisions/0003.
 */

// True on the frame the BOOT button was released. See bootbtn.h on the board
// for why this is sampled slowly and never in a hot loop.
bool sensors_boot_clicked(void);

// The BOOT button's current level, for gestures that need it held.
bool sensors_boot_down(void);

// Swallows whatever BOOT click is currently pending (or the next one to
// complete), so that releasing BOOT as part of a chord (BOOT held with PWR
// long-pressed, see runtime_core.c's menu gesture) does not ALSO deliver a
// bootClicked to whichever app the chord just switched to. Without this, a
// chord that closes the menu back into chrono would reset the stopwatch the
// instant BOOT comes back up, purely as a side effect of how the chord was
// released, not anything the child asked chrono to do.
void sensors_boot_consume_click(void);

// Injects the BOOT level and a BOOT click edge, for the agent-facing devlink
// test link. Two separate entry points because the two consumers want two
// different shapes of the same button: the chord above needs the LEVEL
// ("is BOOT down right now", sensors_boot_down()), while an app like the
// stopwatch or the timer needs the CLICK EDGE ("BOOT was just released",
// sensors_boot_clicked()). A real press produces both, at different moments;
// a test tool has to be able to drive either alone, since the chord
// specifically depends on the level still being true at the instant the PWR
// verdict lands, not on a click ever completing.
//
// Both are OR'd on top of whatever bootbtn.c's own physical read reports
// (see sensors.c), rather than reaching into bootbtn.c's internal debounce
// state (its static held/lastSampleMs). BOOT sampling already runs entirely
// on core0 - bootbtn.c borrows the flash chip select, which this file's
// "BOOT button" section above already explains is core0-only by nature, not
// by the i2c1 ownership rule - so there is no cross-core race to guard
// against here the way sensors_inject_touch() and sensors_inject_key() have
// to. OR-ing at this wrapper is enough, and it keeps bootbtn.c exactly as
// blind to devlink's existence as it always was.
//
// THE HONESTY REQUIREMENT, same as sensors_inject_key() above: this starts
// downstream of the physical pad read. bootbtn.c's read_cs_low() is what
// actually borrows the flash chip select, waits for the line to settle, and
// decodes QSPI_CSN - real, timing-sensitive, flash-disturbing work that
// injection never touches. Nothing here proves BOOT is actually wired to the
// chip select the way bootbtn.h's header comment says it should be (that is
// what bootbtn_selftest_poll() is for). A devlink chord that opens the menu
// proves the runtime's chord logic and the app underneath it; it says
// nothing about whether a real thumb on the real button still reaches this
// code path. See sensors_inject_key()'s comment for why this is the mirror
// image of emu_abi.h's rule, not a violation of it, and why the gap is
// written down here rather than assumed away by a passing test.
void sensors_inject_boot(bool down);
void sensors_inject_boot_click(void);

/* ---- lifecycle --------------------------------------------------------- */

// Core0, single-threaded, before core1 exists: brings up the IMU, the touch
// controller and the PMIC interrupt mask. Everything it touches is on i2c1,
// which is safe only because core1 has not launched yet.
void sensors_init(void);

// Launches core1. After this returns, the ownership rule above is in force.
void sensors_start(void);

/* ---- core1 restart: the guard's recovery half --------------------------
 *
 * PERMANENT. Call only from core0, and only once core1 has been observed
 * dead for long enough to be sure (see runtime.c's core1-liveness guard) -
 * never speculatively, since this forcibly resets core1's CPU state and
 * bit-bangs the i2c1 pins, both of which are only safe when core1
 * genuinely is not running. Performs, in order: a hardware reset of core1
 * (works regardless of what core1 was doing - a wedged i2c1 wait or a
 * genuine LOCKUP, this does not depend on core1's cooperation), an i2c1
 * bus recovery (release SDA, clock SCL by hand, issue a STOP, reinit the
 * peripheral - the fix for a slave caught holding the bus mid-byte), and a
 * fresh multicore_launch_core1(). See sensors.c's i2c1_bus_recover() for
 * the full argument.
 */
void sensors_restart_core1(void);

// PERMANENT diagnostic: how many times sensors_restart_core1() has run
// this boot - part of the standing guard's own visibility, not a
// temporary investigation artifact. A bus that wedges constantly should be
// visible, not silently and endlessly papered over.
uint32_t sensors_debug_i2c1_recoveries(void);

#endif // SENSORS_H
