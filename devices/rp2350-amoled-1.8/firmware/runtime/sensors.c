/*
 * sensors: implementation. See sensors.h for the ownership rule this whole
 * file exists to enforce (core1 owns i2c1 exclusively once sensors_start()
 * has run) and for why core1 never printfs.
 *
 * This is an extraction, not a rewrite: the logic below ran on real hardware
 * as part of main.c before the runtime/app split, and is carried over with
 * its tuning and its comments intact.
 */
#include "sensors.h"

#include <math.h>
#include <stddef.h>

#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

#include "DEV_Config.h"
#include "FT3168.h"
#include "QMI8658.h"

/* ---------------------------------------------------------------------
 * Touch power state.
 *
 * FT3168_Init writes 0x01 to REG_POWER_MODE (0xA5), which is MONITOR in the
 * driver's own Device_Mode enum, not ACTIVE. Monitor is the low-power scan
 * state: the controller samples at a reduced rate and is meant to promote
 * itself to active when it sees a finger. If that promotion does not happen,
 * the chip stays alive on the bus, keeps answering the WhoAmI, and reports
 * zero fingers forever, which is exactly the failure seen here. We put it in
 * ACTIVE explicitly after init and after every recovery.
 * ------------------------------------------------------------------- */
// FT3168 register 0x88 is the active-mode scan period in milliseconds. The
// datasheet gives 60fps as the default and says the part supports up to 100Hz,
// which is worth taking: the report rate is the hard floor on input latency,
// and it also shortens every dropout gap the stroke code has to bridge.
// Written after the mode is set, and the effect is checked by the profiler's
// reports-per-second rather than assumed.
#define FT3168_REG_PERIOD_ACTIVE 0x88
#define FT3168_PERIOD_MS         10

// Runs once, on core0, before multicore_launch_core1() - i.e. before any
// contention over i2c1 can exist. Never call this after core1 has started;
// touch_set_active_to() (below) is the timeout-guarded equivalent for use
// once core1 owns the bus.
static void touch_set_active(void) {
    // Register 0x00 is DEVICE_MODE. 0 is normal working mode; the chip will not
    // report points in factory/test mode, and nothing in the vendor init ever
    // sets it, so it is whatever the last state left behind.
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, 0x00, 0x00);
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, REG_POWER_MODE, FT3168_POWER_ACTIVE);
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE, FT3168_PERIOD_MS);
}

/* ---------------------------------------------------------------------
 * PMIC power key.
 * ------------------------------------------------------------------- */
#define AXP2101_ADDR 0x34

static void buttons_init(void) {
    // Only the middle interrupt register is enabled. That is where the power
    // key events land; enabling all three also produced battery and charger
    // events, which are noise for this purpose. This is one-time setup and
    // runs on core0 before multicore_launch_core1(), so touching i2c1 here is
    // still safe.
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x40, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x41, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x42, 0x00);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x48, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x4A, 0xFF);
}

// Set by pmic_poll_core1() (core1), consumed by sensors_key_take() (core0),
// which is where the framebuffer lives. Bits are REG 0x49's: 3 short press, 2
// long press. A plain volatile byte is enough synchronisation for this: it is
// only ever read-and-cleared once per outer loop on core0, a stale or
// slightly-late read costs at most one loop iteration of a button gesture
// that is measured in hundreds of milliseconds, and there is no queue of
// events to lose since the PMIC itself already debounces press/release in
// hardware.
static volatile uint8_t g_keyEvent;

/* =========================================================================
 * Core 1: the sole owner of i2c1.
 *
 * i2c1 is shared by the touch controller, the IMU and the PMIC (AGENTS.md).
 * From the moment sensors_start() runs, core1 is the ONLY code in this
 * firmware allowed to touch i2c1: no DEV_I2C_*, no i2c_*_blocking or
 * i2c_*_timeout_us against I2C_PORT, and no call into the FT3168 or QMI8658
 * drivers (they call DEV_I2C_* internally) from core0. Everything core0
 * still does with a GPIO - SYS_OUT, Touch_INT_PIN, the boot-select pad, the
 * probe pins - is fine from either core; GPIO input registers are just
 * memory, reading them from both cores at once is not a race. I2C
 * transactions are not: two cores issuing them concurrently could interleave
 * on the wire.
 *
 * Every read or write in this section uses the SDK's *_timeout_us calls,
 * never the plain _blocking ones. The plain ones have no timeout at all, so
 * a wedged bus would hang core1 forever with nothing core0 could do about it
 * short of a physical power cycle. A timeout failure here is counted, not
 * retried in a loop.
 * ========================================================================= */

