// See bootbtn.h for why reading this button is unusual and what it costs.

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/pads_qspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "bootbtn.h"

// The chip select is index 1 in the QSPI pad bank (0 is SCLK).
#define QSPI_CS_INDEX 1

// How often the pad is actually borrowed. Every sample disturbs flash for a
// few microseconds, so this is deliberately slow: 50ms is still far finer
// than a human press and keeps the disturbance negligible. Raised from the
// original 20ms when this got wired into a real app loop, to cut exposure
// (fewer chip-select borrows per second) now that it runs continuously
// rather than only in a self-test.
#define SAMPLE_INTERVAL_MS 50

// Must run from RAM: it makes flash unreadable for its duration, so a function
// executing in place from flash would fetch its own next instruction through a
// chip select it has just taken away.
static bool __no_inline_not_in_flash_func(read_cs_low)(void) {
    uint32_t flags = save_and_disable_interrupts();

    // Enable the pad's input buffer. On this part it is off for the QSPI pads
    // in normal operation, since nothing ever reads them back, and with it off
    // the pin reads as a constant zero no matter what the line is doing. That
    // is what an earlier version of this measured: a resting level that never
    // moved, which looked like a button that was not wired up.
    uint32_t padSaved = pads_qspi_hw->io[QSPI_CS_INDEX];
    hw_write_masked(&pads_qspi_hw->io[QSPI_CS_INDEX],
                    PADS_QSPI_GPIO_QSPI_SS_IE_BITS,
                    PADS_QSPI_GPIO_QSPI_SS_IE_BITS | PADS_QSPI_GPIO_QSPI_SS_OD_BITS);

    // Stop driving the chip select, so the line floats to whatever the
    // external circuit says. Held high by the flash pull-up normally; pulled
    // low by the button.
    hw_write_masked(&ioqspi_hw->io[QSPI_CS_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Let the line settle. The pull-up is weak and the trace has capacitance,
    // so an immediate read catches the old level. This loop is the reason the
    // whole thing costs microseconds rather than nanoseconds.
    for (volatile int i = 0; i < 1000; i++) {
        tight_loop_contents();
    }

    // The chip select's input bit is NOT at the pad's index. On RP2350 the
    // high-bank input register packs GPIO32..47 in the low half and the QSPI
    // and USB pins in the top byte, so CSn is bit 27. The RP2040 recipe that
    // everyone copies uses the pad index, which here reads GPIO33: a pin that
    // does not exist on this package and always returns zero. That produced a
    // resting level which never moved, and looked exactly like a button that
    // was not wired up.
    bool low = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);

    // Give the pad back before anything tries to fetch from flash again.
    hw_write_masked(&ioqspi_hw->io[QSPI_CS_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    pads_qspi_hw->io[QSPI_CS_INDEX] = padSaved;

    restore_interrupts(flags);
    return low;
}

bool bootbtn_pressed(void) {
    return read_cs_low();
}

static bool g_swallowNextClick = false;

void bootbtn_consume_next_click(void) {
    g_swallowNextClick = true;
}

bool bootbtn_poll_clicked(void) {
    static uint32_t lastSampleMs = 0;
    static bool held = false;

    uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    if (nowMs - lastSampleMs < SAMPLE_INTERVAL_MS) return false;
    lastSampleMs = nowMs;

    bool now = read_cs_low();
    if (now == held) return false;
    held = now;

    // Report on release, which is the conventional moment for a click and
    // avoids firing repeatedly while the button is held down.
    if (now) return false;

    if (g_swallowNextClick) {
        g_swallowNextClick = false;
        return false;
    }
    return true;
}

bool bootbtn_poll_level(bool *level) {
    static uint32_t lastSampleMs = 0;

    uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    if (nowMs - lastSampleMs < SAMPLE_INTERVAL_MS) return false;
    lastSampleMs = nowMs;

    *level = read_cs_low();
    return true;
}

void bootbtn_selftest_poll(void) {
    static uint32_t lastMs = 0;
    static int prev = -1;

    uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    if (nowMs - lastMs < SAMPLE_INTERVAL_MS) return;
    lastMs = nowMs;

    int lvl = read_cs_low() ? 1 : 0;
    if (lvl != prev) {
        prev = lvl;
        printf("BOOT %s t=%lu\r\n", lvl ? "DOWN" : "up", (unsigned long)nowMs);
        return;
    }

    // Also report the resting level on a slow tick. Reporting only on change
    // looks efficient and is useless here: the one line that establishes the
    // idle level is printed within milliseconds of boot, and USB CDC throws
    // away everything written before a host opens the port. A flat level then
    // produces no output at all, which is indistinguishable from the polling
    // never running. It cost one round trip to learn that.
    static uint32_t lastTickMs = 0;
    if (nowMs - lastTickMs >= 2000) {
        lastTickMs = nowMs;
        printf("BOOT idle level=%d t=%lu\r\n", lvl, (unsigned long)nowMs);
    }
}
