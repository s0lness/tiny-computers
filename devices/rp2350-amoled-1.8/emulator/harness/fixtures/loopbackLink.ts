// A software stand-in for the real board's devlink firmware
// (firmware/devlink.c), speaking the same line protocol
// (tools/README-devlink.md) over an in-process queue instead of a real USB
// CDC serial port.
//
// Why this exists: the owner is using the real board right now, and
// "never open the real serial port during development or testing" is a
// hard constraint on this task - but the whole second half of the task
// (detection, the tuning panel's device column, console tagging) is
// untestable without SOMETHING that answers like a board. This is that
// something: a Bridge-shaped (tools/dev.ts) fake that DevlinkHost
// (emulator/devlink-host.ts) can hold in "fake" mode exactly where it would
// otherwise hold a real PowerShell serial bridge, so every consumer above
// it (the WS relay, the page's status indicator and console tagging, the
// tuning panel, dev.ts's own server-routing) is exercised for real, with no
// hardware and no PowerShell involved at any point.
//
// This is a fake of the WIRE PROTOCOL, not a simulation of the firmware's
// behaviour: it does not run sketch.c, does not track strokes or dropouts,
// and its TUNE state is just a plain map matching the reply shapes
// devlink_dispatch() produces. Anything beyond "does the host-side code
// handle this reply shape, and this shared-port noise, correctly" needs the
// real device - see tools/README-devlink.md's own "what injection cannot
// test" for the same distinction drawn between KEY/BOOT/CHORD and real
// button hardware.

import type { LineSource } from "../../../tools/dev";

export interface FakeTunableSpec {
  name: string;
  min: number;
  max: number;
  default: number;
}

// Mirrors firmware/apps/sketch.c's g_sketchTunables exactly (name, min,
// max, default) - the "happy path" scenario, where the emulator's wasm
// build and the fake board declare the identical set.
export const SKETCH_TUNABLES: FakeTunableSpec[] = [
  { name: "lift", min: 20, max: 1000, default: 220 },
  { name: "confirm", min: 0, max: 500, default: 40 },
  { name: "pendgrace", min: 20, max: 1000, default: 80 },
  { name: "minjump", min: 0, max: 300, default: 40 },
  { name: "maxjump", min: 10, max: 368, default: 150 },
  { name: "maxspeed", min: 0.5, max: 30, default: 6 },
];

// Deliberately NOT the same set as SKETCH_TUNABLES: drops "pendgrace",
// renames "maxspeed" to "speedcap", and adds "jitter" - a firmware build
// that has drifted from the emulator's own wasm build, which is the edge
// case the task calls out explicitly: "the emulator and the board can be
// running different builds, so their knob lists can differ. A knob present
// on only one side must show as such, not vanish silently."
export const MISMATCHED_TUNABLES: FakeTunableSpec[] = [
  { name: "lift", min: 20, max: 1000, default: 220 },
  { name: "confirm", min: 0, max: 500, default: 40 },
  { name: "minjump", min: 0, max: 300, default: 40 },
  { name: "maxjump", min: 10, max: 368, default: 150 },
  { name: "speedcap", min: 0.5, max: 30, default: 6 },
  { name: "jitter", min: 0, max: 100, default: 12 },
];

export interface LoopbackLinkOptions {
  tunables?: FakeTunableSpec[];
  panel?: { w: number; h: number };
  // Emits a harmless noise line on this interval (ms), mirroring the real
  // board's once-a-second profiler tick sharing the same port, so a long-
  // running test against this fixture (as opposed to a scripted
  // injectNoise() call) also exercises the noise-tolerant shape matching
  // under realistic, unscripted interleaving. 0 (the default) disables it -
  // most callers want deterministic behaviour, not a timer racing their
  // assertions.
  noiseIntervalMs?: number;
}

// Same decimal formatting devlink.c's devlink_print_tune_value() uses:
// whole numbers as-is, otherwise one fractional digit.
function fmt(v: number): string {
  const tenths = Math.round(v * 10);
  const whole = Math.trunc(tenths / 10);
  const frac = Math.abs(tenths % 10);
  return frac === 0 ? `${whole}` : `${whole}.${frac}`;
}

// Bridge-shaped (tools/dev.ts's Bridge: { lines, send, close }): this
// handle IS a LineSource itself (readLine), so devlink-host.ts (in "fake"
// mode) builds the Bridge as `{ lines: link, send: (c) => link.send(c),
// close: () => link.close() }` where `link` is exactly this handle.
export interface LoopbackLinkHandle extends LineSource {
  send(cmd: string): Promise<void>;
  close(): Promise<void>;
  // Test-only seam, not part of the Bridge shape: pushes a line as if the
  // board had sent it unprompted, for deterministically testing that a
  // consumer skips noise rather than mistaking it for the reply it is
  // waiting on. See harness/selftest.ts.
  injectNoise(text: string): void;
}

