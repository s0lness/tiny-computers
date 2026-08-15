/*
 * sound: a runtime SERVICE for the one speaker on this board, on the same
 * "apps read signals, never chips" shape as sensors.h (app.h's rule). An app
 * calls sound_play(id) / sound_stop(); nothing outside this file ever talks
 * to the ES8311 codec or the I2S pins directly.
 *
 * THE OWNERSHIP DECISION, which decision 0002 section 7 flagged as the
 * reason sound was reserved rather than built immediately: "the codec sits
 * on i2c1, so its configuration belongs to core 1, which is the most
 * delicate part of this architecture." This file's answer is neither of the
 * two options that sentence poses - it does not run on core1, and it does
 * not reopen sensors.c/sensors.h at all.
 *
 * sound_init() brings the ES8311 up over i2c1 from CORE0, ONCE, before
 * sensors_start() launches core1 (called from runtime.c right after
 * sensors_init(), the same place the PMIC and touch controller are brought
 * up - see sensors.h's ownership banner: "core0 must NEVER touch i2c1" is a
 * rule about AFTER sensors_start(), and core0 is single-threaded up to that
 * call, same as it always was). After that one-time init, THIS FILE NEVER
 * TOUCHES I2C1 AGAIN - see "why playback needs no further i2c1 traffic"
 * below for how sound_play()/sound_stop(), called at arbitrary times
 * afterward, get away with that. So there is no cross-core signal to design
 * here at all, unlike every sensor in sensors.h: no queue, no injected ring,
 * no core0-request/core1-execute split. The codec's control surface is used
 * exactly once, at boot, entirely before core1 exists to have an opinion
 * about it.
 *
 * WHY PLAYBACK NEEDS NO FURTHER I2C1 TRAFFIC. The ES8311 is configured once
 * at boot to a fixed volume, unmuted, and left that way permanently; "play"
 * and "stop" are then purely a DATA-PLANE decision (is the I2S stream
 * carrying the chime's samples or all-zero silence), never a CONTROL-PLANE
 * one (no mute/unmute register write, no volume change). The I2S clock
 * itself (bit clock + word select, both PIO side-set outputs) and the DMA
 * feeding it are started once in sound_init() and never stopped: sound.c's
 * own DMA-refill routine (sound_poll(), called every core0 main-loop
 * iteration, exactly like sensors.c's touch/key drains) always keeps the
 * PIO's FIFO fed with SOMETHING, chime or silence, so the bus never glitches
 * from being started or stopped. See sound.c's header comment for why this
 * is also what makes sound_stop() effectively instant.
 *
 * THE PIO/DMA HALF is not an i2c1 question at all - it is a completely
 * separate bus (I2S over PIO, and a DMA channel neither sensors.c nor gfx.c
 * uses), so nothing about it depends on core1 either. It happens to be set
 * up in the same sound_init() call for locality (one function that brings up
 * "the speaker", not two), not because it needs to run before core1 starts.
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>

/* ---- sounds --------------------------------------------------------------
 *
 * Two ids today (the timer's alarm chime, the tilt-a-ball's capture pop),
 * same shape sensors.h's key bits use: a small enum rather than a raw
 * pointer or a sample-table index, so a future app (the dice's clatter, the
 * sketchpad's erase whoosh - see docs/decisions/0002-runtime-architecture.md's
 * app table) adds a case here and a case in sound_synth.c/sound.c's
 * dispatch, never a new API. sound_play() takes the id and remembers it;
 * sound.c's refill picks which sound_synth_* function to sample from.
 */
typedef enum {
    SOUND_ID_TIMER_ALARM = 0,
    // firmware/apps/tiltball.c, played once when the ball reaches the hole.
    // A single falling-pitch "plunk" (sound_synth_capture_sample), not a
    // repeating phrase like the alarm - see that function's own comment.
    SOUND_ID_BALL_CAPTURE = 1,
} sound_id_t;

// Starts (or restarts, if already playing) the given sound. Synthesised on
// the fly, sample by sample, in sound_poll()'s refill - see sound.c's header
// comment for why a stored sample buffer does not fit this device's SRAM
// budget and a synthesiser does. Repeats internally (per the sound's own
// definition, e.g. the alarm's "a short rising motif... repeated gently") for
// as long as sound_play() is not superseded by sound_stop() or a fixed
// internal safety ceiling (sound.c) - the primary 30-second self-limit is the
// TIMER's own (ALARM_MAX_MS in timer.c), not this file's; see sound.c for why
// this file still carries a much longer backstop of its own.
void sound_play(sound_id_t id);

// Silences output immediately. "Immediately" is a real claim, not a
// figure of speech: see sound.c's header comment for why zeroing the DMA
// buffers directly, rather than merely changing what the next refill
// synthesises, is what makes this true within microseconds rather than
// within one buffer's worth of playback (tens of milliseconds).
void sound_stop(void);

/* ---- lifecycle ------------------------------------------------------------
 *
 * sound_init(): core0, before sensors_start() - see this file's header
 * comment. Brings up the ES8311 over i2c1 and the I2S PIO/DMA path; both are
 * silent (all-zero output) until the first sound_play().
 *
 * sound_poll(): core0's main loop, once per iteration, exactly like
 * devlink_poll() - refills whichever DMA buffer just finished playing.
 * Cheap on every call where nothing needs refilling yet (a few comparisons);
 * see sound.c for the buffer sizing that keeps this safe against the panel
 * push's own worst-case stall.
 */
void sound_init(void);
void sound_poll(void);

#endif // SOUND_H
