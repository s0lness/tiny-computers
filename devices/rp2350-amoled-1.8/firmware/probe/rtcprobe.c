/* rtcprobe: the three hardware questions docs/decisions/0011 could not
 * settle from the schematic alone, in one standalone image.
 *
 * THIS IS NOT PART OF THE PRODUCT. It builds its own .uf2 from its own
 * CMakeLists in this directory, links nothing from firmware/runtime, and
 * never touches the panel. It exists so the answers in decision 0011 can be
 * upgraded from "the schematic and two datasheets say so" to "measured on
 * this puck", without putting an experiment inside the firmware a child
 * holds.
 *
 * It was written and built but DELIBERATELY NOT FLASHED (the owner was
 * asleep and flashing is a physical act on a device someone might be
 * holding). See decision 0011's "What the probe would tell us" for the
 * protocol; the short version is:
 *
 *   1. flash it, read the serial output
 *   2. unplug USB, hold PWR for 12s until the screen goes black
 *   3. wait a few minutes by a wall clock
 *   4. plug back in, read the serial output again
 *
 * and the question is whether section B's clock advanced by the real
 * elapsed time with OS=0. If it did, this device keeps time across a power
 * cycle and the clock app is a clock. If OS came back 1, it does not, and
 * the clock app is a stopwatch with a face.
 *
 * WARNING, section D writes flash. It erases and programs ONE sector, the
 * LAST one (offset 0x00FFF000, 4KB below the top of the 16MB part), which
 * is 15.9MB clear of the ~106KB image and outside every region
 * store/partitions.json ever claimed. It does not touch the running image.
 * Section D is still gated behind PROBE_WRITE_FLASH below, off by default,
 * because "the probe that answered a read-only question also wrote to
 * flash" is not a sentence anyone should have to read after the fact.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/i2c.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// Section D (erase + program + read back, and what parking core1 costs).
// Off by default: turn it on with -DPROBE_WRITE_FLASH=1 and know that you
// are asking for a flash write.
#ifndef PROBE_WRITE_FLASH
#define PROBE_WRITE_FLASH 0
#endif

// The one i2c bus on this board. AGENTS.md's pin table, confirmed against
// the schematic: RTC_SDA/RTC_SCL, QMI_SDA/QMI_SCL and AXP2101_SDA/SCL are
// all the same two nets, GPIO6 and GPIO7.
#define PROBE_I2C     i2c1
#define PROBE_SDA_PIN 6
#define PROBE_SCL_PIN 7

#define ADDR_PCF85063 0x51 // NXP PCF85063A/ATL, fixed: A2h write / A3h read
#define ADDR_AXP2101  0x34

// PCF85063A register map (NXP PCF85063A data sheet, Rev. 7):
//   00 Control_1, 01 Control_2, 02 Offset, 03 RAM_byte,
//   04 Seconds (bit7 = OS, oscillator-stop), 05 Minutes, 06 Hours,
//   07 Days, 08 Weekdays, 09 Months, 0A Years
#define PCF_REG_CONTROL_1 0x00
#define PCF_REG_RAM_BYTE  0x03
#define PCF_REG_SECONDS   0x04

// Written into RAM_byte (03h) the first time this probe ever runs on a
// given device. RAM_byte is powered from the same VDD as the counters, so
// if it comes back on a later boot the RTC domain never lost power, and it
// says so independently of whether anybody trusts the OS flag.
#define PROBE_RAM_MAGIC 0x5A

static int reg_read(uint8_t addr, uint8_t reg, uint8_t *dst, size_t len) {
    int w = i2c_write_timeout_us(PROBE_I2C, addr, &reg, 1, true, 5000);
    if (w != 1) return -1;
    int r = i2c_read_timeout_us(PROBE_I2C, addr, dst, len, false, 5000);
    return (r == (int)len) ? 0 : -1;
}

static int reg_write(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_write_timeout_us(PROBE_I2C, addr, b, 2, false, 5000) == 2 ? 0 : -1;
}

static unsigned bcd(uint8_t v) { return (unsigned)((v >> 4) * 10 + (v & 0x0F)); }

/* --- A: what is actually on the bus ---------------------------------- */
static void section_a_bus_scan(void) {
    printf("\r\n[A] i2c1 scan (GPIO%d/%d)\r\n", PROBE_SDA_PIN, PROBE_SCL_PIN);
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t b;
        // A one-byte read is the least invasive probe that still needs an
        // ACK from the addressed device.
        if (i2c_read_timeout_us(PROBE_I2C, a, &b, 1, false, 3000) == 1) {
            const char *who = "?";
            if (a == ADDR_AXP2101) who = "AXP2101 (PMIC)";
            else if (a == ADDR_PCF85063) who = "PCF85063A (RTC)";
            else if (a == 0x38) who = "FT3168 (touch)";
            else if (a == 0x6A || a == 0x6B) who = "QMI8658C (IMU)";
            else if (a == 0x18) who = "ES8311 (codec)";
            printf("  0x%02X  %s\r\n", a, who);
        }
    }
    printf("  (an address that does NOT appear is the answer for anything "
           "expected: a missing 0x51 means no RTC on this board)\r\n");
}

