// Plays the SAME SAMPLES the firmware plays. firmware/runtime/sound_synth.c
// compiles into the wasm module unmodified (see emu_abi.h's "sound"
// section), so the browser is not approximating the chime, it is decoding
// PCM the same object code already computed, the same way emu_fb() hands
// over a framebuffer the module already rendered rather than a description
// of one. Only the output path differs: DMA to an ES8311 on the board,
// Web Audio here.
//
// HONESTY, carried from emu_abi.h and worth repeating where a person will
// actually see it: the device's speaker is small and cheap and will
// distort and lose bass in ways whatever plays this browser tab will not.
// This emulator is for judging the TUNE, never the TIMBRE. That is logged
// once (see the onFirstUnlock callback below), not left sitting on the
// page.
//
// Sound is an output, not something the host calls into: firmware logic
// (the timer's alarm) calls sound_play()/sound_stop() on its own, and the
// host's only job is noticing, via two counters it diffs against what it
// last saw (emu_sound_play_seq/emu_sound_stop_seq), exactly the pattern
// emu_push_count() and app_current() already use elsewhere in this ABI.

import type { EmuExports } from "./wasm";

export type SoundStatus = "idle" | "playing" | "suspended";

export class SoundPlayer {
  private ctx: AudioContext | null = null;
  private currentNode: AudioBufferSourceNode | null = null;
  private gain: GainNode | null = null;
  private lastPlaySeq = -1;
  private lastStopSeq = -1;
  private loggedOnce = false;
  muted = false;

  get status(): SoundStatus {
    if (this.currentNode) return "playing";
    if (!this.ctx || this.ctx.state === "suspended") return "suspended";
    return "idle";
  }

  // Call from inside a real user gesture handler (click, keydown,
  // pointerdown): the browser's autoplay policy ties both AudioContext
  // creation and resuming a suspended one to an actual gesture, not to
  // page load. Idempotent, so it is safe to wire to every gesture rather
  // than trying to guess which one is "the first".
  ensureContext(onFirstUnlock: (text: string) => void): void {
    if (!this.ctx) {
      this.ctx = new AudioContext();
      this.gain = this.ctx.createGain();
      this.gain.connect(this.ctx.destination);
    }
    if (this.ctx.state === "suspended") void this.ctx.resume();
    if (!this.loggedOnce) {
      this.loggedOnce = true;
      onFirstUnlock(
        "sound: same samples the firmware plays, judge the tune here; the real speaker is tiny and will change how it sounds, judge that on hardware"
      );
    }
  }

  // Called once per tick, right after emu_tick(): compares both counters
  // against what was last seen and acts on a change, per emu_abi.h.
  poll(emu: EmuExports): void {
    if (!emu.emu_sound_play_seq || !emu.emu_sound_stop_seq) return; // no sound exported: not every device has one

    const stopSeq = emu.emu_sound_stop_seq();
    if (stopSeq !== this.lastStopSeq) {
      this.lastStopSeq = stopSeq;
      // Dismissal must cut the sound immediately, not let an
      // already-started buffer finish (emu_abi.h's "immediate" claim).
      this.stopCurrent();
    }

    const playSeq = emu.emu_sound_play_seq();
    if (playSeq !== this.lastPlaySeq) {
      this.lastPlaySeq = playSeq;
      this.play(emu);
    }
  }

  // Called once right after a module (re)load, before any tick runs. A
  // fresh module's counters do not continue where the previous one left
  // off, so without this, a reload would read "the new module's counter
  // differs from what I last saw" as a real play event and fire the old
  // module's stale sound. Also stops whatever was still audibly playing
  // from the module that just got replaced.
  resetForReload(emu: EmuExports): void {
    this.stopCurrent();
    this.lastPlaySeq = emu.emu_sound_play_seq ? emu.emu_sound_play_seq() : -1;
    this.lastStopSeq = emu.emu_sound_stop_seq ? emu.emu_sound_stop_seq() : -1;
  }

  toggleMute(): boolean {
    this.muted = !this.muted;
    if (this.gain) this.gain.gain.value = this.muted ? 0 : 1;
    return this.muted;
  }

  private stopCurrent(): void {
    if (this.currentNode) {
      try {
        this.currentNode.stop();
      } catch {
        // already stopped or never started; nothing to do
      }
      this.currentNode = null;
    }
  }

  private play(emu: EmuExports): void {
    // No context yet (no gesture has happened) or still suspended: this
    // sound event is simply not played, and status reads "suspended" so
    // the page says so rather than looking like silence for no reason.
    // Queuing it to fire later would risk playing a stale alarm out of
    // context once a gesture finally arrives, which is worse than skipping it.
    if (!this.ctx || !this.gain || this.ctx.state === "suspended") return;
    if (!emu.emu_sound_sample_rate || !emu.emu_sound_buffer || !emu.emu_sound_frames) return;

    const frameCount = emu.emu_sound_frames();
    if (frameCount <= 0) return;
    const sampleRate = emu.emu_sound_sample_rate();
    const framesPtr = emu.emu_sound_buffer();

    // int16, native byte order, per emu_abi.h; divide by 32768 to reach
    // the [-1, 1) float range Web Audio wants.
    const int16 = new Int16Array(emu.memory.buffer, framesPtr, frameCount);
    const buffer = this.ctx.createBuffer(1, frameCount, sampleRate);
    const channel = buffer.getChannelData(0);
    for (let i = 0; i < frameCount; i++) channel[i] = int16[i]! / 32768;

    this.stopCurrent(); // a new phrase starting replaces whatever was still playing
    const node = this.ctx.createBufferSource();
    node.buffer = buffer;
    node.connect(this.gain);
    node.addEventListener("ended", () => {
      if (this.currentNode === node) this.currentNode = null;
    });
    node.start();
    this.currentNode = node;
  }
}
