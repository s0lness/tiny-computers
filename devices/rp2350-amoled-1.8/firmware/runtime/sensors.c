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
#include <stdio.h>

#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/exception.h"
#include "hardware/structs/syscfg.h"

#include "DEV_Config.h"
#include "FT3168.h"
#include "QMI8658.h"

// BOOT is not on i2c1 (see sensors.h's "BOOT button" section on why its two
// functions live in this header anyway): it borrows the flash chip select,
// which is what bootbtn.c actually does the reading. Included by relative
// path for the same reason runtime.c used to (CMakeLists.txt owns the
// include-path list, a relative path resolves regardless of how that list
// is written).
#include "../bootbtn.h"

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

// AXP2101 register 0x27, "IRQLEVEL/OFFLEVEL/ONLEVEL setting". Source: AXP2101
// datasheet (X-Powers Ltd, Rev 0.1, 2019-04-28), register map + bitfield
// table for REG 27H:
//
//   bits 7:6  reserved, reads 0
//   bits 5:4  IRQLEVEL: long-press interrupt threshold.
//             00=1s  01=1.5s (POR default)  10=2s  11=2.5s
//   bits 3:2  OFFLEVEL: hard power-off threshold, the PWRON hold that cuts
//             the rails with no firmware involvement.
//             00=4s  01=6s (POR default)  10=8s  11=10s
//   bits 1:0  ONLEVEL: how long POK must be held to power the board ON from
//             off. 00=128ms  01=512ms (POR default)  10=1s  11=2s
//
// IRQLEVEL's and OFFLEVEL's POR defaults (1.5s, 6s) match what AGENTS.md
// already documented from hardware measurement, before this function
// existed. This function reads the register rather than assuming any
// composite byte value for it, ONLEVEL's POR state included, so it is
// correct regardless of what silicon actually shipped in that field.
//
// ONLEVEL is a different field from IRQLEVEL and governs cold power-on, not
// the runtime long-press gesture; do not confuse the two when reading this
// register.
#define AXP2101_REG_LEVELS    0x27
#define AXP2101_OFFLEVEL_MASK 0x0C // bits 3:2
#define AXP2101_OFFLEVEL_10S  0x0C // 11b: the maximum, 10s

