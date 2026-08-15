// One loader for the compiled firmware module, and one way to drive it.
//
// Every file under emulator/wasm/tests/ currently carries its own copy of
// loadDevice(): the same nine math imports, the same js_log, the same
// emu_init() check. That duplication is why bug 8 (a stale module) and bug
// 7 (clean input where the hardware is filthy) could be true of some tests
// and not others - there was no single place where either could be decided.
// This is that place. Tests are welcome to import it; the gate requires it.
//
// It deliberately does NOT hide the clock. emu_tick(nowMs) taking the
// host's time is the property that makes any of this reproducible
// (emu_abi.h), so the caller always says what time it is.
import { readFileSync } from "node:fs";
import { assertFresh, WASM_PATH } from "./freshness";

export interface Rect { x: number; y: number; w: number; h: number }

export interface DeviceDescriptor {
  name: string;
  panel: { w: number; h: number; format: string };
  buttons: { id: string; label: string; edge: string; at: number; longPressMs?: number }[];
  touch?: { points: number };
  sensors?: { id: string; kind: string }[];
  apps?: string[];
  gestures?: unknown[];
  tunables?: { id: string; min: number; max: number; default: number }[];
}

export interface Device {
  descriptor: DeviceDescriptor;
  panelW: number;
  panelH: number;
  tick(nowMs: number): void;
  /**
   * The raw ABI call. Named `touchRaw` rather than `touch` on purpose: a
   * gesture assembled out of these is a gesture the real controller never
   * produced. Use TouchDriver (touchsim.ts) unless there is a stated reason
   * not to - see cleaninput.ts.
   */
  touchRaw(down: boolean, x: number, y: number): void;
  button(index: number, down: boolean): void;
  buttonVerdict(index: number, isLong: boolean): void;
  sensorEvent(index: number): void;
  appCurrent(): number;
  appSwitch(index: number): void;
  /** A live view into the module's memory. Copy it if you need to keep it. */
  fb(): Uint16Array;
  pushCount(): number;
  pushes(): Rect[];
  /** Total pixels this tick's pushes covered, counted without allocating. */
  pushedPixels(): number;
  drainLog(): string[];
  tuneGet(index: number): number;
  tuneSet(index: number, value: number): void;
  /** Arena bytes the CURRENT app allocated in enter() (emu_abi.h). */
  arenaUsed(): number;
  arenaCapacity(): number;
}

export const BTN_BOOT = 0;
export const BTN_PWR = 1;

let cachedModule: WebAssembly.Module | null = null;

/**
 * The compiled (not instantiated) module, shared with checks that read the
 * module's own surface rather than driving it - the gate's ABI-parity rule
 * reads WebAssembly.Module.exports() from this, so it inspects the exact
 * bytes every other rule runs, not a second compile of possibly-different
 * bytes.
 */
export function compiledModule(): WebAssembly.Module {
  if (cachedModule === null) cachedModule = new WebAssembly.Module(readFileSync(WASM_PATH));
  return cachedModule;
}

/**
 * Compiles the module once per process and instantiates a fresh one per
 * call - a device is single-use state (arena, framebuffer, app index), so
 * every scenario gets its own, and only the expensive half is shared.
 *
 * Refuses outright if the module is older than its sources (bug 8). Pass
 * `skipFreshness` only from a tool that has already checked.
 */
export async function loadDevice(opts: { skipFreshness?: boolean } = {}): Promise<Device> {
  if (!opts.skipFreshness) assertFresh();
  if (cachedModule === null) cachedModule = compiledModule();

  let memory!: WebAssembly.Memory;
  const decoder = new TextDecoder();
  const fwLog: string[] = [];

  const instance = await WebAssembly.instantiate(cachedModule, {
    env: {
      js_log(ptr: number, len: number) {
        fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len)));
      },
      sinf: Math.sin,
      cosf: Math.cos,
      atan2f: Math.atan2,
      sqrtf: Math.sqrt,
      fabsf: Math.abs,
      floorf: Math.floor,
      fmodf: (x: number, y: number) => x % y,
      powf: Math.pow,
      expf: Math.exp,
    },
  });

  memory = instance.exports.memory as WebAssembly.Memory;
  const exp = instance.exports as Record<string, CallableFunction>;

  const readCString = (ptr: number): string => {
    const bytes = new Uint8Array(memory.buffer);
    let end = ptr;
    while (end < bytes.length && bytes[end] !== 0) end++;
    return decoder.decode(bytes.subarray(ptr, end));
  };

  const descriptor = JSON.parse(readCString(exp.emu_device!() as number)) as DeviceDescriptor;
  if ((exp.emu_init!() as number) !== 1) {
    throw new Error(`emu_init() failed:\n${fwLog.join("\n")}`);
  }

  const panelW = descriptor.panel.w;
  const panelH = descriptor.panel.h;
  const fbWords = panelW * panelH;

  return {
    descriptor,
    panelW,
    panelH,
    tick: (nowMs) => { exp.emu_tick!(nowMs >>> 0); },
    touchRaw: (down, x, y) => { exp.emu_touch!(down ? 1 : 0, Math.round(x), Math.round(y)); },
    button: (i, down) => { exp.emu_button!(i, down ? 1 : 0); },
    buttonVerdict: (i, isLong) => { exp.emu_button_verdict!(i, isLong ? 1 : 0); },
    sensorEvent: (i) => { exp.emu_sensor_event!(i); },
    appCurrent: () => exp.emu_app_current!() as number,
    appSwitch: (i) => { exp.emu_app_switch!(i); },
    // Re-derived every call: a wasm memory can grow, which detaches any
    // previously handed-out view.
    fb: () => new Uint16Array(memory.buffer, exp.emu_fb!() as number, fbWords),
    pushCount: () => exp.emu_push_count!() as number,
    pushes: () => {
      const n = exp.emu_push_count!() as number;
      const out: Rect[] = [];
      for (let i = 0; i < n; i++) {
        out.push({
          x: exp.emu_push_x!(i) as number,
          y: exp.emu_push_y!(i) as number,
          w: exp.emu_push_w!(i) as number,
          h: exp.emu_push_h!(i) as number,
        });
      }
      return out;
    },
    pushedPixels: () => {
      const n = exp.emu_push_count!() as number;
      let total = 0;
      for (let i = 0; i < n; i++) total += (exp.emu_push_w!(i) as number) * (exp.emu_push_h!(i) as number);
      return total;
    },
    drainLog: () => { const out = fwLog.slice(); fwLog.length = 0; return out; },
    tuneGet: (i) => exp.emu_tune_get!(i) as number,
    tuneSet: (i, v) => { exp.emu_tune_set!(i, v); },
    arenaUsed: () => exp.emu_arena_used!() as number,
    arenaCapacity: () => exp.emu_arena_capacity!() as number,
  };
}
