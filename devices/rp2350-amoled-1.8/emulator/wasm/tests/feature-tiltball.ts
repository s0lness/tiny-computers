// feature-tiltball: what the tilt-a-ball (firmware/apps/tiltball.c) is
// supposed to do, asserted against the REAL compiled firmware. Run with:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/feature-tiltball.ts
//
// The same convention every feature-*.ts file in this directory shares:
// clean input, a readable statement of intent, reading at the app boundary
// (emu_tilt-adjacent state read off the FRAMEBUFFER and off emu_sound_*,
// never an internal) - a convention the now-removed bubble level's own
// feature-level.ts established first, before this app existed. The repro-*
// half is repro-tiltball-residue.ts, which drives motion and a full capture
// cycle and guards the residue/push-budget class of defect this device
// keeps paying for. There is no touch-dropout partner, deliberately: this
// app reads no touch at all.
//
// HOW TILT IS DRIVEN, and which panel-axis direction reaches this app's
// hole. emu_sensor_vector() (emu_abi.h) injects a gravity vector in PANEL
// axes, g, on the shared "gravity" sensor every orientation-aware app now
// reads through (firmware/runtime/tilt.h). This app's hole sits at
// LANDSCAPE +x (tiltball.c's HOLE_X = BALL_CX + HOLE_DIST). feature-tilt.ts
// already established the rotation runtime_core.c's tilt_for_app() applies
// for a landscape app (landscape gx = panel gy, landscape gy = -panel gx),
// so panel gravity (0, sin(deg), cos(deg)) - this file's gravityFor(deg, 90)
// - is what drives the ball toward landscape +x, i.e. toward the hole.
// Nothing here re-derives that rotation; it is read off the same tested
// rule feature-tilt.ts pins.
//
// WHAT THIS CANNOT CHECK: the device-to-panel mapping is a HYPOTHESIS until
// the on-board axis ritual runs (tilt.h) - the same caveat every
// orientation-aware app's test carries, inherited unchanged since this app
// has no orientation code of its own to get wrong (see tiltball.c's own
// header comment).
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
let APP_TILTBALL = -1;

// tiltball.c's geometry and physics constants, lifted rather than
// re-derived - the same convention every other test in this directory uses
// against its app.
const CX = 224, CY = 184;
const DISH_RIM_R = 168, DISH_RIM_HALF = 3.5, DISH_GAP = 4;
const BALL_R = 26;
const TRAVEL_MAX = DISH_RIM_R - DISH_RIM_HALF - BALL_R - DISH_GAP; // 134.5
const HOLE_DIST = 92, HOLE_X = CX + HOLE_DIST, HOLE_Y = CY;
const HOLE_R = 30;
const ACCEL_PER_G = 5200, OMEGA2 = 24;
const CAPTURE_ANIM_MS = 380, HIDDEN_PAUSE_MS = 260, RESPAWN_ANIM_MS = 260;
const BEZEL = 10; // gfx.h's PANEL_BEZEL_MARGIN_PX

