/*
 * sound_synth: the actual chime, as pure math. No PIO, no DMA, no i2c, no
 * pico-sdk - just <math.h> - so this compiles for both the board
 * (firmware/runtime/sound.c calls it from its DMA refill) and the emulator
 * (emulator/wasm/emu_shim.c calls the SAME function to fill the buffer it
 * hands the browser for WebAudio playback, per emu_abi.h's sound section).
 * That split is what makes emu_abi.h able to say the emulator's audio is
 * genuinely produced by the firmware's own synthesiser rather than a
 * JavaScript reimplementation: this file is not duplicated, it is compiled
 * twice, once into main.uf2 and once into emu.wasm - the same object-code
 * argument decision 0003 already makes for the graphics and app logic.
 *
 * See sound_synth.c for the musical reasoning (the pentatonic scale, the
 * envelope, the harmonic, the amplitude headroom).
 */
#ifndef SOUND_SYNTH_H
#define SOUND_SYNTH_H

#include <stdint.h>

// Both targets synthesise and play at this rate. Fixed rather than
// configurable: it is also the ES8311's BCLK-derived-clock coefficient-table
// row this board's I2S setup depends on (sound.c's header comment), so
// changing it is a hardware decision, not a per-call parameter.
#define SOUND_SYNTH_SAMPLE_RATE_HZ 32000

// One mono sample of the timer alarm's chime, tSec seconds into playback
// (0.0 = the instant sound_play() was called). Stateless and a pure function
// of tSec - see sound_synth.c's "COST" section for why sample-accurate
// synthesis is computed this way rather than from accumulated per-note phase
// state. Both I2S channels (board) or both... there is only one channel here
// (emulator, mono WebAudio) get this same value; see sound.c/emu_shim.c for
// how each target turns one mono int16 into what its own output path wants.
int16_t sound_synth_alarm_sample(float tSec);

// One mono sample of the tilt-a-ball's capture sound (firmware/apps/
// tiltball.c), tSec seconds into playback. A single falling-pitch note, not
// a repeating phrase - see sound_synth.c's own comment for the reasoning.
// Same stateless-function-of-tSec shape as the alarm, for the same reason.
int16_t sound_synth_capture_sample(float tSec);

#endif // SOUND_SYNTH_H
