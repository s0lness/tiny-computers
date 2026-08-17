// repro-tiltball-residue: the standing guard on the tilt-a-ball's drawing,
// against the one defect class this project keeps paying for. Run:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/repro-tiltball-residue.ts
//
// Same shape as the now-removed bubble level's own
// repro-level-bubble-residue.ts, the file this one was written from: a
// moving object over a static background has left residue on this device
// four separate times (AGENTS.md), and firmware/apps/tiltball.c is exactly
// that shape twice over - a ball rolling on a dish that never moves, AND a
// capture ripple expanding from a hole that never moves either.
//
// THREE PROPERTIES, all measured on the framebuffer and the real push
// windows, never on an internal - identical to the level's own three, back
// when it had them:
//
//   1. Every pixel that CHANGES during a tick lies inside a rectangle that
//      tick pushed.
//   2. Every pushed window's row length is a multiple of 8 pixels
//      (docs/decisions/0001).
//   3. An incrementally-updated screen is BIT-IDENTICAL to one built by
//      replaying the exact same input from a fresh app entry.
//
// Plus a work budget per frame (the emulator cannot see time - decision
// 0003 - so pixels pushed is the proxy a headless run can actually
// measure), covering BOTH things this app pushes: the rolling ball (every
// tick, unlike the level's occasional settle) and the capture sequence
// (ball shrink+slide, an expanding ripple, respawn).
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const PANEL_PX = PANEL_W * PANEL_H;
let APP_TILTBALL = -1;
const FRAME_MS = 16;

// How long to hold the final tilt before comparing a swept/slammed run
// against a freshly-entered one for bit-identity. Unlike the level's dot
// (memoryless: its position is a pure function of the CURRENT tilt), this
// ball carries momentum, and the settle-snap (tiltball.c's
// BALL_SETTLE_POS_EPS/VEL_EPS) only engages once both position and velocity
// have decayed under those thresholds - a run that arrives with more
// residual velocity (e.g. fresh off an orbit) needs more time to cross that
// line than a run that was already near rest. 3000ms is comfortably past
// the spring's own decay envelope (zeta~=0.66, omega~=4.9 rad/s) even
// starting from BALL_MAX_SPEED, so both paths snap to the exact same
// equilibrium regardless of how they got there - see this constant's first
// use for the two comparisons this was tuned against (both failed
// intermittently at 1200ms with a few hundred differing bytes: not
// residue - the "no pixel outside its pushed window" checks stayed clean
// throughout - just two runs settling to the same picture on two different
// schedules).
const SETTLE_HOLD_MS = 3000;

// Worst case one frame of this app may push, as a fraction of the panel.
// The ripple's own bounding box at its widest (RIPPLE_MAX_R=46,
// tiltball.c) is roughly 100x100=10000px (~6.1%); a moving ball's own
// bounding box rarely exceeds a few thousand more. Set with headroom over
// both, and confirmed against the measurement this file prints - the same
// convention the now-removed bubble level's own MAX_FRAME_PUSH_FRACTION used.
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
    tilt(g: [number, number, number]) { e.emu_sensor_vector(1, g[0] / 1000, g[1] / 1000, g[2] / 1000); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_PX * 2).slice(); },
    soundPlaySeq(): number { return e.emu_sound_play_seq() >>> 0; },
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

async function enterBall(g?: [number, number, number]): Promise<{ dev: Device; t: number }> {
  const dev = await loadDevice();
  if (g) dev.tilt(g); // primes tilt.c's shared filter before the first tick,
                       // the same reason the now-removed bubble level's own
                       // enterLevel() did this
  dev.tick(0);
  dev.appSwitch(APP_TILTBALL);
  dev.tick(FRAME_MS);
  if (dev.appCurrent() !== APP_TILTBALL) throw new Error("did not land in tiltball");
  return { dev, t: FRAME_MS };
}

function hold(dev: Device, t: number, ms: number, g: [number, number, number]): number {
  const end = t + ms;
  while (t < end) { dev.tilt(g); t += FRAME_MS; dev.tick(t); }
  return t;
}

