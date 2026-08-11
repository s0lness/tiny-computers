/*
 * appswitch: hold PWR to jump to the other app slot.
 *
 * The device holds two apps, in slot A and slot B, and always boots straight
 * into one of them. There is no launcher: switching is a long press, so the
 * "menu" is a thumb on the one button you can find without looking.
 *
 * How the button is actually read matters, because the obvious answer is
 * wrong. PWR is not wired to the MCU at all. The schematic has it going to the
 * AXP2101 PMIC's PWRON pin, and `SYS_OUT` (GPIO18), which the vendor header
 * names as though it were the key, carries PWROK, a power-good indicator that
 * does not move when the button is pressed. Verified across several sessions:
 * pressing either button leaves GPIO18 untouched.
 *
 * So the press arrives second hand. The PMIC pulls its interrupt line (GPIO2)
 * low and latches a bit in register 0x49: 0x02 press, 0x01 release, 0x08 short
 * press, 0x04 long press. The long-press bit is the one this module wants, and
 * the PMIC times it in hardware, so no software timer is involved and there is
 * no assumption about the line staying asserted.
 *
 * Thresholds live in PMIC register 0x27. Defaults, both measured on this
 * board: the long-press interrupt fires at 1.5s (observed at 1480ms) and the
 * hard power-off at 6s (a 4.5s hold did not cut power). That 4.5s gap is what
 * makes a hold usable as a gesture at all.
 *
 * Integration: main.c already reads register 0x49. When it sees the long-press
 * bit, it calls appswitch_go_other(). Nothing here polls anything.
 */
#ifndef APPSWITCH_H
#define APPSWITCH_H

#include <stdbool.h>
#include <stdint.h>

// Which slot this build is running from, worked out from the running code's
// own address rather than a compile-time flag, so one source builds for either
// slot and cannot disagree with where it was actually loaded.
typedef enum {
    APPSWITCH_SLOT_A = 0,
    APPSWITCH_SLOT_B = 1,
    APPSWITCH_SLOT_UNKNOWN = -1,
} appswitch_slot_t;

appswitch_slot_t appswitch_current_slot(void);

// Reboots into the other slot. Does not return on success. If the current slot
// cannot be determined, or the other slot holds no valid image, this returns
// so the caller can carry on rather than bricking the session.
void appswitch_go_other(void);

#endif // APPSWITCH_H
