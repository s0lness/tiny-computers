// repro-level-bubble-residue: the standing guard on the bubble level's
// drawing, against the one defect class this project keeps paying for. Run:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/repro-level-bubble-residue.ts
//
// The level is in the default app table unconditionally (runtime_core.c,
// docs/decisions/0012) - no build flag needed, see feature-level.ts's
// header for why.
//
// WHY THIS FILE EXISTS BEFORE THE BUG DOES. Every other repro-* file here
// reproduces something that actually happened. This one is written from the
// scar tissue instead: a small object moving over a static background has
// left residue on this device four separate times (the timer ring's black
// dashes, the sketchpad palette's pop-in, the ring-shrink after a snap, and
// most recently an animation whose whiten box was sized for the wrong
// extent), and firmware/apps/level.c is exactly that shape - a dot sliding
// over a dial that never moves. The failure is always the same: the
// rectangle repainted did not fully contain what the previous frame drew,
// or a pixel was written where nothing was pushed, and neither is visible
// until someone photographs the panel.
//
// THREE PROPERTIES, all measured on the framebuffer and the real push
// windows, never on an internal:
//
//   1. Every pixel that CHANGES during a tick lies inside a rectangle that
//      tick pushed. This is the invariant that makes partial refresh legal
//      at all: a pixel written outside a pushed window is correct in memory
//      and wrong on the glass, so it is invisible to a screenshot and
//      visible to a child.
//   2. Every pushed window's row length is a multiple of 8 pixels
//      (docs/decisions/0001). gfx_push enforces it; level.c pre-rounds to
//      it so the two agree exactly, and this checks the result rather than
//      trusting either.
//   3. An incrementally-updated screen is BIT-IDENTICAL to a freshly
//      entered one at the same tilt. This is the strong form of "no
//      residue": it does not ask whether leftovers look bad, it asks
//      whether any leftover exists at all, and it catches a wrongly-sized
//      repaint rectangle on the first frame rather than the fiftieth.
//
// Plus a work budget per frame, because the emulator cannot see time
// (decision 0003) and this device has already rebooted itself through the
// watchdog once by redrawing too much - the palette animation, see
// repro-sketch-palette-watchdog-reset.ts. Pixels pushed is the proxy a
// headless run can actually measure.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const PANEL_PX = PANEL_W * PANEL_H;
// Read from emu_device()'s own app list rather than hardcoded - see
// feature-level.ts on why the index is not a constant here.
let APP_LEVEL = -1;
const FRAME_MS = 16;

// Worst case one frame of this app may push, as a fraction of the panel.
// Derived from level.c's own budget note (two dot-sized boxes of about
// 68x68, or the target ring's own 137x137 box on a verdict change) and then
// confirmed by the measurement this file prints. Deliberately a hard
// number: the point of a budget is that growing past it is a decision
// somebody makes on purpose.
const MAX_FRAME_PUSH_FRACTION = 0.14;

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
    // g is milli-g (this file's own gravityFor()); emu_sensor_vector()
    // wants g, on the shared "gravity" sensor (index 1).
    tilt(g: [number, number, number]) { e.emu_sensor_vector(1, g[0] / 1000, g[1] / 1000, g[2] / 1000); },
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

function gravityFor(deg: number, phiDeg: number): [number, number, number] {
  const t = (deg * Math.PI) / 180, p = (phiDeg * Math.PI) / 180;
  return [1000 * Math.sin(t) * Math.cos(p), 1000 * Math.sin(t) * Math.sin(p), 1000 * Math.cos(t)];
}

type Violation = { px: number; py: number; kind: string };

