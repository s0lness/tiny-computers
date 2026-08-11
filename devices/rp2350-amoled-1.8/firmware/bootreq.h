/*
 * bootreq: the tiny protocol appswitch.c and store/bootloader/main.c share to
 * hand a "boot into slot X" request across a reboot.
 *
 * The channel is a watchdog scratch register, not flash. Why that matters:
 * flash survives a power cycle, which is exactly the behaviour we do NOT
 * want here. Per store/README.md's design note, the device must come back
 * up in the default slot after a real power cycle, not wherever it was last
 * switched to. WATCHDOG_SCRATCH0..7 are documented ("Scratch register.
 * Information persists through soft reset of the chip") to survive a
 * watchdog_reboot() but NOT a power-on reset, since they live outside the
 * always-on POWMAN domain (unlike POWMAN_SCRATCH0..3, which explicitly do
 * survive low-power/power cycling and are the wrong register family for
 * exactly that reason). So this register already does the right thing on
 * both paths for free; bootreq_read_and_clear() below additionally clears it
 * on every read as defence in depth, so an unrelated crash-reboot later can
 * never replay a stale request.
 *
 * WHICH register: SCRATCH0. Checked against every scratch-register consumer
 * in this pico-sdk checkout (pico-sdk 2.x, RP2350) before picking it:
 *
 *   - hardware_watchdog/watchdog.c's watchdog_reboot()/watchdog_enable() use
 *     SCRATCH4..7 only: SCRATCH4/5/6/7 hold a magic/pc/sp/pc quadruple for a
 *     reboot-to-address, or SCRATCH4 alone holds a "watchdog_enable caused
 *     this reboot" marker. appswitch.c's call site is
 *     `watchdog_reboot(0, 0, 0)` (pc=0), which per that function's own
 *     source just sets scratch[4] = 0 and leaves scratch[0..3] and
 *     scratch[5..7] untouched.
 *   - pico_crt0's crt0.S / crt0_riscv.S write SCRATCH2..7 as a block, but
 *     only on the `PICO_NO_FLASH` + `PICO_CRT0_DEBUG_ENTRY_RESETS_VIA_BOOTROM`
 *     path (a debugger launching a RAM binary via a RAM-image boot request).
 *     Neither this bootloader nor any app in this repo is a NO_FLASH build,
 *     so that path never executes here.
 *   - appswitch_current_slot() / the bootloader's own idea of "who am I"
 *     goes through rom_get_boot_info() -> rom_get_sys_info(), a bootrom
 *     software API (SYS_INFO_BOOT_INFO), not scratch registers. It cannot
 *     collide with this scheme.
 *   - Nothing else in this repo touches WATCHDOG_SCRATCH*.
 *
 * That leaves SCRATCH0 and SCRATCH1 with zero known consumers anywhere in
 * this build. SCRATCH0 is used here; SCRATCH1 is left free.
 */
#ifndef BOOTREQ_H
#define BOOTREQ_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/watchdog.h"

#define BOOTREQ_SCRATCH_INDEX 0

// A 24-bit magic in the top of the word, the requested slot in the bottom
// byte. The magic just has to avoid colliding with 0x00000000 (the erased /
// post-power-cycle value) and with the bootrom's own watchdog_reboot
// markers listed above (0xb007c0d3 and friends, 0x6ab73121); it carries no
// other meaning.
#define BOOTREQ_MAGIC        0x00A55A5Au
#define BOOTREQ_MAGIC_SHIFT  8u
#define BOOTREQ_SLOT_MASK    0xFFu

// Called by the app that wants to switch, immediately before
// watchdog_reboot(0, 0, 0). Does not itself reboot.
static inline void bootreq_write(uint8_t slot) {
    watchdog_hw->scratch[BOOTREQ_SCRATCH_INDEX] =
        (BOOTREQ_MAGIC << BOOTREQ_MAGIC_SHIFT) | (uint32_t)(slot & BOOTREQ_SLOT_MASK);
}

// Called once by the bootloader at boot, before it decides which slot to
// chain into. Always clears the register — whether or not a valid request
// was found — so a request is consumed exactly once: a stale value can never
// re-fire on a later, unrelated reboot (e.g. a watchdog timeout inside the
// app itself), and a genuine power cycle already zeroed it anyway.
//
// Returns true and fills *slot_out only if a validly-tagged word was found.
// Does not itself validate that the slot number is in range; the caller
// (which knows how many slots exist) does that.
static inline bool bootreq_read_and_clear(uint8_t *slot_out) {
    uint32_t v = watchdog_hw->scratch[BOOTREQ_SCRATCH_INDEX];
    watchdog_hw->scratch[BOOTREQ_SCRATCH_INDEX] = 0;
    if ((v >> BOOTREQ_MAGIC_SHIFT) != BOOTREQ_MAGIC) return false;
    *slot_out = (uint8_t)(v & BOOTREQ_SLOT_MASK);
    return true;
}

#endif // BOOTREQ_H
