// store/bootloader/main.c
//
// First-stage bootloader for the two-app-slot layout in
// ../partitions.json. Lives in the unpartitioned space below 0x00100000 (a
// plain, normally-linked pico2 binary at the SDK default 0x10000000 — see
// CMakeLists.txt's pico_override_flash_size call for the guarantee that it
// stays there). Its whole job: pick a slot, chain_image() into it, and never
// brick the device if that fails.
//
// WHY THIS EXISTS, instead of the running app just rebooting straight into
// the other slot (see ../README.md, "Switching apps", and
// ../../firmware/appswitch.c's own header comment for the fuller version):
// the RP2350 datasheet, section 5.1.16, says flash-update reboots have no
// effect on standalone (non A/B) partitions, which slot_a/slot_b
// deliberately are. Linking them as an A/B pair to make flash-update reboots
// work is not the fix either — that boot type erases the first sector of
// "the other image" on every such boot, i.e. exactly the slot being switched
// away from. chain_image() (datasheet section 5.10.6) is the documented
// mechanism for "run this other, unrelated image without touching it", and
// it is meant to be called from a bootloader, not from the app currently
// running (chain_image() is how you get TO an image; it cannot be what an
// image uses to hand off to a sibling while remaining resident itself).
//
// HOW THE TWO ENDS TALK: ../../firmware/bootreq.h. The running app calls
// appswitch_go_other(), which writes a magic+slot word into a watchdog
// scratch register and does a plain watchdog_reboot(0, 0, 0). This
// bootloader reads that register, clears it — so an unrelated crash-reboot
// later can't replay a stale request, and so a real power cycle (which
// resets the register anyway) always lands on the default slot — and chains
// into the requested slot. See bootreq.h for exactly which scratch register
// and why it's safe.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "boot/bootrom_constants.h"
#include "hardware/regs/addressmap.h" // XIP_BASE

#include "bootreq.h"

// Slot -> partition id mapping, fixed by ../partitions.json (slot_a id 0,
// slot_b id 1). Only two slots exist; this is deliberately not general.
#define SLOT_A 0u
#define SLOT_B 1u
#define DEFAULT_SLOT SLOT_A

// Flash sector size in bytes: the granularity PICOBIN_PARTITION_LOCATION's
// first/last-sector fields (boot/picobin.h) are expressed in. Same value as
// the SDK's hardware/flash.h FLASH_SECTOR_SIZE; defined locally instead of
// linking hardware_flash for one constant, in keeping with "no display, no
// touch" — this bootloader's whole link graph is pico_stdlib plus
// pico_bootrom (which every executable pulls in transitively via pico_crt0
// regardless).
#define BOOT_FLASH_SECTOR_SIZE 4096u

// rom_load_partition_table() / rom_chain_image() / rom_pick_ab_partition()
// all document the same requirement in the current pico-sdk checkout's
// pico/bootrom.h: "The work area size currently required is 3264, so 3.25K
// is a good choice." The RP2350 datasheet's own bootloader example (section
// 5.10.6) uses 0xc00 (3072 bytes) for this same buffer — SMALLER than what
// this SDK build actually requires, by 192 bytes. This is a real
// discrepancy between the two documents, not a typo on one side that's
// obviously right; since this code calls the SDK's own wrapper functions
// (not the datasheet's pseudocode directly), the SDK header's number is the
// one that governs what BOOTROM_ERROR_INSUFFICIENT_RESOURCES actually checks
// against, so it wins here. Word-aligned; static so it costs .bss, not
// bootloader stack.
#define WORKAREA_SIZE 3264u
static uint8_t s_workarea[WORKAREA_SIZE] __attribute__((aligned(4)));