// Raises OFFLEVEL (bits 3:2) to its maximum, 10s, and leaves every other bit
// in the register untouched: read-modify-write, not a literal, so IRQLEVEL
// (the 1.5s menu-gesture threshold, which the owner has explicitly decided
// keeps its feel) and ONLEVEL survive exactly as silicon set them.
//
// Why this exists: the menu gesture is both buttons held until PWR's
// long-press verdict fires at 1.5s. At the 6s default, a child holding PWR
// past that verdict was only 4.5s from an unannounced power cut (see
// docs/decisions/0002-runtime-architecture.md section 4). 10s is the most
// margin this register can give without changing how the gesture feels.
//
// Runs on core0 in sensors_init(), before sensors_start() launches core1
// (see the ownership rule in sensors.h): this is the only point in this
// firmware's lifetime where touching i2c1 from core0 is safe.
static void pmic_raise_poweroff_threshold(void) {
    uint8_t before = DEV_I2C_Read_Byte(AXP2101_ADDR, AXP2101_REG_LEVELS);
    uint8_t target = (uint8_t)((before & ~AXP2101_OFFLEVEL_MASK) | AXP2101_OFFLEVEL_10S);
    DEV_I2C_Write_Byte(AXP2101_ADDR, AXP2101_REG_LEVELS, target);
    uint8_t after = DEV_I2C_Read_Byte(AXP2101_ADDR, AXP2101_REG_LEVELS);
    // Do not trust a write: read it back and print both values. Deliberately
    // cheap, this runs exactly once at startup, not in any hot path.
    printf("PMIC reg 0x27 (power-off threshold): before=0x%02X after=0x%02X\r\n", before, after);
    if ((after & AXP2101_OFFLEVEL_MASK) != AXP2101_OFFLEVEL_10S) {
        printf("PMIC reg 0x27: WRITE DID NOT TAKE, still not at 10s\r\n");
    }

    // ONLEVEL (bits 1:0): how long POK must be held to power the board ON
    // from off. This function's own read-modify-write only ever touches
    // AXP2101_OFFLEVEL_MASK (bits 3:2, above), so `before`'s bottom two bits
    // are exactly what ONLEVEL was already set to before this function ran,
    // untouched by it - decoded and printed here (read-only, no write of
    // its own) so that fact is legible in the boot log rather than left for
    // someone to hand-decode a raw hex byte. Grepping this codebase's own
    // history turns up no other write to register 0x27 at all, so whatever
    // this prints is silicon's own POR value: the datasheet says 01b/512ms.
    // Reported here because a small child pressing PWR to turn the device
    // back ON is exactly the scenario this field governs, and 512ms is
    // forgiving; if a future change ever needs to shorten or lengthen it,
    // this is the log line that tells you what it was before you touched
    // it.
    static const uint16_t onlevel_ms[4] = { 128, 512, 1000, 2000 };
    printf("PMIC reg 0x27 ONLEVEL (power-ON hold, bits 1:0, unmodified by this function) = %ums\r\n",
           (unsigned)onlevel_ms[before & 0x03]);
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

// core0-owned: devlink's synthetic key bits (sensors_inject_key(), see
// sensors.h for why this cannot just OR straight into g_keyEvent above)
// land here instead. sensors_key_take() OR's the two together and clears
// both; that is safe for the identical reason g_injectEraseSeq (below) is
// safe next to g_eraseSeq: each word has exactly one writer, so there is
// nothing for two cores to race over even without a barrier.
static uint8_t g_injectKeyEvent;

// core0-owned: devlink's synthetic BOOT level and click
// (sensors_inject_boot() / sensors_inject_boot_click()). Not merged into
// bootbtn.c's own state (see sensors.h's injection comment for why); OR'd on
// top of the real read instead, inside sensors_boot_down() /
// sensors_boot_clicked() below. BOOT sampling is core0-only already
// (bootbtn.c borrows the flash chip select), so, like the key injection
// above, this needs no barrier: everything that touches these two is core0.
static bool g_injectBootDown;
static bool g_injectBootClickPending;

/* ---------------------------------------------------------------------
 * PMIC software power-off. See sensors.h's "power off" section for the
 * full ownership argument (core0 requests, core1 executes).
 *
 * Register 0x10 bit 0 is confirmed straight from the datasheet text, not
 * inferred: an earlier attempt to WebFetch the AXP2101 PDF got PDF
 * structure noise back and fell through to corroborating this against
 * lewisxhe/XPowersLib instead. Downloading the same PDF
 * (m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/core/
 * Core2%20v1.1/axp2101.pdf) and running `pdftotext -layout` on it locally
 * DOES extract readable English text (some Chinese-font glyphs elsewhere in
 * the document fail to decode and print harmless syntax errors to stderr;
 * the register tables are unaffected). Section 7.5.4.3 "Power Off" lists
 * six power-off sources for register 0x10 - X-Powers Limited, AXP2101
 * datasheet, Revision 0.1, 2019-04-28:
 *
 *   "(2). Write "1" to REG10H[0]."
 *
 * and register 0x10's own bitfield table names bit 0 "Soft PWROFF",
 * access RWAC (read/write/auto-clear), "0: Normal, 1: PWROFF Config".
 * XPowersLib's value was correct.
 * ------------------------------------------------------------------- */
#define AXP2101_REG_COMMON_CONFIG 0x10
#define AXP2101_SHUTDOWN_BIT      0x01 // bit 0, "Soft PWROFF" (datasheet
                                       // section 7.5.4.3 and REG10H's own
                                       // bitfield table - see above)

// core0-owned request, core1-owned execution: see sensors_request_poweroff()
// in sensors.h. A plain volatile bool is enough synchronisation for the same
// reason g_fingerDownShared is: one writer, one reader, and a one-iteration
// delay before core1 notices is invisible for a gesture measured in seconds.
static volatile bool g_poweroffRequested;

// Set to 1 to verify the DECISION path (did firmware decide to power off)
// without ever cutting real power: the i2c write in core1_entry() below is
// skipped, but g_poweroffCmds still counts as though it had happened, so the
// once-a-second profiler line can confirm the request was seen and would
// have been acted on. MUST be 0 in anything actually shipped: this is what
// turns "the counter went up" into "the screen actually went dark", and only
// the second one is proof the button really works on real silicon. Mirrors
// sensors_inject_key()'s "THE HONESTY REQUIREMENT" comment above: a dry run
// proves the software decided correctly, it proves nothing about the AXP2101
// actually obeying the command.
#define POWEROFF_DRY_RUN 0

void sensors_request_poweroff(void) {
    g_poweroffRequested = true;
}

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

/* ---- i2c1 write: a locally-owned, fully-bounded implementation -----------
 *
 * pico-sdk 2.3.0's i2c_write_blocking_internal() DOES check a timeout on
 * both of its own wait loops (TX_EMPTY and STOP_DET) - read in full while
 * chasing where core1 froze at PHASE_PMIC_CLR, and it is sound. That freeze
 * is not this bug: see core1_entry()'s comment on that investigation.
 *
 * This local reimplementation exists anyway, on the coordinator's
 * instruction: "on the one core that owns every sensor, an unbounded call is
 * a fault waiting for its moment" - closing the class of bug is worth doing
 * even where the current SDK source reads as sound, the same reasoning that
 * justified i2c1_read_bytes_bounded() above beyond the one specific gap it
 * closes. It also means core1's i2c1 traffic no longer depends on trusting
 * an SDK internal that could change between pico-sdk versions without this
 * file noticing; every wait this core takes is now owned, and auditable, in
 * one place.
 */
// TEMPORARY diagnostic: which wait loop this function is currently
// spinning in, and how many passes it has made through the CURRENT entry
// to that loop (reset to 0 each time a new wait begins, so this reads as
// "spins so far in the loop it's in right now", not a lifetime total).
// Added after a real button press reproduced the freeze at
// PHASE_PMIC_CLR_48 with SYSCFG_PROC_CONFIG.PROC1_HALTED reading 0 - core1
// is executing, not stopped, which core0's own timeout-bounded loop should
// already have escaped within I2C_TIMEOUT_US regardless. This is what
// turns "stuck at phase 15" into "spinning in THIS exact while, N times
// and counting, or not incrementing at all" - the second case would mean
// the CPU is not looping in software here either, but stalled on the
// single register load the while-condition itself evaluates, which is the
// bus-fabric-hang shape rather than a software loop bug.
#define WRITE_WAIT_NONE     0
#define WRITE_WAIT_TX_EMPTY 1
#define WRITE_WAIT_STOP_DET 2
static volatile uint32_t g_core1WriteWaitKind;
static volatile uint32_t g_core1WriteWaitSpins;

static bool i2c1_write_bytes_bounded(uint8_t addr, const uint8_t *src, size_t len, bool nostop, uint32_t timeout_us) {
    i2c_inst_t *i2c = I2C_PORT;
    absolute_time_t deadline = make_timeout_time_us(timeout_us);

    i2c_get_hw(i2c)->enable = 0;
    i2c_get_hw(i2c)->tar = addr;
    i2c_get_hw(i2c)->enable = 1;

    bool abort = false;
    size_t byte_ctr = 0;

    for (; byte_ctr < len; ++byte_ctr) {
        bool first = byte_ctr == 0;
        bool last = byte_ctr == len - 1;

        i2c_get_hw(i2c)->data_cmd =
                (uint32_t)((first && i2c->restart_on_next) ? 1u : 0u) << I2C_IC_DATA_CMD_RESTART_LSB |
                (uint32_t)((last && !nostop) ? 1u : 0u) << I2C_IC_DATA_CMD_STOP_LSB |
                src[byte_ctr];

        // Wait for the shift register to take the byte - needs TX_EMPTY_CTRL
        // set in IC_CON, which i2c_init() (pico-sdk) already does.
        g_core1WriteWaitKind = WRITE_WAIT_TX_EMPTY;
        g_core1WriteWaitSpins = 0;
        while (!(i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_EMPTY_BITS)) {
            g_core1WriteWaitSpins++;
            if (time_reached(deadline)) { abort = true; break; }
            tight_loop_contents();
        }
        g_core1WriteWaitKind = WRITE_WAIT_NONE;
        if (abort) break;

        if (i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
            i2c_get_hw(i2c)->clr_tx_abrt; // clear-on-read; reason unused, bool result is enough here
            abort = true;
        }

        if (abort || (last && !nostop)) {
            bool stopTimedOut = false;
            g_core1WriteWaitKind = WRITE_WAIT_STOP_DET;
            g_core1WriteWaitSpins = 0;
            while (!(i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
                g_core1WriteWaitSpins++;
                if (time_reached(deadline)) { stopTimedOut = true; break; }
                tight_loop_contents();
            }
            g_core1WriteWaitKind = WRITE_WAIT_NONE;
            abort |= stopTimedOut;
            if (!stopTimedOut) i2c_get_hw(i2c)->clr_stop_det; // matches SDK's own "if (!timeout) clr_stop_det"
        }

        if (abort) break;
    }

    i2c->restart_on_next = nostop; // mirrors the SDK's own bookkeeping for a chained transfer
    return !abort && byte_ctr == len;
}

static inline bool i2c1_write_to(uint8_t addr, const uint8_t *data, size_t len) {
    return i2c1_write_bytes_bounded(addr, data, len, false, I2C_TIMEOUT_US);
}

static inline bool i2c1_write_reg_to(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c1_write_to(addr, b, 2);
}

/* ---- i2c1 read: a locally-owned, fully-bounded implementation ------------
 *
 * pico-sdk 2.3.0's own i2c_read_timeout_us() (hardware_i2c/i2c.c,
 * i2c_read_blocking_internal()) does not actually bound every wait it
 * takes - read the SDK source directly, not assumed. Its per-byte "wait for
 * room in the TX FIFO to issue the next read request" loop is:
 *
 *   while (!i2c_get_write_available(i2c)) tight_loop_contents();
 *
 * with no timeout check anywhere in it. Every OTHER wait in that same
 * function (and both waits in the write path, i2c_write_blocking_internal)
 * DOES check a timeout - this one loop is the sole exception. That makes it
 * the one call on this file's hot path that could not actually keep the
 * "always _timeout_us, never plain _blocking" promise sensors.h's ownership
 * banner makes: if the TX FIFO ever stopped draining (a wedged bus, a slave
 * holding a line), this specific wait would spin forever, on the one core
 * that owns every sensor, with no way for core0 to notice short of the
 * (chip-wide, core0-fed-so-blind-to-this) watchdog.
 *
 * i2c1_read_bytes_bounded() is that same read, reimplemented locally against
 * the same public register API (hardware/i2c.h's i2c_get_hw() /
 * i2c_get_write_available() / i2c_get_read_available(), and i2c_inst_t's own
 * restart_on_next field - all public, not SDK-internal), with every wait,
 * including the one above, checked against one deadline.
 *
 * UPDATE: the write side is ALSO now a local reimplementation
 * (i2c1_write_bytes_bounded() above), not because its SDK equivalent had
 * the same gap (it did not - both of ITS wait loops check a timeout
 * correctly, read in full) but on the coordinator's instruction to close
 * the whole class regardless. That turned out to matter: a real button
 * press later froze core1 INSIDE this local, hand-verified-bounded write
 * path anyway, with SYSCFG_PROC_CONFIG.PROC1_HALTED reading 0 (core1
 * executing, not stopped/LOCKUP). See core1_entry()'s comment and the spin
 * counters on i2c1_write_bytes_bounded() for where that investigation
 * stands - a software timeout this careful still not escaping points at
 * the register load inside the wait CONDITION itself never completing
 * (a bus-fabric stall), not a loop-logic bug, but that is being measured,
 * not assumed.
 *
 * This file's reads are always a plain, non-chained transfer (nostop=false,
 * "first byte restarts if the bus is mid-transaction, last byte stops"), so
 * that is the only shape implemented here; there is no reason to carry a
 * nostop parameter this codebase never uses.
 */
static bool i2c1_read_bytes_bounded(uint8_t addr, uint8_t *dst, size_t len, uint32_t timeout_us) {
    i2c_inst_t *i2c = I2C_PORT;
    absolute_time_t deadline = make_timeout_time_us(timeout_us);

    i2c_get_hw(i2c)->enable = 0;
    i2c_get_hw(i2c)->tar = addr;
    i2c_get_hw(i2c)->enable = 1;

    bool abort = false;
    size_t byte_ctr = 0;

    for (; byte_ctr < len; ++byte_ctr) {
        bool first = byte_ctr == 0;
        bool last = byte_ctr == len - 1;

        // THE FIX: the SDK's equivalent wait has no time_reached() check at
        // all - see this function's header comment.
        while (!i2c_get_write_available(i2c)) {
            if (time_reached(deadline)) { abort = true; break; }
            tight_loop_contents();
        }
        if (abort) break;

        i2c_get_hw(i2c)->data_cmd =
                (uint32_t)((first && i2c->restart_on_next) ? 1u : 0u) << I2C_IC_DATA_CMD_RESTART_LSB |
                (uint32_t)(last ? 1u : 0u) << I2C_IC_DATA_CMD_STOP_LSB |
                I2C_IC_DATA_CMD_CMD_BITS; // 1 = read

        while (true) {
            if (i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
                i2c_get_hw(i2c)->clr_tx_abrt; // clear-on-read; reason itself unused, bool result is enough here
                abort = true;
                break;
            }
            if (i2c_get_read_available(i2c)) break;
            if (time_reached(deadline)) { abort = true; break; }
            tight_loop_contents();
        }
        if (abort) break;

        *dst++ = (uint8_t) i2c_get_hw(i2c)->data_cmd;
    }

    i2c->restart_on_next = false; // this file's reads never chain into a further nostop transfer
    return !abort && byte_ctr == len;
}

// Register-pointer write (repeated START, no STOP) followed by a bounded
// read, same transaction shape DEV_I2C_Read_nByte uses, just with a timeout
// on both halves - both now i2c1_write_bytes_bounded()/i2c1_read_bytes_
// bounded() above, not the SDK's i2c_write_timeout_us()/i2c_read_timeout_us(),
// so every wait core1 can hit on this bus is locally owned, not just the one
// gap the read side had - see those two functions' comments for why.
static inline bool i2c1_read_reg_n_to(uint8_t addr, uint8_t reg, uint8_t *out, size_t len) {
    if (!i2c1_write_bytes_bounded(addr, &reg, 1, true, I2C_TIMEOUT_US)) return false;
    return i2c1_read_bytes_bounded(addr, out, len, I2C_TIMEOUT_US);
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

// Non-destructive read of the next sample, used only by the merge in
// sensors_touch_next() below to compare timestamps before deciding which
// ring to actually pop from.
static inline bool touch_q_peek(touch_sample_t *out) {
    uint32_t head = g_touchHead;
    if (head == g_touchTail) return false; // empty
    __dmb();
    *out = g_touchQ[head];
    return true;
}

/* ---- the injection ring: core0 (devlink) -> core0 (sensors_touch_next) --
 *
 * devlink_poll() runs on core0, inside the same main-loop iteration that
 * calls sensors_touch_next() (see runtime.c). Pushing an injected sample
 * straight into g_touchQ above from there would make core0 a second writer
 * of a ring whose entire safety argument is "core1 is the only producer"
 * (see the banner comment above touch_q_push()). So injected samples get
 * their own ring instead: g_injectQ is written only by
 * sensors_inject_touch() (core0) and read only by sensors_touch_next()
 * (also core0, in the same thread of execution, not an interrupt). One core
 * producing and consuming its own ring needs no barrier and no volatile:
 * there is no other core and nothing asynchronous anywhere in this path to
 * reorder against, unlike g_touchQ.
 *
 * sensors_touch_next() merges the two rings by timestamp rather than
 * draining one first, so an agent-injected sample and a real one that land
 * in the same frame come out in the order they actually happened, not
 * "injected always wins". An exact tie (same tMs, which at 1ms resolution
 * can genuinely happen) is broken toward the injected sample: which one a
 * human would call "first" is not knowable at that resolution anyway, and a
 * fixed, documented tie-break is all this needs. The practical takeaway for
 * the one case that matters, driving devlink and a real finger at once
 * (which tools/README-devlink.md already says not to do), is that the two
 * streams interleave correctly rather than corrupting each other; it is
 * still confusing to watch, just no longer unsafe.
 */
#define INJECT_Q_CAP 8
_Static_assert((INJECT_Q_CAP & (INJECT_Q_CAP - 1)) == 0, "INJECT_Q_CAP must be a power of two");

static touch_sample_t g_injectQ[INJECT_Q_CAP];
static uint32_t g_injectHead = 0;
static uint32_t g_injectTail = 0;

static inline bool inject_q_push(const touch_sample_t *s) {
    uint32_t tail = g_injectTail;
    uint32_t next = (tail + 1) & (INJECT_Q_CAP - 1);
    if (next == g_injectHead) return false; // full: drop, same policy touch_q_push() uses
    g_injectQ[tail] = *s;
    g_injectTail = next;
    return true;
}

static inline bool inject_q_peek(touch_sample_t *out) {
    if (g_injectHead == g_injectTail) return false; // empty
    *out = g_injectQ[g_injectHead];
    return true;
}

static inline bool inject_q_pop(touch_sample_t *out) {
    if (g_injectHead == g_injectTail) return false; // empty
    *out = g_injectQ[g_injectHead];
    g_injectHead = (g_injectHead + 1) & (INJECT_Q_CAP - 1);
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

// core0-owned: devlink's synthetic shake (sensors_inject_erase()) bumps
// this instead of g_eraseSeq above, so devlink is never a second writer of
// a word core1 also writes. sensors_erase_seq() sums the two; that is safe
// because each summand individually has exactly one writer and is
// non-decreasing, so the sum is too, which is all a "did this change since
// last time" comparison (see runtime.c) needs.
static uint32_t g_injectEraseSeq;

// Diagnostics, core1-increment-only, core0-read-only (via sensors_stats()).
static volatile uint32_t g_touchReads;
static volatile uint32_t g_touchTimeouts;
static volatile uint32_t g_touchQueueDrops;
static volatile uint32_t g_touchRecoveries;
static volatile uint32_t g_imuTimeouts;
static volatile uint32_t g_pmicTimeouts;
static volatile uint32_t g_poweroffCmds;
// Diagnostic readback around the shutdown write - see
// pmic_poweroff_poll_core1()'s comment on why this exists. 0xFF for
// g_poweroffRegAfter specifically means "the readback transaction itself
// failed", not "register 0x10 reads as 0xFF" (a real 0xFF is not a value
// this register can actually hold - bits 7:6 are hard reserved-0).
static volatile uint32_t g_poweroffRegBefore = 0xFFFFFFFFu; // sentinel: never attempted
static volatile uint32_t g_poweroffRegAfter = 0xFFFFFFFFu;

// TEMPORARY diagnostic: see core1_entry()'s comment. Remove once the
// investigation this was added for is closed.
static volatile uint32_t g_core1Loops;

// TEMPORARY diagnostic: which step of core1_entry()'s loop body last
// STARTED. g_core1Loops alone proves core1 stopped iterating, but not WHERE
// inside one iteration it stopped; this is written immediately before every
// step that can block (every i2c transaction, specifically) so a live
// incident can be read back as "stuck inside step N" rather than just
// "stuck". Remove alongside g_core1Loops once the investigation is closed.
static volatile uint32_t g_core1Phase;
#define PHASE_LOOP_TOP         1
#define PHASE_TOUCH_FINGERS_RD 2
#define PHASE_TOUCH_XY_RD      3
#define PHASE_TOUCH_PUSH       4
#define PHASE_TOUCH_IDLE_PUSH  5
#define PHASE_STALL_RECOVER    6
#define PHASE_IMU_RD           7
#define PHASE_PMIC_RD_S0       8
#define PHASE_PMIC_RD_S1       9
#define PHASE_PMIC_RD_S2       10
#define PHASE_PMIC_CLR         11
#define PHASE_PMIC_POWEROFF_RD 12
#define PHASE_PMIC_POWEROFF_WR 13
#define PHASE_LOOP_BOTTOM      14
// Sub-phases of PHASE_PMIC_CLR. Originally three (one per write-1-to-clear
// i2c1 write, when there were three separate transactions), added after a
// live capture froze AT 11 with no way to tell which one it was in.
// pmic_poll_core1() now does the clear as a single 4-byte burst transaction
// (register 0x48 plus three data bytes - see that function's comment for
// why), so only PHASE_PMIC_CLR_48 is set any more; 49/4A stay defined
// rather than renumbered, since they may still be read out of old capture
// logs, but nothing in this file sets them any more.
#define PHASE_PMIC_CLR_48      15
#define PHASE_PMIC_CLR_49      16
#define PHASE_PMIC_CLR_4A      17

/* ---- TEMPORARY diagnostic: core1 fault capture ---------------------------
 *
 * Added after the phase marker above (g_core1Phase) came back stuck at
 * PHASE_LOOP_TOP with none of touch/imu/pmic ever having been mid-read - a
 * result that rules out a blocking i2c call (every i2c call sets its own
 * phase immediately before it runs, and none of those were the last phase
 * written) and points at an actual fault: something between two phase
 * writes, in code with no i2c and no reason to ever not return, stopped
 * executing altogether. exception_set_exclusive_handler() is per-core (see
 * hardware/exception.h's own header comment - "one set of exception
 * handlers per core"), so this has to be installed from core1 itself, once,
 * before the main loop.
 *
 * IPSR (bottom 9 bits, read in the handler itself, not passed in) already
 * names which exception fired - no need for one handler per exception
 * number. The stacked frame at the exception entry SP is, in order,
 * r0 r1 r2 r3 r12 lr pc xpsr (ARM EABI exception entry, unchanged since
 * Cortex-M3): word 6 is the faulting PC, word 5 the LR at the fault.
 *
 * Deliberately no printf here either - same rule as everywhere else in this
 * file, and doubly true inside a fault handler, which cannot assume ANYTHING
 * about the state it interrupted.
 */
static volatile uint32_t g_core1FaultKind;  // IPSR exception number; 0 = no fault seen
static volatile uint32_t g_core1FaultPC;
static volatile uint32_t g_core1FaultLR;
static volatile uint32_t g_core1FaultCFSR;  // SCB->CFSR (0xE000ED28): which sub-reason
static volatile uint32_t g_core1FaultPhase; // g_core1Phase at the moment of the fault

// Not static, and marked used/noinline: it is only ever reached from the
// inline asm branch in core1_fault_handler() below, which the compiler
// cannot see as a call, so a static definition here is free to be optimised
// away or renamed at -O3, and the branch below then fails to link (seen on
// the first build of this file: "undefined reference to
// core1_fault_handler_c"). External linkage plus these two attributes keep
// the symbol present under its exact name.
void __attribute__((used, noinline)) core1_fault_handler_c(uint32_t *sp) {
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
    g_core1FaultPhase = g_core1Phase;
    g_core1FaultPC = sp[6];
    g_core1FaultLR = sp[5];
    g_core1FaultCFSR = *(volatile uint32_t *)0xE000ED28u;
    g_core1FaultKind = ipsr & 0x1FFu; // written last: this is what a reader polls for "a fault happened"
    while (true) { tight_loop_contents(); } // never return into whatever was faulting
}

static void __attribute__((naked)) core1_fault_handler(void) {
    __asm volatile (
        "mrs r0, msp    \n" // this firmware never switches to PSP (no RTOS on either core)
        "b core1_fault_handler_c \n"
    );
}

/* ---- TEMPORARY diagnostic: core1 LOCKUP and stack high-water mark --------
 *
 * Coordinator's hypothesis, after the fully-bounded read/write reimplemen-
 * tation still froze (at both a write and a read - see core1_entry()'s
 * comment): a fault that cannot be entered - because a lower-priority fault
 * is already active, or the exception frame itself cannot be stacked -
 * escalates the Cortex-M core straight into LOCKUP. In LOCKUP no handler
 * ever runs at all, which is the one explanation that fits g_core1FaultKind
 * staying 0 with the handler installed and otherwise working: there is
 * nothing to catch, not a failure to catch it.
 *
 * TWO checks, both readable from core0 regardless of whether core1 can
 * still execute anything:
 *
 * 1. SYSCFG_PROC_CONFIG.PROC1_HALTED (hardware/regs/syscfg.h) - a SHARED
 *    register (SYSCFG is a normal peripheral, not core-private PPB like
 *    DHCSR/S_LOCKUP would be), so core0 can read it directly. It is not
 *    documented as LOCKUP-specific (just "proc1 has halted"), so a positive
 *    reading is corroborating, not definitive on its own, but a clean
 *    negative (never set across a real freeze) would meaningfully weigh
 *    against LOCKUP.
 * 2. A stack canary painted across the whole of core1's stack
 *    (PICO_CORE1_STACK_SIZE, 0x800 bytes, at &__StackLimit per the linker
 *    map - see multicore.c's own core1_stack placement) before
 *    multicore_launch_core1(). Scanned from the LOW end (the deepest a
 *    stack pointer starting at the HIGH end could ever reach) upward: the
 *    first non-canary word marks the lowest address anything ever wrote,
 *    i.e. the high-water mark of core1's real stack usage. If that mark
 *    sits at or near the very bottom, a stack overflow - and MSPLIM being
 *    unable to stack its OWN fault's exception frame once there is no room
 *    left, which is exactly a fault-during-fault-entry - is a live
 *    candidate for what forces LOCKUP.
 *
 * Both are read-only with respect to i2c1 and do not depend on core1
 * executing anything, so they remain answerable no matter how core1 died.
 */
#define CORE1_STACK_CANARY 0xC1C1C1C1u

// FOUND AND FIXED: this used to be __StackLimit (0x20080000, where
// core1_stack[] - the .stack1 SECTION - is statically placed), which is
// why the first version of this instrument always read back "2048/2048
// intact": that address is not where core1's stack pointer ever lives.
// Read pico-sdk 2.3.0's multicore.c directly (multicore_launch_core1()) to
// find the real one:
//
//   extern uint32_t __StackOneBottom;
//   uint32_t *stack_limit = (uint32_t *) &__StackOneBottom;
//   // hack to reference core1_stack although that pointer is wrong....
//   uint32_t *stack = core1_stack <= stack_limit ? stack_limit : (uint32_t *) -1;
//   multicore_launch_core1_with_stack(entry, stack, sizeof(core1_stack));
//
// - the SDK's own comment calls this a "hack": multicore_launch_core1()
// (the plain, no-explicit-stack entry point this file uses) passes
// __StackOneBottom as stack_bottom, NOT core1_stack's own address, and
// only uses core1_stack for its SIZE and a sanity assert. Then
// multicore_launch_core1_with_stack() computes
// stack_ptr = stack_bottom + stack_size_bytes/4, i.e. the REAL region core1's
// SP ever occupies is [__StackOneBottom, __StackOneBottom + PICO_CORE1_STACK_SIZE)
// = [0x20080800, 0x20081000) on this build (confirmed against the .map
// file's own __StackOneBottom/__StackOneTop symbols) - the SECOND half of
// the 4KB SCRATCH_X bank, not the region .stack1 itself occupies.
extern uint32_t __StackOneBottom[];

// Runs on core0, once, before multicore_launch_core1() - see sensors_start().
static void core1_paint_stack_canary(void) {
    uint32_t *stack = __StackOneBottom;
    size_t words = PICO_CORE1_STACK_SIZE / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) stack[i] = CORE1_STACK_CANARY;
}

// Callable from core0 at any time, alive core1 or not - see this section's
// header comment. Returns bytes of headroom still intact below the deepest
// point core1's stack pointer is ever known to have reached (0 = every byte
// of the 2KB stack was touched at least once).
static uint32_t core1_stack_headroom_bytes(void) {
    uint32_t *stack = __StackOneBottom;
    size_t words = PICO_CORE1_STACK_SIZE / sizeof(uint32_t);
    size_t i = 0;
    while (i < words && stack[i] == CORE1_STACK_CANARY) i++;
    return (uint32_t)(i * sizeof(uint32_t));
}

static void core1_install_fault_handlers(void) {
    // MemManage/BusFault/UsageFault/SecureFault are routed to HardFault
    // unless individually enabled in SHCSR (0xE000ED24, bits 16/17/18/19).
    // pico-sdk's own crt0 already enables them for the boot core; core1 gets
    // its own copy of this register too (it is core-local, like the
    // exception table), so it needs the same enable here.
    volatile uint32_t *shcsr = (volatile uint32_t *)0xE000ED24u;
    *shcsr |= (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19);

    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, core1_fault_handler);
    exception_set_exclusive_handler(MEMMANAGE_EXCEPTION, core1_fault_handler);
    exception_set_exclusive_handler(BUSFAULT_EXCEPTION, core1_fault_handler);
    exception_set_exclusive_handler(USAGEFAULT_EXCEPTION, core1_fault_handler);
    exception_set_exclusive_handler(SECUREFAULT_EXCEPTION, core1_fault_handler);
}

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
    g_core1Phase = PHASE_IMU_RD;
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
 * bit0 POWERON negative edge (release), bit1 positive edge (press), bit2
 * long press, bit3 short press. Long press (0x04) and short-press verdict
 * (0x08) drive the app-level actions; the press edge (0x02) is captured
 * too, for gestures that need to time this press against the previous one
 * (the eventual verdict alone cannot tell a caller that).
 *
 * The mask below was `s1 & 0x0E` (bits 1-3 only) for a while, deliberately
 * excluding bit 0: nothing read it, so it was discarded before ever
 * reaching g_keyEvent - see sensors.h's PWR key section, "KEY_RELEASE WAS
 * DELETED FROM THIS FILE EARLIER, AND IS BACK", for the full story of why
 * that was correct then and is not correct now. Widened to `0x0F` because
 * runtime_core.c's power-off gesture (hold PWR alone for 5s) has to know
 * when a hold ENDS, and the release edge is the only signal that says so;
 * KEY_LONG is a one-shot verdict at 1.5s, not a level, and cannot stand in
 * for it.
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
    // Split from the original one-line `A && B && C` so each read can be
    // phase-marked individually - see g_core1Phase's comment. Short-circuit
    // order is unchanged: s0, then s1, then s2, same as before.
    g_core1Phase = PHASE_PMIC_RD_S0;
    bool ok = i2c1_read_reg_n_to(AXP2101_ADDR, 0x48, &s0, 1);
    if (ok) {
        g_core1Phase = PHASE_PMIC_RD_S1;
        ok = i2c1_read_reg_n_to(AXP2101_ADDR, 0x49, &s1, 1);
    }
    if (ok) {
        g_core1Phase = PHASE_PMIC_RD_S2;
        ok = i2c1_read_reg_n_to(AXP2101_ADDR, 0x4A, &s2, 1);
    }
    if (!ok) {
        g_pmicTimeouts++;
        return;
    }
    if (s0 || s1 || s2) {
        // PUBLISH FIRST, CLEAR SECOND. This order used to be reversed, and
        // that was the actual bug the acceptance test found: core1 could
        // die on the write below with the press never having reached
        // g_keyEvent at all, so a device that survived the press still
        // never acted on it - recovery brought core1 back, but the button
        // press itself was already gone. Publishing here means the owner's
        // press reaches core0 (and the app it drives) regardless of
        // whatever happens to the clear write that follows: worst case, a
        // wedge costs a stuck IRQ line and a recovery, not the press.
        if (s1 & 0x0F) g_keyEvent = s1 & 0x0F;

        // Write-1-to-clear, now ONE transaction instead of three: register
        // 0x48 followed by three consecutive data bytes, the same
        // "consecutive registers, one burst" pattern touch_read_xy_to() and
        // imu_poll_core1() above already use, for the same reason - three
        // separate transactions were three separate chances to wedge, and
        // every wedge actually observed (a real button press, decoded
        // against hardware/regs/i2c.h) landed on the FIRST of the three.
        // One transaction does not prove the wedge cannot happen; it is
        // strictly less exposure, and by now g_keyEvent above already has
        // the press no matter what this write does.
        g_core1Phase = PHASE_PMIC_CLR;
        g_core1Phase = PHASE_PMIC_CLR_48;
        uint8_t clr[4] = { 0x48, s0, s1, s2 };
        i2c1_write_to(AXP2101_ADDR, clr, sizeof(clr));
    }
}

/* ---- PMIC: software power-off, core1 side -------------------------------
 *
 * Checked every pass through core1_entry()'s loop - a volatile bool read,
 * essentially free next to the i2c transactions the rest of this loop
 * already does every iteration. See sensors.h's "power off" section and
 * g_poweroffRequested's own comment above for the core0/core1 ownership
 * argument; this function is the core1 half of it, the only half allowed to
 * actually touch i2c1.
 */
static void pmic_poweroff_poll_core1(void) {
    if (!g_poweroffRequested) return;
    g_poweroffRequested = false;
    g_poweroffCmds++;

#if !POWEROFF_DRY_RUN
    // Read-modify-write, not a literal, matching pmic_raise_poweroff_
    // threshold()'s own style: register 0x10 has other configuration bits
    // (reset, BATFET control, ...) this codebase has no reason to touch, and
    // there is no reason to guess at their state just because the board is
    // about to lose power anyway.
    // TEMPORARY: readback bisection disabled while investigating core1
    // freezing at boot (g_core1Loops stops advancing entirely, before any
    // poweroff is ever requested - so this code, gated behind
    // g_poweroffRequested, should be unreachable on that path, but it is
    // the most recent change and is being ruled out first, cheaply).
    g_core1Phase = PHASE_PMIC_POWEROFF_RD;
    uint8_t cfg = 0;
    bool ok = i2c1_read_reg_n_to(AXP2101_ADDR, AXP2101_REG_COMMON_CONFIG, &cfg, 1);
    g_poweroffRegBefore = cfg;
    if (ok) {
        cfg = (uint8_t)(cfg | AXP2101_SHUTDOWN_BIT);
        g_core1Phase = PHASE_PMIC_POWEROFF_WR;
        ok = i2c1_write_reg_to(AXP2101_ADDR, AXP2101_REG_COMMON_CONFIG, cfg);
    }
    g_poweroffRegAfter = 0xFEu; // sentinel: readback intentionally skipped, bisecting
    // Best-effort, not retried: if this specific transaction times out, the
    // OFFLEVEL backstop (pmic_raise_poweroff_threshold(), 10s) still cuts
    // power a few seconds later from PWRON continuing to be held - see that
    // function's comment for why the backstop is not made redundant by this
    // path existing.
    if (!ok) g_pmicTimeouts++;
#endif
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
    // Restored. Bisect result read back: with sound_init() removed and this
    // call also disabled, core1 ran clean for 25s (no freeze) where the same
    // config with sound_init() active froze at PHASE_PMIC_CLR within ~3.5s,
    // twice. That correlates the freeze with sound_init() running - but see
    // runtime.c's sound_init() comment for the confound this does NOT yet
    // rule out (sound_init() itself costing ~20+ms of boot time, in a race
    // shown to be sensitive to far smaller perturbations than that). Root
    // cause is open; this call stays on so a real fault is never invisible
    // again while that continues.
    core1_install_fault_handlers();

    uint32_t lastFingerMs = to_ms_since_boot(get_absolute_time());
    uint32_t lastIdleHeartbeatMs = 0;
    bool wasTouchLow = false;

    while (true) {
        g_core1Loops++; // TEMPORARY diagnostic: direct, unambiguous proof
                         // core1 is actually iterating, added while
                         // investigating why recoveries/reg10h readback
                         // never moved during a live incident. Remove once
                         // that investigation is closed.
        g_core1Phase = PHASE_LOOP_TOP;
        uint32_t nowMs = to_ms_since_boot(get_absolute_time());

        int intLow = (gpio_get(Touch_INT_PIN) == 0);
        if (intLow) {
            uint8_t fingers = 0;
            g_core1Phase = PHASE_TOUCH_FINGERS_RD;
            bool ok = touch_read_fingers_to(&fingers);
            g_touchReads++;
            uint16_t x = 0, y = 0;
            if (ok && fingers != 0) {
                g_core1Phase = PHASE_TOUCH_XY_RD;
                ok = touch_read_xy_to(&x, &y);
                g_touchReads++;
            }
            if (!ok) {
                g_touchTimeouts++;
            } else {
                g_core1Phase = PHASE_TOUCH_PUSH;
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
                g_core1Phase = PHASE_TOUCH_IDLE_PUSH;
                lastIdleHeartbeatMs = nowMs;
                touch_sample_t s = { nowMs, 0, 0, 0 };
                if (!touch_q_push(&s)) g_touchQueueDrops++;
            }
        }

        if (nowMs - lastFingerMs >= TOUCH_STALL_MS) {
            lastFingerMs = nowMs;
            g_core1Phase = PHASE_STALL_RECOVER;
            touch_recover_core1();
        }

        imu_poll_core1(nowMs);
        pmic_poll_core1(nowMs);
        pmic_poweroff_poll_core1();
        g_core1Phase = PHASE_LOOP_BOTTOM;
    }
}

/* =========================================================================
 * Public API (sensors.h)
 * ========================================================================= */

bool sensors_touch_next(touch_sample_t *out) {
    touch_sample_t realHead, injHead;
    bool haveReal = touch_q_peek(&realHead);
    bool haveInj = inject_q_peek(&injHead);
    if (!haveReal && !haveInj) return false;

    // See the injection-ring banner above for the tie-break rule.
    bool takeInj = haveInj && (!haveReal || (int32_t)(injHead.tMs - realHead.tMs) <= 0);
    return takeInj ? inject_q_pop(out) : touch_q_pop(out);
}

void sensors_inject_touch(uint8_t fingers, uint16_t x, uint16_t y) {
    touch_sample_t s = { to_ms_since_boot(get_absolute_time()), fingers, x, y };
    (void)inject_q_push(&s); // full: drop, agent can just send it again
}

void sensors_set_finger_down(bool down) {
    g_fingerDownShared = down;
}

uint8_t sensors_key_take(void) {
    uint8_t ev = g_keyEvent;
    g_keyEvent = 0;
    uint8_t injected = g_injectKeyEvent;
    g_injectKeyEvent = 0;
    return ev | injected;
}

void sensors_inject_key(uint8_t bits) {
    g_injectKeyEvent |= bits;
}

uint32_t sensors_erase_seq(void) {
    return g_eraseSeq + g_injectEraseSeq;
}

void sensors_inject_erase(void) {
    g_injectEraseSeq++;
}

// bootbtn_poll_clicked() and bootbtn_poll_level() each gate their own
// sampling on the same SAMPLE_INTERVAL_MS timer, independently (see
// bootbtn.h): calling both once per rtcore_tick(), as runtime_core.c and
// (via the level cache below) this function do, does not double the actual
// borrow rate against calling just one of them.
bool sensors_boot_clicked(void) {
    // bootbtn_poll_clicked() rate-limits its own sampling internally and must
    // still be called every loop so its debounce timer stays fed (see its
    // own header comment); calling it even when an injected click is also
    // pending costs nothing and keeps that timer honest.
    bool real = bootbtn_poll_clicked();
    if (g_injectBootClickPending) {
        g_injectBootClickPending = false;
        return true;
    }
    return real;
}

bool sensors_boot_down(void) {
    static bool level = false;
    bool sampled;
    if (bootbtn_poll_level(&sampled)) level = sampled;
    return level || g_injectBootDown;
}

void sensors_boot_consume_click(void) {
    bootbtn_consume_next_click();
}

void sensors_inject_boot(bool down) {
    g_injectBootDown = down;
}

void sensors_inject_boot_click(void) {
    g_injectBootClickPending = true;
}

void sensors_stats(sensors_stats_t *out) {
    out->touchReads = g_touchReads;
    out->touchTimeouts = g_touchTimeouts;
    out->touchQueueDrops = g_touchQueueDrops;
    out->touchRecoveries = g_touchRecoveries;
    out->imuTimeouts = g_imuTimeouts;
    out->pmicTimeouts = g_pmicTimeouts;
    out->poweroffCmds = g_poweroffCmds;
    out->poweroffRegBefore = g_poweroffRegBefore;
    out->poweroffRegAfter = g_poweroffRegAfter;
}

// TEMPORARY diagnostic accessor for g_core1Loops - see core1_entry()'s
// comment. Remove alongside it once the investigation is closed.
uint32_t sensors_debug_core1_loops(void) {
    return g_core1Loops;
}

// TEMPORARY diagnostic accessor for g_core1Phase - see its comment.
uint32_t sensors_debug_core1_phase(void) {
    return g_core1Phase;
}

// TEMPORARY diagnostic accessors for the core1 fault capture - see
// core1_fault_handler_c()'s comment.
uint32_t sensors_debug_core1_fault_kind(void) { return g_core1FaultKind; }
uint32_t sensors_debug_core1_fault_pc(void) { return g_core1FaultPC; }
uint32_t sensors_debug_core1_fault_lr(void) { return g_core1FaultLR; }
uint32_t sensors_debug_core1_fault_cfsr(void) { return g_core1FaultCFSR; }
uint32_t sensors_debug_core1_fault_phase(void) { return g_core1FaultPhase; }

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
    pmic_raise_poweroff_threshold();
    // Everything above touches i2c1 (QMI8658_init, FT3168_Init,
    // touch_set_active, buttons_init, pmic_raise_poweroff_threshold) and runs
    // single-threaded on core0, so there is no ownership question yet. That
    // changes the moment sensors_start() launches core1: from here on only
    // core1 may touch i2c1 (see the banner comment above core1_entry()).
}

