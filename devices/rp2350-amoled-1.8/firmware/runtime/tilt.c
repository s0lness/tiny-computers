/*
 * tilt: the orientation signal's one implementation. See tilt.h for what is
 * published, why that shape, what filtering was chosen and what no
 * instrument here can see.
 *
 * PORTABLE, LIKE runtime_core.c AND sound_synth.c, and for the same reason
 * (docs/decisions/0003): it is compiled into the board's image
 * (firmware/CMakeLists.txt) and into emu.wasm (emulator/wasm/build.ts) from
 * this one source, so the emulator's tilt is the board's tilt - the same
 * filter with the same constants, the same hysteresis, the same
 * device-to-panel mapping - rather than a browser-side reimplementation
 * that agrees on the day it is written and drifts from the next commit.
 * That means: no pico-sdk, nothing under hardware/ or pico/, nothing but
 * freestanding headers and math.h.
 */
#include "tilt.h"

#include <math.h>

// device_to_panel(), lp_alpha() and tilt_submit_device_g() below run on
// core1 (sensors.c's imu_poll_core1() calls tilt_submit_device_g() on every
// IMU sample), so they are RAM-pinned like every other function on that
// path - see docs/decisions/0004/0005 (the hazard) and
// tools/invariants/rules/rp2350-amoled-1.8.ts (the check that enforces it).
// tilt_read() below is core0-only (runtime_core.c reads the published
// snapshot) and is deliberately left in flash.
//
// A hand-rolled macro, not `#include "pico/platform.h"`: this file's own
// portability rule (see the header comment above) is "no pico-sdk, nothing
// but freestanding headers and math.h", so it does not reach for the SDK's
// own `__not_in_flash_func` just to get one section attribute. The
// expansion is pico-sdk's own (`pico/platform/sections.h`:
// `__attribute__((section(".time_critical." #name))) name`), copied rather
// than included, because the RAM-placement need is real on the board build
// but the wasm build (emulator/wasm/build.ts, wasm32-freestanding) has no
// flash/RAM distinction to make at all.
#if defined(__ARM_ARCH)
#define TILT_NOT_IN_FLASH(name) __attribute__((section(".time_critical." #name))) name
#else
#define TILT_NOT_IN_FLASH(name) name
#endif

#define TILT_RAD_TO_DEG 57.29577951308232f

// M_PI is not guaranteed under -std=c11, same reason level.c, menu.c and
// timer.c each carry their own.
#define TILT_PI 3.14159265358979323846f