// 2ms against a transaction that normally costs a few hundred microseconds
// (695us measured for the worst case, touch's finger-count-then-XY-burst
// pair) gives comfortable margin without turning a single real stall into a
// multi-second one.
#define I2C_TIMEOUT_US 2000

static inline bool i2c1_write_to(uint8_t addr, const uint8_t *data, size_t len) {
    return i2c_write_timeout_us(I2C_PORT, addr, data, len, false, I2C_TIMEOUT_US) == (int)len;
}

static inline bool i2c1_write_reg_to(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c1_write_to(addr, b, 2);
}

// Register-pointer write (repeated START, no STOP) followed by a bounded
// read, same transaction shape DEV_I2C_Read_nByte uses, just with a timeout
// on both halves.
static inline bool i2c1_read_reg_n_to(uint8_t addr, uint8_t reg, uint8_t *out, size_t len) {
    if (i2c_write_timeout_us(I2C_PORT, addr, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, addr, out, len, false, I2C_TIMEOUT_US) == (int)len;
}

/* ---- touch: read helpers, gated on Touch_INT_PIN ---------------------- */

static inline bool touch_read_fingers_to(uint8_t *out) {
    uint8_t v = 0;
    if (!i2c1_read_reg_n_to(FT3168_I2C_ADDR, REG_FINGER_NUM, &v, 1)) return false;
    *out = v;
    return true;
}

// One burst for both axes, same as the single-core version used to do: the
// vendor path reads X (0x03,0x04) and Y (0x05,0x06) as two separate
// transactions, so a report landing between them yields a point built from
// the new X and the old Y - a coordinate the finger never visited, off the
// line at right angles. Auto-increment across these four registers is
// already relied on by the vendor's own 2-byte reads, so this is the same
// access widened, not a new assumption.
static inline bool touch_read_xy_to(uint16_t *x, uint16_t *y) {
    uint8_t b[4];
    if (!i2c1_read_reg_n_to(FT3168_I2C_ADDR, REG_X1_H, b, 4)) return false;
    *x = ((uint16_t)(b[0] & 0x0F) << 8) | b[1];
    *y = ((uint16_t)(b[2] & 0x0F) << 8) | b[3];
    return true;
}

// Timeout-guarded equivalent of touch_set_active() (above), for use after
// sensors_start(). Never call the plain touch_set_active() past that point.
static bool touch_set_active_to(void) {
    bool ok = true;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, 0x00, 0x00) && ok;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, REG_POWER_MODE, (uint8_t)FT3168_POWER_ACTIVE) && ok;
    ok = i2c1_write_reg_to(FT3168_I2C_ADDR, FT3168_REG_PERIOD_ACTIVE, FT3168_PERIOD_MS) && ok;
    return ok;
}

/* ---- the touch queue: core1 (producer) -> core0 (consumer) ------------
 *
 * A single-producer/single-reader ring, power-of-two sized so the index
 * wrap is a mask instead of a modulo. head is only ever written by core0,
 * tail only ever written by core1; each core only *reads* the other's
 * index. That split, plus a memory barrier around the point where the new
 * element becomes visible, is what makes this safe without a mutex or a
 * spinlock:
 *   - a spinlock held across an I2C transaction is exactly what the vendor
 *     demo's own touch/IMU locking bug looked like (see AGENTS.md's "Do not
 *     move either onto an interrupt" note) - so this deliberately never
 *     takes a lock around anything that can block;
 *   - the producer writes the sample into touchQ[tail] *then* publishes the
 *     new tail with __dmb() in between, so a consumer that observes the new
 *     tail is guaranteed to observe a fully-written sample, not a torn one;
 *   - the consumer's __dmb() before copying out the sample is the mirror of
 *     that: it stops the compiler or core from reading the sample data
 *     ahead of reading the tail index that guards it.
 * The queue can never appear to have more than one writer or reader because
 * only core1 calls touch_q_push() and only core0 calls touch_q_pop().
 */
