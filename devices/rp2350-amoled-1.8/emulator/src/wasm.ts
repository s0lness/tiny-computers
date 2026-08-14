// Loads and instantiates a firmware compiled to WebAssembly, per
// emulator/wasm/emu_abi.h. Nothing here names a specific device: the panel
// size, buttons, sensors and apps all come from the module's own
// emu_device() JSON, read after instantiation (see readDeviceDescriptor
// below and chrome.ts, which builds the page from it).
//
// The wasm URL is a parameter, not a constant baked in here, on purpose:
// this is a local dev tool (a firmware author runs it against their own
// build), not a hosted page that takes an upload, but keeping the URL a
// parameter costs nothing and avoids closing that door later.

export interface EmuExports {
  memory: WebAssembly.Memory;
  emu_device(): number;
  emu_init(): number;
  emu_tick(nowMs: number): void;
  emu_fb(): number;
  emu_push_count(): number;
  emu_push_x(i: number): number;
  emu_push_y(i: number): number;
  emu_push_w(i: number): number;
  emu_push_h(i: number): number;
  emu_touch(down: number, x: number, y: number): void;
  emu_button(index: number, down: number): void;
  emu_button_verdict(index: number, isLong: number): void;
  emu_sensor_event(index: number): void;
  // Optional: only present when the firmware declared an "apps" array in
  // its device descriptor (emu_abi.h, "the emulator will not call them"
  // otherwise).
  emu_app_current?(): number;
  emu_app_switch?(index: number): void;
  // Live tunables (see DeviceDescriptor.tunables and emu_abi.h's "optional:
  // live tunables" section). Optional, same reasoning as emu_app_current/
  // emu_app_switch: a firmware with nothing tunable omits these and the
  // emulator builds no tuning panel.
  emu_tune_get?(index: number): number;
  emu_tune_set?(index: number, value: number): void;
  // Sound: two counters to diff against what was last seen, not a call the
  // host makes (sound is an output, driven by firmware logic like the
  // timer's alarm, never by the host directly). See emu_abi.h's "sound"
  // section and audio.ts, which does the diffing. Optional, same reasoning
  // as emu_app_current/emu_app_switch above: a device with no sound simply
  // does not export these, and audio.ts must not assume every firmware has
  // a speaker.
  emu_sound_sample_rate?(): number;
  emu_sound_play_seq?(): number;
  emu_sound_stop_seq?(): number;
  emu_sound_buffer?(): number;
  emu_sound_frames?(): number;
}

export interface DeviceButton {
  id: string;
  label: string;
  edge: "left" | "right" | "top" | "bottom";
  at: number; // 0..1 along that edge, 0 = top (left/right edges) or left (top/bottom edges)
  longPressMs?: number;
}

export interface DeviceSensor {
  id: string;
  kind: string; // "event" is the only kind emu_abi.h currently defines
  label?: string;
}

// A development-only live-tuning knob (emu_abi.h's "tunables" section).
// Index in the declared array is how emu_tune_get/emu_tune_set address it,
// the same convention DeviceButton uses against emu_button().
export interface DeviceTunable {
  id: string;
  min: number;
  max: number;
  default: number;
}

// emu_device()'s JSON also carries a "gestures" array (the BOOT+PWR chord's
// own prose, per emu_abi.h) that nothing on this page reads any more: the
// sidebar's gesture disclosure is gone (btnChord in the bottom bar performs
// the chord directly, see main.ts's performChord), so there is no consumer
// left to type that field for. The emulator (emu_shim.c) still emits it;
// readDeviceDescriptor below simply ignores whatever extra keys it does not
// know about, so that is harmless.
export interface DeviceDescriptor {
  name?: string;
  panel: { w: number; h: number; format: string };
  buttons?: DeviceButton[];
  touch?: { points?: number };
  sensors?: DeviceSensor[];
  apps?: string[];
  tunables?: DeviceTunable[];
}

export const DEFAULT_WASM_URL = "wasm/emu.wasm";

// The nine math functions and the one logging call emu_abi.h documents as
// what a freestanding module imports, under the "env" module namespace the
// header's own import list is written against. expf joined the list for
// the alarm chime's decay envelope (sound_synth.c) and is not needed by
// anything drawn before sound existed. If the actual build uses different
// names, a different namespace, or asks for an import not listed here,
// WebAssembly.instantiate throws naming exactly what it asked for and
// didn't get; that error is rethrown verbatim by loadEmuModule rather than
// swallowed, which is the mechanism emu_abi.h itself names for reconciling
// the two sides.
function buildImportObject(onLog: (text: string) => void, getMemory: () => WebAssembly.Memory | undefined): WebAssembly.Imports {
  const readString = (ptr: number, len: number): string => {
    const memory = getMemory();
    if (!memory) return "";
    return new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len));
  };
  return {
    env: {
      sinf: Math.sin,
      cosf: Math.cos,
      atan2f: Math.atan2,
      sqrtf: Math.sqrt,
      fabsf: Math.abs,
      floorf: Math.floor,
      fmodf: (a: number, b: number) => a % b,
      powf: Math.pow,
      expf: Math.exp,
      js_log: (ptr: number, len: number) => onLog(readString(ptr, len)),
    },
  };
}

