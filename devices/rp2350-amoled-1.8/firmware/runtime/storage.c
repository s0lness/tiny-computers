/*
 * storage: the ONE place in this firmware that ever calls flash_range_erase
 * or flash_range_program. Built to docs/decisions/0011's design, after
 * firmware/probe/rtcprobe.c's section D proved the mechanism (that probe
 * was deliberately never flashed - see its own header - so tSE/tPP below
 * are still the W25Q128JV datasheet's typical/maximum, not this puck's own
 * measured numbers; see the honesty note near the bottom of this file).
 *
 * TWO ADDRESS SPACES, NOT ONE - docs/decisions/0018. Decision 0011 sized
 * this sector against the chip's own 16MB, and that reasoning was correct
 * for a write: flash_range_erase()/flash_range_program() take an offset
 * from the start of the WHOLE CHIP. It does not hold for a READ: the
 * RP2350 bootrom maps the XIP window onto the ACTIVE PARTITION, not the
 * chip, so a read through XIP_BASE only ever sees the 7MB slot_a/slot_b is
 * declared as (store/partitions.json), never anything past it - measured
 * on hardware sweeping XIP reads with a watchdog-armed probe: reads
 * succeed to 6.75MB into the partition and stall the bus from 7.00MB up,
 * exactly slot_a's length, hard enough to lose the USB link too. Every
 * offset below is therefore named for which space it lives in:
 * PARTITION-RELATIVE ones feed an XIP read; CHIP-ABSOLUTE ones feed
 * flash_range_erase()/flash_range_program(). They differ by the active
 * partition's own base, which active_partition_base() below resolves at
 * runtime through the bootrom's partition-table API rather than assuming
 * slot_a's 0x100000 - the same image also boots from slot_b's 0x800000
 * whenever store/'s crash-recovery machinery has last chosen that slot
 * (decision 0002 section 6), and a literal here would be wrong exactly
 * then.
 *
 * THE SECTOR. PARTITION-RELATIVE offset STORAGE_SECTOR_OFFSET, the LAST
 * 4KB sector of whichever partition is active - clear of the ~110KB image
 * at the front, and (rule 6, tools/invariants/rules/rp2350-amoled-1.8.ts)
 * checked at build time against store/partitions.json to actually fit
 * inside the smallest partition this firmware could boot from. If store/'s
 * golden-image fallback is ever revived, that table must still declare
 * this sector rather than leave it squatted - decision 0011 says so and
 * rule 6 only checks that it FITS, not that it is declared.
 *
 * A SLOT SWITCH MAKES THIS PER-SLOT, AND THAT IS NEW. Because the sector
 * is partition-relative, slot_a and slot_b each keep their OWN copy at
 * different chip-absolute addresses. Ordinary app switching never reboots
 * (decision 0002: it is a function call), so this never matters during
 * normal play. It matters only for store/'s crash-recovery reboot path: a
 * board that boots into slot_b for the first time finds its own sector
 * genuinely erased (all-0xFF) and starts a fresh dino high score, even if
 * slot_a's sector still holds an old one a few megabytes away and
 * unreachable from here. Nothing in this file carries a value across that
 * boundary; if that is ever undesirable, it is store/'s problem to solve,
 * not this file's to paper over.
 *
 * THE RECORD. 16 bytes, append-only:
 *
 *   offset  0  uint16  magic     (STORAGE_MAGIC; erased flash reads 0xFFFF,
 *                                 which this can never equal, so an
 *                                 unwritten slot is unambiguous)
 *   offset  2  uint8   kind      (STORAGE_KIND_* - storage.h)
 *   offset  3  uint8   reserved  (0, for a field this format may want later)
 *   offset  4  uint32  value
 *   offset  8  uint32  crc32     (over bytes 0..7)
 *   offset 12  uint32  reserved  (0xFFFFFFFF - stays "erased" until this
 *                                 format actually needs it, so an old
 *                                 image reading a newer record's padding
 *                                 sees exactly what an unwritten byte
 *                                 looks like)
 *
 * 256 of these fit a 4096-byte sector, 16 to a 256-byte flash page (a page
 * is the smallest unit flash_range_program() will write - see below for
 * why that, not 16 bytes, is what one save actually costs).
 *
 * THE SCAN, at boot, on core0, once (storage_init()). Walk slot 0 upward.
 * The first slot that reads back all-0xFF (erased, never written) is the
 * append point. The first slot whose magic does not match, or whose CRC
 * fails, WITHOUT being all-0xFF, is a torn write: everything before it is
 * trusted (each valid record updates this boot's cached value for its own
 * kind), and the scan stops there, refusing to program into that slot -
 * flash can only clear bits by erasing a whole sector, so a slot a torn
 * write left in an unknown state must never be written into again before
 * an erase. A power cut mid-program therefore costs at most the record
 * being written, per decision 0011; every earlier record is still there
 * and still valid.
 *
 * THE SAVE (storage_save_u32()). flash_range_program() only ever writes a
 * whole 256-byte page (hardware/flash.h: "count Must be a multiple of
 * 256"), so even though a record is 16 bytes, appending one means reading
 * the page it lands in back through the XIP window (safe: this only runs
 * on core0, which is also the core that executes this very function's own
 * instructions by XIP - storage.c is core0-only, so it is not part of the
 * RAM-pinned set tools/invariants rule 1 enforces for core1, decisions
 * 0004/0005/0016 - so an ordinary memory-mapped read through XIP_BASE here
 * is the SAME kind of flash access as fetching this function's own next
 * instruction, not the CS-deselecting borrow bootbtn.c's read_cs_low() does;
 * nothing about it resembles that hazard), patching in the new 16 bytes,
 * and programming the whole page back. That is one page program per save
 * (0.4ms typical, 3ms max -
 * decision 0011's own table), not sixteen times less the way the record
 * size alone would suggest. The bytes for every OTHER slot already in that
 * page are read back unchanged and written back identical, which programs
 * no new bit low for them - decision 0011's own claim that "a power cut
 * mid-program corrupts at most the record being written" depends on
 * exactly this (see the honesty note below).
 *
 * THE ERASE. Only when the sector is genuinely full (all 256 slots
 * written), which is once every 256 saves. A child would have to beat her
 * own high score 256 times to pay for one 45ms-typical, 400ms-worst-case
 * hiccup, and the part's rated endurance (100K erase cycles/sector, per
 * the datasheet) makes that irrelevant on any timescale that matters here.
 *
 * WHY THIS IS SAFE AGAINST CORE1. Both the erase and the program run
 * inside flash_safe_execute(), which - now that sensors.c's core1_entry()
 * calls flash_safe_execute_core_init() once, per decision 0011 - parks
 * core1 behind pico_multicore's lockout IRQ for the duration and disables
 * interrupts on core0 for the same span. Core1 is parked for at most
 * ~400ms (the erase, worst case), against CORE1_STALL_MS's 1500ms
 * liveness guard (runtime.c) - so a save, even the rare one that erases
 * first, never trips the guard that forcibly resets core1. See sensors.c's
 * core1_entry() for the one added line.
 *
 * WHAT THIS FILE DELIBERATELY DOES NOT DO, per decision 0011: no
 * key-value API, no wear levelling, no generic "settings" concept. One
 * kind exists today (STORAGE_KIND_DINO_HISCORE). A second kind sharing
 * this sector is architecturally fine (the kind byte exists for exactly
 * that), but the erase above is SECTOR-WIDE: if the sector fills on
 * account of one kind's saves, every OTHER kind's durable copy is
 * discarded by that same erase (this boot's cached RAM value survives; the
 * flash copy does not, until that kind is saved again). Harmless while
 * only one kind exists. The moment a second kind is added, decide then
 * whether it still shares this sector or needs its own - do not carry this
 * shortcut forward without noticing it.
 *
 * THE HONESTY NOTE, same spirit as sensors_inject_key()'s in sensors.h:
 * this file's "previous record is still valid after a torn write"
 * argument rests on an assumption about the W25Q128JV that is not written
 * down in a datasheet page: that re-programming a byte with the SAME value
 * it already holds is a genuine no-op at the cell level, so an interrupted
 * page program can only leave the record actively being written in an
 * unknown state, never a record that already matched what was sent. That
 * is standard NOR-flash behaviour (partial-page programming exists
 * precisely because this holds) and it is the assumption decision 0011's
 * own cost table is already built on, not a new one introduced here - but
 * it has not been measured on THIS part, the way firmware/probe/rtcprobe.c
 * measured tSE/tPP. Only a real torn-write-mid-save on real hardware would
 * settle it, and nobody should go looking for that on a board a child
 * uses.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/error.h"
#include "pico/flash.h"
#include "pico/bootrom.h"  // rom_get_boot_info(), rom_get_partition_table_info() -
                            // active_partition_base() below. Already linked:
                            // hardware_flash pulls in pico_bootrom (its own
                            // CMakeLists.txt), so no new target_link_libraries
                            // entry was needed in firmware/CMakeLists.txt.
#include "boot/picobin.h"  // PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_* -
                            // pulled in transitively by pico_bootrom's own
                            // link to boot_picobin_headers.

#define STORAGE_RECORD_SIZE       16u
#define STORAGE_RECORDS_PER_PAGE  (FLASH_PAGE_SIZE / STORAGE_RECORD_SIZE)  // 16
#define STORAGE_TOTAL_RECORDS     (FLASH_SECTOR_SIZE / STORAGE_RECORD_SIZE) // 256
#define STORAGE_MAGIC             0xC0DEu // arbitrary, non-0x0000, non-0xFFFF (erased)

// PARTITION-RELATIVE (see "TWO ADDRESS SPACES" above) - this puck's
// slot_a/slot_b are each 7MiB (store/partitions.json), and
// STORAGE_PARTITION_BYTES is that same number restated here as one plain
// numeric #define (not an expression, not derived from
// PICO_FLASH_SIZE_BYTES - the chip's 16MB is real, per
// firmware/CMakeLists.txt's own comment, but it is simply the wrong number
// for this: the XIP window this offset is read through never sees more
// than the active PARTITION), so
// tools/invariants/rules/rp2350-amoled-1.8.ts's rule 6 can parse it as a
// single token and check it against store/partitions.json directly - the
// same discipline rule 5 already applies to gfx.h's PANEL_W/PANEL_H.
#define STORAGE_PARTITION_BYTES 7340032u   // 7 * 1024 * 1024
#define STORAGE_SECTOR_OFFSET   (STORAGE_PARTITION_BYTES - FLASH_SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  kind;
    uint8_t  reserved0;
    uint32_t value;
    uint32_t crc;
    uint32_t reserved1;
} storage_record_t;

_Static_assert(sizeof(storage_record_t) == STORAGE_RECORD_SIZE,
               "storage_record_t must be exactly 16 bytes");

// Plain CRC-32 (poly 0xEDB88320), bit-at-a-time: storage_save_u32() runs a
// handful of times per session at most, so a 256-entry lookup table would
// cost more in .rodata than the loop ever costs in cycles.
static uint32_t crc32_bytes(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static bool record_all_ff(const storage_record_t *r) {
    const uint8_t *raw = (const uint8_t *)r;
    for (size_t b = 0; b < STORAGE_RECORD_SIZE; b++) {
        if (raw[b] != 0xFFu) return false;
    }
    return true;
}

static bool record_valid(const storage_record_t *r) {
    if (r->magic != STORAGE_MAGIC) return false;
    return crc32_bytes((const uint8_t *)r, 8) == r->crc;
}

// Cached per-kind values, populated once by storage_init() and updated by
// every successful storage_save_u32(). These two functions are the ONLY
// places this firmware ever reads this sector: storage_init() (once, at
// boot, core0, before core1 exists) and storage_save_u32()'s own
// page-patch step (core0 only, and only while core1 is locked out by the
// very flash_safe_execute() call about to program over it). Sized for
// comfortably more kinds than exist today; a second kind adds a second
// cache slot the same way, not a data structure.
// STORAGE_MAX_KINDS raised 4 -> 6 -> 10. A sector written by any past build
// can hold DINO_HISCORE (1), the retired TABLES_CALIB (2), the retired
// TABLES_CALIB_LO/_HI (3, 4) and the live TABLES_CALIB10_LO/_MID/_HI (5, 6,
// 7): SEVEN distinct kinds could appear in one sector's scan, and a retired
// kind still costs a cache slot because the scan meets its records whether
// or not anything ever asks for them again. This is a fixed-size RAM array
// sized "comfortably more kinds than exist today" per its own original
// comment - 10 keeps that true (70 bytes of RAM) rather than trimming the
// margin to exactly what is used right now.
#define STORAGE_MAX_KINDS 10
static uint8_t  s_cacheKind[STORAGE_MAX_KINDS];
static uint32_t s_cacheValue[STORAGE_MAX_KINDS];
static uint8_t  s_cacheTag[STORAGE_MAX_KINDS];   // the record's own `reserved` byte - see storage.h's storage_get/save_u32_tagged()
static bool     s_cacheHave[STORAGE_MAX_KINDS];
static int      s_cacheCount = 0;

static int cache_slot_for(uint8_t kind, bool create) {
    for (int i = 0; i < s_cacheCount; i++) {
        if (s_cacheKind[i] == kind) return i;
    }
    if (!create || s_cacheCount >= STORAGE_MAX_KINDS) return -1;
    int i = s_cacheCount++;
    s_cacheKind[i] = kind;
    s_cacheHave[i] = false;
    s_cacheValue[i] = 0;
    s_cacheTag[i] = 0;
    return i;
}

// -2: storage_init() has not run yet (storage_save_u32() refuses outright
// rather than guess at the sector's real state). -1: the sector needs an
// erase before the next write (either genuinely full, or a torn write was
// found and nothing past it may ever be trusted). 0..STORAGE_TOTAL_RECORDS-1:
// the next free slot.
static int s_nextFreeSlot = -2;

// CHIP-ABSOLUTE (see "TWO ADDRESS SPACES" above): active partition's own
// base plus STORAGE_SECTOR_OFFSET, resolved once by storage_init() and fed
// to flash_range_erase()/flash_range_program() below. The XIP read a few
// lines down needs none of this - it stays partition-relative on purpose.
static uint32_t s_absoluteSectorOffset = 0;

// THE READ OFFSET, kept as its own variable rather than reusing
// STORAGE_SECTOR_OFFSET, because the two address spaces decision 0018 is
// about do not agree in both of the ways this board can boot:
//
//   PARTITIONED. The XIP window IS the active partition, so an XIP read is
//   partition-relative (STORAGE_SECTOR_OFFSET) while flash_range_erase/
//   program take chip-absolute offsets (partition base + that). Two
//   different numbers for one sector - the whole of decision 0018.
//
//   UNPARTITIONED, which is how this puck actually boots today: there is no
//   partition table (confirmed on silicon, `picotool partition info`), the
//   image is linked at 0x10000000, and the XIP window maps the whole chip.
//   Read and write then use the SAME chip-absolute number.
//
// Writing that as one constant plus a conditional base was what produced the
// white-screen freeze the first time. Two named variables, each set once,
// cannot drift apart the same way.
static uint32_t s_xipReadOffset = STORAGE_SECTOR_OFFSET;

// Where storage lives when no partition table exists. The LAST sector of the
// 16MB chip: past slot_b's end (0x800000..0xF00000) and past the manifest
// (0xF00000), so it is in unpartitioned space under store/partitions.json
// and cannot collide with any partition if one is ever installed. It is also
// nowhere near the linked image, which ends around 0x1002a9f0 (~171KB).
#define STORAGE_UNPARTITIONED_SECTOR (0x1000000u - FLASH_SECTOR_SIZE) // 0xFFF000

// Resolves the CHIP-ABSOLUTE base of the partition THIS BOOT is actually
// running from, via two bootrom ROM calls (pico/bootrom.h) rather than a
// hardcoded 0x100000 (slot_a) or 0x800000 (slot_b, store/partitions.json):
// the same image boots from either, depending on which one store/'s
// crash-recovery machinery last chose (decision 0002 section 6), so a
// literal here would be wrong exactly half the time. Both calls are
// already-shipped pico-sdk API, not new machinery - this is the identical
// shape pico_cyw43_driver's cyw43_driver.c uses at runtime to locate its
// own firmware partition:
//
//   1. rom_get_boot_info() - which partition index did THIS boot come
//      from (0 = slot_a, 1 = slot_b, matching store/partitions.json's own
//      "id" field)? A negative index ("not applicable" - a RAM or debug
//      boot) is treated as failure, never as "assume slot_a".
//   2. rom_get_partition_table_info(..., PT_INFO_PARTITION_LOCATION_AND_
//      FLAGS | PT_INFO_SINGLE_PARTITION | (partition << 24)) - that one
//      partition's own location, packed as first/last SECTOR NUMBERS
//      (PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS/_LSB). Multiplying
//      the first-sector number by FLASH_SECTOR_SIZE is exactly the byte
//      offset flash_range_erase()/flash_range_program() want.
//
// UNVERIFIED ON SILICON, same honesty this file's header already keeps for
// tSE/tPP: this is read from the bootrom header and pico-sdk's own caller
// of the identical two calls, not observed on this puck. See
// docs/decisions/0018's own account of what could not be established.
static bool active_partition_base(uint32_t *outBase) {
    boot_info_t info;
    if (!rom_get_boot_info(&info)) return false;
    if (info.partition < 0) return false; // not booted from a partition - refuse rather than guess

    uint32_t buffer[3];
    uint32_t query = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                      ((uint32_t)info.partition << 24);
    int words = rom_get_partition_table_info(buffer, count_of(buffer), query);
    if (words != 3) return false;
    if (buffer[0] != (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION)) return false;

    uint32_t firstSector = (buffer[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                            PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    *outBase = firstSector * FLASH_SECTOR_SIZE;
    return true;
}

void storage_init(void) {
    s_cacheCount = 0;
    s_nextFreeSlot = -1; // sector full/unwritable until a genuinely erased slot is found

    uint32_t partitionBase;
    if (!active_partition_base(&partitionBase)) {
        // Could not learn which partition this boot is running from -
        // refuse rather than guess a chip-absolute address. Reuses the
        // same "-2" state storage_save_u32() already reads as "storage_
        // init() never ran": from a caller's point of view the two are
        // indistinguishable - storage simply is not available this boot.
        //
        // PRINTED, NOT SILENT, since 2026-08-19 - this is the one failure
        // mode a caller cannot tell apart from "genuinely never saved
        // anything" without this line. tables.c's own investigation (see
        // its NUMPAD TOUCH CALIBRATION section and AGENTS.md's flash notes)
        // found a calibration that had been saved and then, after an
        // ordinary firmware reflash, read back as "no stored calibration" -
        // consistent with the reflash overwriting whatever partition table
        // this boot needed rom_get_boot_info() to see, which makes THIS
        // branch fire, which disables storage for the whole boot, silently,
        // for every kind at once. A caller reading storage_get_u32() return
        // false has no way to distinguish "nothing was ever saved" from
        // "nothing can be read this boot" without this line existing
        // somewhere; this is that somewhere, printed once, at the only
        // moment storage.c itself knows which case it is.
        // NO LONGER FATAL, since 2026-08-19. Refusing here was the right
        // instinct - guessing a chip-absolute address while actually booted
        // from a partition is precisely the white-screen freeze - but it
        // turned out to be the NORMAL case on this puck, not the exotic one:
        // there is no partition table, because the image links at 0x10000000
        // and every ordinary reflash overwrites whatever table sat there (see
        // AGENTS.md's flash notes). So this branch fired on every boot and
        // took all of storage with it, silently, until the line above was
        // added and the owner lost a calibration to it.
        //
        // Unpartitioned is not a guess: it is a KNOWN layout. The image is at
        // the chip's start and the XIP window maps the whole chip, so read and
        // write share one chip-absolute offset, and there is no second address
        // space to get wrong. What made the freeze possible was a partition
        // whose base was not zero while the code assumed it was; here there is
        // no partition at all.
        printf("storage: no partition table this boot - using the chip's last "
               "sector at 0x%08lx (unpartitioned layout)\r\n",
               (unsigned long)STORAGE_UNPARTITIONED_SECTOR);
        s_absoluteSectorOffset = STORAGE_UNPARTITIONED_SECTOR;
        s_xipReadOffset        = STORAGE_UNPARTITIONED_SECTOR;
    } else {
        s_absoluteSectorOffset = partitionBase + STORAGE_SECTOR_OFFSET;
        s_xipReadOffset        = STORAGE_SECTOR_OFFSET;
    }

    const uint8_t *base = (const uint8_t *)(XIP_BASE + s_xipReadOffset);
    for (int i = 0; i < STORAGE_TOTAL_RECORDS; i++) {
        storage_record_t rec;
        memcpy(&rec, base + (size_t)i * STORAGE_RECORD_SIZE, STORAGE_RECORD_SIZE);

        if (record_all_ff(&rec)) {
            s_nextFreeSlot = i;
            break;
        }
        if (!record_valid(&rec)) {
            // A torn write, or anything else that fails to parse: stop
            // trusting the sector from here (see this file's header - never
            // program into a slot flash can only re-clear via a full erase).
            s_nextFreeSlot = -1;
            break;
        }

        int slot = cache_slot_for(rec.kind, true);
        if (slot >= 0) {
            s_cacheValue[slot] = rec.value;
            s_cacheTag[slot] = rec.reserved0;
            s_cacheHave[slot] = true;
        }
        // Ran off the end with every slot valid: the sector is genuinely
        // full. s_nextFreeSlot stays -1, which storage_save_u32() reads as
        // "erase before the next write" - exactly the same action as the
        // torn-write case, for a different, ordinary reason.
    }
}

bool storage_get_u32(uint8_t kind, uint32_t *outValue) {
    uint8_t tag;
    return storage_get_u32_tagged(kind, outValue, &tag);
}

bool storage_get_u32_tagged(uint8_t kind, uint32_t *outValue, uint8_t *outTag) {
    int slot = cache_slot_for(kind, false);
    if (slot < 0 || !s_cacheHave[slot]) return false;
    *outValue = s_cacheValue[slot];
    *outTag = s_cacheTag[slot];
    return true;
}

static void do_erase(void *param) {
    (void)param;
    flash_range_erase(s_absoluteSectorOffset, FLASH_SECTOR_SIZE);
}

typedef struct {
    uint32_t pageOffset; // byte offset within the sector, page-aligned
    const uint8_t *data; // FLASH_PAGE_SIZE bytes
} program_page_args_t;

static void do_program(void *param) {
    program_page_args_t *a = (program_page_args_t *)param;
    flash_range_program(s_absoluteSectorOffset + a->pageOffset, a->data, FLASH_PAGE_SIZE);
}

// 1500, not the probe's 1000: this bounds "core1 never answered the
// lockout handshake" (flash_safe_execute()'s own timeout governs the
// LOCKOUT, not the flash operation), and matching CORE1_STALL_MS
// (runtime.c) means a call that is genuinely stuck is caught by this
// timeout at the same moment the liveness guard would otherwise notice,
// rather than one silently outlasting the other.
#define STORAGE_LOCKOUT_TIMEOUT_MS 1500u

void storage_save_u32(uint8_t kind, uint32_t value) {
    storage_save_u32_tagged(kind, value, 0);
}

void storage_save_u32_tagged(uint8_t kind, uint32_t value, uint8_t tag) {
    if (s_nextFreeSlot == -2) return; // storage_init() never ran - refuse
                                       // rather than guess at the sector.

    if (s_nextFreeSlot < 0 || s_nextFreeSlot >= (int)STORAGE_TOTAL_RECORDS) {
        int rc = flash_safe_execute(do_erase, NULL, STORAGE_LOCKOUT_TIMEOUT_MS);
        if (rc != PICO_OK) return; // could not lock core1 out - touch nothing
        s_nextFreeSlot = 0;
    }

    storage_record_t rec;
    rec.magic = (uint16_t)STORAGE_MAGIC;
    rec.kind = kind;
    rec.reserved0 = tag;
    rec.value = value;
    rec.crc = crc32_bytes((const uint8_t *)&rec, 8);
    rec.reserved1 = 0xFFFFFFFFu;

    int slotInSector = s_nextFreeSlot;
    uint32_t pageIndex = (uint32_t)slotInSector / STORAGE_RECORDS_PER_PAGE;
    uint32_t slotInPage = (uint32_t)slotInSector % STORAGE_RECORDS_PER_PAGE;
    uint32_t pageOffset = pageIndex * FLASH_PAGE_SIZE;

    uint8_t page[FLASH_PAGE_SIZE];
    const uint8_t *pageFlash = (const uint8_t *)(XIP_BASE + s_xipReadOffset + pageOffset);
    memcpy(page, pageFlash, FLASH_PAGE_SIZE);
    memcpy(page + slotInPage * STORAGE_RECORD_SIZE, &rec, STORAGE_RECORD_SIZE);

    program_page_args_t args = { pageOffset, page };
    int rc = flash_safe_execute(do_program, &args, STORAGE_LOCKOUT_TIMEOUT_MS);
    if (rc != PICO_OK) return; // the cache below must not move on a write
                                // that may not have happened

    int slot = cache_slot_for(kind, true);
    s_cacheValue[slot] = value;
    s_cacheTag[slot] = tag;
    s_cacheHave[slot] = true;
    s_nextFreeSlot = slotInSector + 1;
}
