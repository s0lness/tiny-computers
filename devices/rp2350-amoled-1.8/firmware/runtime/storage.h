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

#endif // STORAGE_H