// Fetches the module's raw bytes. Kept separate from instantiate() so a
// caller (main.ts, for replay / hot-reload) can re-instantiate a fresh
// instance from the SAME bytes without hitting the network again, which
// matters for two different reasons: replay wants a byte-identical restart,
// and re-fetching mid-rebuild risks reading a half-written file.
export async function fetchWasmBytes(url: string): Promise<ArrayBuffer> {
  let res: Response;
  try {
    res = await fetch(url);
  } catch (err) {
    throw new Error(`could not reach ${url}: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!res.ok) {
    throw new Error(
      `wasm module not found at ${url} (HTTP ${res.status}). This half of the project builds separately ` +
        `(see emulator/wasm/emu_abi.h); build it, or point the watcher at it, then reload.`
    );
  }
  return res.arrayBuffer();
}

// The wasm magic bytes ("\0asm"), checked before ever attempting to compile.
// The dev server debounces its own reload broadcast until the file has
// stopped changing (server.ts, waitForStableFile), but that is a second
// line of defence, not a substitute for this one: this is the only check
// that runs no matter how the bytes got here (a stale cache, a proxy that
// truncated a response, a build tool the server doesn't watch). A bad
// magic-byte read gets a specific, actionable message instead of whatever
// generic parse error WebAssembly.compile would otherwise throw.
function hasWasmMagic(bytes: ArrayBuffer): boolean {
  if (bytes.byteLength < 8) return false;
  const b = new Uint8Array(bytes, 0, 4);
  return b[0] === 0x00 && b[1] === 0x61 && b[2] === 0x73 && b[3] === 0x6d;
}

// Instantiates a fresh instance from already-fetched bytes. onLog receives
// every env.js_log() call, decoded to a string.
//
// Validates before touching anything the caller already has running:
// magic bytes, then WebAssembly.instantiate (compile + link against the
// real import object), then (in main.ts's bringUp) emu_init() and the
// device descriptor. Every one of those can fail on a half-written or
// incompatible module, and the whole point of checking all of them BEFORE
// main.ts swaps its `emu` reference is that a failure here must never tear
// down a module that was working.
export async function instantiate(bytes: ArrayBuffer, onLog: (text: string) => void): Promise<EmuExports> {
  if (!hasWasmMagic(bytes)) {
    throw new Error(
      `not a valid wasm module: bad magic bytes (${bytes.byteLength} bytes read). ` +
        `This usually means the file was read mid-rebuild; wait a moment and retry.`
    );
  }

  let memory: WebAssembly.Memory | undefined;
  const importObject = buildImportObject(onLog, () => memory);

  let result: WebAssembly.WebAssemblyInstantiatedSource;
  try {
    result = await WebAssembly.instantiate(bytes, importObject);
  } catch (err) {
    // Rethrown as-is: the message names the missing import (module.field),
    // which is exactly what needs to reach the page (see file header).
    throw err instanceof Error ? err : new Error(String(err));
  }

  const exports = result.instance.exports as unknown as EmuExports;
  memory = exports.memory;
  if (!memory) {
    throw new Error(
      "wasm module has no exported 'memory'; the emulator reads the framebuffer and emu_device() through it."
    );
  }
  return exports;
}

export function readCString(memory: WebAssembly.Memory, ptr: number): string {
  const bytes = new Uint8Array(memory.buffer);
  let end = ptr;
  while (end < bytes.length && bytes[end] !== 0) end++;
  return new TextDecoder().decode(bytes.subarray(ptr, end));
}

// Reads and parses emu_device(). Thrown errors are meant to be shown
// verbatim on the page: an unparsable descriptor or a missing "panel" is a
// firmware-side bug worth seeing immediately, not a blank screen.
export function readDeviceDescriptor(emu: EmuExports): DeviceDescriptor {
  const ptr = emu.emu_device();
  const text = readCString(emu.memory, ptr);
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch (err) {
    throw new Error(`emu_device() returned invalid JSON: ${err instanceof Error ? err.message : String(err)}\n${text}`);
  }
  const d = parsed as Partial<DeviceDescriptor>;
  if (!d.panel || typeof d.panel.w !== "number" || typeof d.panel.h !== "number" || typeof d.panel.format !== "string") {
    throw new Error(`emu_device() is missing a valid "panel" (w, h, format): ${text}`);
  }
  return d as DeviceDescriptor;
}