#define TOUCH_Q_CAP 64
_Static_assert((TOUCH_Q_CAP & (TOUCH_Q_CAP - 1)) == 0, "TOUCH_Q_CAP must be a power of two");

static volatile touch_sample_t g_touchQ[TOUCH_Q_CAP];
static volatile uint32_t g_touchHead = 0; // core0-owned (consumer index)
static volatile uint32_t g_touchTail = 0; // core1-owned (producer index)

static inline bool touch_q_push(const touch_sample_t *s) {
    uint32_t tail = g_touchTail;
    uint32_t next = (tail + 1) & (TOUCH_Q_CAP - 1);
    if (next == g_touchHead) return false; // full: core0 is behind, drop and count it
    g_touchQ[tail] = *s;
    __dmb();
    g_touchTail = next;
    return true;
}

static inline bool touch_q_pop(touch_sample_t *out) {
    uint32_t head = g_touchHead;
    if (head == g_touchTail) return false; // empty
    __dmb();
    *out = g_touchQ[head];
    g_touchHead = (head + 1) & (TOUCH_Q_CAP - 1);
    return true;
}

// How often to push a synthetic "no finger" sample while Touch_INT_PIN is
// high, so core0's lift-debounce and stall-watchdog timing (both wall-clock
// based) keep getting fresh timestamps without core1 spending any I2C time
// to produce them. Well under LIFT_DEBOUNCE_MS (an app-level constant, 80ms),
// so a real lift is still noticed promptly; the transition itself is also
// pushed immediately (see core1_entry), so this interval only governs the
// steady idle state, not the edge.
#define TOUCH_IDLE_HEARTBEAT_MS 5

// Touch recovery. The FT3168 can be left in a state where it answers I2C with
// the right WhoAmI but never reports a finger again; observed after a picotool
// reboot, which resets the RP2350 without power-cycling the touch chip, so the
// state survives every reflash. If nothing has been reported for this long,
// pulse the reset line and re-arm the chip. ~110ms, invisible while idle.
// The trigger is wall-clock time since the last observed finger, which is
// unaffected by whether core1 was gating its I2C reads on Touch_INT_PIN at
// the time (see core1_entry()).
#define TOUCH_STALL_MS 5000

/* ---- cross-core published state, other than the touch queue -----------
 *
 * All of these are single, plain-old-data words: each is written by
 * exactly one core and only ever read by the other, so an aligned 32-bit
 * (or bool-sized) load/store is already atomic on Cortex-M33 and no barrier
 * is needed for correctness - the worst case is the reader seeing last
 * loop's value for one more iteration, which for a button gesture, a shake
 * flag or a diagnostic counter is nowhere near visible.
 */
static volatile bool g_fingerDownShared; // core0 -> core1: suppress shake while drawing
static volatile uint32_t g_eraseSeq;     // core1 -> core0: bumped once per accepted shake

// Diagnostics, core1-increment-only, core0-read-only (via sensors_stats()).
static volatile uint32_t g_touchReads;
static volatile uint32_t g_touchTimeouts;
static volatile uint32_t g_touchQueueDrops;
static volatile uint32_t g_touchRecoveries;
static volatile uint32_t g_imuTimeouts;
static volatile uint32_t g_pmicTimeouts;

/* ---- touch stall recovery, core1 side ---------------------------------- */

static void touch_recover_core1(void) {
    FT3168_Reset();          // RST pin toggle + sleep_ms only, no I2C: safe as-is.
    if (!touch_set_active_to()) g_touchTimeouts++;
    g_touchRecoveries++;
}

/* ---- IMU: shake-to-erase, core1 side ------------------------------------
 *
 * QMI8658_init() (called once on core0, before sensors_start()) leaves the
 * part at QMI8658AccRange_8g, which is what fixes the raw-to-mg scale factor
 * below: acc_lsb_div = 1<<12 = 4096 for that range (see QMI8658.c's
 * QMI8658_config_acc()). That divisor is a private static in the vendor
 * driver with no getter, so it is re-derived here rather than read back; if
 * QMI8658_init()'s accRange is ever changed, this constant has to change
 * with it.
 *
 * Shake tuning (JOLT_*, ERASE_COOLDOWN_MS): measured against real hardware.
 * Detection requires several jolts inside a window, not one threshold
 * crossing, so a social shake or a knock cannot fire it.
 */
