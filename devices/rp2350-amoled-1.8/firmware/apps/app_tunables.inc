/* app_tunables.inc: which of THIS firmware's apps expose live tunables.
 *
 * INCLUDED, not compiled, by firmware/runtime/tune_registry.c - a SYNCED PACK
 * SOURCE (tools/sync-pack.ts) that aggregates these providers and knows
 * nothing about which apps exist. Read that file's comment at the include site
 * for why the seam is here; the short version is that a provider row naming an
 * app a firmware does not compile is a link error, not a stale comment.
 *
 * THIS FILE IS OURS. One row per app that declares live tunables, in the order
 * `TUNE` (bare list) and the emulator's tunables panel present them. The
 * function names come from sensors.h's "DEVELOPMENT: live tuning" section.
 */
static const tune_provider_t g_tuneProviders[] = {
    { sketch_tune_count, sketch_tune_describe, sketch_tune_define_name, sketch_tune_get, sketch_tune_set },
    { clock_tune_count,  clock_tune_describe,  clock_tune_define_name,  clock_tune_get,  clock_tune_set },
    { tables_tune_count, tables_tune_describe, tables_tune_define_name, tables_tune_get, tables_tune_set },
};