/* --- B: does the RTC know what time it is ----------------------------- */
static void section_b_rtc(bool *sawRtc) {
    uint8_t regs[11];
    printf("\r\n[B] PCF85063A at 0x%02X\r\n", ADDR_PCF85063);
    if (reg_read(ADDR_PCF85063, PCF_REG_CONTROL_1, regs, sizeof regs) != 0) {
        printf("  NO ANSWER. Either the part is absent, or VCC-RTC is down, "
               "or the bus is wedged (check section A).\r\n");
        *sawRtc = false;
        return;
    }
    *sawRtc = true;

    uint8_t seconds = regs[PCF_REG_SECONDS];
    bool osStopped = (seconds & 0x80) != 0;
    uint8_t ram = regs[PCF_REG_RAM_BYTE];

    printf("  Control_1=0x%02X Control_2=0x%02X RAM_byte=0x%02X\r\n",
           regs[0], regs[1], ram);
    printf("  OS (oscillator stop, Seconds bit 7) = %d  -> the time below is %s\r\n",
           osStopped ? 1 : 0,
           osStopped ? "NOT TRUSTWORTHY (power to the RTC domain was lost)"
                     : "trustworthy (the crystal has run continuously)");
    printf("  time = 20%02u-%02u-%02u %02u:%02u:%02u\r\n",
           bcd(regs[0x0A]), bcd(regs[0x09] & 0x1F), bcd(regs[0x07] & 0x3F),
           bcd(regs[0x06] & 0x3F), bcd(regs[0x05] & 0x7F), bcd(seconds & 0x7F));

    if (ram == PROBE_RAM_MAGIC) {
        printf("  RAM_byte still holds this probe's magic: the RTC domain has "
               "NOT lost power since a previous run of this probe.\r\n");
    } else {
        printf("  RAM_byte is not this probe's magic (first run here, or the "
               "RTC domain lost power). Writing 0x%02X and a known time now.\r\n",
               PROBE_RAM_MAGIC);
        reg_write(ADDR_PCF85063, PCF_REG_RAM_BYTE, PROBE_RAM_MAGIC);
        // A fixed, obviously-synthetic stamp: 2000-01-01 12:00:00. Nothing
        // reads this but the next run of this probe, and a value that is
        // plainly not the real date keeps it from being mistaken for one.
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 0, 0x00); // seconds, clears OS
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 1, 0x00); // minutes
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 2, 0x12); // hours
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 3, 0x01); // days
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 5, 0x01); // months
        reg_write(ADDR_PCF85063, PCF_REG_SECONDS + 6, 0x00); // years
        printf("  Now: power the board fully off, wait a measured number of "
               "minutes, power on, and run this again. If the clock advanced "
               "by that number with OS=0, this device keeps time.\r\n");
    }

    // A second sample a second later proves the oscillator is actually
    // counting, which OS=0 alone does not (OS=0 only says it has not been
    // observed to stop since the flag was last cleared).
    uint8_t again = 0;
    sleep_ms(1100);
    if (reg_read(ADDR_PCF85063, PCF_REG_SECONDS, &again, 1) == 0) {
        printf("  seconds sampled 1.1s apart: %02u then %02u -> oscillator %s\r\n",
               bcd(seconds & 0x7F), bcd(again & 0x7F),
               ((again & 0x7F) != (seconds & 0x7F)) ? "IS running" : "IS NOT running");
    }
}

/* --- C: is there a battery, and does anything back up the RTC --------- */
static void section_c_pmic(void) {
    uint8_t st[2] = {0, 0};
    printf("\r\n[C] AXP2101 at 0x%02X\r\n", ADDR_AXP2101);
    if (reg_read(ADDR_AXP2101, 0x00, st, 2) != 0) {
        printf("  NO ANSWER (this would be very surprising: the runtime talks "
               "to this chip every boot).\r\n");
        return;
    }
    printf("  REG00H=0x%02X REG01H=0x%02X\r\n", st[0], st[1]);
    printf("  REG00H[5] VBUS_GD = %d (USB present)\r\n", (st[0] >> 5) & 1);
    printf("  REG00H[3] battery detect = %d  <- THE ANSWER TO 'is a cell "
           "actually plugged into J1'\r\n", (st[0] >> 3) & 1);
    printf("  (AXP2101 datasheet Rev 0.1 s7.5.3.1 for VBUS_GD, and s7.7's "
           "'the detection result is saved in reg00H[3]' for battery detect. "
           "If bat_det_en is disabled the PMU reports battery-always-present, "
           "so a 1 here is worth confirming against a real unplug.)\r\n");

    uint8_t chg = 0;
    if (reg_read(ADDR_AXP2101, 0x18, &chg, 1) == 0) {
        printf("  REG18H=0x%02X, bit 2 = backup-battery charger enable = %d\r\n",
               chg, (chg >> 2) & 1);
        printf("  (Only meaningful if a cell is fitted at H1. On the shipped "
               "board H1 is marked NC and its pads are empty, so this bit "
               "charges nothing.)\r\n");
    }
}