/* ---- i2c1 bus recovery and core1 restart --------------------------------
 *
 * THE FIX, not another diagnostic. Evidence (a real button press, decoded
 * against hardware/regs/i2c.h): a START gets issued, the TX FIFO runs dry,
 * and no STOP ever follows - the peripheral still reports MST_ACTIVITY
 * with tx_abrt_source=0, so it does not think anything is wrong either.
 * That is the textbook signature of a slave holding the bus mid-byte, and
 * the textbook fix is not a software timeout (core1's own hand-bounded
 * wait already has one and it did not save it - a timed-out wait can still
 * leave the peripheral wedged, since the timeout only stops WAITING, it
 * does not un-stick the WIRE) but a bus recovery sequence performed with
 * the pins dropped to plain GPIO.
 *
 * i2c1_bus_recover() is that sequence: disable the peripheral, release SDA
 * and clock SCL by hand for up to nine pulses (a slave stuck mid-byte
 * releases SDA once it has seen enough clock edges to finish shifting out
 * whatever it thinks it still owes), issue a STOP condition by hand, then
 * hand the pins back to the peripheral and reinitialise it exactly as
 * DEV_Module_Init() did at boot.
 *
 * This can only be called when core1 is NOT running: it bit-bangs the
 * exact two pins the ownership rule exists to protect. sensors_restart_
 * core1() is the only caller, and it calls multicore_reset_core1() first -
 * a pure PSM-level hardware reset of core1's CPU state that works
 * regardless of what core1 was doing (a wedged i2c1 wait, a genuine
 * LOCKUP, anywhere), so there is no race between "core1 might still be
 * touching these pins" and this function touching them: by the time it
 * runs, core1 provably is not running at all.
 */
