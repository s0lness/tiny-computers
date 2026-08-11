/*
 * bootbtn: read the BOOT button at runtime.
 *
 * BOOT is not connected to a GPIO. Like every RP2xxx board, it sits on the
 * flash chip-select line, which is a QSPI pad rather than a user pin, and is
 * only there to be sampled by the bootrom at reset. It can nevertheless be
 * read from a running program: briefly stop driving the pad, let the pull-up
 * or the button decide the level, sample it, and put it back.
 *
 * The cost is that flash is unusable for those few microseconds, because the
 * chip select is not doing its job while we borrow it. So the read must
 * execute from RAM with interrupts off, and it must be brief. That is what the
 * __no_inline_not_in_flash_func wrapper in the implementation is for; calling
 * this from an interrupt handler, or on the second core while the first is
 * executing from flash, is asking for a hard fault.
 *
 * Sampling at a modest rate (say every 20ms) is plenty for a button and keeps
 * the total time spent with flash disturbed down in the parts per million.
 *
 * Whether this board's BOOT button is actually wired to the chip select the
 * way a Pico's is has NOT been confirmed from the schematic; the netlist shows
 * a "Key2" and a series resistor near the flash, which is consistent, but
 * consistent is not verified. bootbtn_selftest() exists to settle that on
 * hardware rather than by assumption.
 */
#ifndef BOOTBTN_H
#define BOOTBTN_H

#include <stdbool.h>

// True while the BOOT button is held. Safe to call from the main loop only.
// Reads the pad directly, so call it on an event rather than every iteration.
bool bootbtn_pressed(void);

// Makes the next completed press not report as a click. Used when BOOT was
// part of a chord (BOOT plus a PWR long press switches app), so that letting
// go of the chord does not also fire whatever BOOT does on its own.
void bootbtn_consume_next_click(void);

// Polls with debouncing and reports a completed press. Call once per main-loop
// iteration; it rate-limits its own sampling internally.
// Returns true exactly once per press, on release.
bool bootbtn_poll_clicked(void);

// Logs the raw level whenever it changes, so it can be established on real
// hardware whether this button is readable here at all.
void bootbtn_selftest_poll(void);

#endif // BOOTBTN_H
