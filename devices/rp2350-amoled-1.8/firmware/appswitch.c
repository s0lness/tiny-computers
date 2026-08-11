// See appswitch.h for why the PWR button is read through the PMIC rather than
// from a GPIO. This file only has to answer two questions: which slot am I,
// and how do I hand over to the other one.

#include <stdio.h>

#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"
#include "appswitch.h"

// Fixed by store/partitions.json. The firmware cannot read that JSON at build
// time, and picotool does not relocate a binary when loading it into a
// partition: the write addresses come from the file's own link-time addresses.
// So these constants are doing double duty, as what this code compares its own
// address against, and as what each slot's build must be linked at. Change
// partitions.json and you must change these, and the linker script, together.
#define XIP_BASE_ADDR 0x10000000u
#define SLOT_A_ADDR   (XIP_BASE_ADDR + 0x00100000u)
#define SLOT_B_ADDR   (XIP_BASE_ADDR + 0x00800000u)
#define SLOT_END_ADDR (XIP_BASE_ADDR + 0x00F00000u)

// A slot that has never been written reads as erased flash. Treat that as
// empty rather than jumping into it.
#define FLASH_ERASED_WORD 0xFFFFFFFFu

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

// Reads through the XIP window at the slot's real flash offset. Address
// translation maps the *booted* partition to 0x10000000, but the raw offsets
// remain addressable, so this can inspect the other slot.
static bool slot_looks_populated(uint32_t addr) {
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)addr;
    // Erased flash is all ones. A handful of words is enough to tell an image
    // from an empty slot without pretending to validate it.
    for (int i = 0; i < 8; i++) {
        if (p[i] != FLASH_ERASED_WORD) return true;
    }
    return false;
}

void appswitch_go_other(void) {
    appswitch_slot_t slot = appswitch_current_slot();
    if (slot == APPSWITCH_SLOT_UNKNOWN) {
        // Running unpartitioned, straight from 0x10000000, which is what a
        // plain `picotool load` produces. Switching is meaningless here.
        printf("appswitch: not running from a slot, staying put\r\n");
        return;
    }

    uint32_t target = (slot == APPSWITCH_SLOT_A) ? SLOT_B_ADDR : SLOT_A_ADDR;
    if (!slot_looks_populated(target)) {
        printf("appswitch: slot %c is empty, staying put\r\n",
               (slot == APPSWITCH_SLOT_A) ? 'B' : 'A');
        return;
    }

    printf("appswitch: %c -> %c, rebooting\r\n",
           (slot == APPSWITCH_SLOT_A) ? 'A' : 'B',
           (slot == APPSWITCH_SLOT_A) ? 'B' : 'A');

    // Flash-update reboot: hand the bootrom the target image's address and let
    // it boot that instead. p0 is the address, p1 unused. 10ms of delay gives
    // the printf above a chance to reach the host before the world stops.
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
               10, target, 0);

    // Only reached if the bootrom refused.
    printf("appswitch: reboot refused\r\n");
}