static void i2c1_bus_recover(void) {
    i2c_get_hw(I2C_PORT)->enable = 0; // stop the peripheral driving the lines while this bit-bangs them

    gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_SIO);
    gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_SIO);
    gpio_pull_up(DEV_SDA_PIN);
    gpio_pull_up(DEV_SCL_PIN);
    gpio_set_dir(DEV_SDA_PIN, GPIO_IN);  // released: the external pull-up (and this one) hold it high
    gpio_set_dir(DEV_SCL_PIN, GPIO_OUT);
    gpio_put(DEV_SCL_PIN, 1);
    sleep_us(5);

    for (int i = 0; i < 9; i++) {
        gpio_put(DEV_SCL_PIN, 0);
        sleep_us(5);
        gpio_put(DEV_SCL_PIN, 1);
        sleep_us(5);
        if (gpio_get(DEV_SDA_PIN)) break; // slave already released SDA - no need for the rest
    }

    // Manual STOP condition: SDA rises while SCL is high.
    gpio_set_dir(DEV_SDA_PIN, GPIO_OUT);
    gpio_put(DEV_SDA_PIN, 0);
    sleep_us(5);
    gpio_put(DEV_SCL_PIN, 1);
    sleep_us(5);
    gpio_set_dir(DEV_SDA_PIN, GPIO_IN); // release: pull-up brings it high while SCL is still high = STOP
    sleep_us(5);

    // Hand the pins back and bring the peripheral up exactly as
    // DEV_Module_Init() (DEV_Config.c) originally did.
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(DEV_SDA_PIN);
    gpio_pull_up(DEV_SCL_PIN);

    // THE OTHER HALF OF THE FIX, found by watching a recovered core1 die
    // again within seconds with nobody touching the device: unwedging the
    // WIRE does not clear the AXP2101's own IRQ status registers
    // (0x48/0x49/0x4A). The write that was supposed to write-1-to-clear
    // them (PHASE_PMIC_CLR) is exactly what never completed, so whatever
    // event set them is still latched on the chip. The instant the
    // relaunched core1 polls the PMIC again (every 40ms,
    // pmic_poll_core1()), it reads the SAME stale nonzero bits and walks
    // straight back into the same write-clear path - no new button press
    // required, which is exactly what was observed. Core0 clears them
    // here, once, while core1 is provably not running (same argument as
    // the bus recovery above), using the plain DEV_I2C_* calls: safe
    // because this is a single one-shot recovery step, not a hot path, and
    // if this specific write happens to hang too the caller
    // (sensors_restart_core1()) has already reset core1 and unwedged the
    // bus, so a stuck DEV_I2C_Write_Byte here blocks core0 rather than
    // leaving core1 dead - a strictly better failure than the one this is
    // fixing.
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x48, DEV_I2C_Read_Byte(AXP2101_ADDR, 0x48));
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x49, DEV_I2C_Read_Byte(AXP2101_ADDR, 0x49));
    DEV_I2C_Write_Byte(AXP2101_ADDR, 0x4A, DEV_I2C_Read_Byte(AXP2101_ADDR, 0x4A));
}

