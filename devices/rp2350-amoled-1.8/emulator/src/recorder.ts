// Records every ABI input call and every tick timestamp, in order, into a
// trace. This is the foundation the owner asked debugging to be built on:
// emu_tick(nowMs) is the guest's only clock (emu_abi.h), so a session is
// fully described by the ordered sequence of (call, timestamp) pairs that
// were made against it. Replaying that same sequence against a freshly
// booted module (see replay.ts) reproduces the same behaviour, which turns
// a bug that only shows up "sometimes, while drawing fast" into a file that
// reproduces it on demand.

import { TRACE_MAX_EVENTS } from "./constants";
import type { DeviceDescriptor } from "./wasm";

export type TraceEvent =
  | { t: number; k: "touch"; down: number; x: number; y: number }
  | { t: number; k: "button"; i: number; down: number }
  | { t: number; k: "verdict"; i: number; long: number }
  | { t: number; k: "sensor"; i: number }
  | { t: number; k: "tick" };

export interface Trace {
  schemaVersion: 1;
  recordedAt: string;
  device: DeviceDescriptor;
  events: TraceEvent[];
}

export class Recorder {
  events: TraceEvent[] = [];
  enabled = true;

  record(ev: TraceEvent): void {
    if (!this.enabled) return;
    this.events.push(ev);
    // A ring buffer, not an ever-growing array: bounds memory in a session
    // left running, at the cost of only keeping the most recent history
    // (see constants.ts for what that cap actually covers in wall time).
    if (this.events.length > TRACE_MAX_EVENTS) this.events.shift();
  }

  recent(n: number): TraceEvent[] {
    return this.events.slice(-n);
  }

  clear(): void {
    this.events.length = 0;
  }

  toTrace(device: DeviceDescriptor): Trace {
    return {
      schemaVersion: 1,
      recordedAt: new Date().toISOString(),
      device,
      events: this.events.slice(),
    };
  }
}
