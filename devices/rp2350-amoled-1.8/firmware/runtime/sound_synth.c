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