// PERMANENT: how many times core1 has been restarted after a stall -
// surfaced in the prof line (runtime.c) so a bus that wedges constantly is
// visible rather than silently papered over by a guard that just keeps
// quietly fixing it.
static volatile uint32_t g_i2c1Recoveries;

// Core0-only. See this section's header comment for the full argument.
// Called from runtime.c's core1-liveness guard once core1 has been
// observed dead for long enough to be sure, never speculatively.
void sensors_restart_core1(void) {
    multicore_reset_core1();
    i2c1_bus_recover();
    core1_paint_stack_canary();
    g_i2c1Recoveries++;
    multicore_launch_core1(core1_entry);
}

uint32_t sensors_debug_i2c1_recoveries(void) {
    return g_i2c1Recoveries;
}

void sensors_start(void) {
    core1_paint_stack_canary(); // TEMPORARY, see its comment - must run before launch
    multicore_launch_core1(core1_entry);
}

// TEMPORARY diagnostic accessors - see the core1 LOCKUP/stack section above.
bool sensors_debug_core1_halted(void) {
    return (syscfg_hw->proc_config & SYSCFG_PROC_CONFIG_PROC1_HALTED_BITS) != 0;
}

uint32_t sensors_debug_core1_stack_headroom(void) {
    return core1_stack_headroom_bytes();
}

