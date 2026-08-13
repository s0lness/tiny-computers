/*
 * shim/math.h: NOT one of the shims the task brief anticipated - see
 * shim/stdlib.h's header comment for why this directory has to grow beyond
 * DEV_Config.h/AMOLED_1in8.h at all (short version: zig 0.16's
 * wasm32-freestanding target has no <math.h>, confirmed empirically, because
 * it is a hosted-only header the C standard does not require a freestanding
 * implementation to ship).
 *
 * apps/sketch.c and apps/timer.c `#include <math.h>` as real firmware code
 * compiled unmodified for this target. Two different things live here:
 *
 * 1. The eight functions emu_abi.h documents as host imports (env.sinf,
 *    env.cosf, ...): declared `extern`, given NO body, so the linker leaves
 *    them undefined and `-Wl,--import-symbols` (see build.ts) turns them
 *    into real wasm imports the JS host must supply. Deliberately not
 *    implemented locally - see emu_abi.h's own reasoning: a second,
 *    hand-rolled sin/sqrt would be a second source of numerical divergence
 *    from the board, on top of the one that already exists (float vs
 *    double), and the host's Math.* is at least correct.
 *
 * 2. ceilf(), fminf() and lroundf(): used by apps/sketch.c and
 *    apps/timer.c but NOT in emu_abi.h's import list - a real gap between
 *    what the firmware needs and what the documented ABI supplies, found by
 *    actually linking, not assumed. Reported rather than silently patched
 *    would normally mean leaving them undefined too and letting them become
 *    a THIRD, undocumented import the JS side has to guess about, which is
 *    the exact failure mode emu_abi.h warns against ("a mismatch has to be
 *    visible, not silently patched"). The difference here: all three are
 *    exact, non-transcendental operations that compose losslessly from
 *    floorf() alone (already a legitimate import, already host-correct), so
 *    defining them locally carries none of the numerical-divergence risk
 *    the ABI comment is actually worried about. They stay OFF the import
 *    list on purpose - see build.ts's report notes.
 */
#ifndef _SHIM_MATH_H_
#define _SHIM_MATH_H_

#define M_PI 3.14159265358979323846

extern float sinf(float x);
extern float cosf(float x);
extern float atan2f(float y, float x);
extern float sqrtf(float x);
extern float fabsf(float x);
extern float floorf(float x);
extern float fmodf(float x, float y);
extern float powf(float x, float y);

// Composed from floorf() alone - see this file's header comment, item 2.
static inline float ceilf(float x) {
    return -floorf(-x);
}

static inline float fminf(float a, float b) {
    return a < b ? a : b;
}

static inline long lroundf(float x) {
    return (long)(x >= 0.0f ? floorf(x + 0.5f) : ceilf(x - 0.5f));
}

#endif
