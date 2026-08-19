/*
 * sound_synth: the timer alarm's chime, as pure math. See sound_synth.h for
 * why this file has to stay hardware-blind (compiled into both main.uf2 and
 * emu.wasm, unmodified).
 *
 * THE BRIEF: "un truc vraiment plutot mignon" (genuinely, rather cute). Two
 * choices carry almost all of the character, both cheap:
 *
 * PENTATONIC SCALE. C major pentatonic, C5..C6 (523.25, 659.25, 783.99,
 * 1046.50 Hz - equal temperament, A4=440Hz; the four notes used skip D5
 * (587.33) and A5 (880.00) to keep the motif to four notes): no combination
 * of these notes, played in any order, sounds "wrong" - it is why music
 * boxes and toys use this scale. A short RISING four-note motif (C5, E5, G5,
 * C6) reads as a bright, simple, upward gesture rather than a melody someone
 * could get "wrong".
 *
 * ENVELOPE, not frequency, is what makes a note read as STRUCK (a chime, a
 * marimba) rather than as an appliance buzzer - a beep and a chime can be
 * the exact same pitch, and the envelope is the entire difference. Fast
 * linear attack (CHIME_ATTACK_S, ~4ms: near-instant, but not a single-sample
 * click), then exponential decay (CHIME_DECAY_TAU_S) - the shape a struck
 * idiophone actually produces, energy bleeding away continuously rather than
 * being cut off or sustained.
 *
 * A small second harmonic (CHIME_HARMONIC_RATIO=2x the fundamental, at
 * CHIME_HARMONIC_PEAK ~18% of the fundamental's peak, decaying on the same
 * envelope - simple, and adequate for "a small amount... to give it body"
 * without modelling a real bell's inharmonic partials) adds body without
 * changing the note's identity.
 *
 * Notes are spaced CHIME_NOTE_SPACING_S (140ms) apart, closer than each
 * note's own audible window (CHIME_NOTE_AUDIBLE_S, 260ms), so consecutive
 * notes overlap slightly as their tails ring into the next attack - a
 * legato chime, not four separate clicks. The whole four-note phrase then
 * repeats every CHIME_PHRASE_PERIOD_S (1.5s): the phrase itself lasts well
 * under a second, so most of each 1.5s cycle is silence - "repeated gently
 * rather than continuously", per the brief.
 *
 * Peak amplitude (CHIME_FUND_PEAK 9000 of a possible 32767, plus harmonic)
 * stays well under half of int16 full scale: "keep the peak amplitude well
 * below clipping, since a tiny speaker distorts easily".
 *
 * COST. Up to CHIME_NOTE_COUNT notes are evaluated per sample (their audible
 * windows can overlap by construction), each with two sinf() calls and one
 * expf() while inside its attack-decay window. On the board this costs
 * single-digit percent of core0's budget on average (worst case, all notes
 * overlapping every sample, would still be a small fraction of the 150MHz
 * budget) - accepted deliberately rather than optimised into a phase-
 * accumulator/lookup-table synthesiser (the standard cheaper alternative),
 * because it runs only for the alarm's own bounded window (ALARM_MAX_MS in
 * timer.c, 30s, itself rare), nothing else on core0 is time-critical during
 * it (touch/IMU/PMIC are core1's, per sensors.h), and computing each sample
 * directly from tSec rather than from accumulated per-note phase state keeps
 * the frequencies this file claims exactly what it computes, with no
 * separate quantisation error to account for when checking a recording's
 * measured spectrum against them. expf is imported from the host in the
 * emulator build (emu_abi.h) the same way sinf already is, for the same
 * reason: a hand-rolled exp() would be a second source of numerical
 * divergence from the board, and the host's is at least correct.
 */
#include "sound_synth.h"

#include <math.h>

#define CHIME_NOTE_COUNT 4
static const float CHIME_NOTE_HZ[CHIME_NOTE_COUNT] = { 523.25f, 659.25f, 783.99f, 1046.50f }; // C5 E5 G5 C6
#define CHIME_NOTE_SPACING_S  0.140f
#define CHIME_ATTACK_S        0.004f
#define CHIME_DECAY_TAU_S     0.090f
#define CHIME_NOTE_AUDIBLE_S  0.260f
#define CHIME_PHRASE_PERIOD_S 1.500f
#define CHIME_FUND_PEAK       9000.0f
#define CHIME_HARMONIC_RATIO  2.0f
#define CHIME_HARMONIC_PEAK   (CHIME_FUND_PEAK * 0.18f)
#define CHIME_TWO_PI          6.283185307179586f
#define CHIME_CLIP_PEAK       32000.0f // headroom under int16's 32767