/* ---- device axes to panel axes -------------------------------------------
 *
 * MEASURED 2026-08-17, on the real board, via tilt.h's axis ritual
 * (`bun tools/dev.ts tilt`), several stable samples per pose. The RAW column
 * below is what devlink's TILT command printed; the target is tilt.h's own
 * panel convention (+x right, +y down, +z into the glass):
 *
 *   flat, screen up               raw ~= ( 0.04,  0.05, -1.01)  want (0,0,+1)
 *   upright, top edge up          raw ~= ( 1.02,  0.03,  0.05)  want (0,+1,0)
 *   quarter turn, right edge up   raw ~= ( 0.04,  1.05,  0.03)  want (+1,0,0)
 *
 * (The third pose is the one AGENTS.md calls "the button edge pointing at
 * the ceiling" - screen facing the owner, BOOT above PWR, then rolled a
 * quarter turn so that edge points at the sky.)
 *
 * Each pose isolates one raw axis to within noise, so the fit is direct:
 * raw Z is what reads near +-1 when flat, raw X when upright-top-up, raw Y
 * when on the button edge. That gives panel Z = -raw Z, panel Y = raw X,
 * and panel X = raw Y - swap X and Y, negate Z:
 *
 *   *px = dy;
 *   *py = dx;
 *   *pz = -dz;
 *
 * THE Z SIGN IS THE ACCELEROMETER-VS-GRAVITY FLIP, MADE CONCRETE. An
 * accelerometer at rest reports specific force, which points AWAY from the
 * earth (the reaction holding the part up), not the direction gravity
 * itself pulls. Flat, screen up, that reads -1 on the axis this part calls
 * Z; tilt.h's own contract wants +1 there (tiltDeg must read 0 at rest, and
 * atan2(inPlane, gz) cannot do that unless gz is positive) - so this one
 * axis needs the flip and the fit above is where it lives, not folded in a
 * second time anywhere else.
 *
 * VERIFY IT ROTATES RATHER THAN REFLECTS, because a reflection is not
 * something any physical mounting can produce, and it is easy to derive one
 * by accident chasing the flip above axis by axis. As a matrix (row i is
 * the panel component pI as a function of dx, dy, dz):
 *
 *   | 0  1  0 |
 *   | 1  0  0 |
 *   | 0  0 -1 |
 *
 * det = 0*(0*-1 - 0*0) - 1*(1*-1 - 0*0) + 0*(1*0 - 0*0) = -1*(-1) = +1.
 * Proper rotation: geometrically a 180-degree turn about the in-plane
 * diagonal (1,1,0)/sqrt(2) of the chip's own X-Y plane - an entirely
 * ordinary way to solder a six-axis part onto a board that was not laid out
 * around it. (A naive fit straight off the ritual's OWN older wording for
 * the third pose - "expect g = (-1, 0, 0)" - gives determinant -1, a
 * reflection, which is how this got caught: that pose's g was written down
 * with the wrong sign, inconsistent with its own "up = LEFT," which the
 * up-edge code below only produces for a POSITIVE panel X. Corrected here
 * and in tilt.h's ritual text.)
 *
 * CROSS-CHECKED against a fourth, independent reading: the puck tilted
 * toward the BOOT button while running breakout (a landscape app), raw
 * ~= (-0.86, -0.01, -0.48). This mapping turns that into panel g ~=
 * (-0.01, -0.86, +0.48); runtime_core.c's tilt_for_app() then rotates a
 * landscape app's gx from panel gy (see that function's own header
 * comment), so breakout's paddle receives gx ~= -0.86 - strongly negative,
 * i.e. LEFT, which is exactly what the owner reported the paddle should do
 * tilting toward BOOT and what it was NOT doing under the identity mapping
 * this replaces (which put the same gesture on gy, "front/back", instead).
 *
 * THE 90-DEGREE ROTATION THE OWNER NOTICED BETWEEN RAW AND WHAT THE APP
 * RECEIVED WAS NEVER A BUG. It is runtime_core.c's own tilt_for_app(),
 * applied because chrono and breakout are both app_t.landscape - correct,
 * deliberate, and unrelated to this function. It was only ever visible
 * because THIS function, until now, was the identity: every measurement in
 * this file's history could be explained by that one rotation alone, which
 * is what made the axis mapping below look unverified rather than simply
 * uncorrected.
 *
 * UNVERIFIED ON SILICON: this correction is derived from the raw readings
 * above, taken on the real board against the OLD (identity) code. The
 * corrected function itself has run in the emulator and its tests, never
 * yet on the board. The owner will verify with a flashed build.
 */
static void TILT_NOT_IN_FLASH(device_to_panel)(float dx, float dy, float dz,
                            float *px, float *py, float *pz) {
    *px = dy;
    *py = dx;
    *pz = -dz;
}

/* ---- the up-edge decision, and its hysteresis ----------------------------
 *
 * Policy, published rather than left to each app, because three apps
 * deriving "which way is up" from atan2 would get three different flicker
 * behaviours out of the same hardware. Two guards, both needed:
 *
 *   TILT_UP_MIN_G      the in-plane part of gravity must be at least this
 *                      big before the answer may change at all. 0.35g is
 *                      about 20 degrees off flat. Below it, a device lying
 *                      on a table has an in-plane vector that is pure
 *                      noise, and the answer would spin.
 *   TILT_UP_DOMINANCE  near a diagonal the two axes are nearly equal, so
 *                      the winner would alternate every few samples.
 *                      Requiring one axis to beat the other by 1.3x moves
 *                      each boundary from 45 degrees to about 37.6, which
 *                      leaves a 15 degree band around each diagonal where
 *                      the previous answer simply holds.
 *
 * Holding rather than reporting "unknown" is deliberate: an orientation
 * aware clock laid flat on a table should keep the orientation it had, not
 * blank out. An app that wants to know it is flat reads tiltDeg.
 */
#define TILT_UP_MIN_G     0.35f
#define TILT_UP_DOMINANCE 1.3f

/* ---- the filter's constants -----------------------------------------------
 *
 * One euro (Casiez et al.) plus a magnitude trust gate. See tilt.h's
 * "FILTERING" section for the full argument and the measurement it is
 * built on; the short version is in each constant's own line below. Ported
 * verbatim, in behaviour, from firmware/apps/level.c's own provisional
 * filter (commit c00db2f) - the level measured this trade before there was
 * a shared signal to hand it to, and this file is that hand-off.
 */
#define TILT_FC_MIN_HZ        0.9f    // corner at rest
#define TILT_BETA_HZ_PER_G_MS 3000.0f // extra corner per unit of measured speed
#define TILT_TAU_D_MS       200.0f    // corner for smoothing the derivative itself
#define TILT_TRUST_FULL_G     0.15f   // |a-1g| below this: fully trusted
#define TILT_TRUST_NONE_G     0.4f    // |a-1g| at or above this: fully coasting