export function createLoopbackLink(opts: LoopbackLinkOptions = {}): LoopbackLinkHandle {
  const tunables = opts.tunables ?? SKETCH_TUNABLES;
  const panel = opts.panel ?? { w: 368, h: 448 };
  const values = new Map<string, number>(tunables.map((t) => [t.name, t.default]));

  const queue: string[] = [];
  const waiters: ((v: string | null) => void)[] = [];
  let closed = false;

  function emit(line: string): void {
    if (closed) return;
    const w = waiters.shift();
    if (w) w(line);
    else queue.push(line);
  }

  let noiseTimer: ReturnType<typeof setInterval> | null = null;
  const noiseMs = opts.noiseIntervalMs ?? 0;
  if (noiseMs > 0) {
    noiseTimer = setInterval(() => emit(`prof loop (fake board) t=${Date.now()}`), noiseMs);
  }

  function tuneList(): void {
    for (const t of tunables) emit(`TUNE ${t.name} ${fmt(values.get(t.name)!)} ${fmt(t.min)} ${fmt(t.max)} ${fmt(t.default)}`);
    emit("END");
  }
  function tuneGet(name: string): void {
    const t = tunables.find((x) => x.name === name);
    if (!t) { emit(`ERR unknown ${name}`); return; }
    emit(`TUNE ${name} ${fmt(values.get(name)!)}`);
  }
  function tuneSet(name: string, raw: string): void {
    const t = tunables.find((x) => x.name === name);
    if (!t) { emit(`ERR unknown ${name}`); return; }
    const v = Number(raw);
    if (!Number.isFinite(v)) { emit("ERR args"); return; }
    const applied = Math.min(t.max, Math.max(t.min, v));
    values.set(name, applied);
    emit(`TUNE ${name} ${fmt(applied)}`);
  }
  function tuneResetOne(name: string): boolean {
    const t = tunables.find((x) => x.name === name);
    if (!t) { emit(`ERR unknown ${name}`); return false; }
    const previous = values.get(name)!;
    values.set(name, t.default);
    emit(`TUNE ${name} ${fmt(t.default)} ${fmt(previous)}`);
    return true;
  }
  function tuneResetAll(): void {
    if (tunables.length === 0) { emit("ERR no tunables"); return; }
    for (const t of tunables) tuneResetOne(t.name);
    emit("END");
  }

  function dispatch(rawLine: string): void {
    const trimmed = rawLine.trim();
    if (trimmed.length === 0) return;
    const sp = trimmed.indexOf(" ");
    const cmd = (sp < 0 ? trimmed : trimmed.slice(0, sp)).toUpperCase();
    const rest = sp < 0 ? "" : trimmed.slice(sp + 1).trim();

    switch (cmd) {
      case "PING":
        emit(`OK devlink 1 ${panel.w} ${panel.h}`);
        return;
      case "APP":
        emit("APP 0 chrono");
        return;
      case "SWITCH": {
        const idx = Number(rest);
        emit(Number.isFinite(idx) && idx >= 0 && idx < 3 ? "OK" : "ERR range");
        return;
      }
      case "DOWN":
      case "MOVE":
      case "UP":
      case "TAP":
      case "ERASE":
      case "KEY":
      case "BOOT":
      case "CHORD":
        // Not exercised by this task's console/tuning work; answered the
        // same way a real board answers a well-formed one, so a script
        // driven against the fake at least sees the right shape.
        emit("OK");
        return;
      case "TUNE": {
        if (tunables.length === 0) { emit("ERR no tunables"); return; }
        if (rest.length === 0) { tuneList(); return; }
        const parts = rest.split(/\s+/);
        const sub = parts[0].toUpperCase();
        if (sub === "GET") { tuneGet(parts[1] ?? ""); return; }
        if (sub === "SET") { tuneSet(parts[1] ?? "", parts[2] ?? ""); return; }
        if (sub === "RESET") {
          if (parts[1]) tuneResetOne(parts[1]);
          else tuneResetAll();
          return;
        }
        if (sub === "FREEZE") {
          for (const t of tunables) emit(`#define ${t.name.toUpperCase()}_DEFAULT ${fmt(values.get(t.name)!)}f`);
          emit("END");
          return;
        }
        emit("ERR args");
        return;
      }
      default:
        emit(`ERR unknown ${cmd}`);
    }
  }

  return {
    readLine(): Promise<string | null> {
      if (queue.length > 0) return Promise.resolve(queue.shift()!);
      if (closed) return Promise.resolve(null);
      return new Promise((resolve) => waiters.push(resolve));
    },
    async send(cmd: string): Promise<void> {
      dispatch(cmd);
    },
    async close(): Promise<void> {
      closed = true;
      if (noiseTimer) clearInterval(noiseTimer);
      while (waiters.length) waiters.shift()!(null);
    },
    injectNoise(text: string): void {
      emit(text);
    },
  };
}
