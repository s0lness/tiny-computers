// repro-breakout-residue: the standing guard on breakout's drawing, against
// the residue class this project keeps paying for, PLUS a pin on the one
// property its whole game loop is built to have: the fixed-timestep clock
// (breakout.c's own header comment, "THE GAME LOOP") makes the picture at
// any given nowMs a pure function of nowMs, independent of how many real
// tick() calls it took to get there. Run:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/repro-breakout-residue.ts
//
// Breakout is in the default app table unconditionally, the way every real
// app on this device is now - see feature-breakout.ts's header for why no
// build flag is needed.
//
// THREE PROPERTIES, all measured on the framebuffer and the real push
// windows, never on an internal - the same convention every repro-*-
// residue.ts file in this directory uses, and the same reason: anything
// that fails here is something a person looking at the device could also
// have seen.
//
//   1. Every pixel that CHANGES during a tick lies inside a rectangle that
//      tick pushed. A pixel written outside a pushed window is correct in
//      memory and wrong on the glass.
//   2. Every pushed window's row length is a multiple of 8 pixels
//      (docs/decisions/0001).
//   3. TWO FRESH DEVICES, driven through the EXACT SAME sequence of
//      absolute nowMs values with no touch and no tilt, land on
//      BIT-IDENTICAL framebuffers. This is not a residue check, it is the
//      determinism claim breakout.c's header comment makes about the whole
//      point of a fixed-timestep accumulator: nothing in the simulation may
//      depend on anything but nowMs (not a hidden host-clock read, not
//      leftover state from a previous run, not float accumulation order).
//      A ball that is even slightly path-dependent would still look right
//      in any one run and would fail this the moment two runs are compared.
//
// Plus a work budget per frame - the emulator cannot see time (decision
// 0003) and this device has already rebooted itself through the watchdog
// once by redrawing too much (the sketchpad's palette). Pixels pushed is
// the proxy a headless run can actually measure; tools/gate/run.ts
// --measure's own line for this app (peak 3504px/tick, 0.02 panels) is
// where the number below comes from, generously rounded up.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const PANEL_PX = PANEL_W * PANEL_H;
let APP_BREAKOUT = -1;
const FRAME_MS = 16;

// Generous over the gate's own measured peak (3504px), since this file
// drives longer and rougher sequences than the gate's stimulus list -
// still two orders of magnitude under the palette-bug shape the gate's own
// budget (3 full panels) exists to catch.
const MAX_TICK_PUSH_PIXELS = 20000;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
    env: {
      js_log(_p: number, _l: number) { /* quiet: this file drives thousands of frames */ },
      sinf: (x: number) => Math.sin(x),
      cosf: (x: number) => Math.cos(x),
      atan2f: (y: number, x: number) => Math.atan2(y, x),
      sqrtf: (x: number) => Math.sqrt(x),
      fabsf: (x: number) => Math.abs(x),
      floorf: (x: number) => Math.floor(x),
      fmodf: (x: number, y: number) => x % y,
      powf: (x: number, y: number) => Math.pow(x, y),
      expf: (x: number) => Math.exp(x),
    },
  });
  memory = inst.exports.memory as WebAssembly.Memory;
  const e = inst.exports as any;
  if (e.emu_init() !== 1) throw new Error("emu_init() failed");
  return {
    e,
    tick(nowMs: number) { e.emu_tick(nowMs); },
    tilt(x: number, y: number, z: number) { e.emu_sensor_vector(1, x, y, z); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_PX * 2).slice(); },
    apps(): string[] {
      const ptr = e.emu_device();
      const bytes = new Uint8Array(memory.buffer, ptr);
      let end = 0; while (bytes[end] !== 0) end++;
      return JSON.parse(dec.decode(bytes.subarray(0, end))).apps || [];
    },
    pushes(): { x: number; y: number; w: number; h: number }[] {
      const out = [];
      for (let i = 0; i < e.emu_push_count(); i++) {
        out.push({ x: e.emu_push_x(i), y: e.emu_push_y(i), w: e.emu_push_w(i), h: e.emu_push_h(i) });
      }
      return out;
    },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

type Violation = { px: number; py: number };

function auditedTick(dev: Device, t: number, acc: {
  violations: Violation[];
  badRowLen: number;
  maxPushPx: number;
  frames: number;
  budgetBreaches: number;
}): void {
  const before = dev.fb();
  dev.tick(t);
  const after = dev.fb();
  const rects = dev.pushes();

  let pushPx = 0;
  for (const r of rects) {
    if (r.w % 8 !== 0) acc.badRowLen++;
    pushPx += r.w * r.h;
  }
  acc.frames++;
  if (pushPx > acc.maxPushPx) acc.maxPushPx = pushPx;
  if (pushPx > MAX_TICK_PUSH_PIXELS) acc.budgetBreaches++;

  const inside = (px: number, py: number) =>
    rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);

  for (let i = 0; i < PANEL_PX; i++) {
    const b = i * 2;
    if (before[b] === after[b] && before[b + 1] === after[b + 1]) continue;
    const px = i % PANEL_W, py = (i / PANEL_W) | 0;
    if (inside(px, py)) continue;
    if (acc.violations.length < 20) acc.violations.push({ px, py });
  }
}