const FRAME_MS = 16;
const SETTLE_MS = 3000; // comfortably past the spring's own settle time at
                         // these constants (omega ~= 4.9 rad/s, zeta ~= 0.66)

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
      js_log(_p: number, _l: number) {},
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
    exports: e,
    tick(nowMs: number) { e.emu_tick(nowMs); },
    // x/y/z here are milli-g (this file's own gravityFor() convention);
    // emu_sensor_vector() wants g, index 1 = the shared "gravity" sensor.
    tilt(x: number, y: number, z: number) { e.emu_sensor_vector(1, x / 1000, y / 1000, z / 1000); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    soundPlaySeq(): number { return e.emu_sound_play_seq() >>> 0; },
    device(): any {
      const ptr = e.emu_device();
      const bytes = new Uint8Array(memory.buffer, ptr);
      let end = 0; while (bytes[end] !== 0) end++;
      return JSON.parse(dec.decode(bytes.subarray(0, end)));
    },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function grayAt(fb: Uint8Array, lx: number, ly: number): number {
  if (lx < 0 || ly < 0 || lx >= LAND_W || ly >= LAND_H) return 252;
  const px = PANEL_W - 1 - ly, py = lx;
  const i = (py * PANEL_W + px) * 2;
  const v = (fb[i]! << 8) | fb[i + 1]!;
  return ((v >> 5) & 0x3f) << 2;
}
const isBlack = (g: number) => g <= 24;

// The ball's centre, found by eroding the black mask so the rim's stroke
// cannot pull a plain centroid off target (the same technique the
// now-removed bubble level used for its own dot). K is smaller here (the
// ball itself is smaller than the level's dot was), so the erosion box only
// ever survives inside the ball's own
// interior, never inside the dish rim's 7px-thick stroke.
//
// UNLIKE the level, this dish has a SECOND permanent black disc - the hole
// itself, always drawn, not just during a capture. Left alone it erodes to
// its own solid blob exactly like the ball does, and a plain image-wide
// centroid then averages the two together into a meaningless point
// somewhere between them. So pixels within HOLE_MASK_R of the hole's own
// centre are excluded from the "black" input before erosion ever runs -
// the hole is a KNOWN, FIXED fixture (tiltball.c's HOLE_X/HOLE_Y/HOLE_R),
// not something this file needs to also treat as an unknown blob.
const HOLE_MASK_R2 = (HOLE_R + 2) * (HOLE_R + 2);
function ballCentre(fb: Uint8Array): { x: number; y: number; n: number } {
  const W = LAND_W, H = LAND_H;
  const sat = new Int32Array((W + 1) * (H + 1));
  for (let y = 0; y < H; y++) {
    let rowSum = 0;
    for (let x = 0; x < W; x++) {
      let black = isBlack(grayAt(fb, x, y));
      if (black) {
        const dhx = x - HOLE_X, dhy = y - HOLE_Y;
        if (dhx * dhx + dhy * dhy <= HOLE_MASK_R2) black = false;
      }
      rowSum += black ? 1 : 0;
      sat[(y + 1) * (W + 1) + (x + 1)] = sat[y * (W + 1) + (x + 1)] + rowSum;
    }
  }
  const box = (x0: number, y0: number, x1: number, y1: number) =>
    sat[(y1 + 1) * (W + 1) + (x1 + 1)]! - sat[y0 * (W + 1) + (x1 + 1)]!
    - sat[(y1 + 1) * (W + 1) + x0]! + sat[y0 * (W + 1) + x0]!;

  const K = 5; // 11x11 erosion box: survives inside the ball (52px across)
               // and inside the hole (60px across), never inside a 7-8px stroke
  let sx = 0, sy = 0, n = 0;
  for (let y = K; y < H - K; y++) {
    for (let x = K; x < W - K; x++) {
      if (box(x - K, y - K, x + K, y + K) !== (2 * K + 1) * (2 * K + 1)) continue;
      sx += x; sy += y; n++;
    }
  }
  return n === 0 ? { x: NaN, y: NaN, n: 0 } : { x: sx / n, y: sy / n, n };
}

// Panel-axis gravity helper, milli-g - the same shape the now-removed
// bubble level's own tests used.
function gravityFor(deg: number, phiDeg: number): [number, number, number] {
  const t = (deg * Math.PI) / 180, p = (phiDeg * Math.PI) / 180;
  return [1000 * Math.sin(t) * Math.cos(p), 1000 * Math.sin(t) * Math.sin(p), 1000 * Math.cos(t)];
}

function hold(dev: Device, t: number, ms: number, g: [number, number, number]): number {
  const end = t + ms;
  while (t < end) { dev.tilt(g[0], g[1], g[2]); t += FRAME_MS; dev.tick(t); }
  return t;
}

// Ticks with a sustained tilt until emu_sound_play_seq() bumps (tiltball.c
// fires it on exactly one tick: the ROLLING -> CAPTURED transition), and
// returns the clock at THAT tick. Needed because how long a given tilt
// takes to reach the hole depends on the spring's own transient, not on a
// number this file can predict in advance - polling for the real edge is
// the only way to then hold a KNOWN, exact duration through the rest of
// the capture/hidden/respawn sequence.
function tickUntilCaptured(dev: Device, t: number, g: [number, number, number], maxMs: number): number {
  const seqBefore = dev.soundPlaySeq();
  const end = t + maxMs;
  while (t < end) {
    dev.tilt(g[0], g[1], g[2]);
    t += FRAME_MS;
    dev.tick(t);
    if (dev.soundPlaySeq() !== seqBefore) return t;
  }
  throw new Error(`capture did not trigger within ${maxMs}ms`);
}

async function enterBall(): Promise<{ dev: Device; t: number }> {
  const dev = await loadDevice();
  dev.tick(0);
  dev.appSwitch(APP_TILTBALL);
  dev.tick(FRAME_MS);
  return { dev, t: FRAME_MS };
}

async function main() {
  console.log("=== feature: tilt-a-ball ===\n");

  // ---- the device declares the shared continuous gravity sensor, and the
  // app table carries this app ----------------------------------------
  {
    const dev = await loadDevice();
    const d = dev.device();
    const gravity = (d.sensors || []).find((s: any) => s.id === "gravity");
    check("emu_device() declares a continuous 'gravity' sensor", !!gravity && gravity.kind === "gravity");

    APP_TILTBALL = (d.apps || []).indexOf("tiltball");
    check("the app table carries 'tiltball'", APP_TILTBALL >= 0, `index ${APP_TILTBALL} of ${JSON.stringify(d.apps)}`);
    if (APP_TILTBALL < 0) {
      console.log(`\n${passCount} passed, ${failCount} failed`);
      process.exit(1);
    }
  }

  // ---- at rest, flat, the ball sits at the dish's own centre ------------
  {
    const { dev, t: t0 } = await enterBall();
    check("switched into tiltball", dev.appCurrent() === APP_TILTBALL, `app_current()=${dev.appCurrent()}`);
    hold(dev, t0, SETTLE_MS, gravityFor(0, 0));
    const b = ballCentre(dev.fb());
    const off = Math.hypot(b.x + 0.5 - CX, b.y + 0.5 - CY);
    check("held flat, the ball rests at the dish's centre", off < 1.0,
      `centre=(${b.x.toFixed(2)}, ${b.y.toFixed(2)}), off by ${off.toFixed(2)}px, n=${b.n}`);

    // Nothing may be drawn where the case hides it.
    let worstR = 0;
    const fb = dev.fb();
    for (let ly = 0; ly < LAND_H; ly++) {
      for (let lx = 0; lx < LAND_W; lx++) {
        if (grayAt(fb, lx, ly) >= 248) continue;
        const r2 = Math.hypot(lx - CX, ly - CY);
        if (r2 > worstR) worstR = r2;
      }
    }
    check("nothing is drawn inside the bezel band", worstR <= LAND_H / 2 - BEZEL,
      `furthest ink is ${worstR.toFixed(1)}px from centre, the case hides past ${LAND_H / 2 - BEZEL}`);
  }

  // ---- a still puck costs nothing, once settled --------------------------
  {
    const { dev, t: t0 } = await enterBall();
    let t = hold(dev, t0, SETTLE_MS, gravityFor(6, 45));
    let pushes = 0;
    for (let i = 0; i < 60; i++) {
      dev.tilt(...gravityFor(6, 45));
      t += FRAME_MS;
      dev.tick(t);
      pushes += dev.exports.emu_push_count();
    }
    check("a perfectly still device pushes nothing at all once settled", pushes === 0, `${pushes} pushes over 60 frames`);
  }

  // ---- rolling toward the hole moves the ball in that direction ---------
  // Deliberately gentle and brief: a decisive push (see the capture tests
  // below) can reach and trigger the hole within under a second, and this
  // block only wants to see the DIRECTION of motion, not a settled offset.
  {
    const { dev, t: t0 } = await enterBall();
    hold(dev, t0, 250, gravityFor(10, 90)); // panel phi=90 -> landscape +x, see header comment
    const b = ballCentre(dev.fb());
    check("tilting toward the hole moves the ball toward landscape +x", b.n > 0 && b.x + 0.5 > CX + 5,
      `ball x=${b.n > 0 ? (b.x + 0.5).toFixed(1) : "not found"}, started at ${CX}`);
  }

  // ---- equilibrium: a careful, adult-scale tip (15deg, level.c's own
  // reference) does NOT reach the hole; a decisive, toddler-scale tip does -
  // this is the whole point of this app's tilt-to-accel scale (see
  // tiltball.c's header comment). --------------------------------------
  {
    const { dev, t: t0 } = await enterBall();
    hold(dev, t0, SETTLE_MS, gravityFor(15, 90));
    const b = ballCentre(dev.fb());
    const dist = Math.hypot(b.x + 0.5 - HOLE_X, b.y + 0.5 - HOLE_Y);
    check("a careful 15deg tip (level.c's own 'full scale') falls short of the hole",
      dist > 20, `ball at (${(b.x + 0.5).toFixed(1)}, ${(b.y + 0.5).toFixed(1)}), ${dist.toFixed(1)}px from the hole`);
  }
  {
    // A separate, SMALLER angle for the tight numeric equilibrium check:
    // at 15deg the ball's near edge sits close enough to the hole that this
    // file's own hole-exclusion mask (ballCentre's HOLE_MASK_R2) clips part
    // of the ball and biases the measured centroid - a measurement artifact
    // of this test file, not of the app. 8deg keeps the whole ball clear of
    // that mask.
    const { dev, t: t0 } = await enterBall();
    hold(dev, t0, SETTLE_MS, gravityFor(8, 90));
    const b = ballCentre(dev.fb());
    const gInPlane = Math.sin((8 * Math.PI) / 180);
    const eq = Math.min(TRAVEL_MAX, (ACCEL_PER_G * gInPlane) / OMEGA2);
    const measuredOffset = Math.hypot(b.x + 0.5 - CX, b.y + 0.5 - CY);
    check("the settled offset at 8deg is close to the predicted equilibrium",
      Math.abs(measuredOffset - eq) < 6,
      `measured=${measuredOffset.toFixed(1)}px predicted=${eq.toFixed(1)}px`);
  }

  // ---- extreme tilt never sends the ball past the dish's own travel -----
  // phi=180 (panel axes), not 90: 90 points straight at the hole, which
  // would intercept the ball (HOLE_DIST=92 < TRAVEL_MAX=134.5) before it
  // ever got close to testing the RIM's own containment. 180 drives the
  // ball toward landscape +y instead - perpendicular to the hole, so this
  // block tests the bowl+rim's containment in isolation from the hole.
  {
    const { dev, t: t0 } = await enterBall();
    hold(dev, t0, SETTLE_MS, gravityFor(85, 180)); // as hard a tip as a puck can register
    const b = ballCentre(dev.fb());
    check("even a near-90deg tip finds a real ball (it did not fly off or get lost)", b.n > 0, `n=${b.n}`);
    const dist = Math.hypot(b.x + 0.5 - CX, b.y + 0.5 - CY);
    check("...and it stays inside its travel radius (the bowl + rim contain it)",
      dist <= TRAVEL_MAX + 2.0, `${dist.toFixed(1)}px from centre, travel max is ${TRAVEL_MAX}`);
  }

  // ---- !valid: DELIBERATELY NOT TESTED HERE, and said out loud rather
  // than faked. tilt.h's own header comment states the gap plainly: "the
  // staleness path...emu_tick() submits a sample every frame by
  // construction, so 'the IMU went silent' cannot be reproduced in the
  // emulator at all" - confirmed against emu_shim.c's emu_tick(), which
  // calls tilt_submit_device_g() unconditionally every tick with whatever
  // gravity was last set, so app_frame_t.tilt.valid becomes true on the
  // first tick and CANNOT be made false again in this environment (see
  // feature-tilt.ts's own precedent, which carries the identical caveat).
  // tiltball.c's header comment reasons through what the ball does in that
  // state (the external push drops to zero, the bowl's own restoring term
  // keeps running); on the board, that reasoning is what there is to check
  // it against, not a test file - only a real IMU going quiet can exercise
  // this path.

  // ---- capture: rolling into the hole triggers the sequence, plays a
  // sound exactly once, and the ball comes back --------------------------
  {
    const { dev, t: t0 } = await enterBall();
    const seqBefore = dev.soundPlaySeq();
    // A hard, sustained tip straight at the hole, ticked until the exact
    // instant it triggers (see tickUntilCaptured's own comment).
    let t = tickUntilCaptured(dev, t0, gravityFor(40, 90), 2000);

    check("the capture sound was triggered exactly once", dev.soundPlaySeq() === seqBefore + 1,
      `seq before=${seqBefore} after=${dev.soundPlaySeq()}`);

    // From this KNOWN instant, ride out the rest of the sequence exactly:
    // captured, hidden, respawning, and back to rolling - hold flat from
    // here so the ball does not immediately roll off again once control
    // returns.
    const restOfSequenceMs = CAPTURE_ANIM_MS + HIDDEN_PAUSE_MS + RESPAWN_ANIM_MS + 200;
    t = hold(dev, t, restOfSequenceMs, gravityFor(0, 0));

    const b = ballCentre(dev.fb());
    const off = Math.hypot(b.x + 0.5 - CX, b.y + 0.5 - CY);
    check("after the full capture sequence, the ball is back at the dish's centre",
      b.n > 0 && off < 2.0,
      `centre=(${b.n > 0 ? (b.x + 0.5).toFixed(1) : "?"}, ${b.n > 0 ? (b.y + 0.5).toFixed(1) : "?"}), off by ${off.toFixed(2)}px`);

    // And it can be captured again - no stuck state.
    const seqBefore2 = dev.soundPlaySeq();
    tickUntilCaptured(dev, t, gravityFor(40, 90), 2000);
    check("a second capture in the same session fires the sound again",
      dev.soundPlaySeq() === seqBefore2 + 1, `seq before=${seqBefore2} after=${dev.soundPlaySeq()}`);
  }

  // ---- while hidden (mid-capture), the ball itself is not drawn, only
  // the hole and the fading ripple -----------------------------------
  {
    const { dev, t: t0 } = await enterBall();
    // Ticked to the EXACT instant of capture, then a known, precise offset
    // into the middle of the HIDDEN phase - see tickUntilCaptured's comment
    // for why this cannot be a fixed guessed duration from the start.
    let t = tickUntilCaptured(dev, t0, gravityFor(40, 90), 2000);
    t = hold(dev, t, CAPTURE_ANIM_MS + HIDDEN_PAUSE_MS / 2, gravityFor(0, 0));
    const b = ballCentre(dev.fb());
    check("mid-sequence (hidden), no ball-sized blob is on screen away from the hole",
      b.n === 0 || Math.hypot(b.x + 0.5 - HOLE_X, b.y + 0.5 - HOLE_Y) < 15,
      `n=${b.n}, centre=(${b.x}, ${b.y})`);
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
