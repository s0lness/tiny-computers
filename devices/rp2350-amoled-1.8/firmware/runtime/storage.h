/*
 * storage: a few bytes that survive a power cycle - built exactly to
 * docs/decisions/0011's design ("A few bytes that survive a power cycle,
 * and the reason is decision 0004") and nothing more than it specifies.
 *
 * ONE runtime-owned service. Apps never touch flash: they call
 * storage_get_u32()/storage_save_u32() below, which is the API surface
 * decision 0011 insists on so that eight future apps improvising their own
 * flash_range_program() calls never becomes a real possibility.
 *
 * Portable, like sensors.h: this header compiles into both the board and
 * emu.wasm, so apps and runtime_core.c may include it without knowing
 * which they are running on. What differs is the implementation -
 * firmware/runtime/storage.c (real flash, board only) vs
 * emulator/wasm/emu_shim.c's in-RAM stand-in - exactly the split sensors.h
 * already has between sensors.c and emu_shim.c's sensor stand-ins.
 *
 * THE HONESTY REQUIREMENT, same shape as sensors_inject_key()'s comment in
 * sensors.h: the emulator's storage never touches a flash chip, never calls
 * flash_safe_execute(), never parks a second core, and its value resets
 * every time the wasm module is (re)loaded. It proves an app's OWN logic
 * (when it decides to save, what it saves) is correct. It proves nothing
 * about whether the write survives a real power cycle, a real sector
 * erase, or a real core1 lockout - only a flashed board answers that. See
 * docs/decisions/0011 and storage.c's own header for the board side.
 *
 * WHAT THIS IS NOT: a key-value store, a settings API, or wear-levelling.
 * One shared sector, one 16-byte record shape, a handful of named kinds
 * below. Decision 0011 is explicit that the moment this becomes a
 * subsystem, someone writes to it from an app, on a timer, in a loop - so
 * it stays exactly this small on purpose.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

// One kind so far. A future second value (the clock's offset, per decision
// 0011) gets a new number here, appended - never reuse a retired one,
// since a stale record carrying an old kind byte would otherwise be
// misread by whatever claims that number next.
#define STORAGE_KIND_DINO_HISCORE 1

// RETIRED, 2026-08-19 - never reused (this header's own rule, two
// paragraphs up in the original: "never reuse a retired one, since a stale
// record carrying an old kind byte would otherwise be misread"). This kind
// used to carry a fitted `offset_y = alpha + beta*y` line, ONE uint32 for
// both coefficients (tables.c's old tables_calib_pack()/unpack()). Replaced
// by STORAGE_KIND_TABLES_CALIB_LO/_HI below: the owner asked for each of
// the numpad's nine keys to learn its own centre from ten real taps rather
// than a line fitted through three of them, and nine centres do not fit in
// one record - see tables.c's own NUMPAD TOUCH CALIBRATION section for the
// full design and the two-record torn-write argument.
#define STORAGE_KIND_TABLES_CALIB 2

// RETIRED, 2026-08-19, hours after they were added - never reused, same
// rule as kind 2 above. This PAIR carried NINE keys' learned y-offsets,
// packed 7 bits each (63 bits) across two records: LO (bits 0..31) and HI
// (bits 32..62). Replaced by STORAGE_KIND_TABLES_CALIB10_* below when the
// owner asked for the ZERO key to be calibrated too ("also we should
// calibrate 0"): ten values do not fit 64 bits, and a stale 7-bit-packed
// record must never be read back as if it were the ten-key layout - which
// is exactly what reusing these two numbers would have allowed.
#define STORAGE_KIND_TABLES_CALIB_LO 3
#define STORAGE_KIND_TABLES_CALIB_HI 4

// tables.c's per-key numpad calibration: TEN keys' learned y-offsets (the
// nine digits and the zero), packed 8 bits each (80 bits) across THREE
// records of this shared 16-byte shape - LO is keys 0..3, MID keys 4..7, HI
// keys 8..9. Eight bits rather than seven because a third record was needed
// for ten values anyway, and 8 divides 32 exactly, so no key's field
// straddles a record boundary. All three records are written with the SAME
// small tag in the `reserved` byte (storage_save_u32_tagged() below) so a
// read can tell a genuine, complete trio from one where power was lost
// partway through the three saves - including the case only three records
// can produce, a fresh LO and MID next to a STALE HI, each individually
// well-formed. See tables.c's tables_calib_pack10()/unpack10() for the bit
// layout and its STORAGE, AND THE TORN-WRITE ARGUMENT section for the
// argument this tag exists to make.
#define STORAGE_KIND_TABLES_CALIB10_LO  5
#define STORAGE_KIND_TABLES_CALIB10_MID 6
#define STORAGE_KIND_TABLES_CALIB10_HI  7

// Core0, once, at boot, before sensors_start() - see storage.c's own
// comment for why. On the board this scans the sector; in the emulator it
// just resets the in-RAM stand-in (emu_shim.c).
void storage_init(void);

// A cached read: no flash access, safe to call every tick. Returns false
// (leaving *outValue untouched) if nothing has ever been saved for this
// kind this boot.
bool storage_get_u32(uint8_t kind, uint32_t *outValue);

// Appends one record. On the board this costs one page program almost
// always (~0.4ms typical, 3ms worst case) and, once every 256 saves, an
// additional sector erase first (~45ms typical, 400ms worst case) - see
// storage.c. Call only on the event that made the value worth keeping -
// never per frame, never in a loop, never speculatively. Core0 only.
void storage_save_u32(uint8_t kind, uint32_t value);

// Same as storage_get_u32()/storage_save_u32() above, plus the record's own
// `reserved` byte (offset 3, storage.c's own record layout - already
// covered by the CRC, so this changes no on-flash shape, only what value
// goes in a byte that used to always be 0) as a small caller-defined tag.
// storage_save_u32(k, v) is exactly storage_save_u32_tagged(k, v, 0); a kind
// that never needs a tag can ignore these two and use the plain pair above.
// Built for STORAGE_KIND_TABLES_CALIB10_LO/_MID/_HI's own multi-record
// torn-write check (tables.c) - a caller that appends more than one record per logical
// save and needs to tell "both landed" from "one did" reads this back and
// compares tags itself; storage.c does not know what a matching pair means
// to any particular kind.
bool storage_get_u32_tagged(uint8_t kind, uint32_t *outValue, uint8_t *outTag);
void storage_save_u32_tagged(uint8_t kind, uint32_t value, uint8_t tag);

#endif // STORAGE_H