// alpha for a one-pole low pass with corner fc (Hz) at a dt (ms) step.
// tau = 1/(2*pi*fc); alpha = dt/(tau+dt). No expf: exact enough (under a
// percent off the exponential form at these rates - measured when this
// lived in level.c).
static float TILT_NOT_IN_FLASH(lp_alpha)(float fcHz, float dtMs) {
    float tauMs = 1000.0f / (2.0f * TILT_PI * fcHz);
    return dtMs / (tauMs + dtMs);
}

/* ---- state owned by the submitting core ----------------------------------
 *
 * On the board every one of these is touched only by core1, inside
 * tilt_submit_device_g(). They are not volatile and need not be: nothing
 * else reads them. What crosses to core0 is the published snapshot below.
 */
static bool  s_haveSample = false;
static uint32_t s_lastMs = 0;
static float s_fx = 0.0f, s_fy = 0.0f, s_fz = 1.0f; // filtered, panel axes
static float s_rawPrevX = 0.0f, s_rawPrevY = 0.0f, s_rawPrevZ = 1.0f; // raw, for the derivative
static float s_dgx = 0.0f, s_dgy = 0.0f, s_dgz = 0.0f; // low-passed derivative, g/ms (one euro)
static uint8_t s_up = TILT_UP_TOP;

/* ---- publication: one snapshot, one writer, one reader --------------------
 *
 * The other cross-core signals in this firmware are single words (see
 * sensors.c's "cross-core published state"), and a single aligned word
 * needs no barrier: the worst case is the reader seeing last loop's value.
 * A vector is not a single word. Three separate floats would let core0 read
 * x from one sample and y from the next, and while the practical error is
 * tiny for a filtered signal, "tiny" is not an argument a reader of this
 * file should have to reconstruct.
 *
 * So: a sequence lock, the standard shape. The writer bumps the counter to
 * an odd value, writes, and bumps it back to even; the reader takes the
 * counter, copies, and retries if the counter moved. Barriers on both
 * sides, because the whole point is that the copy is not reordered across
 * the counter.
 *
 * The reader cannot spin forever, and does not: the writer holds the lock
 * for the few dozen cycles of a struct copy at 50Hz, so a reader that fails
 * twice has hit something impossible; it takes the last snapshot it read
 * successfully instead (kept in a reader-owned static) and moves on. A
 * frame of orientation is not worth a stall on the render core.
 */
#if defined(__ARM_ARCH)
// Cortex-M33, two cores against one SRAM: a real data memory barrier, which
// is what __atomic_thread_fence emits here (the same dmb sensors.c's touch
// ring takes through the SDK's __dmb()).
#define TILT_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
// wasm: one thread, no second core to be visible to, so all that is needed
// is that the compiler does not reorder the copy across the counter.
#define TILT_BARRIER() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#endif

typedef struct {
    float gx, gy, gz;
    float tiltDeg;
    float rawX, rawY, rawZ;
    uint32_t stampMs;
    uint8_t up;
    bool haveSample;
    bool coasting;
} tilt_pub_t;

static volatile uint32_t s_pubSeq = 0;
static tilt_pub_t s_pub = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0u, TILT_UP_TOP, false, false };

