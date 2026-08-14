// Deterministic replay of a recorded trace (see recorder.ts): re-issues the
// exact same sequence of ABI calls, in the same order, with the exact same
// emu_tick timestamps the original session used. Since emu_tick's nowMs is
// the module's only clock, replaying the same (call, timestamp) sequence
// against a freshly booted module reproduces the same behaviour, PROVIDED
// the firmware itself has no other source of nondeterminism (a global PRNG
// seeded from wall-clock time would break this; nothing in the ABI hands
// one over, which is deliberate: see emu_abi.h's import list, which is
// eight math functions and a logger, nothing random).
//
// What this file deliberately does NOT do: rewind. A ring buffer of recent
// framebuffer/state snapshots to step backward through would be the natural
// next feature, but it was called out as a later pass rather than something
// to half-build, so replay here is forward-only (reset-and-step-forward,
// which for a short recent trace is cheap enough to feel instant anyway).

import type { EmuExports } from "./wasm";
import type { TraceEvent } from "./recorder";

export class Replayer {
  private events: TraceEvent[];
  private index = 0;

  constructor(events: TraceEvent[]) {
    this.events = events;
  }

  get done(): boolean {
    return this.index >= this.events.length;
  }

  get progress(): { at: number; total: number } {
    return { at: this.index, total: this.events.length };
  }

  reset(): void {
    this.index = 0;
  }

  // Applies events in order until (and including) the next "tick", then
  // stops: one frame of replay. Returns the tick's timestamp, or null if
  // the trace ran out before another tick.
  stepFrame(emu: EmuExports): number | null {
    while (this.index < this.events.length) {
      const ev = this.events[this.index++]!;
      switch (ev.k) {
        case "touch":
          emu.emu_touch(ev.down, ev.x, ev.y);
          break;
        case "button":
          emu.emu_button(ev.i, ev.down);
          break;
        case "verdict":
          emu.emu_button_verdict(ev.i, ev.long);
          break;
        case "sensor":
          emu.emu_sensor_event(ev.i);
          break;
        case "sensorv":
          // Optional export (a firmware with no vector sensor omits it), so
          // a trace recorded against one module and replayed against
          // another that lacks it is skipped rather than crashing the
          // replay - same tolerance emu_app_switch already gets.
          emu.emu_sensor_vector?.(ev.i, ev.x, ev.y, ev.z);
          break;
        case "tick":
          emu.emu_tick(ev.t);
          return ev.t;
      }
    }
    return null;
  }
}