// Advances one frame with the given tilt and audits it: what changed, what
// was pushed, and whether the first is inside the second.
function auditedTick(dev: Device, t: number, g: [number, number, number], acc: {
  violations: Violation[];
  badRowLen: number;
  maxPushPx: number;
  totalPushPx: number;
  frames: number;
  framesWithPush: number;
}): void {
  const before = dev.fb();
  dev.tilt(g);
  dev.tick(t);
  const after = dev.fb();
  const rects = dev.pushes();

  let pushPx = 0;
  for (const r of rects) {
    if (r.w % 8 !== 0) acc.badRowLen++;
    pushPx += r.w * r.h;
  }
  acc.frames++;
  if (rects.length > 0) acc.framesWithPush++;
  acc.totalPushPx += pushPx;
  if (pushPx > acc.maxPushPx) acc.maxPushPx = pushPx;

  const inside = (px: number, py: number) =>
    rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);

  for (let i = 0; i < PANEL_PX; i++) {
    const b = i * 2;
    if (before[b] === after[b] && before[b + 1] === after[b + 1]) continue;
    const px = i % PANEL_W, py = (i / PANEL_W) | 0;
    if (inside(px, py)) continue;
    if (acc.violations.length < 20) acc.violations.push({ px, py, kind: "changed outside every pushed window" });
  }
}

async function enterLevel(g?: [number, number, number]): Promise<{ dev: Device; t: number }> {
  const dev = await loadDevice();
  if (g) dev.tilt(g); // primes tilt.c's shared filter before the first tick,
                       // so this device's own settle time starts from the
                       // target rather than from the default flat pose
  dev.tick(0);
  dev.appSwitch(APP_LEVEL);
  dev.tick(FRAME_MS);
  if (dev.appCurrent() !== APP_LEVEL) throw new Error("did not land in level");
  return { dev, t: FRAME_MS };
}

function hold(dev: Device, t: number, ms: number, g: [number, number, number]): number {
  const end = t + ms;
  while (t < end) { dev.tilt(g); t += FRAME_MS; dev.tick(t); }
  return t;
}

function fbDiffCount(a: Uint8Array, b: Uint8Array): { n: number; first: number } {
  let n = 0, first = -1;
  for (let i = 0; i < a.length; i++) {
    if (a[i] === b[i]) continue;
    n++;
    if (first < 0) first = (i / 2) | 0;
  }
  return { n, first };
}

