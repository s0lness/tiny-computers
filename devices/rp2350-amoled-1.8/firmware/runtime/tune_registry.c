// tune_registry.c: the aggregator described in tune_registry.h. Portable
// (no pico-sdk, no hardware/*), like runtime_core.c and gfx.c, so it
// compiles for both the board and the wasm32-freestanding emulator target
// (emulator/wasm/build.ts) unmodified.
#include "tune_registry.h"
#include "sensors.h" // sketch_tune_*, clock_tune_*, tables_tune_* - see that
                      // header's "DEVELOPMENT: live tuning" section.

typedef int (*tune_count_fn)(void);
typedef bool (*tune_describe_fn)(int index, const char **name, float *min, float *max, float *def);
typedef const char *(*tune_define_name_fn)(int index);
typedef bool (*tune_get_fn)(const char *name, float *out);
typedef bool (*tune_set_fn)(const char *name, float value, float *outApplied);

typedef struct {
    tune_count_fn count;
    tune_describe_fn describe;
    tune_define_name_fn define_name;
    tune_get_fn get;
    tune_set_fn set;
} tune_provider_t;

// One row per app that declares live tunables. Order here is the order
// `TUNE` (bare list) and the emulator's tunables panel present them in -
// nothing downstream depends on that order beyond it being stable within
// one build.
static const tune_provider_t g_tuneProviders[] = {
    { sketch_tune_count, sketch_tune_describe, sketch_tune_define_name, sketch_tune_get, sketch_tune_set },
    { clock_tune_count,  clock_tune_describe,  clock_tune_define_name,  clock_tune_get,  clock_tune_set },
    { tables_tune_count, tables_tune_describe, tables_tune_define_name, tables_tune_get, tables_tune_set },
};
#define TUNE_PROVIDER_COUNT ((int)(sizeof(g_tuneProviders) / sizeof(g_tuneProviders[0])))

int tune_registry_count(void) {
    int total = 0;
    for (int p = 0; p < TUNE_PROVIDER_COUNT; p++) total += g_tuneProviders[p].count();
    return total;
}

bool tune_registry_describe(int index, const char **name, float *min, float *max, float *def) {
    if (index < 0) return false;
    for (int p = 0; p < TUNE_PROVIDER_COUNT; p++) {
        int n = g_tuneProviders[p].count();
        if (index < n) return g_tuneProviders[p].describe(index, name, min, max, def);
        index -= n;
    }
    return false;
}

const char *tune_registry_define_name(int index) {
    if (index < 0) return NULL;
    for (int p = 0; p < TUNE_PROVIDER_COUNT; p++) {
        int n = g_tuneProviders[p].count();
        if (index < n) return g_tuneProviders[p].define_name(index);
        index -= n;
    }
    return NULL;
}

// Name lookup tries each provider in turn rather than tracking which
// provider owns which name: every provider's own find-by-name already
// returns false for a name it does not recognise (see e.g. sketch.c's
// sketch_tune_find), so this is a plain linear probe, same shape as
// devlink_tune_index_of() walking tune_describe() one entry at a time.
// Provider tunable names must stay unique across the whole registry for
// this to be unambiguous; today's three sets (sketch's stroke-timing
// words, clock's "pulse*" words, tables' "thumbbias") do not collide, and
// a fourth app joining this list should pick names that keep it that way.
bool tune_registry_get(const char *name, float *out) {
    for (int p = 0; p < TUNE_PROVIDER_COUNT; p++) {
        if (g_tuneProviders[p].get(name, out)) return true;
    }
    return false;
}

bool tune_registry_set(const char *name, float value, float *outApplied) {
    for (int p = 0; p < TUNE_PROVIDER_COUNT; p++) {
        if (g_tuneProviders[p].set(name, value, outApplied)) return true;
    }
    return false;
}
