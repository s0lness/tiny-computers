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
 * 0x41 at init. The long-press interrupt fires at 1.5s and the PMIC's hard
 * power-off at 6s (register 0x27), measured at 1480ms and no cut at 4.5s,
 * so a 1.5s gesture has 4.5s of margin before the rails drop.
 */
#define KEY_RELEASE 0x01
#define KEY_PRESS   0x02
#define KEY_LONG    0x04
#define KEY_SHORT   0x08

// Reads and clears the pending key bits. Exactly one caller per loop
// iteration, or events are lost: this is a read-and-clear, not a queue.
// The runtime is that caller, and it hands apps what it does not consume
// itself (see app.h's app_key field).
uint8_t sensors_key_take(void);

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
} sensors_stats_t;

void sensors_stats(sensors_stats_t *out);

/* ---- lifecycle --------------------------------------------------------- */

// Core0, single-threaded, before core1 exists: brings up the IMU, the touch
// controller and the PMIC interrupt mask. Everything it touches is on i2c1,
// which is safe only because core1 has not launched yet.
void sensors_init(void);

// Launches core1. After this returns, the ownership rule above is in force.
void sensors_start(void);

#endif // SENSORS_H
