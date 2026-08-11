// See appswitch.h for why the PWR button is read through the PMIC rather than
// from a GPIO. This file only has to answer two questions: which slot am I,
// and how do I hand over to the other one.

#include <stdio.h>

#include "pico/bootrom.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "appswitch.h"
#include "bootreq.h"

// How switching actually works, and why it is not a one-line reboot call:
//
// The first thing tried here was `rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE
// | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, ..., target_offset, 0)` aimed at the
// other slot's flash offset. It never switched. The RP2350 datasheet (section
// 5.1.16) says why, verbatim: "Flash update and version downgrade have no
// effect when using a single slot, or standalone (non A/B) partitions." Our
// slot_a/slot_b are deliberately standalone, not an A/B pair (see
// store/partitions.json and store/README.md), so FLASH_UPDATE was a no-op
// every time: the bootrom fell back to its default and re-booted slot A,
// which from the outside looked exactly like switching into the same app
// twice.
//
// Linking them as an A/B pair to make FLASH_UPDATE work is not the fix
// either: the same datasheet section says that boot type erases "the first
// sector of the other image" on every such boot, which would destroy the
// slot being switched AWAY from on every single switch.
//
// The documented mechanism for "run this other, unrelated image without
// touching it" is chain_image(), and that has to run from a separate,
// unpartitioned first-stage image (store/bootloader/), because chain_image()
// itself must be called on the way TO the target, not FROM the app currently
// running — see the bootloader's own comments for the mechanics. This file's
// job shrinks to: leave that bootloader a note (bootreq_write(), see
// bootreq.h for the scratch-register protocol and why it survives a
// watchdog reboot but not a power cycle) and do a plain watchdog reboot so
// the bootloader runs next.

appswitch_slot_t appswitch_current_slot(void) {
    // Ask the bootrom which partition it booted, rather than inferring it from
    // the address of our own code.
    //
    // Inferring from the address is what an earlier version did, and it cannot
    // work here: the RP2350 bootrom sets up flash address translation so that a
    // partition's image appears at 0x10000000 whichever slot it lives in. That
    // is also why one normally-linked binary boots from either slot and no
    // per-slot build is needed, which is worth knowing because picotool itself
    // does not relocate anything.
    boot_info_t info;
    if (!rom_get_boot_info(&info)) return APPSWITCH_SLOT_UNKNOWN;
    if (info.partition == 0) return APPSWITCH_SLOT_A;
    if (info.partition == 1) return APPSWITCH_SLOT_B;
    return APPSWITCH_SLOT_UNKNOWN;
}

void appswitch_go_other(void) {
    // Announce first. Everything below can fail in ways that leave nothing to
    // print with, and a silent hang is far harder to place than a hang with a
    // last known line. This was learned by hanging here: the probe that used
    // to run before any output faulted, so the log showed the long press
    // arriving and then simply stopped.
    printf("appswitch: entered\r\n");

    appswitch_slot_t slot = appswitch_current_slot();
    if (slot == APPSWITCH_SLOT_UNKNOWN) {
        // Running unpartitioned, straight from 0x10000000, which is what a
        // plain `picotool load` produces. Switching is meaningless here.
        printf("appswitch: not running from a slot, staying put\r\n");
        return;
    }

    appswitch_slot_t other = (slot == APPSWITCH_SLOT_A) ? APPSWITCH_SLOT_B : APPSWITCH_SLOT_A;

    // Validation of the *other* slot's image is not this app's job any more.
    // It used to be probed here by reading a few words through the XIP window
    // at the slot's raw flash offset, which is what hung the board: the
    // bootrom maps the *booted* partition to 0x10000000, so the other slot's
    // raw address is not reliably addressable from a running app, and the
    // load faulted with interrupts still enabled and nothing left to run.
    // That validation now belongs entirely to store/bootloader/main.c, which
    // calls chain_image() (the bootrom checks the image before handing
    // control over, and falls back to slot 0 — then BOOTSEL — if it is
    // invalid) rather than to code running from the slot being switched away
    // from.
    printf("appswitch: %c -> %c, requesting via watchdog scratch and rebooting\r\n",
           (slot == APPSWITCH_SLOT_A) ? 'A' : 'B', (other == APPSWITCH_SLOT_A) ? 'A' : 'B');

    // Leave the bootloader a note, then hand off. bootreq_write() only writes
    // a watchdog scratch register; it does not touch flash and cannot itself
    // fail. watchdog_reboot(0, 0, 0) is a *plain* reboot: pc=0 means "boot
    // the normal way" as far as the bootrom's own reboot-to-address mechanism
    // is concerned (see bootreq.h's header comment for exactly which scratch
    // registers that touches), so store/bootloader/main.c is what actually
    // runs next and decides where to go from the note left here.
    bootreq_write((uint8_t)other);

    sleep_ms(10); // give the printf above a chance to reach the host

    watchdog_reboot(0, 0, 0);

    // Only reached if watchdog_reboot() itself somehow did not take, which
    // nothing in the SDK suggests is possible for these arguments, but there
    // is nothing to lose by saying so if it happens.
    printf("appswitch: reboot did not happen\r\n");
}

void appswitch_go_next(void) {
    printf("appswitch: entered (next)\r\n");

    appswitch_slot_t slot = appswitch_current_slot();
    if (slot == APPSWITCH_SLOT_UNKNOWN) {
        printf("appswitch: not running from a slot, staying put\r\n");
        return;
    }

    // Cycle. Which of the next slots actually holds an image is not knowable
    // from here: reading another partition's flash directly is what faulted
    // and hung the board once already, because the bootrom maps the booted
    // partition to 0x10000000 and the others are not reliably addressable from
    // a running app. So ask for the next one and let the bootloader, which has
    // the partition table and the bootrom's own validation, skip the empties.
    uint8_t next = (uint8_t)((slot + 1) % APPSWITCH_SLOT_COUNT);
    printf("appswitch: %u -> %u, rebooting\r\n", (unsigned)slot, (unsigned)next);

    bootreq_write(next);
    watchdog_reboot(0, 0, 0);

    printf("appswitch: reboot refused\r\n");
}