function tickUntilCaptured(dev: Device, t: number, g: [number, number, number], maxMs: number, acc?: Parameters<typeof auditedTick>[3]): number {
  const seqBefore = dev.soundPlaySeq();
  const end = t + maxMs;
  while (t < end) {
    if (acc) auditedTick(dev, t + FRAME_MS, g, acc);
    else dev.tilt(g);
    t += FRAME_MS;
    if (!acc) dev.tick(t);
    if (dev.soundPlaySeq() !== seqBefore) return t;
  }
  throw new Error(`capture did not trigger within ${maxMs}ms`);
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
  console.log("=== repro + regression: the ball and the ripple must leave nothing behind ===\n");

  {
    const probe = await loadDevice();
    APP_TILTBALL = probe.apps().indexOf("tiltball");
    check("the app table carries 'tiltball'", APP_TILTBALL >= 0, `apps=${JSON.stringify(probe.apps())}`);
    if (APP_TILTBALL < 0) {
      console.log(`\n${passCount} passed, ${failCount} failed`);
      process.exit(1);
    }
  }

  const acc = { violations: [] as Violation[], badRowLen: 0, maxPushPx: 0, totalPushPx: 0, frames: 0, framesWithPush: 0 };

  // ---- motion A: a slow orbit, the ball driven in a full circle at a
  // moderate tilt - the same shape the now-removed bubble level's own test
  // used, adapted: this sweeps the ball across a wide arc of the dish, never
  // toward the hole (phi excludes the neighbourhood of 90) so a capture
  // does not cut the orbit short. --------------------------------------
  {
    const { dev, t: t0 } = await enterBall();
    let t = hold(dev, t0, 500, gravityFor(10, 135));
    const STEPS = 150;
    for (let i = 1; i <= STEPS; i++) {
      t += FRAME_MS;
      // Sweep phi from 135 to 405 (=45), i.e. all the way AROUND avoiding
      // 90 (the hole's own direction) at the START and END of the sweep,
      // and passing through it only briefly in the middle at a moderate
      // 10deg - well short of the hole (see feature-tiltball.ts's own
      // "15deg falls short" finding).
      auditedTick(dev, t, gravityFor(10, 135 + (270 * i) / STEPS), acc);
    }
    check("a wide orbit changes no pixel outside a pushed window",
      acc.violations.length === 0,
      acc.violations.length ? `${acc.violations.length} violations, first ${JSON.stringify(acc.violations[0])}` : "clean");

    t = hold(dev, t, SETTLE_HOLD_MS, gravityFor(10, 45));
    const swept = dev.fb();
    const fresh = await enterBall(gravityFor(10, 45));
    hold(fresh.dev, fresh.t, SETTLE_HOLD_MS, gravityFor(10, 45));
    const d = fbDiffCount(swept, fresh.dev.fb());
    check("after the orbit, the screen is identical to a freshly drawn one at the same tilt",
      d.n === 0, d.n ? `${d.n} differing bytes, first at pixel ${d.first}` : "bit-identical");
  }

  // ---- motion B: a slam - a fresh, far-away target every few frames,
  // the update shape that produced the level's own original ring-shrink
  // residue. Kept well away from the hole's direction (phi=270, opposite
  // it) so this exercises the ball alone, not the capture path. ----------
  {
    const { dev, t: t0 } = await enterBall();
    let t = hold(dev, t0, 500, gravityFor(14, 200));
    for (let i = 0; i < 8; i++) {
      const phi = i % 2 === 0 ? 200 : 340;
      for (let k = 0; k < 4; k++) {
        t += FRAME_MS;
        auditedTick(dev, t, gravityFor(14, phi), acc);
      }
    }
    check("slamming the ball from one side of the dish to the other stays inside its pushes",
      acc.violations.length === 0,
      acc.violations.length ? JSON.stringify(acc.violations[0]) : "clean");

    t = hold(dev, t, SETTLE_HOLD_MS, gravityFor(14, 200));
    const slammed = dev.fb();
    const fresh = await enterBall(gravityFor(14, 200));
    hold(fresh.dev, fresh.t, SETTLE_HOLD_MS, gravityFor(14, 200));
    const d = fbDiffCount(slammed, fresh.dev.fb());
    check("after eight slams, the screen is identical to a freshly drawn one",
      d.n === 0, d.n ? `${d.n} differing bytes, first at pixel ${d.first}` : "bit-identical");
  }

  // ---- motion C: a full capture cycle - the ball rolled in, shrinking
  // and sliding; the ripple expanding from the hole; the pause; the
  // respawn growing back at the centre. This is the frame this app's OWN
  // header comment says gets the most design care, so it gets its own
  // audited pass here too, not just a budget number. ---------------------
  {
    const { dev, t: t0 } = await enterBall();
    let t = tickUntilCaptured(dev, t0, gravityFor(40, 90), 2000, acc);
    // Ride the rest of the sequence under audit: captured -> hidden ->
    // respawning -> rolling again.
    const restMs = 380 + 260 + 260 + 200; // CAPTURE_ANIM_MS + HIDDEN_PAUSE_MS + RESPAWN_ANIM_MS + margin
    const end = t + restMs;
    while (t < end) { t += FRAME_MS; auditedTick(dev, t, gravityFor(0, 0), acc); }
    check("a full capture cycle changes no pixel outside a pushed window",
      acc.violations.length === 0,
      acc.violations.length ? JSON.stringify(acc.violations[0]) : "clean");

    // ...and it arrives at the SAME picture a fresh entry does after
    // replaying the identical capture-then-settle sequence.
    const captured = dev.fb();
    const fresh = await enterBall();
    let ft = tickUntilCaptured(fresh.dev, fresh.t, gravityFor(40, 90), 2000);
    const fend = ft + restMs;
    while (ft < fend) { fresh.dev.tilt(gravityFor(0, 0)); ft += FRAME_MS; fresh.dev.tick(ft); }
    const d = fbDiffCount(captured, fresh.dev.fb());
    check("after a full capture cycle, the screen is identical to a freshly-driven replay of the same sequence",
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
