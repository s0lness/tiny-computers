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
 * The requirement changed: the owner's power-off gesture (hold PWR alone
 * for 5 seconds) has to be timed in firmware, because 5 seconds does not
 * exist as an OFFLEVEL value (4/6/8/10s only, see above), and timing a hold
 * needs to know when it ENDS, not just when it started or how long the PMIC
 * decided it had already been. KEY_PRESS alone cannot answer "is PWR still
 * down right now", and re-deriving that from KEY_LONG (a one-shot verdict
 * at 1.5s, not a level) does not cover the 1.5s-5s window at all. So the
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
// PWR-held-5s power-off gesture (runtime_core.c) hard to reproduce safely
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
 * held 5 seconds alone, per runtime_core.c's gesture logic - is made by a
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

// TEMPORARY diagnostic, added while investigating whether core1 is actually
// iterating during a live incident (recoveries/reg10h readback not moving).
// Remove alongside its definition and core1_entry()'s g_core1Loops once
// that investigation is closed.
uint32_t sensors_debug_core1_loops(void);

// TEMPORARY diagnostic, added alongside sensors_debug_core1_loops() to show
// not just THAT core1 stopped but WHERE in one loop iteration it stopped.
// Returns one of sensors.c's PHASE_* constants (see g_core1Phase's comment
// there for what each number means); 0 before core1's first iteration ever
// writes it. Remove alongside its definition once the investigation is
// closed.
uint32_t sensors_debug_core1_phase(void);

// TEMPORARY diagnostics: core1 fault capture. fault_kind is the IPSR
// exception number that fired (0 = no fault seen this boot; 3 = HardFault,
// 4 = MemManage, 5 = BusFault, 6 = UsageFault, 7 = SecureFault - see
// hardware/exception.h's enum exception_number). fault_pc/fault_lr are the
// faulting instruction address and link register, read off the exception
// stack frame. fault_cfsr is SCB->CFSR, which sub-reason (bit layout: ARMv8-M
// architecture reference manual, section on CFSR). fault_phase is
// sensors_debug_core1_phase()'s value at the instant of the fault. Remove
// all of these alongside sensors.c's core1_fault_handler_c() once the
// investigation is closed.
uint32_t sensors_debug_core1_fault_kind(void);
uint32_t sensors_debug_core1_fault_pc(void);
uint32_t sensors_debug_core1_fault_lr(void);
uint32_t sensors_debug_core1_fault_cfsr(void);
uint32_t sensors_debug_core1_fault_phase(void);

// TEMPORARY diagnostics: LOCKUP check and stack high-water mark, added
// chasing the hypothesis that core1 is entering Cortex-M LOCKUP (a fault
// that cannot itself be entered) rather than blocking or faulting normally
// - see sensors.c's core1 LOCKUP/stack section for the full argument.
// halted() reads SYSCFG_PROC_CONFIG.PROC1_HALTED, a shared (not core-
// private) register, so it is answerable even if core1 cannot execute
// anything at all. stack_headroom() returns bytes of core1's 2KB stack
// never touched below the deepest point reached (0 = the whole stack was
// used at least once) - a canary painted before multicore_launch_core1(),
// so this is meaningful any time after boot, dead core1 or not.
bool sensors_debug_core1_halted(void);
uint32_t sensors_debug_core1_stack_headroom(void);

// TEMPORARY diagnostic: i2c1's hardware status, read live from core0 - see
// sensors.c's sensors_debug_i2c1_live() for the full argument and the ONE
// CAVEAT (tx_abrt_source is clear-on-read; only call this once core1 is
// already observed dead, never while it might still be alive).
void sensors_debug_i2c1_live(uint32_t *enable, uint32_t *enableStatus, uint32_t *status,
                              uint32_t *rawIntrStat, uint32_t *txAbrtSource,
                              uint32_t *txflr, uint32_t *rxflr);

// TEMPORARY diagnostics: which wait loop core1's bounded i2c1 write is
// currently in (0=none, 1=TX_EMPTY wait, 2=STOP_DET wait) and how many
// passes it has made through the CURRENT entry to that loop - see
// i2c1_write_bytes_bounded()'s comment in sensors.c. Answerable at any
// time, including while core1 might still be alive (these do not touch
// i2c1 itself, just report where core1's own bounded write function is).
uint32_t sensors_debug_core1_write_wait_kind(void);
uint32_t sensors_debug_core1_write_wait_spins(void);

// TEMPORARY diagnostic: SDA/SCL sampled as plain GPIO levels, bypassing
// the i2c peripheral - see sensors.c's sensors_debug_i2c1_pins() for why
// this is the electrical truth on the wire and the classic "a slave is
// holding the bus" check. Safe from core0 at any time.
void sensors_debug_i2c1_pins(bool *sda, bool *scl);

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