#define QMI8658_I2C_ADDR    0x6B  // confirmed on this board by i2c_scan(); QMI8658_init()
                                  // itself probes 0x6A then 0x6B and only 0x6B acks here.
#define QMI8658_ACC_LSB_DIV 4096.0f

#define IMU_POLL_MS    20
#define JOLT_DEV_MG    900.0f
#define JOLT_WINDOW_MS 700
#define JOLT_MIN_COUNT 4
#define ERASE_COOLDOWN_MS 1200
#define JOLT_MAX 16

static void imu_poll_core1(uint32_t nowMs) {
    static uint32_t lastMs = 0;
    static uint32_t joltTimes[JOLT_MAX];
    static int joltCount = 0;
    static uint32_t cooldownUntilMs = 0;

    if (nowMs - lastMs < IMU_POLL_MS) return;
    lastMs = nowMs;

    uint8_t buf[6];
    if (!i2c1_read_reg_n_to(QMI8658_I2C_ADDR, QMI8658Register_Ax_L, buf, 6)) {
        g_imuTimeouts++;
        return;
    }
    short rawX = (short)((uint16_t)(buf[1] << 8) | buf[0]);
    short rawY = (short)((uint16_t)(buf[3] << 8) | buf[2]);
    short rawZ = (short)((uint16_t)(buf[5] << 8) | buf[4]);
    float ax = ((float)rawX * 1000.0f) / QMI8658_ACC_LSB_DIV;
    float ay = ((float)rawY * 1000.0f) / QMI8658_ACC_LSB_DIV;
    float az = ((float)rawZ * 1000.0f) / QMI8658_ACC_LSB_DIV;

    float mag = sqrtf(ax * ax + ay * ay + az * az);
    float dev = fabsf(mag - 1000.0f);
    if (dev > JOLT_DEV_MG) {
        int w = 0;
        for (int i = 0; i < joltCount; i++) {
            if (nowMs - joltTimes[i] <= JOLT_WINDOW_MS) joltTimes[w++] = joltTimes[i];
        }
        joltCount = w;
        if (joltCount < JOLT_MAX) joltTimes[joltCount++] = nowMs;
    }

    if (joltCount >= JOLT_MIN_COUNT && nowMs >= cooldownUntilMs && !g_fingerDownShared) {
        cooldownUntilMs = nowMs + ERASE_COOLDOWN_MS;
        joltCount = 0;
        g_eraseSeq++;
    }
}

/* ---- PMIC: power key events, core1 side --------------------------------
 *
 * Register names are from the AXP2101 datasheet, REG 49H (IRQ Status 1):
 * bit0 POWERON positive edge, bit1 negative edge, bit2 long press, bit3
 * short press. Long press (0x04) and short-press verdict (0x08) drive the
 * app-level actions; the press edge (0x02) is captured too, for gestures
 * that need to time this press against the previous one (the eventual
 * verdict alone cannot tell a caller that).
 *
 * Deliberately NOT batched into one 3-byte burst read from 0x48, even though
 * the registers are consecutive: unlike the touch and IMU bursts above, that
 * pattern is not something the existing, hardware-proven code already does
 * for this chip.
 */
static void pmic_poll_core1(uint32_t nowMs) {
    static uint32_t lastMs = 0;
    if (nowMs - lastMs < 40) return;
    lastMs = nowMs;

    uint8_t s0 = 0, s1 = 0, s2 = 0;
    bool ok = i2c1_read_reg_n_to(AXP2101_ADDR, 0x48, &s0, 1) &&
              i2c1_read_reg_n_to(AXP2101_ADDR, 0x49, &s1, 1) &&
              i2c1_read_reg_n_to(AXP2101_ADDR, 0x4A, &s2, 1);
    if (!ok) {
        g_pmicTimeouts++;
        return;
    }
    if (s0 || s1 || s2) {
        if (s1 & 0x0E) g_keyEvent = s1 & 0x0E;
        // Write-1-to-clear, so the next event is distinguishable from this one.
        i2c1_write_reg_to(AXP2101_ADDR, 0x48, s0);
        i2c1_write_reg_to(AXP2101_ADDR, 0x49, s1);
        i2c1_write_reg_to(AXP2101_ADDR, 0x4A, s2);
    }
}

