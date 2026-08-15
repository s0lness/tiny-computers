/*
 * storage: the ONE place in this firmware that ever calls flash_range_erase
 * or flash_range_program. Built to docs/decisions/0011's design, after
 * firmware/probe/rtcprobe.c's section D proved the mechanism (that probe
 * was deliberately never flashed - see its own header - so tSE/tPP below
 * are still the W25Q128JV datasheet's typical/maximum, not this puck's own
 * measured numbers; see the honesty note near the bottom of this file).
 *
 * THE SECTOR. Offset 0x00FFF000, the LAST 4KB sector of the 16MB part -
 * 15.9MB clear of the ~110KB image, and above every region
 * store/partitions.json has ever claimed (decision 0011). If store/'s
 * golden-image fallback is ever revived, that table must declare this
 * sector rather than leave it squatted - decision 0011 says so and nothing
 * here enforces it.
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

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/error.h"
#include "pico/flash.h"

#define STORAGE_RECORD_SIZE       16u
#define STORAGE_RECORDS_PER_PAGE  (FLASH_PAGE_SIZE / STORAGE_RECORD_SIZE)  // 16
#define STORAGE_TOTAL_RECORDS     (FLASH_SECTOR_SIZE / STORAGE_RECORD_SIZE) // 256
#define STORAGE_MAGIC             0xC0DEu // arbitrary, non-0x0000, non-0xFFFF (erased)

// Literal, not derived from PICO_FLASH_SIZE_BYTES: the pico2 board file
// this build uses defaults that to 4MB (the Raspberry Pi Pico 2's own
// flash, not this Waveshare board's actual 16MB W25Q128JVSIQ), and
// firmware/CMakeLists.txt does not override it. firmware/probe/rtcprobe.c's
// PROBE_FLASH_OFFSET is the same literal for the same reason. See
// docs/decisions/0011.
#define STORAGE_FLASH_OFFSET (16u * 1024u * 1024u - FLASH_SECTOR_SIZE)

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
#define STORAGE_MAX_KINDS 4
static uint8_t  s_cacheKind[STORAGE_MAX_KINDS];
static uint32_t s_cacheValue[STORAGE_MAX_KINDS];
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
    return i;
}

// -2: storage_init() has not run yet (storage_save_u32() refuses outright
// rather than guess at the sector's real state). -1: the sector needs an
// erase before the next write (either genuinely full, or a torn write was
// found and nothing past it may ever be trusted). 0..STORAGE_TOTAL_RECORDS-1:
// the next free slot.
static int s_nextFreeSlot = -2;

void storage_init(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORAGE_FLASH_OFFSET);
    s_cacheCount = 0;
    s_nextFreeSlot = -1; // sector full/unwritable until a genuinely erased slot is found
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
            s_cacheHave[slot] = true;
        }
        // Ran off the end with every slot valid: the sector is genuinely
        // full. s_nextFreeSlot stays -1, which storage_save_u32() reads as
        // "erase before the next write" - exactly the same action as the
        // torn-write case, for a different, ordinary reason.
    }
}

bool storage_get_u32(uint8_t kind, uint32_t *outValue) {
    int slot = cache_slot_for(kind, false);
    if (slot < 0 || !s_cacheHave[slot]) return false;
    *outValue = s_cacheValue[slot];
    return true;
}

static void do_erase(void *param) {
    (void)param;
    flash_range_erase(STORAGE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

typedef struct {
    uint32_t pageOffset; // byte offset within the sector, page-aligned
    const uint8_t *data; // FLASH_PAGE_SIZE bytes
} program_page_args_t;

static void do_program(void *param) {
    program_page_args_t *a = (program_page_args_t *)param;
    flash_range_program(STORAGE_FLASH_OFFSET + a->pageOffset, a->data, FLASH_PAGE_SIZE);
}

// 1500, not the probe's 1000: this bounds "core1 never answered the
// lockout handshake" (flash_safe_execute()'s own timeout governs the
// LOCKOUT, not the flash operation), and matching CORE1_STALL_MS
// (runtime.c) means a call that is genuinely stuck is caught by this
// timeout at the same moment the liveness guard would otherwise notice,
// rather than one silently outlasting the other.
#define STORAGE_LOCKOUT_TIMEOUT_MS 1500u

void storage_save_u32(uint8_t kind, uint32_t value) {
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
    rec.reserved0 = 0;
    rec.value = value;
    rec.crc = crc32_bytes((const uint8_t *)&rec, 8);
    rec.reserved1 = 0xFFFFFFFFu;

    int slotInSector = s_nextFreeSlot;
    uint32_t pageIndex = (uint32_t)slotInSector / STORAGE_RECORDS_PER_PAGE;
    uint32_t slotInPage = (uint32_t)slotInSector % STORAGE_RECORDS_PER_PAGE;
    uint32_t pageOffset = pageIndex * FLASH_PAGE_SIZE;

    uint8_t page[FLASH_PAGE_SIZE];
    const uint8_t *pageFlash = (const uint8_t *)(XIP_BASE + STORAGE_FLASH_OFFSET + pageOffset);
    memcpy(page, pageFlash, FLASH_PAGE_SIZE);
    memcpy(page + slotInPage * STORAGE_RECORD_SIZE, &rec, STORAGE_RECORD_SIZE);

    program_page_args_t args = { pageOffset, page };
    int rc = flash_safe_execute(do_program, &args, STORAGE_LOCKOUT_TIMEOUT_MS);
    if (rc != PICO_OK) return; // the cache below must not move on a write
                                // that may not have happened

    int slot = cache_slot_for(kind, true);
    s_cacheValue[slot] = value;
    s_cacheHave[slot] = true;
    s_nextFreeSlot = slotInSector + 1;
}