void TILT_NOT_IN_FLASH(tilt_submit_device_g)(float dx, float dy, float dz, uint32_t nowMs) {
    float px, py, pz;
    device_to_panel(dx, dy, dz, &px, &py, &pz);

    bool coasting = false;

    if (!s_haveSample) {
        // First sample lands whole rather than fading in from the (0,0,1)
        // initial state: a level that spends its first half second easing
        // toward the truth is a level that lies for half a second.
        s_fx = px;
        s_fy = py;
        s_fz = pz;
        s_rawPrevX = px;
        s_rawPrevY = py;
        s_rawPrevZ = pz;
        s_dgx = s_dgy = s_dgz = 0.0f;
        s_haveSample = true;
    } else {
        uint32_t dtMs = nowMs - s_lastMs;
        if (dtMs > 1000u) dtMs = 1000u; // a gap this long (a paused emulator
                                         // tab, a stalled bus) means the old
                                         // value is meaningless anyway, and
                                         // the clamp keeps every alpha below
                                         // sane bounds rather than letting a
                                         // huge dt dominate the filter.
        if (dtMs < 1u) dtMs = 1u;       // floor for the derivative's divide
        float dtF = (float)dtMs;

        // Low-passed derivative of the RAW signal (one euro), so a fast
        // tremor cannot masquerade as a fast intentional move and unlock
        // the filter that is meant to be hiding it. TILT_TAU_D_MS is the
        // corner for this smoothing step specifically, separate from the
        // main filter's own adaptive corner below.
        float aD = dtF / (TILT_TAU_D_MS + dtF);
        float ddx = (px - s_rawPrevX) / dtF;
        float ddy = (py - s_rawPrevY) / dtF;
        float ddz = (pz - s_rawPrevZ) / dtF;
        s_rawPrevX = px;
        s_rawPrevY = py;
        s_rawPrevZ = pz;
        s_dgx += aD * (ddx - s_dgx);
        s_dgy += aD * (ddy - s_dgy);
        s_dgz += aD * (ddz - s_dgz);

        // The corner widens with measured speed: barely moving, smooth
        // hard (TILT_FC_MIN_HZ); moving fast, barely at all.
        float speed = sqrtf(s_dgx * s_dgx + s_dgy * s_dgy + s_dgz * s_dgz);
        float alpha = lp_alpha(TILT_FC_MIN_HZ + TILT_BETA_HZ_PER_G_MS * speed, dtF);

        // Magnitude trust gate: 1.0 while |a| is near one g (this is
        // genuinely gravity), falling to 0.0 as the device is picked up,
        // dropped or shaken - a low pass alone cannot fix this, because the
        // reading itself has stopped being gravity.
        float mag = sqrtf(px * px + py * py + pz * pz);
        float off = fabsf(mag - 1.0f);
        float trust = 1.0f;
        if (off >= TILT_TRUST_NONE_G) {
            trust = 0.0f;
        } else if (off > TILT_TRUST_FULL_G) {
            trust = 1.0f - (off - TILT_TRUST_FULL_G) / (TILT_TRUST_NONE_G - TILT_TRUST_FULL_G);
        }
        alpha *= trust;
        coasting = (trust <= 0.0f);

        s_fx += alpha * (px - s_fx);
        s_fy += alpha * (py - s_fy);
        s_fz += alpha * (pz - s_fz);
    }
    s_lastMs = nowMs;

    // Which edge is up, from the FILTERED vector: deciding this from raw
    // samples would put the jitter back into the one field whose whole
    // purpose is to be stable.
    float inPlane = sqrtf(s_fx * s_fx + s_fy * s_fy);
    if (inPlane >= TILT_UP_MIN_G) {
        float ax = fabsf(s_fx), ay = fabsf(s_fy);
        if (ay >= ax * TILT_UP_DOMINANCE) {
            // Gravity pulls toward the bottom edge, so the TOP edge is up.
            s_up = (s_fy > 0.0f) ? TILT_UP_TOP : TILT_UP_BOTTOM;
        } else if (ax >= ay * TILT_UP_DOMINANCE) {
            s_up = (s_fx > 0.0f) ? TILT_UP_LEFT : TILT_UP_RIGHT;
        }
        // else: inside the diagonal dead band, hold the previous answer.
    }

    tilt_pub_t next;
    next.gx = s_fx;
    next.gy = s_fy;
    next.gz = s_fz;
    // atan2f, not asinf/acosf: those two are not in emu_abi.h's host import
    // list and adding an import to the ABI to compute an angle two other
    // functions can already give is not worth it. atan2(in-plane, z) is
    // also the form that stays correct past 90 degrees, which acos of a
    // clamped dot product does not.
    next.tiltDeg = atan2f(inPlane, s_fz) * TILT_RAD_TO_DEG;
    next.rawX = dx;
    next.rawY = dy;
    next.rawZ = dz;
    next.stampMs = nowMs;
    next.up = s_up;
    next.haveSample = true;
    next.coasting = coasting;

    s_pubSeq++;            // odd: a write is in progress
    TILT_BARRIER();
    s_pub = next;
    TILT_BARRIER();
    s_pubSeq++;            // even again: consistent
}

void tilt_read(uint32_t nowMs, tilt_reading_t *out) {
    // Reader-owned, so a retry exhaustion (see the seqlock comment) has
    // something consistent to fall back on rather than a torn read.
    static tilt_pub_t lastGood = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0u, TILT_UP_TOP, false, false };

    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t before = s_pubSeq;
        if (before & 1u) continue; // writer mid-update
        TILT_BARRIER();
        tilt_pub_t snap = s_pub;
        TILT_BARRIER();
        if (s_pubSeq == before) {
            lastGood = snap;
            break;
        }
    }

    out->gx = lastGood.gx;
    out->gy = lastGood.gy;
    out->gz = lastGood.gz;
    out->tiltDeg = lastGood.tiltDeg;
    out->up = lastGood.up;
    out->rawX = lastGood.rawX;
    out->rawY = lastGood.rawY;
    out->rawZ = lastGood.rawZ;
    out->coasting = lastGood.coasting;
    // Unsigned wrap is fine and intended: nowMs and stampMs come from the
    // same clock, so the difference is small and correct across the 32-bit
    // rollover the same way every other elapsed-time comparison in this
    // firmware is.
    out->valid = lastGood.haveSample && (nowMs - lastGood.stampMs) <= TILT_STALE_MS;
}