/* ---- core1 entry point --------------------------------------------------
 *
 * Touch is gated on Touch_INT_PIN (active low, already configured as an
 * input with a pull-up in sensors_init() before this core starts): skip the
 * I2C read while the line is high, which is most of the time when no finger
 * is down, and that is where the 695us-per-loop cost measured on this board
 * (98% of frame time) actually went. aliceisjustplaying/tinydraw's own
 * writeup on this same board warns that reading *only* on the falling
 * edge - i.e. one read, then wait for the next edge - was jittery and had
 * to be reverted; this does not do that. The gate is on the *level*, so
 * every pass through this loop re-checks it: while the line is low the read
 * repeats continuously, same as the old single-core hot loop did, and it
 * only stops once the line is actually back high.
 */
static void core1_entry(void) {
    uint32_t lastFingerMs = to_ms_since_boot(get_absolute_time());
    uint32_t lastIdleHeartbeatMs = 0;
    bool wasTouchLow = false;

    while (true) {
        uint32_t nowMs = to_ms_since_boot(get_absolute_time());

        int intLow = (gpio_get(Touch_INT_PIN) == 0);
        if (intLow) {
            uint8_t fingers = 0;
            bool ok = touch_read_fingers_to(&fingers);
            g_touchReads++;
            uint16_t x = 0, y = 0;
            if (ok && fingers != 0) {
                ok = touch_read_xy_to(&x, &y);
                g_touchReads++;
            }
            if (!ok) {
                g_touchTimeouts++;
            } else {
                touch_sample_t s = { nowMs, fingers, x, y };
                if (!touch_q_push(&s)) g_touchQueueDrops++;
                if (fingers != 0) lastFingerMs = nowMs;
            }
            wasTouchLow = true;
        } else {
            // Push immediately on the low->high edge (a lift should be seen
            // right away), then keep a light heartbeat afterward so core0's
            // wall-clock timers (lift debounce, stall watchdog) keep getting
            // fresh "still no finger" timestamps without any I2C cost.
            bool edge = wasTouchLow;
            wasTouchLow = false;
            if (edge || nowMs - lastIdleHeartbeatMs >= TOUCH_IDLE_HEARTBEAT_MS) {
                lastIdleHeartbeatMs = nowMs;
                touch_sample_t s = { nowMs, 0, 0, 0 };
                if (!touch_q_push(&s)) g_touchQueueDrops++;
            }
        }

        if (nowMs - lastFingerMs >= TOUCH_STALL_MS) {
            lastFingerMs = nowMs;
            touch_recover_core1();
        }

        imu_poll_core1(nowMs);
        pmic_poll_core1(nowMs);
    }
}

/* =========================================================================
 * Public API (sensors.h)
 * ========================================================================= */

bool sensors_touch_next(touch_sample_t *out) {
    return touch_q_pop(out);
}

void sensors_set_finger_down(bool down) {
    g_fingerDownShared = down;
}

uint8_t sensors_key_take(void) {
    uint8_t ev = g_keyEvent;
    g_keyEvent = 0;
    return ev;
}

uint32_t sensors_erase_seq(void) {
    return g_eraseSeq;
}

void sensors_stats(sensors_stats_t *out) {
    out->touchReads = g_touchReads;
    out->touchTimeouts = g_touchTimeouts;
    out->touchQueueDrops = g_touchQueueDrops;
    out->touchRecoveries = g_touchRecoveries;
    out->imuTimeouts = g_imuTimeouts;
    out->pmicTimeouts = g_pmicTimeouts;
}

void sensors_init(void) {
    QMI8658_init();
    FT3168_Init(FT3168_Point_Mode);
    // The vendor never configures the interrupt line; we only read it, but it
    // has to be an input with a pull-up or the level read is meaningless.
    gpio_init(Touch_INT_PIN);
    gpio_set_dir(Touch_INT_PIN, GPIO_IN);
    gpio_pull_up(Touch_INT_PIN);
    touch_set_active();
    buttons_init();
    // Everything above touches i2c1 (QMI8658_init, FT3168_Init,
    // touch_set_active, buttons_init) and runs single-threaded on core0, so
    // there is no ownership question yet. That changes the moment
    // sensors_start() launches core1: from here on only core1 may touch
    // i2c1 (see the banner comment above core1_entry()).
}

void sensors_start(void) {
    multicore_launch_core1(core1_entry);
}
