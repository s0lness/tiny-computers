#pragma once
#include <stdbool.h>

/* ---- the live-tune registry: one flat index/name space over every app's
 * own tunables ---------------------------------------------------------
 *
 * Started as sketch.c's own six dropout-tolerance knobs, addressed
 * directly by devlink's TUNE command and the emulator's tunables panel
 * (sensors.h's "DEVELOPMENT: sketchpad live tuning" comment has that
 * history). The owner then asked to feel out the clock's separator pulse
 * and the numpad's thumb bias the same way, which meant TWO apps under
 * devlink.c and emu_shim.c's SAME five function pointers - `sketch_tune_*`
 * stopped being an honest name for what those callers actually reach.
 *
 * This file is the fix: devlink.c's `g_hooks.tune_*` (runtime.c) and
 * emu_shim.c's `emu_tune_get`/`emu_tune_set`/`emu_tune_reset`/`emu_device`
 * now all call `tune_registry_*` instead of any one app's functions
 * directly. Neither caller needs to know how many apps declare tunables or
 * which one owns a given name - the same "declare the shape, nothing else
 * hardcodes a list" pattern `app.h`'s `g_apps[]` already uses for
 * switching, and `sketch.c`'s own `g_sketchTunables[]` already used for one
 * app's knobs.
 *
 * Each app still owns its own tunables exactly as sketch.c always has -
 * its own gate macro (`SKETCH_LIVE_TUNE`, `CLOCK_LIVE_TUNE`,
 * `TABLES_LIVE_TUNE`), its own static table, its own find-by-name helper -
 * declared in sensors.h alongside sketch's (see that header) and
 * implemented in the app's own .c file, the only file with the state to
 * report or change. This registry does not reimplement any of that; it
 * only aggregates, by walking a small fixed list of "providers" (one per
 * app) in `tune_registry.c` and dispatching a global index or a name to
 * whichever provider owns it. Adding a fourth app's tunables is one row in
 * that list; nothing here, in devlink.c, or in emu_shim.c has to change.
 *
 * All five functions read as "nothing declared" (count 0, describe/get/set
 * false) when every provider's own gate is off, the same "0 when the gate
 * is off" contract each app's own functions already promise on their own -
 * a registry over zero providers with anything enabled is still exactly
 * zero tunables, not a special case.
 */

// Total tunables declared across every registered app, summed. 0 if every
// app's own gate is off.
int tune_registry_count(void);

// Describes the tunable at global index `index` (0..tune_registry_count()-1):
// its devlink/emulator-facing name, its clamp range, and its declared
// default. Returns false for an out-of-range index.
bool tune_registry_describe(int index, const char **name, float *min, float *max, float *def);

// The #define this tunable becomes when frozen back into its owning app's
// source (see that app's own FREEZE comment). NULL for an out-of-range
// index.
const char *tune_registry_define_name(int index);

// Current value of the tunable named `name` (as returned by
// tune_registry_describe()'s `name` out-param). Returns false, leaving
// *out untouched, if no registered app declares a tunable by that name.
bool tune_registry_get(const char *name, float *out);

// Sets the tunable named `name` to `value`, clamped by its owning app to
// its declared [min, max]. *outApplied (if non-NULL) receives the value
// actually applied. Returns false, applying nothing, if no registered app
// declares a tunable by that name.
bool tune_registry_set(const char *name, float value, float *outApplied);