int16_t sound_synth_alarm_sample(float tSec) {
    float phraseT = fmodf(tSec, CHIME_PHRASE_PERIOD_S);
    float acc = 0.0f;
    for (int n = 0; n < CHIME_NOTE_COUNT; n++) {
        float noteT = phraseT - (float)n * CHIME_NOTE_SPACING_S;
        if (noteT < 0.0f || noteT >= CHIME_NOTE_AUDIBLE_S) continue;

        float env = (noteT < CHIME_ATTACK_S)
            ? (noteT / CHIME_ATTACK_S)
            : expf(-(noteT - CHIME_ATTACK_S) / CHIME_DECAY_TAU_S);

        float freq = CHIME_NOTE_HZ[n];
        float phase = CHIME_TWO_PI * freq * tSec;
        acc += env * (CHIME_FUND_PEAK * sinf(phase) +
                       CHIME_HARMONIC_PEAK * sinf(phase * CHIME_HARMONIC_RATIO));
    }
    if (acc > CHIME_CLIP_PEAK) acc = CHIME_CLIP_PEAK;
    if (acc < -CHIME_CLIP_PEAK) acc = -CHIME_CLIP_PEAK;
    return (int16_t)acc;
}

/* ---------------------------------------------------------------------
 * The tilt-a-ball's capture sound (firmware/apps/tiltball.c): played once,
 * the instant the ball reaches the hole - the moment the app's own header
 * comment argues is worth more effort than the physics that got the ball
 * there. A DESCENDING pitch, not the alarm's rising phrase: a falling glide
 * is the standard game-audio shorthand for "something dropping in" (a coin
 * in a slot, a marble down a chute), and using it here means the sound and
 * the picture agree about what just happened without either needing words.
 *
 * ONE NOTE, ONE SHOT, NOT A REPEATING PHRASE. The alarm rings for up to 30s
 * and has to stay legible without wearing on the ear (docs/decisions/0002
 * section 7's "genuinely, rather cute" brief); this fires once per capture
 * and has to read as a single, complete event. sound_play() resets playback
 * to tSec=0 on every call (sound.c/emu_shim.c), so tiltball.c can call it
 * again on the very next capture with no extra bookkeeping: past
 * CAPTURE_AUDIBLE_S this returns 0 forever until the next sound_play().
 *
 * PHASE, INTEGRATED RATHER THAN MULTIPLIED. A linearly swept frequency is
 * NOT phase = 2*pi*f(t)*t (that is only correct for a constant f); the
 * actual instantaneous frequency is the phase's own derivative, so a linear
 * sweep from CAPTURE_F0 to CAPTURE_F1 over CAPTURE_SWEEP_S needs the
 * INTEGRAL of that ramp, which is the quadratic term below. Getting this
 * wrong does not fail loudly - it still glides, just at a warped rate - so
 * it is worth writing the derivation down rather than eyeballing it: for
 * f(t) = f0 + (f1-f0)*t/T, phase(t) = 2*pi*(f0*t + (f1-f0)*t^2/(2T)), and
 * phase(T) (used below to splice on the constant-frequency tail cleanly, no
 * click at the seam) is that same expression at t=T.
 */
#define CAPTURE_F0            900.0f  // Hz, bright, where the ball WAS
#define CAPTURE_F1            260.0f  // Hz, low, where the hole IS
#define CAPTURE_SWEEP_S        0.22f  // how long the glide takes
#define CAPTURE_ATTACK_S      0.003f
#define CAPTURE_DECAY_TAU_S    0.09f
#define CAPTURE_AUDIBLE_S      0.30f  // > SWEEP_S: the decay tail rings out
                                       // after the glide has already landed
#define CAPTURE_PEAK        11000.0f  // headroom to spare vs the alarm's per-
                                       // note 9000: one note, not up to four
                                       // overlapping ones, so more of int16's
                                       // range is available before clipping
#define CAPTURE_CLIP_PEAK   32000.0f

int16_t sound_synth_capture_sample(float tSec) {
    if (tSec < 0.0f || tSec >= CAPTURE_AUDIBLE_S) return 0;

    float phase;
    if (tSec < CAPTURE_SWEEP_S) {
        phase = CHIME_TWO_PI * (CAPTURE_F0 * tSec +
                 (CAPTURE_F1 - CAPTURE_F0) * (tSec * tSec) / (2.0f * CAPTURE_SWEEP_S));
    } else {
        float phaseAtLanding = CHIME_TWO_PI *
            (CAPTURE_F0 * CAPTURE_SWEEP_S + (CAPTURE_F1 - CAPTURE_F0) * CAPTURE_SWEEP_S / 2.0f);
        phase = phaseAtLanding + CHIME_TWO_PI * CAPTURE_F1 * (tSec - CAPTURE_SWEEP_S);
    }

    float env = (tSec < CAPTURE_ATTACK_S)
        ? (tSec / CAPTURE_ATTACK_S)
        : expf(-(tSec - CAPTURE_ATTACK_S) / CAPTURE_DECAY_TAU_S);

    float acc = env * CAPTURE_PEAK * sinf(phase);
    if (acc > CAPTURE_CLIP_PEAK) acc = CAPTURE_CLIP_PEAK;
    if (acc < -CAPTURE_CLIP_PEAK) acc = -CAPTURE_CLIP_PEAK;
    return (int16_t)acc;
}