// TEMPORARY diagnostic: i2c1's own hardware status, read live from core0.
// Unlike a transaction, a status-register read does not drive the bus, so
// it does not by itself re-open the concurrency hazard sensors.h's
// ownership rule exists to prevent (that banner's own reasoning is about
// two cores issuing TRANSACTIONS concurrently and interleaving on the
// wire; this issues none). Added because the one-shot post-sound_init
// snapshot (g_i2c1PostSound*, runtime.c) only ever showed the bus BEFORE
// anything interesting happens; this is callable every profiler tick, so
// the LAST reading before a freeze is the state AT (or a second after) the
// hang, which is the one the coordinator actually asked to decode.
//
// ONE CAVEAT THAT DOES MATTER: tx_abrt_source is clear-on-read (datasheet,
// and see pmic_poweroff_poll_core1()'s own comment on the twin register).
// Calling this while core1 is STILL ALIVE could clear an abort flag out
// from under core1's own next read of it, which core1 relies on for real
// (touch/imu/pmic timeout accounting) - a live core1 is the ownership
// rule's actual concern, not a dead one. The caller (runtime.c) MUST gate
// this on core1 already being observed dead (core1LoopsPerSec == 0) - see
// the call site there.
void sensors_debug_i2c1_live(uint32_t *enable, uint32_t *enableStatus, uint32_t *status,
                              uint32_t *rawIntrStat, uint32_t *txAbrtSource,
                              uint32_t *txflr, uint32_t *rxflr) {
    i2c_hw_t *hw = i2c_get_hw(I2C_PORT);
    *enable = hw->enable;
    *enableStatus = hw->enable_status;
    *status = hw->status;
    *rawIntrStat = hw->raw_intr_stat;
    *txAbrtSource = hw->tx_abrt_source; // clear-on-read: this is the only
                                         // reader anywhere of this register
                                         // now that core1 is presumed dead,
                                         // so reading it here does not race
                                         // core1's own (now-moot) reads.
    *txflr = hw->txflr;
    *rxflr = hw->rxflr;
}