/* --- D: what a flash save actually costs ------------------------------ */
#if PROBE_WRITE_FLASH

// The last 4KB sector of the 16MB part. 15.9MB clear of the image, and
// above every region store/partitions.json ever named.
#define PROBE_FLASH_OFFSET (16u * 1024u * 1024u - FLASH_SECTOR_SIZE)

static volatile uint32_t g_core1Loops;
static volatile bool g_core1Go;

// Stands in for sensors.c's core1: spins as fast as it can and counts, so
// the cost of parking it is a number rather than an argument. The real one
// runs above 1M loops/sec (runtime.c's CORE1_STALL_MS comment).
static void core1_spin(void) {
    // Without this, flash_safe_execute() cannot lock this core out and
    // returns PICO_ERROR_NOT_PERMITTED. It is exactly the call the product
    // firmware would have to add to sensors.c's core1_entry().
    if (!flash_safe_execute_core_init()) {
        printf("  core1: flash_safe_execute_core_init() FAILED\r\n");
    }
    while (!g_core1Go) tight_loop_contents();
    for (;;) {
        g_core1Loops++;
        tight_loop_contents();
    }
}

static void do_erase(void *param) {
    (void)param;
    flash_range_erase(PROBE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

static uint8_t g_page[FLASH_PAGE_SIZE];

static void do_program(void *param) {
    (void)param;
    flash_range_program(PROBE_FLASH_OFFSET, g_page, FLASH_PAGE_SIZE);
}

static void section_d_flash(void) {
    printf("\r\n[D] flash write with core1 running and parked\r\n");
    multicore_launch_core1(core1_spin);
    sleep_ms(50);
    g_core1Go = true;
    sleep_ms(200);

    uint32_t l0 = g_core1Loops;
    sleep_ms(100);
    uint32_t rate = (uint32_t)((g_core1Loops - l0) * 10u); // loops/sec
    printf("  core1 free-running at about %lu loops/sec\r\n", (unsigned long)rate);

    // Erase.
    l0 = g_core1Loops;
    uint64_t t0 = time_us_64();
    int rc = flash_safe_execute(do_erase, NULL, 1000);
    uint64_t eraseUs = time_us_64() - t0;
    printf("  erase 4KB: rc=%d  wall=%lu us  core1 advanced %lu loops during it\r\n",
           rc, (unsigned long)eraseUs, (unsigned long)(g_core1Loops - l0));
    printf("  (W25Q128JV tSE is 45ms typical, 400ms MAXIMUM. Anything near "
           "400ms is the number the design has to survive, not the typical.)\r\n");

    // Program one page.
    for (size_t i = 0; i < sizeof g_page; i++) g_page[i] = (uint8_t)(i ^ 0xA5);
    l0 = g_core1Loops;
    t0 = time_us_64();
    rc = flash_safe_execute(do_program, NULL, 1000);
    uint64_t progUs = time_us_64() - t0;
    printf("  program 256B: rc=%d  wall=%lu us  core1 advanced %lu loops\r\n",
           rc, (unsigned long)progUs, (unsigned long)(g_core1Loops - l0));
    printf("  (tPP is 0.4ms typical, 3ms max. If this lands under a "
           "millisecond, an append-only save is invisible inside one frame.)\r\n");

    // Read it back through the XIP window. Safe HERE only because nothing
    // in this probe borrows the flash chip select; in the product, this
    // read must happen on core0 and never on core1. See decision 0011.
    const uint8_t *seen = (const uint8_t *)(XIP_BASE + PROBE_FLASH_OFFSET);
    bool ok = true;
    for (size_t i = 0; i < sizeof g_page; i++) {
        if (seen[i] != g_page[i]) { ok = false; break; }
    }
    printf("  read back through XIP: %s\r\n", ok ? "MATCHES" : "DOES NOT MATCH");
}
#endif // PROBE_WRITE_FLASH

int main(void) {
    stdio_init_all();

    // USB CDC drops everything written before a host opens the port
    // (bootbtn.c learned this the expensive way), so wait, then say
    // everything twice-over slowly rather than once at boot.
    sleep_ms(3000);

    i2c_init(PROBE_I2C, 400 * 1000);
    gpio_set_function(PROBE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PROBE_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PROBE_SDA_PIN);
    gpio_pull_up(PROBE_SCL_PIN);

    for (;;) {
        printf("\r\n===== rtcprobe (docs/decisions/0011) =====\r\n");
        section_a_bus_scan();
        bool sawRtc = false;
        section_b_rtc(&sawRtc);
        section_c_pmic();
#if PROBE_WRITE_FLASH
        static bool flashDone = false;
        if (!flashDone) { flashDone = true; section_d_flash(); }
#else
        printf("\r\n[D] skipped: built with PROBE_WRITE_FLASH=0, so this "
               "image never writes flash.\r\n");
#endif
        printf("\r\n===== repeating in 10s =====\r\n");
        sleep_ms(10000);
    }
}