// Tries to chain into `slot`. Only returns on failure — a successful
// chain_image() does not return at all. Prints why it failed.
static bool try_chain(uint8_t slot) {
    uint32_t partition_info[3];
    int words = rom_get_partition_table_info(
        partition_info, count_of(partition_info),
        PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | ((uint32_t)slot << 24));

    uint32_t want_flags = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION;
    if (words != 3 || (partition_info[0] & want_flags) != want_flags) {
        printf("bootloader: get_partition_table_info(slot %u) failed, rc=%d\r\n",
               (unsigned)slot, words);
        return false;
    }

    uint32_t location = partition_info[1];
    uint16_t first_sector = (uint16_t)((location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS)
                                        >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB);
    uint16_t last_sector = (uint16_t)((location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS)
                                       >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB);
    uint32_t data_start_addr = (uint32_t)first_sector * BOOT_FLASH_SECTOR_SIZE;
    uint32_t data_end_addr = (uint32_t)(last_sector + 1) * BOOT_FLASH_SECTOR_SIZE;
    uint32_t data_size = data_end_addr - data_start_addr;

    // region_base passed to chain_image() is an XIP ADDRESS (XIP_BASE + flash
    // offset), not a bare flash offset. Getting this wrong is exactly the
    // class of mistake documented in appswitch.c's history (the earlier
    // flash-update attempt passed an XIP address where the bootrom wanted a
    // flash offset, the mirror-image bug of the one this line guards
    // against).
    printf("bootloader: chaining slot %u, flash 0x%08x..0x%08x (xip base 0x%08x, size %u)\r\n",
           (unsigned)slot, (unsigned)data_start_addr, (unsigned)data_end_addr,
           (unsigned)(XIP_BASE + data_start_addr), (unsigned)data_size);

    int rc = rom_chain_image(s_workarea, WORKAREA_SIZE, XIP_BASE + data_start_addr, data_size);

    // Only reached if chain_image() refused to launch the image.
    printf("bootloader: chain_image(slot %u) failed, rc=%d\r\n", (unsigned)slot, rc);
    return false;
}

int main(void) {
    // USB CDC stdio, for the fallback/error path only. Cost on the normal,
    // fast path (decide a slot, chain into it, gone in well under a
    // millisecond of our own code, every boot that isn't actively failing):
    // effectively nothing. stdio_init_all() brings up the USB peripheral
    // registers, which is fast, and does NOT block waiting for a host to
    // enumerate — PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS defaults to 0 — so
    // this adds no measurable boot delay. It only matters on the rare
    // failure path below, where a short sleep_ms gives a host that's already
    // watching a chance to actually see the printf before BOOTSEL takes the
    // USB peripheral away.
    stdio_init_all();

    // The partition table is not guaranteed to be resident on entry. It is
    // definitely NOT resident after the watchdog reboot appswitch.c uses to
    // get here: pico/bootrom.h's own docs for rom_get_partition_table_info
    // say "If the partition table has not been loaded (e.g. from a watchdog
    // or RAM boot)... you should load the partition table via
    // load_partition_table() first." force_reload=false makes this cheap
    // when it's already resident (e.g. a cold boot straight into this
    // bootloader), so it's called unconditionally on every boot rather than
    // trying to detect which path got us here.
    int load_rc = rom_load_partition_table(s_workarea, WORKAREA_SIZE, false);
    if (load_rc < 0) {
        printf("bootloader: load_partition_table failed, rc=%d\r\n", load_rc);
        // Fall through anyway: try_chain()'s own get_partition_table_info
        // call will fail cleanly too, and we land in the same BOOTSEL
        // fallback below rather than needing a separate error path here.
    }

    uint8_t requested_slot = 0;
    uint8_t target_slot = DEFAULT_SLOT;
    if (bootreq_read_and_clear(&requested_slot) && requested_slot <= SLOT_B) {
        target_slot = requested_slot;
        printf("bootloader: switch request -> slot %u\r\n", (unsigned)target_slot);
    } else {
        printf("bootloader: no switch request, default slot %u\r\n", (unsigned)target_slot);
    }

    if (!try_chain(target_slot)) {
        if (target_slot != DEFAULT_SLOT) {
            printf("bootloader: falling back to slot %u\r\n", (unsigned)DEFAULT_SLOT);
            try_chain(DEFAULT_SLOT);
        }
    }

    // Both attempts refused, or the only attempt (already the default slot)
    // did. Never brick: sit in BOOTSEL so picotool can still reach the
    // device over USB no matter what is or isn't in either slot.
    printf("bootloader: no bootable image in either slot, dropping into BOOTSEL\r\n");
    sleep_ms(50); // let the line above actually leave the USB endpoint

    reset_usb_boot(0, 0); // noreturn: no activity-LED pin, both USB interfaces enabled

    while (true) { } // unreachable; kept so main() has a defined path out
}