// TEMPORARY diagnostic accessors for the write-side spin counters - see
// i2c1_write_bytes_bounded()'s comment.
uint32_t sensors_debug_core1_write_wait_kind(void) { return g_core1WriteWaitKind; }
uint32_t sensors_debug_core1_write_wait_spins(void) { return g_core1WriteWaitSpins; }

// TEMPORARY diagnostic: SDA/SCL read as plain GPIO levels, bypassing the
// i2c peripheral entirely - gpio_get() reads the pad's synchronized input
// value regardless of which function (GPIO_FUNC_I2C here) the pin is
// muxed to, so this is the ELECTRICAL truth on the wire, not the
// peripheral's opinion of it. The classic "a slave is holding SDA low"
// bus-lockup signature: if SDA reads 0 with nobody mid-transaction, no
// master, this one included, can ever complete a START condition again,
// and the fix is a bus recovery (toggle SCL manually, clock out up to nine
// pulses, issue a STOP) rather than anything in the transaction code
// above. Safe from core0 at any time: reading a GPIO input level is not a
// bus transaction and cannot race core1's own use of the pins.
void sensors_debug_i2c1_pins(bool *sda, bool *scl) {
    *sda = gpio_get(DEV_SDA_PIN) != 0;
    *scl = gpio_get(DEV_SCL_PIN) != 0;
}