async function main() {
  console.log("=== repro + regression: the level's dot must leave nothing behind ===\n");

  {
    const probe = await loadDevice();
    APP_LEVEL = probe.apps().indexOf("level");
    check("the app table carries 'level'", APP_LEVEL >= 0, `apps=${JSON.stringify(probe.apps())}`);
    if (APP_LEVEL < 0) {
      console.log(`\n${passCount} passed, ${failCount} failed`);
      process.exit(1);
    }
  }

  const acc = { violations: [] as Violation[], badRowLen: 0, maxPushPx: 0, totalPushPx: 0, frames: 0, framesWithPush: 0 };

  // ---- motion A: a slow orbit. The dot walks a full circle at 8 degrees,
  // which sweeps it right across the target ring's own band on two sides -
  // the overlap case, where a repaint that forgets what it covered shows up
  // as a bite taken out of the ring. -------------------------------------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, 500, gravityFor(8, 0));
    const STEPS = 120;
    for (let i = 1; i <= STEPS; i++) {
      t += FRAME_MS;
      auditedTick(dev, t, gravityFor(8, (360 * i) / STEPS), acc);
    }
    check("a full orbit changes no pixel outside a pushed window",
      acc.violations.length === 0,
      acc.violations.length ? `${acc.violations.length} violations, first ${JSON.stringify(acc.violations[0])}` : "clean");

    // ...and the picture it arrives at is the picture it would have been
    // drawn from scratch.
    t = hold(dev, t, 1200, gravityFor(8, 0));
    const swept = dev.fb();
    const fresh = await enterLevel(gravityFor(8, 0));
    hold(fresh.dev, fresh.t, 1200, gravityFor(8, 0));
    const d = fbDiffCount(swept, fresh.dev.fb());
    check("after an orbit, the screen is identical to a freshly drawn one",
      d.n === 0, d.n ? `${d.n} differing bytes, first at pixel ${d.first}` : "bit-identical");
  }

  // ---- motion B: a slam. A fresh, far-away target every few frames is the
  // update shape that produced the original ring-shrink residue: a big jump
  // rather than a smooth walk through every intermediate position. --------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, 500, gravityFor(12, 90));
    for (let i = 0; i < 8; i++) {
      const phi = i % 2 === 0 ? 270 : 90;
      for (let k = 0; k < 4; k++) {
        t += FRAME_MS;
        auditedTick(dev, t, gravityFor(12, phi), acc);
      }
    }
    check("slamming the dot from one side of the dial to the other stays inside its pushes",
      acc.violations.length === 0,
      acc.violations.length ? JSON.stringify(acc.violations[0]) : "clean");

    t = hold(dev, t, 1200, gravityFor(12, 90));
    const slammed = dev.fb();
    const fresh = await enterLevel(gravityFor(12, 90));
    hold(fresh.dev, fresh.t, 1200, gravityFor(12, 90));
    const d = fbDiffCount(slammed, fresh.dev.fb());
    check("after eight slams, the screen is identical to a freshly drawn one",
      d.n === 0, d.n ? `${d.n} differing bytes, first at pixel ${d.first}` : "bit-identical");
  }

  // ---- motion C: the verdict changing, which is the one frame that
  // repaints something other than the dot (the target ring grows from 4 px
  // to 10 px and back). A ring that thickened without its old, thinner self
  // being inside the repainted rectangle would leave a hairline ghost. ----
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, 700, gravityFor(9, 45));
    // in
    for (let i = 0; i < 60; i++) { t += FRAME_MS; auditedTick(dev, t, gravityFor(0, 0), acc); }
    // out
    for (let i = 0; i < 60; i++) { t += FRAME_MS; auditedTick(dev, t, gravityFor(9, 225), acc); }
    // and back in
    for (let i = 0; i < 60; i++) { t += FRAME_MS; auditedTick(dev, t, gravityFor(0, 0), acc); }
    check("closing and reopening the ring changes no pixel outside a pushed window",
      acc.violations.length === 0,
      acc.violations.length ? JSON.stringify(acc.violations[0]) : "clean");

    t = hold(dev, t, 1200, gravityFor(0, 0));
    const cycled = dev.fb();
    const fresh = await enterLevel(gravityFor(0, 0));
    hold(fresh.dev, fresh.t, 1200, gravityFor(0, 0));
    const d = fbDiffCount(cycled, fresh.dev.fb());
    check("after the verdict has flipped three times, the screen is identical to a freshly drawn one",
      d.n === 0, d.n ? `${d.n} differing bytes, first at pixel ${d.first}` : "bit-identical");
  }

  // ---- the two invariants that hold across every frame above ------------
  check("every pushed window's row length is a multiple of 8 pixels (decision 0001)",
    acc.badRowLen === 0, `${acc.badRowLen} bad windows out of ${acc.frames} audited frames`);

  const frac = acc.maxPushPx / PANEL_PX;
  const avg = acc.totalPushPx / Math.max(1, acc.framesWithPush);
  console.log(`\n    audited ${acc.frames} frames, ${acc.framesWithPush} of them pushed anything`);
  console.log(`    worst frame pushed ${acc.maxPushPx} px (${(frac * 100).toFixed(1)}% of the panel)`);
  console.log(`    average pushing frame ${avg.toFixed(0)} px (${((avg / PANEL_PX) * 100).toFixed(1)}%)`);
  check(`no frame pushes more than ${(MAX_FRAME_PUSH_FRACTION * 100).toFixed(0)}% of the panel`,
    frac <= MAX_FRAME_PUSH_FRACTION, `worst was ${(frac * 100).toFixed(1)}%`);

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