async function main() {
  console.log("=== repro + regression: breakout draws nothing outside its pushes, and is a pure function of the clock ===\n");

  {
    const probe = await loadDevice();
    APP_BREAKOUT = probe.apps().indexOf("breakout");
    check("the app table carries 'breakout'", APP_BREAKOUT >= 0, `apps=${JSON.stringify(probe.apps())}`);
    if (APP_BREAKOUT < 0) { console.log(`\n${passCount} passed, ${failCount} failed`); process.exit(1); }
  }

  // ---- residue: a long, untouched, untilted run --------------------------
  // No touch, no tilt: the ball, the paddle (which still sits at its
  // default centred rest position with no tilt reading) and the wall are
  // the whole picture, moving on the clock alone.
  {
    const acc = { violations: [] as Violation[], badRowLen: 0, maxPushPx: 0, frames: 0, budgetBreaches: 0 };
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_BREAKOUT);
    let t = FRAME_MS;
    dev.tick(t);
    const TICKS = 4000; // a little over a minute of simulated time
    for (let i = 0; i < TICKS; i++) {
      t += FRAME_MS;
      auditedTick(dev, t, acc);
    }
    check(`no pixel changed outside a pushed window across ${TICKS} frames`,
      acc.violations.length === 0,
      acc.violations.length ? `${acc.violations.length} violations, first ${JSON.stringify(acc.violations[0])}` : "clean");
    check("every pushed window's row length is a multiple of 8 pixels (decision 0001)",
      acc.badRowLen === 0, `${acc.badRowLen} bad windows out of ${acc.frames} audited frames`);
    check(`no single tick pushed more than ${MAX_TICK_PUSH_PIXELS}px`,
      acc.budgetBreaches === 0, `worst tick pushed ${acc.maxPushPx}px, ${acc.budgetBreaches} breach(es)`);
  }

  // ---- residue under tilt: the paddle sweeping back and forth -----------
  {
    const acc = { violations: [] as Violation[], badRowLen: 0, maxPushPx: 0, frames: 0, budgetBreaches: 0 };
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_BREAKOUT);
    let t = FRAME_MS;
    dev.tick(t);
    const TICKS = 1500;
    for (let i = 0; i < TICKS; i++) {
      // A slow sweep, landscape gx from -1 to 1 and back - see
      // feature-breakout.ts for why the panel vector's Y carries landscape
      // gx for a landscape app.
      const phase = (i / TICKS) * 2 * Math.PI;
      const gx = Math.sin(phase);
      dev.tilt(0, gx, Math.sqrt(Math.max(0, 1 - gx * gx)));
      t += FRAME_MS;
      auditedTick(dev, t, acc);
    }
    check(`no pixel changed outside a pushed window while the paddle sweeps, across ${TICKS} frames`,
      acc.violations.length === 0,
      acc.violations.length ? `${acc.violations.length} violations, first ${JSON.stringify(acc.violations[0])}` : "clean");
    check("every pushed window's row length is a multiple of 8 pixels while sweeping",
      acc.badRowLen === 0, `${acc.badRowLen} bad windows out of ${acc.frames} audited frames`);
  }

  // ---- determinism: the fixed-timestep clock's whole point --------------
  // Two INDEPENDENT, freshly-loaded devices, driven through the identical
  // sequence of absolute nowMs values (no touch, no tilt - nothing for
  // either run to disagree about except the physics itself), must land on
  // a bit-identical framebuffer. The nowMs sequence deliberately chops time
  // unevenly (a mix of small and large jumps) rather than a uniform 16ms
  // cadence, because a uniform cadence could pass by accident even with a
  // per-call Euler step; an irregular one is closer to what the settle-
  // cadence gate probe actually exercises.
  {
    const seqMs: number[] = [];
    let t = 0;
    const jumps = [16, 16, 233, 16, 700, 40, 16, 900, 16, 16, 55, 16, 16, 16, 1200, 16, 16, 333];
    for (let rep = 0; rep < 40; rep++) {
      for (const j of jumps) { t += j; seqMs.push(t); }
    }

    async function replay(): Promise<Uint8Array> {
      const dev = await loadDevice();
      dev.tick(0);
      dev.appSwitch(APP_BREAKOUT);
      dev.tick(16);
      for (const nowMs of seqMs) dev.tick(16 + nowMs);
      return dev.fb();
    }

    const a = await replay();
    const b = await replay();
    let diffs = 0, first = -1;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) { diffs++; if (first < 0) first = (i / 2) | 0; }
    }
    check(`two independent runs through the identical ${seqMs.length}-tick nowMs sequence are bit-identical`,
      diffs === 0, diffs ? `${diffs} differing bytes, first at pixel ${first}` : "bit-identical");
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
